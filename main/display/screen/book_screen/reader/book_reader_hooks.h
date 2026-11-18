#pragma once
#include "reader/reader_types.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// host（宿主）指提供书籍文件的外部应用；UI 任务拥有其打开策略与回调。
// 换书或退出宿主时释放；阅读 worker 只接收 StartAt 的值。
enum class StartAt { Saved, First, Last };
// 宿主的一本书的目录快照，各章节共享；读到哪一项见 HostToc。
struct ReaderToc {
    struct Entry {
        std::string title;
        bool selectable = true; // 卷标题等分组条目不可跳转。
    };
    std::string title;
    std::vector<Entry> entries;
};
// 宿主目录的当前位置与跳转方式：快照、当前项、打开回调一起给出，缺一即无效。
struct HostToc {
    std::shared_ptr<const ReaderToc> toc;
    // 下标对应宿主自己的章节列表，阅读请求存活期间该映射须保持不变。
    int index = -1;
    std::function<bool(int)> open; // UI 任务调用；true 表示接受异步打开请求。
    bool Valid() const {
        return toc && index >= 0 && index < static_cast<int>(toc->entries.size()) &&
               toc->entries[index].selectable;
    }
    // 跳过不可跳转条目取后一项（direction=1）或前一项（-1）；到边界返回 -1。
    int Adjacent(int direction) const {
        for (int i = index + direction; toc && i >= 0 && i < static_cast<int>(toc->entries.size());
             i += direction)
            if (toc->entries[i].selectable) return i;
        return -1;
    }
    // 宿主可能在回调中替换当前 launch，调用期间独立持有回调。
    bool Select(int target) const {
        if (!toc || target < 0 || target >= static_cast<int>(toc->entries.size()) ||
            !toc->entries[target].selectable)
            return false;
        auto select = open;
        return select && select(target);
    }
};
struct ReaderLaunch {
    reader::BookInfo book;
    StartAt start = StartAt::Saved;
    std::optional<HostToc> host_toc; // 为空时目录来自文档本身。
    std::function<bool(int)> adjacent; // -1 前一项，+1 后一项；true 表示已处理
    std::function<void()> opened;
    std::function<void()> returning;
    std::function<void()> suspending;
};
