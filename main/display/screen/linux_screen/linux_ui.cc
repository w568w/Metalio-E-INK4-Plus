#include "linux_ui.h"
#include "linux_console.h"
#include "assets/lang_linux.h"
#include "fontpack_lvgl.h"
#include "screen_keyboard.h"
#include "screen_text.h"
#include "settings_screen/settings_common.h"
#include "haptic_feedback.h"
#include "ui_scale.h"
#include <algorithm>
#include <cassert>
#include <utility>

namespace linux_ui {
namespace {
const lv_font_t* TitleFont()
{
    auto* font = fontpack_lv_font_get(30, 4);
    return font ? font : fontpack_lv_font_ui();
}
const lv_font_t* BodyFont()
{
    auto* font = fontpack_lv_font_get(25, 2);
    return font ? font : fontpack_lv_font_ui();
}
void Strip(lv_obj_t* object)
{
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}
lv_obj_t* Text(lv_obj_t* parent, const char* text, bool title = false)
{
    auto* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, title ? TitleFont() : BodyFont(), 0);
    lv_label_set_text(label, text);
    return label;
}
void Click(lv_obj_t* object, std::function<void()> action)
{
    auto* callback = new std::function<void()>(std::move(action));
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(object);
    lv_obj_add_event_cb(object, [](lv_event_t* e) {
        (*static_cast<std::function<void()>*>(lv_event_get_user_data(e)))();
    }, LV_EVENT_CLICKED, callback);
    lv_obj_add_event_cb(object, [](lv_event_t* e) {
        delete static_cast<std::function<void()>*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, callback);
}
lv_obj_t* Row(lv_obj_t* parent, int height)
{
    auto* row = lv_obj_create(parent);
    Strip(row);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UiSx(8), 0);
    return row;
}
void Link(lv_obj_t* parent, const char* text, std::function<void()> action)
{
    auto* link = lv_obj_create(parent);
    Strip(link);
    lv_obj_set_size(link, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_pad_hor(link, UiSx(4), 0);
    auto* label = Text(link, text);
    lv_obj_set_style_border_side(label, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(label, UiSx(1), 0);
    lv_obj_set_style_border_color(label, lv_color_black(), 0);
    lv_obj_center(label);
    Click(link, std::move(action));
}
void Base(View& view, lv_obj_t* parent, const Actions& actions, bool terminal)
{
    view.actions = actions;
    view.root = lv_obj_create(parent);
    Strip(view.root);
    lv_obj_set_size(view.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_hor(view.root, UiSx(12), 0);
    lv_obj_set_style_pad_ver(view.root, UiSy(12), 0);
    lv_obj_set_style_pad_row(view.root, UiSy(8), 0);
    lv_obj_set_flex_flow(view.root, LV_FLEX_FLOW_COLUMN);
    auto* header = Row(view.root, UiSy(44));
    Text(header, Lang::Strings::LINUX_TITLE, true);
    view.status = Text(header, "");
    lv_obj_set_width(view.status, 0);
    lv_obj_set_flex_grow(view.status, 1);
    lv_obj_set_height(view.status, UiSy(24));
    lv_label_set_long_mode(view.status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view.status, LV_TEXT_ALIGN_LEFT, 0);
    if (terminal) {
        Link(header, Lang::Strings::LINUX_IMAGE_LINK, actions.choose);
        Link(header, Lang::Strings::LINUX_INTERRUPT, actions.interrupt);
    }
}
void OnRow(lv_event_t* event)
{
    auto* button = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* view = static_cast<ListView*>(lv_obj_get_user_data(button));
    size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    auto select = view->actions.select; // 选中行后可能销毁当前列表。
    select(index);
}
void Gesture(lv_event_t* event)
{
    auto* view = static_cast<View*>(lv_event_get_user_data(event));
    auto* indev = lv_indev_active();
    if (!indev) return;
    auto direction = lv_indev_get_gesture_dir(indev);
    if (direction != LV_DIR_TOP && direction != LV_DIR_BOTTOM) return;
    lv_event_stop_bubbling(event);
    lv_indev_wait_release(indev);
    view->actions.turn(direction == LV_DIR_TOP ? 1 : -1);
}
void Keyboard(ScreenKeyboardEvent event, const char* text, void* context)
{
    auto& view = *static_cast<TerminalView*>(context);
    if (event == ScreenKeyboardEvent::kInsert) {
        std::string input = lv_textarea_get_text(view.input);
        if (input.size() + std::char_traits<char>::length(text) <= linux_vm::kInputBytes)
            lv_textarea_add_text(view.input, text);
    } else if (event == ScreenKeyboardEvent::kBackspace) lv_textarea_delete_char(view.input);
    else if (event == ScreenKeyboardEvent::kClear) lv_textarea_set_text(view.input, "");
    else if (event == ScreenKeyboardEvent::kConfirm && view.actions.send(std::string(lv_textarea_get_text(view.input)) + "\n"))
        lv_textarea_set_text(view.input, "");
}
}
View::~View() { if (root) lv_obj_delete(root); }
size_t TerminalView::Rows() const
{
    const auto* font = lv_obj_get_style_text_font(terminal, LV_PART_MAIN);
    int line = font->line_height + lv_obj_get_style_text_line_space(terminal, LV_PART_MAIN);
    return std::max<int>(1, lv_obj_get_content_height(terminal) / std::max(1, line));
}
linux_vm::Console::Layout TerminalView::TextLayout() const
{
    const auto* font = lv_obj_get_style_text_font(terminal, LV_PART_MAIN);
    linux_vm::Console::Layout layout{};
    layout.width = lv_obj_get_content_width(terminal);
    int spacing = lv_obj_get_style_text_letter_space(terminal, LV_PART_MAIN);
    lv_font_glyph_dsc_t fallback{};
    lv_font_get_glyph_dsc(font, &fallback, '?', 0);
    for (uint32_t code = 0; code < layout.advance.size(); ++code) {
        lv_font_glyph_dsc_t glyph{};
        int advance = lv_font_get_glyph_dsc(font, &glyph, code, 0) ? glyph.adv_w : fallback.adv_w;
        layout.advance[code] = std::max(1, advance + spacing);
    }
    return layout;
}
void View::Status(const std::string& message)
{
    if (message == lv_label_get_text(status)) return;
    lv_label_set_text(status, message.c_str());
}
std::unique_ptr<TerminalView> Terminal(lv_obj_t* parent, const Actions& actions)
{
    auto view = std::make_unique<TerminalView>();
    Base(*view, parent, actions, true);
    // 键盘和输入行锚定页底，终端高度由剩余空间计算；日志增长不参与布局。
    view->keyboard = ScreenKeyboardCreate(view->root, Keyboard, view.get());
    lv_obj_add_flag(view->keyboard, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(view->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    ScreenKeyboardReset(view->keyboard);
    view->input = lv_textarea_create(view->root);
    lv_obj_add_flag(view->input, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(view->input, LV_PCT(100), UiSy(48));
    lv_textarea_set_one_line(view->input, true);
    lv_textarea_set_max_length(view->input, linux_vm::kInputBytes);
    lv_textarea_set_text(view->input, "");
    lv_obj_set_style_bg_color(view->input, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(view->input, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->input, UiSx(2), 0);
    lv_obj_set_style_border_color(view->input, lv_color_black(), 0);
    lv_obj_set_style_radius(view->input, UiSx(8), 0);
    lv_obj_set_style_pad_hor(view->input, UiSx(12), 0);
    lv_obj_set_style_pad_ver(view->input, UiSy(8), 0);
    // LVGL 9 无隐藏光标接口：关闭该部件绘制和闪烁动画。
    lv_obj_set_style_opa(view->input, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(view->input, 0, LV_PART_CURSOR);
    HapticAttachClick(view->input);
    // 先确定页底键盘，再由输入行的实际坐标划出终端的可用高度。
    lv_obj_update_layout(view->root);
    lv_obj_align_to(view->input, view->keyboard, LV_ALIGN_OUT_TOP_MID, 0, -UiSy(8));
    view->terminal = Text(view->root, "");
    lv_obj_set_style_text_font(view->terminal, &lv_font_montserrat_14, 0);
    lv_obj_set_width(view->terminal, LV_PCT(100));
    lv_label_set_long_mode(view->terminal, LV_LABEL_LONG_CLIP);
    lv_obj_update_layout(view->root);
    int height = lv_obj_get_y(view->input) - lv_obj_get_y(view->terminal) - UiSy(8);
    lv_obj_set_height(view->terminal, std::max<int>(height, lv_font_montserrat_14.line_height));
    lv_obj_add_flag(view->terminal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(view->terminal, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(view->terminal, Gesture, LV_EVENT_GESTURE, view.get());
    return view;
}
std::unique_ptr<ListView> List(lv_obj_t* parent, const Actions& actions, const char* notice)
{
    auto view = std::make_unique<ListView>();
    Base(*view, parent, actions, false);
    view->Status(Lang::Strings::LINUX_LIST_LOADING);
    // 启动错误在创建列表时确定，容量只为实际显示的提示留空间。
    if (*notice) {
        view->notice = Text(view->root, notice);
        lv_obj_set_width(view->notice, LV_PCT(100));
        lv_label_set_long_mode(view->notice, LV_LABEL_LONG_WRAP);
    }
    view->list = lv_obj_create(view->root);
    Strip(view->list);
    lv_obj_set_width(view->list, LV_PCT(100));
    lv_obj_set_flex_grow(view->list, 1);
    lv_obj_add_flag(view->list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(view->list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(view->list, Gesture, LV_EVENT_GESTURE, static_cast<View*>(view.get()));
    lv_obj_set_flex_flow(view->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(view->list, UiSy(8), 0);
    view->number = Text(view->root, "");
    lv_obj_set_size(view->number, LV_PCT(100), UiSy(36));
    lv_obj_set_style_text_align(view->number, LV_TEXT_ALIGN_CENTER, 0);
    auto* sample = SettingsCreateSelectableOption(view->list, "", OnRow, 0);
    // 临时填满剩余空间来量容量，随后恢复内容高度，让页号紧跟实际列表。
    // 量的是按钮固定高度，与标签字体和字号缩放无关，所以样例行沿用默认行为。
    lv_obj_update_layout(view->root);
    int gap = lv_obj_get_style_pad_row(view->list, LV_PART_MAIN);
    view->capacity = std::max<int>(1, (lv_obj_get_content_height(view->list) + gap) / (lv_obj_get_height(sample) + gap));
    lv_obj_delete(sample);
    lv_obj_set_flex_grow(view->list, 0);
    lv_obj_set_height(view->list, LV_SIZE_CONTENT);
    return view;
}
void ListView::Items(const std::vector<std::string>& names, int page, int pages, bool busy, const std::string& selected, const std::string& empty_text)
{
    assert(names.size() <= static_cast<size_t>(capacity));
    Status(busy ? Lang::Strings::LINUX_LIST_LOADING : Lang::Strings::LINUX_CHOOSE);
    lv_obj_clean(list);
    if (names.empty()) {
        auto* empty = Text(list, !empty_text.empty() ? empty_text.c_str() : busy ? Lang::Strings::LINUX_LIST_LOADING : Lang::Strings::LINUX_NO_IMAGES);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    }
    const auto* font = BodyFont();
    const char* ellipsis = ScreenFontHasText(font, "…") ? "…" : "...";
    for (size_t i = 0; i < names.size(); ++i) {
        auto* button = SettingsCreateSelectableOption(list, "", OnRow, i, false);
        lv_obj_set_user_data(button, this);
        auto* label = lv_obj_get_child(button, 0);
        lv_obj_set_style_bg_color(button, lv_color_black(), LV_STATE_PRESSED);
        lv_obj_update_layout(button);
        lv_obj_set_width(label, lv_obj_get_content_width(button));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_font(label, font, 0);
        ScreenSetFilenameMiddle(label, names[i].c_str(), ellipsis);
        SettingsStyleSelectable(button, label, names[i] == selected);
        // 让文字继承按钮状态色；固定的局部颜色会遮住按下态白字。
        lv_obj_remove_local_style_prop(label, LV_STYLE_TEXT_COLOR, LV_PART_MAIN);
        lv_obj_set_style_text_color(button, names[i] == selected ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_text_color(button, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        if (busy) lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    if (names.empty()) lv_obj_add_flag(number, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_clear_flag(number, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(number, "%d / %d", page + 1, pages);
    }
}
}
