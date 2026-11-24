#include "web_novel_screen.h"
#include "web_novel_service.h"
#include "web_novel_download.h"
#include "web_novel_cache.h"
#include "web_novel_portal.h"
#include "web_novel_ui.h"
#include "novelia_site.h"
#include "novalpie_site.h"
#include "assets/lang_web_novel.h"
#include "assets/lang_reader.h"
#include "book_screen/book_external.h"
#include "book_screen/book_screen.h"
#include "screen_common.h"
#include "vk_key_handler.h"
#include "standby_screen/standby_screen.h"
#include "fontpack_lvgl.h"
#include "board.h"
#include "power_policy.h"
#include "sd_access.h"
#include "sd_paths.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>

namespace {
using namespace web_novel;
using Action = std::function<void()>;
using Job = std::function<void(Service&)>;
using View = web_novel::ui::View;
using CatalogKey = std::tuple<std::string, std::string, int>;

enum class SiteIndex : size_t { Novelia = 0, Novalpie = 1, Count = 2 };

struct SiteDescriptor {
    SiteIndex index;
    const char* id;
    const char* app_name;
    bool supports_translation;
    lv_obj_t* (*factory)();
    std::unique_ptr<Service> (*make_service)();
};

constexpr std::array<SiteDescriptor, static_cast<size_t>(SiteIndex::Count)> kSites = {{
    {SiteIndex::Novelia, "novelia", "Novelia", true, WebNovelScreen::CreateNovelia,
     []() -> std::unique_ptr<Service> { return std::make_unique<NoveliaService>(); }},
    {SiteIndex::Novalpie, "novalpie", "Novalpie", false, WebNovelScreen::CreateNovalpie,
     []() -> std::unique_ptr<Service> { return std::make_unique<NovalpieService>(); }},
}};

const SiteDescriptor& SiteInfo(SiteIndex index) {
    return kSites[static_cast<size_t>(index)];
}

const SiteDescriptor* FindSiteById(const std::string& id) {
    for (const auto& site : kSites)
        if (id == site.id)
            return &site;
    return nullptr;
}

const SiteDescriptor* FindSiteByChapterPath(const std::string& path) {
    for (const auto& site : kSites) {
        const std::string prefix = std::string(SD_APP_ROOT) + "/" + site.id + "/chapters/";
        if (path.rfind(prefix, 0) == 0)
            return &site;
    }
    return nullptr;
}

struct Work {
    Job run;
    bool needs_sd = true;
    unsigned generation;
    SiteIndex site;
};

struct ViewState {
    View view = View::Folders;
    View settings_return = View::Folders;
    View login_return = View::Settings;
    std::vector<Item> folders, novels, chapters;
    std::shared_ptr<const ReaderToc> reader_toc; // 与 chapters 同序，宿主回传的目录下标对两者都适用。
    Item folder, novel;
    std::string message, title, username, mode, portal_url;
    // prefetch 和空 proxy_summary 是首次配置结果返回前的 UI 占位，由 LoadFolders 一起更新。
    std::string last_chapter, proxy_summary, cache_summary;
    bool busy = false, prefetch = true, was_standby = false;
    int page = 0, remote_page = 0, novel_pages = 0, folder_page = 0;
    bool novels_have_next = false;
    std::map<CatalogKey, CatalogPage> catalog_pages;
};

struct State {
    std::atomic<SiteIndex> site{SiteIndex::Novelia}; // HTTP 配置页也可以向当前站点提交任务。
    QueueHandle_t jobs = nullptr;                    // worker 随固件存活，无工作时阻塞于队列。
    std::atomic<unsigned> epoch{0};
    std::atomic<bool> portal_live{false};

