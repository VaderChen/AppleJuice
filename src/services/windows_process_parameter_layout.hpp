#pragma once
#include <array>
#include <cstdint>

namespace x64arm::detail {
// Windows 10 RTL_USER_PROCESS_PARAMETERS；欄位以客體 ABI 表示，不使用宿主結構大小。
struct ProcessParameterLayout {
    unsigned unit,header,environment,environment_size,console_flags;
    // CurrentDirectory、DllPath、ImagePathName、CommandLine、WindowTitle、DesktopInfo、ShellInfo、RuntimeData、RedirectionDllName。
    std::array<unsigned,9> strings;
    static constexpr ProcessParameterLayout for_x86(bool x86) {
        return x86?ProcessParameterLayout{4,0x2c0,0x48,0x290,0x14,{0x24,0x30,0x38,0x40,0x70,0x78,0x80,0x88,0x2a4}}:
                   ProcessParameterLayout{8,0x440,0x80,0x3f0,0x18,{0x38,0x50,0x60,0x70,0xb0,0xc0,0xd0,0xe0,0x410}};
    }
};
}
