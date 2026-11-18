#include "novelia_screen.h"
#include "novelia_service.h"
#include "novelia_download.h"
#include "novelia_cache.h"
#include "assets/lang_novelia.h"
#include <cstdio>
#include <iterator>
#include "novelia_portal.h"
#include "novelia_ui.h"
#include "book_screen/book_external.h"
#include "book_screen/book_screen.h"
#include "screen_common.h"
#include "vk_key_handler.h"
#include "standby_screen/standby_screen.h"
#include "fontpack_lvgl.h"
#include "board.h"
#include "power_policy.h"
#include "sd_access.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <functional>
#include <atomic>
#include <algorithm>
#include <map>
#include <tuple>
#include <sys/stat.h>

namespace {
using namespace novelia;
using Action = std::function<void()>;
struct Work {
    Action run;
    bool needs_sd = true;
    unsigned generation;
};
using View = novelia::ui::View;
struct CatalogPage {
    std::vector<Item> items;
    int pages = 0;
    bool next = false;
};
using CatalogKey = std::tuple<std::string, std::string, int>;
struct State {
    Service service;              // 仅串行 worker 访问。
    QueueHandle_t jobs = nullptr; // worker 随固件存活，无工作时阻塞于队列。
    std::atomic<unsigned> epoch{0};
    std::atomic<bool> portal_live{false};

