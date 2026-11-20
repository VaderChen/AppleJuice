#pragma once
#include "x64arm/windows_abi.hpp"

namespace x64arm::msvcp_detail {
// VC10 與 VC14 共用操作語義；ABI 差異只在此描述。VC14 streambuf 不含 VC10 的鎖指標。
// x86 的 ios_base 因含 8 位元組 streamsize，虛擬表指標後補齊到 8：VC10／VC14 的 _Mystate 皆在 12、_Fmtfl 在 20
// （VC14 實際程式內嵌碼讀 _Fmtfl 於 0x14、_Wide 於 0x20）；VC9 的 streamsize 為 int，沒有這段補齊。
// VC12（msvcp120）的 streambuf、facet 虛擬表、ios_base 與虛擬基底位移都與 VC14 相同，只有 locale 物件仍是 VC10 的
// 單一 _Locimp 指標（位於 +0、大小一個指標）：compact_locale 只改 locale 物件版面，其餘沿用 modern。
struct StreamAbi {
    bool modern{},legacy{},compact_locale{};
    unsigned element{2},p{},state{},flags{},precision{},width{},locale{},ios_size{},buffer_size{};
    explicit StreamAbi(bool x86,bool vc14=false,unsigned characters=2,bool vc9=false,bool vc12=false):modern(vc14),legacy(vc9),compact_locale(vc14 && vc12),element(characters),p(x86?4U:8U),
        state(x86?(vc9?8U:12U):16U),flags(x86?(vc9?16U:20U):24U),precision(x86?(vc9?20U:24U):32U),width(x86?(vc9?24U:32U):40U),
        locale(x86?(vc9?36U:48U):64U),ios_size(x86?(vc9?40U:56U):72U),buffer_size((vc14?12U:13U)*p+8U){}
    unsigned count_offset()const{return legacy?p:8U;}
    std::uint8_t stream_bits()const{return static_cast<std::uint8_t>(legacy?p*8:64U);}
    unsigned slot(unsigned vc10_slot)const{return legacy && vc10_slot>=3?vc10_slot-(vc10_slot>=9?1U:2U):vc10_slot;}
    unsigned file_pointer()const{return p==4?buffer_size+16U:buffer_size+24U;}
    unsigned file_size()const{return file_pointer()+p;}
    unsigned first(bool put=false)const{return ((modern?1U:2U)+(put?1U:0U))*p;}
    unsigned first_indirect(bool put=false)const{return first(put)+2*p;}
    unsigned next(bool put=false)const{return first(put)+4*p;}
    unsigned next_indirect(bool put=false)const{return first(put)+6*p;}
    unsigned count(bool put=false)const{return (modern?9U:10U)*p+(put?4U:0U);}
    unsigned count_indirect(bool put=false)const{return (modern?9U:10U)*p+8+(put?p:0U);}
    unsigned buffer_locale()const{return (modern?11U:12U)*p+8;}
    unsigned ostream_base()const{return modern?(p==4?8U:16U):8U;}
    unsigned istream_base()const{return legacy?2*p:modern?(p==4?20U:24U):16U;}
    unsigned iostream_base()const{return modern?(p==4?24U:32U):24U;}
    unsigned locale_pointer()const{return modern && !compact_locale?p:0U;}
    unsigned locale_size()const{return modern && !compact_locale?2*p:p;}
    unsigned variant()const{return compact_locale?1U:0U;} // 0＝VC14、1＝VC12 的各自常駐物件與虛擬表
    unsigned facet_slot(unsigned vc10_slot)const{return vc10_slot+(modern?2U:0U);}
    std::uint8_t reference_bits()const{return static_cast<std::uint8_t>(modern?32U:p*8);}
    std::uint32_t eof()const{return element==1?UINT32_MAX:0xffffU;}
    std::uint32_t int_mask()const{return element==1?UINT32_MAX:0xffffU;}
    std::uint8_t character_bits()const{return static_cast<std::uint8_t>(element*8);}
};
// STL 的 C 匯出名稱不含類別字樣，由此併入 VC10／VC14 共用流程。
inline bool c_export(std::string_view name){
    return name=="_Getctype" || name=="_Getcvt" || name=="_Lock_shared_ptr_spin_lock" || name=="_Unlock_shared_ptr_spin_lock";
}
inline bool vc12(std::string_view family){return family=="msvcp120.dll" || family=="internal-msvcp120";}
inline bool vc12(const WindowsServiceDescriptor& service){return vc12(service.family);}
inline bool modern(const WindowsServiceDescriptor& service){return service.family=="msvcp140.dll" || service.family=="internal-msvcp140" || vc12(service);}
// 各版本的匯出 DLL 與內部虛擬函式家族；VC12 物件的虛擬表指向自己的內部家族，派送時才取得 VC12 版面。
inline const char* modern_dll(unsigned variant){return variant?"msvcp120.dll":"msvcp140.dll";}
inline const char* modern_internal(unsigned variant){return variant?"internal-msvcp120":"internal-msvcp140";}
inline unsigned element(const WindowsServiceDescriptor& service){
    // VC9／VC10 基底 streambuf 的內部虛擬表依字元型別分開：DStream…（char）、WStream…（寬字元）。
    if((service.family=="internal-msvcp90" || service.family=="internal-msvcp100") && (service.symbol.starts_with("DStream") || service.symbol.starts_with("WStream")))return service.symbol[0]=='D'?1U:2U;
    if(service.family=="internal-msvcp90")return 1U;
    if(service.family=="internal-msvcp140" || service.family=="internal-msvcp120")return service.symbol.starts_with('D')?1U:2U;
    return service.symbol.find("basic_streambuf@D")!=service.symbol.npos || service.symbol.find("basic_ios@D")!=service.symbol.npos ||
        service.symbol.find("basic_istream@D")!=service.symbol.npos || service.symbol.find("basic_ostream@D")!=service.symbol.npos ||
        service.symbol.find("basic_iostream@D")!=service.symbol.npos || service.symbol.find("ctype@D")!=service.symbol.npos ||
        service.symbol.find("time_put@D")!=service.symbol.npos || service.symbol.find("time_get@D")!=service.symbol.npos || service.symbol.find("codecvt@DD")!=service.symbol.npos ||
        service.symbol.find("num_put@D")!=service.symbol.npos || service.symbol.find("num_get@D")!=service.symbol.npos || service.symbol.find("numpunct@D")!=service.symbol.npos ||
        service.symbol.find("numpunct_byname@D")!=service.symbol.npos || service.symbol.find("basic_ifstream@D")!=service.symbol.npos ||
        service.symbol.find("basic_filebuf@D")!=service.symbol.npos?1U:2U;
}
inline StreamAbi abi(bool x86,const WindowsServiceDescriptor& service){return StreamAbi(x86,modern(service),element(service),service.family=="msvcp90.dll" || service.family=="internal-msvcp90",vc12(service));}
}
