#pragma once
#include <cstdint>
#include <string>

namespace web_novel {
struct CacheUsage {
    uint64_t bytes = 0;
    int files = 0;
    bool ok = true;
};
// 只遍历章节缓存与所有账号的快照；目录不存在按空缓存处理。
CacheUsage MeasureCache(const std::string& root);
// 单次遍历删除缓存，返回未能删除的文件占用；配置与阅读记录不在遍历范围内。
CacheUsage ClearCache(const std::string& root);
} // namespace web_novel
