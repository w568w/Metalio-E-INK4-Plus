#include "sd_file.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

static std::string fault, source, target, backup;
static bool fail_restore = false;
extern "C" {
int __real_rename(const char*, const char*);
int __real_unlink(const char*);
size_t __real_fwrite(const void*, size_t, size_t, FILE*);
int __real_fflush(FILE*);
int __real_fsync(int);
int __real_fclose(FILE*);
int __wrap_rename(const char* from, const char* to) {
    if (fail_restore && from == backup) { errno = EACCES; return -1; }
    if ((fault == "install_temp" && from == source) ||
        (fault == "backup_previous" && from == target)) { errno = EIO; return -1; }
    // Linux rename 可以覆盖目标；在此重现 FatFS 拒绝覆盖的行为。
    if (access(to, F_OK) == 0) { errno = EEXIST; return -1; }
    return __real_rename(from, to);
}
int __wrap_unlink(const char* path) {
    if (fault == "remove_old_backup" && path == backup) { errno = EACCES; return -1; }
    return __real_unlink(path);
}
size_t __wrap_fwrite(const void* p, size_t size, size_t count, FILE* file) {
    if (fault == "write_temp") { errno = ENOSPC; return 0; }
    return __real_fwrite(p, size, count, file);
}
int __wrap_fflush(FILE* file) {
    if (fault == "flush_temp") { errno = ENOSPC; return EOF; }
    return __real_fflush(file);
}
int __wrap_fsync(int fd) {
    if (fault == "sync_temp") { errno = EIO; return -1; }
    return __real_fsync(fd);
}
int __wrap_fclose(FILE* file) {
    const int result = __real_fclose(file);
    if (fault == "close_temp") { errno = EIO; return EOF; }
    return result;
}
}
static std::string Read(const std::string& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}
int main(int argc, char** argv) {
    assert(argc == 2);
    target = std::string(argv[1]) + "/replace";
    source = target + ".tmp";
    backup = target + ".bak";
    assert(sd_file::WriteFile(target, backup, "old", 3).Ok());
    for (const char* stage : {"write_temp", "flush_temp", "sync_temp", "close_temp",
                              "remove_old_backup", "backup_previous", "install_temp"}) {
        fault = stage;
        auto result = sd_file::WriteFile(target, backup, "new", 3);
        const int expected = (fault == "write_temp" || fault == "flush_temp") ? ENOSPC
                             : fault == "remove_old_backup" ? EACCES : EIO;
        assert(!result.Ok() && result.stage == fault && result.error == expected && !result.rollback_error);
        assert(Read(target) == "old" && access(source.c_str(), F_OK) != 0);
    }
    fault.clear();
    assert(sd_file::WriteFile(target, backup, "new", 3).Ok());
    assert(Read(target) == "new" && Read(backup) == "old");
    // 提交成功与备份清理独立，清理失败不改变已安装的内容。
    fault = "remove_old_backup";
    assert(unlink(backup.c_str()) != 0 && Read(target) == "new");
    fault.clear();
    assert(unlink(backup.c_str()) == 0);
    assert(sd_file::WriteFile(source, source + ".bak", "move", 4).Ok());
    fault = "install_temp";
    auto result = sd_file::ReplaceFile(source, target, backup);
    assert(!result.Ok() && result.error == EIO && !result.rollback_error);
    assert(Read(source) == "move" && Read(target) == "new");
    fail_restore = true;
    result = sd_file::ReplaceFile(source, target, backup);
    assert(!result.Ok() && result.error == EIO && result.rollback_error == EACCES);
    assert(Read(source) == "move" && Read(backup) == "new" && access(target.c_str(), F_OK) != 0);
    // 旧副本尚在 backup 时，也能提交新文件并保留它供业务层恢复或清理。
    fault.clear();
    fail_restore = false;
    assert(sd_file::ReplaceFile(source, target, backup).Ok());
    assert(Read(target) == "move" && Read(backup) == "new");
    result = sd_file::ReplaceFile(source, argv[1], backup);
    assert(!result.Ok() && std::string(result.stage) == "destination_not_regular_file");
    puts("PASS: file commit, write/flush/sync/close/rename faults, rollback errors and retained backups");
}
