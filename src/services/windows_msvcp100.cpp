#include "ansi_codepage.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "crt_c_locale.hpp"
#include "crt_float_chars.hpp"
#include "msvcp_stream_abi.hpp"
#include "msvcp_numeric.hpp"
#include "msvcp_number_scan.hpp"
#include "x64arm/x87_fp.hpp"
#include "msvcp_rtti_shapes.hpp"
#include "msvcp_time_get.hpp"
#include "msvc_exception_abi.hpp"
#include "windows_locale_data.hpp"
#include "service_callback_task.hpp"
#include "crt_time_format.hpp"
#include "x64arm/windows_error_support.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#include <limits>
#include <set>
#include <unicode/uchar.h>

namespace x64arm {
namespace {
using Layout=msvcp_detail::StreamAbi;
// VC12 的 mbstate_t 是 int：修飾名與 RTTI 型別名稱以 H 取代 VC14 的 _Mbstatet（與 msvcp120 描述表的產生規則相同）。
std::string msvcp120_symbol(std::string symbol){
    for(const auto& [from,to]:std::array<std::pair<std::string_view,std::string_view>,5>{{{"U_Mbstatet@@@","H@"},{"AEAU_Mbstatet@@","AEAH"},{"AAU_Mbstatet@@","AAH"},{"AEBU_Mbstatet@@","AEBH"},{"ABU_Mbstatet@@","ABH"}}})
        for(auto at=symbol.find(from);at!=symbol.npos;at=symbol.find(from,at+to.size()))symbol.replace(at,from.size(),to);
    return symbol;
}
struct Operation {
    WindowsServiceDescriptor service;
    std::string kind;
    std::uint64_t object{},buffer{},ios{},output{},limit{},count{},scratch{},final_result{},tie_ios{};
    unsigned phase{},callback{};
    bool modern{};unsigned element{2};
    bool floating{},unsigned_integer{},started{},skip{},locked{},lock_started{},completed{},final_wide{},throw_failure{},suffix_done{};
    msvc_detail::ExceptionRecord exception_record;
    std::vector<std::uint64_t> objects;
    std::size_t caught_exception_base{SIZE_MAX};
    unsigned copy_failures{};
    bool rethrow_exception{};
    std::size_t retire_cursor{SIZE_MAX};
    bool retire_callback_pending{};
    std::set<std::uint64_t> retired_objects;
    std::uint64_t rethrow_owned_object{};
    bool decref_pending{};
    msvcp_detail::NumberInput numeric_value;
    msvcp_detail::NumberFormat numeric_format;
    msvcp_detail::NumericPunctuation numeric_punctuation;
    std::uint64_t numeric_punct{};
    std::uint64_t numeric_ctype{},numeric_widened{};
    std::u16string numeric_text,numeric_raw;
    std::vector<std::uint64_t> numeric_facets,numeric_allocations;
    std::size_t numeric_held{},numeric_written{};
    bool numeric_failed{},numeric_hold_pending{};
    std::uint64_t locale_source{},locale_pending{},locale_lock_storage{},locale_capacity{};
    bool locale_source_owned{},locale_facet_owned{};
    bool locale_lock_owned{},allocation_failure{};
    std::uint64_t delimiter{'\n'},ios_node{},space_table{};
    bool keep_delimiter{}; // get(char*／streambuf&, …)：遇到分隔字元即停止且不取出；緩衝區滿不算失敗。
    msvcp_detail::NumberField number_field;
    std::uint64_t time_state{},time_tm{},input_value{}; // time_get／num_get：ios_base::iostate 與 tm／數值輸出位址。
    std::u16string time_seen;int time_look{-2}; // 已取走的字元與前瞻（-2 未知、-1 輸入結束）。
    bool number_ready{};
};
msvcp_detail::NumberType msvcp_number_type(std::string_view name){
    using Type=msvcp_detail::NumberType;
    if(name.ends_with("_N@Z"))return Type::Boolean;if(name.ends_with("_J@Z"))return Type::Signed64;
    if(name.ends_with("_K@Z"))return Type::Unsigned64;if(name.ends_with("F@Z"))return Type::Signed16;
    if(name.ends_with("G@Z"))return Type::Unsigned16;if(name.ends_with("I@Z") || name.ends_with("K@Z"))return Type::Unsigned32;
    if(name.ends_with("M@Z"))return Type::Floating32;if(name.ends_with("N@Z") || name.ends_with("O@Z"))return Type::Floating64;
    if(name.ends_with("PEBX@Z") || name.ends_with("PBX@Z") || name.ends_with("PEAX@Z") || name.ends_with("PAX@Z"))return Type::Pointer;
    return Type::Signed32;
}
msvcp_detail::NumberFormat msvcp_number_format(const GuestMemory& memory,const Layout& l,std::uint64_t ios,char16_t fill){
    return {static_cast<std::uint32_t>(memory.load(GuestAddress{ios+l.flags},32)),
        std::bit_cast<std::int64_t>(memory.load(GuestAddress{ios+l.precision},64)),
        std::bit_cast<std::int64_t>(memory.load(GuestAddress{ios+l.width},64)),fill};
}
std::u16string format_msvcp_number(const Operation& op,GuestMemory& memory,const Layout& l){
    const auto fill=static_cast<char16_t>(memory.load(GuestAddress{op.ios+l.ios_size+2*l.p},l.character_bits()));
    const auto result=msvcp_detail::format_number({msvcp_number_type(op.service.symbol),op.count,l.p},msvcp_number_format(memory,l,op.ios,fill));
    memory.store(GuestAddress{op.ios+l.width},64,0);return result;
}
// ios_base::_Findarr：同索引沿用節點，否則回收全零節點，再不然配置新節點置於串列前端；配置失敗回傳 0。
template<class Allocate> std::uint64_t msvcp_ios_array(GuestMemory& memory,const Layout& l,std::uint64_t base,std::uint32_t index,Allocate&& allocate){
    const auto bits=static_cast<std::uint8_t>(l.p*8);const auto head=base+l.width+8;std::uint64_t spare=0;
    for(auto node=memory.load(GuestAddress{head},bits);node;node=memory.load(GuestAddress{node},bits)){
        if(memory.load(GuestAddress{node+l.p},32)==index)return node;
        if(!spare && !memory.load(GuestAddress{node+l.p+4},32) && !memory.load(GuestAddress{node+l.p+8},bits))spare=node;
    }
    if(spare){memory.store(GuestAddress{spare+l.p},32,index);return spare;}
    const auto node=allocate(2*l.p+8);if(!node)return 0;
    memory.fill(GuestAddress{node},0,2*l.p+8);memory.store(GuestAddress{node},bits,memory.load(GuestAddress{head},bits));
    memory.store(GuestAddress{node+l.p},32,index);memory.store(GuestAddress{head},bits,node);return node;
}
unsigned character_mask(char16_t c){
    unsigned value=0;
    if(u_isupper(c))value|=1;if(u_islower(c))value|=2;if(u_isdigit(c))value|=4;
    if(u_isUWhiteSpace(c))value|=8;if(u_ispunct(c))value|=16;if(u_iscntrl(c))value|=32;
    if(u_isblank(c))value|=64;if(u_isxdigit(c))value|=128;if(u_isalpha(c))value|=0x100;
    return value;
}
}
struct Msvcp100State {
    std::uint64_t file_table9{},ifstream_table9{},ifstream_vb9{};
    std::map<std::string,std::uint64_t,std::less<>> data;
    std::map<std::uint64_t,Operation> operations;
    std::map<std::uint64_t,std::pair<std::uint64_t,std::uint64_t>> ios_objects;
    std::uint64_t global{},codecvt_table{},ctype_table{},buffer_table{},ios_table{},istream_vb{},iostream_vb{},next{1},next_id{2};
    bool sync_with_stdio{true}; // ios_base::sync_with_stdio 的目前設定（MSVC 只保存值，不改變串流行為）。
    std::uint64_t std_new_handler{},ostream_vb{}; // std::set_new_handler 的處理常式、VC10 ostream 的虛擬基底表。
    struct Tables {std::uint64_t buffer{},ios{},istream{},ostream{},iostream{},codecvt{},codecvt16{},codecvt32{},codecvt_ushort{},ctype{},ctype16{},num_put{},num_put16{},num_get{},num_get16{},numpunct{},numpunct_byname{},time_put{},time_put16{},time_get{};};
    // VC14 與 VC12 各自的常駐物件：虛擬表指向各自的內部家族，locale 物件與標準串流依各自版面建立（索引見 StreamAbi::variant）。
    struct ModernRuntime {
        std::map<std::string,std::uint64_t,std::less<>> data14;
        std::uint64_t codecvt_base14{}; // codecvt_base 的虛擬表（do_always_noconv／do_max_length／do_encoding）。
        std::uint64_t ctype_base14{},winit_count14{}; // ctype_base 虛擬表、_Winit 計數格。
        std::array<Tables,2> tables14;
        std::uint64_t global14{},classic14{},facet_table14{},locimp_table14{},ios_base_table14{},ostream_vb14{},istream_vb14{},iostream_vb14{},id_count14{},next_id14{8};
        std::uint64_t ctype_masks14{},classic_locale14{},init_count14{},ios_index14{},empty_wide14{};
        std::int32_t init_locks14{-1}; // std::_Init_locks::_Init_cnt（初值 -1）
    };
    std::array<ModernRuntime,2> runtimes;
    bool shared_ptr_locked{};
    std::map<std::uint64_t,unsigned> standard_buffers;
    std::set<std::uint64_t> owned_locale_imps;
    struct CapturedException {std::uint64_t record{},object{},destroy{};bool interface_handle{},constructed{},immortal{};};
    std::map<std::uint64_t,CapturedException> exception_pointers;
    std::uint64_t exception_control_table{},bad_alloc_pointer{},bad_exception_pointer{};
    std::array<std::uint64_t,2> buffer_table9{};std::uint64_t buffer_table_narrow{}; // VC9 基底 streambuf（char／寬字元）與 VC10 char streambuf 的虛擬表
    struct FacetLocale {CrtLocaleCategory category;std::vector<std::uint64_t> resources;};
    std::map<std::uint64_t,FacetLocale> facet_locales;
    std::array<std::uint64_t,3> error_categories{},error_category_names{};
    std::map<std::string,std::uint64_t,std::less<>> generic_error_messages;
};
void WindowsServices::initialize_msvcp100(GuestMemory& memory){
    msvcp100_=std::make_shared<Msvcp100State>();auto& s=*msvcp100_;const Layout l(x86_);
    const auto bits=static_cast<std::uint8_t>(l.p*8);
    const auto put=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},bits,value);};
    const auto table=[&](std::initializer_list<const char*> names){
        const auto address=runtime_allocate(memory,{},names.size()*l.p);unsigned i=0;
        for(const auto name:names)put(address+i++*l.p,resolve("internal-msvcp100",std::string(name)+(x86_?"32":"64")).value);
        return address;
    };
    s.codecvt_table=table({"FacetDelete","CodecvtNoconv","CodecvtMaxLength","CodecvtEncoding","CodecvtIn","CodecvtOut","CodecvtUnshift","CodecvtLength"});
    s.ctype_table=table({"FacetDelete"});
    // VC10 寬字元沿用原本的無前綴名稱；char 版以 D 前綴另建一份。
    s.buffer_table_narrow=table({"DStreamDelete","DStreamLock","DStreamUnlock","DStreamOverflow","DStreamPbackfail","DStreamShowmany","DStreamUnderflow","DStreamUflow","DStreamXsgetn","DStreamXsputn","DStreamSeekoff","DStreamSeekpos","DStreamSetbuf","DStreamSync","DStreamImbue"});
    s.buffer_table=table({"StreamDelete","StreamLock","StreamUnlock","StreamOverflow","StreamPbackfail","StreamShowmany","StreamUnderflow","StreamUflow","StreamXsgetn","StreamXsputn","StreamSeekoff","StreamSeekpos","StreamSetbuf","StreamSync","StreamImbue"});
    s.ios_table=table({"IosDelete"});
    const auto table9=[&](std::initializer_list<const char*> names){
        const auto address=runtime_allocate(memory,{},(names.size()+1)*l.p)+l.p;unsigned i=0;
        for(const auto name:names)put(address+i++*l.p,resolve("internal-msvcp90",std::string(name)+(x86_?"32":"64")).value);return address;
    };
    s.file_table9=table9({"FileDelete","FileOverflow","FilePbackfail","StreamShowmany","FileUnderflow","FileUflow","FileXsgetn","FileXsgetnSafe","FileXsputn","FileSeekoff","FileSeekpos","FileSetbuf","FileSync","FileImbue"});
    // VC9 基底 streambuf 的虛擬表：沒有 _Lock／_Unlock，xsgetn 之後多一格 _Xsgetn_s。
    // char 與寬字元各一份（eof 與字元寬度不同）。
    s.buffer_table9[0]=table9({"DStreamDelete","DStreamOverflow","DStreamPbackfail","DStreamShowmany","DStreamUnderflow","DStreamUflow","DStreamXsgetn","DStreamXsgetnSafe","DStreamXsputn","DStreamSeekoff","DStreamSeekpos","DStreamSetbuf","DStreamSync","DStreamImbue"});
    s.buffer_table9[1]=table9({"WStreamDelete","WStreamOverflow","WStreamPbackfail","WStreamShowmany","WStreamUnderflow","WStreamUflow","WStreamXsgetn","WStreamXsgetnSafe","WStreamXsputn","WStreamSeekoff","WStreamSeekpos","WStreamSetbuf","WStreamSync","WStreamImbue"});
    s.ifstream_table9=table9({"FileIosDelete"});s.ifstream_vb9=runtime_allocate(memory,{},8);
    const Layout vc9(x86_,false,1,true);memory.store(GuestAddress{s.ifstream_vb9+4},32,2*l.p+vc9.file_size());
    for(const auto name:{"??_7ios_base@std@@6B@","??_7?$basic_ios@GU?$char_traits@G@std@@@std@@6B@","??_7?$basic_istream@GU?$char_traits@G@std@@@std@@6B@"})s.data.emplace(name,s.ios_table);
    for(const auto& [name,value]:std::array<std::pair<const char*,std::uint64_t>,3>{{
        {"?id@?$ctype@G@std@@2V0locale@2@A",1},{"?id@?$codecvt@GDH@std@@2V0locale@2@A",2},{"?_BADOFF@std@@3_JB",UINT64_MAX}}}){
        const auto address=runtime_allocate(memory,{},8);memory.store(GuestAddress{address},64,value);s.data.emplace(name,address);
    }
    s.global=runtime_allocate(memory,{},96);put(s.global,s.ctype_table);put(s.global+l.p,1);
    const auto facets=runtime_allocate(memory,{},3*l.p);put(s.global+2*l.p,facets);put(s.global+3*l.p,3);
    memory.store(GuestAddress{s.global+4*l.p},32,0x3f);
    // _Locimp 的 _Yarn 名稱位於兩個 DWORD（category／transparent）之後。
    const auto name=runtime_allocate(memory,std::array<std::uint8_t,2>{'C',0});put(s.global+4*l.p+8,name);
    s.istream_vb=runtime_allocate(memory,{},8);memory.store(GuestAddress{s.istream_vb+4},32,16);
    s.iostream_vb=runtime_allocate(memory,{},16);memory.store(GuestAddress{s.iostream_vb+4},32,24);
    memory.store(GuestAddress{s.iostream_vb+8},32,static_cast<std::uint32_t>(-16));memory.store(GuestAddress{s.iostream_vb+12},32,8);
    msvc_rtti::Storage rtti(l.p);std::vector<std::pair<std::uint64_t,std::uint32_t>> rtti_tables;
    msvc_rtti::Shape file_shape;file_shape.bases={{msvcp_detail::stream_type("filebuf",false),1},{msvcp_detail::stream_type("streambuf",false),0}};
    rtti_tables.emplace_back(s.file_table9-l.p,rtti.add(file_shape));
    msvc_rtti::Shape input_file_shape;input_file_shape.flags=1;input_file_shape.offset=2*l.p+vc9.file_size();
    input_file_shape.bases={{msvcp_detail::stream_type("ifstream",false),4},{msvcp_detail::stream_type("istream",false),3},
        {msvcp_detail::stream_type("ios",false),2,0,0,4,80},{".?AVios_base@std@@",1,0,0,4},{".?AV?$_Iosb@H@std@@",0,static_cast<std::int32_t>(l.p),0,4}};
    rtti_tables.emplace_back(s.ifstream_table9-l.p,rtti.add(input_file_shape));
    const auto modern_table=[&](unsigned variant,char element,std::initializer_list<const char*> names,msvc_rtti::Shape shape=msvc_rtti::Shape{}){
        const auto storage=runtime_allocate(memory,{},(names.size()+1)*l.p),address=storage+l.p;unsigned i=0;
        for(const auto name:names)put(address+i++*l.p,resolve(msvcp_detail::modern_internal(variant),std::string(1,element)+name+(x86_?"32":"64")).value);
        if(!shape.bases.empty()){
            if(variant)for(auto& base:shape.bases)base.name=msvcp120_symbol(base.name);
            rtti_tables.emplace_back(storage,rtti.add(shape));s.runtimes[variant].data14.emplace("??_7"+shape.bases.front().name.substr(4)+"6B@",address);
        }
        return address;
    };
    const auto initialize_modern=[&](unsigned variant){auto& m=s.runtimes[variant];
    for(unsigned element=0;element<2;++element){auto& t=m.tables14[element];const auto prefix=element?'W':'D';
        const auto stream_shape=[&](std::string_view kind){return msvcp_detail::stream_shape(kind,element!=0,l.p);};const std::string character=element?"_W":"D";
        t.buffer=modern_table(variant,prefix,{"StreamDelete","StreamLock","StreamUnlock","StreamOverflow","StreamPbackfail","StreamShowmany","StreamUnderflow","StreamUflow","StreamXsgetn","StreamXsputn","StreamSeekoff","StreamSeekpos","StreamSetbuf","StreamSync","StreamImbue"},stream_shape("streambuf"));
        t.ios=modern_table(variant,prefix,{"IosDelete"},stream_shape("ios"));t.istream=modern_table(variant,prefix,{"IosDelete"},stream_shape("istream"));
        t.ostream=modern_table(variant,prefix,{"IosDelete"},stream_shape("ostream"));t.iostream=modern_table(variant,prefix,{"IosDelete"},stream_shape("iostream"));
        t.codecvt=modern_table(variant,prefix,{"FacetDelete","FacetIncref","FacetDecref","CodecvtNoconv","CodecvtMaxLength","CodecvtEncoding","CodecvtIn","CodecvtOut","CodecvtUnshift","CodecvtLength"},
            msvcp_detail::facet_shape(element?".?AV?$codecvt@_WDU_Mbstatet@@@std@@":".?AV?$codecvt@DDU_Mbstatet@@@std@@",l.p,".?AVcodecvt_base@std@@"));
        if(element){
            // unsigned short 版與 wchar_t 共用碼頁轉換（自己的 RTTI）；char16_t／char32_t 為 UTF-8 轉換。
            t.codecvt_ushort=modern_table(variant,prefix,{"FacetDelete","FacetIncref","FacetDecref","CodecvtNoconv","CodecvtMaxLength","CodecvtEncoding","CodecvtIn","CodecvtOut","CodecvtUnshift","CodecvtLength"},
                msvcp_detail::facet_shape(".?AV?$codecvt@GDU_Mbstatet@@@std@@",l.p,".?AVcodecvt_base@std@@"));
            t.codecvt16=modern_table(variant,prefix,{"FacetDelete","FacetIncref","FacetDecref","Codecvt16Noconv","Codecvt16MaxLength","Codecvt16Encoding","Codecvt16In","Codecvt16Out","Codecvt16Unshift","Codecvt16Length"},
                msvcp_detail::facet_shape(".?AV?$codecvt@_SDU_Mbstatet@@@std@@",l.p,".?AVcodecvt_base@std@@"));
            t.codecvt32=modern_table(variant,prefix,{"FacetDelete","FacetIncref","FacetDecref","Codecvt32Noconv","Codecvt32MaxLength","Codecvt32Encoding","Codecvt32In","Codecvt32Out","Codecvt32Unshift","Codecvt32Length"},
                msvcp_detail::facet_shape(".?AV?$codecvt@_UDU_Mbstatet@@@std@@",l.p,".?AVcodecvt_base@std@@"));
        }
        const auto ctype_shape=msvcp_detail::facet_shape(".?AV?$ctype@"+character+"@std@@",l.p,".?AUctype_base@std@@");
        if(element){
            const std::initializer_list<const char*> wide_ctype{"FacetDelete","FacetIncref","FacetDecref","CtypeIsRange","CtypeIs","CtypeScanIs","CtypeScanNot","CtypeLowerRange","CtypeLower","CtypeUpperRange","CtypeUpper","CtypeWidenRange","CtypeWiden","CtypeNarrowRange","CtypeNarrow"};
            t.ctype=modern_table(variant,prefix,wide_ctype,ctype_shape);
            // unsigned short 版與 wchar_t 共用分類實作，另有自己的 vtable 與 RTTI。
            t.ctype16=modern_table(variant,prefix,wide_ctype,msvcp_detail::facet_shape(".?AV?$ctype@G@std@@",l.p,".?AUctype_base@std@@"));
        }
        else t.ctype=modern_table(variant,prefix,{"FacetDelete","FacetIncref","FacetDecref","CtypeLowerRange","CtypeLower","CtypeUpperRange","CtypeUpper","CtypeWidenRange","CtypeWiden","CtypeNarrowRange","CtypeNarrow"},ctype_shape);
        const auto num_put_table=[&](const std::string& c){return modern_table(variant,prefix,{"FacetDelete","FacetIncref","FacetDecref","NumPutPointer","NumPutLongDouble","NumPutDouble","NumPutUnsigned64","NumPutSigned64","NumPutUnsigned32","NumPutSigned32","NumPutBoolean"},
            msvcp_detail::facet_shape(".?AV?$num_put@"+c+"V?$ostreambuf_iterator@"+c+"U?$char_traits@"+c+"@std@@@std@@@std@@",l.p));};
        // unsigned short 字元版與 wchar_t 共用實作，另有自己的 vtable 與 RTTI。
        t.num_put=num_put_table(character);if(element)t.num_put16=num_put_table("G");
        // num_get 的 do_get 依 MSVC 多載反向宣告順序：3 void*&、4 long double、5 double、6 float、7 unsigned long long、8 long long、
        // 9 unsigned long、10 long、11 unsigned int、12 unsigned short、13 bool；unsigned short 字元版另有自己的 RTTI。
        const auto num_get_shape=[&](const std::string& c){return msvcp_detail::facet_shape(".?AV?$num_get@"+c+"V?$istreambuf_iterator@"+c+"U?$char_traits@"+c+"@std@@@std@@@std@@",l.p);};
        const auto num_get_table=[&](const std::string& c){return modern_table(variant,prefix,{"FacetDelete","FacetIncref","FacetDecref","NumGetPointer","NumGetLongDouble","NumGetDouble","NumGetFloat",
            "NumGetUnsigned64","NumGetSigned64","NumGetUnsignedLong","NumGetLong","NumGetUnsigned32","NumGetUnsigned16","NumGetBoolean"},num_get_shape(c));};
        t.num_get=num_get_table(character);if(element)t.num_get16=num_get_table("G");
        const auto time_put_table=[&](const std::string& c){return modern_table(variant,prefix,{"FacetDelete","FacetIncref","FacetDecref","TimePutDoPut"},
            msvcp_detail::facet_shape(".?AV?$time_put@"+c+"V?$ostreambuf_iterator@"+c+"U?$char_traits@"+c+"@std@@@std@@@std@@",l.p));};
        t.time_put=time_put_table(character);if(element)t.time_put16=time_put_table("G"); // unsigned short 版另有 RTTI，實作與 wchar_t 相同。
        // time_get 虛擬函式：do_date_order、do_get_time／date／weekday／monthname／year、do_get(規格)，與 get(格式範圍) 共用解析器。
        t.time_get=modern_table(variant,prefix,{"FacetDelete","FacetIncref","FacetDecref","TimeGetDateOrder","TimeGetTime","TimeGetDate","TimeGetWeekday","TimeGetMonth","TimeGetYear","TimeGetDoGet"},
            msvcp_detail::facet_shape(".?AV?$time_get@"+character+"V?$istreambuf_iterator@"+character+"U?$char_traits@"+character+"@std@@@std@@@std@@",l.p,".?AVtime_base@std@@"));
        t.numpunct=modern_table(variant,prefix,{"FacetDelete","FacetIncref","FacetDecref","NumPunctDecimal","NumPunctThousands","NumPunctGrouping","NumPunctFalseName","NumPunctTrueName"},msvcp_detail::facet_shape(".?AV?$numpunct@"+character+"@std@@",l.p));
        t.numpunct_byname=modern_table(variant,prefix,{"FacetDelete","FacetIncref","FacetDecref","NumPunctDecimal","NumPunctThousands","NumPunctGrouping","NumPunctFalseName","NumPunctTrueName"},msvcp_detail::facet_shape(".?AV?$numpunct_byname@"+character+"@std@@",l.p,".?AV?$numpunct@"+character+"@std@@"));
    }
    m.facet_table14=modern_table(variant,'W',{"FacetDelete","FacetIncref","FacetDecref"},msvcp_detail::facet_shape(".?AVfacet@locale@std@@",l.p));
    m.codecvt_base14=modern_table(variant,'D',{"FacetDelete","FacetIncref","FacetDecref","CodecvtBaseNoconv","CodecvtBaseMaxLength","CodecvtBaseEncoding"},msvcp_detail::facet_shape(".?AVcodecvt_base@std@@",l.p));
    m.ctype_base14=modern_table(variant,'W',{"FacetDelete","FacetIncref","FacetDecref"},msvcp_detail::facet_shape(".?AUctype_base@std@@",l.p));
    m.winit_count14=runtime_allocate(memory,{},4);m.data14.emplace("?_Init_cnt@_Winit@std@@0HA",m.winit_count14);
    m.locimp_table14=modern_table(variant,'W',{"FacetDelete","FacetIncref","FacetDecref"},msvcp_detail::facet_shape(".?AV_Locimp@locale@std@@",l.p));
    // _Facet_base 內聯建構／解構會寫入此 vtable；_Incref／_Decref 在這一層是純虛擬。
    msvc_rtti::Shape facet_base_shape;facet_base_shape.bases.push_back({".?AV_Facet_base@std@@",0});
    const auto facet_base_table=modern_table(variant,'W',{"FacetDelete","FacetIncref","FacetDecref"},std::move(facet_base_shape));
    for(unsigned slot=1;slot<3;++slot)put(facet_base_table+slot*l.p,resolve("crt","_purecall").value);
    m.ios_base_table14=modern_table(variant,'D',{"IosDelete"},msvcp_detail::stream_shape("ios_base",false,l.p));
    m.ctype_masks14=runtime_allocate(memory,{},512);for(unsigned c=0;c<256;++c)memory.store(GuestAddress{m.ctype_masks14+c*2},16,crt_c_locale::character_mask(c));
    const Layout modern(x86_,true,1,false,variant!=0);m.global14=runtime_allocate(memory,{},96);m.classic14=m.global14;put(m.global14,m.locimp_table14);memory.store(GuestAddress{m.global14+l.p},32,1);
    const auto modern_facets=runtime_allocate(memory,{},9*l.p);put(m.global14+2*l.p,modern_facets);put(m.global14+3*l.p,9);memory.store(GuestAddress{m.global14+4*l.p},32,0x3f);put(m.global14+4*l.p+8,name);
    // locale::classic 的常駐物件與 MSVC 相同共用 classic _Locimp 的基礎參考，不另外累加。
    m.classic_locale14=runtime_allocate(memory,{},modern.locale_size());put(m.classic_locale14+modern.locale_pointer(),m.classic14);
    m.empty_wide14=runtime_allocate(memory,{},2);
    m.init_count14=runtime_allocate(memory,{},4);memory.store(GuestAddress{m.init_count14},32,UINT32_MAX);m.data14.emplace("?_Init_cnt@Init@ios_base@std@@0HA",m.init_count14);
    m.ios_index14=runtime_allocate(memory,{},4);memory.store(GuestAddress{m.ios_index14},32,0);m.data14.emplace("?_Index@ios_base@std@@0HA",m.ios_index14);
    const auto classic_group=runtime_allocate(memory,std::array<std::uint8_t,1>{0});
    for(unsigned element=0;element<2;++element){const auto characters=element?2U:1U;const auto& tables=m.tables14[element];
        const auto type=runtime_allocate(memory,{},96);put(type,tables.ctype);memory.store(GuestAddress{type+l.p},32,1);
        put(type+2*l.p+(x86_?4U:8U),m.ctype_masks14);s.facet_locales.emplace(type,Msvcp100State::FacetLocale{});
        const auto num=runtime_allocate(memory,{},2*l.p);put(num,tables.num_put);memory.store(GuestAddress{num+l.p},32,1);
        const auto punct=runtime_allocate(memory,{},6*l.p);put(punct,tables.numpunct);memory.store(GuestAddress{punct+l.p},32,1);put(punct+2*l.p,classic_group);
        memory.store(GuestAddress{punct+3*l.p},static_cast<std::uint8_t>(characters*8),'.');memory.store(GuestAddress{punct+3*l.p+characters},static_cast<std::uint8_t>(characters*8),',');
        const auto boolean_name=[&](std::string_view text){std::vector<std::uint8_t> bytes((text.size()+1)*characters);for(std::size_t i=0;i<text.size();++i)bytes[i*characters]=static_cast<std::uint8_t>(text[i]);return runtime_allocate(memory,bytes);};
        put(punct+4*l.p,boolean_name("false"));put(punct+5*l.p,boolean_name("true"));
        put(modern_facets+(element?6U:1U)*l.p,type);put(modern_facets+(element?7U:4U)*l.p,num);put(modern_facets+(element?8U:5U)*l.p,punct);
    }
    for(const auto& [symbol,id]:std::array<std::pair<const char*,unsigned>,8>{{
        {"?id@?$ctype@D@std@@2V0locale@2@A",1},{"?id@?$codecvt@_WDU_Mbstatet@@@std@@2V0locale@2@A",2},{"?id@?$collate@D@std@@2V0locale@2@A",3},
        {"?id@?$num_put@DV?$ostreambuf_iterator@DU?$char_traits@D@std@@@std@@@std@@2V0locale@2@A",4},
        {"?id@?$numpunct@D@std@@2V0locale@2@A",5},
        {"?id@?$ctype@_W@std@@2V0locale@2@A",6},
        {"?id@?$num_put@_WV?$ostreambuf_iterator@_WU?$char_traits@_W@std@@@std@@@std@@2V0locale@2@A",7},
        {"?id@?$numpunct@_W@std@@2V0locale@2@A",8}}}){
        const auto address=runtime_allocate(memory,{},l.p);put(address,id);m.data14.emplace(symbol,address);
    }
    // 不在 classic locale 預建清單的 facet：id 為 0，第一次 use_facet 時由 locale::id 配發，再經 _Getcat 建立。
    for(const auto* symbol:{"?id@?$codecvt@DDU_Mbstatet@@@std@@2V0locale@2@A","?id@?$collate@_W@std@@2V0locale@2@A",
        "?id@?$time_get@DV?$istreambuf_iterator@DU?$char_traits@D@std@@@std@@@std@@2V0locale@2@A",
        "?id@?$num_get@DV?$istreambuf_iterator@DU?$char_traits@D@std@@@std@@@std@@2V0locale@2@A",
        "?id@?$num_get@_WV?$istreambuf_iterator@_WU?$char_traits@_W@std@@@std@@@std@@2V0locale@2@A",
        "?id@?$num_get@GV?$istreambuf_iterator@GU?$char_traits@G@std@@@std@@@std@@2V0locale@2@A",
        "?id@?$num_put@GV?$ostreambuf_iterator@GU?$char_traits@G@std@@@std@@@std@@2V0locale@2@A",
        "?id@?$time_get@_WV?$istreambuf_iterator@_WU?$char_traits@_W@std@@@std@@@std@@2V0locale@2@A",
        "?id@?$time_put@GV?$ostreambuf_iterator@GU?$char_traits@G@std@@@std@@@std@@2V0locale@2@A",
        "?id@?$codecvt@_SDU_Mbstatet@@@std@@2V0locale@2@A","?id@?$codecvt@_UDU_Mbstatet@@@std@@2V0locale@2@A","?id@?$codecvt@GDU_Mbstatet@@@std@@2V0locale@2@A",
        "?id@?$time_put@DV?$ostreambuf_iterator@DU?$char_traits@D@std@@@std@@@std@@2V0locale@2@A",
        "?id@?$time_put@_WV?$ostreambuf_iterator@_WU?$char_traits@_W@std@@@std@@@std@@2V0locale@2@A","?id@?$ctype@G@std@@2V0locale@2@A"}){
        const auto address=runtime_allocate(memory,{},l.p);put(address,0);m.data14.emplace(symbol,address);
    }
    // ctype<char>::table_size（靜態常數 256，x86 為 unsigned int、x64 為 size_t）。
    {const auto address=runtime_allocate(memory,{},l.p);put(address,256);m.data14.emplace(x86_?"?table_size@?$ctype@D@std@@2IB":"?table_size@?$ctype@D@std@@2_KB",address);}
    for(unsigned element=0;element<2;++element){const auto character=element?"_W":"D";const auto& tables=m.tables14[element];
        m.data14.emplace(std::string("??_7?$numpunct@")+character+"@std@@6B@",tables.numpunct);
        m.data14.emplace(std::string("??_7?$num_put@")+character+"V?$ostreambuf_iterator@"+character+"U?$char_traits@"+character+"@std@@@std@@@std@@6B@",tables.num_put);
    }
    // locale::id::_Id_cnt 是匯出的 static int；inline locale::id 轉換與 ??Bid 共用同一個客體計數，已預建 8 個標準 facet id。
    m.id_count14=runtime_allocate(memory,{},4);memory.store(GuestAddress{m.id_count14},32,8);m.data14.emplace("?_Id_cnt@id@locale@std@@0HA",m.id_count14);
    // _Locimp 的虛擬表與 static _Clocptr（classic locale 的 _Locimp）。
    m.data14.emplace("??_7_Locimp@locale@std@@6B@",m.locimp_table14);
    {const auto cell=runtime_allocate(memory,{},l.p);put(cell,m.classic14);m.data14.emplace(x86_?"?_Clocptr@_Locimp@locale@std@@0PAV123@A":"?_Clocptr@_Locimp@locale@std@@0PEAV123@EA",cell);}
    if(!variant){s.ostream_vb=runtime_allocate(memory,{},8);memory.store(GuestAddress{s.ostream_vb+4},32,8);}
    m.ostream_vb14=runtime_allocate(memory,{},8);memory.store(GuestAddress{m.ostream_vb14+4},32,modern.ostream_base());
    m.istream_vb14=runtime_allocate(memory,{},8);memory.store(GuestAddress{m.istream_vb14+4},32,modern.istream_base());
    m.iostream_vb14=runtime_allocate(memory,{},16);memory.store(GuestAddress{m.iostream_vb14+4},32,modern.iostream_base());
    memory.store(GuestAddress{m.iostream_vb14+8},32,static_cast<std::uint32_t>(-16));memory.store(GuestAddress{m.iostream_vb14+12},32,modern.iostream_base()-16);
    // 標準串流使用相同客體 ABI 與 streambuf 虛擬派送，輸出交給既有 Windows 標準 handle。
    // cout／cerr／clog 各有自己的 streambuf 與 locale 參考；cerr 預設 unitbuf 並 tie 到 cout，與 MSVC 初始化相同。
    const auto standard_ostream=[&](unsigned handle,std::uint64_t index,std::uint32_t flags,std::uint64_t tie){
        const auto console_buffer=runtime_allocate(memory,{},modern.buffer_size);put(console_buffer,m.tables14[0].buffer);
        for(unsigned side=0;side<2;++side){put(console_buffer+modern.first_indirect(side),console_buffer+modern.first(side));put(console_buffer+modern.next_indirect(side),console_buffer+modern.next(side));put(console_buffer+modern.count_indirect(side),console_buffer+modern.count(side));}
        const auto buffer_locale=runtime_allocate(memory,{},modern.locale_size());put(buffer_locale+modern.locale_pointer(),m.global14);put(console_buffer+modern.buffer_locale(),buffer_locale);s.standard_buffers.emplace(console_buffer,handle);
        const auto stream=runtime_allocate(memory,{},modern.ostream_base()+modern.ios_size+3*l.p);put(stream,m.ostream_vb14);const auto stream_ios=stream+modern.ostream_base();put(stream_ios,m.tables14[0].ostream);memory.store(GuestAddress{stream_ios-4},32,0);
        put(stream_ios+l.p,index);memory.store(GuestAddress{stream_ios+modern.flags},32,flags);memory.store(GuestAddress{stream_ios+modern.precision},64,6);
        const auto stream_locale=runtime_allocate(memory,{},modern.locale_size());put(stream_locale+modern.locale_pointer(),m.global14);put(stream_ios+modern.locale,stream_locale);put(stream_ios+modern.ios_size,console_buffer);
        put(stream_ios+modern.ios_size+l.p,tie);memory.store(GuestAddress{stream_ios+modern.ios_size+2*l.p},8,' ');
        memory.store(GuestAddress{m.global14+l.p},32,memory.load(GuestAddress{m.global14+l.p},32)+2);s.ios_objects.emplace(stream_ios,std::pair{stream,modern.ostream_base()+modern.ios_size+3*l.p});return stream;
    };
    const auto cout=standard_ostream(1,1,0x201,0);
    m.data14.emplace("?cout@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A",cout);
    m.data14.emplace("?cerr@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A",standard_ostream(2,2,0x203,cout));
    m.data14.emplace("?clog@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A",standard_ostream(2,3,0x201,0));
    if(variant){std::map<std::string,std::uint64_t,std::less<>> renamed;for(auto& [symbol,address]:m.data14)renamed.emplace(msvcp120_symbol(symbol),address);m.data14=std::move(renamed);}
    };
    initialize_modern(0);initialize_modern(1);
    s.exception_control_table=modern_table(0,'D',{"ExceptionControlDestroy","ExceptionControlDelete","ExceptionControlDestructor","ExceptionControlDeleter"});
    for(unsigned i=0;i<3;++i){const std::array<std::string_view,3> names{"generic","system","iostream"};const std::array<std::string,3> types{".?AV_Generic_error_category@std@@",".?AV_System_error_category@std@@",".?AV_Iostream_error_category2@std@@"};
        msvc_rtti::Shape shape;shape.bases={{types[i],1},{".?AVerror_category@std@@",0}};
        const auto category_table=modern_table(0,'D',{"ErrorCategoryDelete","ErrorCategoryName","ErrorCategoryMessage","ErrorCategoryCondition","ErrorCategoryEquivalentB","ErrorCategoryEquivalentA"},std::move(shape));
        const auto category=runtime_allocate(memory,{},2*l.p);put(category,category_table);put(category+l.p,i==0?3:i==1?7:5);s.error_categories[i]=category;
        std::vector<std::uint8_t> text(names[i].begin(),names[i].end());text.push_back(0);s.error_category_names[i]=runtime_allocate(memory,text);}
    // noexcept current_exception 的配置失敗分支使用預先配置的實際型別／例外紀錄。
    initialize_msvc_exception_types(memory);
    for(const auto kind:{MsvcExceptionKind::BadAlloc,MsvcExceptionKind::BadException}){
        const auto control=runtime_allocate(memory,{},x86_?128:224),record=control+l.p+8,object=control+(x86_?96:176);const auto i=static_cast<unsigned>(kind);
        put(control,s.exception_control_table);memory.store(GuestAddress{control+l.p},32,1);memory.store(GuestAddress{control+l.p+4},32,1);
        put(object,runtime_.cxx_exceptions.vtable[i]);put(object+l.p,runtime_.cxx_exceptions.message[i]);
        msvc_detail::ExceptionRecord saved;saved.code=0xe06d7363U;saved.flags=1;saved.parameters={0x19930520U,object,runtime_.cxx_exceptions.throw_info[i]};if(!x86_)saved.parameters.push_back(runtime_.cxx_exceptions.image.base);
        msvc_detail::write_exception_record(memory,record,x86_,saved);s.exception_pointers.emplace(control,Msvcp100State::CapturedException{record,object,0,false,true,true});
        (kind==MsvcExceptionKind::BadAlloc?s.bad_alloc_pointer:s.bad_exception_pointer)=control;
    }
    const auto rtti_image=runtime_allocate(memory,{},rtti.bytes.size(),l.p);rtti.relocate(rtti_image);memory.write(GuestAddress{rtti_image},rtti.bytes);
    for(const auto& [cell,locator]:rtti_tables)put(cell,rtti_image+locator);
    for(auto& runtime:s.runtimes)for(const auto& [type_name,offset]:rtti.types)runtime.data14.emplace("??_R0"+type_name.substr(1)+"@8",rtti_image+offset);
}
void WindowsServices::initialize_msvcp_numeric_facet(GuestState& state,GuestMemory& memory,std::uint64_t object,bool punct,bool wide,const CrtLocaleCategory& category,std::uint32_t references,bool defaults,unsigned variant){
    auto& s=*msvcp100_;const unsigned unit=x86_?4:8,characters=wide?2:1;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto& tables=s.runtimes[variant].tables14[wide?1:0];Msvcp100State::FacetLocale metadata;metadata.category=category;metadata.resources.reserve(3);
    const auto put=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},bits,value);};
    const auto save=[&](std::vector<std::uint8_t> bytes){const auto at=allocate_heap(memory,bytes.size());if(!at)throw std::bad_alloc();metadata.resources.push_back(at);memory.write(GuestAddress{at},bytes);return at;};
    try{
        memory.fill(GuestAddress{object},0,(punct?6U:2U)*unit);put(object,punct?tables.numpunct:tables.num_put);memory.store(GuestAddress{object+unit},32,references);
        if(punct){const auto numeric=defaults?CrtNumericData{}:windows_crt_numeric(category);
            const auto character=[&](std::u16string_view text){if(text.empty())return std::uint16_t{};if(wide)return static_cast<std::uint16_t>(text.front());
                const auto bytes=windows_crt_encode(text,category.codepage);if(!bytes || bytes->empty())throw std::invalid_argument("numpunct 分隔符無法轉為客體碼頁");return static_cast<std::uint16_t>(bytes->front());};
            std::vector<std::uint8_t> grouping(numeric.grouping.begin(),numeric.grouping.end());grouping.push_back(0);put(object+2*unit,save(std::move(grouping)));
            memory.store(GuestAddress{object+3*unit},static_cast<std::uint8_t>(characters*8),character(numeric.decimal));
            const auto separator=defaults || !category.codepage?static_cast<std::uint16_t>(','):character(numeric.thousands);
            memory.store(GuestAddress{object+3*unit+characters},static_cast<std::uint8_t>(characters*8),separator);
            const auto name=[&](std::string_view text){std::vector<std::uint8_t> bytes((text.size()+1)*characters);for(std::size_t i=0;i<text.size();++i)bytes[i*characters]=static_cast<std::uint8_t>(text[i]);return save(std::move(bytes));};
            put(object+4*unit,name("false"));put(object+5*unit,name("true"));
        }
        if(!s.facet_locales.emplace(object,metadata).second)throw HostCallFault("數值 facet 已有未釋放的來源資料");metadata.resources.clear();
    }catch(...){for(const auto address:metadata.resources)(void)free_heap(memory,address,state);throw;}
}
void WindowsServices::queue_msvcp_facet_retirement(std::uint64_t facet){
    if(!facet || !threads_.items.contains(threads_.current) || threads_.items.at(threads_.current).status==ThreadStatus::Exited)return;
    if(gui_.sent_messages.size()>=8192)throw std::bad_alloc();GuiSentMessage request;request.receiver=threads_.current;request.abandoned=true;
    request.rpc=GuestCallback{resolve("internal-msvcp140",x86_?"DFacetRetire32":"DFacetRetire64").value,{facet}};
    gui_.sent_messages.emplace(gui_.next_object++,std::move(request));
}
GuestAddress WindowsServices::msvcp100_data(std::string_view name,bool modern,bool vc12)const{
    if(!msvcp100_)return GuestAddress{};const auto& data=modern?msvcp100_->runtimes[vc12?1:0].data14:msvcp100_->data;const auto it=data.find(name);return it==data.end()?GuestAddress{}:GuestAddress{it->second};
}
void WindowsServices::prepare_msvcp_exception_capture(GuestState& state,GuestMemory& memory,std::uint64_t token){
    auto& s=*msvcp100_;auto& op=s.operations.at(token);const Layout l(x86_,true,1);const auto bits=static_cast<std::uint8_t>(l.p*8);
    if(op.scratch){s.exception_pointers.erase(op.scratch);(void)free_heap(memory,op.scratch,state);op.scratch=0;}
    op.phase=0;op.callback=0;op.buffer=0;op.object=0;op.started=false;op.skip=false;
    if(op.copy_failures>1 || op.exception_record.code==0xe0434f4dU || op.exception_record.code==0xe0434352U){op.final_result=s.bad_exception_pointer;return;}
    try{
        const auto type=op.exception_record.cpp()?msvc_detail::exception_type(memory,op.exception_record,x86_):msvc_detail::ExceptionType{};
        const auto header=x86_?96U:176U;const auto allocation=allocate_heap(memory,header+type.size);if(!allocation){op.final_result=s.bad_alloc_pointer;return;}
        try{
            const auto record=allocation+l.p+8,object=type.size?allocation+header:0;memory.fill(GuestAddress{allocation},0,static_cast<std::size_t>(header+type.size));
            if(type.size){if(!type.copy)memory.move(GuestAddress{object},GuestAddress{type.source},static_cast<std::size_t>(type.size));op.exception_record.parameters[1]=object;}
            memory.store(GuestAddress{allocation},bits,s.exception_control_table);memory.store(GuestAddress{allocation+l.p},32,1);memory.store(GuestAddress{allocation+l.p+4},32,1);
            msvc_detail::write_exception_record(memory,record,x86_,op.exception_record);s.exception_pointers.emplace(allocation,Msvcp100State::CapturedException{record,object,type.destroy,type.interface_handle(),false,false});
            op.scratch=allocation;op.final_result=allocation;op.object=object;op.buffer=type.copy;op.count=type.source;op.skip=type.virtual_base();op.started=type.interface_handle();
        }catch(...){s.exception_pointers.erase(allocation);(void)free_heap(memory,allocation,state);throw;}
    }catch(const std::bad_alloc&){op.final_result=s.bad_alloc_pointer;}
}
std::optional<std::uint32_t> WindowsServices::finish_msvcp_callback_exception(GuestState& state,GuestMemory& memory,std::size_t depth){
    if(depth>=runtime_.callbacks.size())throw HostCallFault("STL 例外邊界沒有 callback");const auto frame=runtime_.callbacks[depth];auto& s=*msvcp100_;auto& op=s.operations.at(frame.value);
    auto record=msvc_detail::exception_record(memory,exceptions_.stack.back().record,x86_);record.flags&=~(2U|4U|0x10U|0x20U|0x40U);
    const auto captured_depth=exceptions_.stack.size()-1;if(op.caught_exception_base==SIZE_MAX)op.caught_exception_base=frame.exception_depth;
    discard_callbacks(depth+1,state,memory);if(runtime_.callbacks.size()!=depth+1)throw HostCallFault("STL callback 邊界深度無效");
    const auto finished=runtime_.callbacks.back();runtime_.callbacks.pop_back();finish_shell_callbacks(memory,finished);state=frame.saved;
    exceptions_.stack.at(captured_depth).msvc.catching=true;exceptions_.stack.at(captured_depth).msvc.entered_catch=true;
    exceptions_.stack.at(captured_depth).protected_callback_depth=SIZE_MAX;(void)msvc_exception_cells(state,memory,false,true);
    if(op.kind=="exception_capture"){op.exception_record=std::move(record);++op.copy_failures;prepare_msvcp_exception_capture(state,memory,frame.value);return resume_msvcp100(state,memory,frame.value,0);}
    if(op.ios){
        const Layout l(x86_,op.modern,op.element,op.service.family=="msvcp90.dll" || op.service.family=="internal-msvcp90",msvcp_detail::vc12(op.service));const auto at=op.ios+l.state;
        const bool suffix=op.callback==52;
        const bool body=op.callback!=10 && op.callback!=11 && !suffix;const auto flags=memory.load(GuestAddress{at},32)|(body?4U:0U);if(body)memory.store(GuestAddress{at},32,flags);
        op.rethrow_exception=!suffix && (!body || (memory.load(GuestAddress{at+4},32)&4U));op.throw_failure=false;op.exception_record=std::move(record);op.callback=0;
        op.completed=true;if(!suffix)op.final_result=op.kind=="prefix"?0:op.kind=="peek"?l.eof():op.kind=="ostream_suffix"?0:op.object;
        if(op.kind=="getline" && op.limit)memory.store(GuestAddress{op.output+op.count*l.element},l.character_bits(),0);
        if(!suffix && (op.kind=="tellg" || op.kind=="tellp")){memory.fill(GuestAddress{op.output},0,24);memory.store(GuestAddress{op.output},64,UINT64_MAX);op.final_result=op.output;}
        return resume_msvcp100(state,memory,frame.value,0);
    }
    throw HostCallFault("STL 例外邊界沒有恢復流程");
}
void WindowsServices::discard_msvcp100(std::uint64_t token,GuestState& state,GuestMemory& memory){
    if(!msvcp100_)return;const auto found=msvcp100_->operations.find(token);if(found==msvcp100_->operations.end())return;
    auto operation=std::move(found->second);msvcp100_->operations.erase(found);
    if(operation.kind=="clone_locale_imp"){
        const auto bits=static_cast<std::uint8_t>(x86_?32:64);const unsigned unit=x86_?4:8;
        for(std::uint64_t i=0;i<operation.count;++i){const auto facet=memory.load(GuestAddress{operation.output+i*unit},bits);if(facet)queue_msvcp_facet_retirement(facet);}
        const auto label=operation.object?memory.load(GuestAddress{operation.object+4*unit+8},bits):0;
        msvcp100_->owned_locale_imps.erase(operation.object);
        if(label)(void)free_heap(memory,label,state);if(operation.output)(void)free_heap(memory,operation.output,state);if(operation.object)(void)free_heap(memory,operation.object,state);
    }
    if(operation.locale_facet_owned && operation.locale_pending)queue_msvcp_facet_retirement(operation.locale_pending);
    if(operation.locale_source_owned && operation.locale_source)queue_msvcp_facet_retirement(operation.locale_source);
    if(operation.locale_lock_owned && operation.locale_lock_storage && threads_.items.contains(threads_.current) && threads_.items.at(threads_.current).status!=ThreadStatus::Exited){
        GuiSentMessage request;request.receiver=threads_.current;request.abandoned=true;
        request.rpc=GuestCallback{resolve("internal-msvcp140",x86_?"DLocaleUnlock32":"DLocaleUnlock64").value,{operation.locale_lock_storage}};
        gui_.sent_messages.emplace(gui_.next_object++,std::move(request));operation.locale_lock_storage=0;
    }
    if(operation.locale_lock_storage)(void)free_heap(memory,operation.locale_lock_storage,state);
    for(const auto address:operation.numeric_allocations)(void)free_heap(memory,address,state);
    while(operation.numeric_held)queue_msvcp_facet_retirement(operation.numeric_facets[--operation.numeric_held]);
    if(operation.scratch){msvcp100_->exception_pointers.erase(operation.scratch);(void)free_heap(memory,operation.scratch,state);}
}

