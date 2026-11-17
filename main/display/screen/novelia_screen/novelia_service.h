#pragma once
#include "novelia_model.h"
#include "proxy_transport/http.h"
#include <functional>
#include <memory>
#include <string>
namespace novelia {
enum class FetchPolicy { Refresh, PreferCache };
struct Fetched {
    Json json{nullptr, cJSON_Delete};
    std::string error; // 离线回退时可同时有数据和错误提示。
};
class Service {
public:
    bool Init();
    bool Ready();
    std::function<bool()> network_ready;
    std::function<bool()> network_present; // 只查询当前连接，不触发联网或提示。
    std::function<bool()> cancelled;
    std::string error;
    bool Configure(const cJSON* input);
    bool Login(const std::string& username, const std::string& password);
    bool Logout();
    bool TestProxy(const cJSON* input = nullptr);
    Fetched Fetch(const std::string& path, const std::string& cache_key,
                  FetchPolicy policy = FetchPolicy::Refresh);
    std::string Read(const std::string& path);
    bool Save(const std::string& path, const std::string& text);
    std::string Root() const;
    std::string AccountRoot() const;
    std::string Last(const Item& novel);
    bool Shown(const Item& novel, const Item& chapter);
    bool SetTranslation(const std::string& mode);
    bool SetPrefetch(bool enabled);
    std::string ProxySummary() const;
    std::string Username() const;
    std::string Translation() const;
    bool Prefetch() const;
    std::string PublicConfig() const;
    std::unique_ptr<proxy_transport::Dialer> MakeDialer() const;
    proxy_transport::Response Get(proxy_transport::Dialer& dialer, const std::string& url,
                                  size_t limit = proxy_transport::kMaxBodyBytes);

private:
    Json config = Object(); // 仅串行 worker 访问。
    static std::unique_ptr<proxy_transport::Dialer> Dialer(const cJSON* config);
    Json ProxyConfig(const cJSON* input);
    proxy_transport::Response Api(const std::string& method, const std::string& path,
                                 const std::string& body = {}, bool wait_for_network = true);
    int NetworkStatus(bool wait_for_network = true) const;
    bool Write(const std::string& path, const std::string& text) const;
    bool Persist();
};
struct FormResult {
    bool ok = false;
    std::string message;
};
FormResult ApplyForm(const cJSON* input, Service& service);
} // namespace novelia
