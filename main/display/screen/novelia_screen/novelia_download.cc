#include "novelia_download.h"
#include "novelia_service.h"
#include "novelia_epub.h"
#include "assets/lang_novelia.h"
#include "sd_file.h"
#include <cstdio>
#include <cerrno>
#include <dirent.h>
#include <memory>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include <esp_log.h>
#endif
namespace novelia {
namespace {
constexpr size_t kMaxImageBytes = 2 * 1024 * 1024;
// directory 必须是 Download 独占的 root/work；析构会删除其中所有文件，不能指向章节缓存目录。
struct ChapterScratch {
    std::string directory;
    std::vector<EpubImage> images{};
    ~ChapterScratch() {
        if (auto* entries = opendir(directory.c_str())) {
            while (auto* entry = readdir(entries)) {
                const std::string name = entry->d_name;
                if (name != "." && name != "..")
                    std::remove((directory + "/" + name).c_str());
            }
            closedir(entries);
            rmdir(directory.c_str());
        }
    }
};
proxy_transport::Response FetchImage(Service& service, proxy_transport::Dialer& dialer, const std::string& url) {
    auto image = service.Get(dialer, url, kMaxImageBytes);
    // CDN 跳转只跟随 HTTP(S)，不附带 Novelia 的认证信息。
    for (int redirects = 0;
         redirects < 3 && !(service.cancelled && service.cancelled()) && image.status >= 300 && image.status < 400;
         ++redirects) {
        auto location = image.Header("location");
        if (location.rfind("https://", 0) != 0 && location.rfind("http://", 0) != 0)
            break;
        image = service.Get(dialer, location, kMaxImageBytes);
    }
    return image;
}
} // namespace
Chapter Download(Service& service, const Item& novel, const Item& chapter, const std::string& mode) {
    Chapter result;
    const char* field = TranslationField(mode);
    if (!field) {
        result.error = Lang::Strings::NOVELIA_TRANSLATION_UNSUPPORTED;
        return result;
    }
    auto root = service.Root() + "/chapters/" + sd_file::Key(novel.id + "/" + chapter.id);
    ChapterScratch scratch{root + "/work"}; // 也回收上次断电留下的私有暂存文件。
    result.path = root + "/" + mode + ".epub";
    result.title = chapter.title;
    if (access(result.path.c_str(), R_OK) == 0)
        return result;
    auto fetched =
        service.Fetch("/novel/" + Quote(novel.provider) + "/" + Quote(novel.novel) + "/chapter/" + Quote(chapter.id),
                      novel.id + "/" + chapter.id);
    auto& payload = fetched.json;
    if (!payload) {
        result.error = fetched.error;
        return result;
    }
    for (const auto& translation : kTranslations) {
        auto* paragraphs = cJSON_GetObjectItemCaseSensitive(payload.get(), translation.field);
        if (cJSON_GetArraySize(paragraphs))
            result.available.push_back(translation.id);
    }
    auto* paragraphs = cJSON_GetObjectItemCaseSensitive(payload.get(), field);
    if (!cJSON_IsArray(paragraphs) || !cJSON_GetArraySize(paragraphs)) {
        result.error = Lang::Strings::NOVELIA_TRANSLATION_UNAVAILABLE;
        return result;
    }
    if (!sd_file::EnsurePath(scratch.directory)) {
        result.error = Lang::Strings::NOVELIA_CHAPTER_DIR_FAILED;
        return result;
    }
    auto& images = scratch.images;
    const auto xhtml_path = scratch.directory + "/chapter.xhtml";
    auto close_file = [](FILE* file) { std::fclose(file); };
    std::unique_ptr<FILE, decltype(close_file)> xhtml(std::fopen(xhtml_path.c_str(), "wb"), close_file);
    if (!xhtml) {
        result.error = Lang::Strings::NOVELIA_CHAPTER_TEXT_FAILED;
        return result;
    }
    auto append = [&](const std::string& text) {
        if (std::fwrite(text.data(), 1, text.size(), xhtml.get()) == text.size())
            return true;
        result.error = Lang::Strings::NOVELIA_CHAPTER_TEXT_FAILED;
        return false;
    };
    if (!append("<?xml version=\"1.0\" encoding=\"UTF-8\"?><html "
                "xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>" +
                EscapeXml(chapter.title) + "</title></head><body><h1>" + EscapeXml(chapter.title) + "</h1>"))
        return result;
    const cJSON* paragraph;
    auto dialer = service.MakeDialer();
    cJSON_ArrayForEach(paragraph, paragraphs) {
        if (service.cancelled && service.cancelled()) {
            result.error = Lang::Strings::NOVELIA_CANCELLED;
            return result;
        }
        if (!cJSON_IsString(paragraph))
            continue;
        std::string text = paragraph->valuestring;
        const std::string marker = "<图片>";
        if (text.rfind(marker, 0) == 0) {
            auto url = text.substr(marker.size());
            auto image = FetchImage(service, *dialer, url);
            auto mime = image.Header("content-type");
            mime = mime.substr(0, mime.find(';'));
            if (image.error_code == 0 && image.status == 200 && !image.body.empty() &&
                (mime == "image/jpeg" || mime == "image/png")) {
                auto name = "image" + std::to_string(images.size()) + (mime == "image/png" ? ".png" : ".jpg");
                if (!append("<p><img src=\"" + name + "\" alt=\"" + Lang::Strings::NOVELIA_IMAGE_ALT + "\"/></p>"))
                    return result;
                auto image_path = scratch.directory + "/" + name;
                images.push_back({name, mime, image_path});
                FILE* file = std::fopen(image_path.c_str(), "wb");
                if (!file) {
                    result.error = Lang::Strings::NOVELIA_IMAGE_WRITE_FAILED;
                    return result;
                }
                bool written = std::fwrite(image.body.data(), 1, image.body.size(), file) == image.body.size();
                if (std::fclose(file) != 0)
                    written = false;
                if (!written) {
                    result.error = Lang::Strings::NOVELIA_IMAGE_WRITE_FAILED;
                    return result;
                }
            } else if (!append("<p>" + std::string(Lang::Strings::NOVELIA_IMAGE_PLACEHOLDER) + "</p>"))
                return result;
        } else if (!append("<p>" + EscapeXml(text) + "</p>"))
            return result;
    }
    if (!append("</body></html>"))
        return result;
    if (std::fclose(xhtml.release()) != 0) {
        result.error = Lang::Strings::NOVELIA_CHAPTER_TEXT_FAILED;
        return result;
    }
    auto detail = Parse(service.Read(service.AccountRoot() + "/snapshots/" + sd_file::Key(novel.id) + ".json"));
    std::string authors;
    const cJSON* author;
    cJSON_ArrayForEach(author, cJSON_GetObjectItemCaseSensitive(detail.get(), "authors")) {
        auto name = Text(author, "name");
        if (!name.empty()) {
            if (!authors.empty())
                authors += "、";
            authors += name;
        }
    }
    const auto backup = result.path + ".previous";
    if (!WriteEpub(result.path + ".tmp", novel.title + " · " + chapter.title, authors, xhtml_path, images) ||
        !sd_file::ReplaceFile(result.path + ".tmp", result.path, backup).Ok()) {
        std::remove((result.path + ".tmp").c_str());
        result.error = Lang::Strings::NOVELIA_CHAPTER_SAVE_FAILED;
    } else if (std::remove(backup.c_str()) != 0 && errno != ENOENT) {
#ifdef ESP_PLATFORM
        ESP_LOGW("Novelia", "chapter saved; backup cleanup failed errno=%d", errno);
#endif
    }
    return result;
}
} // namespace novelia
