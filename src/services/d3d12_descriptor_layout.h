#pragma once

#define X64ARM_D3D12_DESCRIPTOR_ARENA_HANDLE 0x0000600000000000
#define X64ARM_D3D12_DESCRIPTOR_ARENA_HANDLES 0x10000000
#define X64ARM_D3D12_DESCRIPTOR_STRIDE 32
#define X64ARM_D3D12_DESCRIPTOR_WORD_SHIFT 2
#define X64ARM_D3D12_DESCRIPTOR_DEVICE_ARENA 8

#ifdef __cplusplus
#include <cstdlib>
#include <string_view>

namespace x64arm::detail {
// Win64 常駐描述元入口（建立 CBV、複製單一數值槽）：與數值槽映射同開，2026-11-08《暗影》2 vCPU +4%、尾端改善，
// 預設開啟。X64ARM_RESIDENT_D3D12_DESCRIPTORS=0 關閉。
inline bool d3d12_descriptor_lowering_enabled() noexcept {
    static const bool enabled=[] {
        const auto* value=std::getenv("X64ARM_RESIDENT_D3D12_DESCRIPTORS");
        return !value || std::string_view(value)!="0";
    }();
    return enabled;
}
}
#endif
