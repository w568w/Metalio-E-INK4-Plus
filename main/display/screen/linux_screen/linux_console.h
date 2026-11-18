#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace linux_vm {
constexpr size_t kInputBytes = 256;

// 行式控制台：限定历史行数与单行长度，窗口不会从行中间开始。
class Console {
public:
    static constexpr size_t kHistoryLines = 128;
    struct Layout {
        size_t width;
        // 按输出字节索引，仅适用单字节文本，不解码 UTF-8。
        std::array<uint16_t, 256> advance;
    };
    explicit Console(const Layout& layout) : layout_(layout) {}
    void Append(const std::string& bytes);
    struct Cursor { size_t back = 0; uint64_t seen = 0; };
    enum class Boundary { None, Oldest, Latest };
    size_t MaxBack(size_t rows) const;
    Cursor Advance(size_t rows, Cursor cursor) const;
    std::string Window(size_t rows, const Cursor& cursor) const;
    // 翻页同时更新游标并返回边界反馈；Window 仅取值，Advance 仅返回新游标。
    Boundary Scroll(size_t rows, int direction, Cursor& cursor) const;
private:
    const Layout layout_;
    size_t line_width_ = 0;
    std::deque<std::string> lines_{1};
    uint64_t total_lines_ = 1;
    enum class Escape { None, Start, Csi } escape_ = Escape::None;
    void NewLine();
};
}
