#include "linux_console.h"
#include <cassert>
#include <string>

static std::string At(const linux_vm::Console& console, size_t rows, size_t back)
{
    linux_vm::Console::Cursor cursor{back, 0};
    cursor = console.Advance(rows, cursor);
    return console.Window(rows, cursor);
}
static linux_vm::Console::Layout Fixed(size_t width)
{
    linux_vm::Console::Layout layout{};
    layout.width = width;
    layout.advance.fill(1);
    return layout;
}
int main()
{
    linux_vm::Console console(Fixed(40));
    console.Append("hello\r\nworld");
    assert(At(console, 1, 0) == "world");
    assert(At(console, 2, 0) == "hello\nworld");
    console.Append("\b!\x1b[");
    console.Append("31mred\x1b[0m");
    assert(At(console, 1, 0) == "worl!red");
    console.Append("\n" + std::string(100, 'x'));
    assert(At(console, 1, 0) == std::string(20, 'x'));
    for (int i = 0; i < 200; ++i) console.Append("\nline" + std::to_string(i));
    assert(console.MaxBack(1) == linux_vm::Console::kHistoryLines - 1);
    assert(At(console, 1, 0) == "line199");
    assert(At(console, 1, 999) == "line72");
    assert(At(console, 2, 1) == "line197\nline198");
    linux_vm::Console narrow(Fixed(3));
    narrow.Append("abcdefg");
    assert(At(narrow, 3, 0) == "abc\ndef\ng");
    linux_vm::Console history(Fixed(80));
    for (int i = 0; i < 160; ++i) history.Append("line" + std::to_string(i) + "\n");
    linux_vm::Console::Cursor cursor;
    history.Window(3, cursor);
    history.Scroll(3, -1, cursor);
    auto anchored = history.Window(3, cursor);
    auto original = cursor;
    history.Window(3, cursor);
    assert(cursor.back == original.back && cursor.seen == original.seen);
    history.Append("more1\nmore2\nmore3\n");
    assert(history.MaxBack(1) == linux_vm::Console::kHistoryLines - 1);
    cursor = history.Advance(3, cursor);
    assert(history.Window(3, cursor) == anchored); // 环形缓冲饱和后仍固定同一窗口。
    assert(cursor.back == 6);
    history.Scroll(3, 1, cursor);
    assert(cursor.back == 3);
    assert(history.Scroll(3, 1, cursor) == linux_vm::Console::Boundary::Latest);
    history.Append("latest\n");
    cursor = history.Advance(2, cursor);
    assert(history.Window(2, cursor) == "latest\n");
    assert(cursor.back == 0);
    for (int i = 0; i < 100; ++i) history.Scroll(3, -1, cursor);
    assert(cursor.back == history.MaxBack(3));
    auto layout = Fixed(10);
    layout.advance['W'] = 3;
    linux_vm::Console proportional(layout);
    proportional.Append("iiiiiiiiiiW");
    assert(At(proportional, 2, 0) == "iiiiiiiiii\nW");
    proportional.Append("WWi");
    assert(At(proportional, 1, 0) == "WWWi");
    proportional.Append("\bi");
    assert(At(proportional, 1, 0) == "WWWi");

}
