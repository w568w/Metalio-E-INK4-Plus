#pragma once
#include <string>
#include <vector>
namespace web_novel {
struct EpubImage {
    std::string name, mime, path;
};
bool WriteEpub(const std::string& path, const std::string& title, const std::string& author,
               const std::string& xhtml_path, const std::vector<EpubImage>& images);
} // namespace web_novel
