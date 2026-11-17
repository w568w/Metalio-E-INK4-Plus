#pragma once
#include "reader/book_reader_hooks.h"

// 为提供 SD 卡书籍文件的应用接入原生阅读器；仅限 UI 任务调用。
void BookExternalOpen(ReaderLaunch request);
