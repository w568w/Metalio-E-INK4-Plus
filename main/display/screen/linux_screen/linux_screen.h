#pragma once
#include <lvgl.h>

class LinuxScreen {
public:
    static lv_obj_t* Create();
    static void SetStandby(bool paused);
};
