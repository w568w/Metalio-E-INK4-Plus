#pragma once
#include <cstddef>
#include <string>

// sd_access 保护单次 VFS 调用和打开的句柄。需阻止整段操作被挂载切换打断时，
// 调用方持有 Transaction 或等效的生命周期互斥；本层不创建事务。
namespace sd_file {
std::string Key(const std::string& value);
bool EnsurePath(const std::string& directory);
struct Result {
    const char* stage = nullptr; // 失败阶段；空值表示无错误。
    int error = 0; // 失败时的 errno，不受后续关闭、回滚和清理影响。
    int rollback_error = 0;
    bool Ok() const { return stage == nullptr; }
};
// 三个路径互不相同且位于同一文件系统；源文件已关闭、同步，调用方串行化同目标操作。
// 成功后保留备份；失败时尝试恢复目标，并保留源文件供调用方处理。
Result ReplaceFile(const std::string& source, const std::string& target, const std::string& backup);
// 写入 target.tmp 并提交；失败时清理本次临时文件，成功后的备份由调用方清理。
Result WriteFile(const std::string& target, const std::string& backup, const void* data, size_t size);
} // namespace sd_file
