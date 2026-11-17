#include "sd_file.h"
#include "sd_paths.h"
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>
#include <mbedtls/md.h>

namespace sd_file {
std::string Key(const std::string& value) {
    unsigned char digest[32];
    const auto* algorithm = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!algorithm ||
        mbedtls_md(algorithm, reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest)) {
        ESP_LOGE("SdFile", "SHA-256 path key failed");
        return {};
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string key(64, '0');
    for (size_t i = 0; i < sizeof(digest); ++i) {
        key[2 * i] = hex[digest[i] >> 4];
        key[2 * i + 1] = hex[digest[i] & 15];
    }
    return key;
}
bool EnsurePath(const std::string& directory) {
    for (size_t i = 1; i <= directory.size(); ++i) {
        if (i != directory.size() && directory[i] != '/')
            continue;
        if (!SdEnsureDir(directory.substr(0, i).c_str()))
            return false;
    }
    return !directory.empty();
}
Result ReplaceFile(const std::string& source, const std::string& target, const std::string& backup) {
    struct stat info{};
    const bool existed = stat(target.c_str(), &info) == 0;
    if (!existed && errno != ENOENT)
        return {"stat", errno};
    if (existed && !S_ISREG(info.st_mode))
        return {"destination_not_regular_file", EINVAL};
    if (existed) {
        if (unlink(backup.c_str()) != 0 && errno != ENOENT)
            return {"remove_old_backup", errno};
        if (rename(target.c_str(), backup.c_str()) != 0)
            return {"backup_previous", errno};
    }
    if (rename(source.c_str(), target.c_str()) == 0)
        return {};
    Result result{"install_temp", errno};
    if (existed && rename(backup.c_str(), target.c_str()) != 0)
        result.rollback_error = errno;
    return result;
}
Result WriteFile(const std::string& target, const std::string& backup, const void* data, size_t size) {
    const auto temp = target + ".tmp";
    if (!EnsurePath(target.substr(0, target.find_last_of('/'))))
        return {"create_directory", errno};
    FILE* file = fopen(temp.c_str(), "wb");
    if (!file)
        return {"open_temp", errno};
    Result result;
    if (fwrite(data, 1, size, file) != size)
        result = {"write_temp", errno};
    else if (fflush(file) != 0)
        result = {"flush_temp", errno};
    else if (fsync(fileno(file)) != 0)
        result = {"sync_temp", errno};
    const int closed = fclose(file);
    if (closed != 0 && result.Ok())
        result = {"close_temp", errno};
    if (result.Ok())
        result = ReplaceFile(temp, target, backup);
    if (!result.Ok())
        remove(temp.c_str());
    return result;
}
} // namespace sd_file
