#include "web_novel_cache.h"
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace web_novel {
namespace {
template <typename Visitor>
void Entries(const std::string& path, CacheUsage& usage, Visitor visit) {
    DIR* directory = opendir(path.c_str());
    if (!directory) {
        if (errno != ENOENT)
            usage.ok = false;
        return;
    }
    for (;;) {
        errno = 0;
        auto* entry = readdir(directory);
        if (!entry) {
            if (errno)
                usage.ok = false;
            break;
        }
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        const auto child = path + "/" + name;
        struct stat info{};
        if (stat(child.c_str(), &info) != 0)
            usage.ok = false;
        else
            visit(child, info);
    }
    closedir(directory);
}
void File(const std::string& path, const struct stat& info, bool clear, CacheUsage& usage) {
    if (!S_ISREG(info.st_mode)) {
        usage.ok = false;
        return;
    }
    if (clear) {
        if (std::remove(path.c_str()) == 0 || errno == ENOENT)
            return;
        usage.ok = false;
    }
    usage.bytes += info.st_size;
    ++usage.files;
}
void RemoveEmpty(const std::string& path, CacheUsage& usage) {
    if (rmdir(path.c_str()) != 0 && errno != ENOENT && errno != ENOTEMPTY)
        usage.ok = false;
}
CacheUsage VisitCache(const std::string& root, bool clear) {
    CacheUsage usage;
    Entries(root + "/chapters", usage, [&](const auto& chapter, const auto& info) {
        if (!S_ISDIR(info.st_mode)) {
            File(chapter, info, clear, usage);
            return;
        }
        Entries(chapter, usage, [&](const auto& path, const auto& info) {
            if (path == chapter + "/work" && S_ISDIR(info.st_mode)) {
                Entries(path, usage, [&](const auto& file, const auto& info) {
                    File(file, info, clear, usage);
                });
                if (clear)
                    RemoveEmpty(path, usage);
            } else
                File(path, info, clear, usage);
        });
        if (clear)
            RemoveEmpty(chapter, usage);
    });
    Entries(root + "/accounts", usage, [&](const auto& account, const auto& info) {
        if (!S_ISDIR(info.st_mode))
            return;
        const auto snapshots = account + "/snapshots";
        Entries(snapshots, usage, [&](const auto& path, const auto& info) {
            // 快照的提交与回退副本也属于缓存，避免删除后 Read() 又恢复旧数据。
            const auto suffix = path.rfind(".json");
            if (suffix != std::string::npos) {
                const auto ending = path.substr(suffix);
                if (ending == ".json" || ending == ".json.tmp" || ending == ".json.previous")
                    File(path, info, clear, usage);
            }
        });
        if (clear)
            RemoveEmpty(snapshots, usage);
    });
    return usage;
}
} // namespace
CacheUsage MeasureCache(const std::string& root) {
    return VisitCache(root, false);
}
CacheUsage ClearCache(const std::string& root) {
    return VisitCache(root, true);
}
} // namespace web_novel
