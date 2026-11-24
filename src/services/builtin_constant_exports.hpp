#pragma once
#include "ansi_codepage.hpp"
#include "crt_library.hpp"
#include "x64arm/windows_guid.hpp"
#include "msvcp_numeric_limits.hpp"
namespace x64arm::detail {
// 不可變資料與可呼叫服務分別發布；資料保持真正的 PE RVA 及唯讀頁面。
inline std::span<const msvcp_numeric::Constant> builtin_constant_exports(std::string_view family){
    static const msvcp_numeric::Constant crt[]{{"_HUGE",0x7ff0000000000000ULL,8}};
    // ntdll 的 NLS 資料匯出：與 GetACP／GetOEMCP 相同的程序碼頁（UTF-8 與 DBCS 碼頁皆屬多位元組碼頁）。
    static thread_local msvcp_numeric::Constant nls[]{{"NlsAnsiCodePage",65001,2},{"NlsMbCodePageTag",1,1},{"NlsMbOemCodePageTag",1,1}};
    if(family=="ntdll.dll"){nls[0].value=process_ansi_codepage();return nls;}
    if(const auto numeric=msvcp_numeric::module_constants(family);!numeric.empty())return numeric;
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
