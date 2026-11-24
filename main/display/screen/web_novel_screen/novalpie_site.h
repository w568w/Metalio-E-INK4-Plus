#pragma once
#include "web_novel_service.h"
#include <chrono>

namespace web_novel {
class NovalpieService : public Service {
public:
    const char* SiteId() const override { return "novalpie"; }
    const char* AppName() const override { return "Novalpie"; }
    std::string Root() const override;

    bool Login(const std::string& username, const std::string& password, const std::string& captcha);
    bool ImportSession(const std::string& token);
    bool LoginForm(const cJSON* input) override;
    bool Logout() override;
    bool TestProxy(const cJSON* input = nullptr) override;
    CatalogPage LoadFolders() override;
    CatalogPage LoadNovels(const std::string& folder, int page, int page_size, FetchPolicy policy) override;
    Directory LoadDirectory(const Item& book, FetchPolicy policy = FetchPolicy::Refresh) override;
    std::string ProgressPath(const Item& novel) const override;
    std::string Resume(const Item& novel, const Directory& directory) override;
    bool Shown(const Item& novel, const Item& chapter) override;
    std::string ChapterContent(const Item& chapter);

    bool PrepareChapter(const Item& novel, const Item& chapter, const std::string& mode,
                        std::string& cache_root, std::string& epub_filename,
                        std::string& out_error) const override;
    bool WriteChapterBody(const Item& novel, const Item& chapter, const std::string& mode,
                          const ChapterWriter& writer,
                          std::vector<std::string>& available_translations) override;
    std::string ImageUrl(const std::string& url) const override;
    std::string AuthorsFromDetail(const cJSON* detail) const override;

protected:
    void OnProxyChanged() override { reader_session = {}; }
    void AppendPortalConfig(cJSON* visible) const override;
    proxy_transport::Response Api(const std::string& method, const std::string& path,
                                  const std::string& body = {}, bool wait_for_network = true) override;

private:
    CatalogPage Bookshelf(const std::string* folder, int page, int page_size, FetchPolicy policy);
    bool OpenReaderSession();
    bool SaveSession(const std::string& token, const std::string& username);
    struct ReaderSession {
        std::string id, key;
        std::chrono::steady_clock::time_point expires;
    } reader_session; // 仅由串行 worker 使用；不保存到 SD。
};
} // namespace web_novel
