#pragma once
#include <lvgl.h>
#include <string>

class WebNovelScreen {
public:
    static lv_obj_t* CreateNovelia();
    static lv_obj_t* CreateNovalpie();
    static lv_obj_t* RestoreReading(const std::string& path, const std::string& context);
};
