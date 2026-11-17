#include "novelia_ui.h"
#include "assets/lang_novelia.h"
#include "fontpack_lvgl.h"
#include "settings_screen/settings_common.h"
#include "haptic_feedback.h"
#include "ui_qrcode.h"
#include "ui_scale.h"
#include "screen_text.h"
#include "font_awesome.h"

LV_FONT_DECLARE(font_awesome_30_1);
#include <algorithm>
#include <utility>

namespace novelia::ui {
namespace {
constexpr lv_coord_t kPad = UiSx(12);
constexpr lv_coord_t kGap = UiSy(8);
constexpr lv_coord_t kHeaderHeight = UiSy(44);
constexpr lv_coord_t kFooterHeight = UiSy(36);
using Action = std::function<void()>;

const lv_font_t* TitleFont() {
    auto* font = fontpack_lv_font_get(30, 4);
    return font ? font : fontpack_lv_font_ui();
}
void Strip(lv_obj_t* object) {
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
}
lv_obj_t* Column(lv_obj_t* parent) {
    auto* object = lv_obj_create(parent);
    Strip(object);
    lv_obj_set_width(object, LV_PCT(100));
    lv_obj_set_height(object, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(object, kGap, 0);
    return object;
}
lv_obj_t* Row(lv_obj_t* parent, lv_coord_t height) {
    auto* object = lv_obj_create(parent);
    Strip(object);
    lv_obj_set_size(object, LV_PCT(100), height);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(object, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(object, UiSx(8), 0);
    return object;
}
lv_obj_t* Text(lv_obj_t* parent, const std::string& text, bool title = false) {
    auto* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, title ? TitleFont() : fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(label, text.c_str());
    return label;
}
void FitLeft(lv_obj_t* label, int width, int height, const lv_font_t* font = nullptr) {
    ScreenFitLabel(label, width, height, font);
    lv_obj_set_height(label, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
}
void ActionClicked(lv_event_t* event) {
    (*static_cast<Action*>(lv_event_get_user_data(event)))();
}
void ActionDeleted(lv_event_t* event) {
    delete static_cast<Action*>(lv_event_get_user_data(event));
}
void Click(lv_obj_t* object, Action action) {
    auto* callback = new Action(std::move(action));
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(object);
    lv_obj_add_event_cb(object, ActionClicked, LV_EVENT_CLICKED, callback);
    lv_obj_add_event_cb(object, ActionDeleted, LV_EVENT_DELETE, callback);
}
lv_obj_t* Link(lv_obj_t* parent, const std::string& text, Action action, bool enabled = true) {
    auto* object = lv_obj_create(parent);
    Strip(object);
    lv_obj_set_size(object, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_pad_hor(object, UiSx(4), 0);
    auto* label = Text(object, text);
    lv_obj_set_style_border_side(label, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(label, UiSx(1), 0);
    lv_obj_set_style_border_color(label, lv_color_black(), 0);
    lv_obj_set_style_pad_bottom(label, UiSy(2), 0);
    lv_obj_center(label);
    Click(object, std::move(action));
    if (!enabled)
        lv_obj_add_state(object, LV_STATE_DISABLED);
    return object;
}
lv_obj_t* Option(lv_obj_t* parent, const std::string& title, Action action, bool selected = false) {
    auto* callback = new Action(std::move(action));
    auto* button = SettingsCreateSelectableOption(parent, title.c_str(), ActionClicked,
                                                  reinterpret_cast<intptr_t>(callback));
    lv_obj_add_event_cb(button, ActionDeleted, LV_EVENT_DELETE, callback);
    SettingsStyleSelectable(button, lv_obj_get_child(button, 0), selected);
    return button;
}
void Header(lv_obj_t* content, const Page& page, const Actions& actions) {
    auto* header = Row(content, kHeaderHeight);
    auto* heading = Text(header, page.heading, true);
    lv_obj_set_style_max_width(heading, LV_PCT(45), 0);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    auto* status = Text(header, page.message);
    auto* small = fontpack_lv_font_get(25, 2);
    lv_obj_set_style_text_font(status, small ? small : fontpack_lv_font_ui(), 0);
    lv_obj_set_width(status, 0);
    lv_obj_set_height(status, small ? small->line_height : fontpack_lv_font_ui()->line_height);
    lv_obj_set_flex_grow(status, 1);
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    const bool secondary = page.view == View::Settings || page.view == View::Login;
    Link(header, secondary ? Lang::Strings::NOVELIA_BACK : Lang::Strings::NOVELIA_SETTINGS, secondary ? actions.back : actions.settings,
         secondary || !page.busy);
    if (!page.subtitle.empty()) {
        auto* subtitle = Text(content, page.subtitle);
        lv_obj_set_width(subtitle, LV_PCT(100));
        FitLeft(subtitle, DISPLAY_CONTENT_W - 2 * kPad, UiSy(44));
    }
}
void Translation(lv_obj_t* content, const Page& page, const Actions& actions) {
    auto* row = Row(content, kSettingsOptionH);
    for (const auto& translation : kTranslations) {
        const std::string mode = translation.id;
        auto* option = Option(
            row, TranslationTitle(translation), [choose = actions.set_translation, mode] { choose(mode); },
            page.translation == mode);
        lv_obj_set_width(option, 0);
        lv_obj_set_flex_grow(option, 1);
        if (page.busy)
            lv_obj_add_state(option, LV_STATE_DISABLED);
    }
}
void ContinueReading(lv_obj_t* content, const Page& page, const Actions& actions) {
    std::string chapter = Lang::Strings::NOVELIA_FROM_FIRST;
    for (const auto& item : page.items)
        if (!item.volume && item.id == page.last_chapter)
            chapter = item.title;
    auto* card = Column(content);
    lv_obj_set_height(card, UiSy(80));
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(card, lv_color_white(), 0);
    lv_obj_set_style_radius(card, UiSx(10), 0);
    lv_obj_set_style_pad_all(card, UiSx(12), 0);
    lv_obj_set_style_pad_row(card, UiSy(4), 0);
    Text(card, page.last_chapter.empty() ? Lang::Strings::NOVELIA_START_READING : Lang::Strings::NOVELIA_CONTINUE_READING);
    auto* label = Text(card, chapter, true);
    const int caption_height = fontpack_lv_font_ui()->line_height;
    const int available = UiSy(80) - 2 * UiSx(12) - UiSy(4) - caption_height;
    FitLeft(label, DISPLAY_CONTENT_W - 2 * kPad - 2 * UiSx(12), available, TitleFont());
    Click(card, actions.continue_reading);
    if (page.busy)
        lv_obj_add_state(card, LV_STATE_DISABLED);
}
void Empty(lv_obj_t* parent, const Page& page, const Actions& actions) {
    auto* box = Column(parent);
    lv_obj_set_height(box, LV_PCT(100));
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (page.busy)
        return;
    const bool login = page.username.empty();
    Text(box, login ? Lang::Strings::NOVELIA_LOGIN_TO_VIEW : Lang::Strings::NOVELIA_NO_ITEMS, true);
    if (login) {
        Text(box, Lang::Strings::NOVELIA_LOGIN_ON_PHONE);
        auto* button = Option(box, Lang::Strings::NOVELIA_SCAN_LOGIN, actions.login, true);
        lv_obj_set_width(button, UiSx(220));
    }
}
void List(lv_obj_t* content, const Page& page, const Actions& actions) {
    auto* frame = Column(content);
    lv_obj_set_height(frame, 0);
    lv_obj_set_flex_grow(frame, 1);
    lv_obj_set_style_pad_all(frame, UiSx(10), 0);
    lv_obj_set_style_pad_row(frame, 0, 0);
    lv_obj_set_style_border_width(frame, UiSx(2), 0);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    lv_obj_set_style_radius(frame, UiSx(10), 0);
    auto* list = Column(frame);
    lv_obj_set_height(list, 0);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_pad_row(list, 0, 0);
    auto* toolbar = Row(frame, kSettingsOptionH);
    lv_obj_set_style_border_side(toolbar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(toolbar, UiSx(1), 0);
    lv_obj_set_style_border_color(toolbar, lv_color_black(), 0);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Link(toolbar, Lang::Strings::NOVELIA_REFRESH, actions.refresh, !page.busy);
    lv_obj_update_layout(content);
    std::vector<std::pair<lv_obj_t*, bool>> titles;
    if (page.items.empty())
        Empty(list, page, actions);
    else {
        const int first = page.view == View::Novels ? 0 : page.index * kRows;
        const int end = std::min<int>(page.items.size(), first + kRows);
        int chapter_number = 0;
        for (int i = 0; i < first; ++i)
            if (!page.items[i].volume)
                ++chapter_number;
        for (int i = first; i < end; ++i) {
            const auto item = page.items[i];
            auto* row = Row(list, 0);
            lv_obj_set_flex_grow(row, 1);
            lv_obj_set_style_pad_hor(row, UiSx(8), 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(row, i + 1 < end ? 1 : 0, 0);
            lv_obj_set_style_border_color(row, lv_color_black(), 0);
            if (page.view == View::Toc && !item.volume) {
                auto* number = Text(row, std::to_string(++chapter_number));
                lv_obj_set_width(number, UiSx(42));
                lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_CENTER, 0);
            }
            auto* title = Text(row, item.title.empty() ? Lang::Strings::NOVELIA_UNNAMED : item.title, item.volume);
            lv_obj_set_width(title, 0);
            lv_obj_set_flex_grow(title, 1);
            titles.emplace_back(title, item.volume);
            if (page.view == View::Toc && !item.volume && item.id == page.last_chapter)
                Text(row, Lang::Strings::NOVELIA_LAST_READ);
            if (!item.volume) {
                Click(row, [open = actions.open, item, i] { open(item, i); });
                if (page.busy)
                    lv_obj_add_state(row, LV_STATE_DISABLED);
            }
        }
        for (int i = end - first; i < kRows; ++i) {
            auto* space = Column(list);
            lv_obj_set_height(space, 0);
            lv_obj_set_flex_grow(space, 1);
        }
    }
    auto* footer = Row(content, kFooterHeight);
    auto text = page.count > 0 ? std::to_string(page.index + 1) + " / " + std::to_string(page.count)
                               : Format(Lang::Strings::NOVELIA_PAGE_FMT, page.index + 1);
    auto* count = Text(footer, text);
    lv_obj_set_width(count, LV_PCT(100));
    lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_update_layout(content);
    for (const auto& [label, volume] : titles) {
        auto* font = volume ? TitleFont() : fontpack_lv_font_ui();
        const int width = lv_obj_get_width(label);
        const int available = lv_obj_get_content_height(lv_obj_get_parent(label));
        ScreenFitLabel(label, width, font->line_height, font);
        lv_point_t extent;
        lv_text_get_size(&extent, lv_label_get_text(label), lv_obj_get_style_text_font(label, LV_PART_MAIN),
                         0, 0, width, LV_TEXT_FLAG_NONE);
        lv_obj_set_height(label, std::min<int>(extent.y, available));
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    }
}
void Settings(lv_obj_t* content, const Page& page, const Actions& actions) {
    auto* account = Column(content);
    Text(account, Lang::Strings::NOVELIA_ACCOUNT_CONNECTION, true);
    for (const auto& text :
         {Format(Lang::Strings::NOVELIA_ACCOUNT_FMT,
                 page.username.empty() ? Lang::Strings::NOVELIA_LOGGED_OUT : page.username.c_str()),
          Format(Lang::Strings::NOVELIA_PROXY_FMT,
                 page.proxy_summary.empty() ? Lang::Strings::NOVELIA_PROXY_DIRECT : page.proxy_summary.c_str())}) {
        auto* label = Text(account, text);
        FitLeft(label, DISPLAY_CONTENT_W - 2 * kPad, fontpack_lv_font_ui()->line_height);
    }
    auto* login = Option(account, Lang::Strings::NOVELIA_PHONE_SETUP, actions.login);
    lv_obj_set_style_pad_left(login, UiSx(38), 0);
    auto* icon = Text(login, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(icon, &font_awesome_30_1, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
    if (page.busy)
        lv_obj_add_state(login, LV_STATE_DISABLED);
    auto* logout = Option(account, Lang::Strings::NOVELIA_LOGOUT, actions.logout);
    if (page.busy || page.username.empty())
        lv_obj_add_state(logout, LV_STATE_DISABLED);
    if (page.username.empty())
        lv_obj_set_style_opa(logout, LV_OPA_40, 0);

    auto* reading = Column(content);
    lv_obj_set_style_pad_top(reading, UiSy(16), 0);
    Text(reading, Lang::Strings::NOVELIA_READING_EXPERIENCE, true);
    Text(reading, Lang::Strings::NOVELIA_PREFETCH);
    auto* choices = Row(reading, kSettingsOptionH);
    for (bool enabled : {true, false}) {
        auto* button = Option(
            choices, enabled ? Lang::Strings::NOVELIA_ON : Lang::Strings::NOVELIA_OFF, [set = actions.set_prefetch, enabled] { set(enabled); },
            page.prefetch == enabled);
        lv_obj_set_width(button, 0);
        lv_obj_set_flex_grow(button, 1);
        if (page.busy)
            lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    Text(reading, Lang::Strings::NOVELIA_PREFETCH_HINT);

    auto* cache = Column(content);
    lv_obj_set_style_pad_top(cache, UiSy(16), 0);
    Text(cache, Lang::Strings::NOVELIA_CACHE_TITLE, true);
    auto* summary = Text(cache, page.cache_summary.empty() ? Lang::Strings::NOVELIA_CACHE_HINT
                                                        : page.cache_summary);
    FitLeft(summary, DISPLAY_CONTENT_W - 2 * kPad, fontpack_lv_font_ui()->line_height);
    auto* buttons = Row(cache, kSettingsOptionH);
    for (const auto& choice : {
             std::pair{Lang::Strings::NOVELIA_CACHE_MEASURE, actions.measure_cache},
             std::pair{Lang::Strings::NOVELIA_CACHE_CLEAR, actions.clear_cache}}) {
        auto* button = Option(buttons, choice.first, choice.second);
        lv_obj_set_width(button, 0);
        lv_obj_set_flex_grow(button, 1);
        if (page.busy)
            lv_obj_add_state(button, LV_STATE_DISABLED);
    }
}
void Login(lv_obj_t* content, const Page& page, const Actions& actions) {
    auto* center = lv_obj_create(content);
    Strip(center);
    lv_obj_set_width(center, LV_PCT(100));
    lv_obj_set_height(center, 0);
    lv_obj_set_flex_grow(center, 1);
    if (!page.qr_url.empty()) {
        const lv_coord_t side = UiSx(240);
        auto* frame = UiCreateQrFrame(center, side);
        lv_obj_center(frame);
        auto* qr = lv_qrcode_create(frame);
        lv_qrcode_set_size(qr, side);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_set_quiet_zone(qr, true);
        lv_obj_center(qr);
        lv_qrcode_update(qr, page.qr_url.data(), page.qr_url.size());
        auto* hint = Text(content, Lang::Strings::NOVELIA_SCAN_SETUP);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    }
    auto* button = Option(content, page.qr_url.empty() ? Lang::Strings::NOVELIA_OPEN_LOGIN : Lang::Strings::NOVELIA_NEW_QR, actions.login);
    if (page.busy)
        lv_obj_add_state(button, LV_STATE_DISABLED);
}
} // namespace
void Render(lv_obj_t* content, const Page& page, const Actions& actions) {
    lv_obj_clean(content);
    lv_obj_set_style_pad_all(content, kPad, 0);
    lv_obj_set_style_pad_row(content, kGap, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    Header(content, page, actions);
    if (page.view == View::Settings)
        Settings(content, page, actions);
    else if (page.view == View::Login)
        Login(content, page, actions);
    else {
        if (page.view == View::Toc) {
            Translation(content, page, actions);
            ContinueReading(content, page, actions);
        }
        List(content, page, actions);
    }
}
} // namespace novelia::ui
