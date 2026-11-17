#pragma once
#include "novelia_model.h"
#include <string>
#include <vector>
namespace novelia {
class Service;
struct Chapter {
    std::string path, title, error;
    std::vector<std::string> available;
};
Chapter Download(Service& service, const Item& novel, const Item& chapter, const std::string& mode);
} // namespace novelia
