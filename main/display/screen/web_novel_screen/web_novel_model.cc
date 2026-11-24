#include "web_novel_model.h"
namespace web_novel {
std::string ResumeChapter(const std::vector<Item>& chapters, const std::string& local,
                          const std::string& remote) {
    // 优先项失效或缺失时回退到次选项。
    for (const auto& id : {local, remote}) {
        if (id.empty())
            continue;
        for (const auto& chapter : chapters)
            if (chapter.id == id)
                return id;
    }
    return {};
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
} // namespace web_novel
