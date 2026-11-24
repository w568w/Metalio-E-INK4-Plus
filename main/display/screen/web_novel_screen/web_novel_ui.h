#pragma once
#include "web_novel_model.h"
#include <lvgl.h>
#include <functional>

namespace web_novel::ui {
enum class View { Folders, Novels, Toc, Settings, Login, Count };
constexpr int kRows = 8;

struct Page {
    const std::vector<Item>& items;
    View view = View::Folders;
    std::string heading, subtitle, message, username, proxy_summary, translation, last_chapter, qr_url;
    std::string cache_summary;
    bool busy = false;
    bool prefetch = true;
    int index = 0;
    int count = 1; // 仅 Novels 服务端未给总页数时为 0
};
struct Actions {
    std::function<void()> back, settings, login, logout, refresh, continue_reading;
    std::function<void()> measure_cache, clear_cache;
    std::function<void(bool)> set_prefetch;
    std::function<void(std::string)> set_translation;
    std::function<void(const Item&, int)> open;
};
// The caller owns the screen and status bar; only this content container is rebuilt.
void Render(lv_obj_t* content, const Page& page, const Actions& actions);
} // namespace web_novel::ui