    // 以下状态仅由 UI 任务访问。
    int64_t portal_deadline = 0;
    lv_obj_t* root = nullptr;
    lv_obj_t* content = nullptr;
    ScreenPaintCoalesce paint;
    View view = View::Folders;
    View settings_return = View::Folders;
    View login_return = View::Settings;
    std::vector<Item> folders, novels, chapters;
    std::shared_ptr<const ReaderToc> reader_toc; // 与 chapters 同序，宿主回传的目录下标对两者都适用。
    Item folder, novel;
    std::string message, title, username, mode, portal_url;
    // prefetch 和空 proxy_summary 是首次配置结果返回前的 UI 占位，由 LoadFolders 一起更新。
    std::string last_chapter, proxy_summary;
    std::string cache_summary;
    bool busy = false, initialized = false, prefetch = true, was_standby = false;
    int page = 0, remote_page = 0, novel_pages = 0, folder_page = 0;
    bool novels_have_next = false;
    std::map<CatalogKey, CatalogPage> catalog_pages;
};
State state;
constexpr int kRows = novelia::ui::kRows;
constexpr int64_t kPortalLifetimeUs = 10LL * 60 * 1000 * 1000;
struct ViewSpec {
    std::vector<Item> State::*rows;
    bool remote_paging;
};
const ViewSpec& CurrentView() {
    static constexpr ViewSpec views[] = {
        {&State::folders, false},
        {&State::novels, true},
        {&State::chapters, false},
        {nullptr, false}, // Settings
        {nullptr, false}, // Login
    };
    static_assert(std::size(views) == static_cast<size_t>(View::Count));
    return views[static_cast<unsigned>(state.view)];
}
void Render();
void Refresh();
void OpenChapter(int index, StartAt start = StartAt::Saved);
bool TrySubmit(Action action, bool needs_sd = true) {
    if (!state.jobs)
        return false;
    auto* work = new Work{std::move(action), needs_sd, state.epoch.load()};
    if (xQueueSend(state.jobs, &work, 0) == pdTRUE)
        return true;
    delete work;
    return false;
}
void Main(Action action) {
    // worker 可能持有 SD 事务；队列满时立即放弃，不能等待 UI 而阻塞待机/USB 切换。
    auto* callback = new Action(std::move(action));
    if (!ScreenLvAsync(
            [](void* context) {
                std::unique_ptr<Action> callback(static_cast<Action*>(context));
                (*callback)();
            },
            callback)) {
        delete callback;
        ESP_LOGE("Novelia", "UI callback dropped: adapter unavailable or queue full");
    }
}
void Repaint() {
    ScreenPaintCoalesceRequest(&state.paint);
}
// 仅由 UI 任务调用；worker / 手机请求回调使用不访问 UI 状态的 TrySubmit。
void Submit(Action action, bool needs_sd = true) {
    if (state.jobs && !TrySubmit(std::move(action), needs_sd)) {
        state.busy = false;
        state.message = Lang::Strings::NOVELIA_QUEUE_FULL;
        Repaint();
    }
}
void Complete(unsigned generation, Action action) {
    Main([generation, action = std::move(action)] {
        if (generation != state.epoch.load())
            return;
        state.busy = false;
        action();
        Repaint();
    });
}
void StopLogin() {
    if (!state.portal_live.exchange(false))
        return;
    Submit([] { StopPortal(); }, false);
}
void CancelPendingWork() {
    ++state.epoch;
    state.busy = false;
    if (state.portal_live.load()) {
        StopLogin();
        state.portal_url.clear();
        state.message = Lang::Strings::NOVELIA_PORTAL_CLOSED;
    } else {
        // 在途任务已丢弃，没有结果可等，不要把加载提示留在页面上。
        state.message.clear();
    }
    Repaint();
}
void Back() {
    if (state.busy) {
        ++state.epoch;
        state.busy = false;
    }
    StopLogin();
    state.portal_url.clear();
    if (state.view == View::Login) {
        state.view = state.login_return;
    } else if (state.view == View::Settings) {
        state.view = state.settings_return;
    } else if (state.view == View::Toc) {
        state.view = View::Novels;
        state.page = 0;
    } else if (state.view == View::Novels) {
        state.view = View::Folders;
        state.page = state.folder_page;
    } else {
        ScreenRequestHome();
        return;
    }
    state.message.clear();
    Repaint();
}
void LoadFolders() {
    state.busy = true;
    state.message = Lang::Strings::NOVELIA_LOADING_FOLDERS;
    Repaint();
    unsigned generation = ++state.epoch;
    Submit([generation] {
        auto user = state.service.Username();
        auto translation = state.service.Translation();
        auto data =
            user.empty() ? Fetched{} : state.service.Fetch("/user/favored", "folders");
        auto error = std::move(data.error);
        auto items = Folders(data.json.get(), data.json ? &error : nullptr);
        auto proxy = state.service.ProxySummary();
        bool fetch_next = state.service.Prefetch();
        Complete(generation, [items = std::move(items), error, user, translation, fetch_next, proxy] {
            ESP_LOGI("Novelia", "folders loaded: count=%u", (unsigned)items.size());
            state.folders = items;
            state.username = user;
            state.proxy_summary = proxy;
            state.mode = translation;
            state.prefetch = fetch_next;
            state.view = View::Folders;
            state.message = error;
        });
    });
}
void LoadNovels(bool refresh = false) {
    const CatalogKey key{state.username, state.folder.id, state.remote_page};
    if (!refresh) {
        auto found = state.catalog_pages.find(key);
        if (found != state.catalog_pages.end()) {
            const auto& saved = found->second;
            state.novels = saved.items;
            state.novel_pages = saved.pages;
            state.novels_have_next = saved.next;
            state.view = View::Novels;
            state.page = 0;
            state.message.clear();
            Repaint();
            ESP_LOGI("Novelia", "catalog page cache hit: page=%d", state.remote_page + 1);
            return;
        }
    }
    state.busy = true;
    state.message = Lang::Strings::NOVELIA_LOADING_NOVELS;
    Repaint();
    unsigned generation = ++state.epoch;
    auto id = state.folder.id;
    int requested_page = state.remote_page;
    Submit([generation, id, requested_page, refresh, key] {
        const auto path = "/user/favored-web/" + Quote(id) + "?page=" + std::to_string(requested_page) +
                          "&pageSize=" + std::to_string(kRows) +
                          "&query=&provider=kakuyomu,syosetu,novelup,hameln,pixiv,alphapolis&type=0&level=0&"
                          "translate=0&sort=update";
        auto data =
            state.service.Fetch(path, path, refresh ? FetchPolicy::Refresh : FetchPolicy::PreferCache);
        auto error = std::move(data.error);
        auto items = Novels(data.json.get(), data.json ? &error : nullptr);
        const auto* count = cJSON_GetObjectItemCaseSensitive(data.json.get(), "pageNumber");
        int total = cJSON_IsNumber(count) ? std::max(1, count->valueint) : 0;
        // 兼容未返回 pageNumber 的响应：满页时允许继续探测，空页即停止。
        bool next = total ? requested_page + 1 < total : items.size() == kRows;
        Complete(generation, [items = std::move(items), error, total, next, key] {
            if (!items.empty() || error.empty())
                state.catalog_pages[key] = CatalogPage{items, total, next};
            state.novels = items;
            state.novel_pages = total;
            state.novels_have_next = next;
            state.view = View::Novels;
            state.message = error;
            state.page = 0;
        });
    });
}
void LoadToc() {
    state.busy = true;
    state.message = Lang::Strings::NOVELIA_LOADING_TOC;
    Repaint();
    unsigned generation = ++state.epoch;
    auto book = state.novel;
    Submit([generation, book] {
        auto data = state.service.Fetch("/novel/" + Quote(book.provider) + "/" + Quote(book.novel), book.id);
        auto error = std::move(data.error);
        auto items = Toc(data.json.get(), data.json ? &error : nullptr);
        auto book_title = Title(data.json.get());
        auto reader_toc = std::make_shared<ReaderToc>();
        reader_toc->title = book_title.empty() ? book.title : book_title;
        for (const auto& item : items)
            reader_toc->entries.push_back({item.title, !item.volume});
        auto progress = Parse(state.service.Last(book));
        auto remote = Text(data.json.get(), "lastReadChapterId");
        auto last = ResumeChapter(items, Text(progress.get(), "chapter"), remote);
        // 本地进度比云端新时补推：离线读过的章节不会只留在本机，联网后打开目录即补上。
        if (!last.empty() && last != remote)
            for (const auto& item : items)
                if (item.id == last) {
                    state.service.Shown(book, item);
                    break;
                }
        Complete(generation, [items = std::move(items), reader_toc, error, book_title, last] {
            ESP_LOGI("Novelia", "directory loaded: count=%u", (unsigned)items.size());
            state.chapters = items;
            state.reader_toc = reader_toc;
            state.view = View::Toc;
            state.message = error;
            state.title = book_title;
            state.last_chapter = last;
        });
    });
}
void Refresh() {
    if (state.busy)
        return;
    if (state.view == View::Folders)
        LoadFolders();
    else if (state.view == View::Novels)
        LoadNovels(true);
    else if (state.view == View::Toc)
        LoadToc();
}
void OpenChapter(int index, StartAt start) {
    if (state.busy || index < 0 || index >= static_cast<int>(state.chapters.size()) ||
        state.chapters[index].volume)
        return;
    state.busy = true;
    state.message = Lang::Strings::NOVELIA_LOADING_CHAPTER;
    if (BookScreen::IsReadScreenActive())
        ScreenShowToast(state.message.c_str());
    Repaint();
    unsigned generation = ++state.epoch;
    auto book = state.novel, chapter = state.chapters[index];
    auto translation = state.mode;
    Submit([generation, book, chapter, translation, index, start] {
        auto result = Download(state.service, book, chapter, translation);
        Complete(generation, [result, book, chapter, translation, index, start] {
            if ((!state.root || lv_screen_active() != state.root) && !BookScreen::IsReadScreenActive())
                return;
            if (!result.error.empty()) {
                state.message = result.error;
                const auto& available = result.available;
                if (!available.empty()) {
                    std::string names;
                    for (const auto& name : available)
                        names += (names.empty() ? "" : ", ") + name;
                    state.message =
                        Format(Lang::Strings::NOVELIA_TRANSLATION_AVAILABLE_FMT, names.c_str());
                }
                if (BookScreen::IsReadScreenActive())
                    ScreenShowToast(state.message.c_str());
                return;
            }
            state.message.clear();
            ReaderLaunch request;
            HostToc host_toc;
            host_toc.toc = state.reader_toc;
            host_toc.index = index;
            host_toc.open = [](int target) {
                if (state.busy) return false;
                OpenChapter(target, StartAt::First);
                return state.busy;
            };
            request.host_toc = host_toc;
            request.book.path = result.path;
            request.book.title = book.title + " · " + chapter.title;
            request.book.format = reader::BookFormat::kEpub;
            struct stat info{};
            if (stat(result.path.c_str(), &info) == 0) {
                request.book.file_size = info.st_size;
                request.book.mtime = info.st_mtime;
            }
            request.start = start;
            request.suspending = CancelPendingWork;
            request.returning = [] {
                ++state.epoch;
                state.busy = false;
                state.message.clear();
            };
            request.adjacent = [host_toc](int direction) {
                const int next = host_toc.Adjacent(direction);
                if (next < 0) {
                    ScreenShowToast(direction > 0 ? Lang::Strings::NOVELIA_LAST_CHAPTER
                                                  : Lang::Strings::NOVELIA_FIRST_CHAPTER);
                    return true;
                }
                if (!state.busy)
                    OpenChapter(next, direction > 0 ? StartAt::First : StartAt::Last);
                return true;
            };
            request.opened = [book, chapter, translation, index, host_toc] {
                state.last_chapter = chapter.id;
                state.page = index / kRows;
                Submit([book, chapter] {
                    if (!state.service.Shown(book, chapter)) {
                        auto error = state.service.error;
                        ESP_LOGW("Novelia", "reading progress: %s", error.c_str());
                        if (error == Lang::Strings::NOVELIA_LOGIN_EXPIRED)
                            Main([error] {
                                if (BookScreen::IsReadScreenActive())
                                    ScreenShowToast(error.c_str());
                            });
                    }
                });
                if (state.prefetch) {
                    const int next = host_toc.Adjacent(1);
                    if (next >= 0) {
                        auto following = state.chapters[next];
                        Submit([book, following, translation] {
                            Download(state.service, book, following, translation);
                        });
                    }
                }
            };
            BookExternalOpen(std::move(request));
        });
    });
}
void Login() {
    if (state.busy)
        return;
    if (state.view != View::Login)
        state.login_return = state.view;
    state.view = View::Login;
    state.busy = true;
    state.message = Lang::Strings::NOVELIA_STARTING_PORTAL;
    Repaint();
    unsigned generation = ++state.epoch;
    Submit([generation] {
        if (!state.service.Ready()) {
            auto error = state.service.error;
            Complete(generation, [error] { state.message = error; });
            return;
        }
        PortalStatus(Lang::Strings::NOVELIA_PORTAL_READY, state.service.PublicConfig());
        auto url = StartPortal([generation](std::string body) {
            return TrySubmit([generation, body = std::move(body)] {
                if (generation != state.epoch.load() || !state.portal_live.load())
                    return;
                auto input = Parse(body);
                auto action = Text(input.get(), "action");
                auto form = ApplyForm(input.get(), state.service);
                const bool ok = form.ok;
                const auto& status_message = form.message;
                ESP_LOGI("Novelia", "phone action=%s ok=%d", action.c_str(), ok);
                if (!ok)
                    ESP_LOGW("Novelia", "operation failed: %s", status_message.c_str());
                PortalStatus(status_message, state.service.PublicConfig());
                auto user = state.service.Username();
                auto proxy = state.service.ProxySummary();
                Main([generation, action, ok, status_message, user, proxy] {
                    if (generation != state.epoch.load())
                        return;
                    state.message = status_message;
                    state.username = user;
                    state.proxy_summary = proxy;
                    if (ok && action == "login") {
                        state.catalog_pages.clear();
                        StopLogin();
                        state.view = View::Folders;
                        state.page = 0;
                        LoadFolders();
                    }
                    Repaint();
                });
            });
        });
        if (generation != state.epoch.load()) {
            StopPortal();
            return;
        }
        state.portal_live.store(!url.empty());
        Complete(generation, [url] {
            state.portal_url = url;
            state.portal_deadline = esp_timer_get_time() + kPortalLifetimeUs;
            state.message = url.empty() ? Lang::Strings::NOVELIA_PORTAL_FAILED : "";
        });
    });
}
const std::vector<Item>& Rows() {
    static const std::vector<Item> empty;
    auto rows = CurrentView().rows;
    return rows ? state.*rows : empty;
}
int PageCount(const std::vector<Item>& items) {
    return std::max(1, (int(items.size()) + kRows - 1) / kRows);
}
void Turn(int direction) {
    const auto& view = CurrentView();
    if (state.busy || !view.rows)
        return;
    if (view.remote_paging) {
        if ((direction < 0 && state.remote_page > 0) || (direction > 0 && state.novels_have_next)) {
            state.remote_page += direction;
            LoadNovels();
        }
    } else {
        int pages = PageCount(Rows());
        state.page = std::clamp(state.page + direction, 0, pages - 1);
        Repaint();
    }
}
void OpenSettings() {
    if (!state.busy && state.view != View::Settings) {
        state.settings_return = state.view;
        state.view = View::Settings;
        state.message.clear();
        Repaint();
    }
}

void ContinueReading() {
    if (state.view != View::Toc)
        return;
    for (size_t i = 0; i < state.chapters.size(); ++i) {
        if (!state.last_chapter.empty() && state.chapters[i].id == state.last_chapter) {
            OpenChapter(i);
            return;
        }
    }
    for (size_t i = 0; i < state.chapters.size(); ++i) {
        if (!state.chapters[i].volume) {
            OpenChapter(i);
            return;
        }
    }
}

void SetTranslation(std::string selected) {
    if (state.busy || state.view != View::Toc || selected == state.mode)
        return;
    state.busy = true;
    unsigned generation = ++state.epoch;
    Submit([selected, generation] {
        bool ok = state.service.SetTranslation(selected);
        Complete(generation, [selected, ok] {
            if (ok)
                state.mode = selected;
            else
                state.message = Lang::Strings::NOVELIA_SETTING_SAVE_FAILED;
        });
    });
}

void SetPrefetch(bool enabled) {
    if (state.busy || enabled == state.prefetch)
        return;
    state.busy = true;
    unsigned generation = ++state.epoch;
    Submit([generation, enabled] {
        bool ok = state.service.SetPrefetch(enabled);
        Complete(generation, [enabled, ok] {
            if (ok)
                state.prefetch = enabled;
            else
                state.message = Lang::Strings::NOVELIA_SETTING_SAVE_FAILED;
        });
    });
}

std::string CacheSummary(const CacheUsage& usage) {
    if (!usage.ok)
        return Lang::Strings::NOVELIA_CACHE_MEASURE_FAILED;
    if (!usage.files)
        return Lang::Strings::NOVELIA_CACHE_EMPTY;
    const bool megabytes = usage.bytes >= 1024 * 1024;
    const auto* format = megabytes ? Lang::Strings::NOVELIA_CACHE_MB_FMT : Lang::Strings::NOVELIA_CACHE_KB_FMT;
    const double size = double(usage.bytes) / (megabytes ? 1024 * 1024 : 1024);
    return Format(format, size, usage.files);
}
void ManageCache(bool clear) {
    if (state.busy)
        return;
    state.busy = true;
    state.message.clear();
    Repaint();
    const unsigned generation = ++state.epoch;
    Submit([generation, clear] {
        auto usage = clear ? ClearCache(state.service.Root()) : MeasureCache(state.service.Root());
        Complete(generation, [usage, clear] {
            if (clear) {
                state.catalog_pages.clear();
                state.cache_summary = usage.ok ? Lang::Strings::NOVELIA_CACHE_CLEARED
                                               : Lang::Strings::NOVELIA_CACHE_CLEAR_FAILED;
            } else
                state.cache_summary = CacheSummary(usage);
        });
    });
}

void Logout() {
    if (state.busy)
        return;
    state.busy = true;
    unsigned generation = ++state.epoch;
    Submit([generation] {
        bool ok = state.service.Logout();
        Complete(generation, [ok] {
            if (ok) {
                state.username.clear();
                state.folders.clear();
                state.novels.clear();
                state.chapters.clear();
                state.reader_toc.reset();
                state.catalog_pages.clear();
            }
            state.message = ok ? Lang::Strings::NOVELIA_LOGOUT_DONE : Lang::Strings::NOVELIA_LOGOUT_FAILED;
        });
    });
}

void OpenItem(View source, const Item& item, int index) {
    if (state.busy || state.view != source)
        return;
    if (state.view == View::Folders) {
        state.folder_page = state.page;
        state.folder = item;
        state.remote_page = 0;
        state.page = 0;
        LoadNovels();
    } else if (state.view == View::Novels) {
        state.novel = item;
        state.page = 0;
        LoadToc();
    } else
        OpenChapter(index);
}

void Render() {
    if (!state.root || !state.content || lv_screen_active() != state.root)
        return;
    const auto& view = CurrentView();
    if (view.rows && !view.remote_paging) {
        int pages = PageCount(Rows());
        state.page = std::clamp(state.page, 0, pages - 1);
    }
    novelia::ui::Page data{Rows()};
    data.view = state.view;
    switch (state.view) {
    case View::Folders:
        data.heading = "Novelia";
        data.subtitle = Lang::Strings::NOVELIA_FOLDERS_SUBTITLE;
        break;
    case View::Novels:
        data.heading = Lang::Strings::NOVELIA_NOVELS_HEADING;
        data.subtitle = state.folder.title;
        break;
    case View::Toc:
        data.heading = Lang::Strings::NOVELIA_TOC_HEADING;
        data.subtitle = state.title.empty() ? state.novel.title : state.title;
        break;
    case View::Settings:
        data.heading = Lang::Strings::NOVELIA_SETTINGS_HEADING;
        break;
    case View::Login:
        data.heading = Lang::Strings::NOVELIA_LOGIN_HEADING;
        data.subtitle = Lang::Strings::NOVELIA_LOGIN_HINT;
        break;
    case View::Count:
        return;
    }
    data.message = state.message;
    data.username = state.username;
    data.proxy_summary = state.proxy_summary;
    data.translation = state.mode;
    data.last_chapter = state.last_chapter;
    data.qr_url = state.portal_url;
    data.busy = state.busy;
    data.prefetch = state.prefetch;
    data.cache_summary = state.cache_summary;
    data.index = view.remote_paging ? state.remote_page : state.page;
    data.count = view.remote_paging ? state.novel_pages : PageCount(Rows());

    novelia::ui::Actions actions;
    actions.back = Back;
    actions.settings = OpenSettings;
    actions.login = Login;
    actions.refresh = Refresh;
    actions.open = [source = state.view](const Item& item, int index) { OpenItem(source, item, index); };
    actions.continue_reading = ContinueReading;
    actions.set_translation = SetTranslation;
    actions.set_prefetch = SetPrefetch;
    actions.logout = Logout;
    actions.measure_cache = [] { ManageCache(false); };
    actions.clear_cache = [] { ManageCache(true); };
    novelia::ui::Render(state.content, data, actions);
}
void Initialize() {
    if (state.initialized)
        return;
    state.initialized = true;
    state.paint.paint = Render;
    state.service.network_ready = [] { return Board::GetInstance().EnsureNetworkReady(); };
    state.service.network_present = [] { return Board::GetInstance().IsNetworkReady(); };
    state.jobs = xQueueCreate(8, sizeof(Work*));
    if (!state.jobs) {
        state.message = Lang::Strings::NOVELIA_QUEUE_CREATE_FAILED;
        return;
    }
    auto created = xTaskCreate(
        [](void*) {
            Work* work;
            while (xQueueReceive(state.jobs, &work, portMAX_DELAY) == pdTRUE) {
                {
                    PowerNeedHold power(PowerNeed::ForegroundNetwork);
                    unsigned job_epoch = work->generation;
                    state.service.cancelled = [job_epoch] { return job_epoch != state.epoch.load(); };
                    if (!work->needs_sd)
                        work->run(); // 关闭 portal 等清理任务，即使请求已过期也必须执行。
                    else {
                        sd_access::Transaction sd;
                        if (sd && state.service.Init()) {
                            if (!state.service.cancelled())
                                work->run();
                        } else
                            Complete(job_epoch,
                                     [error = sd ? state.service.error : Lang::Strings::NOVELIA_SD_BUSY] {
                                         state.message = error;
                                     });
                    }
                }
                delete work;
            }
            vTaskDelete(nullptr);
        },
        "novelia", 16384, nullptr, 2, nullptr);
    if (created != pdPASS) {
        vQueueDelete(state.jobs);
        state.jobs = nullptr;
        state.message = Lang::Strings::NOVELIA_WORKER_FAILED;
        return;
    }
    LoadFolders();
}
} // namespace
lv_obj_t* NoveliaScreen::Create() {
    Initialize();
    auto* screen = lv_obj_create(nullptr);
    state.root = screen;
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    ScreenSetIsHome(false);
    auto status = ScreenCreateStatusBar(screen);
    state.content = lv_obj_create(screen);
    lv_obj_remove_style_all(state.content);
    lv_obj_set_size(state.content, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(state.content, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_clear_flag(state.content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(state.content, LV_OBJ_FLAG_CLICKABLE);
    auto* timer = lv_timer_create(
        [](lv_timer_t*) {
            const bool standby = StandbyScreen::IsActive();
            if ((standby && !state.was_standby) ||
                (state.portal_live.load() && esp_timer_get_time() >= state.portal_deadline)) {
                CancelPendingWork();
            }
            state.was_standby = standby;
        },
        100, nullptr);
    lv_obj_add_event_cb(
        screen,
        [](lv_event_t* event) {
            if (auto* timer = static_cast<lv_timer_t*>(lv_event_get_user_data(event)))
                lv_timer_delete(timer);
            if (lv_event_get_target(event) == state.root) {
                state.root = nullptr;
                state.content = nullptr;
                if (state.busy || state.portal_live.load()) CancelPendingWork();
            }
        },
        LV_EVENT_DELETE, timer);
    lv_obj_add_event_cb(screen, [](lv_event_t*) { Repaint(); }, LV_EVENT_SCREEN_LOADED, nullptr);
    VkKeyScreenDesc keys{};
    keys.factory = NoveliaScreen::Create;
    keys.on_key = [](const char* key) {
        if (std::string(key) == "vk_home")
            Back();
        else if (std::string(key) == "vk_prev")
            Turn(-1);
        else if (std::string(key) == "vk_next")
            Turn(1);
        return true;
    };
    VkKey_AttachScreen(screen, "novelia", keys);
    return screen;
}
