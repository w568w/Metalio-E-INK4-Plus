#include "sd_data_layout.h"
#include "sd_paths.h"
#include "sd_file.h"
#include <cassert>
#include <fstream>
#include <string>

int main() {
    assert(SdEnsureAppLayout());
    const std::string local = SD_PATH_BOOKS "/local.epub";
    auto pos = sd_data::BookPath(local.c_str(), ".pos");
    assert(!pos.empty() && pos.find("/external/") == std::string::npos);
    { std::ofstream file(local + ".pos"); file << "existing progress"; }
    assert(sd_data::BookPath(local.c_str(), ".pos") == local + ".pos");
    const std::string external = SD_APP_ROOT "/novelia/chapters/book/chapter.epub";
    auto external_pos = sd_data::BookPath(external.c_str(), ".pos");
    auto external_cache = sd_data::BookPath(external.c_str(), ".cpg");
    assert(external_pos.find(SD_PATH_BOOK_STATE "/external/") == 0);
    assert(external_cache.find(SD_PATH_BOOK_CACHE "/external/") == 0);
    assert(sd_data::BookPath(external.c_str(), ".pos") == external_pos);
    assert(sd_data::BookPath((external + ".other").c_str(), ".pos") != external_pos);
    assert(sd_data::BookPath(SD_MOUNT_POINT "/../bad.epub", ".pos").empty());
    assert(sd_data::BookPath(SD_MOUNT_POINT "-other/book.epub", ".pos").empty());
    assert(sd_data::BookPath(nullptr, ".pos").empty());
    assert(sd_data::BookPath(SD_MOUNT_POINT "/Download/user.epub", ".pos").empty());
    assert(sd_data::BookPath(SD_APP_ROOT "-other/book.epub", ".pos").empty());
    const auto relative = external.substr(std::string(SD_MOUNT_POINT).size() + 1);
    const auto key = sd_file::Key(relative);
    assert(external_pos == SD_PATH_BOOK_STATE "/external/" + key.substr(0, 1) + "/" + key + ".pos");
    const std::string moved = SD_PATH_BOOKS "/moved.epub";
    { std::ofstream file(moved); file << "book"; }
    { std::ofstream file(moved + ".pos"); file << "old target"; }
    { std::ofstream file(moved + ".pos.previous"); file << "stale backup"; }
    assert(sd_data::Relocate(local.c_str(), moved.c_str()));
    { std::ifstream file(moved + ".pos"); std::string text; std::getline(file, text);
      assert(text == "existing progress"); }
    assert(access((local + ".pos").c_str(), F_OK) != 0);
    assert(access((moved + ".pos.previous").c_str(), F_OK) != 0);
    const std::string recovered = SD_PATH_BOOKS "/recovered.epub";
    { std::ofstream file(recovered); file << "book"; }
    { std::ofstream file(recovered + ".pos.previous"); file << "interrupted replacement"; }
    assert(sd_data::Relocate(moved.c_str(), recovered.c_str()));
    { std::ifstream file(recovered + ".pos"); std::string text; std::getline(file, text);
      assert(text == "existing progress"); }
    assert(access((recovered + ".pos.previous").c_str(), F_OK) != 0);

}
