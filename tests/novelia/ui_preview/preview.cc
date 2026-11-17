#include "novelia_ui.h"
#include "assets/lang_novelia.h"
#include "fontpack_lvgl.h"
#include "ui_scale.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace Lang::Strings {
const char* NOVELIA_BACK = "返回";
const char* NOVELIA_SETTINGS = "设置";
const char* NOVELIA_FROM_FIRST = "从第一章开始";
const char* NOVELIA_START_READING = "开始阅读";
const char* NOVELIA_CONTINUE_READING = "继续阅读";
const char* NOVELIA_LOGIN_TO_VIEW = "登录后查看收藏";
const char* NOVELIA_NO_ITEMS = "暂无内容";
const char* NOVELIA_LOGIN_ON_PHONE = "在手机上完成账号登录";
const char* NOVELIA_SCAN_LOGIN = "扫码登录";
const char* NOVELIA_REFRESH = "刷新";
const char* NOVELIA_UNNAMED = "未命名";
const char* NOVELIA_LAST_READ = "上次";
const char* NOVELIA_PAGE_FMT = "第 %d 页";
const char* NOVELIA_ACCOUNT_CONNECTION = "账号与连接";
const char* NOVELIA_ACCOUNT_FMT = "当前登录：%s";
const char* NOVELIA_LOGGED_OUT = "未登录";
const char* NOVELIA_PROXY_FMT = "当前代理：%s";
const char* NOVELIA_PROXY_DIRECT = "未启用（直连）";
const char* NOVELIA_PROXY_UNCONFIGURED = "Trojan（未配置）";
const char* NOVELIA_PHONE_SETUP = "通过 WiFi 配置账户与代理设置";
const char* NOVELIA_LOGOUT = "退出登录";
const char* NOVELIA_READING_EXPERIENCE = "阅读体验";
const char* NOVELIA_PREFETCH = "下一章预取";
const char* NOVELIA_ON = "开启";
const char* NOVELIA_OFF = "关闭";
const char* NOVELIA_PREFETCH_HINT = "提前缓存下一章，方便翻章时快速阅读";
const char* NOVELIA_SCAN_SETUP = "扫码填写节点配置并登录 Novelia";
const char* NOVELIA_OPEN_LOGIN = "打开登录页面";
const char* NOVELIA_NEW_QR = "重新生成二维码";

const char* NOVELIA_CACHE_TITLE = "本地缓存";
const char* NOVELIA_CACHE_HINT = "点「统计缓存」查看占用";
const char* NOVELIA_CACHE_MB_FMT = "已使用 %.1f MB，共 %d 个文件";
const char* NOVELIA_CACHE_KB_FMT = "已使用 %.1f KB，共 %d 个文件";
const char* NOVELIA_CACHE_EMPTY = "无缓存";
const char* NOVELIA_CACHE_CLEARED = "缓存已清空";
const char* NOVELIA_CACHE_CLEAR_FAILED = "删除缓存失败，请检查 SD 卡";
const char* NOVELIA_CACHE_MEASURE_FAILED = "统计缓存失败，请检查 SD 卡";
const char* NOVELIA_CACHE_MEASURE = "统计缓存";
const char* NOVELIA_CACHE_CLEAR = "删除缓存";
const char* NOVELIA_TRANSLATION_SOURCE = "原文";
}

extern "C" void fontpack_emulator_set_path(const char*);
extern "C" void HapticAttachClick(lv_obj_t*) {}

