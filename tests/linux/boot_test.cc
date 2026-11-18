#include "linux_vm.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

struct Console {
    std::string output;
    std::string input;
    size_t cursor = 0;
};
int main(int argc, char** argv)
{
    if (argc != 2) { std::fputs("Usage: linux_boot_test /path/to/Image\n", stderr); return 2; }
    std::ifstream image(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(image), {}};
    if (bytes.empty()) {
        std::fputs("Linux image missing or empty. Run python tests/linux/prepare.py, then pass build/linux-assets/metalio/e-ink/linux/Image.\n", stderr);
        return 2;
    }
    std::vector<uint8_t> ram(linux_vm::kRamBytes);
    assert(bytes.size() < ram.size());
    std::copy(bytes.begin(), bytes.end(), ram.begin());
    Console console;
    linux_vm::Machine vm(ram.data(), {
        &console,
        [](void* p, char c) { static_cast<Console*>(p)->output += c; },
        [](void* p, bool consume) {
            auto& c = *static_cast<Console*>(p);
            if (c.cursor == c.input.size()) return -1;
            int value = static_cast<unsigned char>(c.input[c.cursor]);
            if (consume) ++c.cursor;
            return value;
        }
    });
    assert(!vm.Boot(0));
    assert(!vm.Boot(linux_vm::kDtbOffset + 1));
    assert(vm.Boot(bytes.size()));
    assert(vm.Cycles() == 0);
    bool sent = false;
    bool login = false;
    bool shutdown = false;
    for (int batch = 0; batch < 300000; ++batch) {
    
        uint64_t before = vm.Cycles();
        int result = vm.Step(100, 4096);
        assert(vm.Cycles() >= before);
        assert(vm.Cycles() - before <= 4096);
        if (result == linux_vm::kPowerOff) {
            assert(shutdown);
            assert(vm.Cycles() > 0);
            std::puts(console.output.c_str());
            std::puts("PASS: Linux boot, login, shell arithmetic, uname and poweroff");
            return 0;
        }
        if (!login && console.output.find("login: ") != std::string::npos) {
            console.input = "root\n";
            login = true;
        }
        if (!sent && console.output.find("# ") != std::string::npos) {
            console.input += "echo METALIO_VM_$((6*7)); uname -r\n";
            sent = true;
        }
        if (!shutdown && console.output.find("METALIO_VM_42") != std::string::npos &&
            console.output.find("\n6.1.14") != std::string::npos) {
            console.input += "poweroff -f\n";
            shutdown = true;
        }
        if (console.output.find("Kernel panic") != std::string::npos) break;
    }
    std::fputs(console.output.c_str(), stderr);
    return 1;
}
