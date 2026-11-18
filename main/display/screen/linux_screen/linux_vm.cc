#include "linux_vm.h"
#include "linux_dtb.h"

#include <cstring>
#include <new>

#define MINI_RV32_RAM_SIZE linux_vm::kRamBytes
#define MINIRV32_IMPLEMENTATION
// 上游扩展钩子在成员函数内展开，串口和定时器只触及当前 Machine。
#define MINIRV32_STEPPROTO int32_t linux_vm::Machine::Execute(MiniRV32IMAState* state, uint8_t* image, uint32_t vProcAddress, uint32_t elapsedUs, int count)
#define MINIRV32_HANDLE_MEM_STORE_CONTROL(address, value) { if (auto result = Store(address, value)) return result; }
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(address, value) value = Load(address);
#include "vendor/mini-rv32ima.h"

namespace linux_vm {
static_assert(sizeof(MiniRV32IMAState) <= 192);

Machine::Machine(uint8_t* ram, Io io) : ram_(ram), io_(io) {}

bool Machine::Boot(size_t image_bytes)
{
    if (!ram_ || !image_bytes || image_bytes > kDtbOffset) return false;
    std::memset(ram_ + image_bytes, 0, kRamBytes - image_bytes);
    std::memcpy(ram_ + kDtbOffset, kDtb, sizeof(kDtb));
    uart_lcr_ = 0;
    auto* core = new (core_) MiniRV32IMAState{};
    core->pc = 0x80000000;
    core->regs[11] = 0x80000000 + kDtbOffset;
    core->extraflags = 3;
    return true;
}

int Machine::Step(uint32_t elapsed_us, int instructions)
{
    return Execute(reinterpret_cast<MiniRV32IMAState*>(core_), ram_, 0, elapsed_us, instructions);
}

uint64_t Machine::Cycles() const
{
    auto* core = reinterpret_cast<const MiniRV32IMAState*>(core_);
    return (uint64_t(core->cycleh) << 32) | core->cyclel;
}

uint32_t Machine::Store(uint32_t address, uint32_t value)
{
    auto* core = reinterpret_cast<MiniRV32IMAState*>(core_);
    if (address == 0x10000000 && !(uart_lcr_ & 0x80)) io_.write(io_.context, static_cast<char>(value));
    else if (address == 0x10000003) uart_lcr_ = value;
    else if (address == 0x11004000) core->timermatchl = value;
    else if (address == 0x11004004) core->timermatchh = value;
    else if (address == 0x11100000) return value;
    return 0;
}

uint32_t Machine::Load(uint32_t address)
{
    auto* core = reinterpret_cast<MiniRV32IMAState*>(core_);
    if (address == 0x10000005) return 0x60 | (io_.read(io_.context, false) >= 0);
    if (address == 0x10000000) {
        int value = io_.read(io_.context, true);
        return value >= 0 ? value : 0;
    }
    if (address == 0x1100bffc) return core->timerh;
    if (address == 0x1100bff8) return core->timerl;
    return 0;
}
}