static void CheckBounds(lv_obj_t* object) {
    lv_area_t parent; lv_obj_get_coords(object, &parent);
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        auto* child = lv_obj_get_child(object, i);
        lv_area_t area; lv_obj_get_coords(child, &area);
        assert(area.x1 >= parent.x1 && area.y1 >= parent.y1);
        assert(area.x2 <= parent.x2 && area.y2 <= parent.y2);
        CheckBounds(child);
    }
}
static void Dump(lv_obj_t* object, FILE* file, int depth = 0) {
    for (int i = 0; i < depth; ++i) fputs("  ", file);
    lv_area_t area; lv_obj_get_coords(object, &area);
    fprintf(file, "%d,%d %dx%d", int(area.x1), int(area.y1), int(lv_obj_get_width(object)), int(lv_obj_get_height(object)));
    if (lv_obj_check_type(object, &lv_label_class)) fprintf(file, " %s", lv_label_get_text(object));
    fputc('\n', file);
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) Dump(lv_obj_get_child(object, i), file, depth + 1);
}
static void Snapshot(lv_obj_t* screen, const std::string& path) {
    lv_obj_update_layout(screen);
    CheckBounds(screen);
    FILE* layout = fopen((path + ".layout").c_str(), "w");
    Dump(screen, layout); fclose(layout);
    auto* draw = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
    assert(draw);
    FILE* file = fopen(path.c_str(), "wb");
    assert(file);
    fprintf(file, "P6\n%u %u\n255\n", draw->header.w, draw->header.h);
    for (unsigned y = 0; y < draw->header.h; ++y) {
        const uint8_t* row = draw->data + y * draw->header.stride;
        for (unsigned x = 0; x < draw->header.w; ++x) {
            const uint8_t rgb[] = {row[4*x+2], row[4*x+1], row[4*x]};
            fwrite(rgb, 1, 3, file);
        }
    }
    fclose(file);
    lv_draw_buf_destroy(draw);
}
int main(int argc, char** argv) {
    assert(argc == 3);
    lv_init();
    fontpack_emulator_set_path(argv[1]);
    auto* font = fontpack_lv_font_ui();
    assert(font);
    auto* display = lv_display_create(DISPLAY_CONTENT_W, DISPLAY_CONTENT_H);
    std::vector<uint8_t> pixels(DISPLAY_CONTENT_W * DISPLAY_CONTENT_H * 4);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(display, pixels.data(), nullptr, pixels.size(), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    auto* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, font, 0);
    auto* status = lv_label_create(screen);
    lv_label_set_text(status, "Wi-Fi                         12:30                         95%");
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, UiSy(8));
    const int top = font->line_height + 2 * UiSy(8);
    auto* content = lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, DISPLAY_CONTENT_W, DISPLAY_CONTENT_H - top);
    lv_obj_set_y(content, top);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    std::filesystem::create_directories(argv[2]);
    novelia::ui::Actions actions;
    actions.back = actions.settings = actions.login = actions.logout = actions.refresh =
        actions.continue_reading = actions.measure_cache = actions.clear_cache = [] {};
    actions.set_prefetch = [](bool) {};
    actions.set_translation = [](std::string) {};
    actions.open = [](const novelia::Item&, int) {};
    std::vector<novelia::Item> items;
    novelia::ui::Page page{items};
    page.heading = "Novelia"; page.subtitle = "我的收藏夹"; page.username = "读者";
    items = {{"1", "正在阅读", {}, {}, false}, {"2", "想读的故事", {}, {}, false}};
    novelia::ui::Render(content, page, actions); Snapshot(screen, std::string(argv[2])+"/folders.ppm");
    page.view = novelia::ui::View::Novels; page.heading = "正在阅读"; page.subtitle = "在线收藏";
    items.clear();
    for (int i = 0; i < 8; ++i) items.push_back({std::to_string(i), i % 2 ? "在异世界的图书馆寻找那些被时间遗忘的故事与尚未写下结局的旅人们" : "雨后的旅人", {}, {}, false});
    page.count = 3;
    novelia::ui::Render(content, page, actions); Snapshot(screen, std::string(argv[2])+"/novels.ppm");
    lv_area_t before; lv_obj_get_coords(lv_obj_get_child(content, 2), &before);
    page.message = "正在读取下一页…"; page.busy = true;
    novelia::ui::Render(content, page, actions); Snapshot(screen, std::string(argv[2])+"/novels-loading.ppm");
    lv_area_t after; lv_obj_get_coords(lv_obj_get_child(content, 2), &after);
    assert(before.x1 == after.x1 && before.y1 == after.y1 && before.x2 == after.x2 && before.y2 == after.y2);
    page.message.clear(); page.busy = false;
    page.view = novelia::ui::View::Toc; page.heading = "小说目录"; page.subtitle = "在异世界的图书馆寻找失落的故事";
    page.translation = "sakura"; page.last_chapter = "3"; page.count = 42;
    items.clear(); items.push_back({{}, "第一卷　雨与书页", {}, {}, true});
    for (int i = 1; i < 8; ++i) items.push_back({std::to_string(i), i == 3 ? "雨后的图书馆" : "在旅途中遇见的人们与故事", {}, {}, false});
    novelia::ui::Render(content, page, actions); Snapshot(screen, std::string(argv[2])+"/directory.ppm");
    page.view = novelia::ui::View::Settings; page.heading = "Novelia 设置"; page.subtitle.clear(); page.proxy_summary = "Trojan · proxy.example:443";
    novelia::ui::Render(content, page, actions); Snapshot(screen, std::string(argv[2])+"/settings.ppm");
    page.cache_summary = "已使用 12.3 MB，共 84 个文件";
    novelia::ui::Render(content, page, actions); Snapshot(screen, std::string(argv[2])+"/settings-cache.ppm");
    page.busy = true;
    novelia::ui::Render(content, page, actions); Snapshot(screen, std::string(argv[2])+"/settings-cache-busy.ppm");
    page.busy = false;
    page.heading = "Novelia settings";
    Lang::Strings::NOVELIA_BACK = "Back";
    Lang::Strings::NOVELIA_SETTINGS = "Settings";
    Lang::Strings::NOVELIA_FROM_FIRST = "Start from chapter one";
    Lang::Strings::NOVELIA_START_READING = "Start reading";
    Lang::Strings::NOVELIA_CONTINUE_READING = "Continue reading";
    Lang::Strings::NOVELIA_LOGIN_TO_VIEW = "Log in to view favorites";
    Lang::Strings::NOVELIA_NO_ITEMS = "No items";
    Lang::Strings::NOVELIA_LOGIN_ON_PHONE = "Complete sign-in on your phone";
    Lang::Strings::NOVELIA_SCAN_LOGIN = "Scan to log in";
    Lang::Strings::NOVELIA_REFRESH = "Refresh";
    Lang::Strings::NOVELIA_UNNAMED = "Untitled";
    Lang::Strings::NOVELIA_LAST_READ = "Last read";
    Lang::Strings::NOVELIA_PAGE_FMT = "Page %d";
    Lang::Strings::NOVELIA_ACCOUNT_CONNECTION = "Account & connection";
    Lang::Strings::NOVELIA_ACCOUNT_FMT = "Logged in: %s";
    Lang::Strings::NOVELIA_LOGGED_OUT = "Not logged in";
    Lang::Strings::NOVELIA_PROXY_FMT = "Proxy: %s";
    Lang::Strings::NOVELIA_PROXY_DIRECT = "Disabled (direct)";
    Lang::Strings::NOVELIA_PROXY_UNCONFIGURED = "Trojan (not configured)";
    Lang::Strings::NOVELIA_PHONE_SETUP = "Set up account & proxy via Wi-Fi";
    Lang::Strings::NOVELIA_LOGOUT = "Log out";
    Lang::Strings::NOVELIA_READING_EXPERIENCE = "Reading experience";
    Lang::Strings::NOVELIA_PREFETCH = "Prefetch next chapter";
    Lang::Strings::NOVELIA_ON = "On";
    Lang::Strings::NOVELIA_OFF = "Off";
    Lang::Strings::NOVELIA_PREFETCH_HINT = "Cache the next chapter for faster reading";
    Lang::Strings::NOVELIA_SCAN_SETUP = "Scan to set up your proxy and sign in";
    Lang::Strings::NOVELIA_OPEN_LOGIN = "Open sign-in page";
    Lang::Strings::NOVELIA_NEW_QR = "Generate a new QR code";
    Lang::Strings::NOVELIA_CACHE_TITLE = "Local cache";
    Lang::Strings::NOVELIA_CACHE_MEASURE = "Measure cache";
    Lang::Strings::NOVELIA_CACHE_CLEAR = "Clear cache";
    page.cache_summary = "Used 12.3 MB in 84 files";
    novelia::ui::Render(content, page, actions); Snapshot(screen, std::string(argv[2])+"/settings-cache-en.ppm");
    page.view = novelia::ui::View::Login; page.heading = "手机登录"; page.subtitle = "手机与设备连接同一 Wi-Fi";
    page.qr_url = "http://192.168.1.179:8081/0123456789abcdef0123456789abcdef/";
    novelia::ui::Render(content, page, actions); Snapshot(screen, std::string(argv[2])+"/login.ppm");
    page.view = novelia::ui::View::Folders; page.heading = "Novelia"; page.subtitle = "我的收藏夹"; page.username.clear(); items.clear(); page.count = 1;
    novelia::ui::Render(content, page, actions); Snapshot(screen, std::string(argv[2])+"/empty.ppm");
    lv_display_delete(display);
    puts("UI snapshots rendered without a window");
}
