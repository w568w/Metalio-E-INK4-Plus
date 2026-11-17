#include "novelia_service.h"
#include "assets/lang_novelia.h"
#include "assets/lang_screen.h"
#include "sd_paths.h"
#include <cstdio>
#include <cerrno>
#include <unistd.h>
#include "sd_file.h"
#include <strings.h>
#include <utility>
#ifdef ESP_PLATFORM
#include <esp_log.h>
#endif
namespace novelia {
namespace {
std::string HttpError(int code) {
    switch (code) {
    case 0: return {};
    case -EINVAL: return Lang::Strings::NOVELIA_HTTP_INVALID_REQUEST;
    case -ENOTSUP: return Lang::Strings::NOVELIA_HTTP_UNSUPPORTED_METHOD;
    case -EMSGSIZE: return Lang::Strings::NOVELIA_HTTP_RESPONSE_TOO_LARGE;
    case -ENOMEM: return Lang::Strings::COMMON_MEMORY_RETRY;
    case -EPROTO: return Lang::Strings::NOVELIA_HTTP_PROTOCOL_ERROR;
    case -ECANCELED: return Lang::Strings::NOVELIA_CANCELLED;
    case -ENETDOWN: return Lang::Strings::NOVELIA_NETWORK_NOT_READY;
    default: return Format(Lang::Strings::NOVELIA_CONNECTION_FAILED_FMT, code);
    }
}
constexpr const char* kRoot = SD_APP_ROOT "/novelia";
std::string ApiError(const proxy_transport::Response& response) {
    if (response.error_code)
        return HttpError(response.error_code);
    if (response.status == 401)
        return Lang::Strings::NOVELIA_LOGIN_EXPIRED;
    return Format(Lang::Strings::NOVELIA_REQUEST_FAILED_FMT, response.status);
}
constexpr const char* kApi = "https://n.novelia.cc/api";
constexpr const char* kAuth = "https://auth.novelia.cc/api/v1/auth/";
std::string Token(std::string body) {
    auto parsed = Parse(body);
    if (cJSON_IsString(parsed.get()))
        return parsed->valuestring;
    return body;
}
} // namespace
std::string Service::Root() const {
    return kRoot;
}
std::string Service::AccountRoot() const {
    return Root() + "/accounts/" + sd_file::Key(Username());
}
bool Service::Init() {
    if (!sd_file::EnsurePath(Root())) {
        error = Lang::Strings::NOVELIA_NO_SD;
        return false;
    }
    auto saved = Parse(Read(Root() + "/config.json"));
    if (cJSON_IsObject(saved.get()))
        config = std::move(saved);
    if (Translation().empty())
        Set(config.get(), "translation", kTranslations[0].id);
    return true;
}
std::string Service::Read(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        file = std::fopen((path + ".previous").c_str(), "rb");
    if (!file)
        return {};
    std::string data;
    // 快照来自 Request，采用同一个响应体上限，避免损坏文件耗尽内存。
    char buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file))) {
        if (data.size() + n > proxy_transport::kMaxBodyBytes) {
            data.clear();
            break;
        }
        data.append(buffer, n);
    }
    if (std::ferror(file))
        data.clear();
    std::fclose(file);
    return data;
}
bool Service::Write(const std::string& path, const std::string& text) const {
    const auto backup = path + ".previous";
    const auto result = sd_file::WriteFile(path, backup, text.data(), text.size());
    if (result.Ok()) {
        if (std::remove(backup.c_str()) != 0 && errno != ENOENT) {
#ifdef ESP_PLATFORM
            ESP_LOGW("Novelia", "saved; backup cleanup failed errno=%d", errno);
#endif
        }
        return true;
    }
#ifdef ESP_PLATFORM
    ESP_LOGE("Novelia", "save failed: %s errno=%d restore_errno=%d",
             result.stage, result.error, result.rollback_error);
#endif
    return false;
}
bool Service::Save(const std::string& path, const std::string& text) {
    if (Write(path, text))
        return true;
    error = Lang::Strings::NOVELIA_SAVE_FAILED;
    return false;
}
bool Service::Persist() {
    return Save(Root() + "/config.json", Encode(config.get()));
}
std::unique_ptr<proxy_transport::Dialer> Service::Dialer(const cJSON* config) {
    auto direct = proxy_transport::MakeDirectDialer();
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(config, "proxy_enabled")))
        return direct;
    proxy_transport::TrojanOptions options;
    options.server = {
        Text(config, "server"),
        static_cast<uint16_t>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(config, "port")))};
    options.password = Text(config, "proxy_password");
    options.tls.server_name = Text(config, "sni");
    options.tls.verify_certificate =
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(config, "skip_cert_verify"));
    return proxy_transport::MakeTrojanDialer(std::move(options), std::move(direct));
}
Json Service::ProxyConfig(const cJSON* input) {
    auto replacement = Json(cJSON_Duplicate(config.get(), true), cJSON_Delete);
    for (const char* key : {"server", "sni", "proxy_password"}) {
        auto* value = cJSON_GetObjectItemCaseSensitive(input, key);
        if (cJSON_IsString(value) && (std::string(key) != "proxy_password" || value->valuestring[0]))
            Set(replacement.get(), key, value->valuestring);
    }
    for (const char* key : {"proxy_enabled", "skip_cert_verify", "port"}) {
        auto* value = cJSON_GetObjectItemCaseSensitive(input, key);
        if (value) {
            cJSON_DeleteItemFromObjectCaseSensitive(replacement.get(), key);
            cJSON_AddItemToObject(replacement.get(), key, cJSON_Duplicate(value, true));
        }
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(replacement.get(), "proxy_enabled"))) {
        double port = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(replacement.get(), "port"));
        if (!(port >= 1 && port <= 65535 && port == static_cast<int>(port)) ||
            Text(replacement.get(), "server").empty() || Text(replacement.get(), "proxy_password").empty()) {
            error = Lang::Strings::NOVELIA_PROXY_INCOMPLETE;
            return Json(nullptr, cJSON_Delete);
        }
    }
    return replacement;
}
bool Service::Configure(const cJSON* input) {
    auto replacement = ProxyConfig(input);
    if (!replacement)
        return false;
    auto old = std::move(config);
    config = std::move(replacement);
    if (!Persist()) {
        config = std::move(old);
        return false;
    }
    return true;
}
int Service::NetworkStatus(bool wait_for_network) const {
    if (cancelled && cancelled())
        return -ECANCELED;
    const auto& check = wait_for_network ? network_ready : network_present;
    const bool ready = !check || check();
    if (cancelled && cancelled())
        return -ECANCELED;
    return ready ? 0 : -ENETDOWN;
}
bool Service::Ready() {
    const int status = NetworkStatus();
    if (status)
        error = HttpError(status);
    return status == 0;
}
std::unique_ptr<proxy_transport::Dialer> Service::MakeDialer() const {
    return Dialer(config.get());
}
proxy_transport::Response Service::Get(proxy_transport::Dialer& dialer, const std::string& url, size_t limit) {
    proxy_transport::Response response;
    response.error_code = NetworkStatus();
    if (response.error_code)
        return response;
    response = Request(dialer, "GET", url, {}, {}, limit);
    if (cancelled && cancelled())
        response.error_code = -ECANCELED;
    return response;
}
bool Service::Login(const std::string& username, const std::string& password) {
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
    auto dialer = Dialer(config.get());
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
    auto token = Token(response.body);
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
bool Service::Logout() {
    Set(config.get(), "username", "");
    Set(config.get(), "token", "");
    Set(config.get(), "refresh_token", "");
    return Persist();
}
proxy_transport::Response Service::Api(const std::string& method, const std::string& path,
                                      const std::string& body, bool wait_for_network) {
    proxy_transport::Response result;
    result.error_code = NetworkStatus(wait_for_network);
    if (result.error_code)
        return result;
    auto dialer = MakeDialer();
    auto token = Text(config.get(), "token");
    std::vector<std::pair<std::string, std::string>> headers{{"Authorization", "Bearer " + token}};
    // 阅读历史接口用 call.receive<String>() 读取章节 ID，网页端 ky 也发送 text/plain。
    // 目前仅 read-history 使用 PUT；以后发送 JSON 时需改为显式传入请求头。
    if (method == "PUT")
        headers.emplace_back("Content-Type", "text/plain;charset=UTF-8");
    auto response =
        Request(*dialer, method, std::string(kApi) + path, body, headers);
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
            auto refreshed = Token(renewed.body);
            if (!refreshed.empty() && refreshed.find_first_of("\r\n") == std::string::npos) {
                Set(config.get(), "token", refreshed);
                Write(Root() + "/config.json", Encode(config.get()));
                headers.front().second = "Bearer " + refreshed;
                response = Request(*dialer, method, std::string(kApi) + path, body, headers);
            }
        }
    }
    return response;
}
bool Service::TestProxy(const cJSON* input) {
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
Fetched Service::Fetch(const std::string& path, const std::string& cache_key, FetchPolicy policy) {
    auto cache = AccountRoot() + "/snapshots/" + sd_file::Key(cache_key) + ".json";
    if (policy == FetchPolicy::PreferCache) {
        auto saved = Parse(Read(cache));
        if (saved) {
#ifdef ESP_PLATFORM
            ESP_LOGI("Novelia", "snapshot cache hit");
#endif
            return {std::move(saved), {}};
        }
    }
    auto response = Api("GET", path);
    auto result = Parse(response.body);
    if (response.error_code == 0 && response.status == 200 && result) {
        // 写入失败记录日志，查询仍返回有效数据。
        Write(cache, response.body);
        return {std::move(result), {}};
    }
    auto failure = ApiError(response);
    auto saved = Parse(Read(cache));
    if (saved)
        failure += Lang::Strings::NOVELIA_OFFLINE_CACHE;
    return {std::move(saved), std::move(failure)};
}
std::string Service::Username() const {
    return Text(config.get(), "username");
}
std::string Service::Translation() const {
    return Text(config.get(), "translation");
}
bool Service::Prefetch() const {
    return !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(config.get(), "prefetch"));
}
std::string Service::PublicConfig() const {
    auto visible = Json(cJSON_Duplicate(config.get(), true), cJSON_Delete);
    cJSON_AddBoolToObject(visible.get(), "has_proxy_password", !Text(config.get(), "proxy_password").empty());
    for (const char* key : {"password", "token", "refresh_token", "proxy_password"})
        cJSON_DeleteItemFromObjectCaseSensitive(visible.get(), key);
    return Encode(visible.get());
}
std::string Service::ProxySummary() const {
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(config.get(), "proxy_enabled")))
        return Lang::Strings::NOVELIA_PROXY_DIRECT;
    auto server = Text(config.get(), "server");
    if (server.empty())
        return Lang::Strings::NOVELIA_PROXY_UNCONFIGURED;
    auto* port = cJSON_GetObjectItemCaseSensitive(config.get(), "port");
    return "Trojan · " + server + (cJSON_IsNumber(port) ? ":" + std::to_string(port->valueint) : "");
}
bool Service::SetTranslation(const std::string& mode) {
    if (!TranslationField(mode))
        return false;
    Set(config.get(), "translation", mode);
    return Persist();
}
bool Service::SetPrefetch(bool enabled) {
    cJSON_DeleteItemFromObjectCaseSensitive(config.get(), "prefetch");
    cJSON_AddBoolToObject(config.get(), "prefetch", enabled);
    return Persist();
}
std::string Service::Last(const Item& novel) {
    return Read(Root() + "/progress/" + sd_file::Key(novel.id) + ".json");
}
bool Service::Shown(const Item& novel, const Item& chapter) {
    error.clear();
    auto value = Object();
    Set(value.get(), "chapter", chapter.id);
    if (!Save(Root() + "/progress/" + sd_file::Key(novel.id) + ".json", Encode(value.get())))
        return false;
    if (Text(config.get(), "token").empty())
        return true;
    auto response = Api("PUT", "/user/read-history/" + Quote(novel.provider) + "/" + Quote(novel.novel),
                        chapter.id, false); // 不等待联网。
    if (response.error_code == -ENETDOWN)
        return true; // 后台同步仅使用现有连接，离线时保留本地进度。
    if (response.error_code != 0 || response.status < 200 || response.status >= 300) {
        error = ApiError(response);
        return false;
    }
    return true;
}
FormResult ApplyForm(const cJSON* input, Service& service) {
    const auto action = Text(input, "action");
    FormResult result;
    service.error.clear();
    if (action == "configure") {
        result.ok = service.Configure(input);
        result.message = Lang::Strings::NOVELIA_PROXY_SAVED;
    } else if (action == "test") {
        result.ok = service.TestProxy(input);
        result.message = Lang::Strings::NOVELIA_PROXY_TEST_OK;
    } else if (action == "login") {
        result.ok = service.Login(Text(input, "username"), Text(input, "password"));
        result.message = Lang::Strings::NOVELIA_FORM_LOGIN_OK;
    }
    if (!result.ok)
        result.message = service.error.empty() ? Lang::Strings::NOVELIA_FORM_INVALID : service.error;
    return result;
}
} // namespace novelia
