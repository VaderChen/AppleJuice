#pragma once
#include "x64arm/windows_abi.hpp"

namespace x64arm::msvcp_detail {
// VC10 與 VC14 共用操作語義；ABI 差異只在此描述。VC14 streambuf 不含 VC10 的鎖指標。
struct StreamAbi {
    bool modern{};
    unsigned element{2},p{},state{},flags{},precision{},width{},locale{},ios_size{},buffer_size{};
    explicit StreamAbi(bool x86,bool vc14=false,unsigned characters=2):modern(vc14),element(characters),p(x86?4U:8U),
        state(x86?(vc14?8U:12U):16U),flags(x86?(vc14?16U:20U):24U),precision(x86?24U:32U),width(x86?32U:40U),
        locale(x86?48U:64U),ios_size(x86?56U:72U),buffer_size((vc14?12U:13U)*p+8U){}
    unsigned first(bool put=false)const{return ((modern?1U:2U)+(put?1U:0U))*p;}
    unsigned first_indirect(bool put=false)const{return first(put)+2*p;}
    unsigned next(bool put=false)const{return first(put)+4*p;}
    unsigned next_indirect(bool put=false)const{return first(put)+6*p;}
    unsigned count(bool put=false)const{return (modern?9U:10U)*p+(put?4U:0U);}
    unsigned count_indirect(bool put=false)const{return (modern?9U:10U)*p+8+(put?p:0U);}
    unsigned buffer_locale()const{return (modern?11U:12U)*p+8;}
    unsigned ostream_base()const{return modern?(p==4?8U:16U):8U;}
    unsigned istream_base()const{return modern?(p==4?20U:24U):16U;}
    unsigned iostream_base()const{return modern?(p==4?24U:32U):24U;}
    unsigned locale_pointer()const{return modern?p:0U;}
    unsigned locale_size()const{return modern?2*p:p;}
    unsigned facet_slot(unsigned vc10_slot)const{return vc10_slot+(modern?2U:0U);}
    std::uint8_t reference_bits()const{return static_cast<std::uint8_t>(modern?32U:p*8);}
    std::uint32_t eof()const{return element==1?UINT32_MAX:0xffffU;}
    std::uint32_t int_mask()const{return element==1?UINT32_MAX:0xffffU;}
    std::uint8_t character_bits()const{return static_cast<std::uint8_t>(element*8);}
};
inline bool modern(const WindowsServiceDescriptor& service){return service.family=="msvcp140.dll" || service.family=="internal-msvcp140";}
inline unsigned element(const WindowsServiceDescriptor& service){
    if(service.family=="internal-msvcp140")return service.symbol.starts_with('D')?1U:2U;
    return service.symbol.find("basic_streambuf@D")!=service.symbol.npos || service.symbol.find("basic_ios@D")!=service.symbol.npos ||
        service.symbol.find("basic_istream@D")!=service.symbol.npos || service.symbol.find("basic_ostream@D")!=service.symbol.npos ||
        service.symbol.find("basic_iostream@D")!=service.symbol.npos || service.symbol.find("ctype@D")!=service.symbol.npos ||
        service.symbol.find("time_put@D")!=service.symbol.npos || service.symbol.find("codecvt@DD")!=service.symbol.npos ||
        service.symbol.find("num_put@D")!=service.symbol.npos || service.symbol.find("numpunct@D")!=service.symbol.npos ||
        service.symbol.find("numpunct_byname@D")!=service.symbol.npos?1U:2U;
}
inline StreamAbi abi(bool x86,const WindowsServiceDescriptor& service){return StreamAbi(x86,modern(service),element(service));}
}
