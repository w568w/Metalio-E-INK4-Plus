#include "novelia_site.h"
#include "assets/lang_web_novel.h"
#include "sd_file.h"
#include "sd_paths.h"
#include <algorithm>
#include <cerrno>
#include <strings.h>
#include <utility>

namespace web_novel {
namespace {
constexpr const char* kApi = "https://n.novelia.cc/api";
constexpr const char* kAuth = "https://auth.novelia.cc/api/v1/auth/";
std::string ExtractToken(std::string body) {
    auto parsed = Parse(body);
    if (cJSON_IsString(parsed.get()))
        return parsed->valuestring;
    return body;
}
} // namespace

std::vector<Item> NoveliaFolders(const cJSON* response, std::string* error) {
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
std::vector<Item> NoveliaNovels(const cJSON* response, std::string* error) {
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
std::vector<Item> NoveliaToc(const cJSON* response, std::string* error) {
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

std::string NoveliaService::Root() const {
    return SD_APP_ROOT "/novelia";
}
void NoveliaService::OnInit() {
    if (Translation().empty())
        Set(config.get(), "translation", kTranslations[0].id);
}
bool NoveliaService::Login(const std::string& username, const std::string& password) {
    if (username.empty() || password.empty()) {
        error = Lang::Strings::NOVELIA_CREDENTIALS_REQUIRED;
        return false;
    }
    if (!Ready())
        return false;
    auto body = Object();
    Set(body.get(), "username", username);
    Set(body.get(), "password", password);
    Set(body.get(), "app", "n");
    auto dialer = MakeDialer();
    auto response = Request(*dialer, "POST", std::string(kAuth) + "login", Encode(body.get()),
                            {{"Content-Type", "application/json"}});
    if (response.error_code != 0 || response.status != 200) {
        error = response.error_code ? HttpError(response.error_code)
                                    : Format(Lang::Strings::NOVELIA_LOGIN_FAILED_FMT, response.status);
        return false;
    }
    if (cancelled && cancelled()) {
        error = Lang::Strings::NOVELIA_CANCELLED;
        return false;
    }
    auto token = ExtractToken(response.body);
    if (token.empty() || token.find_first_of("\r\n") != std::string::npos) {
        error = Lang::Strings::NOVELIA_LOGIN_RESPONSE_INVALID;
        return false;
    }
    std::string refresh;
    for (const auto& header : response.headers)
        if (strcasecmp(header.first.c_str(), "set-cookie") == 0) {
            constexpr char prefix[] = "refresh-token=";
            constexpr size_t length = sizeof(prefix) - 1;
            if (header.second.rfind(prefix, 0) == 0)
                refresh = header.second.substr(length, header.second.find(';') - length);
        }
    auto old = Json(cJSON_Duplicate(config.get(), true), cJSON_Delete);
    Set(config.get(), "username", username);
    Set(config.get(), "token", token);
    Set(config.get(), "refresh_token", refresh);
    if (!Persist()) {
        config = std::move(old);
        return false;
    }
    return true;
}
bool NoveliaService::LoginForm(const cJSON* input) {
    return Login(Text(input, "username"), Text(input, "password"));
}
bool NoveliaService::TestProxy(const cJSON* input) {
    auto trial = ProxyConfig(input);
    if (!trial || !Ready())
        return false;
    auto dialer = Dialer(trial.get());
    auto response = Request(*dialer, "GET", std::string(kApi) + "/user/favored");
    if (response.error_code != 0 || response.status < 200 || response.status >= 500) {
        error = ApiError(response);
        return false;
    }
    // 鉴权拒绝也说明代理及目标 HTTPS 已连通，测试无需登录态。
    return true;
}
proxy_transport::Response NoveliaService::Api(const std::string& method, const std::string& path,
                                              const std::string& body, bool wait_for_network) {
    proxy_transport::Response result;
    result.error_code = NetworkStatus(wait_for_network);
    if (result.error_code)
        return result;
    auto dialer = MakeDialer();
    auto token = Text(config.get(), "token");
    std::vector<std::pair<std::string, std::string>> headers{{"Authorization", "Bearer " + token}};
    // 阅读历史接口用 call.receive<String>() 读取章节 ID，网页端 ky 也发送 text/plain。
    if (!body.empty())
        headers.emplace_back("Content-Type", "text/plain;charset=UTF-8");
    const std::string url = std::string(kApi) + path;
    auto response = Request(*dialer, method, url, body, headers);
    if (cancelled && cancelled()) {
        response.error_code = -ECANCELED;
        return response;
    }
    if (response.status == 401 && !Text(config.get(), "refresh_token").empty()) {
        auto renewed = Request(*dialer, "POST", std::string(kAuth) + "refresh?app=n", {},
                               {{"Cookie", "refresh-token=" + Text(config.get(), "refresh_token")}});
        if (cancelled && cancelled()) {
            response.error_code = -ECANCELED;
            return response;
        }
        if (renewed.status == 200 && renewed.error_code == 0) {
            auto refreshed = ExtractToken(renewed.body);
            if (!refreshed.empty() && refreshed.find_first_of("\r\n") == std::string::npos) {
                Set(config.get(), "token", refreshed);
                Write(Root() + "/config.json", Encode(config.get()));
                headers.front().second = "Bearer " + refreshed;
                response = Request(*dialer, method, url, body, headers);
            }
        }
    }
    return response;
}
CatalogPage NoveliaService::LoadFolders() {
    if (Username().empty())
        return {};
    auto data = Fetch("/user/favored", "folders");
    auto items = NoveliaFolders(data.json.get(), data.json ? &data.error : nullptr);
    return {std::move(items), 0, false, std::move(data.error)};
}
CatalogPage NoveliaService::LoadNovels(const std::string& folder, int page, int page_size, FetchPolicy policy) {
    const auto path = "/user/favored-web/" + Quote(folder) + "?page=" + std::to_string(page) +
                      "&pageSize=" + std::to_string(page_size) +
                      "&query=&provider=kakuyomu,syosetu,novelup,hameln,pixiv,alphapolis&type=0&level=0&"
                      "translate=0&sort=update";
    auto data = Fetch(path, path, policy);
    auto items = NoveliaNovels(data.json.get(), data.json ? &data.error : nullptr);
    const auto* count = cJSON_GetObjectItemCaseSensitive(data.json.get(), "pageNumber");
    const int pages = cJSON_IsNumber(count) ? std::max(1, count->valueint) : 0;
    const bool next = pages ? page + 1 < pages : items.size() == static_cast<size_t>(page_size);
    return {std::move(items), pages, next, std::move(data.error)};
}
Directory NoveliaService::LoadDirectory(const Item& book, FetchPolicy policy) {
    auto data = Fetch("/novel/" + Quote(book.provider) + "/" + Quote(book.novel), book.id, policy);
    auto items = NoveliaToc(data.json.get(), data.json ? &data.error : nullptr);
    return {std::move(items), Title(data.json.get()), Text(data.json.get(), "lastReadChapterId"),
            std::move(data.error)};
}
bool NoveliaService::Shown(const Item& novel, const Item& chapter) {
    error.clear();
    auto value = Object();
    Set(value.get(), "chapter", chapter.id);
    if (!Save(ProgressPath(novel), Encode(value.get())))
        return false;
    if (Text(config.get(), "token").empty())
        return true;
    auto response =
        Api("PUT", "/user/read-history/" + Quote(novel.provider) + "/" + Quote(novel.novel), chapter.id, false);
    if (response.error_code == -ENETDOWN)
        return true; // 后台同步仅使用现有连接，离线时保留本地进度。
    if (response.error_code != 0 || response.status < 200 || response.status >= 300) {
        error = ApiError(response);
        return false;
    }
    return true;
}
bool NoveliaService::PrepareChapter(const Item& novel, const Item& chapter, const std::string& mode,
                                    std::string& cache_root, std::string& epub_filename,
                                    std::string& out_error) const {
    if (!TranslationField(mode)) {
        out_error = Lang::Strings::NOVELIA_TRANSLATION_UNSUPPORTED;
        return false;
    }
    cache_root = Root() + "/chapters/" + sd_file::Key(novel.id + "/" + chapter.id);
    epub_filename = mode + ".epub";
    return true;
}
bool NoveliaService::WriteChapterBody(const Item& novel, const Item& chapter, const std::string& mode,
                                      const ChapterWriter& writer,
                                      std::vector<std::string>& available_translations) {
    const char* field = TranslationField(mode);
    if (!field) {
        if (writer.error)
            *writer.error = Lang::Strings::NOVELIA_TRANSLATION_UNSUPPORTED;
        return false;
    }
    const auto chapter_key = novel.id + "/" + chapter.id;
    auto fetched = Fetch("/novel/" + Quote(novel.provider) + "/" + Quote(novel.novel) + "/chapter/" + Quote(chapter.id),
                         chapter_key);
    auto& payload = fetched.json;
    if (!payload) {
        if (writer.error)
            *writer.error = fetched.error;
        return false;
    }
    for (const auto& translation : kTranslations) {
        auto* paragraphs = cJSON_GetObjectItemCaseSensitive(payload.get(), translation.field);
        if (cJSON_GetArraySize(paragraphs))
            available_translations.push_back(translation.id);
    }
    const auto* paragraphs = cJSON_GetObjectItemCaseSensitive(payload.get(), field);
    if (!cJSON_IsArray(paragraphs) || !cJSON_GetArraySize(paragraphs)) {
        if (writer.error)
            *writer.error = Lang::Strings::NOVELIA_TRANSLATION_UNAVAILABLE;
        return false;
    }
    const cJSON* paragraph;
    const std::string marker = "<图片>";
    cJSON_ArrayForEach(paragraph, paragraphs) {
        if (writer.cancelled())
            return false;
        if (!cJSON_IsString(paragraph))
            continue;
        const std::string text = paragraph->valuestring;
        const bool ok = text.rfind(marker, 0) == 0 ? writer.embed_image(text.substr(marker.size()))
                                                   : writer.append_xml("<p>" + EscapeXml(text) + "</p>");
        if (!ok)
            return false;
    }
    return true;
}
std::string NoveliaService::AuthorsFromDetail(const cJSON* detail) const {
    std::string authors;
    const cJSON* author;
    cJSON_ArrayForEach(author, cJSON_GetObjectItemCaseSensitive(detail, "authors")) {
        auto name = Text(author, "name");
        if (!name.empty()) {
            if (!authors.empty())
                authors += "、";
            authors += name;
        }
    }
    return authors;
}
} // namespace web_novel
