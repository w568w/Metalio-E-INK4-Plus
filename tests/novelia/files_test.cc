#include "novelia_service.h"
#include "novelia_download.h"
#include "novelia_cache.h"
#include "sd_file.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace Lang::Strings {
const char* NOVELIA_PROXY_DIRECT = "未启用（直连）";
const char* NOVELIA_PROXY_UNCONFIGURED = "Trojan（未配置）";

const char* NOVELIA_PROXY_SAVED = "代理设置已保存";
const char* NOVELIA_PROXY_TEST_OK = "连接成功；点击「保存代理」后生效";
const char* NOVELIA_FORM_LOGIN_OK = "登录成功，请返回设备";
const char* NOVELIA_FORM_INVALID = "无效操作";
const char* NOVELIA_CACHE_TITLE = "本地缓存";
const char* NOVELIA_CACHE_HINT = "点「统计缓存」查看占用";
const char* NOVELIA_CACHE_MB_FMT = "已使用 %.1f MB，共 %d 个文件";
const char* NOVELIA_CACHE_KB_FMT = "已使用 %.1f KB，共 %d 个文件";
const char* NOVELIA_CACHE_EMPTY = "无缓存";
const char* NOVELIA_CACHE_CLEARED = "缓存已清空";
const char* NOVELIA_CACHE_CLEAR_FAILED = "删除缓存失败，请检查 SD 卡";
const char* NOVELIA_CACHE_MEASURE_FAILED = "统计缓存失败，请检查 SD 卡";
const char* NOVELIA_CACHE_MEASURE = "统计缓存";
const char* NOVELIA_CACHE_CLEAR = "删除缓存";

const char* NOVELIA_IMAGE_WRITE_FAILED = "Could not write chapter image. Check the SD card.";
const char* NOVELIA_FAVORITES_INVALID = "Unexpected favorites data";
const char* NOVELIA_FOLDER_ID_MISSING = "Favorite folder has no id";
const char* NOVELIA_FOLDER_UNNAMED = "Favorites";
const char* NOVELIA_NOVELS_INVALID = "Unexpected novel list data";
const char* NOVELIA_NOVEL_ID_MISSING = "Novel has no id";
const char* NOVELIA_TOC_INVALID = "Unexpected chapter list data";
const char* NOVELIA_LOGIN_RESPONSE_INVALID = "Unexpected sign-in response";
const char* NOVELIA_CREDENTIALS_REQUIRED = "Enter the account and password";
const char* NOVELIA_NO_SD = "Insert the SD card and turn off USB transfer";
const char* NOVELIA_SAVE_FAILED = "Could not save to the SD card";
const char* NOVELIA_PROXY_INCOMPLETE = "Fill in the Trojan server, port and password";
const char* NOVELIA_TRANSLATION_UNSUPPORTED = "Unsupported translation";
const char* NOVELIA_TRANSLATION_UNAVAILABLE = "This translation is not available yet";
const char* NOVELIA_CHAPTER_DIR_FAILED = "Could not create the chapter work directory";
const char* NOVELIA_CHAPTER_TEXT_FAILED = "Could not write the chapter text";
const char* NOVELIA_CHAPTER_SAVE_FAILED = "Could not save the chapter. Check the SD card.";
const char* NOVELIA_IMAGE_ALT = "Illustration";
const char* NOVELIA_IMAGE_PLACEHOLDER = "[Illustration unavailable]";
const char* NOVELIA_HTTP_INVALID_REQUEST = "Invalid HTTP request";
const char* NOVELIA_HTTP_UNSUPPORTED_METHOD = "Unsupported HTTP method";
const char* NOVELIA_HTTP_RESPONSE_TOO_LARGE = "Response too large";
const char* NOVELIA_HTTP_PROTOCOL_ERROR = "Protocol error or incomplete response";
const char* NOVELIA_CONNECTION_FAILED_FMT = "Connection failed: %d";
const char* NOVELIA_LOGIN_FAILED_FMT = "Login failed: %d";
const char* NOVELIA_REQUEST_FAILED_FMT = "Request failed: %d";
const char* NOVELIA_CANCELLED = "Operation cancelled";
const char* NOVELIA_NETWORK_NOT_READY = "Wi-Fi is not connected. Check the network.";
const char* NOVELIA_LOGIN_EXPIRED = "Session expired. Scan the code to log in.";
const char* NOVELIA_OFFLINE_CACHE = " (showing offline cache)";
const char* COMMON_MEMORY_RETRY = "Not enough memory";
}