// worker 端（x64，持共用映射鎖）：ctype<char>::toupper(char)／tolower(char) 的公開包裝在虛擬表仍是內建基底實作時，
// 直接查 facet 語系的大小寫表（與下方 CtypeUpper／CtypeLower 的 convert 相同）。facet 表只在協調者完整路徑增刪
// （全停或獨占映射鎖），這裡唯讀。衍生 facet、表內沒有紀錄或記憶體不可讀時交回協調者，不改任何狀態。
bool WindowsServices::msvcp_ctype_case_worker(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(x86_ || !msvcp100_)return false;
    const auto l=msvcp_detail::abi(x86_,service);if(!l.modern || l.element!=1)return false;
    const bool upper=service.symbol.starts_with("?toupper@");
    const auto object=state.reg(Gpr::Rcx);const unsigned slot=3U+(upper?2U:0U)+1U;
    const auto entry=resolve_service(msvcp_detail::modern_internal(l.variant()),upper?"DCtypeUpper64":"DCtypeLower64",false);if(!entry.value)return false;
    const auto table_pointer=memory.load(GuestAddress{object},64);
    if(memory.load(GuestAddress{table_pointer+slot*8U},64)!=entry.value)return false;
    const auto locale=msvcp100_->facet_locales.find(object);if(locale==msvcp100_->facet_locales.end())return false;
    const auto& table=windows_crt_character_table(locale->second.category);
    const auto target=memory.load(GuestAddress{state.reg(Gpr::Rsp)},64);
    state.reg(Gpr::Rax)=(upper?table.upper:table.lower)[state.reg(Gpr::Rdx)&255U];state.reg(Gpr::Rsp)+=8;state.rip=GuestAddress{target};return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_msvcp100(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);auto& s=*msvcp100_;const auto l=msvcp_detail::abi(x86_,service);const auto bits=static_cast<std::uint8_t>(l.p*8);
    auto& m=s.runtimes[l.variant()];const auto* const modern_internal=msvcp_detail::modern_internal(l.variant());
    const auto name=service.symbol;const bool internal=service.family=="internal-msvcp100" || service.family=="internal-msvcp140" || service.family=="internal-msvcp120" || service.family=="internal-msvcp90";
    if(l.modern && !m.global14)m.global14=m.classic14;
    const auto global=l.modern?m.global14:s.global;
    const auto& tables=m.tables14[l.element==1?0:1];const auto buffer_table=l.modern?tables.buffer:l.legacy?s.buffer_table9[l.element==1?0:1]:l.element==1?s.buffer_table_narrow:s.buffer_table,ios_table=l.modern?tables.ios:s.ios_table;
    const bool stat=service.caller_cleanup || name.starts_with("?_Getcat@") || name.starts_with("?_Getgloballocale@") || name.starts_with("?_Ios_base_dtor@");
    const auto object=stat?0:x86_?state.reg(Gpr::Rcx)&UINT32_MAX:call(0);
    const auto a=[&](unsigned i){return call(i+(!x86_ && !stat?1:0));};
    const auto ptr=[&](std::uint64_t at){return memory.load(GuestAddress{at},bits);};
    const auto put=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},bits,value);};
    const auto done=[&](std::uint64_t value,bool wide=false)->std::optional<std::uint32_t>{call.return_to(state,value,wide);return {};};
    const bool prefixed=l.modern || name.starts_with("DStream") || name.starts_with("WStream");
    const auto method=internal?name.substr(prefixed?1:0,name.size()-(prefixed?3:2)):name.substr(1,name.find('@')-1);
    const auto refs=[&](std::uint64_t imp){return memory.load(GuestAddress{imp+l.p},l.reference_bits());};
    const auto put_refs=[&](std::uint64_t imp,std::uint64_t n){memory.store(GuestAddress{imp+l.p},l.reference_bits(),n);};
    const auto virtual_call=[&](std::uint64_t self,unsigned slot,std::vector<std::uint64_t> args)->std::optional<std::uint32_t>{
        args.insert(args.begin(),self);GuestCallback cb{ptr(ptr(self)+l.slot(slot)*l.p),std::move(args)};cb.x86_register_arguments=x86_?1:0;
        start_callbacks(state,memory,{std::move(cb)},Continuation::ReturnCallback);return {};
    };
    // x86 上回傳 64 位元（streamsize）的虛擬函式：一般回呼只帶回 EAX，改以完整規格取回 EDX:EAX 再交給呼叫端。
    const auto virtual_call_wide=[&](std::uint64_t self,unsigned slot,std::vector<std::uint64_t> args)->std::optional<std::uint32_t>{
        args.insert(args.begin(),self);GuestCallback cb{ptr(ptr(self)+l.slot(slot)*l.p),std::move(args)};cb.x86_register_arguments=x86_?1:0;cb.wide_result=true;
        return nested_continuation(state,memory,"msvcp virtual streamsize",[service,cb](GuestState& s2,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const auto value=co_await service_callback::call_native(cb);const WindowsCall result(s2,m,service);result.return_to(s2,value,true);co_return std::nullopt;});
    };
    const auto new_locale=[&]{const auto loc=allocate_heap(memory,l.locale_size());if(!loc)throw std::bad_alloc{};memory.fill(GuestAddress{loc},0,l.locale_size());put(loc+l.locale_pointer(),global);put_refs(global,refs(global)+1);return loc;};
    // _Cvtvec／_Ctypevec 與 VC11／VC12 的 C 匯出共用 windows_msvcp_runtime.cpp；表配置失敗時退回常駐 classic 表。
    const auto write_cvtvec=[&](std::uint64_t output){msvcp_cvt_vector(state,memory,output);};
    const auto write_ctypevec=[&](std::uint64_t output){if(!msvcp_ctype_vector(state,memory,output))put(output+(x86_?4U:8U),m.ctype_masks14);};
    // ctype 的 _Tidy：_Ctypevec＝{碼頁, 表, _Delfl, 語系名稱}；_Delfl>0 以 free、<0 以 delete[] 配置（兩者都是本執行階段的堆積），語系名稱以 free。
    // 釋放後把 _Delfl 與名稱清零，避免解構與刪除重複釋放。
    const auto tidy_msvcp_ctype=[&](std::uint64_t facet){
        const auto vector=facet+2*l.p,table_at=vector+(x86_?4U:8U),delfl_at=vector+2*l.p,name_at=vector+3*l.p;
        const auto table=ptr(table_at);const auto delfl=memory.load(GuestAddress{delfl_at},32);
        if(table && delfl && table!=m.ctype_masks14)(void)free_heap(memory,table,state);
        if(const auto label=ptr(name_at))(void)free_heap(memory,label,state);
        memory.store(GuestAddress{delfl_at},32,0);put(name_at,0);
    };
    if(!internal && name.starts_with('_')){
        // C 匯出：x64 浮點引數在與參數序相同的 XMM，Win32 cdecl 在堆疊；回傳值分別走 XMM0 與 x87 ST(0)。
        if(name=="_Getctype"){write_ctypevec(a(0));return done(a(0));}
        if(name=="_Getcvt"){write_cvtvec(a(0));return done(a(0));}
        if(name=="_Lock_shared_ptr_spin_lock"){
            // 協調者上的旗標只由這對匯出存取；被占用時讓出客體執行緒再試，不佔住宿主服務。
            if(!s.shared_ptr_locked){s.shared_ptr_locked=true;return done(0);}
            Operation op;op.service=service;op.modern=true;op.element=1;op.kind="shared_ptr_lock";
            const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
        }
        if(name=="_Unlock_shared_ptr_spin_lock"){s.shared_ptr_locked=false;return done(0);}
        throw HostCallFault("未知 STL C 匯出");
    }
