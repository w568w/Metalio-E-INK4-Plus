#include "linux_ui.h"
#include "screen_keyboard.h"
#include "screen_text.h"
#include "epd_refresh.h"
#include "fontpack_lvgl.h"
#include "ui_scale.h"
#include "assets/lang_linux.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

extern "C" void fontpack_emulator_set_path(const char*);
extern "C" void HapticAttachClick(lv_obj_t*) {}
EpdRefreshStats EpdGetStats() { return {}; }
namespace Lang::Strings {
const char* LINUX_IMAGE_LINK = "镜像";
const char* LINUX_LATEST = "最新输出";
const char* LINUX_OLDEST = "已到最早保留的输出";
const char* LINUX_HISTORY_FMT = "历史输出：向上 %u 行";

const char* LINUX_TITLE = "Linux";
const char* LINUX_LOADING = "正在启动…";
const char* LINUX_RUNNING = "运行中";
const char* LINUX_IMAGE_ERROR = "无法打开镜像，请重新选择";
const char* LINUX_MEMORY_ERROR = "内存不足，需要连续 12 MiB PSRAM";
const char* LINUX_TASK_ERROR = "无法创建模拟器任务";
const char* LINUX_POWERED_OFF = "已结束";
const char* LINUX_RESTART = "已结束（请求重启）";
const char* LINUX_CHOOSE = "选择镜像";
const char* LINUX_INTERRUPT = "中断";
const char* LINUX_LIST_LOADING = "正在读取镜像列表…";
const char* LINUX_NO_IMAGES = "没有镜像。请将 RV32IMA Linux 裸 Image 文件放入 metalio/e-ink/linux/，再重新打开应用。";
const char* LINUX_LIST_ERROR = "镜像目录读取失败，请检查 SD 卡";
const char* LINUX_AUTO_FAILED = "上次的镜像启动失败，请重新选择";
const char* LINUX_INPUT_FULL = "输入队列已满，请稍后再次确认";
const char* LINUX_NOT_RUNNING = "Linux 尚未运行";
const char* LINUX_SD_BUSY = "SD 卡暂不可用，请关闭 USB 传输";
const char* LINUX_IMAGE_SIZE_ERROR = "镜像为空或超过虚拟机的内存上限";
const char* LINUX_IMAGE_READ_ERROR = "镜像读取不完整，请检查文件和 SD 卡";
}
static void English() {
    Lang::Strings::LINUX_IMAGE_LINK = "Image";
    Lang::Strings::LINUX_LATEST = "Latest output";
    Lang::Strings::LINUX_OLDEST = "Oldest retained output";
    Lang::Strings::LINUX_HISTORY_FMT = "History: %u lines above latest";

    Lang::Strings::LINUX_TITLE = "Linux";
    Lang::Strings::LINUX_LOADING = "Starting…";
    Lang::Strings::LINUX_RUNNING = "Running";
    Lang::Strings::LINUX_IMAGE_ERROR = "Cannot open image. Choose another.";
    Lang::Strings::LINUX_MEMORY_ERROR = "12 MiB contiguous PSRAM required";
    Lang::Strings::LINUX_TASK_ERROR = "Cannot create emulator task";
    Lang::Strings::LINUX_POWERED_OFF = "Ended";
    Lang::Strings::LINUX_RESTART = "Ended (restart requested)";
    Lang::Strings::LINUX_CHOOSE = "Choose image";
    Lang::Strings::LINUX_INTERRUPT = "Interrupt";
    Lang::Strings::LINUX_LIST_LOADING = "Reading image list…";
    Lang::Strings::LINUX_NO_IMAGES = "No images. Copy an RV32IMA Linux Image file into metalio/e-ink/linux/, then reopen this app.";
    Lang::Strings::LINUX_LIST_ERROR = "Cannot read image directory. Check SD card.";
    Lang::Strings::LINUX_AUTO_FAILED = "Last image could not start. Choose another.";
    Lang::Strings::LINUX_INPUT_FULL = "Input queue full. Confirm again shortly.";
    Lang::Strings::LINUX_NOT_RUNNING = "Linux is not running yet";
    Lang::Strings::LINUX_SD_BUSY = "SD unavailable. Turn off USB transfer.";
    Lang::Strings::LINUX_IMAGE_SIZE_ERROR = "Image is empty or exceeds guest memory";
    Lang::Strings::LINUX_IMAGE_READ_ERROR = "Image read failed. Check file and SD card.";
}
static void Bounds(lv_obj_t* object)
{
    lv_area_t parent;
    lv_obj_get_coords(object, &parent);
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        auto* child = lv_obj_get_child(object, i);
        // 输入框光标、文本内容可在控件内部裁剪；检查控件本身的边界。
        if (lv_obj_check_type(object, &lv_textarea_class)) continue;
        lv_area_t area;
        lv_obj_get_coords(child, &area);
        if (!(area.x1 >= parent.x1 && area.y1 >= parent.y1 && area.x2 <= parent.x2 && area.y2 <= parent.y2)) {
            std::fprintf(stderr, "Out of bounds: parent %d,%d..%d,%d child %d,%d..%d,%d\n", parent.x1,parent.y1,parent.x2,parent.y2,area.x1,area.y1,area.x2,area.y2);
            assert(false);
        }
        Bounds(child);
    }
}
static void Snapshot(lv_obj_t* screen, const std::string& path)
{
    lv_obj_update_layout(screen);
    Bounds(screen);
    auto* draw = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
    assert(draw);
    FILE* file = std::fopen(path.c_str(), "wb");
    assert(file);
    std::fprintf(file, "P6\n%u %u\n255\n", draw->header.w, draw->header.h);
    for (unsigned y = 0; y < draw->header.h; ++y) {
        const uint8_t* row = draw->data + y * draw->header.stride;
        for (unsigned x = 0; x < draw->header.w; ++x) {
            const uint8_t rgb[] = {row[4*x+2], row[4*x+1], row[4*x]};
            std::fwrite(rgb, 1, 3, file);
        }
    }
    std::fclose(file);
    lv_draw_buf_destroy(draw);
}
static lv_indev_data_t pointer_data{};
static void Swipe(lv_indev_t* pointer, lv_obj_t* target, bool up)
{
    lv_area_t area;
    lv_obj_get_coords(target, &area);
    pointer_data.point.x = (area.x1 + area.x2) / 2;
    int start = up ? area.y2 - 8 : area.y1 + 8;
    pointer_data.point.y = start;
    pointer_data.state = LV_INDEV_STATE_PRESSED;
    lv_tick_inc(20);
    lv_indev_read(pointer);
    for (int distance : {20, 50, 90}) {
        pointer_data.point.y = start + (up ? -distance : distance);
        lv_tick_inc(20);
        lv_indev_read(pointer);
    }
    pointer_data.state = LV_INDEV_STATE_RELEASED;
    lv_tick_inc(20);
    lv_indev_read(pointer);
}
int main(int argc, char** argv)
{
    assert(argc == 3);
    lv_init();
    fontpack_emulator_set_path(argv[1]);
    auto* display = lv_display_create(DISPLAY_CONTENT_W, DISPLAY_CONTENT_H);
    std::vector<uint8_t> pixels(DISPLAY_CONTENT_W * DISPLAY_CONTENT_H * 4);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(display, pixels.data(), nullptr, pixels.size(), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    auto* screen = lv_screen_active();
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, fontpack_lv_font_ui(), 0);
    std::filesystem::create_directories(argv[2]);
    auto* pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, [](lv_indev_t*, lv_indev_data_t* data) { *data = pointer_data; });
    int turned = 0;
    linux_ui::Actions actions;
    actions.choose = actions.interrupt = [] {};
    size_t selected_row = static_cast<size_t>(-1);
    actions.select = [&](size_t index) { selected_row = index; };
    actions.turn = [&](int delta) { turned += delta; };
    actions.send = [](const std::string&) { return true; };
    for (bool english : {false, true}) {
        if (english) English();
        {
        auto view = linux_ui::Terminal(screen, actions);
        view->Status(Lang::Strings::LINUX_RUNNING);
        assert(ScreenFontHasText(lv_obj_get_style_text_font(view->status, LV_PART_MAIN), Lang::Strings::LINUX_RUNNING));
        lv_label_set_text(view->terminal, "Welcome to Buildroot\nlogin: root\n~ # uname -r\n6.1.14\n~ #");
        lv_obj_update_layout(screen);
        assert(view->Rows() >= 30);
        auto layout = view->TextLayout();
        assert(layout.width == static_cast<size_t>(lv_obj_get_content_width(view->terminal)));
        assert(layout.advance['i'] < layout.advance['W']);
        linux_vm::Console output(layout);
        output.Append(std::string(300, 'i'));
        auto row = output.Window(view->Rows(), {});
        size_t first_line = row.find('\n');
        assert(first_line != std::string::npos);
        assert(first_line * layout.advance['i'] <= layout.width);
        assert((first_line + 1) * layout.advance['i'] > layout.width);
        std::string text;
        for (int i = 0; i < 12; ++i) text += "Linux version 6.1.14 running in a virtual machine with a full-width terminal. ";
        linux_vm::Console full(layout);
        full.Append(text);
        auto rendered = full.Window(view->Rows(), {});
        size_t begin = 0;
        for (size_t end; (end = rendered.find('\n', begin)) != std::string::npos;) {
            auto line = rendered.substr(begin, end - begin);
            lv_point_t extent{};
            lv_text_get_size(&extent, line.c_str(), &lv_font_montserrat_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            assert(extent.x <= static_cast<int>(layout.width));
            assert(extent.x >= static_cast<int>(layout.width * 9 / 10));
            begin = end + 1;
        }
        lv_label_set_text(view->terminal, rendered.c_str());
        assert(lv_obj_get_style_text_font(view->terminal, LV_PART_MAIN) == &lv_font_montserrat_14);
        Swipe(pointer, view->terminal, true);
        assert(turned == 1);
        Swipe(pointer, view->terminal, false);
        assert(turned == 0);
        assert(lv_obj_get_child_count(view->keyboard) > 30);
        assert(lv_obj_get_style_border_width(view->input, LV_PART_MAIN) > 0);
        assert(lv_obj_get_style_opa(view->input, LV_PART_CURSOR) == LV_OPA_TRANSP);
        assert(lv_textarea_get_placeholder_text(view->input) == nullptr || lv_textarea_get_placeholder_text(view->input)[0] == 0);
        Snapshot(screen, std::string(argv[2]) + (english ? "/terminal-en.ppm" : "/terminal.ppm"));
        }
        {
        auto view = linux_ui::List(screen, actions);
        assert(view->notice == nullptr);
        int capacity = view->capacity;
        assert(capacity >= 10);
        std::vector<std::string> names(capacity, "linux-6.1.14-rv32nommu-cnl-1-Image");
        names[0] = std::string(80, 'a') + "-Image";
        names[1] = std::string(80, 'a') + "-Image2";
        view->Items(names, 1, 3, false, names[1]);
        lv_obj_update_layout(screen);
        auto* list = view->list;
        auto* first = lv_obj_get_child(lv_obj_get_child(list, 0), 0);
        auto* second = lv_obj_get_child(lv_obj_get_child(list, 1), 0);
        assert(std::string(lv_label_get_text(first)) != lv_label_get_text(second));
        auto* short_name = lv_obj_get_child(lv_obj_get_child(list, 2), 0);
        const auto* filename_font = lv_obj_get_style_text_font(short_name, LV_PART_MAIN);
        assert(lv_obj_get_style_text_font(first, LV_PART_MAIN) == filename_font);
        assert(lv_obj_get_style_text_font(second, LV_PART_MAIN) == filename_font);
        const std::string shortened = lv_label_get_text(second);
        assert(shortened.size() < names[1].size());
        assert(shortened.find(ScreenFontHasText(filename_font, "…") ? "…" : "...") != std::string::npos);
        assert(ScreenFontHasText(filename_font, shortened.c_str()));
        lv_obj_send_event(lv_obj_get_child(list, 1), LV_EVENT_CLICKED, nullptr);
        assert(selected_row == 1);
        assert(lv_obj_get_style_text_align(first, LV_PART_MAIN) == LV_TEXT_ALIGN_LEFT);
        assert(lv_color_eq(lv_obj_get_style_bg_color(lv_obj_get_child(list, 1), LV_PART_MAIN), lv_color_black()));
        Swipe(pointer, list, true);
        assert(turned == 1);
        Swipe(pointer, list, false);
        assert(turned == 0);
        auto* fixed_font = lv_obj_get_style_text_font(view->status, LV_PART_MAIN);
        view->Status(Lang::Strings::LINUX_HISTORY_FMT);
        assert(lv_obj_get_style_text_font(view->status, LV_PART_MAIN) == fixed_font);
        assert(lv_obj_get_style_text_align(view->status, LV_PART_MAIN) == LV_TEXT_ALIGN_LEFT);
        auto* root_before = view->root;
        auto* status_before = view->status;
        int capacity_before = view->capacity;
        view->Items({}, 1, 3, false, "", Lang::Strings::LINUX_IMAGE_READ_ERROR);
        view->Items(names, 2, 3, false, names[1]);
        assert(view->root == root_before && view->status == status_before);
        assert(view->capacity == capacity_before);
        assert(std::string(lv_label_get_text(view->status)) == Lang::Strings::LINUX_CHOOSE);
        Snapshot(screen, std::string(argv[2]) + (english ? "/images-en.ppm" : "/images.ppm"));
        }
        {
        auto view = linux_ui::List(screen, actions, Lang::Strings::LINUX_IMAGE_READ_ERROR);
        view->Items({}, 0, 1, false, "");
        assert(std::string(lv_label_get_text(view->notice)) == Lang::Strings::LINUX_IMAGE_READ_ERROR);
        assert(lv_obj_has_flag(view->number, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_get_child_count(view->root) == 4); // 标题、完整错误、空列表，无页号。
        Snapshot(screen, std::string(argv[2]) + (english ? "/empty-en.ppm" : "/empty.ppm"));
        }
    }
    std::puts("PASS: Linux terminal, keyboard, image list, empty state and bilingual layout");
}
