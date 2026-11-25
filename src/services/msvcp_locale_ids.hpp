#pragma once
#include <cstdint>
#include <string_view>
#include "x64arm/memory.hpp"

namespace x64arm::msvcp_locale_ids {
inline constexpr std::string_view counter = "?_Id_cnt@id@locale@std@@0HA";
inline constexpr std::string_view satellite = "msvcp140_codecvt_ids.dll";

enum class Method { construct, clear, assign, convert, counter_address };
struct Binding {
    Method method;
    std::string_view win32, win64;
    unsigned arguments;
    std::string_view resident{};
};
inline constexpr Binding methods[]{
    {Method::construct,"??0id@locale@std@@QAE@I@Z","??0id@locale@std@@QEAA@_K@Z",1,"guest_locale_id_construct"},
    {Method::clear,"??_Fid@locale@std@@QAEXXZ","??_Fid@locale@std@@QEAAXXZ",0,"guest_locale_id_clear"},
    {Method::assign,"??4id@locale@std@@QAEAAV012@ABV012@@Z","??4id@locale@std@@QEAAAEAV012@AEBV012@@Z",1,"guest_locale_id_assign"},
    {Method::convert,"??Bid@locale@std@@QAEIXZ","??Bid@locale@std@@QEAA_KXZ",0},
    {Method::counter_address,"?_Id_cnt_func@id@locale@std@@CAAAHXZ","?_Id_cnt_func@id@locale@std@@CAAEAHXZ",0}
};
inline constexpr std::string_view legacy_modules[]{
    "msvcp60.dll","msvcp70.dll","msvcp71.dll","msvcp90.dll","msvcp100.dll","msvcp110.dll"
};
inline constexpr bool legacy(std::string_view family){
    for(auto module:legacy_modules)if(module==family)return true;
    return false;
}
inline constexpr bool exported(std::string_view family,Method method){
    if(!legacy(family))return false;
    if(method==Method::assign)return family=="msvcp60.dll";
    if(method==Method::counter_address)return family=="msvcp90.dll";
    return family!="msvcp60.dll" || method==Method::convert;
}
inline constexpr const Binding* binding(std::string_view symbol,bool x86){
    if(symbol.find("id@locale@std@@")==symbol.npos)return nullptr;
    for(const auto& entry:methods)if((x86?entry.win32:entry.win64)==symbol)return &entry;
    return nullptr;
}

// 配發的編號寫回客體物件。計數器為 Windows int，物件欄位則為客體 size_t。
inline std::uint64_t facet_id(GuestMemory& memory,GuestAddress object,GuestAddress count,std::uint8_t bits){
    if(const auto current=memory.load(object,bits))return current;
    memory.check(object,bits/8,Access::Write);
    const auto next=static_cast<std::uint32_t>(memory.load(count,32)+1);
    const auto value=static_cast<std::uint64_t>(static_cast<std::int32_t>(next));
    memory.store(count,32,next);
    memory.store(object,bits,value);
    return bits==32?next:value;
}
inline std::uint64_t invoke(GuestMemory& memory,Method method,GuestAddress object,
                            std::uint64_t argument,GuestAddress count,std::uint8_t bits){
    switch(method){
    case Method::construct: memory.store(object,bits,argument); return object.value;
    case Method::clear: memory.store(object,bits,0); return 0;
    case Method::assign: memory.store(object,bits,memory.load(GuestAddress{argument},bits)); return object.value;
    case Method::convert: return facet_id(memory,object,count,bits);
    case Method::counter_address: return count.value;
    }
    throw std::logic_error("未知的 locale::id 成員");
}

// MSVC 型別修飾保留字元型別、iterator 及 mbstate_t，不合併同寬但不同型別的 facet。
#define X64ARM_FACET_ID(type) "?id@?$" type "@std@@2V0locale@2@A"
#define X64ARM_FACET_ITER(kind,ch,stream) X64ARM_FACET_ID(kind "@" ch "V?$" stream "buf_iterator@" ch "U?$char_traits@" ch "@std@@@std@@")
#define X64ARM_STD_FACETS(ch) X64ARM_FACET_ID("codecvt@" ch "DH"), X64ARM_FACET_ID("collate@" ch), \
    X64ARM_FACET_ID("ctype@" ch), X64ARM_FACET_ID("numpunct@" ch), X64ARM_FACET_ITER("num_get",ch,"istream"), \
    X64ARM_FACET_ITER("num_put",ch,"ostream"), X64ARM_FACET_ITER("time_put",ch,"ostream")
inline constexpr std::string_view vc6_facets[]{
    X64ARM_STD_FACETS("D"),X64ARM_FACET_ITER("time_get","D","istream"),X64ARM_STD_FACETS("G")
};
inline constexpr std::string_view wide_facets[]{X64ARM_STD_FACETS("_W"),X64ARM_FACET_ITER("time_get","_W","istream")};
inline constexpr std::string_view modern_ushort[]{X64ARM_FACET_ID("collate@G"),X64ARM_FACET_ID("numpunct@G")};
#define X64ARM_MONEY_MESSAGE(ch) X64ARM_FACET_ID("messages@" ch),X64ARM_FACET_ITER("money_get",ch,"istream"), \
    X64ARM_FACET_ITER("money_put",ch,"ostream"),X64ARM_FACET_ID("moneypunct@" ch "$00"),X64ARM_FACET_ID("moneypunct@" ch "$0A@")
inline constexpr std::string_view system_facets[]{
    X64ARM_MONEY_MESSAGE("D"),X64ARM_MONEY_MESSAGE("G"),X64ARM_MONEY_MESSAGE("_W"),X64ARM_FACET_ITER("time_get","G","istream")
};
inline constexpr std::string_view unicode_facets[]{
    X64ARM_FACET_ID("codecvt@_SDU_Mbstatet@@"),X64ARM_FACET_ID("codecvt@_S_QU_Mbstatet@@"),
    X64ARM_FACET_ID("codecvt@_UDU_Mbstatet@@"),X64ARM_FACET_ID("codecvt@_U_QU_Mbstatet@@")
};
#undef X64ARM_MONEY_MESSAGE
#undef X64ARM_STD_FACETS
#undef X64ARM_FACET_ITER
#undef X64ARM_FACET_ID
}
