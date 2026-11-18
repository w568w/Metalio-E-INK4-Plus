#include "linux_screen.h"
#include "linux_session.h"
#include "linux_ui.h"
#include "assets/lang_linux.h"
#include "fontpack_lvgl.h"
#include "screen_common.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"
#include "sd_directory_page.h"
#include "sd_paths.h"
#include "sd_access.h"
#include "settings.h"
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace {
constexpr char kDirectory[] = SD_APP_ROOT "/linux";
SdDirectoryPage directory; // 与系统目录分页约定一致，生命周期覆盖其异步任务。
struct Page {
    lv_obj_t* content = nullptr;
    lv_timer_t* timer = nullptr;
    // SCREEN_LOADED 前均为空；加载后恰好一个视图存在，切换只在 UI 线程执行。
    std::unique_ptr<linux_ui::ListView> list;
    std::unique_ptr<linux_ui::TerminalView> terminal;
    std::shared_ptr<linux_vm::Session> session;
    bool busy = false, persisted = false, automatic = false, was_standby = false, retry_directory = false;
    int index = 0;
    linux_vm::Console::Cursor cursor;
    std::string pending, message, shown, remembered;
    std::vector<std::string> paths;
};
Page* foreground = nullptr; // UI 线程专用，页面删除时解除绑定。

const char* FailureText(linux_vm::Failure failure)
{
    using namespace Lang::Strings;
    switch (failure) {
        case linux_vm::Failure::None: return "";
        case linux_vm::Failure::Memory: return LINUX_MEMORY_ERROR;
        case linux_vm::Failure::Queue: return LINUX_QUEUE_ERROR;
        case linux_vm::Failure::Task: return LINUX_TASK_ERROR;
        case linux_vm::Failure::SdBusy: return LINUX_SD_BUSY;
        case linux_vm::Failure::Open: return LINUX_IMAGE_ERROR;
        case linux_vm::Failure::Size: return LINUX_IMAGE_SIZE_ERROR;
        case linux_vm::Failure::Read: return LINUX_IMAGE_READ_ERROR;
    }
    return "";
}
bool Filter(const char* path, bool is_directory)
{
    const char* name = std::strrchr(path, '/');
    return !is_directory && name && name[1] != '.' && SdIsValidFileName(name + 1);
}
linux_ui::Actions ActionsFor(Page& page);
void FillList(Page& page)
{
    std::vector<std::string> names;
    for (const auto& path : page.paths) names.push_back(path.substr(path.find_last_of('/') + 1));
    page.list->Items(names, page.index, directory.Pages(), page.busy, page.remembered, page.message);
}
void RequestListPage(Page& page, int index, bool refresh = false)
{
    page.index = index;
    page.retry_directory = false;
    page.busy = true;
    page.message.clear();
    page.paths.clear();
    directory.Request({kDirectory}, Filter, index, page.list->capacity, refresh);
    FillList(page);
}
void ShowList(Page& page, const char* notice = "")
{
    page.terminal.reset();
    page.list = linux_ui::List(page.content, ActionsFor(page), notice);
}
void Choose(Page& page)
{
    if (page.session) page.session->stop = true;
    page.pending.clear();
    page.message.clear();
    ShowList(page);
    RequestListPage(page, 0, true);
}
void Start(Page& page, std::string path, bool automatic)
{
    directory.Cancel();
    page.automatic = automatic;
    page.pending = std::move(path);
    page.message.clear();
    page.persisted = false;
    page.cursor = {};
    page.shown.clear();
    if (page.session) page.session->stop = true;
    page.list.reset();
    page.terminal = linux_ui::Terminal(page.content, ActionsFor(page));
    page.terminal->Status(Lang::Strings::LINUX_LOADING);
}
bool Send(Page& page, const std::string& text)
{
    if (!page.session || page.session->status != linux_vm::Status::Running) {
        ScreenShowToast(Lang::Strings::LINUX_NOT_RUNNING);
        return false;
    }
    if (!page.session->Send(text)) {
        ScreenShowToast(Lang::Strings::LINUX_INPUT_FULL);
        return false;
    }
    return true;
}
void Turn(Page& page, int delta)
{
    if (page.list) {
        int index = page.index + delta;
        if (!page.busy && index >= 0 && index < directory.Pages()) RequestListPage(page, index);
        return;
    }
    if (!page.terminal || !page.session) return;
    auto boundary = page.session->Scroll(page.terminal->Rows(), delta, page.cursor);
    if (boundary == linux_vm::Console::Boundary::Oldest) ScreenShowToast(Lang::Strings::LINUX_OLDEST);
    else if (boundary == linux_vm::Console::Boundary::Latest) ScreenShowToast(Lang::Strings::LINUX_LATEST);
    page.shown.clear();
}
linux_ui::Actions ActionsFor(Page& page)
{
    linux_ui::Actions actions;
    actions.choose = [&page] { Choose(page); };
    actions.interrupt = [&page] { Send(page, "\x03"); };
    actions.send = [&page](const std::string& text) { return Send(page, text); };
    actions.turn = [&page](int delta) { Turn(page, delta); };
    actions.select = [&page](size_t index) {
        if (!page.busy && index < page.paths.size()) Start(page, page.paths[index], false);
    };
    return actions;
}
void UpdateList(Page& page)
{
    if (sd_access::IsQuiescing()) {
        if (!page.retry_directory) {
            page.retry_directory = true;
            directory.Cancel();
            page.busy = false;
            page.paths.clear();
            page.message = Lang::Strings::LINUX_SD_BUSY;
            FillList(page);
        }
        return;
    }
    if (page.retry_directory) RequestListPage(page, page.index, true);
    SdDirectoryPage::Result result;
    if (!directory.Poll(result)) return;
    page.busy = false;
    page.index = result.page;
    page.paths.clear();
    page.message = result.ok ? "" : Lang::Strings::LINUX_LIST_ERROR;
    if (result.ok) for (const auto& entry : result.entries) page.paths.push_back(entry.path);
    FillList(page);
}
void UpdateTerminal(Page& page)
{
    // 等上一个会话退出再建新的：同一时刻只保留一个模拟器任务和它占用的虚拟内存。
    if (!page.pending.empty() && (!page.session || page.session->done)) {
        auto path = std::move(page.pending);
        page.pending.clear();
        page.session = std::make_shared<linux_vm::Session>(page.terminal->TextLayout(), std::move(path));
        linux_vm::Launch(page.session);
    }
    if (!page.session || !page.pending.empty()) return;
    auto status = page.session->status.load();
    if (status == linux_vm::Status::Failed) {
        auto failure = page.session->failure.load();
        if (page.automatic) {
            if (failure == linux_vm::Failure::Open || failure == linux_vm::Failure::Size || failure == linux_vm::Failure::Read) {
                Settings settings("linux", true);
                settings.EraseKey("image");
                page.remembered.clear();
            }
            ScreenShowToast(Lang::Strings::LINUX_AUTO_FAILED);
        }
        page.automatic = false;
        ShowList(page, FailureText(failure));
        RequestListPage(page, 0, true);
        return;
    }
    // 成功装入即记忆，与虚拟机是否启动 getty/Shell 无关。
    if (status != linux_vm::Status::Loading && !page.persisted) {
        Settings settings("linux", true);
        const auto& path = page.session->image_path;
        page.remembered = path.substr(path.find_last_of('/') + 1);
        settings.SetString("image", page.remembered);
        page.persisted = true;
    }
    std::string message;
    switch (status) {
        case linux_vm::Status::Loading: message = Lang::Strings::LINUX_LOADING; break;
        case linux_vm::Status::Running: message = Lang::Strings::LINUX_RUNNING; break;
        case linux_vm::Status::PoweredOff: message = Lang::Strings::LINUX_POWERED_OFF; break;
        case linux_vm::Status::Restarted: message = Lang::Strings::LINUX_RESTART; break;
        case linux_vm::Status::Failed: break;
    }
    auto frame = page.session->AdvanceWindow(page.terminal->Rows(), page.cursor);
    if (frame.back) {
        char text[96];
        std::snprintf(text, sizeof(text), Lang::Strings::LINUX_HISTORY_FMT, static_cast<unsigned>(frame.back));
        message = text;
    }
    page.terminal->Status(message);
    if (page.shown != frame.text) {
        page.shown = std::move(frame.text);
        lv_label_set_text(page.terminal->terminal, page.shown.c_str());
    }
}
void Update(lv_timer_t* timer)
{
    auto& page = *static_cast<Page*>(lv_timer_get_user_data(timer));
    if (StandbyScreen::IsActive()) { page.was_standby = true; return; }
    if (page.was_standby) {
        page.was_standby = false;
        if (page.list) RequestListPage(page, page.index, true);
    }
    if (page.list) UpdateList(page);
    else if (page.terminal) UpdateTerminal(page);
}

}
void LinuxScreen::SetStandby(bool paused)
{
    if (foreground && foreground->session) foreground->session->paused = paused;
}
lv_obj_t* LinuxScreen::Create()
{
    auto* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    ScreenSetIsHome(false);
    auto bar = ScreenCreateStatusBar(screen);
    auto* page = new Page{};
    foreground = page;
    page->content = lv_obj_create(screen);
    lv_obj_remove_style_all(page->content);
    lv_obj_set_size(page->content, LV_HOR_RES, LV_VER_RES - bar.height);
    lv_obj_set_y(page->content, bar.height);
    lv_obj_clear_flag(page->content, LV_OBJ_FLAG_SCROLLABLE);
    page->timer = lv_timer_create(Update, 200, page);
    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        auto* page = static_cast<Page*>(lv_event_get_user_data(e));
        if (page->session) page->session->stop = true;
        directory.Cancel();
        lv_timer_delete(page->timer);
        if (foreground == page) foreground = nullptr;
        delete page;
    }, LV_EVENT_DELETE, page);
    VkKeyScreenDesc keys{};
    keys.factory = LinuxScreen::Create;
    keys.on_key = [](const char* key) {
        if (!foreground) return false;
        if (std::strcmp(key, "vk_prev") == 0) Turn(*foreground, -1);
        else if (std::strcmp(key, "vk_next") == 0) Turn(*foreground, 1);
        else return false;
        return true;
    };
    VkKey_AttachScreen(screen, "linux", keys);
    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        auto& page = *static_cast<Page*>(lv_event_get_user_data(e));
        Settings settings("linux");
        auto name = settings.GetString("image");
        page.remembered = name;
        if (!name.empty() && SdIsValidFileName(name.c_str())) Start(page, std::string(kDirectory) + "/" + name, true);
        else { ShowList(page); RequestListPage(page, 0, true); }
    }, LV_EVENT_SCREEN_LOADED, page);
    return screen;
}