static int http_error = 0;
static int http_status = 200;
static int http_requests = 0;
static std::string last_method, last_url, last_body, last_authorization, last_content_type;
static bool renew_token = false;
static std::string chapter_body;
static std::string image_body = "image-bytes";
static int scratch_commits = 0;
namespace proxy_transport {
std::string Response::Header(const std::string& name) const {
    for (const auto& header : headers)
        if (header.first == name)
            return header.second;
    return {};
}
Response Request(proxy_transport::Dialer&, const std::string& method, const std::string& url,
                 const std::string& body,
                 const std::vector<std::pair<std::string, std::string>>& headers, size_t, int) {
    ++http_requests;
    last_method = method;
    last_url = url;
    last_body = body;
    last_authorization.clear();
    last_content_type.clear();
    for (const auto& header : headers) {
        if (header.first == "Authorization")
            last_authorization = header.second;
        if (header.first == "Content-Type")
            last_content_type = header.second;
    }
    if (renew_token && url.find("/auth/refresh?") != std::string::npos)
        return {200, {}, "renewed-token", 0};
    if (renew_token && last_authorization == "Bearer renewed-token")
        return {204, {}, {}, 0};
    if (url.rfind("https://images.example/", 0) == 0)
        return {200, {{"content-type", "image/png"}}, image_body, http_error};
    return {http_status, {}, chapter_body, http_error};
}
}
static bool fail_install = false;
static std::string blocked_remove;
extern "C" int __real_remove(const char*);
extern "C" int __wrap_remove(const char* path) {
    if (path == blocked_remove) {
        errno = EACCES;
        return -1;
    }
    return __real_remove(path);
}
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* source, const char* destination) {
    // Reproduce FatFS's refusal to overwrite an existing destination.
    if (access(destination, F_OK) == 0) { errno = EEXIST; return -1; }
    const std::string path = source;
    if (path.find("/work/") != std::string::npos)
        ++scratch_commits;
    if (fail_install && path.size() >= 4 && path.substr(path.size() - 4) == ".tmp") {
        errno = EIO; return -1;
    }
    return __real_rename(source, destination);
}
int main(int argc, char** argv) {
    assert(argc == 3);
    const std::string path = argv[1];
    novelia::Service service;
    assert(service.Save(path, "one"));
    assert(service.Save(path, "two"));
    assert(service.Read(path) == "two");
    fail_install = true;
    assert(!service.Save(path, "broken"));
    assert(service.Read(path) == "two");
    fail_install = false;
    assert(rename(path.c_str(), (path + ".previous").c_str()) == 0);
    assert(service.Read(path) == "two");
    assert(service.Save(path, "three"));
    assert(service.Read(path) == "three");
    assert(access((path + ".previous").c_str(), F_OK) != 0);
    int network_attempts = 0;
    service.network_ready = [&] { ++network_attempts; return false; };
    const auto snapshot = service.AccountRoot() + "/snapshots/" + sd_file::Key("favorites-page") + ".json";
    assert(service.Save(snapshot, R"({"items":[{"title":"cached page"}]})"));
    service.error = "previous command error";
    auto cached = service.Fetch("/unused", "favorites-page", novelia::FetchPolicy::PreferCache);
    assert(cached.json && cached.error.empty() && network_attempts == 0);
    assert(service.error == "previous command error");
    auto refreshed = service.Fetch("/unused", "favorites-page", novelia::FetchPolicy::Refresh);
    assert(refreshed.json && !refreshed.error.empty() && network_attempts == 1);
    assert(refreshed.error.find(Lang::Strings::NOVELIA_OFFLINE_CACHE) != std::string::npos);
    assert(service.error == "previous command error");
    service.network_ready = [] { return true; };
    chapter_body = R"({"items":[{"title":"fresh page"}]})";
    fail_install = true;
    auto fresh = service.Fetch("/unused", "favorites-page");
    assert(fresh.json && novelia::Encode(fresh.json.get()) == chapter_body);
    assert(fresh.error.empty());
    assert(service.error == "previous command error");
    assert(service.Read(snapshot) == R"({"items":[{"title":"cached page"}]})");
    fail_install = false;
    chapter_body.clear();
    auto node = novelia::Parse(R"({"server":"saved.example","port":443,"proxy_password":"saved","proxy_enabled":true})");
    novelia::Set(node.get(), "action", "configure");
    auto configured = novelia::ApplyForm(node.get(), service);
    assert(configured.ok && configured.message == Lang::Strings::NOVELIA_PROXY_SAVED);
    auto missing = novelia::Parse(R"({"action":"login"})");
    auto login = novelia::ApplyForm(missing.get(), service);
    assert(!login.ok && !login.message.empty());
    auto unknown = novelia::Parse(R"({"action":"unknown"})");
    auto invalid = novelia::ApplyForm(unknown.get(), service);
    assert(!invalid.ok && invalid.message == Lang::Strings::NOVELIA_FORM_INVALID);
    invalid = novelia::ApplyForm(nullptr, service);
    assert(!invalid.ok && invalid.message == Lang::Strings::NOVELIA_FORM_INVALID);
    auto visible = novelia::Parse(service.PublicConfig());
    assert(!cJSON_GetObjectItemCaseSensitive(visible.get(), "proxy_password"));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(visible.get(), "has_proxy_password")));
    assert(service.Init());
    assert(service.Translation() == novelia::kTranslations[0].id);
    assert(service.Prefetch());
    assert(service.SetTranslation("gpt"));
    assert(service.SetPrefetch(false));
    assert(service.Init());
    assert(service.Translation() == "gpt" && !service.Prefetch());
    const auto saved_config = service.Read(service.Root() + "/config.json");
    auto trial = novelia::Parse(R"({"server":"trial.example","port":8443,"proxy_password":"trial","proxy_enabled":true})");
    const auto active_config = service.PublicConfig();
    service.network_ready = [&] {
        assert(service.PublicConfig() == active_config);
        ++network_attempts;
        return false;
    };
    novelia::Set(trial.get(), "action", "test");
    auto tested = novelia::ApplyForm(trial.get(), service);
    assert(!tested.ok); // Network is deliberately unavailable.
    assert(novelia::Text(novelia::Parse(service.PublicConfig()).get(), "server") == "saved.example");
    assert(service.Read(service.Root() + "/config.json") == saved_config);

    service.network_ready = [] { return true; };
    http_error = -EMSGSIZE;
    assert(!service.TestProxy() && service.error == Lang::Strings::NOVELIA_HTTP_RESPONSE_TOO_LARGE);
    Lang::Strings::NOVELIA_HTTP_RESPONSE_TOO_LARGE = "响应过大";
    assert(!service.TestProxy() && service.error == "响应过大");
    http_error = -ECONNRESET;
    assert(!service.TestProxy() && service.error == "Connection failed: " + std::to_string(-ECONNRESET));
    http_error = 0;
    tested = novelia::ApplyForm(trial.get(), service);
    assert(tested.ok && tested.message == Lang::Strings::NOVELIA_PROXY_TEST_OK);
    assert(service.PublicConfig() == active_config);
    assert(service.Read(service.Root() + "/config.json") == saved_config);
    service.network_ready = [&] { ++network_attempts; return false; };

    novelia::Item book{"syosetu/n1", "cached book", "syosetu", "n1", false};
    novelia::Item chapter{"1", "chapter", {}, {}, false};
    const auto directory = service.Root() + "/chapters/" + sd_file::Key(book.id + "/" + chapter.id);
    const auto epub = service.Read(argv[2]);
    assert(!epub.empty() && service.Save(directory + "/sakura.epub", epub));
    assert(service.Save(directory + "/work/image0.png.previous", "interrupted image"));
    assert(service.Save(directory + "/work/chapter.xhtml.tmp", "interrupted body"));
    const int attempts = network_attempts;
    auto downloaded = novelia::Download(service, book, chapter, "sakura");
    assert(downloaded.error.empty() && network_attempts == attempts);
    assert(service.Read(downloaded.path) == epub);
    assert(access((directory + "/work").c_str(), F_OK) != 0);

    service.network_ready = [] { return true; };
    chapter_body = "{\"sakuraParagraphs\":[";
    for (int i = 0; i < 65; ++i) {
        if (i) chapter_body += ',';
        chapter_body += "\"<图片>https://images.example/" + std::to_string(i) + "\"";
    }
    chapter_body += "]}";
    const auto illustrated_body = chapter_body;
    novelia::Item illustrated{"2", "illustrated chapter", {}, {}, false};
    scratch_commits = 0;
    auto illustrated_result = novelia::Download(service, book, illustrated, "sakura");
    assert(illustrated_result.error.empty());
    assert(scratch_commits == 0);
    const auto illustrated_epub = service.Read(illustrated_result.path);
    assert(illustrated_epub.find("image64.png") != std::string::npos);
    assert(access((service.Root() + "/chapters/" + sd_file::Key(book.id + "/2") + "/work").c_str(), F_OK) != 0);
    image_body.clear();
    chapter_body = R"({"sakuraParagraphs":["<图片>https://images.example/empty"]})";
    auto empty_image = novelia::Download(service, book, {"3", "empty image", {}, {}, false}, "sakura");
    assert(empty_image.error.empty());
    const auto content = service.Read(empty_image.path);
    assert(content.find("image0.png") == std::string::npos);
    assert(content.find(Lang::Strings::NOVELIA_IMAGE_PLACEHOLDER) != std::string::npos);

    const int before_shown = http_requests;
    assert(service.Shown(book, illustrated));
    assert(http_requests == before_shown); // 未登录时只保存本地进度。
    const auto reading_record = service.Last(book);
    auto actual_usage = novelia::MeasureCache(service.Root());
    assert(actual_usage.ok && actual_usage.bytes > 0 && actual_usage.files > 0);
    actual_usage = novelia::ClearCache(service.Root());
    assert(actual_usage.ok && actual_usage.bytes == 0 && actual_usage.files == 0);
    assert(service.Read(service.Root() + "/config.json") == saved_config);
    assert(service.Last(book) == reading_record);
    assert(access(illustrated_result.path.c_str(), F_OK) != 0);
    image_body = "image-bytes";
    chapter_body = illustrated_body;
    auto resumed = novelia::Download(service, book, illustrated, "sakura");
    assert(resumed.error.empty() && resumed.path == illustrated_result.path);
    assert(service.Read(resumed.path) == illustrated_epub);
    assert(service.Last(book) == reading_record);

    // 缓存覆盖所有账号及提交残留；配置、阅读位置与非快照文件保留。
    const auto cache_root = std::filesystem::path(path).parent_path().string() + "/cache-fixture";
    auto usage = novelia::MeasureCache(cache_root);
    assert(usage.ok && usage.bytes == 0 && usage.files == 0);
    usage = novelia::ClearCache(cache_root);
    assert(usage.ok && usage.bytes == 0 && usage.files == 0);
    auto write = [&](const std::string& name, const std::string& content) {
        const auto file = std::filesystem::path(cache_root) / name;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream output(file, std::ios::binary);
        output << content;
        assert(output.good());
    };
    write("config.json", "secret");
    write("progress/x.json", "reading position");
    write("accounts/a/preferences.json", "preferences");
    write("accounts/a/snapshots/note.txt", "keep");
    const std::pair<const char*, const char*> cache_files[] = {
        {"accounts/a/snapshots/s1.json", "one"},
        {"accounts/a/snapshots/s2.json", "two"},
        {"accounts/a/snapshots/s3.json.previous", "old"},
        {"accounts/a/snapshots/s3.json.tmp", "temp"},
        {"accounts/b/snapshots/b.json", "second account"},
        {"chapters/k/chapter.epub", "epub"},
        {"chapters/k/chapter.epub.previous", "previous"},
        {"chapters/k/work/junk.tmp", "junk"},
    };
    uint64_t expected = 0;
    for (const auto& file : cache_files) {
        write(file.first, file.second);
        expected += std::string(file.second).size();
    }
    usage = novelia::MeasureCache(cache_root);
    assert(usage.ok && usage.bytes == expected && usage.files == 8);
    blocked_remove = cache_root + "/chapters/k/chapter.epub";
    usage = novelia::ClearCache(cache_root);
    assert(!usage.ok && usage.bytes == 4 && usage.files == 1);
    assert(service.Read(blocked_remove) == "epub");
    blocked_remove.clear();
    usage = novelia::ClearCache(cache_root);
    assert(usage.ok && usage.bytes == 0 && usage.files == 0);
    usage = novelia::MeasureCache(cache_root);
    assert(usage.ok && usage.bytes == 0 && usage.files == 0);
    assert(service.Read(cache_root + "/config.json") == "secret");
    assert(service.Read(cache_root + "/progress/x.json") == "reading position");
    assert(service.Read(cache_root + "/accounts/a/preferences.json") == "preferences");
    assert(service.Read(cache_root + "/accounts/a/snapshots/note.txt") == "keep");

    assert(service.Save(service.Root() + "/config.json", R"({"username":"reader","token":"test-token"})"));
    assert(service.Init());
    http_status = 204;
    service.network_ready = []() -> bool { assert(false && "background sync must not wait for Wi-Fi"); return false; };
    service.network_present = [] { return true; };
    assert(service.Shown(book, illustrated));
    assert(service.error.empty());
    assert(last_method == "PUT");
    assert(last_url == "https://n.novelia.cc/api/user/read-history/syosetu/n1");
    assert(last_body == illustrated.id && last_authorization == "Bearer test-token");
    assert(last_content_type == "text/plain;charset=UTF-8");
    assert(novelia::Text(novelia::Parse(service.Last(book)).get(), "chapter") == illustrated.id);

    http_error = -ECONNRESET;
    const novelia::Item next_chapter{"3", "next chapter", {}, {}, false};
    assert(!service.Shown(book, next_chapter));
    assert(!service.error.empty());
    assert(novelia::Text(novelia::Parse(service.Last(book)).get(), "chapter") == next_chapter.id);
    http_error = 0;
    http_status = 401;
    assert(!service.Shown(book, illustrated));
    assert(service.error == Lang::Strings::NOVELIA_LOGIN_EXPIRED);
    assert(service.Save(service.Root() + "/config.json",
                        R"({"username":"reader","token":"test-token","refresh_token":"refresh"})"));
    assert(service.Init());
    renew_token = true;
    assert(service.Shown(book, illustrated));
    assert(last_method == "PUT" && last_body == illustrated.id);
    assert(last_authorization == "Bearer renewed-token");
    assert(last_content_type == "text/plain;charset=UTF-8");
    renew_token = false;
    service.network_present = [] { return false; };
    const int before_offline = http_requests;
    assert(service.Shown(book, next_chapter));
    assert(service.error.empty());
    assert(http_requests == before_offline);
    assert(novelia::Text(novelia::Parse(service.Last(book)).get(), "chapter") == next_chapter.id);

}
