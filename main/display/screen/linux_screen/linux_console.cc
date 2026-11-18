#include "linux_console.h"
#include <algorithm>

namespace linux_vm {
void Console::NewLine()
{
    lines_.emplace_back();
    line_width_ = 0;
    ++total_lines_;
    if (lines_.size() > kHistoryLines) lines_.pop_front();
}
void Console::Append(const std::string& bytes)
{
    for (unsigned char c : bytes) {
        if (escape_ == Escape::Start) {
            escape_ = c == '[' ? Escape::Csi : Escape::None;
            continue;
        }
        if (escape_ == Escape::Csi) {
            if (c >= 0x40 && c <= 0x7e) escape_ = Escape::None;
            continue;
        }
        if (c == 0x1b) { escape_ = Escape::Start; continue; }
        if (c == '\n') NewLine();
        else if (c == '\b' || c == 127) {
            if (!lines_.back().empty()) {
                auto previous = static_cast<unsigned char>(lines_.back().back());
                line_width_ -= layout_.advance[previous];
                lines_.back().pop_back();
            }
        } else if (c == '\t' || c >= 32) {
            if (c == '\t') c = ' ';
            if (!lines_.back().empty() && line_width_ + layout_.advance[c] > layout_.width) NewLine();
            lines_.back() += c;
            line_width_ += layout_.advance[c];
        }
    }
}
size_t Console::MaxBack(size_t rows) const
{
    return lines_.size() > rows ? lines_.size() - rows : 0;
}
Console::Cursor Console::Advance(size_t rows, Cursor cursor) const
{
    // 累计行号在历史环形缓冲饱和后仍增长；仅尾随模式跟随新输出。
    if (cursor.back && cursor.seen) cursor.back += total_lines_ - cursor.seen;
    cursor.seen = total_lines_;
    cursor.back = std::min(cursor.back, MaxBack(rows));
    return cursor;
}
Console::Boundary Console::Scroll(size_t rows, int direction, Cursor& cursor) const
{
    cursor = Advance(rows, cursor);
    if (direction < 0) cursor.back = std::min(cursor.back + rows, MaxBack(rows));
    else cursor.back = cursor.back > rows ? cursor.back - rows : 0;
    if (direction < 0 && cursor.back == MaxBack(rows)) return Boundary::Oldest;
    if (direction > 0 && cursor.back == 0) return Boundary::Latest;
    return Boundary::None;
}
std::string Console::Window(size_t rows, const Cursor& cursor) const
{
    rows = std::max<size_t>(1, rows);
    size_t back = std::min(cursor.back, MaxBack(rows));
    size_t end = lines_.size() - back;
    size_t begin = end > rows ? end - rows : 0;
    std::string text;
    for (size_t i = begin; i < end; ++i) {
        if (i > begin) text += '\n';
        text += lines_[i];
    }
    return text;
}
}
