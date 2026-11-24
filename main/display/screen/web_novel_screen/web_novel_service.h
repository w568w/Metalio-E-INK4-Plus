#pragma once
#include "web_novel_model.h"
#include "proxy_transport/http.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace web_novel {
enum class FetchPolicy { Refresh, PreferCache };
struct Fetched {
    Json json{nullptr, cJSON_Delete};
    std::string error; // 离线回退时可同时有数据和错误提示。
};
struct ChapterWriter {
    std::function<bool(const std::string&)> append_xml;
    std::function<bool(const std::string&)> embed_image;
    std::function<bool()> cancelled;
    std::string* error = nullptr;
};

class Service {
public:
    virtual ~Service() = default;

    // 站点标识与能力描述。
    virtual const char* SiteId() const = 0;
    virtual const char* AppName() const = 0;
    virtual std::string Root() const = 0;
    virtual bool SupportsTranslation() const { return false; }
    virtual bool RefreshNovelsByDefault() const { return false; }
    virtual bool SyncResumeOnDirectoryLoad() const { return false; }

    // 生命周期与网络回调。
    bool Init();
    bool Ready();
    std::function<bool()> network_ready;
    std::function<bool()> network_present; // 只查询当前连接，不触发联网或提示。
    std::function<bool()> cancelled;
    std::string error;

    // 共享代理配置（跨站点共用同一份配置）。
    static std::string SharedProxyPath();
    bool Configure(const cJSON* input);
    virtual bool TestProxy(const cJSON* input = nullptr) = 0;
    std::string ProxySummary() const;
    std::unique_ptr<proxy_transport::Dialer> MakeDialer() const;

    // 账号与站点内容。
    virtual bool LoginForm(const cJSON* input) = 0;
    virtual bool Logout();
    virtual CatalogPage LoadFolders() = 0;
    virtual CatalogPage LoadNovels(const std::string& folder, int page, int page_size, FetchPolicy policy) = 0;
    virtual Directory LoadDirectory(const Item& book, FetchPolicy policy = FetchPolicy::Refresh) = 0;

    // 章节下载适配点。
    virtual bool PrepareChapter(const Item& novel, const Item& chapter, const std::string& mode,
                                std::string& cache_root, std::string& epub_filename,
                                std::string& out_error) const = 0;
    virtual bool WriteChapterBody(const Item& novel, const Item& chapter, const std::string& mode,
                                  const ChapterWriter& writer,
                                  std::vector<std::string>& available_translations) = 0;
    virtual std::string ImageUrl(const std::string& url) const { return url; }
    virtual std::string AuthorsFromDetail(const cJSON* detail) const = 0;

    // 通用读写、缓存、设置与进度。
    Fetched Fetch(const std::string& path, const std::string& cache_key,
                  FetchPolicy policy = FetchPolicy::Refresh);
    std::string Read(const std::string& path);
    bool Save(const std::string& path, const std::string& text);
    std::string AccountRoot() const;
    virtual std::string ProgressPath(const Item& novel) const;
    std::string Last(const Item& novel);
    virtual std::string Resume(const Item& novel, const Directory& directory);
    virtual bool Shown(const Item& novel, const Item& chapter) = 0;
    bool SetTranslation(const std::string& mode);
    bool SetPrefetch(bool enabled);
    std::string Username() const;
    std::string Translation() const;
    bool Prefetch() const;
    std::string PublicConfig() const;
    proxy_transport::Response Get(proxy_transport::Dialer& dialer, const std::string& url,
                                  size_t limit = proxy_transport::kMaxBodyBytes);

protected:
    Json config = Object();       // 站点私有配置，仅串行 worker 访问。
    Json proxy_config = Object(); // 共享代理配置，仅串行 worker 访问。

    virtual void OnInit() {}
    virtual void OnProxyChanged() {}
    virtual void AppendPortalConfig(cJSON*) const {}
    virtual proxy_transport::Response Api(const std::string& method, const std::string& path,
                                          const std::string& body = {}, bool wait_for_network = true) = 0;

    static std::unique_ptr<proxy_transport::Dialer> Dialer(const cJSON* cfg);
    static std::string HttpError(int code);
    static std::string ApiError(const proxy_transport::Response& response);
    Json ProxyConfig(const cJSON* input);
    int NetworkStatus(bool wait_for_network = true) const;
    bool Write(const std::string& path, const std::string& text) const;
    bool Persist();
    bool PersistProxy();
};

struct FormResult {
    bool ok = false;
    std::string message;
};
FormResult ApplyForm(const cJSON* input, Service& service);
} // namespace web_novel
