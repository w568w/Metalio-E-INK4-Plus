#include "novelia_model.h"
#include "assets/lang_novelia.h"
namespace novelia {
std::vector<Item> Folders(const cJSON* response, std::string* error) {
    const auto* array = cJSON_GetObjectItemCaseSensitive(response, "favoredWeb");
    std::vector<Item> result;
    if (!cJSON_IsArray(array)) {
        if (error)
            *error = Lang::Strings::NOVELIA_FAVORITES_INVALID;
        return result;
    }
    const cJSON* item;
    cJSON_ArrayForEach(item, array) {
        auto id = Text(item, "id");
        auto name = Text(item, "title");
        if (id.empty()) {
            if (error)
                *error = Lang::Strings::NOVELIA_FOLDER_ID_MISSING;
            return {};
        }
        const auto title = name.empty() ? Lang::Strings::NOVELIA_FOLDER_UNNAMED : name.c_str();
        result.push_back({id, title, {}, {}, false});
    }
    return result;
}
std::vector<Item> Novels(const cJSON* response, std::string* error) {
    const auto* array = cJSON_GetObjectItemCaseSensitive(response, "items");
    std::vector<Item> result;
    if (!cJSON_IsArray(array)) {
        if (error)
            *error = Lang::Strings::NOVELIA_NOVELS_INVALID;
        return result;
    }
    const cJSON* item;
    cJSON_ArrayForEach(item, array) {
        auto provider = Text(item, "providerId"), id = Text(item, "novelId");
        if (provider.empty() || id.empty()) {
            if (error)
                *error = Lang::Strings::NOVELIA_NOVEL_ID_MISSING;
            return {};
        }
        result.push_back({provider + "/" + id, Title(item), provider, id, false});
    }
    return result;
}
std::vector<Item> Toc(const cJSON* response, std::string* error) {
    std::vector<Item> result;
    const auto* array = cJSON_GetObjectItemCaseSensitive(response, "toc");
    if (!cJSON_IsArray(array)) {
        if (error)
            *error = Lang::Strings::NOVELIA_TOC_INVALID;
        return result;
    }
    const cJSON* item;
    cJSON_ArrayForEach(item, array) {
        auto* id = cJSON_GetObjectItemCaseSensitive(item, "chapterId");
        std::string value;
        if (cJSON_IsString(id))
            value = id->valuestring;
        else if (cJSON_IsNumber(id))
            value = std::to_string(static_cast<int64_t>(id->valuedouble));
        result.push_back({value, Title(item), {}, {}, value.empty()});
    }
    return result;
}
std::string Quote(const std::string& text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~')
            result += c;
        else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}
std::string EscapeXml(const std::string& text) {
    std::string result;
    for (char c : text) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '\r':
            break;
        case '\n':
            result += "<br/>";
            break;
        default:
            if (static_cast<unsigned char>(c) >= 32 || c == '\t')
                result += c;
        }
    }
    return result;
}
const char* TranslationField(const std::string& mode) {
    for (const auto& translation : kTranslations)
        if (mode == translation.id)
            return translation.field;
    return nullptr;
}
} // namespace novelia
