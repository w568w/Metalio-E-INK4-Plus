#pragma once
#include "assets/lang_novelia.h"
#include <cJSON.h>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace novelia {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
inline Json Parse(const std::string& text) {
    return Json(cJSON_ParseWithLength(text.data(), text.size()), cJSON_Delete);
}
inline Json Object() {
    return Json(cJSON_CreateObject(), cJSON_Delete);
}
inline std::string Text(const cJSON* obj, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}
inline std::string Encode(const cJSON* obj) {
    char* value = cJSON_PrintUnformatted(obj);
    if (!value)
        return {};
    std::string result(value);
    cJSON_free(value);
    return result;
}
inline void Set(cJSON* obj, const char* key, const std::string& value) {
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddStringToObject(obj, key, value.c_str());
}
inline std::string Title(const cJSON* obj) {
    auto text = Text(obj, "titleZh");
    if (text.empty())
        text = Text(obj, "titleJp");
    if (text.empty())
        text = Text(obj, "title");
    return text;
}
template <typename... Args>
inline std::string Format(const char* format, Args... args) {
    std::string message(std::snprintf(nullptr, 0, format, args...), '\0');
    std::snprintf(message.data(), message.size() + 1, format, args...);
    return message;
}
struct Item {
    std::string id, title, provider, novel;
    bool volume = false;
};
std::vector<Item> Folders(const cJSON* response, std::string* error = nullptr);
std::vector<Item> Novels(const cJSON* response, std::string* error = nullptr);
std::vector<Item> Toc(const cJSON* response, std::string* error = nullptr);
std::string ResumeChapter(const std::vector<Item>& chapters, const std::string& local,
                          const std::string& remote);
std::string EscapeXml(const std::string& text);
std::string Quote(const std::string& text);
struct Translation {
    const char* id;
    const char* field;
    const char* title; // 品牌名固定；空值表示显示名随语言变化。
};
inline constexpr Translation kTranslations[] = {
    {"sakura", "sakuraParagraphs", "Sakura"},
    {"gpt", "gptParagraphs", "GPT"},
    {"youdao", "youdaoParagraphs", "有道"},
    {"source", "paragraphs", nullptr},
};
inline const char* TranslationTitle(const Translation& translation) {
    return translation.title ? translation.title : Lang::Strings::NOVELIA_TRANSLATION_SOURCE;
}
const char* TranslationField(const std::string& mode);
} // namespace novelia
