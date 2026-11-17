#pragma once
#include <cstdlib>
// 路径测试不覆盖后台清理；误入该路径时必须失败，不能静默模拟调度成功。
static inline void vTaskDelay(unsigned) { std::abort(); }
