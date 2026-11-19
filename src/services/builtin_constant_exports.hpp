#pragma once
#include "crt_library.hpp"
#include "x64arm/windows_guid.hpp"
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
// 介面識別碼等 16 位元組的唯讀資料匯出（DEFINE_GUID 的資料），放在 8 位元組常數之後，同一個 .rdata 頁。
struct GuidConstant {std::string_view name;std::array<std::uint8_t,16> bytes;};
constexpr std::array<std::uint8_t,16> guid_bytes(std::uint32_t a,std::uint16_t b,std::uint16_t c,std::array<std::uint8_t,8> d){
    return {static_cast<std::uint8_t>(a),static_cast<std::uint8_t>(a>>8),static_cast<std::uint8_t>(a>>16),static_cast<std::uint8_t>(a>>24),
        static_cast<std::uint8_t>(b),static_cast<std::uint8_t>(b>>8),static_cast<std::uint8_t>(c),static_cast<std::uint8_t>(c>>8),d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]};
}
inline std::span<const GuidConstant> builtin_guid_exports(std::string_view family){
    static constexpr GuidConstant oleacc[]{
        {"IID_IAccessible",guid_bytes(0x618736e0,0x3c3d,0x11cf,{0x81,0x0c,0x00,0xaa,0x00,0x38,0x9b,0x71})},
        {"IID_IAccessibleHandler",guid_bytes(0x03022430,0xabc4,0x11d0,{0xbd,0xe2,0x00,0xaa,0x00,0x1a,0x19,0x53})},
        {"LIBID_Accessibility",guid_bytes(0x1ea4dbf0,0x3c3b,0x11cf,{0x81,0x0c,0x00,0xaa,0x00,0x38,0x9b,0x71})}};
    static constexpr GuidConstant riched[]{
        {"IID_IRichEditOle",guid_bytes(0x00020d00,0,0,{0xc0,0,0,0,0,0,0,0x46})},
        {"IID_IRichEditOleCallback",guid_bytes(0x00020d03,0,0,{0xc0,0,0,0,0,0,0,0x46})},
        {"IID_ITextHost",guid_bytes(0xc5bdd8d0,0xd26e,0x11ce,{0xa8,0x9e,0x00,0xaa,0x00,0x6c,0xad,0xc5})},
        {"IID_ITextHost2",guid_bytes(0x13e670f5,0x1a5a,0x11cf,{0xab,0xeb,0x00,0xaa,0x00,0xb6,0x5e,0xa1})},
        {"IID_ITextServices",guid_bytes(0x8d33f740,0xcf58,0x11ce,{0xa8,0x9d,0x00,0xaa,0x00,0x6c,0xad,0xc5})}};
    static constexpr GuidConstant avi[]{
        {"CLSID_AVISimpleUnMarshal",ole_standard_guid(0x20009)},
        {"IID_IAVIFile",ole_standard_guid(0x20020)},{"IID_IAVIStream",ole_standard_guid(0x20021)},
        {"IID_IGetFrame",ole_standard_guid(0x20023)},{"IID_IAVIEditStream",ole_standard_guid(0x20024)}
    };
    if(family=="avifil32.dll")return avi;
    if(family=="oleacc.dll")return oleacc;
    if(family=="riched20.dll")return riched;
    return {};
}
inline constexpr std::uint32_t builtin_constant_rva=4096;
}
