#pragma once
#include "linux_console.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace linux_vm {
enum class Status { Loading, Running, Failed, PoweredOff, Restarted };
enum class Failure { None, Memory, Queue, Task, SdBusy, Open, Size, Read };

class Session {
public:
    // 前台传入像素宽度及字符度量；会话内固定，不保留字体或 UI 对象引用。
    Session(const Console::Layout& layout, std::string image_path);
    const std::string image_path;
    ~Session();
    std::atomic<bool> stop{false}, paused{false}, done{false};
    std::atomic<Status> status{Status::Loading};
    std::atomic<Failure> failure{Failure::None};
    // 整条命令以零等待投递；队列满时保留 UI 输入，由调用方提示。
    bool Send(const std::string& text);
    int Read(bool consume);
    void Publish(const std::string& bytes);
    struct Frame { std::string text; size_t back; };
    Frame AdvanceWindow(size_t rows, Console::Cursor& cursor);
    Console::Boundary Scroll(size_t rows, int direction, Console::Cursor& cursor);
    void Fail(Failure reason);
    bool Ready() const { return commands_ != nullptr; }
private:
    struct Command { char bytes[kInputBytes + 1]; uint16_t length; };
    QueueHandle_t commands_ = nullptr;
    Command current_{}; // 只由执行任务读取。
    size_t cursor_ = 0;
    std::mutex output_mutex_;
    Console console_;
};
void Launch(const std::shared_ptr<Session>& session);
}
