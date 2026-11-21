#pragma once
#include "x64arm/windows_runtime.hpp"
#include <bit>
#include <span>
#include <string_view>

namespace x64arm::detail {
// 由提供者宣告模組內的唯讀資料容量；PE 檔案、映像大小與實際映射共用此配置。
// 介面表屬於提供者映像，不借用程序 heap 或以擴大模組查詢範圍涵蓋外部配置。
inline constexpr std::size_t builtin_module_data_capacity(std::string_view module) noexcept {
    if(module=="icu.dll")return 65536U; // ICU 的唯讀字元屬性及書寫系統名稱。
    return module=="d3d12.dll" || module=="dxgi.dll"?16384U:0U;
}
// 只在協調者、客體 worker 已停止時初始化未發布的資料；發布後維持唯讀。
// 呼叫端共用自己的介面表快取，避免重複配置；此處不改寫已發布資料。
inline std::uint64_t initialize_builtin_module_data(GuestMemory& memory,RuntimeImage& image,
        std::span<const std::uint8_t> bytes,std::size_t alignment=8){
    if(!image.provider_data_size || !std::has_single_bit(alignment) || alignment>16)throw std::invalid_argument("內建模組唯讀資料配置無效");
    const auto begin=(std::size_t{image.provider_data_used}+alignment-1)&~(alignment-1);
    if(begin>image.provider_data_size || bytes.size()>image.provider_data_size-begin)throw std::bad_alloc();
    const auto address=image.base+image.provider_data_rva+begin;
    memory.initialize_readonly(GuestAddress{address},bytes);
    image.provider_data_used=static_cast<std::uint32_t>(begin+bytes.size());return address;
}
}