    // 以下状态仅由 UI 任务访问。
    int64_t portal_deadline = 0;
    lv_obj_t* root = nullptr;
    lv_obj_t* content = nullptr;
    ScreenPaintCoalesce paint;
    bool initialized = false;
    ViewState ui;
};
State state;
constexpr int kRows = web_novel::ui::kRows;
constexpr int64_t kPortalLifetimeUs = 10LL * 60 * 1000 * 1000;

struct ViewSpec {
    std::vector<Item> ViewState::*rows;
    bool remote_paging;
};
const ViewSpec& CurrentView() {
    static constexpr ViewSpec views[] = {
        {&ViewState::folders, false},
        {&ViewState::novels, true},
        {&ViewState::chapters, false},
        {nullptr, false}, // Settings
        {nullptr, false}, // Login
    };
    static_assert(std::size(views) == static_cast<size_t>(View::Count));
    return views[static_cast<unsigned>(state.ui.view)];
}

void Render();
void Refresh();
struct BootReading {
    std::string path, chapter, username;
};
void OpenChapter(int index, StartAt start = StartAt::Saved, std::string cached_path = {});
void LoadFolders();

bool TrySubmit(Job action, SiteIndex site, bool needs_sd = true) {
    if (!state.jobs)
        return false;
    auto* work = new Work{std::move(action), needs_sd, state.epoch.load(), site};
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
        ESP_LOGE("WebNovel", "UI callback dropped: adapter unavailable or queue full");
    }
}
void Repaint() {
    ScreenPaintCoalesceRequest(&state.paint);
}
// 仅由 UI 任务调用；worker / 手机请求回调使用不访问 UI 状态的 TrySubmit。
void Submit(Job action, bool needs_sd = true) {
    if (state.jobs && !TrySubmit(std::move(action), state.site.load(), needs_sd)) {
        state.ui.busy = false;
        state.ui.message = Lang::Strings::NOVELIA_QUEUE_FULL;
        Repaint();
    }
}
void Complete(unsigned generation, Action action) {
    Main([generation, action = std::move(action)] {
        if (generation != state.epoch.load())
            return;
        state.ui.busy = false;
        action();
        Repaint();
    });
}
void StopLogin() {
    if (!state.portal_live.exchange(false))
        return;
    Submit([](Service&) { StopPortal(); }, false);
}
void CancelPendingWork() {
    ++state.epoch;
    state.ui.busy = false;
    if (state.portal_live.load()) {
        StopLogin();
        state.ui.portal_url.clear();
        state.ui.message = Lang::Strings::NOVELIA_PORTAL_CLOSED;
    } else {
        // 在途任务已丢弃，没有结果可等，不要把加载提示留在页面上。
        state.ui.message.clear();
    }
    Repaint();
}
void Back() {
    if (state.ui.busy) {
        ++state.epoch;
        state.ui.busy = false;
    }
    StopLogin();
    state.ui.portal_url.clear();
    if (state.ui.view == View::Login) {
        state.ui.view = state.ui.login_return;
    } else if (state.ui.view == View::Settings) {
        state.ui.view = state.ui.settings_return;
    } else if (state.ui.view == View::Toc) {
        if (state.ui.folder.id.empty()) {
            LoadFolders();
            return;
        }
        state.ui.view = View::Novels;
        state.ui.page = 0;
    } else if (state.ui.view == View::Novels) {
        state.ui.view = View::Folders;
        state.ui.page = state.ui.folder_page;
    } else {
        ScreenRequestHome();
        return;
    }
    state.ui.message.clear();
    Repaint();
}
void LoadFolders() {
    state.ui.busy = true;
    state.ui.message = Lang::Strings::NOVELIA_LOADING_FOLDERS;
    Repaint();
    unsigned generation = ++state.epoch;
    Submit([generation](Service& service) {
        auto user = service.Username();
        auto translation = service.Translation();
        auto data = service.LoadFolders();
        auto proxy = service.ProxySummary();
        bool fetch_next = service.Prefetch();
        Complete(generation, [items = std::move(data.items), error = std::move(data.error), user, translation,
                              fetch_next, proxy] {
            ESP_LOGI("WebNovel", "folders loaded: count=%u", (unsigned)items.size());
            if (error.empty())
                state.ui.catalog_pages.clear(); // 书架会一次替换所有分组共用的快照。
            state.ui.folders = items;
            state.ui.username = user;
            state.ui.proxy_summary = proxy;
            state.ui.mode = translation;
            state.ui.prefetch = fetch_next;
            state.ui.view = View::Folders;
            state.ui.message = error;
        });
    });
}
void LoadNovels(bool refresh = false) {
    const CatalogKey key{state.ui.username, state.ui.folder.id, state.ui.remote_page};
    if (!refresh) {
        auto found = state.ui.catalog_pages.find(key);
        if (found != state.ui.catalog_pages.end()) {
            const auto& saved = found->second;
            state.ui.novels = saved.items;
            state.ui.novel_pages = saved.pages;
            state.ui.novels_have_next = saved.next;
            state.ui.view = View::Novels;
            state.ui.page = 0;
            state.ui.message.clear();
            Repaint();
            ESP_LOGI("WebNovel", "catalog page cache hit: page=%d", state.ui.remote_page + 1);
            return;
        }
    }
    state.ui.busy = true;
    state.ui.message = Lang::Strings::NOVELIA_LOADING_NOVELS;
    Repaint();
    unsigned generation = ++state.epoch;
    auto id = state.ui.folder.id;
    int requested_page = state.ui.remote_page;
    Submit([generation, id, requested_page, refresh, key](Service& service) {
        const auto policy =
            refresh || service.RefreshNovelsByDefault() ? FetchPolicy::Refresh : FetchPolicy::PreferCache;
        auto data = service.LoadNovels(id, requested_page, kRows, policy);
        Complete(generation, [data = std::move(data), key, refresh] {
            if (refresh && data.error.empty())
                state.ui.catalog_pages.clear();
            if (!data.items.empty() || data.error.empty())
                state.ui.catalog_pages[key] = CatalogPage{data.items, data.pages, data.next, {}};
            state.ui.novels = data.items;
            state.ui.novel_pages = data.pages;
            state.ui.novels_have_next = data.next;
            state.ui.view = View::Novels;
            state.ui.message = data.error;
            state.ui.page = 0;
        });
    });
}
void LoadToc(BootReading boot = {}) {
    state.ui.busy = true;
    state.ui.message = Lang::Strings::NOVELIA_LOADING_TOC;
    Repaint();
    unsigned generation = ++state.epoch;
    auto book = state.ui.novel;
    Submit([generation, book, boot = std::move(boot)](Service& service) {
        const bool restoring = !boot.path.empty();
        const auto user = service.Username();
        if (restoring && user != boot.username) {
            Complete(generation, [] { LoadFolders(); });
            return;
        }
        auto data = service.LoadDirectory(book, restoring ? FetchPolicy::PreferCache : FetchPolicy::Refresh);
        auto& items = data.items;
        const auto& book_title = data.title;
        auto reader_toc = std::make_shared<ReaderToc>();
        reader_toc->title = book_title.empty() ? book.title : book_title;
        for (const auto& item : items)
            reader_toc->entries.push_back({item.title, !item.volume});
        auto last = service.Resume(book, data);
        if (!restoring && service.SyncResumeOnDirectoryLoad() && !last.empty() && last != data.remote)
            for (const auto& item : items)
                if (item.id == last) {
                    service.Shown(book, item);
                    break;
                }
        const char* resume_source = last.empty() ? "first" : last == data.remote ? "cloud" : "local";
        auto mode = service.Translation();
        auto proxy = service.ProxySummary();
        const bool prefetch = service.Prefetch();
        Complete(generation, [items = std::move(items), reader_toc, error = std::move(data.error), book_title, last,
                              resume_source, boot, user, mode, proxy, prefetch] {
            ESP_LOGI("WebNovel", "directory loaded: count=%u resume=%s", (unsigned)items.size(), resume_source);
            state.ui.chapters = items;
            state.ui.reader_toc = reader_toc;
            state.ui.view = View::Toc;
            state.ui.message = error;
            state.ui.title = book_title;
            state.ui.last_chapter = last;
            if (!boot.path.empty()) {
                state.ui.username = user;
                state.ui.mode = mode;
                state.ui.proxy_summary = proxy;
                state.ui.prefetch = prefetch;
                for (int i = 0; i < static_cast<int>(state.ui.chapters.size()); ++i) {
                    if (!state.ui.chapters[i].volume && state.ui.chapters[i].id == boot.chapter) {
                        OpenChapter(i, StartAt::Saved, boot.path);
                        break;
                    }
                }
            }
        });
    });
}
void Refresh() {
    if (state.ui.busy)
        return;
    if (state.ui.view == View::Folders)
        LoadFolders();
    else if (state.ui.view == View::Novels) {
        state.ui.remote_page = 0;
        LoadNovels(true);
    } else if (state.ui.view == View::Toc)
        LoadToc();
}
void OpenChapter(int index, StartAt start, std::string cached_path) {
    if (state.ui.busy || index < 0 || index >= static_cast<int>(state.ui.chapters.size()) ||
        state.ui.chapters[index].volume)
        return;
    state.ui.busy = true;
    state.ui.message = Lang::Strings::NOVELIA_LOADING_CHAPTER;
    if (BookScreen::IsReadScreenActive())
        ScreenShowToast(state.ui.message.c_str());
    Repaint();
    unsigned generation = ++state.epoch;
    auto book = state.ui.novel, chapter = state.ui.chapters[index];
    auto translation = state.ui.mode;
    const auto active_site = state.site.load();
    Submit([generation, book, chapter, translation, index, start, active_site,
            cached_path = std::move(cached_path)](Service& service) {
        auto result = cached_path.empty() ? Download(service, book, chapter, translation)
                                          : Chapter{cached_path, chapter.title};
        if (!cached_path.empty() && access(cached_path.c_str(), R_OK) != 0)
            result.error = Lang::Strings::BOOK_OPEN_FAILED_IO;
        Complete(generation, [result, book, chapter, translation, index, start, active_site] {
            if ((!state.root || lv_screen_active() != state.root) && !BookScreen::IsReadScreenActive())
                return;
            if (!result.error.empty()) {
                state.ui.message = result.error;
                const auto& available = result.available;
                if (!available.empty()) {
                    std::string names;
                    for (const auto& name : available)
                        names += (names.empty() ? "" : ", ") + name;
                    state.ui.message =
                        Format(Lang::Strings::NOVELIA_TRANSLATION_AVAILABLE_FMT, names.c_str());
                }
                if (BookScreen::IsReadScreenActive())
                    ScreenShowToast(state.ui.message.c_str());
                return;
            }
            state.ui.message.clear();
            ReaderLaunch request;
            HostToc host_toc;
            host_toc.toc = state.ui.reader_toc;
            host_toc.index = index;
            host_toc.open = [](int target) {
                if (state.ui.busy)
                    return false;
                OpenChapter(target, StartAt::First);
                return state.ui.busy;
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
            auto context = Object();
            Set(context.get(), "site", SiteInfo(active_site).id);
            Set(context.get(), "id", book.id);
            Set(context.get(), "title", book.title);
            Set(context.get(), "provider", book.provider);
            Set(context.get(), "novel", book.novel);
            Set(context.get(), "chapter", chapter.id);
            Set(context.get(), "username", state.ui.username);
            request.boot_context = Encode(context.get());
            request.suspending = CancelPendingWork;
            request.returning = [] {
                ++state.epoch;
                state.ui.busy = false;
                state.ui.message.clear();
            };
            request.adjacent = [host_toc](int direction) {
                const int next = host_toc.Adjacent(direction);
                if (next < 0) {
                    ScreenShowToast(direction > 0 ? Lang::Strings::NOVELIA_LAST_CHAPTER
                                                  : Lang::Strings::NOVELIA_FIRST_CHAPTER);
                    return true;
                }
                if (!state.ui.busy)
                    OpenChapter(next, direction > 0 ? StartAt::First : StartAt::Last);
                return true;
            };
            request.opened = [book, chapter, translation, index, host_toc] {
                state.ui.last_chapter = chapter.id;
                state.ui.page = index / kRows;
                Submit([book, chapter](Service& service) {
                    if (!service.Shown(book, chapter)) {
                        auto error = service.error;
                        ESP_LOGW("WebNovel", "reading progress: %s", error.c_str());
                        if (error == Lang::Strings::NOVELIA_LOGIN_EXPIRED)
                            Main([error] {
                                if (BookScreen::IsReadScreenActive())
                                    ScreenShowToast(error.c_str());
                            });
                    }
                });
                if (state.ui.prefetch) {
                    const int next = host_toc.Adjacent(1);
                    if (next >= 0) {
                        auto following = state.ui.chapters[next];
                        Submit([book, following, translation](Service& service) {
                            Download(service, book, following, translation);
                        });
                    }
                }
            };
            BookExternalOpen(std::move(request));
        });
    });
}
void Login() {
    if (state.ui.busy)
        return;
    if (state.ui.view != View::Login)
        state.ui.login_return = state.ui.view;
    state.ui.view = View::Login;
    state.ui.busy = true;
    state.ui.message = Lang::Strings::NOVELIA_STARTING_PORTAL;
    Repaint();
    unsigned generation = ++state.epoch;
    const auto active_site = state.site.load();
    Submit([generation, active_site](Service& service) {
        if (!service.Ready()) {
            auto error = service.error;
            Complete(generation, [error] { state.ui.message = error; });
            return;
        }
        PortalStatus(Lang::Strings::NOVELIA_PORTAL_READY, service.PublicConfig());
        auto url = StartPortal([generation, active_site](std::string body) {
            return TrySubmit([generation, body = std::move(body)](Service& service) {
                if (generation != state.epoch.load() || !state.portal_live.load())
                    return;
                auto input = Parse(body);
                auto action = Text(input.get(), "action");
                auto form = ApplyForm(input.get(), service);
                const bool ok = form.ok;
                const auto& status_message = form.message;
                ESP_LOGI("WebNovel", "phone action=%s ok=%d", action.c_str(), ok);
                if (!ok)
                    ESP_LOGW("WebNovel", "operation failed: %s", status_message.c_str());
                PortalStatus(status_message, service.PublicConfig());
                auto user = service.Username();
                auto proxy = service.ProxySummary();
                Main([generation, action, ok, status_message, user, proxy] {
                    if (generation != state.epoch.load())
                        return;
                    state.ui.message = status_message;
                    state.ui.username = user;
                    state.ui.proxy_summary = proxy;
                    if (ok && action == "login") {
                        state.ui.catalog_pages.clear();
                        StopLogin();
                        state.ui.view = View::Folders;
                        state.ui.page = 0;
                        LoadFolders();
                    }
                    Repaint();
                });
            }, active_site);
        });
        if (generation != state.epoch.load()) {
            StopPortal();
            return;
        }
        state.portal_live.store(!url.empty());
        Complete(generation, [url] {
            state.ui.portal_url = url;
            state.portal_deadline = esp_timer_get_time() + kPortalLifetimeUs;
            state.ui.message = url.empty() ? Lang::Strings::NOVELIA_PORTAL_FAILED : "";
        });
    });
}
const std::vector<Item>& Rows() {
    static const std::vector<Item> empty;
    auto rows = CurrentView().rows;
    return rows ? state.ui.*rows : empty;
}
int PageCount(const std::vector<Item>& items) {
    return std::max(1, (int(items.size()) + kRows - 1) / kRows);
}
void Turn(int direction) {
    const auto& view = CurrentView();
    if (state.ui.busy || !view.rows)
        return;
    if (view.remote_paging) {
        if ((direction < 0 && state.ui.remote_page > 0) || (direction > 0 && state.ui.novels_have_next)) {
            state.ui.remote_page += direction;
            LoadNovels();
        }
    } else {
        int pages = PageCount(Rows());
        state.ui.page = std::clamp(state.ui.page + direction, 0, pages - 1);
        Repaint();
    }
}
void OpenSettings() {
    if (!state.ui.busy && state.ui.view != View::Settings) {
        state.ui.settings_return = state.ui.view;
        state.ui.view = View::Settings;
        state.ui.message.clear();
        Repaint();
    }
}
void ContinueReading() {
    if (state.ui.view != View::Toc)
        return;
    for (size_t i = 0; i < state.ui.chapters.size(); ++i) {
        if (!state.ui.last_chapter.empty() && state.ui.chapters[i].id == state.ui.last_chapter) {
            OpenChapter(i);
            return;
        }
    }
    for (size_t i = 0; i < state.ui.chapters.size(); ++i) {
        if (!state.ui.chapters[i].volume) {
            OpenChapter(i);
            return;
        }
    }
}
void SetTranslation(std::string selected) {
    if (state.ui.busy || state.ui.view != View::Toc || selected == state.ui.mode)
        return;
    state.ui.busy = true;
    unsigned generation = ++state.epoch;
    Submit([selected, generation](Service& service) {
        bool ok = service.SetTranslation(selected);
        Complete(generation, [selected, ok] {
            if (ok)
                state.ui.mode = selected;
            else
                state.ui.message = Lang::Strings::NOVELIA_SETTING_SAVE_FAILED;
        });
    });
}
void SetPrefetch(bool enabled) {
    if (state.ui.busy || enabled == state.ui.prefetch)
        return;
    state.ui.busy = true;
    unsigned generation = ++state.epoch;
    Submit([generation, enabled](Service& service) {
        bool ok = service.SetPrefetch(enabled);
        Complete(generation, [enabled, ok] {
            if (ok)
                state.ui.prefetch = enabled;
            else
                state.ui.message = Lang::Strings::NOVELIA_SETTING_SAVE_FAILED;
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
    if (state.ui.busy)
        return;
    state.ui.busy = true;
    state.ui.message.clear();
    Repaint();
    const unsigned generation = ++state.epoch;
    Submit([generation, clear](Service& service) {
        auto usage = clear ? ClearCache(service.Root()) : MeasureCache(service.Root());
        Complete(generation, [usage, clear] {
            if (clear) {
                state.ui.catalog_pages.clear();
                state.ui.cache_summary = usage.ok ? Lang::Strings::NOVELIA_CACHE_CLEARED
                                                  : Lang::Strings::NOVELIA_CACHE_CLEAR_FAILED;
            } else
                state.ui.cache_summary = CacheSummary(usage);
        });
    });
}
void Logout() {
    if (state.ui.busy)
        return;
    state.ui.busy = true;
    unsigned generation = ++state.epoch;
    Submit([generation](Service& service) {
        bool ok = service.Logout();
        Complete(generation, [ok] {
            if (ok) {
                state.ui.username.clear();
                state.ui.folders.clear();
                state.ui.novels.clear();
                state.ui.chapters.clear();
                state.ui.reader_toc.reset();
                state.ui.catalog_pages.clear();
            }
            state.ui.message = ok ? Lang::Strings::NOVELIA_LOGOUT_DONE : Lang::Strings::NOVELIA_LOGOUT_FAILED;
        });
    });
}
void OpenItem(View source, const Item& item, int index) {
    if (state.ui.busy || state.ui.view != source)
        return;
    if (state.ui.view == View::Folders) {
        state.ui.folder_page = state.ui.page;
        state.ui.folder = item;
        state.ui.remote_page = 0;
        state.ui.page = 0;
        LoadNovels();
    } else if (state.ui.view == View::Novels) {
        state.ui.novel = item;
        state.ui.page = 0;
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
        state.ui.page = std::clamp(state.ui.page, 0, pages - 1);
    }
    const auto& site = SiteInfo(state.site.load());
    web_novel::ui::Page data{Rows()};
    data.view = state.ui.view;
    switch (state.ui.view) {
    case View::Folders:
        data.heading = site.app_name;
        data.subtitle = Lang::Strings::NOVELIA_FOLDERS_SUBTITLE;
        break;
    case View::Novels:
        data.heading = Lang::Strings::NOVELIA_NOVELS_HEADING;
        data.subtitle = state.ui.folder.title;
        break;
    case View::Toc:
        data.heading = Lang::Strings::NOVELIA_TOC_HEADING;
        data.subtitle = state.ui.title.empty() ? state.ui.novel.title : state.ui.title;
        break;
    case View::Settings:
        data.heading = Format(Lang::Strings::NOVELIA_SETTINGS_HEADING_FMT, site.app_name);
        break;
    case View::Login:
        data.heading = Lang::Strings::NOVELIA_LOGIN_HEADING;
        data.subtitle = Lang::Strings::NOVELIA_LOGIN_HINT;
        break;
    case View::Count:
        return;
    }
    data.message = state.ui.message;
    data.username = state.ui.username;
    data.proxy_summary = state.ui.proxy_summary;
    data.translation = state.ui.mode;
    data.last_chapter = state.ui.last_chapter;
    data.qr_url = state.ui.portal_url;
    data.busy = state.ui.busy;
    data.prefetch = state.ui.prefetch;
    data.cache_summary = state.ui.cache_summary;
    data.index = view.remote_paging ? state.ui.remote_page : state.ui.page;
    data.count = view.remote_paging ? state.ui.novel_pages : PageCount(Rows());

    web_novel::ui::Actions actions;
    actions.back = Back;
    actions.settings = OpenSettings;
    actions.login = Login;
    actions.refresh = Refresh;
    actions.open = [source = state.ui.view](const Item& item, int index) { OpenItem(source, item, index); };
    actions.continue_reading = ContinueReading;
    if (site.supports_translation)
        actions.set_translation = SetTranslation;
    actions.set_prefetch = SetPrefetch;
    actions.logout = Logout;
    actions.measure_cache = [] { ManageCache(false); };
    actions.clear_cache = [] { ManageCache(true); };
    web_novel::ui::Render(state.content, data, actions);
}
void Initialize() {
    if (state.initialized)
        return;
    state.initialized = true;
    state.paint.paint = Render;
    state.jobs = xQueueCreate(8, sizeof(Work*));
    if (!state.jobs) {
        state.ui.message = Lang::Strings::NOVELIA_QUEUE_CREATE_FAILED;
        return;
    }
    auto created = xTaskCreate(
        [](void*) {
            std::array<std::unique_ptr<Service>, static_cast<size_t>(SiteIndex::Count)> services;
            for (size_t i = 0; i < kSites.size(); ++i) {
                services[i] = kSites[i].make_service();
                services[i]->network_ready = [] { return Board::GetInstance().EnsureNetworkReady(); };
                services[i]->network_present = [] { return Board::GetInstance().IsNetworkReady(); };
            }
            Work* work;
            while (xQueueReceive(state.jobs, &work, portMAX_DELAY) == pdTRUE) {
                {
                    PowerNeedHold power(PowerNeed::ForegroundNetwork);
                    auto& service = *services[static_cast<size_t>(work->site)];
                    unsigned job_epoch = work->generation;
                    service.cancelled = [job_epoch] { return job_epoch != state.epoch.load(); };
                    if (!work->needs_sd)
                        work->run(service); // 关闭 portal 等清理任务，即使请求已过期也必须执行。
                    else {
                        sd_access::Transaction sd;
                        if (sd && service.Init()) {
                            if (!service.cancelled())
                                work->run(service);
                        } else
                            Complete(job_epoch,
                                     [error = sd ? service.error : Lang::Strings::NOVELIA_SD_BUSY] {
                                         state.ui.message = error;
                                     });
                    }
                }
                delete work;
            }
            vTaskDelete(nullptr);
        },
        "web_novel", 16384, nullptr, 2, nullptr);
    if (created != pdPASS) {
        vQueueDelete(state.jobs);
        state.jobs = nullptr;
        state.ui.message = Lang::Strings::NOVELIA_WORKER_FAILED;
        return;
    }
}
lv_obj_t* CreateForSite(SiteIndex site_index, bool restoring = false) {
    const bool first = !state.initialized;
    Initialize();
    const auto& info = SiteInfo(site_index);
    if (state.site.load() != site_index) {
        ++state.epoch;
        StopLogin();
        state.site.store(site_index);
        state.ui = {};
        if (!restoring && state.jobs)
            LoadFolders();
    } else if (!restoring && first && state.jobs) {
        LoadFolders();
    }
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
        [](lv_timer_t* timer) {
            if (lv_timer_get_user_data(timer) != state.root)
                return;
            const bool standby = StandbyScreen::IsActive();
            if ((standby && !state.ui.was_standby) ||
                (state.portal_live.load() && esp_timer_get_time() >= state.portal_deadline)) {
                CancelPendingWork();
            }
            state.ui.was_standby = standby;
        },
        100, screen);
    lv_obj_add_event_cb(
        screen,
        [](lv_event_t* event) {
            if (auto* timer = static_cast<lv_timer_t*>(lv_event_get_user_data(event)))
                lv_timer_delete(timer);
            if (lv_event_get_target(event) == state.root) {
                state.root = nullptr;
                state.content = nullptr;
                if (state.ui.busy || state.portal_live.load())
                    CancelPendingWork();
            }
        },
        LV_EVENT_DELETE, timer);
    lv_obj_add_event_cb(screen, [](lv_event_t*) { Repaint(); }, LV_EVENT_SCREEN_LOADED, nullptr);
    VkKeyScreenDesc keys{};
    keys.factory = info.factory;
    keys.on_key = [](const char* key) {
        if (std::string(key) == "vk_home")
            Back();
        else if (std::string(key) == "vk_prev")
            Turn(-1);
        else if (std::string(key) == "vk_next")
            Turn(1);
        return true;
    };
    VkKey_AttachScreen(screen, info.id, keys);
    return screen;
}
} // namespace

lv_obj_t* WebNovelScreen::CreateNovelia() {
    return CreateForSite(SiteIndex::Novelia);
}
lv_obj_t* WebNovelScreen::CreateNovalpie() {
    return CreateForSite(SiteIndex::Novalpie);
}
lv_obj_t* WebNovelScreen::RestoreReading(const std::string& path, const std::string& context) {
    auto saved = Parse(context);
    const auto* site = FindSiteById(Text(saved.get(), "site"));
    if (!site)
        site = FindSiteByChapterPath(path);
    if (!site)
        return nullptr;
    Item book{Text(saved.get(), "id"), Text(saved.get(), "title"),
              Text(saved.get(), "provider"), Text(saved.get(), "novel")};
    auto chapter = Text(saved.get(), "chapter");
    if (book.id.empty() || chapter.empty())
        return site->factory();
    auto* screen = CreateForSite(site->index, true);
    state.ui.novel = std::move(book);
    state.ui.view = View::Toc;
    if (state.jobs)
        LoadToc({path, std::move(chapter), Text(saved.get(), "username")});
    return screen;
}