#include "msvcp90_file_handlers.inc"
    if(method=="FacetRetire"){
        Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="retire_facet";op.object=a(0);
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
    if(method=="LocaleUnlock"){
        GuestCallback callback{resolve("msvcp140.dll",x86_?"??1_Lockit@std@@QAE@XZ":"??1_Lockit@std@@QEAA@XZ").value,{a(0)}};
        callback.x86_register_arguments=x86_?1:0;callback.exception_policy=GuestExceptionPolicy::NoexceptCpp;
        start_callbacks(state,memory,{std::move(callback)},Continuation::ReturnZeroFreeHeap,a(0));return {};
    }
    if(l.modern && method=="_Setgloballocale"){m.global14=a(0);return done(0);}
    if(l.modern && method=="global" && name.find("@locale@")!=name.npos){
        memory.check(GuestAddress{a(0)},l.locale_size(),Access::Write);const auto replacement=ptr(a(1)+l.locale_pointer());
        if(!replacement)throw HostCallFault("STL 全域 locale 來源為空");memory.check(GuestAddress{replacement},x86_?32:56,Access::Read);
        Operation op;op.service=service;op.modern=true;op.element=1;op.kind="global_locale";op.object=replacement;op.output=a(0);op.locale_lock_storage=allocate_heap(memory,16);
        if(!op.locale_lock_storage){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
        memory.fill(GuestAddress{op.locale_lock_storage},0,16);const auto token=s.next++;
        try{s.operations.emplace(token,std::move(op));}catch(...){(void)free_heap(memory,op.locale_lock_storage,state);throw;}return resume_msvcp100(state,memory,token,0);
    }
    const bool numeric_punct=name.find("numpunct@")!=name.npos || name.find("numpunct_byname@")!=name.npos;
    const bool numeric_put=name.find("num_put@")!=name.npos;
    // unsigned short 字元版的 num_put 使用自己的 vtable（RTTI 不同於 wchar_t）。
    const bool numeric_put16=numeric_put && name.find("num_put@G")!=name.npos;
    if(l.modern && (numeric_punct || numeric_put) && (name.starts_with("??0") || (numeric_put && name.starts_with("??_F")))){
        auto category=current_crt_locale(state,memory).categories[4];const bool byname=name.find("numpunct_byname@")!=name.npos,closure=name.starts_with("??_F");
        if(byname){const auto named=windows_crt_locale(ansi_to_utf16(read_ansi(memory,a(0),4095)));if(!named){throw_msvc_exception(state,memory,service,MsvcExceptionKind::RuntimeError);return {};}category=*named;}
        // refs 位於 byname 名稱或 _Locinfo 之後；預設建構閉包為 refs 0、無回傳值。
        const auto references=closure?0U:static_cast<std::uint32_t>(a(byname || name.find("_Locinfo@")!=name.npos?1:0));
        initialize_msvcp_numeric_facet(state,memory,object,numeric_punct,l.element==2,category,references,false,l.variant());if(byname)put(object,tables.numpunct_byname);if(numeric_put16)put(object,tables.num_put16);
        return done(closure?0:object);
    }
    if(l.modern && numeric_put && method=="_Init")return done(0); // num_put::_Init 不從 _Locinfo 載入任何資料。
    if(l.modern && (numeric_punct || numeric_put) && name.starts_with("??1")){
        if(const auto found=s.facet_locales.find(object);found!=s.facet_locales.end()){for(const auto resource:found->second.resources)(void)free_heap(memory,resource,state);s.facet_locales.erase(found);}
        put(object,m.facet_table14);return done(0);
    }
    if(l.modern && (numeric_punct || numeric_put) && method=="_Getcat"){
        if(a(0) && !ptr(a(0))){const auto imp=a(1)?ptr(a(1)+l.locale_pointer()):global;const auto label=ptr(imp+4*l.p+8);
            const auto text=label?ansi_to_utf16(read_ansi(memory,label,4095)):std::u16string{u"C"};
            const auto category=text==u"*"?std::optional<CrtLocaleCategory>{current_crt_locale(state,memory).categories[4]}:windows_crt_locale(text);
            if(!category){throw_msvc_exception(state,memory,service,MsvcExceptionKind::RuntimeError);return {};}
            const auto facet=allocate_heap(memory,(numeric_punct?6U:2U)*l.p);if(!facet){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
            try{initialize_msvcp_numeric_facet(state,memory,facet,numeric_punct,l.element==2,*category,0,numeric_punct,l.variant());if(numeric_put16)put(facet,tables.num_put16);put(a(0),facet);}
            catch(...){if(const auto record=s.facet_locales.find(facet);record!=s.facet_locales.end()){for(const auto address:record->second.resources)(void)free_heap(memory,address,state);s.facet_locales.erase(record);}(void)free_heap(memory,facet,state);throw;}
        }return done(8);
    }
    if(numeric_punct && !internal){
        const auto slot=method=="decimal_point"?3U:method=="thousands_sep"?4U:method=="grouping"?5U:method=="falsename"?6U:method=="truename"?7U:0U;
        if(slot)return virtual_call(object,slot,slot<5?std::vector<std::uint64_t>{}:std::vector<std::uint64_t>{a(0)});
    }
    if(method.starts_with("NumPunct")){
        if(method=="NumPunctDecimal" || method=="NumPunctThousands")return done(memory.load(GuestAddress{object+3*l.p+(method=="NumPunctThousands"?l.element:0U)},l.character_bits()));
        const bool grouping=method=="NumPunctGrouping";const unsigned characters=grouping?1:l.element;
        const auto source=ptr(object+(grouping?2U:method=="NumPunctFalseName"?4U:5U)*l.p),output=a(0);
        std::u16string text;if(characters==2)text=read_utf16(memory,GuestAddress{source},1048576);else{const auto bytes=read_ansi(memory,source,1048576);text.assign(bytes.begin(),bytes.end());}
        const auto inline_capacity=16U/characters-1;memory.check(GuestAddress{output},16+2*l.p,Access::Write);std::uint64_t storage=0;
        if(text.size()>inline_capacity){storage=allocate_heap(memory,(text.size()+1)*characters);if(!storage)throw std::bad_alloc();}
        try{memory.fill(GuestAddress{output},0,16+2*l.p);const auto data=storage?storage:output;if(storage)put(output,storage);
            for(std::size_t i=0;i<text.size();++i)memory.store(GuestAddress{data+i*characters},static_cast<std::uint8_t>(characters*8),text[i]);
            memory.store(GuestAddress{data+text.size()*characters},static_cast<std::uint8_t>(characters*8),0);put(output+16,text.size());put(output+16+l.p,storage?text.size():inline_capacity);
        }catch(...){if(storage)(void)free_heap(memory,storage,state);throw;}return done(output);
    }
    if(numeric_put && method=="put"){
        const auto type=msvcp_number_type(name);using Type=msvcp_detail::NumberType;
        const auto slot=type==Type::Boolean?10U:type==Type::Unsigned32?8U:type==Type::Signed64?7U:type==Type::Unsigned64?6U:type==Type::Floating64?(name.ends_with("O@Z")?4U:5U):type==Type::Pointer?3U:9U;
        std::vector<std::uint64_t> arguments{object};for(unsigned i=0;i<(x86_?6U:5U);++i)arguments.push_back(a(i));
        GuestCallback callback{ptr(ptr(object)+slot*l.p),std::move(arguments)};callback.x86_register_arguments=x86_?1:0;
        callback.wide_arguments=x86_ && (type==Type::Signed64 || type==Type::Unsigned64 || type==Type::Floating64)?64U:0U;
        start_callbacks(state,memory,{std::move(callback)},Continuation::ReturnCallback);return {};
    }
    if(method.starts_with("NumPut") || (numeric_put && method=="do_put")){
        // do_put 匯出與虛擬表的 NumPut* 同一實作；型別取自修飾名的最後一個參數。
        using Type=msvcp_detail::NumberType;const auto type=method=="do_put"?msvcp_number_type(name):method=="NumPutBoolean"?Type::Boolean:method=="NumPutUnsigned32"?Type::Unsigned32:method=="NumPutSigned64"?Type::Signed64:method=="NumPutUnsigned64"?Type::Unsigned64:method=="NumPutDouble" || method=="NumPutLongDouble"?Type::Floating64:method=="NumPutPointer"?Type::Pointer:Type::Signed32;
        Operation op;op.service=service;op.object=object;op.modern=true;op.element=l.element;op.kind="num_put";op.output=a(0);op.ios=a(x86_?3:2);op.limit=a(x86_?4:3);op.count=a(x86_?5:4);
        op.buffer=x86_?a(2):ptr(a(1)+l.p);op.numeric_failed=(x86_?a(1):memory.load(GuestAddress{a(1)},8))&255U;
        op.numeric_value={type,type==Type::Boolean?op.count&255U:op.count,l.p};memory.check(GuestAddress{op.output},2*l.p,Access::Write);
        const auto locale=ptr(op.ios+l.locale),imp=locale?ptr(locale+l.locale_pointer()):global;const auto id=l.element==1?5U:8U;
        op.numeric_punct=ptr(imp+3*l.p)>id?ptr(ptr(imp+2*l.p)+id*l.p):0;
        if(!op.numeric_punct)op.numeric_punct=ptr(ptr(global+2*l.p)+id*l.p);
        const auto ctype_id=l.element==1?1U:6U;op.numeric_ctype=ptr(imp+3*l.p)>ctype_id?ptr(ptr(imp+2*l.p)+ctype_id*l.p):0;
        if(!op.numeric_ctype)op.numeric_ctype=ptr(ptr(global+2*l.p)+ctype_id*l.p);
        for(const auto facet:{object,op.numeric_punct,op.numeric_ctype})if(refs(facet)){if(refs(facet)==UINT32_MAX)throw std::bad_alloc();op.numeric_facets.push_back(facet);}
        op.numeric_format=msvcp_number_format(memory,l,op.ios,static_cast<char16_t>(op.limit));
        op.numeric_allocations.reserve(8);op.scratch=allocate_heap(memory,64);if(!op.scratch)throw std::bad_alloc();memory.fill(GuestAddress{op.scratch},0,64);
        const auto token=s.next++;try{s.operations.emplace(token,std::move(op));}catch(...){(void)free_heap(memory,op.scratch,state);throw;}return resume_msvcp100(state,memory,token,0);
    }
    if(l.modern && (name.find("num_get@")!=name.npos || method.starts_with("NumGet"))){
        // num_get 沒有資料成員：facet 只有 vtable 與參考數，locale 名稱不影響解析（小數點、千分位與 bool 名稱於解析時向 ios_base 的 numpunct 取得）。
        const auto table=l.element==2 && name.find("num_get@G")!=name.npos?tables.num_get16:tables.num_get;
        if(name.starts_with("??0") || name.starts_with("??_F")){
            const bool closure=name.starts_with("??_F");const auto references=closure?0U:static_cast<std::uint32_t>(a(name.find("_Locinfo@")!=name.npos?1U:0U));
            memory.fill(GuestAddress{object},0,2*l.p);put(object,table);memory.store(GuestAddress{object+l.p},32,references);return done(closure?0:object);
        }
        if(name.starts_with("??1")){put(object,m.facet_table14);return done(0);}
        if(method=="_Init")return done(0); // num_get::_Init 不從 _Locinfo 載入任何資料。
        if(method=="_Getcat"){
            if(a(0) && !ptr(a(0))){
                const auto facet=allocate_heap(memory,2*l.p);if(!facet){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
                memory.fill(GuestAddress{facet},0,2*l.p);put(facet,table);put(a(0),facet);
            }
            return done(4); // _X_NUMERIC
        }
        using Type=msvcp_detail::NumberType;
        if(method=="get"){
            // 公開 get 轉給虛擬 do_get（客體衍生 facet 的覆寫也生效）；槽位依參考目標型別。
            const auto slot=name.ends_with("PEAX@Z") || name.ends_with("PAX@Z")?3U:name.ends_with("O@Z")?4U:name.ends_with("_N@Z")?13U:name.ends_with("N@Z")?5U:name.ends_with("M@Z")?6U:
                name.ends_with("_K@Z")?7U:name.ends_with("_J@Z")?8U:name.ends_with("K@Z")?9U:name.ends_with("J@Z")?10U:name.ends_with("I@Z")?11U:name.ends_with("G@Z")?12U:0U;
            if(!slot)throw HostCallFault("未知 num_get::get 型別");
            std::vector<std::uint64_t> arguments{object};for(unsigned i=0;i<(x86_?8U:6U);++i)arguments.push_back(a(i));
            GuestCallback callback{ptr(ptr(object)+slot*l.p),std::move(arguments)};callback.x86_register_arguments=x86_?1:0;
            start_callbacks(state,memory,{std::move(callback)},Continuation::ReturnCallback);return {};
        }
        if(method!="do_get" && !method.starts_with("NumGet"))throw HostCallFault("未知 num_get 成員");
        const auto type=method=="do_get"?msvcp_number_type(name):method=="NumGetPointer"?Type::Pointer:method=="NumGetLongDouble" || method=="NumGetDouble"?Type::Floating64:
            method=="NumGetFloat"?Type::Floating32:method=="NumGetUnsigned64"?Type::Unsigned64:method=="NumGetSigned64"?Type::Signed64:
            method=="NumGetUnsignedLong" || method=="NumGetUnsigned32"?Type::Unsigned32:method=="NumGetUnsigned16"?Type::Unsigned16:method=="NumGetBoolean"?Type::Boolean:Type::Signed32;
        // x64：a(0) 回傳的迭代器、a(1)／a(2) first／last 副本的位址；x86 的迭代器以值傳遞，各占 a(1..2)、a(3..4)。之後為 ios_base、iostate 與目標。
        // 迭代器的 _Got／_Val 只是 sgetc 的快取，解析時直接向 streambuf 前瞻；last 不是結尾迭代器時依 MSVC 的比較規則與 first 立即相等。
        Operation op;op.service=service;op.object=object;op.modern=true;op.element=l.element;op.kind="num_get";op.started=true;
        const unsigned at=x86_?5U:3U;op.output=a(0);op.buffer=x86_?a(1):ptr(a(1));op.limit=x86_?a(3):ptr(a(2));
        op.ios=a(at);op.time_state=a(at+1);op.input_value=a(at+2);op.numeric_value.type=type;
        memory.check(GuestAddress{op.output},2*l.p,Access::Write);memory.check(GuestAddress{op.time_state},4,Access::Write);
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
    if(method=="ThrowBadAlloc"){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
    if(method=="generic_category" || method=="system_category" || method=="iostream_category")return done(s.error_categories[method=="generic_category"?0:method=="system_category"?1:2]);
    const bool winerror_string=method=="_Winerror_map" && name.find("@@YAHH@Z")==std::string_view::npos;
    if(method=="_Winerror_map" && !winerror_string)return done(windows_generic_error_condition(static_cast<std::uint32_t>(a(0))).value_or(0));
    // std::_Winerror_message：與 STL 相同以 FormatMessageA（FROM_SYSTEM｜IGNORE_INSERTS）取系統訊息，回傳字元數。
    if(method=="_Winerror_message"){start_callbacks(state,memory,{{resolve("kernel32.dll","FormatMessageA").value,{0x1200,0,a(0)&UINT32_MAX,0,a(1),a(2)&UINT32_MAX,0}}},Continuation::ReturnCallback);return {};}
    if(method=="_Syserror_map" || winerror_string){
        const auto code=winerror_string?windows_generic_error_condition(static_cast<std::uint32_t>(a(0))).value_or(0):static_cast<std::uint32_t>(a(0));
        const auto text=windows_generic_error_message(code);
        if(service.family!="msvcp140.dll" && text=="unknown error")return done(0);
        if(const auto found=s.generic_error_messages.find(text);found!=s.generic_error_messages.end())return done(found->second);
        std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);const auto address=runtime_allocate(memory,bytes);
        s.generic_error_messages.emplace(text,address);return done(address);
    }
    if(method=="_Throw_Cpp_error"){
        // std::_Throw_Cpp_error：以 generic_category 拋出 system_error，訊息為「原因: 錯誤說明」。
        static constexpr std::array<std::pair<std::uint32_t,const char*>,7> codes{{{16,"device or resource busy"},{22,"invalid argument"},
            {3,"no such process"},{12,"not enough memory"},{1,"operation not permitted"},{36,"resource deadlock would occur"},{11,"resource unavailable try again"}}};
        const auto index=static_cast<std::uint32_t>(a(0));if(index>=codes.size())return 3;
        const auto error=codes[index].first;auto text=std::string(codes[index].second)+": "+std::string(windows_generic_error_message(error));
        const auto scratch=allocate_heap(memory,text.size()+1);
        if(scratch){memory.write(GuestAddress{scratch},{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});memory.store(GuestAddress{scratch+text.size()},8,0);}
        try{throw_msvc_exception(state,memory,service,MsvcExceptionKind::SystemError,scratch,error,s.error_categories[0]);}catch(...){if(scratch)(void)free_heap(memory,scratch,state);throw;}
        if(scratch)(void)free_heap(memory,scratch,state);return {};
    }
    if(method=="_Throw_C_error" || method=="_XGetLastError"){
        const bool system=method=="_XGetLastError";const auto result=system?last_error_:static_cast<std::uint32_t>(a(0));
        if(!system && (!result || result>4))return 3;const auto error=system?result:result==1 || result==2?11U:result==3?16U:22U;
        auto text=system?utf16_to_utf8(windows_error_message(error)):std::string(windows_crt_error_message(error));
        if(text.empty())text="unknown error";const auto scratch=allocate_heap(memory,text.size()+1);
        if(scratch){memory.write(GuestAddress{scratch},{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});memory.store(GuestAddress{scratch+text.size()},8,0);}
        try{throw_msvc_exception(state,memory,service,MsvcExceptionKind::SystemError,scratch,error,s.error_categories[system?1:0]);}catch(...){if(scratch)(void)free_heap(memory,scratch,state);throw;}
        if(scratch)(void)free_heap(memory,scratch,state);return {};
    }
    if(method.starts_with("ErrorCategory")){
        const auto found=std::find(s.error_categories.begin(),s.error_categories.end(),object);if(found==s.error_categories.end())throw HostCallFault("STL 錯誤分類物件無效");const auto index=static_cast<unsigned>(found-s.error_categories.begin());
        if(method=="ErrorCategoryDelete")return done(object);if(method=="ErrorCategoryName")return done(s.error_category_names[index]);
        const auto condition=[&](std::uint32_t error){
            if(index!=1)return std::pair{error,object};const auto mapped=windows_generic_error_condition(error);
            return std::pair{mapped.value_or(error),mapped?s.error_categories[0]:object};
        };
        if(method=="ErrorCategoryCondition"){const auto output=a(0);const auto value=condition(static_cast<std::uint32_t>(a(1)));memory.write(GuestAddress{output},std::vector<std::uint8_t>(2*l.p));memory.store(GuestAddress{output},32,value.first);put(output+l.p,value.second);return done(output);}
        if(method=="ErrorCategoryEquivalentA"){const auto value=condition(static_cast<std::uint32_t>(a(0)));return done(static_cast<std::uint32_t>(memory.load(GuestAddress{a(1)},32))==value.first && ptr(a(1)+l.p)==value.second);}
        if(method=="ErrorCategoryEquivalentB")return done(static_cast<std::uint32_t>(memory.load(GuestAddress{a(0)},32))==static_cast<std::uint32_t>(a(1)) && ptr(a(0)+l.p)==object);
        if(method=="ErrorCategoryMessage"){
            const auto output=a(0);const auto code=static_cast<std::uint32_t>(a(1));auto text=index==1?utf16_to_utf8(windows_error_message(code)):std::string(index==2 && code==1?"iostream stream error":windows_generic_error_message(code));
            if(text.empty())text="unknown error";
            memory.check(GuestAddress{output},16+2*l.p,Access::Write);std::uint64_t storage=0;if(text.size()>15){storage=allocate_heap(memory,text.size()+1);if(!storage){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}}
            memory.write(GuestAddress{output},std::vector<std::uint8_t>(16+2*l.p));const auto data=storage?storage:output;if(storage)put(output,storage);
            memory.write(GuestAddress{data},{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});memory.store(GuestAddress{data+text.size()},8,0);put(output+16,text.size());put(output+16+l.p,storage?text.size():15);return done(output);
        }throw HostCallFault("未知 STL 錯誤分類操作");
    }
    // 只處理 _Locinfo 類別本身的成員；以 _Locinfo 為參數的 facet 建構子（codecvt、num_put…）由各 facet 處理。
    if(l.modern && (name.find("@_Locinfo@std@@")!=name.npos || name.starts_with("??0_Locinfo@") || name.starts_with("??1_Locinfo@") || name.starts_with("??_F_Locinfo@"))){
        // _Locinfo::_Getlconv 在 msvcp140 內即 localeconv()，回傳目前 CRT locale 的 lconv。
        if(method=="_Getlconv"){start_callbacks(state,memory,{{resolve("crt","localeconv").value,{}}},Continuation::ReturnCallback);return {};}
        if(method=="_Getcvt"){write_cvtvec(a(0));return done(a(0));}
        // _Gettnames 回傳擁有 CRT _Gettnames 配置的 _Timevec（隱藏指標）。
        if(method=="_Gettnames" || method=="_W_Gettnames"){const auto output=a(0);memory.check(GuestAddress{output},l.p,Access::Write);put(output,nested_service(state,memory,"crt",method=="_Gettnames"?"_Gettnames":"_W_Gettnames",{}));return done(output);}
        if(method=="_Getctype"){write_ctypevec(a(0));return done(a(0));}
        if((method=="_Locinfo_ctor" && name.find("@HP")==name.npos) || method=="_Locinfo_dtor"){ // 依類別的 _Locinfo_ctor 見下方 categories。
            // 靜態入口由客體內聯的 _Locinfo 呼叫：鎖與各 _Yarn 的建構解構都在客體，這裡只切換 CRT locale 並填入新舊名稱。
            const bool destroy=method=="_Locinfo_dtor";memory.check(GuestAddress{a(0)},x86_?52:104,Access::Write);
            Operation op;op.service=service;op.modern=true;op.element=1;op.kind=destroy?"locinfo_destroy":"locinfo_construct";op.object=a(0);op.buffer=destroy?0:a(1);op.skip=true;
            const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
        }
        if(method=="_Getfalse" || method=="_Gettrue" || method=="_Getdays" || method=="_Getmonths" || method=="_W_Getdays" || method=="_W_Getmonths"){
            // MSVC 在名稱不可用時回傳的 C locale 預設字串；同一程序共用一份常駐資料。
            const bool wide=method.starts_with("_W_");const auto key="#"+std::string(method);
            if(const auto found=m.data14.find(key);found!=m.data14.end())return done(found->second);
            const std::string text=method=="_Getfalse"?"false":method=="_Gettrue"?"true":method.ends_with("days")?
                ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:Thu:Thursday:Fri:Friday:Sat:Saturday":
                ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:Jul:July:Aug:August:Sep:September:Oct:October:Nov:November:Dec:December";
            std::vector<std::uint8_t> bytes;for(const auto c:text){bytes.push_back(static_cast<std::uint8_t>(c));if(wide)bytes.push_back(0);}bytes.push_back(0);if(wide)bytes.push_back(0);
            const auto address=runtime_allocate(memory,bytes);m.data14.emplace(key,address);return done(address);
        }
        if(method=="_Getcoll"){
            const auto output=a(0);memory.check(GuestAddress{output},2*l.p,Access::Write);const auto& category=current_crt_locale(state,memory).categories[1];std::uint64_t locale_name=0;
            if(category.codepage){const auto bytes=utf16_bytes(category.name);locale_name=allocate_heap(memory,bytes.size());if(!locale_name){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
                try{memory.write(GuestAddress{locale_name},bytes);}catch(...){(void)free_heap(memory,locale_name,state);throw;}}
            memory.write(GuestAddress{output},std::vector<std::uint8_t>(2*l.p));memory.store(GuestAddress{output},32,category.codepage);put(output+l.p,locale_name);return done(output);
        }
        // _Newlocname 的 C 字串：_Yarn 沒有字串時回傳其內嵌的結尾字元（空字串）。
        if(method=="_Getname"){const auto at=object+(x86_?4U:8U)+5*2*l.p;const auto text=ptr(at);return done(text?text:at+l.p);}
        // _Getdateorder：time_base::dateorder；目前的 CRT locale 皆採月／日／年（mdy）。
        if(method=="_Getdateorder")return done(2);
        // 依類別建立或增補 locale 名稱（_Locinfo(int, const char*)、_Locinfo_ctor(_Locinfo*, int, const char*)、_Addcats、_Locinfo_Addcats）：
        // 建構時先記錄舊的完整 locale 名稱（寬字元）；再依 MSVC：名稱為 "*" 不變更、類別 0 只查詢、全部類別一次設定、其餘逐一設定 LC_COLLATE～LC_TIME，
        // 最後以 setlocale(LC_ALL, nullptr) 的結果更新 _Newlocname（失敗為 "*"，原本已是 "*" 時維持）。
        const bool categories=name.find("@HP")!=name.npos && (name.starts_with("??0_Locinfo@") || method=="_Locinfo_ctor" || method=="_Addcats" || method=="_Locinfo_Addcats");
        if(categories){
            const bool static_form=method=="_Locinfo_ctor" || method=="_Locinfo_Addcats";const bool member_ctor=name.starts_with("??0"),construct=member_ctor || method=="_Locinfo_ctor";
            const auto info=static_form?a(0):object;const auto category=static_cast<std::uint32_t>(a(static_form?1:0));const auto locale_name=a(static_form?2:1);
            if(!locale_name){throw_msvc_exception(state,memory,service,MsvcExceptionKind::RuntimeError);return {};}
            // nested_continuation 完成後會重新進入本服務取回結果：清零等副作用都放在續行內，避免重入時覆寫已寫好的名稱。
            memory.check(GuestAddress{info},x86_?52:104,Access::Write);
            const auto lockit=member_ctor?resolve("msvcp140.dll",x86_?"??0_Lockit@std@@QAE@H@Z":"??0_Lockit@std@@QEAA@H@Z").value:0;
            const auto setlocale_entry=resolve("crt","setlocale").value,wsetlocale_entry=resolve("crt","_wsetlocale").value;const auto layout=l;
            const std::uint64_t result=method=="_Locinfo_ctor"?0:info;
            return nested_continuation(state,memory,"locinfo categories",[this,service,info,category,locale_name,construct,lockit,setlocale_entry,wsetlocale_entry,layout,bits,result](GuestState& s2,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const auto load=[&](std::uint64_t at){return m.load(GuestAddress{at},bits);};const auto store=[&](std::uint64_t at,std::uint64_t value){m.store(GuestAddress{at},bits,value);};
                const auto yarns=info+(x86_?4U:8U),old_name=yarns+4*2*layout.p,new_name=old_name+2*layout.p;
                const auto replace=[&](std::uint64_t at,std::uint64_t text){if(const auto previous=load(at);previous && allocations_.contains(previous))(void)free_heap(m,previous,s2);store(at,text);};
                if(lockit)m.write(GuestAddress{info},std::vector<std::uint8_t>(x86_?52:104));
                if(lockit){GuestCallback lock{lockit,{info,0}};lock.x86_register_arguments=x86_?1:0;(void)co_await service_callback::call_native(std::move(lock));}
                if(construct){const auto current=co_await service_callback::call_value(wsetlocale_entry,{0,0});replace(old_name,current?crt_duplicate_string(m,current,true):0);}
                const bool star=m.load(GuestAddress{locale_name},8)=='*' && !m.load(GuestAddress{locale_name+1},8);
                std::uint64_t updated=0;
                if(star)updated=0;
                else if(!category)updated=co_await service_callback::call_value(setlocale_entry,{0,0});
                else if((category&63U)==63U)updated=co_await service_callback::call_value(setlocale_entry,{0,locale_name});
                else{for(unsigned index=1;index<=5;++index)if(category&(1U<<(index-1)))(void)co_await service_callback::call_value(setlocale_entry,{index,locale_name});
                    updated=co_await service_callback::call_value(setlocale_entry,{0,0});}
                const auto current=load(new_name);const bool already_star=current && m.load(GuestAddress{current},8)=='*' && !m.load(GuestAddress{current+1},8);
                if(!updated){const auto marker=allocate_heap(m,2);if(marker){m.store(GuestAddress{marker},16,'*');replace(new_name,marker);}}
                else if(!already_star)replace(new_name,crt_duplicate_string(m,updated));
                const WindowsCall done_call(s2,m,service);done_call.return_to(s2,result);co_return std::nullopt;
            });
        }
        // 預設建構閉包（??_F）：等同 _Locinfo("C")。
        if(name.starts_with("??_F_Locinfo@")){
            auto found=m.data14.find("#locinfo-C");if(found==m.data14.end()){const std::array<std::uint8_t,2> text{'C',0};found=m.data14.emplace("#locinfo-C",runtime_allocate(memory,text)).first;}
            Operation op;op.service=service;op.modern=true;op.element=1;op.kind="locinfo_construct";op.object=object;op.buffer=found->second;
            memory.write(GuestAddress{object},std::vector<std::uint8_t>(x86_?52:104));
            const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
        }
        if(name.starts_with("??0") || name.starts_with("??1")){
            const bool destroy=name.starts_with("??1");if(!destroy && !a(0)){throw_msvc_exception(state,memory,service,MsvcExceptionKind::RuntimeError);return {};}
            Operation op;op.service=service;op.modern=true;op.element=1;op.kind=destroy?"locinfo_destroy":"locinfo_construct";op.object=object;op.buffer=destroy?0:a(0);
            if(!destroy)memory.write(GuestAddress{object},std::vector<std::uint8_t>(x86_?52:104));
            const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
        }
        throw HostCallFault("未知 STL _Locinfo 操作");
    }
    if(method=="_Throw_future_error"){
        // std::_Throw_future_error(const error_code&)：error_code｛int 值, const error_category*｝，訊息依 future_errc。
        const auto code=a(0);throw_msvc_exception(state,memory,service,MsvcExceptionKind::FutureError,0,static_cast<std::uint32_t>(memory.load(GuestAddress{code},32)),ptr(code+l.p));return {};
    }
    if(l.modern && (method.starts_with("__ExceptionPtr") || method.starts_with("ExceptionControl") || method=="_Rethrow_future_exception")){
        const auto control_entry=[&](std::uint64_t control)->Msvcp100State::CapturedException&{const auto found=s.exception_pointers.find(control);if(found==s.exception_pointers.end())throw HostCallFault("STL exception_ptr 控制區不存在");return found->second;};
        const auto retain=[&](std::uint64_t control){if(!control)return;auto& entry=control_entry(control);if(entry.immortal)return;const auto at=control+l.p,n=memory.load(GuestAddress{at},32);if(!n || n>=UINT32_MAX)throw HostCallFault("STL exception_ptr 引用計數無效");memory.store(GuestAddress{at},32,n+1);};
        const auto publish=[&](std::uint64_t target,std::uint64_t control){memory.check(GuestAddress{target},2*l.p,Access::Write);put(target,control?control_entry(control).record:0);put(target+l.p,control);};
        const auto release=[&](std::uint64_t control)->std::optional<std::uint32_t>{
            if(!control)return done(0);Operation op;op.service=service;op.modern=true;op.element=1;op.kind="exception_release";op.object=control;const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
        };
        if(method=="__ExceptionPtrCreate"){memory.check(GuestAddress{a(0)},2*l.p,Access::Write);put(a(0),0);put(a(0)+l.p,0);return done(0);}
        if(method=="__ExceptionPtrToBool")return done(ptr(a(0))!=0);
        if(method=="__ExceptionPtrCompare")return done(ptr(a(0))==ptr(a(1)));
        if(method=="__ExceptionPtrSwap"){
            const auto left=a(0),right=a(1);memory.check(GuestAddress{left},2*l.p,Access::Write);memory.check(GuestAddress{right},2*l.p,Access::Write);
            const auto record=ptr(left),control=ptr(left+l.p);put(left,ptr(right));put(left+l.p,ptr(right+l.p));put(right,record);put(right+l.p,control);return done(0);
        }
        if(method=="__ExceptionPtrCopy" || method=="__ExceptionPtrAssign"){
            const auto target=a(0),source=a(1);if(target==source)return done(0);const auto record=ptr(source),control=ptr(source+l.p);
            if((control && record!=control_entry(control).record) || (!control && record))throw HostCallFault("STL exception_ptr 物件不一致");
            memory.check(GuestAddress{target},2*l.p,Access::Write);const auto old=method=="__ExceptionPtrAssign"?ptr(target+l.p):0;retain(control);publish(target,control);return release(old);
        }
        if(method=="__ExceptionPtrDestroy"){const auto target=a(0),old=ptr(target+l.p);memory.check(GuestAddress{target},2*l.p,Access::Write);put(target,0);put(target+l.p,0);return release(old);}
        if(method=="ExceptionControlRelease")return release(object);
        if(method=="ExceptionControlDeleter")return done(0); // 沒有自訂 deleter 的 shared_ptr 控制區。
        if(method=="ExceptionControlDelete" || method=="ExceptionControlDestructor"){
            const auto immortal=control_entry(object).immortal;if(!immortal && (method=="ExceptionControlDelete" || (a(0)&1U))){s.exception_pointers.erase(object);(void)free_heap(memory,object,state);}return done(method=="ExceptionControlDestructor"?object:0);
        }
        if(method=="ExceptionControlDestroy"){
            auto& entry=control_entry(object);if(entry.immortal || !entry.constructed)return done(0);entry.constructed=false;
            Operation op;op.service=service;op.modern=true;op.element=1;op.kind="exception_destroy";op.object=object;
            const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
        }
        msvc_detail::ExceptionRecord record;std::uint64_t target=0,old=0;
        // _Rethrow_future_exception 的 exception_ptr 以值傳遞（x64 為暫存複本指標），語義即 rethrow_exception。
        const bool rethrow=method=="__ExceptionPtrRethrow" || method=="_Rethrow_future_exception";
        if(rethrow){
            const auto address=a(0),control=ptr(address+l.p);if(!ptr(address)){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadException);return {};}
            if(!control || ptr(address)!=control_entry(control).record)throw HostCallFault("STL exception_ptr 物件不一致");record=msvc_detail::exception_record(memory,ptr(address),x86_);
        }else if(method=="__ExceptionPtrCopyException"){
            target=a(0);memory.check(GuestAddress{target},2*l.p,Access::Write);old=ptr(target+l.p);const auto image=module_image(a(2),true);
            if(!image || !a(1))throw HostCallFault("STL exception_ptr 的 ThrowInfo 沒有所屬客體映像");
            record.code=0xe06d7363U;record.flags=1;record.parameters={0x19930520U,a(1),a(2)};if(!x86_)record.parameters.push_back(image->base);
        }else if(method=="__ExceptionPtrCurrentException"){
            target=a(0);memory.check(GuestAddress{target},2*l.p,Access::Write);old=ptr(target+l.p);const auto cells=msvc_exception_cells(state,memory,true),current=ptr(cells);
            if(!current)return done(0);record=msvc_detail::exception_record(memory,current,x86_);if(record.code==0xe0434f4dU || record.code==0xe0434352U)return done(0);
        }else throw HostCallFault("未知 STL exception_ptr 操作");
        if(rethrow && !record.cpp()){
            auto caller=state;call.return_to(caller,0);begin_exception(caller,memory,record.code,record.flags,record.parameters);state=caller;return {};
        }
        const auto type=record.cpp()?msvc_detail::exception_type(memory,record,x86_):msvc_detail::ExceptionType{};
        const auto header=x86_?96U:176U;const auto allocation=allocate_heap(memory,(rethrow?0:header)+type.size);
        if(!allocation){if(rethrow){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
            publish(target,s.bad_alloc_pointer);return release(old);}
        std::uint64_t operation_token=0;
        try{
            const auto object_copy=type.size?allocation+(rethrow?0:header):0;
            if(type.size){memory.write(GuestAddress{object_copy},std::vector<std::uint8_t>(static_cast<std::size_t>(type.size)));if(!type.copy)memory.move(GuestAddress{object_copy},GuestAddress{type.source},static_cast<std::size_t>(type.size));record.parameters[1]=object_copy;}
            if(!rethrow){const auto saved=allocation+l.p+8;put(allocation,s.exception_control_table);memory.store(GuestAddress{allocation+l.p},32,1);memory.store(GuestAddress{allocation+l.p+4},32,1);
                msvc_detail::write_exception_record(memory,saved,x86_,record);s.exception_pointers.emplace(allocation,Msvcp100State::CapturedException{saved,object_copy,type.destroy,type.interface_handle(),false,false});}
            Operation op;op.service=service;op.modern=true;op.element=1;op.kind=rethrow?"exception_rethrow":"exception_capture";op.object=object_copy;op.buffer=type.copy;op.output=target;op.limit=old;op.scratch=allocation;op.final_result=allocation;op.count=type.source;op.skip=type.virtual_base();op.started=type.interface_handle();op.exception_record=std::move(record);
            operation_token=s.next++;s.operations.emplace(operation_token,std::move(op));return resume_msvcp100(state,memory,operation_token,0);
        }catch(...){if(operation_token)s.operations.erase(operation_token);s.exception_pointers.erase(allocation);(void)free_heap(memory,allocation,state);throw;}
    }
    if(method=="_Getgloballocale")return done(global);
    if(l.modern && method=="classic")return done(m.classic_locale14);
    // ios_base::Init 的建構／解構（含靜態 _Init_ctor／_Init_dtor）；operator= 與 _Init_cnt_func 另行處理。
    if(l.modern && name.find("Init@ios_base@std@@")!=name.npos && !name.starts_with("??4") && !name.starts_with("?_Init_cnt_func@")){
        // 計數由 -1 起算；最後一個 Init 解構時依 cerr、clog、cout 的順序 flush，串流本身維持常駐。
        const auto count=static_cast<std::int32_t>(memory.load(GuestAddress{m.init_count14},32));
        if(name.starts_with("??0") || method=="_Init_ctor"){memory.store(GuestAddress{m.init_count14},32,static_cast<std::uint32_t>(count>=0?count+1:1));return done(object);}
        memory.store(GuestAddress{m.init_count14},32,static_cast<std::uint32_t>(count-1));if(count!=1)return done(0);
        Operation op;op.service=service;op.modern=true;op.element=1;op.kind="ios_init_flush";
        for(const auto* stream:{"?cerr@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A","?clog@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A","?cout@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A"})op.objects.push_back(m.data14.at(stream));
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
    if(l.modern && name.find("_Init_locks@std@@")!=name.npos){
        // std::_Init_locks：_Init_cnt 由 -1 起算，首個建構初始化 STL 鎖、計數回到 -1 時銷毀。這裡的 _Lockit 鎖由 InitOnce
        // 延遲初始化並常駐（其他模組仍可能持有），所以只維持相同的配對計數；成員建構回傳 this，靜態版本無回傳值。
        const bool constructor=name.starts_with("??0") || method=="_Init_locks_ctor";
        m.init_locks14+=constructor?1:-1;
        return done(name.starts_with("??0")?object:0);
    }
    if(l.modern && method=="xalloc"){const auto index=memory.load(GuestAddress{m.ios_index14},32);memory.store(GuestAddress{m.ios_index14},32,index+1);return done(index);}
    if(l.modern && method=="_Init" && name.find("@locale@")!=name.npos){if(static_cast<std::uint8_t>(a(0)))put_refs(global,refs(global)+1);return done(global);}
    // locale::_Locimp 的建立（_New_Locimp(bool)、_Locimp(bool)）：facet(1)、沒有 facet、類別遮罩 none、名稱 "*"。
    const auto init_locimp=[&](std::uint64_t imp,bool transparent)->bool{
        const auto star=allocate_heap(memory,2);if(!star)return false;memory.store(GuestAddress{star},16,'*');
        memory.write(GuestAddress{imp},std::vector<std::uint8_t>(x86_?32:56));put(imp,m.locimp_table14);put_refs(imp,1);
        memory.store(GuestAddress{imp+4*l.p+4},8,transparent?1:0);put(imp+4*l.p+8,star);s.owned_locale_imps.insert(imp);return true;
    };
    if(l.modern && (method=="_New_Locimp" || name.starts_with("??0_Locimp@locale@std@@")) && name.find("@_N@Z")!=name.npos){
        const bool allocate=method=="_New_Locimp";const auto imp=allocate?allocate_heap(memory,x86_?32:56):object;
        if(!allocate)memory.check(GuestAddress{imp},x86_?32:56,Access::Write);
        if(!imp || !init_locimp(imp,static_cast<std::uint8_t>(a(0))!=0)){if(allocate && imp)(void)free_heap(memory,imp,state);throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
        return done(imp);
    }
    // 複製建構（_Locimp(const _Locimp&)、static _Locimp_ctor）：與 _New_Locimp(const _Locimp&) 共用複製流程，寫入呼叫端提供的物件。
    // 解構（~_Locimp、static _Locimp_dtor）：釋放各 facet 的參考與名稱，不釋放物件本身。
    if(l.modern && (name.starts_with("??0_Locimp@locale@std@@") || method=="_Locimp_ctor")){
        const bool static_form=method=="_Locimp_ctor";const auto target=static_form?a(0):object,source=static_form?a(1):a(0);
        memory.check(GuestAddress{target},x86_?32:56,Access::Write);memory.check(GuestAddress{source},x86_?32:56,Access::Read);
        Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="clone_locale_imp";op.locale_source=source;op.scratch=target;
        op.locale_lock_storage=allocate_heap(memory,16);if(!op.locale_lock_storage){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
        memory.fill(GuestAddress{op.locale_lock_storage},0,16);const auto token=s.next++;
        try{s.operations.emplace(token,std::move(op));}catch(...){(void)free_heap(memory,op.locale_lock_storage,state);throw;}
        return resume_msvcp100(state,memory,token,0);
    }
    if(l.modern && (name.starts_with("??1_Locimp@locale@std@@") || method=="_Locimp_dtor")){
        const auto imp=method=="_Locimp_dtor"?a(0):object;memory.check(GuestAddress{imp},x86_?32:56,Access::Write);
        Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="destroy_locale_imp";op.object=imp;op.output=ptr(imp+2*l.p);op.limit=ptr(imp+3*l.p);op.final_result=0;
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
    // _Makeloc／_Makexloc／_Makewloc／_Makeushloc：依 _Locinfo 的名稱為 _Locimp 加入所選類別的 facet。指定了來源 locale 時沿用它的 facet；
    // 否則以 facet 的 _Getcat 依名稱建立。加入的種類是目前已實作的標準 facet（ctype／codecvt 屬 ctype 類別，numpunct／num_get／num_put
    // 屬 numeric，time_get／time_put 屬 time）；_Makeloc 另合併類別遮罩並把 _Locimp 的名稱改為 _Locinfo 的名稱。
    if(l.modern && (method=="_Makeloc" || method=="_Makexloc" || method=="_Makewloc" || method=="_Makeushloc")){
        const auto info=a(0);const auto category=static_cast<std::uint32_t>(a(1));const auto imp=a(2),from=a(3);
        memory.check(GuestAddress{imp},x86_?32:56,Access::Write);memory.check(GuestAddress{info},x86_?52:104,Access::Read);
        struct FacetKind {unsigned element,category;const char* type;};
        static constexpr FacetKind kinds[]{
            {0,2,"ctype@D"},{0,2,"codecvt@DDU_Mbstatet@@"},{0,8,"numpunct@D"},{0,8,"num_get@DV?$istreambuf_iterator@DU?$char_traits@D@std@@@std@@"},
            {0,8,"num_put@DV?$ostreambuf_iterator@DU?$char_traits@D@std@@@std@@"},{0,16,"time_get@DV?$istreambuf_iterator@DU?$char_traits@D@std@@@std@@"},
            {0,16,"time_put@DV?$ostreambuf_iterator@DU?$char_traits@D@std@@@std@@"},
            {1,2,"ctype@_W"},{1,2,"codecvt@_WDU_Mbstatet@@"},{1,8,"numpunct@_W"},{1,8,"num_get@_WV?$istreambuf_iterator@_WU?$char_traits@_W@std@@@std@@"},
            {1,8,"num_put@_WV?$ostreambuf_iterator@_WU?$char_traits@_W@std@@@std@@"},{1,16,"time_get@_WV?$istreambuf_iterator@_WU?$char_traits@_W@std@@@std@@"},
            {1,16,"time_put@_WV?$ostreambuf_iterator@_WU?$char_traits@_W@std@@@std@@"},
            {2,2,"ctype@G"},{2,2,"codecvt@GDU_Mbstatet@@"},{2,8,"num_get@GV?$istreambuf_iterator@GU?$char_traits@G@std@@@std@@"},
            {2,8,"num_put@GV?$ostreambuf_iterator@GU?$char_traits@G@std@@@std@@"},{2,16,"time_put@GV?$ostreambuf_iterator@GU?$char_traits@G@std@@@std@@"}};
        const int only=method=="_Makexloc"?0:method=="_Makewloc"?1:method=="_Makeushloc"?2:-1;
        struct Planned {std::uint64_t getcat,id;};std::vector<Planned> planned;
        for(const auto& kind:kinds){
            if((only>=0 && static_cast<int>(kind.element)!=only) || !(kind.category&category))continue;
            const auto getcat=resolve("msvcp140.dll",std::string("?_Getcat@?$")+kind.type+(x86_?"@std@@SAIPAPBVfacet@locale@2@PBV42@@Z":"@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z")).value;
            const auto id_cell=m.data14.find(std::string("?id@?$")+kind.type+"@std@@2V0locale@2@A");
            if(!getcat || id_cell==m.data14.end())throw HostCallFault("STL _Makeloc 缺少 facet 定義");
            auto id=ptr(id_cell->second);if(!id){id=memory.load(GuestAddress{m.id_count14},32)+1;memory.store(GuestAddress{m.id_count14},32,id);put(id_cell->second,id);}
            planned.push_back({getcat,id});
        }
        // 暫時 locale：_Getcat 只讀 _Ptr->_Name，指向 _Locinfo 的新名稱（不轉移擁有權）。
        const auto info_name=ptr(info+(x86_?4U:8U)+5*2*l.p);
        const auto addfac=resolve("msvcp140.dll",x86_?"?_Locimp_Addfac@_Locimp@locale@std@@CAXPAV123@PAVfacet@23@I@Z":"?_Locimp_Addfac@_Locimp@locale@std@@CAXPEAV123@PEAVfacet@23@_K@Z").value;
        const auto layout=l;const auto global_imp=m.global14;const bool makeloc=method=="_Makeloc";
        // 暫存（假 _Locimp、locale 與 facet 輸出格）在續行內配置：續行完成後會重新進入本服務取回結果。
        return nested_continuation(state,memory,"locale makeloc",[this,service,planned,imp,from,info_name,category,addfac,layout,bits,global_imp,makeloc](GuestState& s2,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const auto load=[&](std::uint64_t at){return m.load(GuestAddress{at},bits);};const auto store=[&](std::uint64_t at,std::uint64_t value){m.store(GuestAddress{at},bits,value);};
            const auto imp_size=x86_?32U:56U;const auto scratch=allocate_heap(m,imp_size+layout.locale_size()+layout.p);
            if(!scratch)throw std::bad_alloc{};
            const auto temp_imp=scratch,temp_locale=scratch+imp_size,cell=temp_locale+layout.locale_size();
            m.write(GuestAddress{scratch},std::vector<std::uint8_t>(imp_size+layout.locale_size()+layout.p));store(temp_imp+4*layout.p+8,info_name);store(temp_locale+layout.locale_pointer(),temp_imp);
            // 透明的 _Locimp 找不到 facet 時改查全域 locale（與 _Locimp::_Getfacet 相同）。
            const auto facet_in=[&](std::uint64_t locimp,std::uint64_t id)->std::uint64_t{
                for(unsigned step=0;step<2 && locimp;++step){
                    const auto count=load(locimp+3*layout.p),array=load(locimp+2*layout.p);
                    if(id<count && array)if(const auto facet=load(array+id*layout.p))return facet;
                    if(!m.load(GuestAddress{locimp+4*layout.p+4},8) || locimp==global_imp)break;
                    locimp=global_imp;
                }
                return 0;
            };
            for(const auto& item:planned){
                std::uint64_t facet=from?facet_in(load(from+layout.locale_pointer()),item.id):0;
                if(!facet){store(cell,0);(void)co_await service_callback::call_value(item.getcat,{cell,from?from:temp_locale});facet=load(cell);}
                if(facet)(void)co_await service_callback::call_value(addfac,{imp,facet,item.id});
            }
            if(makeloc){
                m.store(GuestAddress{imp+4*layout.p},32,static_cast<std::uint32_t>(m.load(GuestAddress{imp+4*layout.p},32))|category);
                const auto label=info_name?crt_duplicate_string(m,info_name):0;
                if(label){if(const auto previous=load(imp+4*layout.p+8);previous && allocations_.contains(previous))(void)free_heap(m,previous,s2);store(imp+4*layout.p+8,label);}
            }
            (void)free_heap(m,scratch,s2);
            const WindowsCall done_call(s2,m,service);done_call.return_to(s2,makeloc?imp:0);co_return std::nullopt;
        });
    }
    if(l.modern && method=="_New_Locimp"){
        const auto source=a(0);memory.check(GuestAddress{source},x86_?32:56,Access::Read);
        Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="clone_locale_imp";op.locale_source=source;
        op.locale_lock_storage=allocate_heap(memory,16);if(!op.locale_lock_storage){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
        memory.fill(GuestAddress{op.locale_lock_storage},0,16);const auto token=s.next++;
        try{s.operations.emplace(token,std::move(op));}catch(...){(void)free_heap(memory,op.locale_lock_storage,state);throw;}
        return resume_msvcp100(state,memory,token,0);
    }
    if(l.modern && (method=="_Locimp_Addfac" || method=="_Addfac")){
        // _Addfac 是 _Locimp 的成員版本（this 為 _Locimp），語義與 static _Locimp_Addfac 相同。
        const bool member=method=="_Addfac";const auto imp=member?object:a(0),facet=a(member?0:1),id=a(member?1:2);if(id>=65536)throw HostCallFault("STL facet id 超過上限");memory.check(GuestAddress{imp},x86_?32:56,Access::Write);
        Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="add_locale_facet";op.object=imp;op.output=id;op.buffer=facet;
        op.locale_lock_storage=allocate_heap(memory,16);if(!op.locale_lock_storage){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
        memory.fill(GuestAddress{op.locale_lock_storage},0,16);const auto token=s.next++;
        try{s.operations.emplace(token,std::move(op));}catch(...){(void)free_heap(memory,op.locale_lock_storage,state);throw;}
        return resume_msvcp100(state,memory,token,0);
    }
    if(method=="setbase" || method=="setiosflags" || method=="resetiosflags"){
        // _Smanip<int>：｛套用函式, 引數｝以隱藏指標回傳；套用函式是 internal-msvcp140 的 cdecl 入口（ios_base&, int）。
        const auto output=a(0);memory.check(GuestAddress{output},2*l.p,Access::Write);memory.write(GuestAddress{output},std::vector<std::uint8_t>(2*l.p));
        put(output,resolve("internal-msvcp140",std::string("D")+(method=="setbase"?"SetBase":method=="setiosflags"?"SetFlags":"ResetFlags")+(x86_?"32":"64")).value);
        memory.store(GuestAddress{output+l.p},32,a(1));return done(output);
    }
    if(method=="SetBase" || method=="SetFlags" || method=="ResetFlags"){
        // setbase：8／10／16 設定對應的 basefield，其他值清除 basefield；setiosflags 設定旗標，resetiosflags 清除旗標。
        const auto at=GuestAddress{a(0)+l.flags};auto flags=static_cast<std::uint32_t>(memory.load(at,32));const auto value=static_cast<std::uint32_t>(a(1));
        if(method=="SetBase")flags=(flags&~0xe00U)|(value==8?0x400U:value==10?0x200U:value==16?0x800U:0U);
        else if(method=="SetFlags")flags|=value;else flags&=~value;
        memory.store(at,32,flags);return done(0);
    }
    if(name=="?set_new_handler@std@@YAP6AXXZP6AXXZ@Z"){
        // 與 MSVC 相同經 _set_new_handler 安裝轉接函式：operator new 配置失敗時轉接函式呼叫 void() 處理常式並回 1（重試）。
        const auto previous=s.std_new_handler;s.std_new_handler=a(0);
        runtime_.new_handler=a(0)?resolve("internal-msvcp140",x86_?"DNewHandlerAdapter32":"DNewHandlerAdapter64").value:0;return done(previous);
    }
    if(method=="NewHandlerAdapter"){
        if(!s.std_new_handler)return done(0);
        start_callbacks(state,memory,{{s.std_new_handler,{}}},Continuation::Return,1);return {};
    }
    if(name.find("@threads@stdext@@")!=name.npos){
        // stdext::threads（舊版執行緒介面）的互斥鎖：CRT heap 配置的一個 CRITICAL_SECTION，等待經 kernel32 臨界區段。
        if(method=="_Mtx_new"){
            const auto cell=a(0);memory.check(GuestAddress{cell},l.p,Access::Write);
            const auto section=allocate_heap(memory,x86_?24U:40U);if(!section){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
            put(cell,section);start_callbacks(state,memory,{{resolve("kernel32.dll","InitializeCriticalSection").value,{section}}},Continuation::Return,0);return {};
        }
        if(method=="_Mtx_delete"){start_callbacks(state,memory,{{resolve("kernel32.dll","DeleteCriticalSection").value,{a(0)}}},Continuation::ReturnZeroFreeHeap,a(0));return {};}
        if(method=="_Mtx_lock" || method=="_Mtx_unlock"){start_callbacks(state,memory,{{resolve("kernel32.dll",method=="_Mtx_lock"?"EnterCriticalSection":"LeaveCriticalSection").value,{a(0)}}},Continuation::Return,0);return {};}
        throw HostCallFault("未知 stdext::threads 操作");
    }
    if(name.starts_with("??0id@locale@") || name.starts_with("??_Fid@locale@")){const bool closure=name.starts_with("??_F");put(object,closure?0:a(0));return done(closure?0:object);}
    if(name.starts_with("?_Getcat@facet@locale@"))return done(x86_?UINT32_MAX:UINT64_MAX); // locale::facet 本身沒有類別：(size_t)-1。
    if(l.modern && (name.starts_with("??0ctype_base@") || name.starts_with("??_Fctype_base@") || name.starts_with("??1ctype_base@"))){
        if(name.starts_with("??1")){put(object,m.facet_table14);return done(0);}
        const bool closure=name.starts_with("??_F");memory.write(GuestAddress{object},std::vector<std::uint8_t>(2*l.p));put(object,m.ctype_base14);put_refs(object,closure?0:a(0));return done(closure?0:object);
    }
    if(name.find("_Timevec@std@@")!=name.npos && !name.starts_with("?_Gettnames@")){
        // _Timevec 擁有 CRT 配置的時間名稱表：複製建構與指派轉移擁有權（來源變為空），解構以 free 釋放。
        if(name.starts_with("??0") || name.starts_with("??_F")){
            const bool closure=name.starts_with("??_F"),copy=name.ends_with("AEBV01@@Z") || name.ends_with("ABV01@@Z");
            const auto value=closure?0:copy?ptr(a(0)):a(0);if(copy)put(a(0),0);put(object,value);return done(closure?0:object);
        }
        if(name.starts_with("??1")){if(const auto table=ptr(object))(void)free_heap(memory,table,state);put(object,0);return done(0);}
        if(name.starts_with("??4")){if(a(0)!=object){if(const auto old=ptr(object))(void)free_heap(memory,old,state);put(object,ptr(a(0)));put(a(0),0);}return done(object);}
        if(method=="_Getptr")return done(ptr(object));
        throw HostCallFault("未知 _Timevec 操作");
    }
    if(name.find("_Winit@std@@")!=name.npos){
        // 寬字元標準串流的初始化計數；本實作沒有常駐的寬字元標準串流物件，最後一個解構沒有需要 flush 的對象。
        if(name.starts_with("??4"))return done(object);
        const auto count=static_cast<std::int32_t>(memory.load(GuestAddress{m.winit_count14},32));
        memory.store(GuestAddress{m.winit_count14},32,static_cast<std::uint32_t>(name.starts_with("??0")?count+1:count-1));return done(name.starts_with("??0")?object:0);
    }
    if(name.starts_with("?_Init_cnt_func@Init@ios_base@"))return done(m.init_count14);
    if(method=="SetWidth" || method=="SetPrecision"){
        memory.store(GuestAddress{a(0)+(method=="SetWidth"?l.width:l.precision)},64,a(1));return done(0);
    }
    if(method=="setw" || method=="setprecision"){
        const auto output=a(0);memory.check(GuestAddress{output},16,Access::Write);
        memory.write(GuestAddress{output},std::array<std::uint8_t,16>{});
        put(output,resolve("internal-msvcp140",std::string("D")+(method=="setw"?"SetWidth":"SetPrecision")+(x86_?"32":"64")).value);
        memory.store(GuestAddress{output+8},64,a(1));return done(output);
    }
    if(name.starts_with("??Bid@")){auto value=ptr(object);if(!value){
        if(l.modern){value=memory.load(GuestAddress{m.id_count14},32)+1;memory.store(GuestAddress{m.id_count14},32,value);}else value=++s.next_id;put(object,value);}return done(value);}
    if(method=="_Incref" || method=="_Decref" || method=="FacetIncref" || method=="FacetDecref"){
        auto n=refs(object);if(method=="_Incref" || method=="FacetIncref"){put_refs(object,n+1);return done(0);}
        if(n)put_refs(object,--n);return done(n?0:object);
    }
    if(l.modern && !internal && (name.find("time_get@")!=name.npos || name.find("time_put@")!=name.npos)){
        const bool getter=name.find("time_get@")!=name.npos;
        const auto table=getter?tables.time_get:name.find("time_put@G")!=name.npos?tables.time_put16:tables.time_put;
        if(name.starts_with("??0") || name.starts_with("??_F")){
            // (refs)、(const _Locinfo&, refs) 與受保護的 (const char* 名稱, refs)：名稱與日期順序採 C locale，facet 只保存 vtable 與參考數。
            const bool closure=name.starts_with("??_F"),second=name.find("_Locinfo@")!=name.npos || name.ends_with("@PEBD_K@Z") || name.ends_with("@PBDI@Z");
            memory.write(GuestAddress{object},std::vector<std::uint8_t>(4*l.p));put(object,table);put_refs(object,closure?0:a(second?1U:0U));
            s.facet_locales.insert_or_assign(object,Msvcp100State::FacetLocale{});return done(closure?0:object);
        }
        if(name.starts_with("??1")){s.facet_locales.erase(object);put(object,m.facet_table14);return done(0);}
        if(method=="_Init" || method=="_Tidy")return done(0); // 沒有從 _Locinfo 載入的名稱表，也沒有要釋放的資料。
        // 公開 get_*／date_order／get(規格) 轉給虛擬函式（衍生 facet 的覆寫也生效）。
        const unsigned slot=method=="date_order"?3U:method=="get_time"?4U:method=="get_date"?5U:method=="get_weekday"?6U:method=="get_monthname"?7U:method=="get_year"?8U:
            method=="get" && getter && name.ends_with("DD@Z")?9U:0U;
        if(slot){std::vector<std::uint64_t> args;const unsigned count=slot==3?0U:slot==9?(x86_?10U:8U):(x86_?8U:6U);for(unsigned i=0;i<count;++i)args.push_back(a(i));return virtual_call(object,slot,std::move(args));}
    }
    if(l.modern && method=="_Getcat" && name.find("time_put@")!=name.npos){
        // time_put 只保存 vtable 與參考數；格式化使用 C locale 的時間名稱（與 _Locinfo 預設相同），回傳 LC_TIME。
        if(a(0) && !ptr(a(0))){
            const auto facet=allocate_heap(memory,4*l.p);if(!facet){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
            memory.write(GuestAddress{facet},std::vector<std::uint8_t>(4*l.p));put(facet,name.find("time_put@G")!=name.npos?tables.time_put16:tables.time_put);s.facet_locales.emplace(facet,Msvcp100State::FacetLocale{});put(a(0),facet);
        }
        return done(5);
    }
    if(l.modern && method=="_Getcat" && name.find("time_get@")!=name.npos){
        // time_get 與 time_put 相同只保存 vtable 與參考數；名稱與日期順序採 C locale，回傳 LC_TIME。
        if(a(0) && !ptr(a(0))){
            const auto facet=allocate_heap(memory,4*l.p);if(!facet){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
            memory.write(GuestAddress{facet},std::vector<std::uint8_t>(4*l.p));put(facet,tables.time_get);s.facet_locales.emplace(facet,Msvcp100State::FacetLocale{});put(a(0),facet);
        }
        return done(5);
    }
    if(l.modern && (method.starts_with("TimeGet") || ((method=="get" || method=="_Getfmt" || method.starts_with("do_")) && name.find("time_get@")!=name.npos))){
        // x64：a(0) 回傳的 istreambuf_iterator、a(1)／a(2) first／last 的副本、a(3) ios_base、a(4) iostate、a(5) tm，之後為格式；
        // x86 的迭代器以值傳遞（各 2 個 DWORD），ios_base 起的引數往後移兩格。do_* 匯出即虛擬函式本身，直接執行同一實作。
        const std::string_view operation=method=="do_date_order"?"TimeGetDateOrder":method=="do_get_time"?"TimeGetTime":method=="do_get_date"?"TimeGetDate":
            method=="do_get_weekday"?"TimeGetWeekday":method=="do_get_monthname"?"TimeGetMonth":method=="do_get_year"?"TimeGetYear":method=="do_get"?"TimeGetDoGet":method;
        if(operation=="TimeGetDateOrder")return done(2); // time_base::mdy（C locale 的 %x 為 %m/%d/%y）
        if(operation.starts_with("do_"))throw HostCallFault("未知 time_get 虛擬函式");
        const unsigned at=x86_?5U:3U;std::u16string format;
        if(operation=="get"){
            const auto first=a(at+3),last=a(at+4);if(last<first || (last-first)/l.element>65536)throw HostCallFault("time_get 格式範圍無效");
            for(auto cursor=first;cursor<last;cursor+=l.element)format.push_back(static_cast<char16_t>(memory.load(GuestAddress{cursor},l.character_bits())));
        }else if(operation=="_Getfmt"){
            // 受保護的 _Getfmt 以窄字元、NUL 結尾的格式字串解析（與 get(格式範圍) 共用解析器）。
            for(const auto c:read_ansi(memory,a(at+3),65536))format.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
        }else if(operation=="TimeGetDoGet"){
            const auto mask=0xffU;format=u"%"; // 規格與修飾字元一律為 char。
            if(const auto modifier=static_cast<char16_t>(a(at+4)&mask))format.push_back(modifier);format.push_back(static_cast<char16_t>(a(at+3)&mask));
        }else format=operation=="TimeGetTime"?u"%H:%M:%S":operation=="TimeGetDate"?u"%m/%d/%y":operation=="TimeGetWeekday"?u"%a":operation=="TimeGetMonth"?u"%b":u"%Y";
        Operation op;op.service=service;op.object=object;op.modern=true;op.element=l.element;op.kind="time_get";
        op.output=a(0);op.buffer=x86_?a(1):ptr(a(1));op.limit=x86_?a(3):ptr(a(2));op.time_state=a(at+1);op.time_tm=a(at+2);op.numeric_text=std::move(format);
        memory.check(GuestAddress{op.output},2*l.p,Access::Write);memory.check(GuestAddress{op.time_state},4,Access::Write);memory.check(GuestAddress{op.time_tm},36,Access::Write);
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
    if(l.modern && (method=="TimePutDoPut" || ((method=="put" || method=="do_put") && name.find("time_put@")!=name.npos))){
        // put(fmtfirst, fmtlast) 與 put／do_put(specifier, modifier)：在宿主依 strftime 規則格式化，再沿用 num_put 的逐字寫出
        // （緩衝區滿時呼叫 overflow），回傳 ostreambuf_iterator。
        const bool range=method=="put" && (name.find("PEBD3@Z")!=name.npos || name.find("PEB_W3@Z")!=name.npos || name.find("PEB_W4@Z")!=name.npos ||
                                           name.find("PBD3@Z")!=name.npos || name.find("PB_W3@Z")!=name.npos || name.find("PB_W4@Z")!=name.npos ||
                                           name.find("PEBG3@Z")!=name.npos || name.find("PBG3@Z")!=name.npos || name.find("PEBG4@Z")!=name.npos || name.find("PBG4@Z")!=name.npos);
        const auto output=a(0);std::uint64_t buffer=0;bool failed=false;unsigned at=0;
        if(x86_){failed=(a(1)&255U)!=0;buffer=a(2);at=3;}else{failed=(memory.load(GuestAddress{a(1)},8)&255U)!=0;buffer=ptr(a(1)+l.p);at=2;}
        const auto ios=a(at),time=a(at+2);const auto mask=l.element==1?0xffU:0xffffU;
        std::u16string format;
        if(range){
            const auto first=a(at+3),last=a(at+4);if(last<first || (last-first)/l.element>65536)throw HostCallFault("time_put 格式範圍無效");
            for(auto cursor=first;cursor<last;cursor+=l.element)format.push_back(static_cast<char16_t>(memory.load(GuestAddress{cursor},l.character_bits())));
        }else{format=u"%";if(const auto modifier=static_cast<char16_t>(a(at+4)&mask))format.push_back(modifier);format.push_back(static_cast<char16_t>(a(at+3)&mask));}
        std::tm value{};const std::array fields{&value.tm_sec,&value.tm_min,&value.tm_hour,&value.tm_mday,&value.tm_mon,&value.tm_year,&value.tm_wday,&value.tm_yday,&value.tm_isdst};
        for(std::size_t i=0;i<fields.size();++i)*fields[i]=static_cast<std::int32_t>(memory.load(GuestAddress{time+i*4},32));
        const auto category=windows_crt_locale(u"C");if(!category)throw HostCallFault("缺少 C locale 時間資料");
        const auto data=windows_crt_time_data(*category);crt_time::Formatter formatter(data,value,u"UTC",0,65536);auto converted=formatter.run(format);
        Operation op;op.service=service;op.object=object;op.modern=true;op.element=l.element;op.kind="num_put";op.output=output;op.ios=ios;op.buffer=buffer;
        op.numeric_failed=failed;op.phase=4;op.numeric_text=converted.error==crt_time::Error::None?std::move(converted.text):std::u16string{};
        memory.check(GuestAddress{output},2*l.p,Access::Write);
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
    if(method=="_Getcat"){
        if(a(0) && !ptr(a(0))){
            const auto loc=a(1)?ptr(a(1)+l.locale_pointer()):global;
            const auto locale_name=ptr(loc+4*l.p+8);
            const auto text=locale_name?ansi_to_utf16(read_ansi(memory,locale_name,4095)):std::u16string{u"C"};
            const auto category=text==u"*"?std::optional<CrtLocaleCategory>{current_crt_locale(state,memory).categories[2]}:windows_crt_locale(text);
            if(!category){throw_msvc_exception(state,memory,service,MsvcExceptionKind::RuntimeError);return {};}
            const bool codecvt=name.find("codecvt@")!=name.npos;
            const bool cvt16=codecvt && name.find("codecvt@_SD")!=name.npos,cvt32=codecvt && name.find("codecvt@_UD")!=name.npos,cvt_char=codecvt && name.find("codecvt@DD")!=name.npos;
            const bool cvt_ushort=codecvt && name.find("codecvt@GD")!=name.npos,cvt_plain=cvt16 || cvt32 || cvt_char;
            const auto size=l.modern?(codecvt?(cvt16 || cvt32?2*l.p+8:cvt_char?2*l.p:64U):96U):64U;const auto facet=allocate_heap(memory,size);
            if(!facet){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}Msvcp100State::FacetLocale metadata;metadata.category=*category;
            try{
                memory.write(GuestAddress{facet},std::vector<std::uint8_t>(size));
                put(facet,codecvt?(l.modern?(cvt16?tables.codecvt16:cvt32?tables.codecvt32:cvt_ushort?tables.codecvt_ushort:tables.codecvt):s.codecvt_table):(l.modern?(name.find("ctype@G@")!=name.npos?tables.ctype16:tables.ctype):s.ctype_table));
                if(l.modern && cvt_plain){if(!cvt_char)memory.store(GuestAddress{facet+2*l.p},32,0x10ffffU);}
                else if(l.modern){
                    const auto cvt=codecvt?facet+2*l.p:facet+(x86_?24U:48U);
                    memory.store(GuestAddress{cvt},32,category->codepage);memory.store(GuestAddress{cvt+4},32,category->maximum_character_bytes);memory.store(GuestAddress{cvt+8},32,category->codepage==0);
                    for(unsigned c=0;c<256;++c)if(category->lead[c]){const auto at=cvt+12+c/8;memory.store(GuestAddress{at},8,memory.load(GuestAddress{at},8)|(1U<<(c%8)));}
                    if(!codecvt){
                        const auto table=allocate_heap(memory,512);if(!table)throw std::bad_alloc{};metadata.resources.push_back(table);const auto& values=windows_crt_character_table(*category);
                        for(unsigned c=0;c<256;++c)memory.store(GuestAddress{table+c*2},16,values.masks[c+1]);memory.store(GuestAddress{facet+2*l.p},32,category->codepage);
                        put(facet+2*l.p+(x86_?4U:8U),table);memory.store(GuestAddress{facet+4*l.p},32,1);
                        if(category->codepage){const auto bytes=utf16_bytes(category->name);const auto category_text=allocate_heap(memory,bytes.size());if(!category_text)throw std::bad_alloc{};metadata.resources.push_back(category_text);memory.write(GuestAddress{category_text},bytes);put(facet+(x86_?20U:40U),category_text);}
                    }
                }
                s.facet_locales.emplace(facet,metadata);put(a(0),facet);
            }catch(...){for(const auto resource:metadata.resources)(void)free_heap(memory,resource,state);s.facet_locales.erase(facet);(void)free_heap(memory,facet,state);throw;}
        }
        return done(2);
    }
    if(method=="FacetDelete"){
        if(a(0)&2U)throw HostCallFault("VC10 facet 不接受陣列刪除");
        if(s.owned_locale_imps.contains(object)){
            Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="destroy_locale_imp";op.object=object;op.output=ptr(object+2*l.p);op.limit=ptr(object+3*l.p);op.final_result=a(0);
            const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
        }
        if(const auto locale=s.facet_locales.find(object);locale!=s.facet_locales.end()){for(const auto resource:locale->second.resources)(void)free_heap(memory,resource,state);s.facet_locales.erase(locale);}
        else if(l.modern){const auto type=ptr(object);if(type && (type==m.tables14[0].ctype || type==m.tables14[1].ctype || type==m.tables14[1].ctype16))tidy_msvcp_ctype(object);}
        if((a(0)&1U) && !free_heap(memory,object,state))throw HostCallFault("VC10 facet 配置無效");return done(object);
    }
    if(!l.modern && method=="is" && name.find("ctype@")!=name.npos)return done((character_mask(static_cast<char16_t>(a(1)))&a(0))!=0);
    if(method=="always_noconv")return virtual_call(object,l.facet_slot(1),{});
    if(l.modern && (name.find("codecvt@")!=name.npos || name.find("codecvt_base@")!=name.npos)){
        if(method=="length")return virtual_call(object,l.facet_slot(7),{a(0),a(1),a(2),a(3)});
        if(method=="encoding")return virtual_call(object,l.facet_slot(3),{});
        if(method=="max_length")return virtual_call(object,l.facet_slot(2),{});
        if(method=="_Init"){
            // codecvt<wchar_t／unsigned short>::_Init 由 locale 取得碼頁向量；其他 codecvt 沒有 locale 資料。
            if(name.find("codecvt@_WD")!=name.npos || name.find("codecvt@GD")!=name.npos){
                const auto category=current_crt_locale(state,memory).categories[2];const auto vector=object+2*l.p;memory.write(GuestAddress{vector},std::vector<std::uint8_t>(44));
                memory.store(GuestAddress{vector},32,category.codepage);memory.store(GuestAddress{vector+4},32,category.maximum_character_bytes);memory.store(GuestAddress{vector+8},32,category.codepage==0);
                for(unsigned c=0;c<256;++c)if(category.lead[c]){const auto at=vector+12+c/8;memory.store(GuestAddress{at},8,memory.load(GuestAddress{at},8)|(1U<<(c%8)));}
                Msvcp100State::FacetLocale metadata;metadata.category=category;s.facet_locales.insert_or_assign(object,std::move(metadata));
            }
            return done(0);
        }
    }
    if(method=="in" || method=="out" || method=="unshift"){
        std::vector<std::uint64_t> args;for(unsigned i=0;i<(method=="unshift"?4U:7U);++i)args.push_back(a(i));
        return virtual_call(object,l.facet_slot(method=="in"?4:method=="out"?5:6),std::move(args));
    }
    // do_* 匯出就是虛擬函式本身：直接執行同一實作（不經虛擬表，避免衍生類別呼叫基底版本時遞迴）。
    std::string codecvt_method(method);
    if(l.modern && method.starts_with("do_") && (name.find("codecvt@")!=name.npos || name.find("codecvt_base@")!=name.npos)){
        const std::string family=name.find("codecvt@_SD")!=name.npos?"Codecvt16":name.find("codecvt@_UD")!=name.npos?"Codecvt32":name.find("codecvt_base@")!=name.npos?"CodecvtBase":"Codecvt";
        const auto suffix=method=="do_always_noconv"?"Noconv":method=="do_max_length"?"MaxLength":method=="do_encoding"?"Encoding":method=="do_in"?"In":method=="do_out"?"Out":method=="do_unshift"?"Unshift":method=="do_length"?"Length":"";
        if(!*suffix)throw HostCallFault("未知 codecvt 虛擬函式");
        codecvt_method=family+suffix;
    }
    if(codecvt_method.starts_with("CodecvtBase")){
        // codecvt_base：do_always_noconv 為 false，do_max_length 與 do_encoding 為 1。
        return done(codecvt_method=="CodecvtBaseNoconv"?0U:1U);
    }
    if(l.modern && l.element==1 && codecvt_method.starts_with("Codecvt") && !codecvt_method.starts_with("Codecvt16") && !codecvt_method.starts_with("Codecvt32")){
        // codecvt<char, char, mbstate_t>：always_noconv；in／out／unshift 回 noconv（3）並把 next 設為起點，length 為 min(範圍, max)。
        if(codecvt_method=="CodecvtNoconv" || codecvt_method=="CodecvtMaxLength" || codecvt_method=="CodecvtEncoding")return done(1);
        if(codecvt_method=="CodecvtLength"){if(a(2)<a(1))throw HostCallFault("STL codecvt length 範圍無效");return done(static_cast<std::uint32_t>(std::min<std::uint64_t>({a(2)-a(1),a(3),INT32_MAX})));}
        if(codecvt_method=="CodecvtUnshift"){put(a(3),a(1));return done(3);}
        put(a(3),a(1));put(a(6),a(4));return done(3);
    }
    if(l.modern && (codecvt_method.starts_with("Codecvt16") || codecvt_method.starts_with("Codecvt32"))){
        // codecvt<char16_t／char32_t, char>：UTF-16／UTF-32 與 UTF-8 互轉。超過 _Maxcode、代理字元或不合法序列回 error（2），輸入不完整或輸出不足回 partial（1）。
        // _Mode：consume_header（4）在輸入開頭略過 UTF-8 BOM，generate_header（2）在輸出開頭寫入 BOM；是否已處理記在 _Mbstatet 的 _State 位元 0。
        const bool wide32=codecvt_method.starts_with("Codecvt32");const std::string_view operation=std::string_view(codecvt_method).substr(9);
        const auto maxcode=std::min<std::uint32_t>(0x10ffffU,static_cast<std::uint32_t>(memory.load(GuestAddress{object+2*l.p},32)));
        const auto mode=static_cast<std::uint32_t>(memory.load(GuestAddress{object+2*l.p+4},32));const unsigned unit=wide32?4U:2U;
        if(operation=="Noconv" || operation=="Encoding")return done(0);
        if(operation=="MaxLength")return done((mode&4U)?7U:4U);
        if(operation=="Unshift"){put(a(3),a(1));return done(3);}
        const auto state_at=a(0);memory.check(GuestAddress{state_at},8,Access::Write);
        const auto header_done=[&]{return (memory.load(GuestAddress{state_at+6},16)&1U)!=0;};
        const auto mark_header=[&]{memory.store(GuestAddress{state_at+6},16,memory.load(GuestAddress{state_at+6},16)|1U);};
        const auto byte=[&](std::uint64_t at){return static_cast<std::uint32_t>(memory.load(GuestAddress{at},8));};
        if(operation=="In" || operation=="Length"){
            auto from=a(1);const auto last=a(2);if(last<from)throw HostCallFault("STL codecvt 輸入範圍無效");
            const bool counting=operation=="Length";auto to=counting?0:a(4);const auto end=counting?0:a(5);
            if(!counting && (end<to || (end-to)%unit))throw HostCallFault("STL codecvt 輸出範圍無效");
            std::uint64_t produced=0;unsigned result=0;
            if((mode&4U) && !header_done()){
                const std::array<std::uint32_t,3> bom{0xefU,0xbbU,0xbfU};std::size_t matched=0;
                while(matched<3 && from+matched<last && byte(from+matched)==bom[matched])++matched;
                if(matched==3){from+=3;mark_header();}
                else if(from+matched==last && matched)result=1; // 只收到 BOM 的前段，等待其餘位元組。
                else if(from<last)mark_header();
            }
            while(!result && from<last){
                if(counting && produced>=a(3))break;
                const auto lead=byte(from);const unsigned length=lead<0x80U?1U:lead<0xc2U?0U:lead<0xe0U?2U:lead<0xf0U?3U:lead<0xf5U?4U:0U;
                if(!length){result=2;break;}
                std::uint32_t code=length==1?lead:lead&(0xffU>>(length+1));bool incomplete=false,invalid=false;
                for(unsigned i=1;i<length;++i){if(from+i>=last){incomplete=true;break;}const auto next=byte(from+i);if((next&0xc0U)!=0x80U){invalid=true;break;}code=(code<<6)|(next&0x3fU);}
                if(invalid){result=2;break;}
                if(incomplete){result=1;break;}
                if((length==3 && code<0x800U) || (length==4 && code<0x10000U) || (code>=0xd800U && code<=0xdfffU) || code>maxcode){result=2;break;}
                const unsigned units=!wide32 && code>0xffffU?2U:1U;
                if(counting){if(produced+units>a(3))break;produced+=units;from+=length;continue;}
                if((end-to)/unit<units){result=1;break;}
                if(wide32)memory.store(GuestAddress{to},32,code);
                else if(units==1)memory.store(GuestAddress{to},16,code);
                else{memory.store(GuestAddress{to},16,0xd800U+((code-0x10000U)>>10));memory.store(GuestAddress{to+2},16,0xdc00U+((code-0x10000U)&0x3ffU));}
                to+=units*unit;from+=length;
            }
            if(counting)return done(static_cast<std::uint32_t>(std::min<std::uint64_t>(from-a(1),INT32_MAX)));
            put(a(3),from);put(a(6),to);return done(result);
        }
        if(operation=="Out"){
            auto from=a(1);const auto last=a(2);auto to=a(4);const auto end=a(5);unsigned result=0;
            if(last<from || end<to || (last-from)%unit)throw HostCallFault("STL codecvt 範圍無效");
            if((mode&2U) && !header_done()){if(end-to<3)result=1;else{memory.store(GuestAddress{to},8,0xefU);memory.store(GuestAddress{to+1},8,0xbbU);memory.store(GuestAddress{to+2},8,0xbfU);to+=3;mark_header();}}
            while(!result && from<last){
                std::uint32_t code=static_cast<std::uint32_t>(memory.load(GuestAddress{from},static_cast<std::uint8_t>(unit*8)));unsigned consumed=unit;
                if(!wide32 && code>=0xd800U && code<=0xdbffU){
                    if(last-from<4){result=1;break;}
                    const auto low=static_cast<std::uint32_t>(memory.load(GuestAddress{from+2},16));if(low<0xdc00U || low>0xdfffU){result=2;break;}
                    code=0x10000U+((code-0xd800U)<<10)+(low-0xdc00U);consumed=4;
                }
                if((code>=0xd800U && code<=0xdfffU) || code>maxcode){result=2;break;}
                std::array<std::uint8_t,4> bytes{};unsigned count=0;
                if(code<0x80U)bytes[count++]=static_cast<std::uint8_t>(code);
                else if(code<0x800U){bytes[count++]=static_cast<std::uint8_t>(0xc0U|(code>>6));bytes[count++]=static_cast<std::uint8_t>(0x80U|(code&0x3fU));}
                else if(code<0x10000U){bytes[count++]=static_cast<std::uint8_t>(0xe0U|(code>>12));bytes[count++]=static_cast<std::uint8_t>(0x80U|((code>>6)&0x3fU));bytes[count++]=static_cast<std::uint8_t>(0x80U|(code&0x3fU));}
                else{bytes[count++]=static_cast<std::uint8_t>(0xf0U|(code>>18));bytes[count++]=static_cast<std::uint8_t>(0x80U|((code>>12)&0x3fU));bytes[count++]=static_cast<std::uint8_t>(0x80U|((code>>6)&0x3fU));bytes[count++]=static_cast<std::uint8_t>(0x80U|(code&0x3fU));}
                if(end-to<count){result=1;break;}
                memory.write(GuestAddress{to},std::span(bytes).first(count));to+=count;from+=consumed;
            }
            put(a(3),from);put(a(6),to);return done(result);
        }
        throw HostCallFault("未知 codecvt<char16_t／char32_t> 操作");
    }
    if(codecvt_method!=method){
        // wchar_t／unsigned short 的 do_* 匯出：以內部方法名稱重新進入同一處理（引數順序相同）。
        const auto internal_name=std::string(1,'W')+codecvt_method+(x86_?"32":"64");
        for(const auto& entry:runtime_service_descriptors())if(entry.family==modern_internal && entry.symbol==internal_name)return invoke_msvcp100(state,memory,entry);
        throw HostCallFault("codecvt 虛擬函式缺少內部入口");
    }
    if(method=="CodecvtNoconv")return done(0);
    if(method=="CodecvtMaxLength")return done(l.modern?memory.load(GuestAddress{object+2*l.p+4},32):1);
    if(method=="CodecvtEncoding")return done(l.modern?memory.load(GuestAddress{object+2*l.p+4},32)==1:0);
    if(method=="CodecvtUnshift" || method=="CodecvtLength" || method=="CodecvtIn" || method=="CodecvtOut"){
        const auto found=s.facet_locales.find(object);const auto page=found!=s.facet_locales.end()?found->second.category.codepage:l.modern?static_cast<std::uint32_t>(memory.load(GuestAddress{object+2*l.p},32)):0U;
        const auto category=windows_crt_codepage(page);if(!category)throw HostCallFault("STL codecvt 碼頁無效");
        if(l.modern){
            // VC14 wchar_t codecvt 每次只轉換一個 UTF-16 單元。半個輸入字元留給
            // 呼叫者補齊；_Mbrtowc／_Wcrtomb 不把它存入 mbstate_t。
            const auto invalid=[&]{memory.store(GuestAddress{runtime_.errno_cell},32,42);};
            const auto decode=[&](std::uint64_t from,std::uint64_t last){
                std::array<std::uint8_t,4> bytes{};
                const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(last-from,category->maximum_character_bytes));
                memory.read(GuestAddress{from},std::span(bytes).first(count));
                // UTF-8 的四位元組前導立即失敗，即使其餘位元組尚未到齊。
                if(page==65001 && bytes[0]>=0xf0)return CrtDecodedCharacter{};
                auto value=windows_crt_decode(std::span(bytes).first(count),page);
                if(value.status==CrtDecodedCharacter::Status::Complete && value.character>0xffff)return CrtDecodedCharacter{};
                return value;
            };
            if(method=="CodecvtUnshift"){
                if(a(2)<a(1))throw HostCallFault("STL codecvt unshift 範圍無效");
                put(a(3),a(1));return done(0);
            }
            auto from=a(1);const auto last=a(2);
            if(last<from)throw HostCallFault("STL codecvt 輸入範圍無效");
            if(method=="CodecvtLength"){
                for(std::uint64_t count=0;from<last && count<a(3);++count){
                    const auto value=decode(from,last);
                    if(value.status!=CrtDecodedCharacter::Status::Complete){if(value.status==CrtDecodedCharacter::Status::Invalid)invalid();break;}
                    from+=value.bytes;
                }
                return done(static_cast<std::uint32_t>(std::min<std::uint64_t>(from-a(1),INT32_MAX)));
            }
            const bool input=method=="CodecvtIn";auto to=a(4);const auto end=a(5);unsigned result=0;
            if(end<to || (input?(end-to)%2:(last-from)%2))throw HostCallFault("STL codecvt 輸出範圍無效");
            while(from<last){
                if(to==end){result=1;break;}
                if(input){
                    const auto value=decode(from,last);
                    if(value.status!=CrtDecodedCharacter::Status::Complete){result=value.status==CrtDecodedCharacter::Status::Partial?1:2;if(result==2)invalid();break;}
                    memory.store(GuestAddress{to},16,value.character);from+=value.bytes;to+=2;
                }else{
                    const auto value=static_cast<char16_t>(memory.load(GuestAddress{from},16));
                    const auto bytes=windows_crt_encode(std::u16string_view(&value,1),page);
                    if(!bytes){invalid();result=2;break;}
                    if(bytes->size()>end-to){result=1;break;}
                    memory.write(GuestAddress{to},*bytes);from+=2;to+=bytes->size();
                }
            }
            put(a(3),from);put(a(6),to);return done(result);
        }
        std::array<std::uint8_t,8> pending{};memory.read(GuestAddress{a(0)},pending);
        const auto reset=[&]{pending={};};const auto save=[&]{memory.write(GuestAddress{a(0)},pending);};
        if(method=="CodecvtUnshift"){put(a(3),a(1));return done(pending[7]?2:3);}
        if(method=="CodecvtLength"){
            auto from=a(1);const auto last=a(2);std::uint64_t chars=0;if(last<from)throw HostCallFault("STL codecvt length 範圍無效");
            if(pending[7]==0x73 && a(3)){++chars;reset();}
            while(from<last && chars<a(3)){
                const auto prefix=pending[7]==0x6d?pending[6]:0U;if(prefix>6 || (pending[7] && pending[7]!=0x6d))break;
                const auto maximum=std::min<unsigned>(6-prefix,category->maximum_character_bytes);const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(last-from,maximum));
                if(!n)break;std::array<std::uint8_t,8> bytes{};std::copy_n(pending.begin(),prefix,bytes.begin());memory.read(GuestAddress{from},std::span(bytes).subspan(prefix,n));
                const auto decoded=windows_crt_decode(std::span(bytes).first(prefix+n),page);
                if(decoded.status==CrtDecodedCharacter::Status::Partial){if(prefix+n<6){reset();std::copy_n(bytes.begin(),prefix+n,pending.begin());pending[6]=static_cast<std::uint8_t>(prefix+n);pending[7]=0x6d;}break;}
                if(decoded.status!=CrtDecodedCharacter::Status::Complete || decoded.bytes<prefix)break;
                const auto units=decoded.character>0xffff?2U:1U;if(units>a(3)-chars)break;
                from+=decoded.bytes-prefix;chars+=units;reset();
            }save();return done(static_cast<std::uint32_t>(std::min<std::uint64_t>(from-a(1),INT32_MAX)));
        }
        const bool input=method=="CodecvtIn";auto from=a(1),to=a(4);unsigned result=0;
        if(a(2)<from || a(5)<to || (input?(a(5)-to)%2:(a(2)-from)%2))throw HostCallFault("STL codecvt 範圍無效");
        if(input && pending[7]==0x73){if(a(5)-to<2)result=1;else{memory.store(GuestAddress{to},16,pending[0]|(unsigned(pending[1])<<8));to+=2;reset();}}
        while(!result && from<a(2)){
            if(input){
                if(a(5)-to<2){result=1;break;}const auto prefix=pending[7]==0x6d?pending[6]:0U;if(prefix>6 || (pending[7] && pending[7]!=0x6d)){result=2;break;}
                std::array<std::uint8_t,8> bytes{};std::copy_n(pending.begin(),prefix,bytes.begin());const auto take=static_cast<std::size_t>(std::min<std::uint64_t>(a(2)-from,std::min<unsigned>(6-prefix,category->maximum_character_bytes)));
                memory.read(GuestAddress{from},std::span(bytes).subspan(prefix,take));const auto decoded=windows_crt_decode(std::span(bytes).first(prefix+take),page);
                if(decoded.status==CrtDecodedCharacter::Status::Invalid || (decoded.status==CrtDecodedCharacter::Status::Complete && decoded.bytes<prefix)){result=2;break;}
                if(decoded.status==CrtDecodedCharacter::Status::Partial){if(prefix+take>=6){result=2;break;}reset();std::copy_n(bytes.begin(),prefix+take,pending.begin());pending[6]=static_cast<std::uint8_t>(prefix+take);pending[7]=0x6d;from+=take;result=1;break;}
                from+=decoded.bytes-prefix;reset();const auto value=decoded.character;
                if(value>0xffff){const auto high=0xd800U+((value-0x10000U)>>10),low=0xdc00U+((value-0x10000U)&1023U);memory.store(GuestAddress{to},16,high);to+=2;
                    if(a(5)-to<2){pending[0]=static_cast<std::uint8_t>(low);pending[1]=static_cast<std::uint8_t>(low>>8);pending[7]=0x73;result=1;break;}memory.store(GuestAddress{to},16,low);to+=2;
                }else{memory.store(GuestAddress{to},16,value);to+=2;}
            }else{
                std::array<char16_t,2> text{};const bool prefix=pending[7]==0x74;const auto current=static_cast<char16_t>(memory.load(GuestAddress{from},16));unsigned units=1,consume=2;
                if(prefix){text[0]=static_cast<char16_t>(pending[0]|(unsigned(pending[1])<<8));text[1]=current;units=2;if(current<0xdc00 || current>0xdfff){result=2;break;}}
                else {if(pending[7]){result=2;break;}text[0]=current;
                    if(current>=0xd800 && current<=0xdbff){if(a(2)-from<4){pending[0]=static_cast<std::uint8_t>(current);pending[1]=static_cast<std::uint8_t>(current>>8);pending[7]=0x74;from+=2;result=1;break;}text[1]=static_cast<char16_t>(memory.load(GuestAddress{from+2},16));units=2;consume=4;}
                }
                const auto encoded=windows_crt_encode(std::u16string_view(text.data(),units),page);if(!encoded){result=2;break;}if(encoded->size()>a(5)-to){result=1;break;}
                memory.write(GuestAddress{to},*encoded);to+=encoded->size();from+=consume;reset();
            }
        }
        if(!result && pending[7])result=1;save();put(a(3),from);put(a(6),to);return done(result);
    }
    if(l.modern && (name.starts_with("??0facet@") || name.starts_with("??_Ffacet@") || name.starts_with("??0?$codecvt@") || name.starts_with("??_F?$codecvt@") || name.starts_with("??0codecvt_base@"))){
        // refs 在 _Locinfo 之後；char16_t／char32_t 的 (_Locinfo, maxcode, mode, refs) 另存最大碼點與標頭模式（預設 0x10FFFF、0）；預設建構閉包 refs 0。
        const bool closure=name.starts_with("??_F"),base=name.starts_with("??0codecvt_base@"),codecvt=!name.starts_with("??0facet@") && !name.starts_with("??_Ffacet@") && !base;
        const bool cvt16=name.find("codecvt@_SD")!=name.npos,cvt32=name.find("codecvt@_UD")!=name.npos,cvt_char=name.find("codecvt@DD")!=name.npos,cvt_ushort=name.find("codecvt@GD")!=name.npos;
        const bool extended=name.find("W4_Codecvt_mode@")!=name.npos,locinfo=name.find("_Locinfo@")!=name.npos;
        const unsigned size=codecvt?(cvt16 || cvt32?2*l.p+8:cvt_char?2*l.p:64U):2*l.p;
        memory.write(GuestAddress{object},std::vector<std::uint8_t>(size));
        put(object,base?m.codecvt_base14:!codecvt?m.facet_table14:cvt16?tables.codecvt16:cvt32?tables.codecvt32:cvt_ushort?tables.codecvt_ushort:tables.codecvt);
        put_refs(object,closure?0:a(extended?3:locinfo?1:0));
        if(cvt16 || cvt32){memory.store(GuestAddress{object+2*l.p},32,extended?static_cast<std::uint32_t>(a(1)):0x10ffffU);memory.store(GuestAddress{object+2*l.p+4},32,extended?static_cast<std::uint32_t>(a(2)):0U);}
        if(codecvt && !cvt16 && !cvt32 && !cvt_char){const auto category=current_crt_locale(state,memory).categories[2];const auto vector=object+2*l.p;
            memory.store(GuestAddress{vector},32,category.codepage);memory.store(GuestAddress{vector+4},32,category.maximum_character_bytes);memory.store(GuestAddress{vector+8},32,category.codepage==0);
            for(unsigned c=0;c<256;++c)if(category.lead[c]){const auto at=vector+12+c/8;memory.store(GuestAddress{at},8,memory.load(GuestAddress{at},8)|(1U<<(c%8)));}
            Msvcp100State::FacetLocale metadata;metadata.category=category;s.facet_locales.insert_or_assign(object,std::move(metadata));
        }return done(closure?0:object);
    }
    if(l.modern && (name.starts_with("??1facet@") || name.starts_with("??1?$codecvt@"))){s.facet_locales.erase(object);put(object,m.facet_table14);return done(0);}
    // ctype<char／wchar_t／unsigned short> 的建構、解構、_Init 與 _Tidy：只寫 MSVC 物件本身的欄位，不越過 sizeof(ctype)——
    // vptr、refs、_Ctypevec（2 個指標寬之後），寬字元版另有 _Cvtvec（x86 +24、x64 +48，44 位元組）。
    if(l.modern && name.find("?$ctype@")!=name.npos && (name.starts_with("??0?$ctype@") || name.starts_with("??1?$ctype@") || method=="_Init" || method=="_Tidy")){
        const auto vector=object+2*l.p,table_at=vector+(x86_?4U:8U),delfl_at=vector+2*l.p,cvt_at=object+(x86_?24U:48U);
        const bool narrow_type=l.element==1,ushort_type=name.find("ctype@G@")!=name.npos;
        if(method=="_Tidy"){tidy_msvcp_ctype(object);return done(0);}
        if(method=="_Init"){write_ctypevec(vector);if(!narrow_type)write_cvtvec(cvt_at);return done(0);}
        if(name.starts_with("??1")){
            if(const auto locale=s.facet_locales.find(object);locale!=s.facet_locales.end()){for(const auto resource:locale->second.resources)(void)free_heap(memory,resource,state);s.facet_locales.erase(locale);}
            else tidy_msvcp_ctype(object);
            put(object,m.facet_table14);return done(0);
        }
        // 建構：(_Locinfo, refs) 依該 _Locinfo 目前套用的 CRT locale 填入；char 的 (表, 是否刪除, refs) 與寬字元的 (refs) 以預設 _Locinfo（"C"）初始化。
        const bool by_locinfo=name.find("_Locinfo@")!=name.npos,table_constructor=narrow_type && !by_locinfo;
        const auto size=narrow_type?6*l.p:(x86_?24U:48U)+44U;memory.check(GuestAddress{object},size,Access::Write);memory.fill(GuestAddress{object},0,size);
        put(object,ushort_type?tables.ctype16:tables.ctype);put_refs(object,a(by_locinfo?1:table_constructor?2:0));
        if(by_locinfo){write_ctypevec(vector);if(!narrow_type)write_cvtvec(cvt_at);}
        else if(table_constructor){
            // MSVC 在 "C" 的 _Init 之後立即 _Tidy 再換表：最終碼頁 0、無名稱；未給表時用 classic 表且不擁有。
            const auto table=a(0);put(table_at,table?table:m.ctype_masks14);memory.store(GuestAddress{delfl_at},32,table && (a(1)&255U)?UINT32_MAX:0U);
        }else{
            const auto copy=allocate_heap(memory,512);if(!copy){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
            std::array<std::uint8_t,512> masks{};memory.read(GuestAddress{m.ctype_masks14},masks);memory.write(GuestAddress{copy},masks);
            put(table_at,copy);memory.store(GuestAddress{delfl_at},32,1);
            std::array<std::uint8_t,44> classic{};classic[4]=1;classic[8]=1;memory.write(GuestAddress{cvt_at},classic); // 碼頁 0、最大 1 位元組、C locale
        }
        return done(object);
    }
    // 區間多載的簽章含指標；x64 與 x86 裝飾名都只在此處出現 P。
    const bool ctype_range=name.find("@?$ctype@")!=name.npos && name.substr(name.rfind("@@")+2).find('P')!=name.npos;
    // 公開包裝轉呼叫的虛擬函式仍是本引擎的基底實作（facet 沒有覆寫）時直接計算，不經客體回呼；引數與虛擬函式相同。
    std::string devirtualized;
    const auto base_virtual=[&](unsigned slot,std::string_view operation){
        const auto entry=std::string(1,l.element==1?'D':'W')+std::string(operation)+(x86_?"32":"64");
        return ptr(ptr(object)+slot*l.p)==resolve(msvcp_detail::modern_internal(l.variant()),entry).value;
    };
    // ctype 分類公開成員：char 版依物件自己的 _Ctypevec 表（MSVC 標頭內聯語義，不經虛擬函式）；
    // 寬字元版轉呼叫 do_is／do_scan_is／do_scan_not（衍生 facet 的覆寫一併生效，基底實作直接計算）。
    if(l.modern && name.find("@?$ctype@")!=name.npos && (method=="is" || method=="scan_is" || method=="scan_not" || method=="table" || method=="classic_table")){
        if(method=="classic_table")return done(m.ctype_masks14);
        const auto own_table=[&]{const auto value=ptr(object+2*l.p+(x86_?4U:8U));return value?value:m.ctype_masks14;};
        if(method=="table")return done(own_table());
        const bool scan=method!="is";
        if(l.element==1){
            const auto masks=own_table();const auto mask_of=[&](std::uint64_t c){return static_cast<std::uint32_t>(memory.load(GuestAddress{masks+(c&255U)*2},16));};
            if(!scan && !ctype_range)return done((mask_of(a(1))&a(0)&0xffffU)!=0);
            if(!scan){
                const auto first=a(0),last=a(1),output=a(2);if(last<first)throw HostCallFault("STL ctype 分類範圍無效");const auto count=last-first;
                memory.check(GuestAddress{first},static_cast<std::size_t>(count),Access::Read);memory.check(GuestAddress{output},static_cast<std::size_t>(count*2),Access::Write);
                for(std::uint64_t i=0;i<count;++i)memory.store(GuestAddress{output+i*2},16,mask_of(memory.load(GuestAddress{first+i},8)));return done(last);
            }
            auto first=a(1);const auto last=a(2);if(last<first)throw HostCallFault("STL ctype 掃描範圍無效");
            const auto wanted=static_cast<std::uint32_t>(a(0)&0xffffU);const bool want_match=method=="scan_is";
            while(first<last && ((mask_of(memory.load(GuestAddress{first},8))&wanted)!=0)!=want_match)++first;return done(first);
        }
        const unsigned slot=method=="scan_is"?5U:method=="scan_not"?6U:ctype_range?3U:4U;
        const std::string operation=method=="scan_is"?"CtypeScanIs":method=="scan_not"?"CtypeScanNot":ctype_range?"CtypeIsRange":"CtypeIs";
        if(!base_virtual(slot,operation))return virtual_call(object,slot,scan?std::vector<std::uint64_t>{a(0)&0xffffU,a(1),a(2)}:
            ctype_range?std::vector<std::uint64_t>{a(0),a(1),a(2)}:std::vector<std::uint64_t>{a(0)&0xffffU,a(1)&0xffffU});
        devirtualized=operation;
    }
    if(l.modern && (method=="tolower" || method=="toupper")){
        const auto slot=(l.element==1?3U:7U)+(method=="toupper"?2U:0U)+(ctype_range?0U:1U);
        const std::string operation=std::string(method=="toupper"?"CtypeUpper":"CtypeLower")+(ctype_range?"Range":"");
        if(!base_virtual(slot,operation))return virtual_call(object,slot,ctype_range?std::vector<std::uint64_t>{a(0),a(1)}:std::vector<std::uint64_t>{a(0)});
        devirtualized=operation;
    }
    // ctype::widen／narrow 單字元公開包裝：與標頭內聯版本相同轉呼叫 do_widen／do_narrow（衍生 facet 的覆寫一併生效）。
    if(l.modern && (method=="widen" || method=="narrow") && name.find("@?$ctype@")!=name.npos){
        const bool widen=method=="widen";const auto slot=(l.element==1?8U:12U)+(widen?0U:2U)-(ctype_range?1U:0U);
        const std::string operation=std::string(widen?"CtypeWiden":"CtypeNarrow")+(ctype_range?"Range":"");
        if(!base_virtual(slot,operation)){
            if(ctype_range)return virtual_call(object,slot,widen?std::vector<std::uint64_t>{a(0),a(1),a(2)}:std::vector<std::uint64_t>{a(0),a(1),a(2)&255U,a(3)});
            return virtual_call(object,slot,widen?std::vector<std::uint64_t>{a(0)&255U}:std::vector<std::uint64_t>{a(0)&(l.element==1?255U:65535U),a(1)&255U});
        }
        devirtualized=operation;
    }
    // ctype 虛擬函式的基底實作：衍生 facet 直接呼叫時與內部 vtable 入口走同一流程。
    const bool ctype_virtual=method.starts_with("do_") && name.find("@?$ctype@")!=name.npos;
    if(method.starts_with("Ctype") || ctype_virtual || !devirtualized.empty()){
        const std::string_view operation=!devirtualized.empty()?std::string_view(devirtualized):!ctype_virtual?method:method=="do_is"?(ctype_range?"CtypeIsRange":"CtypeIs"):method=="do_scan_is"?"CtypeScanIs":method=="do_scan_not"?"CtypeScanNot":
            method=="do_tolower"?(ctype_range?"CtypeLowerRange":"CtypeLower"):method=="do_toupper"?(ctype_range?"CtypeUpperRange":"CtypeUpper"):method=="do_widen"?(ctype_range?"CtypeWidenRange":"CtypeWiden"):(ctype_range?"CtypeNarrowRange":"CtypeNarrow");
        if(operation=="CtypeIs")return done((character_mask(static_cast<char16_t>(a(1)))&a(0))!=0);
        if(operation=="CtypeIsRange"){
            if(a(1)<a(0) || (a(1)-a(0))%l.element)throw HostCallFault("STL ctype 分類範圍無效");const auto count=(a(1)-a(0))/l.element;
            for(std::uint64_t i=0;i<count;++i)memory.store(GuestAddress{a(2)+i*2},16,character_mask(static_cast<char16_t>(memory.load(GuestAddress{a(0)+i*l.element},l.character_bits()))));return done(a(1));
        }
        if(operation=="CtypeScanIs" || operation=="CtypeScanNot"){
            auto first=a(1);if(a(2)<first || (a(2)-first)%l.element)throw HostCallFault("STL ctype 掃描範圍無效");
            while(first<a(2)){const bool matches=(character_mask(static_cast<char16_t>(memory.load(GuestAddress{first},l.character_bits())))&a(0))!=0;if(matches==(operation=="CtypeScanIs"))break;first+=l.element;}return done(first);
        }
        const bool lower=operation.starts_with("CtypeLower"),upper=operation.starts_with("CtypeUpper"),range=operation.ends_with("Range");
        const bool widen=operation.starts_with("CtypeWiden"),narrow=operation.starts_with("CtypeNarrow");
        const auto locale=s.facet_locales.find(object);auto category=locale==s.facet_locales.end()?CrtLocaleCategory{}:locale->second.category;
        // 客體內聯建構的衍生 ctype 不在表內，語系名稱保存在它自己的 _Ctypevec。
        if(locale==s.facet_locales.end() && l.modern)if(const auto locale_name=ptr(object+(x86_?20U:40U)))if(const auto named=windows_crt_locale(read_utf16(memory,GuestAddress{locale_name})))category=*named;
        const auto convert=[&](std::uint32_t c,std::uint32_t fallback=0)->std::uint32_t{
            if(l.element==1){if(lower || upper){const auto& table=windows_crt_character_table(category);return (upper?table.upper:table.lower)[c&255U];}return c&255U;}
            if(lower || upper)return windows_crt_map_character(static_cast<char16_t>(c),category,upper);
            if(widen){const std::array<std::uint8_t,1> input{static_cast<std::uint8_t>(c)};const auto character=windows_crt_decode(input,category.codepage);
                return character.status==CrtDecodedCharacter::Status::Complete && character.character<=65535?character.character:65535U;}
            if(narrow){const char16_t character=static_cast<char16_t>(c);const auto bytes=windows_crt_encode(std::u16string_view(&character,1),category.codepage);
                return bytes && bytes->size()==1?bytes->front():fallback&255U;}
            return c;
        };
        if(!range){const auto c=static_cast<std::uint32_t>(a(0))&((1U<<((widen?1:l.element)*8))-1U);return done(convert(c,static_cast<std::uint32_t>(a(1))));}
        const auto first=a(0),last=a(1);if(last<first || (last-first)%(widen?1:l.element))throw HostCallFault("STL ctype 範圍無效");
        if(lower || upper){memory.check(GuestAddress{first},static_cast<std::size_t>(last-first),Access::Read);memory.check(GuestAddress{first},static_cast<std::size_t>(last-first),Access::Write);
            for(auto at=first;at<last;at+=l.element)memory.store(GuestAddress{at},l.character_bits(),convert(static_cast<std::uint32_t>(memory.load(GuestAddress{at},l.character_bits()))));}
        else {const auto target=a(widen?2:3);const auto count=(last-first)/(widen?1:l.element);
            memory.check(GuestAddress{first},static_cast<std::size_t>(last-first),Access::Read);memory.check(GuestAddress{target},static_cast<std::size_t>(count*(widen?l.element:1)),Access::Write);
            for(std::uint64_t i=0;i<count;++i){const auto c=memory.load(GuestAddress{first+i*(widen?1:l.element)},widen?8:l.character_bits());memory.store(GuestAddress{target+i*(widen?l.element:1)},widen?l.character_bits():8,convert(static_cast<std::uint32_t>(c),static_cast<std::uint32_t>(a(2))));}}
        return done(last);
    }
    if(const auto standard=s.standard_buffers.find(object);standard!=s.standard_buffers.end()){
        if(method=="StreamOverflow"){
            if((a(0)&l.int_mask())==l.eof())return done(0);
            const std::array<std::uint8_t,1> bytes{static_cast<std::uint8_t>(a(0))};const auto written=write_standard_output(standard->second,bytes);
            return done(written.error || written.bytes!=1?l.eof():bytes[0]);
        }
        if(method=="StreamXsputn"){
            const auto count=a(1);if(count>INT64_MAX)return done(0,true);std::uint64_t sent=0;std::array<std::uint8_t,16384> bytes{};
            while(sent<count){const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(count-sent,bytes.size()));memory.read(GuestAddress{a(0)+sent},std::span(bytes).first(n));
                const auto written=write_standard_output(standard->second,std::span(bytes).first(n));sent+=written.bytes;if(written.error || !written.bytes)break;}return done(sent,true);
        }
    }
    const auto class_owner=name.substr(name.find('@')+1);
    const bool buffer=class_owner.starts_with("?$basic_streambuf@") || name.starts_with("??0?$basic_streambuf@") || name.starts_with("??1?$basic_streambuf@") || method.starts_with("Stream");
    // basic_streambuf(_Uninitialized)：只設定虛擬表，保留既有欄位。
    if(buffer && name.starts_with("??0") && name.find("W4_Uninitialized@")!=name.npos){put(object,buffer_table);return done(object);}
    if(buffer && name.starts_with("??0")){
        memory.write(GuestAddress{object},std::vector<std::uint8_t>(l.buffer_size));put(object,buffer_table);put(object+l.buffer_locale(),new_locale());
        for(unsigned side=0;side<2;++side){put(object+l.first_indirect(side),object+l.first(side));put(object+l.next_indirect(side),object+l.next(side));put(object+l.count_indirect(side),object+l.count(side));}
        if(l.modern)return done(object);
        const auto lock=allocate_heap(memory,x86_?24:40);put(object+l.p,lock);
        start_callbacks(state,memory,{{resolve("kernel32.dll","InitializeCriticalSection").value,{lock}}},Continuation::Return,object);return {};
    }
    if(buffer && (method=="_Lock" || method=="_Unlock" || method=="StreamLock" || method=="StreamUnlock")){
        if(l.modern)return done(0); // VC14 basic_streambuf 的基底契約；filebuf 以覆寫方法鎖定 FILE。
        start_callbacks(state,memory,{{resolve("kernel32.dll",method=="_Lock" || method=="StreamLock"?"EnterCriticalSection":"LeaveCriticalSection").value,{ptr(object+l.p)}}},Continuation::Return,0);return {};
    }
    if(method=="getloc"){
        const auto loc=ptr(object+(buffer?l.buffer_locale():l.locale));const auto imp=ptr(loc+l.locale_pointer());memory.check(GuestAddress{a(0)},l.locale_size(),Access::Write);
        if(!l.modern){put(a(0),imp);put_refs(imp,refs(imp)+1);return done(a(0));}
        Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="copy_locale";op.object=imp;op.output=a(0);
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
    const bool ios=name.find("basic_ios@")!=name.npos;
    const bool input_stream=name.find("basic_istream@")!=name.npos;
    const bool output_stream=name.find("basic_ostream@")!=name.npos;
    const bool io_stream=name.find("basic_iostream@")!=name.npos;
    // VC14 ios_base 狀態（goodbit 0、eofbit 1、failbit 2、badbit 4，例外遮罩緊接其後）與格式欄位的存取子。
    const bool const_method=name.find("@QEB")!=name.npos || name.find("@QBE")!=name.npos || name.find("@IEB")!=name.npos || name.find("@IBE")!=name.npos;
    // 靜態成員（_Ios_base_dtor 等）沒有 this，不讀取物件狀態。
    if(name.starts_with("??7ios_base@") || name.starts_with("??Bios_base@")){
        const bool failed=(memory.load(GuestAddress{object+l.state},32)&6U)!=0;return done(name[2]=='7'?failed:!failed);
    }
    if(l.modern && (ios || class_owner.starts_with("ios_base@")) && method=="copyfmt"){
        // basic_ios 先複製 tie 與 fill；其餘（含自我指定的略過）都是 ios_base::copyfmt 的語義。
        const auto source=a(0);
        if(ios){put(object+l.ios_size+l.p,ptr(source+l.ios_size+l.p));memory.store(GuestAddress{object+l.ios_size+2*l.p},l.character_bits(),memory.load(GuestAddress{source+l.ios_size+2*l.p},l.character_bits()));}
        if(source==object)return done(object);
        Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="ios_copyfmt";op.object=object;op.limit=source;
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
    if(l.modern && (ios || class_owner.starts_with("ios_base@")) && method=="imbue"){
        memory.check(GuestAddress{a(0)},l.locale_size(),Access::Write);
        Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="ios_imbue";op.object=object;op.output=a(0);op.count=a(1);op.limit=ptr(a(1)+l.locale_pointer());op.buffer=ios?ptr(object+l.ios_size):0;
        if(!op.limit)throw HostCallFault("STL imbue 的 locale 為空");
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
    if(class_owner.starts_with("ios_base@") && !stat){
        if(l.modern && (method=="iword" || method=="pword")){
            const auto node=msvcp_ios_array(memory,l,object,static_cast<std::uint32_t>(a(0)),[&](std::uint64_t size){return allocate_heap(memory,size);});
            if(!node){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
            return done(node+l.p+(method=="iword"?4U:8U));
        }
        if(l.modern && method=="register_callback"){
            // _Fnarray｛_Next, _Index, _Pfn｝：新節點在串列前端，事件依此順序送出。
            const auto node=allocate_heap(memory,3*l.p);if(!node){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
            const auto head=object+l.width+8+l.p;memory.fill(GuestAddress{node},0,3*l.p);put(node,ptr(head));memory.store(GuestAddress{node+l.p},32,a(1));put(node+2*l.p,a(0));put(head,node);return done(0);
        }
        const auto state_value=static_cast<std::uint32_t>(memory.load(GuestAddress{object+l.state},32));
        if(method=="good")return done(state_value==0);
        if(method=="eof")return done((state_value&1U)!=0);
        if(method=="fail")return done((state_value&6U)!=0);
        if(method=="bad")return done((state_value&4U)!=0);
        if(method=="rdstate")return done(state_value);
        const auto flags_value=static_cast<std::uint32_t>(memory.load(GuestAddress{object+l.flags},32));
        if(method=="flags"){if(!const_method)memory.store(GuestAddress{object+l.flags},32,a(0));return done(flags_value);}
        if(method=="setf"){
            const bool masked=name.find("HHH@Z")!=name.npos || name.find("HHH@Z")!=name.npos;
            const auto value=static_cast<std::uint32_t>(a(0));const auto mask=masked?static_cast<std::uint32_t>(a(1)):value;
            memory.store(GuestAddress{object+l.flags},32,(flags_value&~mask)|(value&mask));return done(flags_value);
        }
        if(method=="unsetf"){memory.store(GuestAddress{object+l.flags},32,flags_value&~static_cast<std::uint32_t>(a(0)));return done(0);}
        if(method=="width" || method=="precision"){
            // VC9 x86 的 streamsize 為 int（4 位元組欄位、單一引數）；其餘為 64 位元。
            const bool narrow=l.legacy && x86_;const auto at=object+(method=="width"?l.width:l.precision);const auto old=memory.load(GuestAddress{at},narrow?32:64);
            if(!const_method)memory.store(GuestAddress{at},narrow?32:64,narrow?call(0):x86_?(call(0)|(call(1)<<32)):a(0));return done(old,!narrow);
        }
        if(method=="exceptions"){
            const auto mask_at=object+l.state+4;if(const_method)return done(memory.load(GuestAddress{mask_at},32));
            // 設定遮罩後與 ios_base::clear(rdstate()) 相同：目前狀態落在遮罩內就拋出 ios_base::failure。
            const auto mask=static_cast<std::uint32_t>(a(0))&7U;memory.store(GuestAddress{mask_at},32,mask);
            if(state_value&mask){throw_msvc_exception(state,memory,service,MsvcExceptionKind::IosFailure,0,1,s.error_categories[2]);return {};}
            return done(0);
        }
    }
    if(ios && const_method && method=="fill")return done(memory.load(GuestAddress{object+l.ios_size+2*l.p},l.character_bits()));
    if(ios && const_method && method=="rdbuf")return done(ptr(object+l.ios_size));
    if(ios && const_method && method=="tie")return done(ptr(object+l.ios_size+l.p));
    if(ios && !const_method && method=="tie"){const auto at=object+l.ios_size+l.p;const auto old=ptr(at);put(at,a(0));return done(old);}
    if(ios && !const_method && method=="rdbuf"){
        // rdbuf(sb) 更換後等同 clear()：沒有 streambuf 時設定 badbit，並套用例外遮罩。
        const auto old=ptr(object+l.ios_size);put(object+l.ios_size,a(0));const auto value=a(0)?0U:4U;memory.store(GuestAddress{object+l.state},32,value);
        if(value&memory.load(GuestAddress{object+l.state+4},32)){throw_msvc_exception(state,memory,service,MsvcExceptionKind::IosFailure,0,1,s.error_categories[2]);return {};}
        return done(old);
    }
    if(buffer && !name.starts_with("??")){
        // get／put 區以間接指標保存（filebuf 可指向 FILE 欄位）：起點、目前位置與剩餘數量。
        const auto side_first=[&](bool put_side){return ptr(ptr(object+l.first_indirect(put_side)));};
        const auto side_next=[&](bool put_side){return ptr(ptr(object+l.next_indirect(put_side)));};
        const auto side_count=[&](bool put_side){return static_cast<std::int32_t>(memory.load(GuestAddress{ptr(object+l.count_indirect(put_side))},32));};
        const auto side_end=[&](bool put_side){const auto next=side_next(put_side);return next?next+static_cast<std::uint64_t>(static_cast<std::int64_t>(side_count(put_side))*l.element):0;};
        if(method=="eback")return done(side_first(false));
        if(method=="gptr")return done(side_next(false));
        if(method=="egptr")return done(side_end(false));
        if(method=="pbase")return done(side_first(true));
        if(method=="pptr")return done(side_next(true));
        if(method=="epptr")return done(side_end(true));
        if(method=="setp"){
            const bool three=name.find("00@Z")!=name.npos;const auto first=a(0),next=three?a(1):a(0),last=three?a(2):a(1);
            put(ptr(object+l.first_indirect(true)),first);put(ptr(object+l.next_indirect(true)),next);
            memory.store(GuestAddress{ptr(object+l.count_indirect(true))},32,static_cast<std::uint32_t>(static_cast<std::int64_t>(last-next)/l.element));return done(0);
        }
        const auto get_available=[&]{return side_next(false)?side_count(false):0;};
        const auto character=[&](std::uint64_t at){return memory.load(GuestAddress{at},l.character_bits());};
        if(method=="in_avail"){if(const auto count=get_available();count>0)return done(static_cast<std::uint64_t>(count),true);return x86_ && !l.legacy?virtual_call_wide(object,5,{}):virtual_call(object,5,{});}
        if(method=="sgetc"){if(get_available()>0)return done(character(side_next(false)));return virtual_call(object,6,{});}
        if(method=="sbumpc"){
            if(get_available()>0){const auto at=side_next(false);put(ptr(object+l.next_indirect()),at+l.element);
                const auto count_at=ptr(object+l.count_indirect());memory.store(GuestAddress{count_at},32,memory.load(GuestAddress{count_at},32)-1);return done(character(at));}
            return virtual_call(object,7,{});
        }
        if(method=="pubsync")return virtual_call(object,13,{});
        if(method=="swap"){
            // 與 MSVC 相同經 setg／setp 交換讀寫區（間接指標維持指向各自欄位），並交換 locale 指標。
            const auto other=a(0);
            if(other!=object){
                const auto area=[&](std::uint64_t self,bool put_side){
                    const auto next=ptr(ptr(self+l.next_indirect(put_side)));const auto count=static_cast<std::int32_t>(memory.load(GuestAddress{ptr(self+l.count_indirect(put_side))},32));
                    return std::array<std::uint64_t,3>{ptr(ptr(self+l.first_indirect(put_side))),next,next?next+static_cast<std::uint64_t>(static_cast<std::int64_t>(count)*l.element):0};};
                const auto assign=[&](std::uint64_t self,bool put_side,const std::array<std::uint64_t,3>& value){
                    put(ptr(self+l.first_indirect(put_side)),value[0]);put(ptr(self+l.next_indirect(put_side)),value[1]);
                    memory.store(GuestAddress{ptr(self+l.count_indirect(put_side))},32,static_cast<std::uint32_t>(static_cast<std::int64_t>(value[2]-value[1])/l.element));};
                for(const bool put_side:{true,false}){const auto mine=area(object,put_side),theirs=area(other,put_side);assign(object,put_side,theirs);assign(other,put_side,mine);}
                const auto locale_mine=ptr(object+l.buffer_locale());put(object+l.buffer_locale(),ptr(other+l.buffer_locale()));put(other+l.buffer_locale(),locale_mine);
            }
            return done(0);
        }
        // 基底預設虛擬函式：沒有後備來源，與 MSVC 相同回 eof／無效位置。
        if(method=="pbackfail" || method=="underflow" || method=="overflow")return done(l.eof());
        if(method=="seekoff" || method=="seekpos"){
            const auto output=a(0);memory.check(GuestAddress{output},24,Access::Write);memory.write(GuestAddress{output},std::vector<std::uint8_t>(24));
            memory.store(GuestAddress{output},64,UINT64_MAX);return done(output);
        }
        // sungetc／sputbackc：讀取區有前一個字元（sputbackc 另須相符）時退回一格，否則交給 pbackfail（第 4 格）。
        if(method=="sungetc" || method=="sputbackc"){
            const auto next=side_next(false),first=side_first(false);const auto wanted=static_cast<std::uint32_t>(a(0))&((1U<<(l.element*8))-1U);
            if(next && next>first && (method=="sungetc" || character(next-l.element)==wanted)){
                put(ptr(object+l.next_indirect()),next-l.element);const auto count_at=ptr(object+l.count_indirect());
                memory.store(GuestAddress{count_at},32,memory.load(GuestAddress{count_at},32)+1);return done(character(next-l.element));
            }
            return virtual_call(object,4,{method=="sungetc"?l.eof():wanted});
        }
        // stossc：丟棄一個字元；讀取區用完時呼叫 uflow（第 7 格），結果不回傳。
        if(method=="stossc"){
            if(get_available()>0){const auto at=side_next(false);put(ptr(object+l.next_indirect()),at+l.element);const auto count_at=ptr(object+l.count_indirect());
                memory.store(GuestAddress{count_at},32,memory.load(GuestAddress{count_at},32)-1);return done(0);}
            GuestCallback callback{ptr(ptr(object)+l.slot(7)*l.p),{object}};callback.x86_register_arguments=x86_?1:0;start_callbacks(state,memory,{std::move(callback)},Continuation::Return,0);return {};
        }
        // pubseekoff／pubseekpos／pubsetbuf 轉給虛擬 seekoff（10）、seekpos（11）、setbuf（12）：x86 VC10／VC14 的 64 位元 streamoff／streamsize 拆成兩個 DWORD，
        // fpos 以 6 個 DWORD 傳值（x64 以指標）；VC9 x86 的 streamoff／streamsize 為 32 位元。
        const bool split_wide=x86_ && !l.legacy;
        const auto push=[&](std::vector<std::uint64_t>& out,std::uint64_t value,bool wide){out.push_back(x86_?value&UINT32_MAX:value);if(wide)out.push_back(value>>32);};
        if(method=="pubseekoff"){std::vector<std::uint64_t> args{a(0)};push(args,a(1),split_wide);args.push_back(a(2));args.push_back(a(3));return virtual_call(object,10,std::move(args));}
        if(method=="pubseekpos"){std::vector<std::uint64_t> args{a(0)};if(x86_)for(unsigned i=1;i<=6;++i)args.push_back(a(i));else args.push_back(a(1));args.push_back(a(x86_?7:2));return virtual_call(object,11,std::move(args));}
        if(method=="pubsetbuf"){std::vector<std::uint64_t> args{a(0)};push(args,a(1),split_wide);return virtual_call(object,12,std::move(args));}
        // VC9 _Sgetn_s 轉給 _Xsgetn_s（VC9 虛擬表第 7 格，VC10 沒有對應格位）。
        if(method=="_Sgetn_s"){GuestCallback callback{ptr(ptr(object)+7*l.p),{object,a(0),a(1),a(2)}};callback.x86_register_arguments=x86_?1:0;start_callbacks(state,memory,{std::move(callback)},Continuation::ReturnCallback);return {};}
        if(method=="pubimbue"){
            // 先呼叫虛擬 imbue（第 14 格），再把 _Plocale 換成新 locale：舊 _Locimp 的參考轉給回傳值，新的加一（與 MSVC 的 locale 指定相同）。
            const auto output=a(0),source=a(1);memory.check(GuestAddress{output},l.locale_size(),Access::Write);
            const auto entry=ptr(ptr(object)+l.slot(14)*l.p);const auto self=object;const auto layout=l;
            return nested_continuation(state,memory,"pubimbue",[this,service,entry,self,output,source,layout,bits](GuestState& s2,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                GuestCallback callback{entry,{self,source}};callback.x86_register_arguments=x86_?1:0;(void)co_await service_callback::call_native(std::move(callback));
                const auto load=[&](std::uint64_t at){return m.load(GuestAddress{at},bits);};
                const auto holder=load(self+layout.buffer_locale()),previous=load(holder+layout.locale_pointer()),fresh=load(source+layout.locale_pointer());
                m.store(GuestAddress{fresh+layout.p},layout.reference_bits(),m.load(GuestAddress{fresh+layout.p},layout.reference_bits())+1);m.store(GuestAddress{holder+layout.locale_pointer()},bits,fresh);
                m.fill(GuestAddress{output},0,layout.locale_size());m.store(GuestAddress{output+layout.locale_pointer()},bits,previous);
                const WindowsCall result(s2,m,service);result.return_to(s2,output);co_return std::nullopt;
            });
        }
    }
    if(input_stream && const_method && method=="gcount")return done(memory.load(GuestAddress{object+l.count_offset()},l.stream_bits()),!l.legacy);
    if(method=="swap" && (io_stream || input_stream || output_stream || ios || class_owner.starts_with("ios_base@"))){
        // 交換 ios_base（狀態、例外遮罩、格式、精度、寬度、iword／pword、回呼、locale）；basic_ios 以上另交換 tie 與 fill（rdbuf 不交換），
        // istream／iostream 再交換 gcount。串流經 vbptr 取得虛擬基底 basic_ios；basic_ios／ios_base 直接作用於物件本身。
        const auto other=a(0);
        if(other!=object){
            const bool streams=io_stream || input_stream || output_stream;
            const auto base=[&](std::uint64_t stream){return streams?stream+static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{ptr(stream)+4},32)))):stream;};
            const auto left=base(object),right=base(other);
            const auto exchange=[&](std::uint64_t first,std::uint64_t second,std::uint8_t field_bits){const auto value=memory.load(GuestAddress{first},field_bits);memory.store(GuestAddress{first},field_bits,memory.load(GuestAddress{second},field_bits));memory.store(GuestAddress{second},field_bits,value);};
            for(const auto& [offset,field_bits]:std::array<std::pair<unsigned,std::uint8_t>,7>{{{l.state,64},{l.flags,32},{l.precision,64},{l.width,64},{l.width+8,bits},{l.width+8+l.p,bits},{l.locale,bits}}})
                exchange(left+offset,right+offset,field_bits);
            if(!class_owner.starts_with("ios_base@")){exchange(left+l.ios_size+l.p,right+l.ios_size+l.p,bits);exchange(left+l.ios_size+2*l.p,right+l.ios_size+2*l.p,l.character_bits());}
            if(io_stream || input_stream)exchange(object+l.count_offset(),other+l.count_offset(),l.stream_bits());
        }
        return done(0);
    }
    if(ios && method=="narrow"){
        // 與 widen 相同交給 locale 的 ctype（衍生 facet 的覆寫一併生效）；沒有 facet 時採 C locale：寬字元超出 0～255 回預設字元。
        const auto c=a(0)&(l.element==1?255U:65535U),fallback=a(1)&255U;
        if(l.modern){const auto locale=ptr(object+l.locale),imp=locale?ptr(locale+l.locale_pointer()):0;const auto id=l.element==1?1U:6U;
            if(imp && ptr(imp+3*l.p)>id){const auto facet=ptr(ptr(imp+2*l.p)+id*l.p);if(facet)return virtual_call(facet,l.element==1?10U:14U,{c,fallback});}}
        return done(l.element==1 || c<256?c&255U:fallback);
    }
    if(input_stream && method=="isfx")return done(0); // MSVC 的 isfx 沒有收尾工作。
    if(name.starts_with("?sync_with_stdio@ios_base@")){const bool previous=s.sync_with_stdio;s.sync_with_stdio=(a(0)&255U)!=0;return done(previous);}
    if(class_owner.starts_with("Init@ios_base@") && name.starts_with("??4"))return done(object);
    if(class_owner.starts_with("ios_base@") && method=="_Findarr"){
        // 與 iword／pword 共用節點搜尋與配置；配置失敗拋 bad_alloc。
        const auto node=msvcp_ios_array(memory,l,object,static_cast<std::uint32_t>(a(0)),[&](std::uint64_t size){return allocate_heap(memory,size);});
        if(!node){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
        return done(node);
    }
    if(ios && method=="fill"){const auto at=object+l.ios_size+2*l.p;const auto old=memory.load(GuestAddress{at},l.character_bits());memory.store(GuestAddress{at},l.character_bits(),a(0));return done(old);}
    if(ios && method=="widen"){
        if(l.modern){const auto locale=ptr(object+l.locale),imp=locale?ptr(locale+l.locale_pointer()):0;const auto id=l.element==1?1U:6U;
            if(imp && ptr(imp+3*l.p)>id){const auto facet=ptr(ptr(imp+2*l.p)+id*l.p);if(facet)return virtual_call(facet,l.element==1?8U:12U,{a(0)&255U});}}
        return done(a(0)&(l.element==1?255U:65535U));
    }
    // basic_ios::init 與 basic_ios／串流建構共用：預設格式、fill、locale 參考與 streambuf；沒有 streambuf 時 badbit。
    const auto init_ios=[&](std::uint64_t base,std::uint64_t stream_buffer,bool standard){
        memory.store(GuestAddress{base+l.flags},32,0x201);memory.store(GuestAddress{base+l.precision},64,6);
        put(base+l.locale,new_locale());put(base+l.ios_size,stream_buffer);memory.store(GuestAddress{base+l.ios_size+2*l.p},l.character_bits(),' ');
        if(standard)put(base+l.p,1);
        if(!stream_buffer)memory.store(GuestAddress{base+l.state},32,4);
    };
    if(ios && method=="init"){memory.fill(GuestAddress{object+l.p},0,l.ios_size+2*l.p);init_ios(object,a(0),static_cast<std::uint8_t>(a(1))!=0);return done(0);}
    if(ios && name.starts_with("??0")){
        if(l.modern)memory.write(GuestAddress{object},std::vector<std::uint8_t>(l.ios_size+3*l.p));put(object,ios_table);s.ios_objects[object]={object,(l.ios_size+3*l.p+7U)&~7U};
        if(!name.ends_with("@XZ"))init_ios(object,a(0),name.ends_with("_N@Z") && static_cast<std::uint8_t>(a(1))!=0);
        return done(object);
    }
    if((input_stream || io_stream || output_stream) && name.starts_with("??0")){
        const auto iostream_vb=l.modern?m.iostream_vb14:s.iostream_vb,istream_vb=l.modern?m.istream_vb14:s.istream_vb,ostream_vb=l.modern?m.ostream_vb14:s.ostream_vb;
        if(static_cast<std::uint32_t>(a(io_stream?1:2))){put(object,io_stream?iostream_vb:output_stream?ostream_vb:istream_vb);if(io_stream)put(object+16,iostream_vb+8);}
        const auto vb=ptr(object);const auto base=object+static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{vb+4},32))));
        s.ios_objects[base]={object,base-object+((l.ios_size+3*l.p+7U)&~7U)};
        memory.write(GuestAddress{base},std::vector<std::uint8_t>(l.ios_size+3*l.p));put(base,l.modern?(io_stream?tables.iostream:output_stream?tables.ostream:tables.istream):ios_table);
        if(l.modern){const auto expected=io_stream?l.iostream_base():output_stream?l.ostream_base():l.istream_base();memory.store(GuestAddress{base-4},32,static_cast<std::uint32_t>(base-object-expected));}
        init_ios(base,a(0),!io_stream && static_cast<std::uint8_t>(a(1))!=0);
        if(!output_stream)memory.store(GuestAddress{object+l.count_offset()},l.stream_bits(),0);return done(object);
    }
    if(method=="clear" || method=="setstate"){
        auto value=static_cast<std::uint32_t>(a(0));if(method=="setstate")value|=static_cast<std::uint32_t>(memory.load(GuestAddress{object+l.state},32));
        if(!ptr(object+l.ios_size))value|=4;memory.store(GuestAddress{object+l.state},32,value);
        if(value&memory.load(GuestAddress{object+l.state+4},32)){
            if(l.modern && static_cast<std::uint8_t>(a(1))){const auto active=std::find_if(exceptions_.stack.rbegin(),exceptions_.stack.rend(),[](const auto& item){return item.msvc.catching;});if(active==exceptions_.stack.rend())return 3;
                const auto record=msvc_detail::exception_record(memory,active->record,x86_);auto caller=state;call.return_to(caller,0);begin_exception(caller,memory,record.code,record.flags,record.parameters);state=caller;return {};}
            throw_msvc_exception(state,memory,service,MsvcExceptionKind::IosFailure,0,1,s.error_categories[2]);return {};
        }return done(0);
    }
    const auto destroy_ios_object=[&](std::uint64_t self)->std::optional<std::uint32_t>{
        if(l.modern)put(self,m.ios_base_table14);
        if(l.modern?memory.load(GuestAddress{self+l.p},32):ptr(self+l.p))return done(0);
        Operation op;op.service=service;op.modern=l.modern;op.element=l.element;op.kind="destroy_ios";op.object=self;op.objects.push_back(self);
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    };
    if((input_stream || io_stream || output_stream) && name.starts_with("??_D")){
        // 虛基底解構：this 為完整物件（vbptr 所在）。先做 ??1 的 vptr／vtordisp 收尾，再解構 virtual base 的 basic_ios。
        const auto offset=static_cast<std::int32_t>(memory.load(GuestAddress{ptr(object)+4},32));const auto base=object+static_cast<std::uint64_t>(static_cast<std::int64_t>(offset));
        if(l.modern){const auto expected=io_stream?l.iostream_base():output_stream?l.ostream_base():l.istream_base();
            put(base,io_stream?tables.iostream:output_stream?tables.ostream:tables.istream);memory.store(GuestAddress{base-4},32,static_cast<std::uint32_t>(static_cast<std::int64_t>(offset)-expected));}
        return destroy_ios_object(base);
    }
    if((input_stream || io_stream || output_stream) && name.starts_with("??1")){
        if(l.modern){const auto expected=io_stream?l.iostream_base():output_stream?l.ostream_base():l.istream_base();const auto root=object-expected;
            const auto offset=static_cast<std::int32_t>(memory.load(GuestAddress{ptr(root)+4},32));const auto base=root+static_cast<std::uint64_t>(static_cast<std::int64_t>(offset));
            put(base,io_stream?tables.iostream:output_stream?tables.ostream:tables.istream);memory.store(GuestAddress{base-4},32,static_cast<std::uint32_t>(static_cast<std::int64_t>(offset)-expected));
        }return done(0); // 原生解構器會更新 vptr／vtordisp；virtual base 的解構另行執行。
    }
    if((ios && name.starts_with("??1")) || method=="_Ios_base_dtor")return destroy_ios_object(stat?a(0):object);
    if(method=="IosDelete"){
        const auto found=s.ios_objects.find(object);if(found==s.ios_objects.end())throw HostCallFault("VC10 ios 物件生命週期無效");
        const auto [owner,size]=found->second;const auto offset=object-owner;
        Operation op;op.service=service;op.modern=l.modern;op.element=l.element;op.kind="destroy_ios";op.object=owner;op.output=a(0);op.final_result=owner;
        if(a(0)&2U){const auto count=ptr(owner-l.p);if(count>1048576)throw HostCallFault("STL ios 陣列長度無效");for(auto i=count;i>0;--i)op.objects.push_back(owner+(i-1)*size+offset);}
        else op.objects.push_back(object);
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
    if(method=="StreamOverflow" || method=="StreamPbackfail" || method=="StreamUnderflow")return done(l.eof());
    if(method=="StreamShowmany" || method=="StreamSync" || method=="StreamImbue" || (buffer && method=="imbue"))return done(0,true);
    if(method=="StreamSetbuf")return done(object);
    if(method=="StreamSeekpos" || method=="StreamSeekoff"){
        memory.write(GuestAddress{a(0)},std::vector<std::uint8_t>(24));memory.store(GuestAddress{a(0)},64,UINT64_MAX);return done(a(0));
    }
    Operation op;op.service=service;op.object=object;op.buffer=object;op.kind=std::string(method);op.modern=l.modern;op.element=l.element;
    if(buffer && (name.starts_with("??1") || method=="StreamDelete")){
        put(object,buffer_table);
        op.kind="destroy_buffer";op.output=method=="StreamDelete"?a(0):0;op.final_result=method=="StreamDelete"?object:0;
        if(op.output&2U){const auto count=ptr(object-l.p);if(count>1048576)throw HostCallFault("STL streambuf 陣列長度無效");for(auto i=count;i>0;--i)op.objects.push_back(object+(i-1)*l.buffer_size);}
        else op.objects.push_back(object);
    }else if(method=="sgetc" || method=="snextc" || method=="uflow" || method=="StreamUflow"){}
    else if(method=="sputc"){op.output=a(0)&((1U<<(l.element*8))-1U);}
    else if(method=="sputn" || method=="sgetn"){op.output=a(0);op.limit=a(1);}
    else if(method=="_Xsgetn_s" || method=="StreamXsgetnSafe"){
        // VC9 基底 _Xsgetn_s {緩衝區, 緩衝區大小, 數量}：讀取 min(大小, 數量) 個字元，其餘與 xsgetn 相同。
        op.kind="read";op.output=a(0);op.limit=std::min(a(1),a(2));if(op.limit>INT64_MAX)op.limit=0;
    }else if(method=="xsgetn" || method=="xsputn" || method=="StreamXsgetn" || method=="StreamXsputn"){
        op.kind=method=="xsputn" || method=="StreamXsputn"?"write":"read";op.output=a(0);op.limit=a(1);if(op.limit>INT64_MAX)op.limit=0;
    }else if(input_stream || output_stream){
        op.ios=object+static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{ptr(object)+4},32))));op.buffer=ptr(op.ios+l.ios_size);
        if((name.starts_with("??6") || name.starts_with("??5")) && name.find("P6A")!=name.npos){
            const bool ios_manipulator=name.find("ios_base@")!=name.npos || name.find("basic_ios@")!=name.npos;
            start_callbacks(state,memory,{{a(0),{ios_manipulator?op.ios:object}}},ios_manipulator?Continuation::Return:Continuation::ReturnCallback,object);return {};
        }
        if(output_stream){
            if(method=="_Osfx" || method=="osfx" || method=="flush")op.kind=method=="flush"?"ostream_flush":"ostream_suffix";
            // opfx：輸出 sentry 的前置（good 時 flush tie），回傳 good()。
            else if(method=="opfx")op.kind="prefix_output";
            else if(method=="write"){op.kind="ostream_write";op.output=a(0);op.limit=a(1);}
            else if(method=="put"){op.kind="ostream_put";op.output=a(0)&((1U<<(l.element*8))-1U);op.limit=1;}
            else if(name.starts_with("??6") && name.find("V?$basic_streambuf@")!=name.npos){op.kind="ostream_buffer";op.limit=a(0);}
            else if(name.starts_with("??6")){
                const auto locale=ptr(op.ios+l.locale),imp=locale?ptr(locale+l.locale_pointer()):0;const auto id=l.element==1?4U:7U;
                const auto num_put=l.modern && imp && ptr(imp+3*l.p)>id?ptr(ptr(imp+2*l.p)+id*l.p):0;
                if(num_put){
                    const bool single=name.ends_with("@M@Z"),floating=single || name.ends_with("@N@Z") || name.ends_with("@O@Z"),pointer=name.ends_with("@PEBX@Z") || name.ends_with("@PBX@Z");
                    const bool wide=name.ends_with("@_J@Z") || name.ends_with("@_K@Z") || pointer;
                    op.kind="ostream_number_facet";op.limit=num_put;op.count=a(0);op.final_wide=wide || floating;
                    if(floating){std::uint64_t raw=x86_?a(0):0;if(!x86_)for(unsigned i=0;i<(single?4U:8U);++i)raw|=std::uint64_t{state.xmm[1][i]}<<(i*8);
                        op.count=single?std::bit_cast<std::uint64_t>(static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw)))):raw;op.output=name.ends_with("@O@Z")?4:5;
                    }else if(pointer){op.output=3; // num_put 虛擬表依 MSVC 反向宣告順序：3 const void*、4 long double、5 double…10 bool。
                    }else if(name.ends_with("@_N@Z")){op.count=static_cast<std::uint8_t>(a(0))!=0;op.output=10;}
                    else if(wide)op.output=name.ends_with("@_J@Z")?7:6;
                    else {op.output=name.ends_with("@G@Z") || name.ends_with("@I@Z") || name.ends_with("@K@Z")?8:9;
                        if(name.ends_with("@F@Z")){const auto flags=memory.load(GuestAddress{op.ios+l.flags},32)&0xe00U;op.count=flags==0x400 || flags==0x800?static_cast<std::uint16_t>(a(0)):static_cast<std::uint32_t>(static_cast<std::int32_t>(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(a(0)))));}
                        else if(name.ends_with("@G@Z"))op.count=static_cast<std::uint16_t>(a(0));else op.count=static_cast<std::uint32_t>(a(0));}
                    op.scratch=allocate_heap(memory,32);if(!op.scratch){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
                    memory.fill(GuestAddress{op.scratch},0,32);put(op.scratch+16+l.p,op.buffer);
                    const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
                }
                op.kind="ostream_number";op.count=a(0);
                if(!x86_ && (name.ends_with("@M@Z") || name.ends_with("@N@Z") || name.ends_with("@O@Z"))){op.count=0;for(unsigned i=0;i<(name.ends_with("@M@Z")?4U:8U);++i)op.count|=std::uint64_t{state.xmm[1][i]}<<(i*8);}
            }else if(method=="seekp" && name.find("fpos")==name.npos){
                // seekp(off, way)：與 seekg 相同交給 pubseekoff，模式為 ios_base::out。
                op.kind="seekp_offset";op.output=a(0);op.limit=static_cast<std::uint32_t>(a(1));op.scratch=allocate_heap(memory,24);if(!op.scratch)throw std::bad_alloc{};
            }else if(method=="seekp" || method=="tellp"){
                op.kind=std::string(method);op.output=method=="tellp"?a(0):0;op.scratch=allocate_heap(memory,48);if(!op.scratch)throw std::bad_alloc{};
                if(method=="seekp"){std::array<std::uint8_t,24> position{};memory.read(GuestAddress{x86_?state.reg(Gpr::Rsp)+4:a(0)},position);memory.write(GuestAddress{op.scratch+24},position);}
            }else throw HostCallFault("未知 STL ostream 操作");
        }else if(method=="read"){
            op.kind="istream_read";op.output=a(0);op.limit=l.stream_bits()==32?static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(a(1)))):a(1);memory.store(GuestAddress{object+l.count_offset()},l.stream_bits(),0);
        }else if(method=="tellg"){
            op.kind="tellg";op.output=a(0);op.scratch=allocate_heap(memory,24);if(!op.scratch)throw std::bad_alloc{};
        }else if(method=="seekg" && (name.find("@_JH@Z")!=name.npos || (l.legacy && name.find("@JH@Z")!=name.npos))){
            op.kind="seekg_offset";op.output=a(0);op.limit=static_cast<std::uint32_t>(a(1));op.scratch=allocate_heap(memory,24);if(!op.scratch)throw std::bad_alloc{};
        }else if(method=="_Ipfx" || method=="ipfx"){op.kind="prefix";op.skip=!a(0);}
        else if(method=="unget"){
            // unget：與 putback 相同（先清 eofbit、gcount 0、noskip sentry），退回任何前一字元，不能退回時交給 pbackfail(eof)。
            op.kind="putback";op.output=UINT64_MAX;memory.store(GuestAddress{object+l.count_offset()},l.stream_bits(),0);
            const auto at=op.ios+l.state;memory.store(GuestAddress{at},32,memory.load(GuestAddress{at},32)&~1U);}
        else if(method=="sync")op.kind="istream_sync";
        else if(method=="readsome"){
            op.kind="readsome";op.output=a(0);op.limit=l.stream_bits()==32?static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(a(1)))):a(1);
            memory.store(GuestAddress{object+l.count_offset()},l.stream_bits(),0);
        }else if(method=="get" && (name.find("V12@PEA")!=name.npos || name.find("V12@PA")!=name.npos)){
            // get(char*, n[, delim])：與 getline 共用讀取迴圈，但分隔字元留在串流中、緩衝區滿不設 failbit。
            op.kind="getline";op.keep_delimiter=true;op.output=a(0);op.limit=l.stream_bits()==32?static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(a(1)))):a(1);if(op.limit>INT64_MAX)op.limit=0;
            const bool two=name.ends_with("_J@Z") || (l.legacy && x86_ && name.ends_with("H@Z"));if(!two)op.delimiter=a(2)&((1U<<(l.element*8))-1U);memory.store(GuestAddress{object+l.count_offset()},l.stream_bits(),0);}
        else if((method=="get" || name.starts_with("??5")) && name.find("basic_streambuf@")!=name.npos){
            // get(streambuf&[, delim])（noskip，分隔字元不取出、計入 gcount）與 operator>>(streambuf*)（略過空白、複製到結尾）：
            // 逐字元由 rdbuf 取出寫入目標 streambuf，目標寫入失敗時停止且不取出該字元；一個字元都沒複製設 failbit。
            op.kind="istream_buffer";op.limit=a(0);op.skip=name.starts_with("??5");
            if(!op.skip){op.keep_delimiter=true;if(!name.ends_with("@@Z"))op.delimiter=a(1)&((1U<<(l.element*8))-1U);memory.store(GuestAddress{object+l.count_offset()},l.stream_bits(),0);}
            else op.delimiter=UINT64_MAX;
        }
        else if(method=="get" && (name.ends_with("XZ") || name.ends_with("@AEAD@Z") || name.ends_with("@AAD@Z") || name.ends_with("@AEA_W@Z") || name.ends_with("@AA_W@Z") || name.ends_with("@AEAG@Z") || name.ends_with("@AAG@Z"))){
            // get() 回 int_type；get(char&) 成功時寫入字元並回 *this。皆以 noskip sentry 讀一個字元並設定 gcount。
            op.kind="istream_get";op.output=name.ends_with("XZ")?0:a(0);memory.store(GuestAddress{object+l.count_offset()},l.stream_bits(),0);}
        else if(method=="putback"){op.kind="putback";op.output=a(0)&((1U<<(l.element*8))-1U);memory.store(GuestAddress{object+l.count_offset()},l.stream_bits(),0);
            // C++11 起 putback 先清除 eofbit，再建立 noskip sentry。
            const auto at=op.ios+l.state;memory.store(GuestAddress{at},32,memory.load(GuestAddress{at},32)&~1U);}
        else if(name.starts_with("??5")){op.kind="number";op.output=a(0);op.skip=true;}
        else if(method=="peek" || method=="ignore"){
            memory.store(GuestAddress{object+l.count_offset()},l.stream_bits(),0);
            if(method=="ignore"){op.limit=a(0);if(op.limit>INT64_MAX)op.limit=0;op.output=a(1)&l.int_mask();}
        }
        else if(method=="getline"){op.output=a(0);op.limit=l.stream_bits()==32?static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(a(1)))):a(1);if(op.limit>INT64_MAX)op.limit=0;
            // 兩參數版沒有分隔字元：VC10／VC14 以 streamsize（_J）結尾，VC9 x86 的 streamsize 為 int（H）。
            const bool two=name.ends_with("_J@Z") || (l.legacy && x86_ && name.ends_with("H@Z"));if(!two)op.delimiter=a(2)&((1U<<(l.element*8))-1U);memory.store(GuestAddress{object+l.count_offset()},l.stream_bits(),0);}
        else if(method=="seekg"){
            op.output=x86_?state.reg(Gpr::Rsp)+4:a(0);op.scratch=allocate_heap(memory,48);
            std::array<std::uint8_t,24> position{};memory.read(GuestAddress{op.output},position);memory.write(GuestAddress{op.scratch+24},position);
        }else throw HostCallFault("未知 VC10 istream 操作");
    }else throw HostCallFault("未知 VC10 STL 操作");
    const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
}

