#pragma once

#include <cstddef>
#include <cstdint>

struct MiniRV32IMAState;

namespace linux_vm {
constexpr size_t kRamBytes = 12 * 1024 * 1024;
// 顶部 16 KiB 是设备树保留区；DTS 的 memory.reg 同步排除该区，防止 Linux 覆盖启动参数。
constexpr size_t kDtbOffset = kRamBytes - 16 * 1024;

constexpr int kWaitForInterrupt = 1;
constexpr int kPowerOff = 0x5555;
constexpr int kRestart = 0x7777;

// 内存及串口归调用方所有；单个虚拟机只在其执行任务内访问。
class Machine {
public:
    struct Io {
        void* context;
        void (*write)(void*, char);
        int (*read)(void*, bool consume); // 无输入返回 -1。
    };
    Machine(uint8_t* ram, Io io);
    // ram[0..image_bytes) 已装入裸 Image；其余虚拟内存清零。
    bool Boot(size_t image_bytes);
    // 返回 mini-rv32ima 的 WFI(1)、重启(0x7777)、关机(0x5555) 或正常(0)。
    int Step(uint32_t elapsed_us, int instructions);
    uint64_t Cycles() const;
private:
    uint32_t Load(uint32_t address);
    uint32_t Store(uint32_t address, uint32_t value);
    uint8_t* ram_;
    Io io_;
    uint8_t uart_lcr_ = 0;
    int32_t Execute(MiniRV32IMAState*, uint8_t*, uint32_t, uint32_t, int);
    // 核心状态独立于虚拟内存，不向虚拟机暴露宿主指针。
    alignas(8) uint8_t core_[192]{};
};
}
