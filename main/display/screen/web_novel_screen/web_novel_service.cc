#include "web_novel_service.h"
#include "assets/lang_web_novel.h"
#include "assets/lang_screen.h"
#include "sd_file.h"
#include "sd_paths.h"
#include <cerrno>
#include <cstdio>
#include <utility>
#ifdef ESP_PLATFORM
#include <esp_log.h>
#endif

namespace web_novel {
namespace {
constexpr const char* kProxyKeys[] = {"server", "port", "sni", "proxy_password", "proxy_enabled", "skip_cert_verify"};
constexpr const char* kLegacyNoveliaConfig = SD_APP_ROOT "/novelia/config.json";

void CopyProxyFields(const cJSON* source, cJSON* target, bool include_password) {
    if (!cJSON_IsObject(source) || !cJSON_IsObject(target))
        return;
    for (const char* key : kProxyKeys) {
        if (!include_password && std::string(key) == "proxy_password")
            continue;
        if (const auto* value = cJSON_GetObjectItemCaseSensitive(source, key)) {
            cJSON_DeleteItemFromObjectCaseSensitive(target, key);
            cJSON_AddItemToObject(target, key, cJSON_Duplicate(value, true));
        }
    }
}
} // namespace

std::string Service::SharedProxyPath() {
    return SD_APP_ROOT "/web_novel/proxy.json";
}
std::string Service::HttpError(int code) {
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
std::string Service::ApiError(const proxy_transport::Response& response) {
    if (response.error_code)
        return HttpError(response.error_code);
    if (response.status == 401)
        return Lang::Strings::NOVELIA_LOGIN_EXPIRED;
    return Format(Lang::Strings::NOVELIA_REQUEST_FAILED_FMT, response.status);
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
    config = cJSON_IsObject(saved.get()) ? std::move(saved) : Object();

    auto shared = Parse(Read(SharedProxyPath()));
    if (cJSON_IsObject(shared.get())) {
        proxy_config = std::move(shared);
    } else {
        proxy_config = Object();
        // 兼容旧版：共享代理文件尚未创建时，从 Novelia 旧配置读取已有代理字段。
        auto legacy = Parse(Read(kLegacyNoveliaConfig));
        CopyProxyFields(legacy.get(), proxy_config.get(), true);
    }
    OnInit();
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
            ESP_LOGW("WebNovel", "saved; backup cleanup failed errno=%d", errno);
#endif
        }
        return true;
    }
#ifdef ESP_PLATFORM
    ESP_LOGE("WebNovel", "save failed: %s errno=%d restore_errno=%d",
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
bool Service::PersistProxy() {
    if (!sd_file::EnsurePath(SD_APP_ROOT "/web_novel")) {
        error = Lang::Strings::NOVELIA_SAVE_FAILED;
        return false;
    }
    return Save(SharedProxyPath(), Encode(proxy_config.get()));
}
std::unique_ptr<proxy_transport::Dialer> Service::Dialer(const cJSON* cfg) {
    auto direct = proxy_transport::MakeDirectDialer();
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cfg, "proxy_enabled")))
        return direct;
    proxy_transport::TrojanOptions options;
    options.server = {
        Text(cfg, "server"),
        static_cast<uint16_t>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(cfg, "port")))};
    options.password = Text(cfg, "proxy_password");
    options.tls.server_name = Text(cfg, "sni");
    options.tls.verify_certificate =
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cfg, "skip_cert_verify"));
    return proxy_transport::MakeTrojanDialer(std::move(options), std::move(direct));
}
Json Service::ProxyConfig(const cJSON* input) {
    auto replacement = Json(cJSON_Duplicate(proxy_config.get(), true), cJSON_Delete);
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
    auto old = std::move(proxy_config);
    proxy_config = std::move(replacement);
    if (!PersistProxy()) {
        proxy_config = std::move(old);
        return false;
    }
    OnProxyChanged();
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
    return Dialer(proxy_config.get());
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
bool Service::Logout() {
    Set(config.get(), "username", "");
    Set(config.get(), "token", "");
    Set(config.get(), "refresh_token", "");
    return Persist();
}
Fetched Service::Fetch(const std::string& path, const std::string& cache_key, FetchPolicy policy) {
    auto cache = AccountRoot() + "/snapshots/" + sd_file::Key(cache_key) + ".json";
    if (policy == FetchPolicy::PreferCache) {
        auto saved = Parse(Read(cache));
        if (saved) {
#ifdef ESP_PLATFORM
            ESP_LOGI("WebNovel", "snapshot cache hit");
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
    for (const char* key : {"password", "token", "refresh_token"})
        cJSON_DeleteItemFromObjectCaseSensitive(visible.get(), key);
    for (const char* key : kProxyKeys)
        cJSON_DeleteItemFromObjectCaseSensitive(visible.get(), key);
    CopyProxyFields(proxy_config.get(), visible.get(), false);
    cJSON_AddBoolToObject(visible.get(), "has_proxy_password", !Text(proxy_config.get(), "proxy_password").empty());
    Set(visible.get(), "app_name", AppName());
    AppendPortalConfig(visible.get());
    return Encode(visible.get());
}
std::string Service::ProxySummary() const {
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(proxy_config.get(), "proxy_enabled")))
        return Lang::Strings::NOVELIA_PROXY_DIRECT;
    auto server = Text(proxy_config.get(), "server");
    if (server.empty())
        return Lang::Strings::NOVELIA_PROXY_UNCONFIGURED;
    auto* port = cJSON_GetObjectItemCaseSensitive(proxy_config.get(), "port");
    return "Trojan · " + server + (cJSON_IsNumber(port) ? ":" + std::to_string(port->valueint) : "");
}
bool Service::SetTranslation(const std::string& mode) {
    if (!SupportsTranslation() || !TranslationField(mode))
        return false;
    Set(config.get(), "translation", mode);
    return Persist();
}
bool Service::SetPrefetch(bool enabled) {
    cJSON_DeleteItemFromObjectCaseSensitive(config.get(), "prefetch");
    cJSON_AddBoolToObject(config.get(), "prefetch", enabled);
    return Persist();
}
std::string Service::ProgressPath(const Item& novel) const {
    return Root() + "/progress/" + sd_file::Key(novel.id) + ".json";
}
std::string Service::Last(const Item& novel) {
    return Read(ProgressPath(novel));
}
std::string Service::Resume(const Item& novel, const Directory& directory) {
    auto saved = Parse(Last(novel));
    return ResumeChapter(directory.items, Text(saved.get(), "chapter"), directory.remote);
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
        result.ok = service.LoginForm(input);
        result.message = Lang::Strings::NOVELIA_FORM_LOGIN_OK;
    }
    if (!result.ok)
        result.message = service.error.empty() ? Lang::Strings::NOVELIA_FORM_INVALID : service.error;
    return result;
}
} // namespace web_novel