std::optional<std::uint32_t> WindowsServices::resume_msvcp100(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t value){
    auto& s=*msvcp100_;auto& op=s.operations.at(token);const Layout l(x86_,op.modern,op.element,op.service.family=="msvcp90.dll" || op.service.family=="internal-msvcp90",msvcp_detail::vc12(op.service));const auto bits=static_cast<std::uint8_t>(l.p*8);
    auto& m=s.runtimes[l.variant()];const auto* const modern_dll=msvcp_detail::modern_dll(l.variant());
    const WindowsCall call(state,memory,op.service);
    const auto ptr=[&](std::uint64_t at){return memory.load(GuestAddress{at},bits);};
    const auto put=[&](std::uint64_t at,std::uint64_t v){memory.store(GuestAddress{at},bits,v);};
    const auto finish=[&](std::uint64_t result,bool wide=false)->std::optional<std::uint32_t>{
        // 所有輸出 sentry 共用收尾；_Osfx 本身沒有 sentry，也不重複鎖定或 flush tie。
        if(!op.suffix_done && op.ios && (op.kind.starts_with("ostream_") || op.kind=="seekp" || op.kind=="tellp" || op.kind=="seekp_offset")){
            op.suffix_done=true;
            const bool unwinding=op.kind!="ostream_suffix" && std::any_of(exceptions_.stack.begin(),exceptions_.stack.end(),[](const ExceptionDispatch& item){return item.code==0xe06d7363U && !item.msvc.entered_catch;});
            if(op.buffer && !op.throw_failure && !op.rethrow_exception && !op.allocation_failure && !unwinding &&
               !memory.load(GuestAddress{op.ios+l.state},32) && (memory.load(GuestAddress{op.ios+l.flags},32)&2U)){
                op.completed=true;op.final_result=result;op.final_wide=wide;op.callback=52;
                GuestCallback cb{ptr(ptr(op.buffer)+13*l.p),{op.buffer}};cb.x86_register_arguments=x86_?1:0;cb.exception_policy=GuestExceptionPolicy::CaptureCpp;
                start_callbacks(state,memory,{std::move(cb)},Continuation::Msvcp100Operation,token);return {};
            }
        }
        if(op.numeric_held){
            const auto facet=op.numeric_facets[op.numeric_held-1];op.completed=true;op.final_result=result;op.final_wide=wide;
            GuestCallback cb{resolve("internal-msvcp140",x86_?"DFacetRetire32":"DFacetRetire64").value,{facet}};cb.exception_policy=GuestExceptionPolicy::NoexceptCpp;
            start_callbacks(state,memory,{std::move(cb)},Continuation::Msvcp100Operation,token);--op.numeric_held;return {};
        }
        if(op.locked){
            op.locked=false;op.completed=true;op.final_result=result;op.final_wide=wide;
            GuestCallback cb{l.legacy?resolve("kernel32.dll","LeaveCriticalSection").value:ptr(ptr(op.buffer)+2*l.p),{l.legacy?ptr(op.buffer+l.p):op.buffer}};cb.x86_register_arguments=x86_ && !l.legacy?1:0;cb.exception_policy=GuestExceptionPolicy::NoexceptCpp;
            start_callbacks(state,memory,{std::move(cb)},Continuation::Msvcp100Operation,token);return {};
        }
        if(op.caught_exception_base!=SIZE_MAX){
            if(op.retire_cursor==SIZE_MAX)op.retire_cursor=exceptions_.stack.size();
            if(op.retire_callback_pending){auto& caught=exceptions_.stack.at(op.retire_cursor);if(caught.msvc_owned_object){(void)free_heap(memory,caught.msvc_owned_object,state);caught.msvc_owned_object=0;}op.retire_callback_pending=false;}
            while(op.retire_cursor>op.caught_exception_base){
                const auto index=--op.retire_cursor;auto& caught=exceptions_.stack.at(index);const auto record=msvc_detail::exception_record(memory,caught.record,x86_);
                caught.msvc.catching=false;caught.msvc.entered_catch=true;(void)msvc_exception_cells(state,memory,false,true);
                if(record.cpp()){
                    const auto object=record.parameters[1];
                    if(op.rethrow_exception && op.exception_record.cpp() && object==op.exception_record.parameters[1]){if(caught.msvc_owned_object){op.rethrow_owned_object=caught.msvc_owned_object;caught.msvc_owned_object=0;}continue;}
                    const auto retained=std::find_if(exceptions_.stack.begin(),exceptions_.stack.begin()+static_cast<std::ptrdiff_t>(op.caught_exception_base),[&](const auto& outer){return outer.msvc.catching && outer.code==0xe06d7363U && ptr(outer.record+(x86_?24U:40U))==object;});
                    if(retained!=exceptions_.stack.begin()+static_cast<std::ptrdiff_t>(op.caught_exception_base)){if(caught.msvc_owned_object){retained->msvc_owned_object=caught.msvc_owned_object;caught.msvc_owned_object=0;}continue;}
                    if(!op.retired_objects.insert(object).second)continue;const auto type=msvc_detail::exception_type(memory,record,x86_);std::uint64_t destructor=type.destroy,self=object;
                    if(!destructor && type.interface_handle()){self=ptr(object);if(self)destructor=ptr(ptr(self)+2*l.p);}
                    if(destructor){op.completed=true;op.final_result=result;op.final_wide=wide;op.retire_callback_pending=true;
                        GuestCallback cb{destructor,{self}};cb.x86_register_arguments=x86_?1:0;cb.exception_policy=GuestExceptionPolicy::NoexceptCpp;start_callbacks(state,memory,{std::move(cb)},Continuation::Msvcp100Operation,token);return {};}
                }
                if(caught.msvc_owned_object){(void)free_heap(memory,caught.msvc_owned_object,state);caught.msvc_owned_object=0;}
            }
            exceptions_.stack.resize(op.caught_exception_base);op.caught_exception_base=SIZE_MAX;(void)msvc_exception_cells(state,memory,false,true);
        }
        const auto rethrow=op.rethrow_exception;const auto record=op.exception_record;const auto owned_object=op.rethrow_owned_object;
        for(const auto address:op.numeric_allocations)(void)free_heap(memory,address,state);
        const auto scratch=op.scratch,locale_lock=op.locale_lock_storage;const auto throw_failure=op.throw_failure,allocation_failure=op.allocation_failure;const auto service=op.service;s.operations.erase(token);
        if(scratch)(void)free_heap(memory,scratch,state);if(locale_lock)(void)free_heap(memory,locale_lock,state);
        if(rethrow){auto caller=state;call.return_to(caller,0);begin_exception(caller,memory,record.code,record.flags,record.parameters);exceptions_.stack.back().msvc_owned_object=owned_object;state=caller;return {};}
        if(allocation_failure){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
        if(throw_failure){throw_msvc_exception(state,memory,service,MsvcExceptionKind::IosFailure,0,1,s.error_categories[2]);return {};}
        call.return_to(state,result,wide);return {};
    };
    const auto callback=[&](std::uint64_t target,std::vector<std::uint64_t> args,unsigned next,bool member=true,std::uint32_t wide_args=0,bool wide_result=false)->std::optional<std::uint32_t>{
        GuestCallback cb{target,std::move(args)};cb.x86_register_arguments=x86_ && member?1:0;cb.wide_arguments=wide_args;cb.wide_result=wide_result;op.callback=next;
        if(op.kind=="exception_capture")cb.exception_policy=op.phase==1?GuestExceptionPolicy::CaptureAll:GuestExceptionPolicy::NoexceptCpp;
        else if(op.ios && op.kind!="num_put" && op.kind!="num_get" && next!=10)cb.exception_policy=GuestExceptionPolicy::CaptureCpp;
        else if(op.kind=="global_locale" || op.kind=="copy_locale" || op.kind=="retire_facet" || (op.kind=="num_put" && next==21) || op.kind=="exception_destroy" || op.kind=="exception_release" || op.kind=="destroy_ios" || op.kind=="destroy_buffer" || op.kind=="destroy_locale_imp" || op.kind=="clone_locale_imp" || op.kind=="add_locale_facet" || op.kind=="ios_init_flush")cb.exception_policy=GuestExceptionPolicy::NoexceptCpp;
        start_callbacks(state,memory,{std::move(cb)},Continuation::Msvcp100Operation,token);return {};
    };
    const auto virt=[&](unsigned slot,std::vector<std::uint64_t> args,unsigned next){
        if(l.legacy && (slot==1 || slot==2))return callback(resolve("kernel32.dll",slot==1?"EnterCriticalSection":"LeaveCriticalSection").value,{ptr(op.buffer+l.p)},next,false);
        args.insert(args.begin(),op.buffer);
        return callback(ptr(ptr(op.buffer)+l.slot(slot)*l.p),std::move(args),next,true,!l.legacy && (slot==8 || slot==9 || slot==10 || slot==12)?4U:0U,!l.legacy && (slot==5 || slot==8 || slot==9));};
    const auto count_address=[&](bool output){return ptr(op.buffer+l.count_indirect(output));};
    const auto position_address=[&](bool output){return ptr(op.buffer+l.next_indirect(output));};
    const auto available=[&](bool output){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{count_address(output)},32)));};
    const auto consume=[&](bool output){const auto pos=position_address(output),count=count_address(output);put(pos,ptr(pos)+l.element);memory.store(GuestAddress{count},32,memory.load(GuestAddress{count},32)-1);};
    const auto setstate=[&](unsigned flags){const auto address=op.ios+l.state,value=memory.load(GuestAddress{address},32)|flags;memory.store(GuestAddress{address},32,value);op.throw_failure|=(value&memory.load(GuestAddress{address+4},32))!=0;};
    const auto returned=op.callback;op.callback=0;
    // _Osfx 的同步失敗會設定 badbit，但其 noexcept 邊界吞掉例外遮罩觸發的 failure。
    if(returned==52 && static_cast<std::uint32_t>(value)==UINT32_MAX){const auto at=op.ios+l.state;memory.store(GuestAddress{at},32,memory.load(GuestAddress{at},32)|4U);}
    if(op.completed)return finish(op.final_result,op.final_wide);
