#pragma once

#include "fontpack_lvgl.h"
#include "reader/text_encoding.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <lvgl.h>

/** @brief 检查字体及其回退链是否包含文本所需字形 */
static inline bool ScreenFontHasText(const lv_font_t* font, const char* text) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t remaining = std::strlen(text);
    while (remaining > 0) {
        uint32_t cp = 0;
        const size_t consumed = reader::Utf8Next(bytes, remaining, &cp);
        if (consumed == 0) return false;
        bytes += consumed;
        remaining -= consumed;
        if (cp == '\n' || cp == '\r') continue;
        lv_font_glyph_dsc_t glyph{};
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, 0) || glyph.is_placeholder) return false;
    }
    return true;
}

/** @brief 按可用宽高选择字库字号并完整换行显示文案 */
static inline void ScreenFitLabel(lv_obj_t* label, int32_t width, int32_t height,
                                  const lv_font_t* preferred = nullptr) {
    width = std::max<int32_t>(1, width);
    const lv_font_t* font = preferred ? preferred : fontpack_lv_font_ui();
    if (font == nullptr) font = LV_FONT_DEFAULT;
    const int32_t letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    const int32_t line_space = lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    for (const int size : {0, 25, 18}) {
        if (size != 0) {
            const auto* smaller = fontpack_lv_font_get(size, 2);
            if (smaller == nullptr || smaller->line_height >= font->line_height ||
                !ScreenFontHasText(smaller, lv_label_get_text(label))) continue;
            font = smaller;
        }
        lv_point_t extent;
        lv_text_get_size(&extent, lv_label_get_text(label), font, letter_space, line_space,
                         width, LV_TEXT_FLAG_NONE);
        if (extent.x <= width && extent.y <= height) break;
    }
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

/** @brief 在按钮尺寸确定后适配其文案 */
static inline void ScreenFitButtonLabelOnSize(lv_event_t* event) {
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(event));
    auto* label = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    ScreenFitLabel(label, lv_obj_get_content_width(button), lv_obj_get_content_height(button));
    lv_obj_center(label);
}

/** @brief 测量不换行文本的像素宽度。 */
static inline lv_coord_t ScreenMeasureTextWidth(const lv_font_t* font, const char* text) {
    if (font == nullptr || text == nullptr || text[0] == '\0') return 0;
    lv_point_t size{};
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<lv_coord_t>(size.x);
}

/** @brief 文件名单行中间省略，保留首尾；沿用阅读器列表规则。 */
static inline void ScreenSetFilenameMiddle(lv_obj_t* label, const char* filename, const char* ellipsis = "…") {
    lv_obj_update_layout(label);
    const auto* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int width = lv_obj_get_content_width(label);
    std::string text = filename ? filename : "";
    if (width > 0 && ScreenMeasureTextWidth(font, text.c_str()) > width) {
        size_t middle = text.size() / 2;
        while (middle > 0 && (static_cast<unsigned char>(text[middle]) & 0xC0) == 0x80) --middle;
        std::string head = text.substr(0, middle);
        std::string tail = text.substr(middle);
        while (!head.empty() || !tail.empty()) {
            if (head.size() >= tail.size()) {
                size_t end = head.size() - 1;
                while (end > 0 && (static_cast<unsigned char>(head[end]) & 0xC0) == 0x80) --end;
                head.resize(end);
            } else {
                size_t next = 1;
                while (next < tail.size() && (static_cast<unsigned char>(tail[next]) & 0xC0) == 0x80) ++next;
                tail.erase(0, next);
            }
            text = head + ellipsis + tail;
            if (ScreenMeasureTextWidth(font, text.c_str()) <= width) break;
        }
        if (ScreenMeasureTextWidth(font, text.c_str()) > width) text.clear();
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text.c_str());
}
