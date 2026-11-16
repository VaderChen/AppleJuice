#pragma once
#include "crt_library.hpp"
#include "msvcp_numeric_limits.hpp"
namespace x64arm::detail {
// 不可變資料與可呼叫服務分別發布；資料保持真正的 PE RVA 及唯讀頁面。
inline std::span<const msvcp_numeric::Constant> builtin_constant_exports(std::string_view family){
    static const msvcp_numeric::Constant crt[]{{"_HUGE",0x7ff0000000000000ULL,8}};
    // ntdll 的 NLS 資料匯出：與 GetACP／GetOEMCP 相同的 UTF-8（65001）系統碼頁，屬多位元組碼頁。
    static const msvcp_numeric::Constant nls[]{{"NlsAnsiCodePage",65001,2},{"NlsMbCodePageTag",1,1},{"NlsMbOemCodePageTag",1,1}};
    if(family=="ntdll.dll")return nls;
    // numeric_limits 靜態資料與 _Inf 等常數不依 STL 物件版面，VC10 沿用 VC9 的同名同值資料。
    if(family=="msvcp90.dll" || family=="msvcp100.dll")return msvcp_numeric::constants();
    // VC11 起 numeric_limits 改為編譯期常數（不再匯出資料），只保留 xmath 的 _Inf／_Nan／_Snan／_Denorm 等 _Dconst；
    // msvcp140 另去掉 _Eps 系列與 _LZero。值與 VC9 的同名資料共用同一份表。
    if(family=="msvcp110.dll" || family=="msvcp120.dll" || family=="msvcp140.dll"){
        static const auto subset=[](bool modern){
            std::vector<msvcp_numeric::Constant> out;
            for(const auto& item:msvcp_numeric::constants()){
                if(item.name.starts_with('?'))continue;
                if(modern && (item.name.ends_with("Eps") || item.name=="_LZero"))continue;
                out.push_back(item);
            }
            // streamoff 的錯誤位置是 -1；VC14 仍為二進位相容保留真正的唯讀資料匯出。
            if(modern)out.push_back({"?_BADOFF@std@@3_JB",UINT64_MAX,8});
            return out;
        };
        static const auto vc11=subset(false),vc14=subset(true);
        return family=="msvcp140.dll"?std::span<const msvcp_numeric::Constant>(vc14):std::span<const msvcp_numeric::Constant>(vc11);
    }
    if(crt_library(family))return crt;
    return {};
}
inline constexpr std::uint32_t builtin_constant_rva=4096;
}
