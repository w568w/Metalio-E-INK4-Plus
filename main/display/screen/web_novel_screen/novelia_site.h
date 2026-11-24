#pragma once
#include "web_novel_service.h"

namespace web_novel {
std::vector<Item> NoveliaFolders(const cJSON* response, std::string* error = nullptr);
std::vector<Item> NoveliaNovels(const cJSON* response, std::string* error = nullptr);
std::vector<Item> NoveliaToc(const cJSON* response, std::string* error = nullptr);

class NoveliaService : public Service {
public:
    const char* SiteId() const override { return "novelia"; }
    const char* AppName() const override { return "Novelia"; }
    std::string Root() const override;
    bool SupportsTranslation() const override { return true; }
    bool RefreshNovelsByDefault() const override { return true; }
    bool SyncResumeOnDirectoryLoad() const override { return true; }

    bool Login(const std::string& username, const std::string& password);
    bool LoginForm(const cJSON* input) override;
    bool TestProxy(const cJSON* input = nullptr) override;
    CatalogPage LoadFolders() override;
    CatalogPage LoadNovels(const std::string& folder, int page, int page_size, FetchPolicy policy) override;
    Directory LoadDirectory(const Item& book, FetchPolicy policy = FetchPolicy::Refresh) override;
    bool Shown(const Item& novel, const Item& chapter) override;

    bool PrepareChapter(const Item& novel, const Item& chapter, const std::string& mode,
                        std::string& cache_root, std::string& epub_filename,
                        std::string& out_error) const override;
    bool WriteChapterBody(const Item& novel, const Item& chapter, const std::string& mode,
                          const ChapterWriter& writer,
                          std::vector<std::string>& available_translations) override;
    std::string AuthorsFromDetail(const cJSON* detail) const override;

protected:
    void OnInit() override;
    proxy_transport::Response Api(const std::string& method, const std::string& path,
                                  const std::string& body = {}, bool wait_for_network = true) override;
};
} // namespace web_novel
