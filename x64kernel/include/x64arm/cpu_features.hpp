#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace x64arm {
// CPUID、XGETBV、XSAVE 與作業系統能力查詢共用；AVX 高半部是獨立元件 2。
inline constexpr std::uint64_t virtual_xstate_mask=7;
inline constexpr std::uint32_t virtual_ymm_offset=576,virtual_ymm_bytes=256;
inline constexpr std::uint32_t virtual_xsave_bytes=virtual_ymm_offset+virtual_ymm_bytes;
// 身分與頻率由客體擁有者設定，不從宿主 CPU 推導 x86 功能位元。
// CPUID 品牌欄共 48 bytes，最後一個 byte 保留 NUL；頻率單位為 MHz，0 表示未知。
struct VirtualProcessorIdentity {
    std::array<char,48> brand{};
    std::uint32_t base_mhz{},maximum_mhz{},bus_mhz{};
    constexpr VirtualProcessorIdentity(std::string_view name="x64arm virtual AMD64 processor",
                                       std::uint32_t base=0,std::uint32_t maximum=0,std::uint32_t bus=0)
        :base_mhz(base),maximum_mhz(maximum),bus_mhz(bus){
        for(std::size_t i=0;i<name.size() && i+1<brand.size() && name[i];++i)brand[i]=name[i];
    }
    bool operator==(const VirtualProcessorIdentity&)const=default;
};
// 客體的記憶體階層描述供 CPUID 與 Windows 拓樸共用；不宣稱宿主快取容量或效能。
// 每個 vCPU 為獨立核心，L1／L2 私有，L3 由同一封裝內所有 vCPU 共用。
struct VirtualProcessorCache {
    std::uint32_t type,level,line_size,ways,size,sharing;
};
inline constexpr std::array<VirtualProcessorCache,4> virtual_processor_caches(std::uint32_t processors) {
    return {{{1,1,64,8,32*1024,1},{2,1,64,8,32*1024,1},
             {3,2,64,8,256*1024,1},{3,3,64,16,2*1024*1024*processors,processors}}};
}
// 虛擬 CPU 能力的唯一描述；不得以宿主 ARM64 能力代替客體 x86 能力。
std::array<std::uint32_t,4> virtual_cpuid(std::uint32_t leaf,std::uint32_t processor_id,std::uint32_t processors,std::uint32_t subleaf=0);
std::array<std::uint32_t,4> virtual_cpuid(std::uint32_t leaf,std::uint32_t processor_id,std::uint32_t processors,std::uint32_t subleaf,const VirtualProcessorIdentity&);
bool virtual_processor_feature(std::uint32_t feature,bool x86);
}
