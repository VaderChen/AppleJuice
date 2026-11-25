#pragma once

#include "builtin_crt_forwarded_exports.hpp"

namespace x64arm::detail {

#include "builtin_d3dx10_forwarded_names.inc"

// VC14 主 DLL 原有的 char16_t／char32_t + char 相容名稱，與 codecvt 輔助 DLL 共用同一份可寫 id。
inline constexpr ForwardedExportName codecvt_char_ids[]{
    {"?id@?$codecvt@_SDU_Mbstatet@@@std@@2V0locale@2@A",3},
    {"?id@?$codecvt@_UDU_Mbstatet@@@std@@2V0locale@2@A",3}
};
inline constexpr ForwardedExports codecvt_id_forwarders[]{
    {"msvcp140.dll","msvcp140_codecvt_ids.dll",codecvt_char_ids}
};
inline constexpr std::span<const ForwardedExports> builtin_forwarder_tables[]{d3dx10_forwarders,crt_forwarders,codecvt_id_forwarders};

inline std::span<const ForwardedExports> builtin_forwarders(std::string_view source){
    for(const auto table:builtin_forwarder_tables){
        const auto range=std::ranges::equal_range(table,source,{},&ForwardedExports::source);
        if(!range.empty())return {range.begin(),range.end()};
    }
    return {};
}

}