#include "msvcp90_file_resume.inc"
    if(op.kind=="ostream_suffix")return finish(0);
    if(op.kind=="time_get"){
        // 前瞻來自 underflow（61）、取走來自 uflow（62）；緩衝區內有字元時直接讀取或推進，不呼叫虛擬函式。
        // last 不是結尾迭代器時兩者依 MSVC 的比較規則立即相等，視為沒有輸入。
        if(returned==61){const auto c=value&l.int_mask();op.time_look=c==l.eof()?msvcp_time_get::end_of_input:static_cast<int>(c);}
        if(returned==62){const auto c=value&l.int_mask();if(c==l.eof())op.time_look=msvcp_time_get::end_of_input;else {op.time_seen.push_back(static_cast<char16_t>(c));op.time_look=msvcp_time_get::unknown;}}
        for(;;){
            try{
                const auto result=msvcp_time_get::parse(op.numeric_text,op.time_seen,op.time_look);
                const std::array<std::pair<const std::optional<std::int32_t>*,unsigned>,8> fields{{{&result.fields.sec,0},{&result.fields.min,4},{&result.fields.hour,8},
                    {&result.fields.mday,12},{&result.fields.mon,16},{&result.fields.year,20},{&result.fields.wday,24},{&result.fields.yday,28}}};
                for(const auto& [field,offset]:fields)if(*field)memory.store(GuestAddress{op.time_tm+offset},32,static_cast<std::uint32_t>(**field));
                memory.store(GuestAddress{op.time_state},32,result.state);
                put(op.output,result.at_end && !op.limit?0:op.buffer);memory.store(GuestAddress{op.output+l.p},16,0);
                return finish(op.output);
            }catch(const msvcp_time_get::Need& need){
                if(need.kind==msvcp_time_get::Need::Kind::Peek){
                    if(!op.buffer || op.limit){op.time_look=msvcp_time_get::end_of_input;continue;}
                    if(available(false)>0){op.time_look=static_cast<int>(memory.load(GuestAddress{ptr(position_address(false))},l.character_bits()));continue;}
                    return virt(6,{},61);
                }
                if(available(false)>0){consume(false);op.time_seen.push_back(static_cast<char16_t>(op.time_look));op.time_look=msvcp_time_get::unknown;continue;}
                return virt(7,{},62);
            }
        }
    }
    if(op.kind=="shared_ptr_lock"){
        if(!s.shared_ptr_locked){s.shared_ptr_locked=true;return finish(0);}
        return callback(resolve("kernel32.dll","SwitchToThread").value,{},1,false);
    }
    if(op.kind=="ios_init_flush"){
        if(op.count<op.objects.size()){const auto stream=op.objects[static_cast<std::size_t>(op.count++)];
            return callback(resolve(modern_dll,x86_?"?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV12@XZ":"?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@XZ").value,{stream},1);}
        return finish(0);
    }
    const auto retire_facet=[&](std::uint64_t facet,unsigned next){return callback(resolve("internal-msvcp140",x86_?"DFacetRetire32":"DFacetRetire64").value,{facet},next,false);};
    // ios_base::_Callfns：event_callback 為 __cdecl，依 _Calls 串列順序（最新註冊在前）逐一呼叫；op.ios_node 指向下一個節點。
    const auto ios_event=[&](std::uint64_t base,unsigned event,unsigned next){
        const auto node=op.ios_node;op.ios_node=ptr(node);
        return callback(ptr(node+2*l.p),{event,base,memory.load(GuestAddress{node+l.p},32)},next,false);
    };
    // ios_base::_Tidy 的釋放部分：iword／pword 與回呼節點都是 CRT 配置。
    const auto free_ios_lists=[&](std::uint64_t base){
        for(const auto at:{l.width+8,l.width+8+l.p}){
            auto node=ptr(base+at);put(base+at,0);
            while(node){const auto next=ptr(node);(void)free_heap(memory,node,state);node=next;}
        }
    };
    if(op.kind=="ios_imbue"){
        // 舊 ios locale 的參考轉給回傳值；ios 與 streambuf 各持有新 locale 一份參考。op.count 為客體 locale 引數。
        const auto locale_cell=ptr(op.object+l.locale)+l.locale_pointer();
        if(!op.phase){op.phase=1;return callback(ptr(ptr(op.limit)+l.p),{op.limit},1);}
        if(op.phase==1){
            memory.fill(GuestAddress{op.output},0,l.locale_size());put(op.output+l.locale_pointer(),ptr(locale_cell));put(locale_cell,op.limit);
            op.ios_node=ptr(op.object+l.width+8+l.p);op.phase=2;
        }
        if(op.phase==2){
            if(op.ios_node)return ios_event(op.object,1,2);
            if(!op.buffer)return finish(op.output);
            op.phase=3;return virt(14,{op.count},3);
        }
        const auto buffer_locale=ptr(op.buffer+l.buffer_locale());if(!buffer_locale)return finish(op.output);
        const auto buffer_cell=buffer_locale+l.locale_pointer();
        if(op.phase==3){
            if(ptr(buffer_cell)==op.limit)return finish(op.output);
            op.phase=4;return callback(ptr(ptr(op.limit)+l.p),{op.limit},4);
        }
        if(op.phase==4){const auto previous=ptr(buffer_cell);put(buffer_cell,op.limit);op.phase=5;return retire_facet(previous,5);}
        return finish(op.output);
    }
    if(op.kind=="ios_copyfmt"){
        // ios_base::copyfmt：_Tidy（erase_event 後釋放）→ locale、格式、非零 iword／pword 與回呼 → copyfmt_event → exceptions()。不複製 _Mystate。
        const auto target=op.object,source=op.limit;const auto target_cell=ptr(target+l.locale)+l.locale_pointer();
        if(!op.phase){op.phase=1;op.ios_node=ptr(target+l.width+8+l.p);}
        if(op.phase==1){
            if(op.ios_node)return ios_event(target,0,1);
            free_ios_lists(target);
            const auto source_imp=ptr(ptr(source+l.locale)+l.locale_pointer());op.phase=4;
            if(ptr(target_cell)!=source_imp){op.phase=2;return callback(ptr(ptr(source_imp)+l.p),{source_imp},2);}
        }
        if(op.phase==2){const auto previous=ptr(target_cell);put(target_cell,ptr(ptr(source+l.locale)+l.locale_pointer()));op.phase=4;return retire_facet(previous,3);}
        if(op.phase==4){
            memory.store(GuestAddress{target+l.flags},32,memory.load(GuestAddress{source+l.flags},32));
            memory.store(GuestAddress{target+l.precision},64,memory.load(GuestAddress{source+l.precision},64));
            memory.store(GuestAddress{target+l.width},64,memory.load(GuestAddress{source+l.width},64));
            const auto allocate=[&](std::uint64_t size){return allocate_heap(memory,size);};
            for(auto node=ptr(source+l.width+8);node;node=ptr(node)){
                const auto word=memory.load(GuestAddress{node+l.p+4},32),pointer=ptr(node+l.p+8);if(!word && !pointer)continue;
                const auto copy=msvcp_ios_array(memory,l,target,static_cast<std::uint32_t>(memory.load(GuestAddress{node+l.p},32)),allocate);
                if(!copy){op.allocation_failure=true;break;}
                memory.store(GuestAddress{copy+l.p+4},32,word);put(copy+l.p+8,pointer);
            }
            for(auto node=ptr(source+l.width+8+l.p);node && !op.allocation_failure;node=ptr(node)){
                const auto copy=allocate(3*l.p);if(!copy){op.allocation_failure=true;break;}
                const auto head=target+l.width+8+l.p;memory.fill(GuestAddress{copy},0,3*l.p);put(copy,ptr(head));
                memory.store(GuestAddress{copy+l.p},32,memory.load(GuestAddress{node+l.p},32));put(copy+2*l.p,ptr(node+2*l.p));put(head,copy);
            }
            if(op.allocation_failure)return finish(target);
            op.ios_node=ptr(target+l.width+8+l.p);op.phase=5;
        }
        if(op.ios_node)return ios_event(target,2,5);
        const auto mask=memory.load(GuestAddress{source+l.state+4},32);memory.store(GuestAddress{target+l.state+4},32,mask);
        op.throw_failure=(memory.load(GuestAddress{target+l.state},32)&mask)!=0;
        return finish(target);
    }
    if(op.kind=="retire_facet"){
        if(!op.phase){op.phase=1;return callback(ptr(ptr(op.object)+2*l.p),{op.object},1);}
        if(returned==1 && value){op.phase=2;return callback(ptr(ptr(value)),{value,1},2);}return finish(0);
    }
    if(op.kind=="copy_locale"){
        if(!op.phase){op.phase=1;return callback(ptr(ptr(op.object)+l.p),{op.object},1);}
        memory.fill(GuestAddress{op.output},0,l.locale_size());put(op.output+l.locale_pointer(),op.object);return finish(op.output);
    }
    if(op.kind=="global_locale"){
        if(!op.phase){op.phase=1;return callback(resolve("msvcp140.dll",x86_?"??0_Lockit@std@@QAE@H@Z":"??0_Lockit@std@@QEAA@H@Z").value,{op.locale_lock_storage,0},40);}
        if(returned==40){op.locale_lock_owned=true;op.locale_source=m.global14;op.phase=2;return callback(ptr(ptr(op.locale_source)+l.p),{op.locale_source},41);}
        if(returned==41){op.locale_source_owned=true;
            if(op.object==op.locale_source)op.phase=5;
            else{op.phase=3;return callback(ptr(ptr(op.object)+l.p),{op.object},42);}
        }
        if(returned==42){op.locale_facet_owned=true;op.locale_pending=op.object;m.global14=op.object;op.locale_facet_owned=false;op.locale_pending=0;
            op.phase=4;return callback(resolve("internal-msvcp140",x86_?"DFacetRetire32":"DFacetRetire64").value,{op.locale_source},43,false);
        }
        if(op.phase==4){const auto label=ptr(op.object+4*l.p+8);const auto mask=memory.load(GuestAddress{op.object+4*l.p},32)&0x3fU;
            if(label){if(mask==0x3fU){if(!op.count++){return callback(resolve("crt","setlocale").value,{0,label},44,false);}}
                else while(op.count<5){const auto category=++op.count;if(mask&(1U<<(category-1)))return callback(resolve("crt","setlocale").value,{category,label},44,false);}
            }op.phase=5;
        }
        if(op.phase==5){memory.fill(GuestAddress{op.output},0,l.locale_size());put(op.output+l.locale_pointer(),op.locale_source);op.locale_source_owned=false;op.phase=6;
            return callback(resolve("msvcp140.dll",x86_?"??1_Lockit@std@@QAE@XZ":"??1_Lockit@std@@QEAA@XZ").value,{op.locale_lock_storage},45);
        }
        if(returned==45){op.locale_lock_owned=false;return finish(op.output);}
        throw HostCallFault("STL 全域 locale 延續狀態無效");
    }
    // num_put 與 operator>> 共用：呼叫 numpunct 虛擬函式，字串經 op.scratch 回傳後讀出並釋放。
    const auto punct=[&](unsigned slot,unsigned next){return callback(ptr(ptr(op.numeric_punct)+slot*l.p),slot<5?std::vector<std::uint64_t>{op.numeric_punct}:std::vector<std::uint64_t>{op.numeric_punct,op.scratch},next);};
    const auto read_string=[&](unsigned characters){
        const auto length=ptr(op.scratch+16),capacity=ptr(op.scratch+16+l.p);if(length>capacity || length>1048576)throw HostCallFault("num_put 字串回傳版面無效");
        const auto data=capacity>16U/characters-1?ptr(op.scratch):op.scratch;
        if(data!=op.scratch){if(capacity>=UINT64_MAX/characters)throw HostCallFault("num_put 字串容量溢位");const auto size=(capacity+1)*characters;
            std::uint64_t base=data;
            if(!allocations_.contains(base) && size>=4096){base=ptr(data-l.p);if(base>=data || data-base<l.p || data-base>31+l.p || !allocations_.contains(base))throw HostCallFault("num_put 字串配置無效");}
            if(!allocations_.contains(base))throw HostCallFault("num_put 字串沒有客體配置來源");op.numeric_allocations.push_back(base);
        }
        std::u16string result(static_cast<std::size_t>(length),0);
        for(std::size_t i=0;i<result.size();++i)result[i]=static_cast<char16_t>(memory.load(GuestAddress{data+i*characters},static_cast<std::uint8_t>(characters*8)));
        if(data!=op.scratch){const auto base=op.numeric_allocations.back();(void)free_heap(memory,base,state);op.numeric_allocations.pop_back();}
        memory.fill(GuestAddress{op.scratch},0,64);return result;
    };
    if(op.kind=="num_put"){
        try{
            if(returned==21 && op.numeric_hold_pending){++op.numeric_held;op.numeric_hold_pending=false;}
            if(op.numeric_held<op.numeric_facets.size()){
                const auto facet=op.numeric_facets[op.numeric_held];op.numeric_hold_pending=true;return callback(ptr(ptr(facet)+l.p),{facet},21);
            }
            const bool boolalpha=op.numeric_value.type==msvcp_detail::NumberType::Boolean && (op.numeric_format.flags&0x4000U);
            const bool floating=op.numeric_value.type==msvcp_detail::NumberType::Floating32 || op.numeric_value.type==msvcp_detail::NumberType::Floating64;
            if(!op.phase){op.phase=1;
                if(boolalpha)return punct(op.numeric_value.value?7U:6U,23);
                auto unpadded=op.numeric_format;unpadded.width=0;op.numeric_raw=msvcp_detail::format_number(op.numeric_value,unpadded);
                const auto count=op.numeric_raw.size();const auto narrow=allocate_heap(memory,std::max<std::size_t>(1,count));if(!narrow)throw std::bad_alloc();op.numeric_allocations.push_back(narrow);
                op.numeric_widened=allocate_heap(memory,std::max<std::size_t>(1,count*l.element));if(!op.numeric_widened)throw std::bad_alloc();op.numeric_allocations.push_back(op.numeric_widened);
                std::vector<std::uint8_t> bytes(count);for(std::size_t i=0;i<count;++i)bytes[i]=static_cast<std::uint8_t>(op.numeric_raw[i]);memory.write(GuestAddress{narrow},bytes);
                return callback(ptr(ptr(op.numeric_ctype)+(l.element==1?7U:11U)*l.p),{op.numeric_ctype,narrow,narrow+count,op.numeric_widened},22);
            }
            if(returned==22){op.numeric_text.resize(op.numeric_raw.size());for(std::size_t i=0;i<op.numeric_text.size();++i)op.numeric_text[i]=static_cast<char16_t>(memory.load(GuestAddress{op.numeric_widened+i*l.element},l.character_bits()));return punct(5,24);}
            if(returned==23){auto text=read_string(l.element);(op.numeric_value.value?op.numeric_punctuation.true_name:op.numeric_punctuation.false_name)=std::move(text);op.phase=3;}
            if(returned==24){const auto group=read_string(1);op.numeric_punctuation.grouping.clear();for(const auto byte:group)op.numeric_punctuation.grouping.push_back(static_cast<char>(byte));
                if(floating){
                    if(op.numeric_raw.find(u'.')!=op.numeric_raw.npos)return punct(3,25);
                    return punct(4,26);
                }
                if(!group.empty() && group.front()>0 && group.front()<127)return punct(4,26);op.phase=3;
            }
            if(returned==25){op.numeric_punctuation.decimal=static_cast<char16_t>(value);return punct(4,26);}
            if(returned==26){op.numeric_punctuation.thousands=static_cast<char16_t>(value);op.phase=3;}
            if(op.phase==3){auto format=op.numeric_format;const auto current=msvcp_number_format(memory,l,op.ios,static_cast<char16_t>(op.limit));
                format.width=current.width;format.flags=(format.flags&~0x1c0U)|(current.flags&0x1c0U);
                if(boolalpha)op.numeric_text=op.numeric_value.value?op.numeric_punctuation.true_name:op.numeric_punctuation.false_name;
                op.numeric_text=msvcp_detail::localize_number(std::move(op.numeric_text),op.numeric_raw,floating,boolalpha,format,op.numeric_punctuation);
                memory.store(GuestAddress{op.ios+l.width},64,0);op.phase=4;
            }
            if(op.phase==4){
                if(returned==31){if((value&l.int_mask())==l.eof())op.numeric_failed=true;++op.numeric_written;}
                while(op.numeric_written<op.numeric_text.size()){
                    if(!op.buffer){op.numeric_failed=true;op.numeric_written=op.numeric_text.size();break;}
                    const auto available_bytes=available(true);
                    if(available_bytes<=0)return virt(3,{op.numeric_text[op.numeric_written]},31);
                    const auto count=std::min<std::size_t>(static_cast<std::size_t>(available_bytes),op.numeric_text.size()-op.numeric_written);
                    std::vector<std::uint8_t> bytes(count*l.element);for(std::size_t i=0;i<count;++i){const auto character=op.numeric_text[op.numeric_written+i];bytes[i*l.element]=static_cast<std::uint8_t>(character);if(l.element==2)bytes[i*2+1]=static_cast<std::uint8_t>(character>>8);}
                    const auto position=position_address(true),at=ptr(position);memory.write(GuestAddress{at},bytes);put(position,at+count*l.element);
                    const auto counter=count_address(true);memory.store(GuestAddress{counter},32,memory.load(GuestAddress{counter},32)-count);op.numeric_written+=count;
                }
                memory.fill(GuestAddress{op.output},0,2*l.p);memory.store(GuestAddress{op.output},8,op.numeric_failed);put(op.output+l.p,op.buffer);return finish(op.output);
            }
            throw HostCallFault("num_put 延續狀態無效");
        }catch(const std::bad_alloc&){return callback(resolve("internal-msvcp140",x86_?"DThrowBadAlloc32":"DThrowBadAlloc64").value,{},32,false);}
    }
    if(op.kind=="destroy_ios"){
        if(returned==1 && value)return callback(ptr(ptr(value)),{value,1},2);
        while(op.count<op.objects.size()){
            const auto base=op.objects[static_cast<std::size_t>(op.count)];
            if(l.modern){
                if(!op.started){op.started=true;op.ios_node=ptr(base+l.width+8+l.p);}
                if(op.ios_node)return ios_event(base,0,4);
                free_ios_lists(base);op.started=false;
            }
            ++op.count;const auto loc=ptr(base+l.locale);if(l.modern)put(base,m.ios_base_table14);put(base+l.locale,0);s.ios_objects.erase(base);
            if(!loc)continue;const auto imp=ptr(loc+l.locale_pointer());if(!free_heap(memory,loc,state))throw HostCallFault("STL locale 配置無效");
            if(l.modern)return callback(ptr(ptr(imp)+2*l.p),{imp},1);
            const auto n=memory.load(GuestAddress{imp+l.p},l.reference_bits());if(n)memory.store(GuestAddress{imp+l.p},l.reference_bits(),n-1);
        }
        if(op.output&1U){if(!free_heap(memory,op.object-((op.output&2U)?l.p:0),state))throw HostCallFault("STL ios 配置無效");}return finish(op.final_result);
    }
    if(op.kind=="locinfo_construct" || op.kind=="locinfo_destroy"){
        const auto yarns=op.object+(x86_?4U:8U);const auto yarn_size=2*l.p;const auto old=yarns+4*yarn_size,new_name=old+yarn_size;
        const auto family="msvcp140.dll";
        // op.skip：靜態 _Locinfo_ctor／_Locinfo_dtor，鎖與 _Yarn 的生命週期都在客體內聯程式碼；名稱空白時恢復為系統預設 locale。
        const auto replace_name=[&](std::uint64_t at,std::uint64_t text){if(op.skip)if(const auto previous=ptr(at))(void)free_heap(memory,previous,state);put(at,text);};
        if(op.kind=="locinfo_construct"){
            if(!op.phase){op.phase=1;if(!op.skip)return callback(resolve(family,x86_?"??0_Lockit@std@@QAE@H@Z":"??0_Lockit@std@@QEAA@H@Z").value,{op.object,0},1);}
            if(op.phase==1){op.phase=2;return callback(resolve("crt","_wsetlocale").value,{0,0},2,false);}
            if(op.phase==2){replace_name(old,value?crt_duplicate_string(memory,value,true):0);op.phase=3;
                if(op.buffer)return callback(resolve("crt","setlocale").value,{0,op.buffer},3,false);value=0;}
            if(value)replace_name(new_name,crt_duplicate_string(memory,value));
            else{const auto star=allocate_heap(memory,2);if(star){memory.store(GuestAddress{star},16,'*');replace_name(new_name,star);}}
            return finish(op.object);
        }
        if(!op.phase){op.phase=1;return callback(resolve("crt","_wsetlocale").value,{0,ptr(old)?ptr(old):m.empty_wide14},1,false);}
        if(op.skip)return finish(0);
        if(op.phase==1){for(unsigned i=0;i<6;++i){const auto at=yarns+i*yarn_size,text=ptr(at);if(text)(void)free_heap(memory,text,state);put(at,0);}op.phase=2;
            return callback(resolve(family,x86_?"??1_Lockit@std@@QAE@XZ":"??1_Lockit@std@@QEAA@XZ").value,{op.object},2);}
        return finish(0);
    }
    if(op.kind=="exception_release"){
        auto& entry=s.exception_pointers.at(op.object);if(entry.immortal)return finish(0);
        if(!op.phase){const auto at=op.object+l.p,n=memory.load(GuestAddress{at},32);if(!n)throw HostCallFault("STL exception_ptr 已被釋放");memory.store(GuestAddress{at},32,n-1);if(n>1)return finish(0);op.phase=1;return callback(ptr(ptr(op.object)),{op.object},1);}
        const auto at=op.object+l.p+4,n=memory.load(GuestAddress{at},32);if(!n)throw HostCallFault("STL exception_ptr weak 計數無效");memory.store(GuestAddress{at},32,n-1);
        if(n==1){const auto control=op.object;s.exception_pointers.erase(control);(void)free_heap(memory,control,state);}return finish(0);
    }
    if(op.kind=="exception_destroy"){
        auto& entry=s.exception_pointers.at(op.object);
        if(!op.phase++){if(entry.destroy)return callback(entry.destroy,{entry.object},1);
            if(entry.interface_handle && entry.object){const auto unknown=ptr(entry.object);if(unknown)return callback(ptr(ptr(unknown)+2*l.p),{unknown},1);}}
        return finish(0);
    }
    if(op.kind=="exception_capture" || op.kind=="exception_rethrow"){
        if(!op.phase){op.phase=1;if(op.buffer){std::vector<std::uint64_t> args{op.object,op.count};if(op.skip)args.push_back(1);return callback(op.buffer,std::move(args),1);}
            if(op.started && op.object){const auto unknown=ptr(op.object);if(unknown)return callback(ptr(ptr(unknown)+l.p),{unknown},1);}}
        if(op.kind=="exception_rethrow"){
            const auto allocation=op.scratch;const auto record=op.exception_record;auto caller=state;call.return_to(caller,0);
            begin_exception(caller,memory,record.code,record.flags,record.parameters);exceptions_.stack.back().msvc_owned_object=allocation;s.operations.erase(token);state=caller;return {};
        }
        if(op.phase==1){const auto control=op.final_result;auto& entry=s.exception_pointers.at(control);entry.constructed=true;
            put(op.output,entry.record);put(op.output+l.p,control);op.scratch=0;op.phase=2;
            if(op.limit)return callback(resolve("internal-msvcp140",std::string("DExceptionControlRelease")+(x86_?"32":"64")).value,{op.limit},2);
        }
        return finish(0);
    }
    if(op.kind=="clone_locale_imp"){
        if(!op.phase){op.phase=1;return callback(resolve("msvcp140.dll",x86_?"??0_Lockit@std@@QAE@H@Z":"??0_Lockit@std@@QEAA@H@Z").value,{op.locale_lock_storage,0},40);}
        if(returned==40){
            op.locale_lock_owned=true;const auto source=op.locale_source;const auto count=ptr(source+3*l.p),facets=ptr(source+2*l.p);
            if(count>65536 || (count && !facets))throw HostCallFault("STL locale facet 清單無效");
            memory.check(GuestAddress{facets},static_cast<std::size_t>(count*l.p),Access::Read);
            std::uint64_t copy=0,array=0,label=0;
            try{
                copy=op.scratch?op.scratch:allocate_heap(memory,x86_?32:56);if(!copy)throw std::bad_alloc{};memory.move(GuestAddress{copy},GuestAddress{source},x86_?32:56);
                if(count){array=allocate_heap(memory,count*l.p);if(!array)throw std::bad_alloc{};memory.move(GuestAddress{array},GuestAddress{facets},static_cast<std::size_t>(count*l.p));}
                if(const auto name=ptr(source+4*l.p+8)){label=crt_duplicate_string(memory,name);if(!label)throw std::bad_alloc{};}
                put(copy,m.locimp_table14);memory.store(GuestAddress{copy+l.p},l.reference_bits(),1);put(copy+2*l.p,array);put(copy+4*l.p+8,label);
                s.owned_locale_imps.insert(copy);op.object=copy;op.output=array;op.limit=count;op.phase=2;
            }catch(const std::bad_alloc&){
                s.owned_locale_imps.erase(copy);if(label)(void)free_heap(memory,label,state);if(array)(void)free_heap(memory,array,state);if(copy && !op.scratch)(void)free_heap(memory,copy,state);
                op.allocation_failure=true;op.phase=3;
            }catch(...){if(label)(void)free_heap(memory,label,state);if(array)(void)free_heap(memory,array,state);if(copy && !op.scratch)(void)free_heap(memory,copy,state);throw;}
        }
        if(returned==1){++op.count;op.locale_pending=0;}
        if(op.phase==2){while(op.count<op.limit){const auto facet=ptr(op.output+op.count*l.p);if(facet){op.locale_pending=facet;return callback(ptr(ptr(facet)+l.p),{facet},1);}++op.count;}op.phase=3;}
        if(op.phase==3){op.phase=4;return callback(resolve("msvcp140.dll",x86_?"??1_Lockit@std@@QAE@XZ":"??1_Lockit@std@@QEAA@XZ").value,{op.locale_lock_storage},45);}
        if(returned==45){op.locale_lock_owned=false;return finish(op.object);}
        throw HostCallFault("STL locale clone 延續狀態無效");
    }
    if(op.kind=="add_locale_facet"){
        if(!op.phase){op.phase=1;return callback(resolve("msvcp140.dll",x86_?"??0_Lockit@std@@QAE@H@Z":"??0_Lockit@std@@QEAA@H@Z").value,{op.locale_lock_storage,0},40);}
        if(returned==40){
            op.locale_lock_owned=true;const auto count=ptr(op.object+3*l.p),array=ptr(op.object+2*l.p);
            if(count>65536 || (count && !array))throw HostCallFault("STL locale facet 清單無效");
            memory.check(GuestAddress{array},static_cast<std::size_t>(count*l.p),Access::Read);op.limit=op.output<count?ptr(array+op.output*l.p):0;
            try{
                if(op.output>=count){
                    op.locale_capacity=std::max<std::uint64_t>(op.output+1,40);op.scratch=allocate_heap(memory,op.locale_capacity*l.p);
                    if(!op.scratch)throw std::bad_alloc{};memory.fill(GuestAddress{op.scratch},0,static_cast<std::size_t>(op.locale_capacity*l.p));
                    if(count)memory.move(GuestAddress{op.scratch},GuestAddress{array},static_cast<std::size_t>(count*l.p));
                }
                op.phase=2;if(op.buffer){op.locale_pending=op.buffer;return callback(ptr(ptr(op.buffer)+l.p),{op.buffer},1);}
            }catch(const std::bad_alloc&){if(op.scratch)(void)free_heap(memory,op.scratch,state);op.scratch=0;op.allocation_failure=true;op.phase=4;}
        }
        if(returned==1)op.locale_facet_owned=true;
        if(op.phase==2){
            auto array=ptr(op.object+2*l.p);if(op.scratch){const auto previous=array;array=op.scratch;put(op.object+2*l.p,array);put(op.object+3*l.p,op.locale_capacity);op.scratch=0;if(previous && allocations_.contains(previous))(void)free_heap(memory,previous,state);}
            put(array+op.output*l.p,op.buffer);op.locale_facet_owned=false;op.locale_pending=0;op.phase=3;if(op.limit)return callback(ptr(ptr(op.limit)+2*l.p),{op.limit},2);
        }
        if(returned==2 && value){op.phase=4;return callback(ptr(ptr(value)),{value,1},3);}
        if(op.phase==3)op.phase=4;
        if(op.phase==4){op.phase=5;return callback(resolve("msvcp140.dll",x86_?"??1_Lockit@std@@QAE@XZ":"??1_Lockit@std@@QEAA@XZ").value,{op.locale_lock_storage},45);}
        if(returned==45){op.locale_lock_owned=false;return finish(0);}
        throw HostCallFault("STL locale facet 新增延續狀態無效");
    }
    if(op.kind=="destroy_locale_imp"){
        if(returned==1 && value)return callback(ptr(ptr(value)),{value,1},2);
        while(op.count<op.limit){const auto facet=ptr(op.output+op.count++*l.p);if(facet)return callback(ptr(ptr(facet)+2*l.p),{facet},1);}
        const auto owner=op.object,name=ptr(owner+4*l.p+8),array=op.output,flags=op.final_result;
        if(name && allocations_.contains(name))(void)free_heap(memory,name,state);if(array && allocations_.contains(array))(void)free_heap(memory,array,state);
        put(owner+2*l.p,0);put(owner+3*l.p,0);put(owner+4*l.p+8,0);s.owned_locale_imps.erase(owner);
        if(flags&1U)(void)free_heap(memory,owner,state);return finish(owner);
    }
    if(op.ios && (op.kind=="number" || op.kind=="getline" || op.kind=="peek" || op.kind=="istream_get" || op.kind=="putback" || op.kind=="ignore" || op.kind=="istream_read" || op.kind=="tellg" || op.kind=="seekg_offset" || op.kind=="seekg" || op.kind=="seekp" || op.kind=="tellp" || op.kind.starts_with("ostream_") ||
                   op.kind=="istream_sync" || op.kind=="readsome" || op.kind=="istream_buffer" || op.kind=="seekp_offset") && !op.lock_started && op.buffer){
        op.lock_started=true;return virt(1,{},10);
    }
    if(returned==10)op.locked=true;
    if(returned==11 && !l.modern && static_cast<std::uint32_t>(value)==UINT32_MAX){
        const auto at=op.tie_ios+l.state;memory.store(GuestAddress{at},32,memory.load(GuestAddress{at},32)|4U);
    }
    if(op.kind=="getline_delimiter"){
        if(op.limit)memory.store(GuestAddress{op.output+op.count*l.element},l.character_bits(),0);
        return finish(op.object);
    }
    if(op.kind=="destroy_buffer"){
        while(op.count<op.objects.size()){
            const auto current=op.objects[static_cast<std::size_t>(op.count)],lock=l.modern?0:ptr(current+l.p);
            if(!op.phase){op.phase=1;if(lock)return callback(resolve("kernel32.dll","DeleteCriticalSection").value,{lock},1,false);}
            if(op.phase==1){op.phase=2;const auto loc=ptr(current+l.buffer_locale());put(current+l.buffer_locale(),0);
                if(loc){const auto imp=ptr(loc+l.locale_pointer());if(!free_heap(memory,loc,state))throw HostCallFault("STL streambuf locale 配置無效");
                    if(l.modern){op.decref_pending=true;return callback(ptr(ptr(imp)+2*l.p),{imp},2);}
                    const auto n=memory.load(GuestAddress{imp+l.p},l.reference_bits());if(n)memory.store(GuestAddress{imp+l.p},l.reference_bits(),n-1);}
            }
            if(op.phase==2){op.phase=3;const auto pending=op.decref_pending;op.decref_pending=false;if(pending && returned==2 && value)return callback(ptr(ptr(value)),{value,1},3);}
            if(lock){if(!free_heap(memory,lock,state))throw HostCallFault("STL streambuf 鎖配置無效");put(current+l.p,0);}++op.count;op.phase=0;
        }
        if(op.output&1U){if(!free_heap(memory,op.object-((op.output&2U)?l.p:0),state))throw HostCallFault("STL streambuf 配置無效");}return finish(op.final_result);
    }
    if(op.kind=="sgetc")return returned?finish(value&l.int_mask()):available(false)>0?finish(memory.load(GuestAddress{ptr(position_address(false))},l.character_bits())):virt(6,{},1);
    if(op.kind=="sputc"){
        if(returned)return finish(value&l.int_mask());
        if(available(true)<=0)return virt(3,{op.output},1);
        memory.store(GuestAddress{ptr(position_address(true))},l.character_bits(),op.output);consume(true);return finish(op.output);
    }
    if(op.kind=="sputn" || op.kind=="sgetn")return returned?finish(value,!l.legacy):virt(op.kind=="sputn"?9:8,{op.output,op.limit},1);
    if(op.kind=="uflow" || op.kind=="StreamUflow"){
        if(returned && (value&l.int_mask())==l.eof())return finish(l.eof());
        if(returned){const auto c=memory.load(GuestAddress{ptr(position_address(false))},l.character_bits());consume(false);return finish(c);}
        return virt(6,{},1);
    }
    if(op.kind=="snextc"){
        if(returned==2)return finish(value&l.int_mask());
        if(returned==1 && (value&l.int_mask())==l.eof())return finish(l.eof());
        if(!returned){if(available(false)>0)consume(false);else return virt(7,{},1);}
        return available(false)>0?finish(memory.load(GuestAddress{ptr(position_address(false))},l.character_bits())):virt(6,{},2);
    }
    if(op.kind=="read" || op.kind=="write"){
        const bool output=op.kind=="write";
        if(returned){if((value&l.int_mask())==l.eof())return finish(op.count,true);if(!output)memory.store(GuestAddress{op.output+op.count*l.element},l.character_bits(),value);++op.count;}
        while(op.count<op.limit){
            if(available(output)>0){const auto position=ptr(position_address(output));const auto n=std::min<std::uint64_t>(op.limit-op.count,static_cast<unsigned>(available(output)));
                const auto bytes=n*l.element,other=op.output+op.count*l.element;
                memory.move(GuestAddress{output?position:other},GuestAddress{output?other:position},static_cast<std::size_t>(bytes));
                put(position_address(output),position+bytes);memory.store(GuestAddress{count_address(output)},32,static_cast<std::uint32_t>(available(output))-static_cast<std::uint32_t>(n));op.count+=n;
            }
            else return virt(output?3:7,output?std::vector<std::uint64_t>{memory.load(GuestAddress{op.output+op.count*l.element},l.character_bits())}:std::vector<std::uint64_t>{},1);
        }
        return finish(op.count,true);
    }
    const bool prefix=op.kind=="prefix",number=op.kind=="number" || op.kind=="num_get",line=op.kind=="getline";
    const auto stream_facet=[&](unsigned id){
        // locale 自身沒有此 facet 時依序退回全域與 classic，與 _Getfacet 相同。
        const auto locale=ptr(op.ios+l.locale);
        for(const auto imp:{locale?ptr(locale+l.locale_pointer()):0,m.global14,m.classic14})if(imp && ptr(imp+3*l.p)>id)if(const auto facet=ptr(ptr(imp+2*l.p)+id*l.p))return facet;
        return std::uint64_t{};
    };
    const auto is_space=[&](std::uint64_t c){
        // ctype<char>::is 查 facet 的遮罩表，客體自訂表（例如把逗號視為空白）也生效；寬字元依分類語義。
        if(l.modern && l.element==1){
            if(!op.space_table){const auto facet=stream_facet(1);op.space_table=facet?ptr(facet+2*l.p+(x86_?4U:8U)):0;if(!op.space_table)op.space_table=m.ctype_masks14;}
            return (memory.load(GuestAddress{op.space_table+(c&255U)*2},16)&8U)!=0;
        }
        return (character_mask(static_cast<char16_t>(c))&8U)!=0;
    };
    if(!op.started){
        op.started=true;
        if(op.kind=="seekg_offset" || op.kind=="seekg"){const auto at=op.ios+l.state;memory.store(GuestAddress{at},32,memory.load(GuestAddress{at},32)&~1U);}
        const auto initial_state=memory.load(GuestAddress{op.ios+l.state},32);
        const bool output_position=op.kind=="seekp" || op.kind=="tellp" || op.kind=="seekp_offset";
        if((output_position?(initial_state&6U):initial_state) || !op.buffer){
            if(op.kind=="prefix_output")return finish(0); // opfx 不改變狀態，只回傳 good()。
            if(output_position){
                if(op.kind=="tellp"){memory.fill(GuestAddress{op.output},0,24);memory.store(GuestAddress{op.output},64,UINT64_MAX);return finish(op.output);}
                return finish(op.object);
            }
            if(op.kind=="ostream_suffix")return finish(0);
            if(op.kind=="ostream_flush")return finish(op.object);
            // operator<<(basic_streambuf*)：sentry 失敗時沒有複製任何字元，與 MSVC 相同清 width 並設 failbit（來源為空設 badbit）。
            if(op.kind=="ostream_buffer"){memory.store(GuestAddress{op.ios+l.width},64,0);setstate(op.limit?2:4);return finish(op.object);}
            if(op.kind.starts_with("ostream_")){setstate(4);return finish(op.object);}
            if(op.kind=="getline" && op.limit)memory.store(GuestAddress{op.output},l.character_bits(),0);
            setstate(2);if(op.kind=="tellg"){memory.write(GuestAddress{op.output},std::array<std::uint8_t,24>{});memory.store(GuestAddress{op.output},64,UINT64_MAX);return finish(op.output);}
            // sync：sentry 失敗回 0，沒有 rdbuf 回 -1；readsome 回 gcount（0）。
            if(op.kind=="istream_sync")return finish(op.buffer?0:UINT32_MAX);
            if(op.kind=="readsome")return finish(0,!l.legacy);
            return finish(prefix?0:op.kind=="peek" || (op.kind=="istream_get" && !op.output)?l.eof():op.object);}
        op.skip=op.skip && (memory.load(GuestAddress{op.ios+l.flags},32)&1U);
        if(const auto tied=ptr(op.ios+l.ios_size+l.p);tied && tied!=op.object && !initial_state){
            op.tie_ios=tied+static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{ptr(tied)+4},32))));const auto tied_buffer=ptr(op.tie_ios+l.ios_size);
            if(l.modern){
                const std::string character=l.element==1?"D":"_W";
                const auto name="?flush@?$basic_ostream@"+character+"U?$char_traits@"+character+"@std@@@std@@"+(x86_?"QAEAAV":"QEAAAEAV")+"12@XZ";
                return callback(resolve(modern_dll,name).value,{tied},11);
            }
            if(tied_buffer && !memory.load(GuestAddress{op.tie_ios+l.state},32))return callback(ptr(ptr(tied_buffer)+l.slot(13)*l.p),{tied_buffer},11);
        }
    }
    if(op.kind=="seekg"){
        if(returned==1){if((l.stream_bits()==32?memory.load(GuestAddress{op.scratch},32)==UINT32_MAX:memory.load(GuestAddress{op.scratch},64)==UINT64_MAX) && !memory.load(GuestAddress{op.scratch+8},64))setstate(2);return finish(op.object);}
        if(memory.load(GuestAddress{op.ios+l.state},32)&6U)return finish(op.object);
        std::vector<std::uint64_t> args{op.scratch};
        if(x86_)for(unsigned i=0;i<6;++i)args.push_back(memory.load(GuestAddress{op.scratch+24+4*i},32));else args.push_back(op.scratch+24);
        args.push_back(1);return virt(11,std::move(args),1);
    }
    if(op.kind=="seekp" || op.kind=="tellp"){
        const bool query=op.kind=="tellp";
        if(returned==1){if(query){memory.move(GuestAddress{op.output},GuestAddress{op.scratch},24);return finish(op.output);}
            if((l.stream_bits()==32?static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{op.scratch},32)))):memory.load(GuestAddress{op.scratch},64))+memory.load(GuestAddress{op.scratch+8},64)==UINT64_MAX)setstate(2);return finish(op.object);}
        if(memory.load(GuestAddress{op.ios+l.state},32)&6U || !op.buffer){if(query){memory.write(GuestAddress{op.output},std::array<std::uint8_t,24>{});memory.store(GuestAddress{op.output},64,UINT64_MAX);}return finish(query?op.output:op.object);}
        if(query)return virt(10,{op.scratch,0,1,2},1);
        std::vector<std::uint64_t> args{op.scratch};if(x86_)for(unsigned i=0;i<6;++i)args.push_back(memory.load(GuestAddress{op.scratch+24+i*4},32));else args.push_back(op.scratch+24);
        args.push_back(2);return virt(11,std::move(args),1);
    }
    if(op.kind.starts_with("ostream_")){
        if(op.kind=="ostream_buffer"){
            // MSVC operator<<(basic_streambuf*)：先 sgetc，之後每次 snextc，逐字元交給 rdbuf()->sputc；來源讀到 eof 或寫入失敗即停。
            // 一個字元都沒複製設 failbit，寫入失敗加 badbit，結束時清 width。op.phase：20 取第一字元、21 取下一字元、22 寫出 op.output。
            const auto source=op.limit;
            const auto source_count=[&]{return ptr(source+l.count_indirect(false));};
            const auto source_position=[&]{return ptr(source+l.next_indirect(false));};
            const auto source_available=[&]{return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{source_count()},32)));};
            const auto source_character=[&]{return memory.load(GuestAddress{ptr(source_position())},l.character_bits());};
            const auto source_virtual=[&](unsigned slot,unsigned next){return callback(ptr(ptr(source)+slot*l.p),{source},next);};
            const auto complete=[&](unsigned flags){memory.store(GuestAddress{op.ios+l.width},64,0);if(flags)setstate(flags);return finish(op.object);};
            if(!source)return complete(4);
            if(!op.phase)op.phase=20;
            if(returned==31){op.output=value&l.int_mask();op.phase=22;}
            else if(returned==32){
                if((value&l.int_mask())==l.eof()){op.output=l.eof();op.phase=22;}
                else if(source_available()>0){op.output=source_character();op.phase=22;}
                else return source_virtual(6,31);
            }else if(returned==33){
                if((value&l.int_mask())==l.eof())return complete(op.count?4U:6U);
                op.count=1;op.phase=21;
            }
            for(;;){
                if(op.phase==20){
                    if(source_available()<=0)return source_virtual(6,31);
                    op.output=source_character();op.phase=22;
                }else if(op.phase==21){
                    if(source_available()<=0)return source_virtual(7,32);
                    const auto position=source_position(),count=source_count();put(position,ptr(position)+l.element);
                    memory.store(GuestAddress{count},32,memory.load(GuestAddress{count},32)-1);
                    if(source_available()<=0)return source_virtual(6,31);
                    op.output=source_character();op.phase=22;
                }else{
                    if(op.output==l.eof())return complete(op.count?0U:2U);
                    if(available(true)<=0)return virt(3,{op.output},33);
                    memory.store(GuestAddress{ptr(position_address(true))},l.character_bits(),op.output);consume(true);op.count=1;op.phase=21;
                }
            }
        }
        if(op.kind=="ostream_number"){
            try{const auto text=format_msvcp_number(op,memory,l);op.limit=text.size();op.scratch=allocate_heap(memory,std::max<std::uint64_t>(1,op.limit*l.element));
                if(!op.scratch)throw std::bad_alloc{};op.output=op.scratch;for(std::size_t i=0;i<text.size();++i)memory.store(GuestAddress{op.output+i*l.element},l.character_bits(),text[i]);op.kind="ostream_write";
            }catch(const std::bad_alloc&){return callback(resolve("internal-msvcp140",std::string("DThrowBadAlloc")+(x86_?"32":"64")).value,{},18,false);}
        }
        if(op.kind=="ostream_number_facet"){
            if(returned==17){if(memory.load(GuestAddress{op.scratch},8))setstate(4);return finish(op.object);}
            const auto fill=memory.load(GuestAddress{op.ios+l.ios_size+2*l.p},l.character_bits());std::vector<std::uint64_t> args;
            if(x86_)args={op.limit,op.scratch,0,op.buffer,op.ios,fill,op.count};else args={op.limit,op.scratch,op.scratch+16,op.ios,fill,op.count};
            return callback(ptr(ptr(op.limit)+op.output*l.p),std::move(args),17,true,x86_ && op.final_wide?64U:0U);
        }
        if(returned==1){
            if(op.kind=="ostream_flush"){if(static_cast<std::uint32_t>(value)==UINT32_MAX)setstate(4);}
            else if(op.kind=="ostream_put"){if((value&l.int_mask())==l.eof())setstate(4);}
            else if(value!=op.limit)setstate(4);
            op.phase=1;
        }
        if(op.kind=="ostream_write" && (!op.limit || op.limit>INT64_MAX))op.phase=1;
        if(op.phase==1)return finish(op.object);
        if(op.kind=="ostream_flush")return virt(13,{},1);
        if(op.kind=="ostream_put"){
            if(available(true)<=0)return virt(3,{op.output},1);
            memory.store(GuestAddress{ptr(position_address(true))},l.character_bits(),op.output);consume(true);op.phase=1;
            return finish(op.object);
        }
        if(op.limit>INT64_MAX){setstate(4);return finish(op.object);}
        return virt(9,{op.output,op.limit},1);
    }
    if(op.kind=="istream_read"){
        if(returned==1){const auto count=value<=op.limit?value:0;memory.store(GuestAddress{op.object+l.count_offset()},l.stream_bits(),count);if(count!=op.limit)setstate(3);return finish(op.object);}
        if(!op.limit || op.limit>INT64_MAX)return finish(op.object);
        if(l.legacy)return callback(ptr(ptr(op.buffer)+7*l.p),{op.buffer,op.output,x86_?UINT32_MAX:UINT64_MAX,op.limit},1);
        return virt(8,{op.output,op.limit},1);
    }
    if(op.kind=="prefix_output")return finish(memory.load(GuestAddress{op.ios+l.state},32)==0);
    if(op.kind=="istream_sync"){
        // rdbuf()->pubsync()：回 -1 時設 badbit 並回 -1，否則 0。
        if(returned==24){if(static_cast<std::uint32_t>(value)==UINT32_MAX){setstate(4);return finish(UINT32_MAX);}return finish(0);}
        return virt(13,{},24);
    }
    if(op.kind=="readsome"){
        // in_avail：讀取區有字元時為剩餘數，否則呼叫 showmanyc；小於 0 設 eofbit，大於 0 時讀取 min(n, 可用數)。
        const auto count_bits=l.stream_bits();
        if(returned==26){const auto got=value<=op.limit?value:0;memory.store(GuestAddress{op.object+l.count_offset()},count_bits,got);if(got<op.count)setstate(3);return finish(got,!l.legacy);}
        std::int64_t avail=0;
        if(returned==25)avail=count_bits==32?static_cast<std::int64_t>(static_cast<std::int32_t>(value)):static_cast<std::int64_t>(value);
        else if(available(false)>0)avail=available(false);
        else return virt(5,{},25);
        if(avail<0){setstate(1);return finish(0,!l.legacy);}
        op.count=std::min<std::uint64_t>(op.limit,static_cast<std::uint64_t>(avail));
        if(!op.count || op.limit>INT64_MAX)return finish(0,!l.legacy);
        if(l.legacy)return callback(ptr(ptr(op.buffer)+7*l.p),{op.buffer,op.output,x86_?UINT32_MAX:UINT64_MAX,op.count},26);
        return virt(8,{op.output,op.count},26);
    }
    if(op.kind=="istream_buffer"){
        // 70：underflow 前瞻結果、71：目標 overflow 結果、72：取走已寫出的字元（uflow）、73：略過空白（uflow）。
        const auto target=op.limit;
        const auto target_count=[&]{return ptr(target+l.count_indirect(true));};
        const auto target_next=[&]{return ptr(target+l.next_indirect(true));};
        const auto target_available=[&]{return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{target_count()},32)));};
        const auto complete=[&](unsigned flags){
            if(op.keep_delimiter)memory.store(GuestAddress{op.object+l.count_offset()},l.stream_bits(),op.count);
            if(!op.count)flags|=2U;if(flags)setstate(flags);return finish(op.object);
        };
        if(!target)return complete(0);
        const auto advance=[&](unsigned next)->std::optional<std::uint32_t>{
            if(available(false)>0){consume(false);return std::nullopt;}
            return virt(7,{},next);
        };
        bool have=returned==70;std::uint64_t c=have?value&l.int_mask():0;
        if(returned==71){
            if((value&l.int_mask())==l.eof())return complete(0);
            if(auto pending=advance(72))return pending;
            ++op.count;
        }else if(returned==72){++op.count;}
        for(;;){
            if(!have){
                if(available(false)>0)c=memory.load(GuestAddress{ptr(position_address(false))},l.character_bits());
                else return virt(6,{},70);
            }
            have=false;
            if(c==l.eof())return complete(1);
            if(op.skip && is_space(c)){if(auto pending=advance(73))return pending;continue;}
            op.skip=false;
            if(op.keep_delimiter && c==op.delimiter)return complete(0);
            if(target_available()>0){
                // target_next() 是 pnext 指標格的位址：字元寫到它指向的位置，再推進指標。
                const auto cell=target_next();memory.store(GuestAddress{ptr(cell)},l.character_bits(),c);put(cell,ptr(cell)+l.element);
                const auto count_at=target_count();memory.store(GuestAddress{count_at},32,memory.load(GuestAddress{count_at},32)-1);
            }else return callback(ptr(ptr(target)+l.slot(3)*l.p),{target,c},71);
            if(auto pending=advance(72))return pending;
            ++op.count;
        }
    }
    if(op.kind=="tellg" || op.kind=="seekg_offset" || op.kind=="seekp_offset"){
        const bool query=op.kind=="tellg";
        if(returned==1){if(query){memory.move(GuestAddress{op.output},GuestAddress{op.scratch},24);return finish(op.output);}
            if((l.stream_bits()==32?static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{op.scratch},32)))):memory.load(GuestAddress{op.scratch},64))+memory.load(GuestAddress{op.scratch+8},64)==UINT64_MAX)setstate(2);return finish(op.object);}
        return virt(10,{op.scratch,query?0:op.output,query?1:op.limit,op.kind=="seekp_offset"?2U:1U},1);
    }
    if(op.kind=="istream_get"){
        std::uint64_t c=0;
        if(returned==22)c=value&l.int_mask();
        else if(available(false)>0){c=memory.load(GuestAddress{ptr(position_address(false))},l.character_bits());consume(false);}
        else return virt(7,{},22);
        if(c==l.eof()){setstate(3);return finish(op.output?op.object:l.eof());}
        memory.store(GuestAddress{op.object+l.count_offset()},l.stream_bits(),1);if(op.output)memory.store(GuestAddress{op.output},l.character_bits(),c);
        return finish(op.output?op.object:c);
    }
    if(op.kind=="putback"){
        // sputbackc：前一個字元相同就退回 gptr，否則交給 pbackfail（虛擬表 4）；失敗設定 badbit。
        if(returned==23){if((value&l.int_mask())==l.eof())setstate(4);return finish(op.object);}
        const auto first=ptr(ptr(op.buffer+l.first_indirect(false))),next=ptr(position_address(false));
        if(next && first<next && (op.output==UINT64_MAX || memory.load(GuestAddress{next-l.element},l.character_bits())==op.output)){
            put(position_address(false),next-l.element);const auto count_at=count_address(false);memory.store(GuestAddress{count_at},32,memory.load(GuestAddress{count_at},32)+1);
            return finish(op.object);}
        return virt(4,{op.output==UINT64_MAX?l.eof():op.output},23);
    }
    if(op.kind=="peek"){
        const auto c=returned==21?value&l.int_mask():available(false)>0?memory.load(GuestAddress{ptr(position_address(false))},l.character_bits()):UINT64_MAX;
        if(c==UINT64_MAX)return virt(6,{},21);
        if(c==l.eof())setstate(1);return finish(c);
    }
    if(op.kind=="ignore"){
        if(returned==20){
            if((value&l.int_mask())==l.eof()){setstate(1);return finish(op.object);}
            ++op.count;memory.store(GuestAddress{op.object+l.count_offset()},l.stream_bits(),op.count);
            if((value&l.int_mask())==op.output)return finish(op.object);
        }
        while(op.limit==INT64_MAX || op.count<op.limit){
            if(available(false)<=0)return virt(7,{},20);
            const auto c=memory.load(GuestAddress{ptr(position_address(false))},l.character_bits());consume(false);
            ++op.count;memory.store(GuestAddress{op.object+l.count_offset()},l.stream_bits(),op.count);
            if(c==op.output)return finish(op.object);
        }
        return finish(op.object);
    }
    const auto complete_input=[&](bool eof)->std::optional<std::uint32_t>{
        if(eof)setstate(1);
        if(prefix){if(eof)setstate(2);return finish(eof?0:1);}
        if(line){if(op.limit)memory.store(GuestAddress{op.output+op.count*l.element},l.character_bits(),0);if(!op.count)setstate(2);return finish(op.object);}
        if(op.kind=="num_get"){
            // 與現行 MSVC 相同：iostate 先設為 goodbit／failbit（失敗時目標為 0 或極值），讀到結尾再加 eofbit；回傳指向第一個未用字元的迭代器。
            const auto result=op.number_field.conclude(eof);memory.store(GuestAddress{op.input_value},op.number_field.store_bits(),result.bits);
            memory.store(GuestAddress{op.time_state},32,(result.failed?2U:0U)|(eof?1U:0U));
            put(op.output,eof && !op.limit?0:op.buffer);memory.store(GuestAddress{op.output+l.p},16,0);return finish(op.output);
        }
        if(number){
            // sentry 在略過空白時就遇到 EOF：提取未開始，目標值不變。
            if(op.skip){setstate(2);return finish(op.object);}
            const auto result=op.number_field.conclude(eof);if(result.failed)setstate(2);
            memory.store(GuestAddress{op.output},op.number_field.store_bits(),result.bits);return finish(op.object);
        }
        throw HostCallFault("VC10 輸入狀態無效");
    };
    if(line && !op.limit){setstate(2);return finish(op.object);}
    // 兩參數 getline 以 widen('\n') 為分隔字元：內建 ctype 即原字元，只有客體衍生 facet 需要虛擬回呼。
    if(returned==65)op.delimiter=value&((1U<<(l.element*8))-1U);
    else if(line && l.modern && !op.phase && op.service.symbol.ends_with("_J@Z")){
        op.phase=1;const auto facet=stream_facet(l.element==1?1U:6U);const auto table=facet?ptr(facet):0;const auto& facet_tables=m.tables14[l.element==1?0:1];
        if(facet && table!=facet_tables.ctype)return callback(ptr(table+(l.element==1?8U:12U)*l.p),{facet,'\n'},65);
    }
    if(number && !op.number_ready){
        using Type=msvcp_detail::NumberType;
        const auto type=op.kind=="num_get"?op.numeric_value.type:msvcp_number_type(op.service.symbol);const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{op.ios+l.flags},32));
        const bool names=type==Type::Boolean && (flags&0x4000U);auto& punctuation=op.numeric_punctuation;const auto character_mask_value=l.element==1?255U:65535U;
        if(returned==60){punctuation.decimal=static_cast<char16_t>(value&character_mask_value);return punct(4,61);}
        if(returned==61){punctuation.thousands=static_cast<char16_t>(value&character_mask_value);return punct(5,62);}
        if(returned==62){
            const auto group=read_string(1);punctuation.grouping.clear();for(const auto byte:group)punctuation.grouping.push_back(static_cast<char>(byte));
            if(names)return punct(6,63);
        }else if(returned==63){punctuation.false_name=read_string(l.element);return punct(7,64);}
        else if(returned==64)punctuation.true_name=read_string(l.element);
        else if(l.modern){
            // 內建 numpunct 直接讀欄位；只有客體衍生的 facet 才需要虛擬回呼。
            const auto facet=stream_facet(l.element==1?5U:8U);const auto& facet_tables=m.tables14[l.element==1?0:1];const auto table=facet?ptr(facet):0;
            if(facet && table!=facet_tables.numpunct && table!=facet_tables.numpunct_byname){
                op.numeric_punct=facet;op.scratch=allocate_heap(memory,64);if(!op.scratch){setstate(4);return finish(op.object);}
                memory.fill(GuestAddress{op.scratch},0,64);return punct(3,60);
            }
            if(facet){
                punctuation.decimal=static_cast<char16_t>(memory.load(GuestAddress{facet+3*l.p},l.character_bits()));
                punctuation.thousands=static_cast<char16_t>(memory.load(GuestAddress{facet+3*l.p+l.element},l.character_bits()));
                if(const auto group=ptr(facet+2*l.p))punctuation.grouping=read_ansi(memory,group,256);
                const auto guest_name=[&](std::uint64_t at){std::u16string text;if(!at)return text;
                    for(std::size_t i=0;i<256;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{at+i*l.element},l.character_bits()));if(!c)break;text.push_back(c);}return text;};
                if(names){punctuation.false_name=guest_name(ptr(facet+4*l.p));punctuation.true_name=guest_name(ptr(facet+5*l.p));}
            }
        }
        op.number_field.configure(type,flags,std::move(punctuation),l.p);op.number_ready=true;
    }
    if(op.kind=="num_get" && (!op.buffer || op.limit))return complete_input(true);
    if(prefix && !op.skip)return complete_input(false);
    if(returned==2){if((value&l.int_mask())==l.eof())return complete_input(true);}
    bool have=returned==1;
    for(;;){
        std::uint64_t c=0;
        if(have){c=value&l.int_mask();have=false;}
        else if(available(false)>0)c=memory.load(GuestAddress{ptr(position_address(false))},l.character_bits());
        else return virt(6,{},1);
        if(c==l.eof())return complete_input(true);
        if(op.skip && is_space(c)){}
        else{
            op.skip=false;if(prefix)return complete_input(false);
            if(line){
                if(c==op.delimiter && op.keep_delimiter){if(op.limit)memory.store(GuestAddress{op.output+op.count*l.element},l.character_bits(),0);return finish(op.object);}
                if(c==op.delimiter){
                    memory.store(GuestAddress{op.object+l.count_offset()},l.stream_bits(),op.count+1);
                    if(available(false)>0){consume(false);if(op.limit)memory.store(GuestAddress{op.output+op.count*l.element},l.character_bits(),0);return finish(op.object);}
                    op.kind="getline_delimiter";return virt(7,{},3);
                }
                if(!op.limit || op.count+1>=op.limit){if(!op.keep_delimiter || !op.limit)setstate(2);return complete_input(false);}
                memory.store(GuestAddress{op.output+op.count*l.element},l.character_bits(),c);++op.count;memory.store(GuestAddress{op.object+l.count_offset()},l.stream_bits(),op.count);
            }else if(number){
                if(!op.number_field.accept(static_cast<char16_t>(c)))return complete_input(false);
            }
        }
        if(available(false)>0)consume(false);else return virt(7,{},2);
    }
}
}
