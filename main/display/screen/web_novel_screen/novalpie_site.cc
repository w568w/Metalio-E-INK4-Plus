#include "novalpie_site.h"
#include "novalpie_chapter.h"
#include "assets/lang_web_novel.h"
#include "reader/html_content.h"
#include "sd_file.h"
#include "sd_paths.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <psa/crypto.h>
#include <string_view>
#include <utility>

namespace web_novel {
namespace {
constexpr const char* kApi = "https://novalpie.cc/api";
constexpr const char* kHcaptchaSitekey = "c33cd62c-47d4-4c00-a4f6-9a4e19acabd2";
constexpr const char* kSessionImportHint =
    "在电脑打开 Novalpie 的开发者工具，进入「应用 → 本地存储 → https://novalpie.cc」，复制 auth_token 的值。";

const cJSON* Field(const cJSON* object, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}
bool IsValidNumericId(const std::string& value) {
    return !value.empty() && (value == "0" || value.front() != '0') &&
           value.find_first_not_of("0123456789") == std::string::npos;
}
// cJSON stores numbers as double. Accept only IDs it can represent exactly.
std::string Id(const cJSON* object, const char* key) {
    const auto* id = Field(object, key);
    if (cJSON_IsString(id)) {
        std::string value = id->valuestring;
        if (IsValidNumericId(value))
            return value;
    } else if (cJSON_IsNumber(id) && id->valuedouble >= 0 &&
               id->valuedouble <= 9007199254740991.0 && std::floor(id->valuedouble) == id->valuedouble) {
        return std::to_string(static_cast<uint64_t>(id->valuedouble));
    }
    return {};
}
} // namespace

std::string NovalpieService::Root() const {
    return SD_APP_ROOT "/novalpie";
}
void NovalpieService::AppendPortalConfig(cJSON* visible) const {
    Set(visible, "hcaptcha_sitekey", kHcaptchaSitekey);
    cJSON_AddBoolToObject(visible, "allow_session_import", true);
    Set(visible, "session_import_hint", kSessionImportHint);
}
bool NovalpieService::Login(const std::string& username, const std::string& password, const std::string& captcha) {
    if (username.empty() || password.empty()) {
        error = Lang::Strings::NOVELIA_CREDENTIALS_REQUIRED;
        return false;
    }
    if (captcha.empty()) {
        error = Lang::Strings::NOVALPIE_CAPTCHA_REQUIRED;
        return false;
    }
    if (!Ready())
        return false;
    auto body = Object();
    Set(body.get(), "username", username);
    Set(body.get(), "password", password);
    // 官网的 hCaptcha、reCAPTCHA 和 Turnstile 都提交到同一字段。
    Set(body.get(), "turnstile_token", captcha);
    auto dialer = MakeDialer();
    auto response = proxy_transport::Request(*dialer, "POST", std::string(kApi) + "/sessions", Encode(body.get()),
                                             {{"Content-Type", "application/json"}});
    auto reply = Parse(response.body);
    if (response.error_code || response.status != 200 || !cJSON_IsTrue(Field(reply.get(), "success"))) {
        error = response.error_code ? ApiError(response) : Text(reply.get(), "message");
        if (error.empty())
            error = response.status == 403 && !reply ? Lang::Strings::NOVALPIE_ROUTE_BLOCKED
                                                     : Format(Lang::Strings::NOVELIA_LOGIN_FAILED_FMT, response.status);
        return false;
    }
    return SaveSession(Text(reply.get(), "token"), Text(Field(reply.get(), "user"), "username"));
}
bool NovalpieService::ImportSession(const std::string& token) {
    if (token.empty() ||
        token.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") !=
            std::string::npos ||
        !Ready())
        return false;
    auto dialer = MakeDialer();
    auto response = proxy_transport::Request(*dialer, "GET", std::string(kApi) + "/users/me", {},
                                             {{"Authorization", "Bearer " + token}});
    auto user = Parse(response.body);
    auto name = Text(user.get(), "username");
    if (response.error_code || response.status != 200 || name.empty()) {
        error = response.error_code || response.status != 200 ? ApiError(response)
                                                              : Lang::Strings::NOVELIA_LOGIN_RESPONSE_INVALID;
        return false;
    }
    return SaveSession(token, name);
}
bool NovalpieService::LoginForm(const cJSON* input) {
    const auto token = Text(input, "token");
    if (!token.empty())
        return ImportSession(token);
    return Login(Text(input, "username"), Text(input, "password"), Text(input, "captcha"));
}
bool NovalpieService::Logout() {
    reader_session = {};
    if (!Text(config.get(), "token").empty())
        Api("DELETE", "/sessions", {}, false);
    return Service::Logout();
}
bool NovalpieService::TestProxy(const cJSON* input) {
    auto trial = ProxyConfig(input);
    if (!trial || !Ready())
        return false;
    auto dialer = Dialer(trial.get());
    auto response = Request(*dialer, "GET", std::string(kApi) + "/sessions/current");
    if (response.error_code || response.status != 200) {
        error = !response.error_code && response.status == 403 ? Lang::Strings::NOVALPIE_ROUTE_BLOCKED
                                                               : ApiError(response);
        return false;
    }
    auto result = Parse(response.body);
    if (!cJSON_IsTrue(Field(result.get(), "success"))) {
        error = Lang::Strings::NOVELIA_HTTP_PROTOCOL_ERROR;
        return false;
    }
    return true;
}
proxy_transport::Response NovalpieService::Api(const std::string& method, const std::string& path,
                                               const std::string& body, bool wait_for_network) {
    proxy_transport::Response result;
    result.error_code = NetworkStatus(wait_for_network);
    if (result.error_code)
        return result;
    auto dialer = MakeDialer();
    auto token = Text(config.get(), "token");
    std::vector<std::pair<std::string, std::string>> headers;
    if (!token.empty())
        headers.emplace_back("Authorization", "Bearer " + token);
    if (!body.empty())
        headers.emplace_back("Content-Type", "application/json");
    if (path == "/reader/session-key") {
        uint8_t random[4];
        if (psa_crypto_init() != PSA_SUCCESS || psa_generate_random(random, sizeof(random)) != PSA_SUCCESS) {
            result.error_code = -EIO;
            return result;
        }
        char nonce[9];
        std::snprintf(nonce, sizeof(nonce), "%02x%02x%02x%02x", unsigned(random[0]), unsigned(random[1]),
                      unsigned(random[2]), unsigned(random[3]));
        auto timestamp = std::to_string(std::time(nullptr));
        auto signature = SignNovalpieSession(timestamp, nonce);
        if (signature.empty()) {
            result.error_code = -EIO;
            return result;
        }
        headers.emplace_back("User-Agent", kNovalpieUserAgent);
        headers.emplace_back("X-Client-Signature", std::move(signature));
        headers.emplace_back("X-Client-Timestamp", std::move(timestamp));
        headers.emplace_back("X-Client-Nonce", nonce);
    }
    auto response = Request(*dialer, method, std::string(kApi) + path, body, headers);
    if (cancelled && cancelled())
        response.error_code = -ECANCELED;
    return response;
}
bool NovalpieService::SaveSession(const std::string& token, const std::string& name) {
    if (token.empty() || name.empty() ||
        token.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") !=
            std::string::npos) {
        error = Lang::Strings::NOVELIA_LOGIN_RESPONSE_INVALID;
        return false;
    }
    if (cancelled && cancelled())
        return false;
    auto old = Json(cJSON_Duplicate(config.get(), true), cJSON_Delete);
    Set(config.get(), "token", token);
    Set(config.get(), "username", name);
    if (Persist()) {
        reader_session = {};
        return true;
    }
    config = std::move(old);
    return false;
}
CatalogPage NovalpieService::Bookshelf(const std::string* folder, int page, int page_size, FetchPolicy policy) {
    auto data = Fetch("/v2/users/me/bookshelf", "bookshelf", policy);
    CatalogPage result;
    result.error = std::move(data.error);
    const auto* shelf = Field(Field(data.json.get(), "data"), "bookshelf");
    if (!cJSON_IsArray(shelf)) {
        if (data.json)
            result.error = Lang::Strings::NOVELIA_FAVORITES_INVALID;
        return result;
    }
    if (!folder)
        result.items.push_back({"0", Lang::Strings::NOVALPIE_UNGROUPED, {}, {}});
    int matched = 0;
    const cJSON* row;
    cJSON_ArrayForEach(row, shelf) {
        const auto type = Text(row, "type");
        if (!folder && type == "group") {
            const auto* group = Field(Field(row, "favorite_group"), "group");
            auto id = Id(group, "id");
            if (!id.empty() && id != "0")
                result.items.push_back({std::move(id), Text(group, "name"), {}, {}});
        } else if (folder && type == "favorite_novel") {
            const auto* favorite = Field(row, "favorite_novel");
            auto group = Id(favorite, "group_id");
            if ((group.empty() ? "0" : group) != *folder)
                continue;
            const auto* novel = Field(favorite, "novel");
            auto id = Id(novel, "id");
            if (!id.empty() && matched++ / page_size == page) {
                const auto* status = Field(favorite, "reading_status");
                result.items.push_back({id, Text(novel, "title"), {}, id, false,
                                        Id(Field(status, "last_chapter"), "id")});
            }
        }
    }
    if (folder) {
        result.pages = std::max(1, (matched + page_size - 1) / page_size);
        result.next = page + 1 < result.pages;
    }
    return result;
}
CatalogPage NovalpieService::LoadFolders() {
    if (Username().empty())
        return {};
    return Bookshelf(nullptr, 0, 0, FetchPolicy::Refresh);
}
CatalogPage NovalpieService::LoadNovels(const std::string& folder, int page, int page_size, FetchPolicy policy) {
    return Bookshelf(&folder, page, page_size, policy);
}
Directory NovalpieService::LoadDirectory(const Item& book, FetchPolicy policy) {
    const auto path = "/novels/" + Quote(book.id);
    auto detail = Fetch(path + "/detail", book.id, policy);
    auto chapters = Fetch(path + "/chapters", book.id + "/toc", policy);
    Directory result;
    result.title = Title(detail.json.get());
    result.error = std::move(chapters.error);
    const auto* entries = Field(chapters.json.get(), "data");
    if (!cJSON_IsArray(entries)) {
        if (chapters.json)
            result.error = Lang::Strings::NOVELIA_TOC_INVALID;
        return result;
    }
    const cJSON* entry;
    cJSON_ArrayForEach(entry, entries) {
        auto id = Id(entry, "id");
        if (!id.empty())
            result.items.push_back({std::move(id), Title(entry), {}, {}});
    }
    // 网页从收藏夹打开时用书架条目的 reading_status；实时进度有可读章节时优先。
    result.remote = ResumeChapter(result.items, book.resume_chapter, Id(detail.json.get(), "lastChapterId"));
    if (policy == FetchPolicy::Refresh && !Username().empty()) {
        const auto response = Api("GET", "/reader/progress?novel_id=" + Quote(book.id), {}, false);
        if (!response.error_code && response.status == 200) {
            auto progress = Parse(response.body);
            const auto* data = Field(progress.get(), "data");
            result.remote = ResumeChapter(result.items, Id(data ? data : progress.get(), "last_chapter_id"),
                                          result.remote);
        }
    }
    return result;
}
std::string NovalpieService::ProgressPath(const Item& novel) const {
    return AccountRoot() + "/progress/" + sd_file::Key(novel.id) + ".json";
}
std::string NovalpieService::Resume(const Item& novel, const Directory& directory) {
    auto saved = Parse(Last(novel));
    auto local = Text(saved.get(), "chapter");
    if (!cJSON_IsTrue(Field(saved.get(), "pending")))
        return ResumeChapter(directory.items, directory.remote, local);
    return ResumeChapter(directory.items, local, directory.remote);
}
bool NovalpieService::Shown(const Item& novel, const Item& chapter) {
    error.clear();
    auto value = Object();
    Set(value.get(), "chapter", chapter.id);
    cJSON_AddBoolToObject(value.get(), "pending", true);
    const auto path = ProgressPath(novel);
    if (!Save(path, Encode(value.get())))
        return false;
    if (Text(config.get(), "token").empty())
        return true;
    if (!IsValidNumericId(novel.id) || !IsValidNumericId(chapter.id)) {
        error = Lang::Strings::NOVELIA_HTTP_INVALID_REQUEST;
        return false;
    }
    auto body = Object();
    cJSON_AddRawToObject(body.get(), "novel_id", novel.id.c_str());
    cJSON_AddRawToObject(body.get(), "chapter_id", chapter.id.c_str());
    auto response = Api("POST", "/reader/progress", Encode(body.get()), false);
    if (response.error_code == -ENETDOWN)
        return true; // 后台同步仅使用现有连接，离线时保留本地进度。
    if (response.error_code != 0 || response.status < 200 || response.status >= 300) {
        error = ApiError(response);
        return false;
    }
    auto reply = Parse(response.body);
    if (cJSON_IsFalse(Field(reply.get(), "success"))) {
        error = Lang::Strings::NOVELIA_HTTP_PROTOCOL_ERROR;
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(value.get(), "pending");
    return Save(path, Encode(value.get()));
}
bool NovalpieService::OpenReaderSession() {
    const auto started = std::chrono::steady_clock::now();
    auto session = Api("GET", "/reader/session-key");
    if (session.error_code || session.status != 200) {
        error = !session.error_code && session.status == 403 ? Lang::Strings::NOVALPIE_READER_SESSION_DENIED
                                                             : ApiError(session);
        return false;
    }
    auto reply = Parse(session.body);
    auto id = Text(reply.get(), "session_id");
    auto key = Text(reply.get(), "session_key");
    if (id.empty() || key.empty()) {
        error = Lang::Strings::NOVELIA_HTTP_PROTOCOL_ERROR;
        return false;
    }
    const auto* ttl = Field(reply.get(), "ttl");
    const auto seconds = cJSON_IsNumber(ttl) ? std::max(0, ttl->valueint) : 0;
    reader_session = {std::move(id), std::move(key), started + std::chrono::seconds(seconds)};
    return true;
}
std::string NovalpieService::ChapterContent(const Item& chapter) {
    error.clear();
    const bool reuse = std::chrono::steady_clock::now() < reader_session.expires;
    if (!reuse && !OpenReaderSession())
        return {};

    auto request = [&] {
        return Api("GET", "/chapters/" + Quote(chapter.id) + "/content?session=" +
                              Quote(reader_session.id) + "&replace_mode=india&show_images=1");
    };
    auto expired = [](const proxy_transport::Response& response) {
        return !response.error_code && response.status == 401 &&
               Text(Parse(response.body).get(), "message") == "会话已过期或无效，请重新获取密钥";
    };
    auto response = request();
    // 正文 API 的 401 也可能是账号过期。只有阅读会话失效才重新申请密钥。
    if (reuse && expired(response)) {
        reader_session = {};
        if (!OpenReaderSession())
            return {};
        response = request();
    }
    if (response.error_code || response.status != 200) {
        if (expired(response)) {
            reader_session = {};
            error = Lang::Strings::NOVALPIE_READER_SESSION_DENIED;
        } else {
            error = ApiError(response);
        }
        return {};
    }
    auto json = Parse(response.body);
    const auto* data = Field(json.get(), "data");
    auto html = DecodeNovalpieChapter(data ? data : json.get(), reader_session.key);
    if (html.empty())
        error = Lang::Strings::NOVELIA_HTTP_PROTOCOL_ERROR;
    return html;
}
bool NovalpieService::PrepareChapter(const Item& novel, const Item& chapter, const std::string&,
                                     std::string& cache_root, std::string& epub_filename,
                                     std::string&) const {
    cache_root = Root() + "/chapters/" + sd_file::Key(Username() + "/" + novel.id + "/" + chapter.id);
    epub_filename = "default.epub";
    return true;
}
bool NovalpieService::WriteChapterBody(const Item&, const Item& chapter, const std::string&,
                                       const ChapterWriter& writer, std::vector<std::string>&) {
    auto html = ChapterContent(chapter);
    if (html.empty()) {
        if (writer.error)
            *writer.error = error;
        return false;
    }
    reader::HtmlBlockParser parser("", [&](reader::ContentBlock&& block) {
        if (writer.cancelled())
            return false;
        if (block.kind == reader::ContentKind::kImage)
            return writer.embed_image(block.image_href);
        return (block.continuation || writer.append_xml("<p>")) && writer.append_xml(EscapeXml(block.text)) &&
               (block.continues || writer.append_xml("</p>"));
    });
    parser.SetWebFragment();
    return parser.Feed(reinterpret_cast<const uint8_t*>(html.data()), html.size()) && parser.Finish();
}
std::string NovalpieService::ImageUrl(const std::string& url) const {
    // 阅读页把这些旧地址映射到同一图片 CDN。
    for (const std::string_view origin : {
             "https://photo.1695ffc38d5faf9827f7fbd055757d56.r2.cloudflarestorage.com/",
             "https://novelkit.top/images/", "https://jpg.freenovel.sbs/"}) {
        if (url.compare(0, origin.size(), origin) == 0) {
            auto file = url.substr(origin.size());
            return "https://images.novelpia.com/imagebox/" + file.substr(0, 2) + "/" + file;
        }
    }
    return url;
}
std::string NovalpieService::AuthorsFromDetail(const cJSON* detail) const {
    return Text(detail, "authorName");
}
} // namespace web_novel
