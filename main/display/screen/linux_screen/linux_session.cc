#include "linux_session.h"
#include "linux_vm.h"
#include "sd_access.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>

namespace linux_vm {
Session::Session(const Console::Layout& layout, std::string image_path) : image_path(std::move(image_path)), console_(layout) { commands_ = xQueueCreate(4, sizeof(Command)); }
Session::~Session() { if (commands_) vQueueDelete(commands_); }
void Session::Fail(Failure reason) { failure = reason; status = Status::Failed; }
bool Session::Send(const std::string& text)
{
    if (!commands_ || text.size() > kInputBytes + 1) return false;
    Command command{};
    command.length = text.size();
    std::memcpy(command.bytes, text.data(), text.size());
    return xQueueSend(commands_, &command, 0) == pdTRUE;
}
int Session::Read(bool consume)
{
    if (cursor_ == current_.length) {
        if (xQueueReceive(commands_, &current_, 0) != pdTRUE) return -1;
        cursor_ = 0;
    }
    int value = static_cast<unsigned char>(current_.bytes[cursor_]);
    if (consume) ++cursor_;
    return value;
}
void Session::Publish(const std::string& bytes)
{
    if (bytes.empty()) return;
    std::lock_guard<std::mutex> lock(output_mutex_);
    console_.Append(bytes);
}
Session::Frame Session::AdvanceWindow(size_t rows, Console::Cursor& cursor)
{
    std::lock_guard<std::mutex> lock(output_mutex_);
    cursor = console_.Advance(rows, cursor);
    auto text = console_.Window(rows, cursor);
    return {std::move(text), cursor.back};
}
Console::Boundary Session::Scroll(size_t rows, int direction, Console::Cursor& cursor)
{
    std::lock_guard<std::mutex> lock(output_mutex_);
    return console_.Scroll(rows, direction, cursor);
}

namespace {
// S31 真机启动、uname 与退出峰值 1916 B；加 1 KiB 后向上取整为 3072 B。
constexpr unsigned kStackBytes = 3072;
using Ram = std::unique_ptr<uint8_t, decltype(&heap_caps_free)>;
size_t ReadImage(Session& s, const std::string& path, uint8_t* ram)
{
    sd_access::Transaction transaction;
    if (!transaction) { s.Fail(Failure::SdBusy); return 0; }
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"), &std::fclose);
    if (!file) { s.Fail(Failure::Open); return 0; }
    if (std::fseek(file.get(), 0, SEEK_END) != 0) { s.Fail(Failure::Read); return 0; }
    long size = std::ftell(file.get());
    if (size <= 0 || static_cast<size_t>(size) > kDtbOffset) { s.Fail(Failure::Size); return 0; }
    if (std::fseek(file.get(), 0, SEEK_SET) != 0) { s.Fail(Failure::Read); return 0; }
    size_t length = 0;
    while (length < static_cast<size_t>(size) && !s.stop) {
        size_t chunk = std::min<size_t>(16384, size - length);
        if (std::fread(ram + length, 1, chunk, file.get()) != chunk) { s.Fail(Failure::Read); return 0; }
        length += chunk;
    }
    return s.stop ? 0 : length;
}
void Run(Session& s)
{
    Ram ram(static_cast<uint8_t*>(heap_caps_malloc(kRamBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), &heap_caps_free);
    if (!ram) { s.Fail(Failure::Memory); return; }
    size_t length = ReadImage(s, s.image_path, ram.get());
    if (!length) return;
    struct Io { Session& session; std::string output; } io{s, {}};
    Machine vm(ram.get(), {&io,
        [](void* p, char c) { static_cast<Io*>(p)->output += c; },
        [](void* p, bool consume) { return static_cast<Io*>(p)->session.Read(consume); }});
    if (!vm.Boot(length)) { s.Fail(Failure::Size); return; }
    s.status = Status::Running;
    int64_t previous = esp_timer_get_time(), slice = previous, report = previous;
    uint64_t last_cycles = vm.Cycles();
    unsigned samples = 0;
    while (!s.stop) {
        if (s.paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            previous = slice = report = esp_timer_get_time();
            last_cycles = vm.Cycles();
            continue;
        }
        int64_t now = esp_timer_get_time();
        int result = vm.Step(static_cast<uint32_t>(now - previous), 4096);
        previous = now;
        s.Publish(io.output);
        io.output.clear();
        if (result == kPowerOff || result == kRestart) {
            s.status = result == kRestart ? Status::Restarted : Status::PoweredOff;
            return;
        }
        now = esp_timer_get_time();
        if (samples < 3 && now - report >= 1000000) {
            uint64_t cycles = vm.Cycles();
            ESP_LOGI("Linux", "instructions/s=%llu guest_cycles=%llu", 
                (unsigned long long)((cycles - last_cycles) * 1000000 / (now - report)),
                (unsigned long long)cycles);
            report = now;
            last_cycles = cycles;
            ++samples;
        }
        // 以实际运行时间分配 CPU：连续运行至多 20 ms，再让出一个 tick。
        // 小步进保留取消响应；WFI 立即阻塞，避免空转和饿死同核 idle。
        if (result == kWaitForInterrupt || now - slice >= 20000) {
            vTaskDelay(1);
            slice = esp_timer_get_time();
        }
    }
}
void Worker(void* argument)
{
    {
        std::unique_ptr<std::shared_ptr<Session>> argument_owner(static_cast<std::shared_ptr<Session>*>(argument));
        auto session = std::move(*argument_owner);
        argument_owner.reset();
        Run(*session); // Machine 先析构，随后 RAM；任务结束前不再持有 SD 句柄。
        unsigned free = uxTaskGetStackHighWaterMark(nullptr);
        ESP_LOGI("Linux", "task_end stack_peak=%u stack_free=%u stack_budget=%u", kStackBytes - free, free, kStackBytes);
        session->done = true;
    }
    vTaskDelete(nullptr);
}
}
void Launch(const std::shared_ptr<Session>& session)
{
    if (!session->Ready()) { session->Fail(Failure::Queue); session->done = true; return; }
    auto* request = new std::shared_ptr<Session>(session);
    if (xTaskCreate(Worker, "linux_vm", kStackBytes, request, 1, nullptr) != pdPASS) {
        delete request;
        session->Fail(Failure::Task);
        session->done = true;
    }
}
}
