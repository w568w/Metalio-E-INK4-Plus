#pragma once
#include <lvgl.h>
#include "linux_console.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace linux_ui {
struct Actions {
    std::function<void()> choose, interrupt;
    std::function<bool(const std::string&)> send;
    std::function<void(size_t)> select;
    std::function<void(int)> turn;
};
struct View {
    lv_obj_t* root = nullptr;
    lv_obj_t* status = nullptr;
    Actions actions;
    ~View();
    void Status(const std::string& message);
};
struct TerminalView : View {
    lv_obj_t* terminal = nullptr;
    lv_obj_t* input = nullptr;
    lv_obj_t* keyboard = nullptr;
    size_t Rows() const;
    linux_vm::Console::Layout TextLayout() const;
};
struct ListView : View {
    lv_obj_t* list = nullptr;
    lv_obj_t* number = nullptr;
    lv_obj_t* notice = nullptr;
    int capacity = 0;
    void Items(const std::vector<std::string>& names, int page, int pages, bool busy, const std::string& selected, const std::string& empty_text = {});
};
std::unique_ptr<TerminalView> Terminal(lv_obj_t* parent, const Actions& actions);
std::unique_ptr<ListView> List(lv_obj_t* parent, const Actions& actions, const char* notice = "");
}
