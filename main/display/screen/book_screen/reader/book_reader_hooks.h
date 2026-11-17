#pragma once
#include "reader/reader_types.h"
#include <functional>

// host（宿主）指提供书籍文件的外部应用；UI 任务拥有其打开策略与回调。
// 换书或退出宿主时释放；阅读 worker 只接收 StartAt 的值。
enum class StartAt { Saved, First, Last };
struct ReaderLaunch {
    reader::BookInfo book;
    StartAt start = StartAt::Saved;
    bool show_toc = true;
    std::function<bool(int)> adjacent; // -1 前一项，+1 后一项；true 表示已处理
    std::function<void()> opened;
    std::function<void()> returning;
    std::function<void()> suspending;
};
