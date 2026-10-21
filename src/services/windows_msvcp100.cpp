#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "crt_c_locale.hpp"
#include "crt_float_chars.hpp"
#include "msvcp_stream_abi.hpp"
#include "msvcp_numeric.hpp"
#include "msvcp_rtti_shapes.hpp"
#include "msvc_exception_abi.hpp"
#include "windows_locale_data.hpp"
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
struct Operation {
    WindowsServiceDescriptor service;
    std::string kind,text;
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
};
msvcp_detail::NumberType msvcp_number_type(std::string_view name){
    using Type=msvcp_detail::NumberType;
    if(name.ends_with("_N@Z"))return Type::Boolean;if(name.ends_with("_J@Z"))return Type::Signed64;
    if(name.ends_with("_K@Z"))return Type::Unsigned64;if(name.ends_with("F@Z"))return Type::Signed16;
    if(name.ends_with("G@Z"))return Type::Unsigned16;if(name.ends_with("I@Z") || name.ends_with("K@Z"))return Type::Unsigned32;
    if(name.ends_with("M@Z"))return Type::Floating32;if(name.ends_with("N@Z") || name.ends_with("O@Z"))return Type::Floating64;
    if(name.ends_with("PEBX@Z") || name.ends_with("PBX@Z"))return Type::Pointer;
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
unsigned character_mask(char16_t c){
    unsigned value=0;
    if(u_isupper(c))value|=1;if(u_islower(c))value|=2;if(u_isdigit(c))value|=4;
    if(u_isUWhiteSpace(c))value|=8;if(u_ispunct(c))value|=16;if(u_iscntrl(c))value|=32;
    if(u_isblank(c))value|=64;if(u_isxdigit(c))value|=128;if(u_isalpha(c))value|=0x100;
    return value;
}
}
struct Msvcp100State {
    std::map<std::string,std::uint64_t,std::less<>> data;
    std::map<std::string,std::uint64_t,std::less<>> data14;
    std::map<std::uint64_t,Operation> operations;
    std::map<std::uint64_t,std::pair<std::uint64_t,std::uint64_t>> ios_objects;
    std::uint64_t global{},codecvt_table{},ctype_table{},buffer_table{},ios_table{},istream_vb{},iostream_vb{},next{1},next_id{2};
    struct Tables {std::uint64_t buffer{},ios{},istream{},ostream{},iostream{},codecvt{},ctype{},num_put{},numpunct{},numpunct_byname{};};
    std::array<Tables,2> tables14;
    std::uint64_t global14{},classic14{},facet_table14{},locimp_table14{},ios_base_table14{},ostream_vb{},ostream_vb14{},istream_vb14{},iostream_vb14{},next_id14{8};
    std::uint64_t ctype_masks14{};
    std::map<std::uint64_t,unsigned> standard_buffers;
    std::set<std::uint64_t> owned_locale_imps;
    struct CapturedException {std::uint64_t record{},object{},destroy{};bool interface_handle{},constructed{},immortal{};};
    std::map<std::uint64_t,CapturedException> exception_pointers;
    std::uint64_t exception_control_table{},bad_alloc_pointer{},bad_exception_pointer{};
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
    s.buffer_table=table({"StreamDelete","StreamLock","StreamUnlock","StreamOverflow","StreamPbackfail","StreamShowmany","StreamUnderflow","StreamUflow","StreamXsgetn","StreamXsputn","StreamSeekoff","StreamSeekpos","StreamSetbuf","StreamSync","StreamImbue"});
    s.ios_table=table({"IosDelete"});
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
    const auto modern_table=[&](char element,std::initializer_list<const char*> names,msvc_rtti::Shape shape=msvc_rtti::Shape{}){
        const auto storage=runtime_allocate(memory,{},(names.size()+1)*l.p),address=storage+l.p;unsigned i=0;
        for(const auto name:names)put(address+i++*l.p,resolve("internal-msvcp140",std::string(1,element)+name+(x86_?"32":"64")).value);
        if(!shape.bases.empty()){rtti_tables.emplace_back(storage,rtti.add(shape));s.data14.emplace("??_7"+shape.bases.front().name.substr(4)+"6B@",address);}return address;
    };
    for(unsigned element=0;element<2;++element){auto& t=s.tables14[element];const auto prefix=element?'W':'D';
        const auto stream_shape=[&](std::string_view kind){return msvcp_detail::stream_shape(kind,element!=0,l.p);};const std::string character=element?"_W":"D";
        t.buffer=modern_table(prefix,{"StreamDelete","StreamLock","StreamUnlock","StreamOverflow","StreamPbackfail","StreamShowmany","StreamUnderflow","StreamUflow","StreamXsgetn","StreamXsputn","StreamSeekoff","StreamSeekpos","StreamSetbuf","StreamSync","StreamImbue"},stream_shape("streambuf"));
        t.ios=modern_table(prefix,{"IosDelete"},stream_shape("ios"));t.istream=modern_table(prefix,{"IosDelete"},stream_shape("istream"));
        t.ostream=modern_table(prefix,{"IosDelete"},stream_shape("ostream"));t.iostream=modern_table(prefix,{"IosDelete"},stream_shape("iostream"));
        t.codecvt=modern_table(prefix,{"FacetDelete","FacetIncref","FacetDecref","CodecvtNoconv","CodecvtMaxLength","CodecvtEncoding","CodecvtIn","CodecvtOut","CodecvtUnshift","CodecvtLength"},element?msvcp_detail::facet_shape(".?AV?$codecvt@_WDU_Mbstatet@@@std@@",l.p,".?AVcodecvt_base@std@@"):msvc_rtti::Shape{});
        const auto ctype_shape=msvcp_detail::facet_shape(".?AV?$ctype@"+character+"@std@@",l.p,".?AUctype_base@std@@");
        if(element)t.ctype=modern_table(prefix,{"FacetDelete","FacetIncref","FacetDecref","CtypeIsRange","CtypeIs","CtypeScanIs","CtypeScanNot","CtypeLowerRange","CtypeLower","CtypeUpperRange","CtypeUpper","CtypeWidenRange","CtypeWiden","CtypeNarrowRange","CtypeNarrow"},ctype_shape);
        else t.ctype=modern_table(prefix,{"FacetDelete","FacetIncref","FacetDecref","CtypeLowerRange","CtypeLower","CtypeUpperRange","CtypeUpper","CtypeWidenRange","CtypeWiden","CtypeNarrowRange","CtypeNarrow"},ctype_shape);
        t.num_put=modern_table(prefix,{"FacetDelete","FacetIncref","FacetDecref","NumPutPointer","NumPutLongDouble","NumPutDouble","NumPutUnsigned64","NumPutSigned64","NumPutUnsigned32","NumPutSigned32","NumPutBoolean"},msvcp_detail::facet_shape(".?AV?$num_put@"+character+"V?$ostreambuf_iterator@"+character+"U?$char_traits@"+character+"@std@@@std@@@std@@",l.p));
        t.numpunct=modern_table(prefix,{"FacetDelete","FacetIncref","FacetDecref","NumPunctDecimal","NumPunctThousands","NumPunctGrouping","NumPunctFalseName","NumPunctTrueName"},msvcp_detail::facet_shape(".?AV?$numpunct@"+character+"@std@@",l.p));
        t.numpunct_byname=modern_table(prefix,{"FacetDelete","FacetIncref","FacetDecref","NumPunctDecimal","NumPunctThousands","NumPunctGrouping","NumPunctFalseName","NumPunctTrueName"},msvcp_detail::facet_shape(".?AV?$numpunct_byname@"+character+"@std@@",l.p,".?AV?$numpunct@"+character+"@std@@"));
    }
    s.facet_table14=modern_table('W',{"FacetDelete","FacetIncref","FacetDecref"},msvcp_detail::facet_shape(".?AVfacet@locale@std@@",l.p));
    s.locimp_table14=modern_table('W',{"FacetDelete","FacetIncref","FacetDecref"},msvcp_detail::facet_shape(".?AV_Locimp@locale@std@@",l.p));
    s.ios_base_table14=modern_table('D',{"IosDelete"},msvcp_detail::stream_shape("ios_base",false,l.p));
    s.ctype_masks14=runtime_allocate(memory,{},512);for(unsigned c=0;c<256;++c)memory.store(GuestAddress{s.ctype_masks14+c*2},16,crt_c_locale::character_mask(c));
    const Layout modern(x86_,true,1);s.global14=runtime_allocate(memory,{},96);s.classic14=s.global14;put(s.global14,s.locimp_table14);memory.store(GuestAddress{s.global14+l.p},32,1);
    const auto modern_facets=runtime_allocate(memory,{},9*l.p);put(s.global14+2*l.p,modern_facets);put(s.global14+3*l.p,9);memory.store(GuestAddress{s.global14+4*l.p},32,0x3f);put(s.global14+4*l.p+8,name);
    const auto classic_group=runtime_allocate(memory,std::array<std::uint8_t,1>{0});
    for(unsigned element=0;element<2;++element){const auto characters=element?2U:1U;const auto& tables=s.tables14[element];
        const auto type=runtime_allocate(memory,{},96);put(type,tables.ctype);memory.store(GuestAddress{type+l.p},32,1);
        put(type+2*l.p+(x86_?4U:8U),s.ctype_masks14);s.facet_locales.emplace(type,Msvcp100State::FacetLocale{});
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
        const auto address=runtime_allocate(memory,{},l.p);put(address,id);s.data14.emplace(symbol,address);
    }
    for(unsigned element=0;element<2;++element){const auto character=element?"_W":"D";const auto& tables=s.tables14[element];
        s.data14.emplace(std::string("??_7?$numpunct@")+character+"@std@@6B@",tables.numpunct);
        s.data14.emplace(std::string("??_7?$num_put@")+character+"V?$ostreambuf_iterator@"+character+"U?$char_traits@"+character+"@std@@@std@@@std@@6B@",tables.num_put);
    }
    s.data14.emplace("?_Raise_handler@std@@3P6AXAEBVexception@stdext@@@ZEA",runtime_allocate(memory,{},l.p));
    s.ostream_vb=runtime_allocate(memory,{},8);memory.store(GuestAddress{s.ostream_vb+4},32,8);
    s.ostream_vb14=runtime_allocate(memory,{},8);memory.store(GuestAddress{s.ostream_vb14+4},32,modern.ostream_base());
    s.istream_vb14=runtime_allocate(memory,{},8);memory.store(GuestAddress{s.istream_vb14+4},32,modern.istream_base());
    s.iostream_vb14=runtime_allocate(memory,{},16);memory.store(GuestAddress{s.iostream_vb14+4},32,modern.iostream_base());
    memory.store(GuestAddress{s.iostream_vb14+8},32,static_cast<std::uint32_t>(-16));memory.store(GuestAddress{s.iostream_vb14+12},32,modern.iostream_base()-16);
    // 標準串流使用相同客體 ABI 與 streambuf 虛擬派送，輸出交給既有 Windows 標準 handle。
    const auto console_buffer=runtime_allocate(memory,{},modern.buffer_size);put(console_buffer,s.tables14[0].buffer);
    for(unsigned side=0;side<2;++side){put(console_buffer+modern.first_indirect(side),console_buffer+modern.first(side));put(console_buffer+modern.next_indirect(side),console_buffer+modern.next(side));put(console_buffer+modern.count_indirect(side),console_buffer+modern.count(side));}
    const auto buffer_locale=runtime_allocate(memory,{},modern.locale_size());put(buffer_locale+modern.locale_pointer(),s.global14);put(console_buffer+modern.buffer_locale(),buffer_locale);s.standard_buffers.emplace(console_buffer,1);
    const auto cout=runtime_allocate(memory,{},modern.ostream_base()+modern.ios_size+3*l.p);put(cout,s.ostream_vb14);const auto cout_ios=cout+modern.ostream_base();put(cout_ios,s.tables14[0].ostream);memory.store(GuestAddress{cout_ios-4},32,0);
    put(cout_ios+l.p,1);memory.store(GuestAddress{cout_ios+modern.flags},32,0x201);memory.store(GuestAddress{cout_ios+modern.precision},64,6);
    const auto cout_locale=runtime_allocate(memory,{},modern.locale_size());put(cout_locale+modern.locale_pointer(),s.global14);put(cout_ios+modern.locale,cout_locale);put(cout_ios+modern.ios_size,console_buffer);memory.store(GuestAddress{cout_ios+modern.ios_size+2*l.p},8,' ');
    memory.store(GuestAddress{s.global14+l.p},32,3);s.ios_objects.emplace(cout_ios,std::pair{cout,modern.ostream_base()+modern.ios_size+3*l.p});s.data14.emplace("?cout@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A",cout);
    s.exception_control_table=modern_table('D',{"ExceptionControlDestroy","ExceptionControlDelete","ExceptionControlDestructor","ExceptionControlDeleter"});
    for(unsigned i=0;i<3;++i){const std::array<std::string_view,3> names{"generic","system","iostream"};const std::array<std::string,3> types{".?AV_Generic_error_category@std@@",".?AV_System_error_category@std@@",".?AV_Iostream_error_category2@std@@"};
        msvc_rtti::Shape shape;shape.bases={{types[i],1},{".?AVerror_category@std@@",0}};
        const auto category_table=modern_table('D',{"ErrorCategoryDelete","ErrorCategoryName","ErrorCategoryMessage","ErrorCategoryCondition","ErrorCategoryEquivalentB","ErrorCategoryEquivalentA"},std::move(shape));
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
    for(const auto& [type_name,offset]:rtti.types)s.data14.emplace("??_R0"+type_name.substr(1)+"@8",rtti_image+offset);
}
void WindowsServices::initialize_msvcp_numeric_facet(GuestState& state,GuestMemory& memory,std::uint64_t object,bool punct,bool wide,const CrtLocaleCategory& category,std::uint32_t references,bool defaults){
    auto& s=*msvcp100_;const unsigned unit=x86_?4:8,characters=wide?2:1;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto& tables=s.tables14[wide?1:0];Msvcp100State::FacetLocale metadata;metadata.category=category;metadata.resources.reserve(3);
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
GuestAddress WindowsServices::msvcp100_data(std::string_view name,bool modern)const{
    if(!msvcp100_)return GuestAddress{};const auto& data=modern?msvcp100_->data14:msvcp100_->data;const auto it=data.find(name);return it==data.end()?GuestAddress{}:GuestAddress{it->second};
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
        const Layout l(x86_,op.modern,op.element);const auto at=op.ios+l.state;
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

std::optional<std::uint32_t> WindowsServices::invoke_msvcp100(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);auto& s=*msvcp100_;const auto l=msvcp_detail::abi(x86_,service);const auto bits=static_cast<std::uint8_t>(l.p*8);
    const auto name=service.symbol;const bool internal=service.family=="internal-msvcp100" || service.family=="internal-msvcp140";
    if(l.modern && !s.global14)s.global14=s.classic14;
    const auto global=l.modern?s.global14:s.global;
    const auto& tables=s.tables14[l.element==1?0:1];const auto buffer_table=l.modern?tables.buffer:s.buffer_table,ios_table=l.modern?tables.ios:s.ios_table;
    const bool stat=service.caller_cleanup || name.starts_with("?_Getcat@") || name.starts_with("?_Getgloballocale@") || name.starts_with("?_Ios_base_dtor@");
    const auto object=stat?0:x86_?state.reg(Gpr::Rcx)&UINT32_MAX:call(0);
    const auto a=[&](unsigned i){return call(i+(!x86_ && !stat?1:0));};
    const auto ptr=[&](std::uint64_t at){return memory.load(GuestAddress{at},bits);};
    const auto put=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},bits,value);};
    const auto done=[&](std::uint64_t value,bool wide=false)->std::optional<std::uint32_t>{call.return_to(state,value,wide);return {};};
    const auto method=internal?name.substr(l.modern?1:0,name.size()-(l.modern?3:2)):name.substr(1,name.find('@')-1);
    const auto refs=[&](std::uint64_t imp){return memory.load(GuestAddress{imp+l.p},l.reference_bits());};
    const auto put_refs=[&](std::uint64_t imp,std::uint64_t n){memory.store(GuestAddress{imp+l.p},l.reference_bits(),n);};
    const auto virtual_call=[&](std::uint64_t self,unsigned slot,std::vector<std::uint64_t> args)->std::optional<std::uint32_t>{
        args.insert(args.begin(),self);GuestCallback cb{ptr(ptr(self)+slot*l.p),std::move(args)};cb.x86_register_arguments=x86_?1:0;
        start_callbacks(state,memory,{std::move(cb)},Continuation::ReturnCallback);return {};
    };
    const auto new_locale=[&]{const auto loc=allocate_heap(memory,l.locale_size());if(!loc)throw std::bad_alloc{};memory.fill(GuestAddress{loc},0,l.locale_size());put(loc+l.locale_pointer(),global);put_refs(global,refs(global)+1);return loc;};
    if(method=="FacetRetire"){
        Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="retire_facet";op.object=a(0);
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
    if(method=="LocaleUnlock"){
        GuestCallback callback{resolve("msvcp140.dll",x86_?"??1_Lockit@std@@QAE@XZ":"??1_Lockit@std@@QEAA@XZ").value,{a(0)}};
        callback.x86_register_arguments=x86_?1:0;callback.exception_policy=GuestExceptionPolicy::NoexceptCpp;
        start_callbacks(state,memory,{std::move(callback)},Continuation::ReturnZeroFreeHeap,a(0));return {};
    }
    if(l.modern && method=="_Setgloballocale"){s.global14=a(0);return done(0);}
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
    if(l.modern && (numeric_punct || numeric_put) && name.starts_with("??0")){
        auto category=current_crt_locale(state,memory).categories[4];const bool byname=name.find("numpunct_byname@")!=name.npos;
        if(byname){const auto named=windows_crt_locale(utf8_to_utf16(read_ansi(memory,a(0),4095)));if(!named){throw_msvc_exception(state,memory,service,MsvcExceptionKind::RuntimeError);return {};}category=*named;}
        initialize_msvcp_numeric_facet(state,memory,object,numeric_punct,l.element==2,category,static_cast<std::uint32_t>(a(byname?1:0)));if(byname)put(object,tables.numpunct_byname);return done(object);
    }
    if(l.modern && (numeric_punct || numeric_put) && name.starts_with("??1")){
        if(const auto found=s.facet_locales.find(object);found!=s.facet_locales.end()){for(const auto resource:found->second.resources)(void)free_heap(memory,resource,state);s.facet_locales.erase(found);}
        put(object,s.facet_table14);return done(0);
    }
    if(l.modern && (numeric_punct || numeric_put) && method=="_Getcat"){
        if(a(0) && !ptr(a(0))){const auto imp=a(1)?ptr(a(1)+l.locale_pointer()):global;const auto label=ptr(imp+4*l.p+8);
            const auto text=label?utf8_to_utf16(read_ansi(memory,label,4095)):std::u16string{u"C"};
            const auto category=text==u"*"?std::optional<CrtLocaleCategory>{current_crt_locale(state,memory).categories[4]}:windows_crt_locale(text);
            if(!category){throw_msvc_exception(state,memory,service,MsvcExceptionKind::RuntimeError);return {};}
            const auto facet=allocate_heap(memory,(numeric_punct?6U:2U)*l.p);if(!facet){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
            try{initialize_msvcp_numeric_facet(state,memory,facet,numeric_punct,l.element==2,*category,0,numeric_punct);put(a(0),facet);}
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
    if(method.starts_with("NumPut")){
        using Type=msvcp_detail::NumberType;const auto type=method=="NumPutBoolean"?Type::Boolean:method=="NumPutUnsigned32"?Type::Unsigned32:method=="NumPutSigned64"?Type::Signed64:method=="NumPutUnsigned64"?Type::Unsigned64:method=="NumPutDouble" || method=="NumPutLongDouble"?Type::Floating64:method=="NumPutPointer"?Type::Pointer:Type::Signed32;
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
    if(method=="ThrowBadAlloc"){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
    if(method=="generic_category" || method=="system_category" || method=="iostream_category")return done(s.error_categories[method=="generic_category"?0:method=="system_category"?1:2]);
    if(method=="_Winerror_map")return done(windows_generic_error_condition(static_cast<std::uint32_t>(a(0))).value_or(0));
    if(method=="_Syserror_map"){
        const auto text=windows_generic_error_message(static_cast<std::uint32_t>(a(0)));
        if(const auto found=s.generic_error_messages.find(text);found!=s.generic_error_messages.end())return done(found->second);
        std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);const auto address=runtime_allocate(memory,bytes);
        s.generic_error_messages.emplace(text,address);return done(address);
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
    if(l.modern && name.find("_Locinfo@")!=name.npos){
        if(method=="_Getcoll"){
            const auto output=a(0);memory.check(GuestAddress{output},2*l.p,Access::Write);const auto& category=current_crt_locale(state,memory).categories[1];std::uint64_t locale_name=0;
            if(category.codepage){const auto bytes=utf16_bytes(category.name);locale_name=allocate_heap(memory,bytes.size());if(!locale_name){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
                try{memory.write(GuestAddress{locale_name},bytes);}catch(...){(void)free_heap(memory,locale_name,state);throw;}}
            memory.write(GuestAddress{output},std::vector<std::uint8_t>(2*l.p));memory.store(GuestAddress{output},32,category.codepage);put(output+l.p,locale_name);return done(output);
        }
        if(name.starts_with("??0") || name.starts_with("??1")){
            const bool destroy=name.starts_with("??1");if(!destroy && !a(0)){throw_msvc_exception(state,memory,service,MsvcExceptionKind::RuntimeError);return {};}
            Operation op;op.service=service;op.modern=true;op.element=1;op.kind=destroy?"locinfo_destroy":"locinfo_construct";op.object=object;op.buffer=destroy?0:a(0);
            if(!destroy)memory.write(GuestAddress{object},std::vector<std::uint8_t>(x86_?52:104));
            const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
        }
        throw HostCallFault("未知 STL _Locinfo 操作");
    }
    if(l.modern && (method.starts_with("__ExceptionPtr") || method.starts_with("ExceptionControl"))){
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
        const bool rethrow=method=="__ExceptionPtrRethrow";
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
    if(l.modern && method=="_Init" && name.find("@locale@")!=name.npos){if(static_cast<std::uint8_t>(a(0)))put_refs(global,refs(global)+1);return done(global);}
    if(l.modern && method=="_New_Locimp"){
        const auto source=a(0);memory.check(GuestAddress{source},x86_?32:56,Access::Read);
        Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="clone_locale_imp";op.locale_source=source;
        op.locale_lock_storage=allocate_heap(memory,16);if(!op.locale_lock_storage){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
        memory.fill(GuestAddress{op.locale_lock_storage},0,16);const auto token=s.next++;
        try{s.operations.emplace(token,std::move(op));}catch(...){(void)free_heap(memory,op.locale_lock_storage,state);throw;}
        return resume_msvcp100(state,memory,token,0);
    }
    if(l.modern && method=="_Locimp_Addfac"){
        const auto imp=a(0),facet=a(1),id=a(2);if(id>=65536)throw HostCallFault("STL facet id 超過上限");memory.check(GuestAddress{imp},x86_?32:56,Access::Write);
        Operation op;op.service=service;op.modern=true;op.element=l.element;op.kind="add_locale_facet";op.object=imp;op.output=id;op.buffer=facet;
        op.locale_lock_storage=allocate_heap(memory,16);if(!op.locale_lock_storage){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
        memory.fill(GuestAddress{op.locale_lock_storage},0,16);const auto token=s.next++;
        try{s.operations.emplace(token,std::move(op));}catch(...){(void)free_heap(memory,op.locale_lock_storage,state);throw;}
        return resume_msvcp100(state,memory,token,0);
    }
    if(method=="SetWidth" || method=="SetPrecision"){
        memory.store(GuestAddress{a(0)+(method=="SetWidth"?l.width:l.precision)},64,a(1));return done(0);
    }
    if(method=="setw" || method=="setprecision"){
        const auto output=a(0);memory.check(GuestAddress{output},16,Access::Write);
        memory.write(GuestAddress{output},std::array<std::uint8_t,16>{});
        put(output,resolve("internal-msvcp140",std::string("D")+(method=="setw"?"SetWidth":"SetPrecision")+(x86_?"32":"64")).value);
        memory.store(GuestAddress{output+8},64,a(1));return done(output);
    }
    if(name.starts_with("??Bid@")){auto value=ptr(object);if(!value){value=l.modern?++s.next_id14:++s.next_id;put(object,value);}return done(value);}
    if(method=="_Incref" || method=="_Decref" || method=="FacetIncref" || method=="FacetDecref"){
        auto n=refs(object);if(method=="_Incref" || method=="FacetIncref"){put_refs(object,n+1);return done(0);}
        if(n)put_refs(object,--n);return done(n?0:object);
    }
    if(method=="_Getcat"){
        if(a(0) && !ptr(a(0))){
            const auto loc=a(1)?ptr(a(1)+l.locale_pointer()):global;
            const auto locale_name=ptr(loc+4*l.p+8);
            const auto text=locale_name?utf8_to_utf16(read_ansi(memory,locale_name,4095)):std::u16string{u"C"};
            const auto category=text==u"*"?std::optional<CrtLocaleCategory>{current_crt_locale(state,memory).categories[2]}:windows_crt_locale(text);
            if(!category){throw_msvc_exception(state,memory,service,MsvcExceptionKind::RuntimeError);return {};}
            const bool codecvt=name.find("codecvt@")!=name.npos;const auto size=l.modern?(codecvt?64U:96U):64U;const auto facet=allocate_heap(memory,size);
            if(!facet){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}Msvcp100State::FacetLocale metadata;metadata.category=*category;
            try{
                memory.write(GuestAddress{facet},std::vector<std::uint8_t>(size));put(facet,codecvt?(l.modern?tables.codecvt:s.codecvt_table):(l.modern?tables.ctype:s.ctype_table));
                if(l.modern){
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
        if((a(0)&1U) && !free_heap(memory,object,state))throw HostCallFault("VC10 facet 配置無效");return done(object);
    }
    if(method=="is" && name.find("ctype@")!=name.npos)return done((character_mask(static_cast<char16_t>(a(1)))&a(0))!=0);
    if(method=="always_noconv")return virtual_call(object,l.facet_slot(1),{});
    if(method=="in" || method=="out" || method=="unshift"){
        std::vector<std::uint64_t> args;for(unsigned i=0;i<(method=="unshift"?4U:7U);++i)args.push_back(a(i));
        return virtual_call(object,l.facet_slot(method=="in"?4:method=="out"?5:6),std::move(args));
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
    if(l.modern && (name.starts_with("??0facet@") || name.starts_with("??0?$codecvt@"))){
        const bool codecvt=name.starts_with("??0?$codecvt@");memory.write(GuestAddress{object},std::vector<std::uint8_t>(codecvt?64:2*l.p));
        put(object,codecvt?tables.codecvt:s.facet_table14);put_refs(object,a(0));
        if(codecvt){const auto category=current_crt_locale(state,memory).categories[2];const auto vector=object+2*l.p;
            memory.store(GuestAddress{vector},32,category.codepage);memory.store(GuestAddress{vector+4},32,category.maximum_character_bytes);memory.store(GuestAddress{vector+8},32,category.codepage==0);
            for(unsigned c=0;c<256;++c)if(category.lead[c]){const auto at=vector+12+c/8;memory.store(GuestAddress{at},8,memory.load(GuestAddress{at},8)|(1U<<(c%8)));}
            Msvcp100State::FacetLocale metadata;metadata.category=category;s.facet_locales.insert_or_assign(object,std::move(metadata));
        }return done(object);
    }
    if(l.modern && (name.starts_with("??1facet@") || name.starts_with("??1?$codecvt@"))){s.facet_locales.erase(object);put(object,s.facet_table14);return done(0);}
    if(l.modern && method=="tolower"){const bool range=name.find("PEAD")!=name.npos || name.find("PEA_W")!=name.npos || name.find("PAD")!=name.npos || name.find("PA_W")!=name.npos;
        return virtual_call(object,(l.element==1?3U:7U)+(range?0U:1U),range?std::vector<std::uint64_t>{a(0),a(1)}:std::vector<std::uint64_t>{a(0)});}
    if(method.starts_with("Ctype")){
        if(method=="CtypeIs")return done((character_mask(static_cast<char16_t>(a(1)))&a(0))!=0);
        if(method=="CtypeIsRange"){
            if(a(1)<a(0) || (a(1)-a(0))%l.element)throw HostCallFault("STL ctype 分類範圍無效");const auto count=(a(1)-a(0))/l.element;
            for(std::uint64_t i=0;i<count;++i)memory.store(GuestAddress{a(2)+i*2},16,character_mask(static_cast<char16_t>(memory.load(GuestAddress{a(0)+i*l.element},l.character_bits()))));return done(a(1));
        }
        if(method=="CtypeScanIs" || method=="CtypeScanNot"){
            auto first=a(1);if(a(2)<first || (a(2)-first)%l.element)throw HostCallFault("STL ctype 掃描範圍無效");
            while(first<a(2)){const bool matches=(character_mask(static_cast<char16_t>(memory.load(GuestAddress{first},l.character_bits())))&a(0))!=0;if(matches==(method=="CtypeScanIs"))break;first+=l.element;}return done(first);
        }
        const bool lower=method.starts_with("CtypeLower"),upper=method.starts_with("CtypeUpper"),range=method.ends_with("Range");
        const bool widen=method.starts_with("CtypeWiden"),narrow=method.starts_with("CtypeNarrow");
        const auto locale=s.facet_locales.find(object);const auto category=locale==s.facet_locales.end()?CrtLocaleCategory{}:locale->second.category;
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
    if(ios && method=="fill"){const auto at=object+l.ios_size+2*l.p;const auto old=memory.load(GuestAddress{at},l.character_bits());memory.store(GuestAddress{at},l.character_bits(),a(0));return done(old);}
    if(ios && method=="widen"){
        if(l.modern){const auto locale=ptr(object+l.locale),imp=locale?ptr(locale+l.locale_pointer()):0;const auto id=l.element==1?1U:6U;
            if(imp && ptr(imp+3*l.p)>id){const auto facet=ptr(ptr(imp+2*l.p)+id*l.p);if(facet)return virtual_call(facet,l.element==1?8U:12U,{a(0)&255U});}}
        return done(a(0)&(l.element==1?255U:65535U));
    }
    if(ios && name.starts_with("??0")){if(l.modern)memory.write(GuestAddress{object},std::vector<std::uint8_t>(l.ios_size+3*l.p));put(object,ios_table);s.ios_objects[object]={object,(l.ios_size+3*l.p+7U)&~7U};return done(object);}
    if((input_stream || io_stream || output_stream) && name.starts_with("??0")){
        const auto iostream_vb=l.modern?s.iostream_vb14:s.iostream_vb,istream_vb=l.modern?s.istream_vb14:s.istream_vb,ostream_vb=l.modern?s.ostream_vb14:s.ostream_vb;
        if(static_cast<std::uint32_t>(a(io_stream?1:2))){put(object,io_stream?iostream_vb:output_stream?ostream_vb:istream_vb);if(io_stream)put(object+16,iostream_vb+8);}
        const auto vb=ptr(object);const auto base=object+static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{vb+4},32))));
        s.ios_objects[base]={object,base-object+((l.ios_size+3*l.p+7U)&~7U)};
        memory.write(GuestAddress{base},std::vector<std::uint8_t>(l.ios_size+3*l.p));put(base,l.modern?(io_stream?tables.iostream:output_stream?tables.ostream:tables.istream):ios_table);
        if(l.modern){const auto expected=io_stream?l.iostream_base():output_stream?l.ostream_base():l.istream_base();memory.store(GuestAddress{base-4},32,static_cast<std::uint32_t>(base-object-expected));}
        memory.store(GuestAddress{base+l.flags},32,0x201);memory.store(GuestAddress{base+l.precision},64,6);
        put(base+l.locale,new_locale());put(base+l.ios_size,a(0));memory.store(GuestAddress{base+l.ios_size+2*l.p},l.character_bits(),' ');
        if(!io_stream && static_cast<std::uint8_t>(a(1)))put(base+l.p,1);
        if(!a(0))memory.store(GuestAddress{base+l.state},32,4);
        if(!output_stream)memory.store(GuestAddress{object+8},64,0);return done(object);
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
    if((input_stream || io_stream || output_stream) && name.starts_with("??1")){
        if(l.modern){const auto expected=io_stream?l.iostream_base():output_stream?l.ostream_base():l.istream_base();const auto root=object-expected;
            const auto offset=static_cast<std::int32_t>(memory.load(GuestAddress{ptr(root)+4},32));const auto base=root+static_cast<std::uint64_t>(static_cast<std::int64_t>(offset));
            put(base,io_stream?tables.iostream:output_stream?tables.ostream:tables.istream);memory.store(GuestAddress{base-4},32,static_cast<std::uint32_t>(static_cast<std::int64_t>(offset)-expected));
        }return done(0); // 原生解構器會更新 vptr／vtordisp；virtual base 的解構另行執行。
    }
    if((ios && name.starts_with("??1")) || method=="_Ios_base_dtor"){
        const auto self=stat?a(0):object;if(l.modern)put(self,s.ios_base_table14);
        if(l.modern?memory.load(GuestAddress{self+l.p},32):ptr(self+l.p))return done(0);
        Operation op;op.service=service;op.modern=l.modern;op.element=l.element;op.kind="destroy_ios";op.object=self;op.objects.push_back(self);
        const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
    }
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
    else if(method=="sputn"){op.output=a(0);op.limit=a(1);}
    else if(method=="xsgetn" || method=="xsputn" || method=="StreamXsgetn" || method=="StreamXsputn"){
        op.kind=method=="xsputn" || method=="StreamXsputn"?"write":"read";op.output=a(0);op.limit=a(1);if(op.limit>INT64_MAX)op.limit=0;
    }else if(input_stream || output_stream){
        op.ios=object+static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{ptr(object)+4},32))));op.buffer=ptr(op.ios+l.ios_size);
        if(output_stream){
            if(name.starts_with("??6") && name.find("P6A")!=name.npos){
                const bool ios_manipulator=name.find("ios_base@")!=name.npos || name.find("basic_ios@")!=name.npos;
                start_callbacks(state,memory,{{a(0),{ios_manipulator?op.ios:object}}},ios_manipulator?Continuation::Return:Continuation::ReturnCallback,object);return {};
            }
            if(method=="_Osfx" || method=="flush")op.kind=method=="flush"?"ostream_flush":"ostream_suffix";
            else if(method=="write"){op.kind="ostream_write";op.output=a(0);op.limit=a(1);}
            else if(method=="put"){op.kind="ostream_put";op.output=a(0)&((1U<<(l.element*8))-1U);op.limit=1;}
            else if(name.starts_with("??6")){
                const auto locale=ptr(op.ios+l.locale),imp=locale?ptr(locale+l.locale_pointer()):0;const auto id=l.element==1?4U:7U;
                const auto num_put=l.modern && imp && ptr(imp+3*l.p)>id?ptr(ptr(imp+2*l.p)+id*l.p):0;
                if(num_put){
                    const bool single=name.ends_with("@M@Z"),floating=single || name.ends_with("@N@Z"),wide=name.ends_with("@_J@Z") || name.ends_with("@_K@Z");
                    op.kind="ostream_number_facet";op.limit=num_put;op.count=a(0);op.final_wide=wide || floating;
                    if(floating){std::uint64_t raw=x86_?a(0):0;if(!x86_)for(unsigned i=0;i<(single?4U:8U);++i)raw|=std::uint64_t{state.xmm[1][i]}<<(i*8);
                        op.count=single?std::bit_cast<std::uint64_t>(static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw)))):raw;op.output=5;
                    }else if(name.ends_with("@_N@Z")){op.count=static_cast<std::uint8_t>(a(0))!=0;op.output=10;}
                    else if(wide)op.output=name.ends_with("@_J@Z")?7:6;
                    else {op.output=name.ends_with("@G@Z") || name.ends_with("@I@Z")?8:9;
                        if(name.ends_with("@F@Z")){const auto flags=memory.load(GuestAddress{op.ios+l.flags},32)&0xe00U;op.count=flags==0x400 || flags==0x800?static_cast<std::uint16_t>(a(0)):static_cast<std::uint32_t>(static_cast<std::int32_t>(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(a(0)))));}
                        else if(name.ends_with("@G@Z"))op.count=static_cast<std::uint16_t>(a(0));else op.count=static_cast<std::uint32_t>(a(0));}
                    op.scratch=allocate_heap(memory,32);if(!op.scratch){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return {};}
                    memory.fill(GuestAddress{op.scratch},0,32);put(op.scratch+16+l.p,op.buffer);
                    const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
                }
                op.kind="ostream_number";op.count=a(0);
                if(!x86_ && (name.ends_with("@M@Z") || name.ends_with("@N@Z"))){op.count=0;for(unsigned i=0;i<(name.ends_with("@M@Z")?4U:8U);++i)op.count|=std::uint64_t{state.xmm[1][i]}<<(i*8);}
            }else if(method=="seekp" || method=="tellp"){
                op.kind=std::string(method);op.output=method=="tellp"?a(0):0;op.scratch=allocate_heap(memory,48);if(!op.scratch)throw std::bad_alloc{};
                if(method=="seekp"){std::array<std::uint8_t,24> position{};memory.read(GuestAddress{x86_?state.reg(Gpr::Rsp)+4:a(0)},position);memory.write(GuestAddress{op.scratch+24},position);}
            }else throw HostCallFault("未知 STL ostream 操作");
        }else if(method=="read"){
            op.kind="istream_read";op.output=a(0);op.limit=a(1);memory.store(GuestAddress{object+8},64,0);
        }else if(method=="tellg"){
            op.kind="tellg";op.output=a(0);op.scratch=allocate_heap(memory,24);if(!op.scratch)throw std::bad_alloc{};
        }else if(method=="seekg" && name.find("@_JH@Z")!=name.npos){
            op.kind="seekg_offset";op.output=a(0);op.limit=static_cast<std::uint32_t>(a(1));op.scratch=allocate_heap(memory,24);if(!op.scratch)throw std::bad_alloc{};
        }else if(method=="_Ipfx"){op.kind="prefix";op.skip=!a(0);}
        else if(name.starts_with("??5")){op.kind="number";op.output=a(0);op.floating=name.find("AAM@")!=name.npos || name.find("AEAM@")!=name.npos;op.unsigned_integer=name.find("AAI@")!=name.npos || name.find("AEAI@")!=name.npos;op.skip=true;}
        else if(method=="peek" || method=="ignore"){
            memory.store(GuestAddress{object+8},64,0);
            if(method=="ignore"){op.limit=a(0);if(op.limit>INT64_MAX)op.limit=0;op.output=a(1)&l.int_mask();}
        }
        else if(method=="getline"){op.output=a(0);op.limit=a(1);if(op.limit>INT64_MAX)op.limit=0;memory.store(GuestAddress{object+8},64,0);}
        else if(method=="seekg"){
            op.output=x86_?state.reg(Gpr::Rsp)+4:a(0);op.scratch=allocate_heap(memory,48);
            std::array<std::uint8_t,24> position{};memory.read(GuestAddress{op.output},position);memory.write(GuestAddress{op.scratch+24},position);
        }else throw HostCallFault("未知 VC10 istream 操作");
    }else throw HostCallFault("未知 VC10 STL 操作");
    const auto token=s.next++;s.operations.emplace(token,std::move(op));return resume_msvcp100(state,memory,token,0);
}

std::optional<std::uint32_t> WindowsServices::resume_msvcp100(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t value){
    auto& s=*msvcp100_;auto& op=s.operations.at(token);const Layout l(x86_,op.modern,op.element);const auto bits=static_cast<std::uint8_t>(l.p*8);
    const WindowsCall call(state,memory,op.service);
    const auto ptr=[&](std::uint64_t at){return memory.load(GuestAddress{at},bits);};
    const auto put=[&](std::uint64_t at,std::uint64_t v){memory.store(GuestAddress{at},bits,v);};
    const auto finish=[&](std::uint64_t result,bool wide=false)->std::optional<std::uint32_t>{
        // 所有輸出 sentry 共用收尾；_Osfx 本身沒有 sentry，也不重複鎖定或 flush tie。
        if(!op.suffix_done && op.ios && (op.kind.starts_with("ostream_") || op.kind=="seekp" || op.kind=="tellp")){
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
            GuestCallback cb{ptr(ptr(op.buffer)+2*l.p),{op.buffer}};cb.x86_register_arguments=x86_?1:0;cb.exception_policy=GuestExceptionPolicy::NoexceptCpp;
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
        else if(op.ios && op.kind!="num_put" && next!=10)cb.exception_policy=GuestExceptionPolicy::CaptureCpp;
        else if(op.kind=="global_locale" || op.kind=="copy_locale" || op.kind=="retire_facet" || (op.kind=="num_put" && next==21) || op.kind=="exception_destroy" || op.kind=="exception_release" || op.kind=="destroy_ios" || op.kind=="destroy_buffer" || op.kind=="destroy_locale_imp" || op.kind=="clone_locale_imp" || op.kind=="add_locale_facet")cb.exception_policy=GuestExceptionPolicy::NoexceptCpp;
        start_callbacks(state,memory,{std::move(cb)},Continuation::Msvcp100Operation,token);return {};
    };
    const auto virt=[&](unsigned slot,std::vector<std::uint64_t> args,unsigned next){args.insert(args.begin(),op.buffer);
        return callback(ptr(ptr(op.buffer)+slot*l.p),std::move(args),next,true,slot==8 || slot==9 || slot==10 || slot==12?4U:0U,slot==5 || slot==8 || slot==9);};
    const auto count_address=[&](bool output){return ptr(op.buffer+l.count_indirect(output));};
    const auto position_address=[&](bool output){return ptr(op.buffer+l.next_indirect(output));};
    const auto available=[&](bool output){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{count_address(output)},32)));};
    const auto consume=[&](bool output){const auto pos=position_address(output),count=count_address(output);put(pos,ptr(pos)+l.element);memory.store(GuestAddress{count},32,memory.load(GuestAddress{count},32)-1);};
    const auto setstate=[&](unsigned flags){const auto address=op.ios+l.state,value=memory.load(GuestAddress{address},32)|flags;memory.store(GuestAddress{address},32,value);op.throw_failure|=(value&memory.load(GuestAddress{address+4},32))!=0;};
    const auto returned=op.callback;op.callback=0;
    // _Osfx 的同步失敗會設定 badbit，但其 noexcept 邊界吞掉例外遮罩觸發的 failure。
    if(returned==52 && static_cast<std::uint32_t>(value)==UINT32_MAX){const auto at=op.ios+l.state;memory.store(GuestAddress{at},32,memory.load(GuestAddress{at},32)|4U);}
    if(op.completed)return finish(op.final_result,op.final_wide);
    if(op.kind=="ostream_suffix")return finish(0);
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
        if(returned==40){op.locale_lock_owned=true;op.locale_source=s.global14;op.phase=2;return callback(ptr(ptr(op.locale_source)+l.p),{op.locale_source},41);}
        if(returned==41){op.locale_source_owned=true;
            if(op.object==op.locale_source)op.phase=5;
            else{op.phase=3;return callback(ptr(ptr(op.object)+l.p),{op.object},42);}
        }
        if(returned==42){op.locale_facet_owned=true;op.locale_pending=op.object;s.global14=op.object;op.locale_facet_owned=false;op.locale_pending=0;
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
    if(op.kind=="num_put"){
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
            const auto base=op.objects[static_cast<std::size_t>(op.count++)],loc=ptr(base+l.locale);if(l.modern)put(base,s.ios_base_table14);put(base+l.locale,0);s.ios_objects.erase(base);
            if(!loc)continue;const auto imp=ptr(loc+l.locale_pointer());if(!free_heap(memory,loc,state))throw HostCallFault("STL locale 配置無效");
            if(l.modern)return callback(ptr(ptr(imp)+2*l.p),{imp},1);
            const auto n=memory.load(GuestAddress{imp+l.p},l.reference_bits());if(n)memory.store(GuestAddress{imp+l.p},l.reference_bits(),n-1);
        }
        if(op.output&1U){if(!free_heap(memory,op.object-((op.output&2U)?l.p:0),state))throw HostCallFault("STL ios 配置無效");}return finish(op.final_result);
    }
    if(op.kind=="locinfo_construct" || op.kind=="locinfo_destroy"){
        const auto yarns=op.object+(x86_?4U:8U);const auto yarn_size=2*l.p;const auto old=yarns+4*yarn_size,new_name=old+yarn_size;
        const auto family="msvcp140.dll";
        if(op.kind=="locinfo_construct"){
            if(!op.phase){op.phase=1;return callback(resolve(family,x86_?"??0_Lockit@std@@QAE@H@Z":"??0_Lockit@std@@QEAA@H@Z").value,{op.object,0},1);}
            if(op.phase==1){op.phase=2;return callback(resolve("crt","_wsetlocale").value,{0,0},2,false);}
            if(op.phase==2){put(old,value?crt_duplicate_string(memory,value,true):0);op.phase=3;return callback(resolve("crt","setlocale").value,{0,op.buffer},3,false);}
            if(value)put(new_name,crt_duplicate_string(memory,value));
            else{const auto star=allocate_heap(memory,2);if(star){memory.store(GuestAddress{star},16,'*');put(new_name,star);}}
            return finish(op.object);
        }
        if(!op.phase){op.phase=1;return callback(resolve("crt","_wsetlocale").value,{0,ptr(old)?ptr(old):old+l.p},1,false);}
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
                copy=allocate_heap(memory,x86_?32:56);if(!copy)throw std::bad_alloc{};memory.move(GuestAddress{copy},GuestAddress{source},x86_?32:56);
                if(count){array=allocate_heap(memory,count*l.p);if(!array)throw std::bad_alloc{};memory.move(GuestAddress{array},GuestAddress{facets},static_cast<std::size_t>(count*l.p));}
                if(const auto name=ptr(source+4*l.p+8)){label=crt_duplicate_string(memory,name);if(!label)throw std::bad_alloc{};}
                put(copy,s.locimp_table14);memory.store(GuestAddress{copy+l.p},l.reference_bits(),1);put(copy+2*l.p,array);put(copy+4*l.p+8,label);
                s.owned_locale_imps.insert(copy);op.object=copy;op.output=array;op.limit=count;op.phase=2;
            }catch(const std::bad_alloc&){
                s.owned_locale_imps.erase(copy);if(label)(void)free_heap(memory,label,state);if(array)(void)free_heap(memory,array,state);if(copy)(void)free_heap(memory,copy,state);
                op.allocation_failure=true;op.phase=3;
            }catch(...){if(label)(void)free_heap(memory,label,state);if(array)(void)free_heap(memory,array,state);if(copy)(void)free_heap(memory,copy,state);throw;}
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
    if(op.ios && (op.kind=="number" || op.kind=="getline" || op.kind=="peek" || op.kind=="ignore" || op.kind=="istream_read" || op.kind=="tellg" || op.kind=="seekg_offset" || op.kind=="seekg" || op.kind=="seekp" || op.kind=="tellp" || op.kind.starts_with("ostream_")) && !op.lock_started && op.buffer){
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
    if(op.kind=="sputn")return returned?finish(value,true):virt(9,{op.output,op.limit},1);
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
    const bool prefix=op.kind=="prefix",number=op.kind=="number",line=op.kind=="getline";
    if(!op.started){
        op.started=true;
        if(op.kind=="seekg_offset" || op.kind=="seekg"){const auto at=op.ios+l.state;memory.store(GuestAddress{at},32,memory.load(GuestAddress{at},32)&~1U);}
        const auto initial_state=memory.load(GuestAddress{op.ios+l.state},32);
        const bool output_position=op.kind=="seekp" || op.kind=="tellp";
        if((output_position?(initial_state&6U):initial_state) || !op.buffer){
            if(output_position){
                if(op.kind=="tellp"){memory.fill(GuestAddress{op.output},0,24);memory.store(GuestAddress{op.output},64,UINT64_MAX);return finish(op.output);}
                return finish(op.object);
            }
            if(op.kind=="ostream_suffix")return finish(0);
            if(op.kind=="ostream_flush")return finish(op.object);
            if(op.kind.starts_with("ostream_")){setstate(4);return finish(op.object);}
            setstate(2);if(op.kind=="tellg"){memory.write(GuestAddress{op.output},std::array<std::uint8_t,24>{});memory.store(GuestAddress{op.output},64,UINT64_MAX);return finish(op.output);}
            return finish(prefix?0:op.kind=="peek"?l.eof():op.object);}
        op.skip=op.skip && (memory.load(GuestAddress{op.ios+l.flags},32)&1U);
        if(const auto tied=ptr(op.ios+l.ios_size+l.p);tied && tied!=op.object && !initial_state){
            op.tie_ios=tied+static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{ptr(tied)+4},32))));const auto tied_buffer=ptr(op.tie_ios+l.ios_size);
            if(l.modern){
                const std::string character=l.element==1?"D":"_W";
                const auto name="?flush@?$basic_ostream@"+character+"U?$char_traits@"+character+"@std@@@std@@"+(x86_?"QAEAAV":"QEAAAEAV")+"12@XZ";
                return callback(resolve("msvcp140.dll",name).value,{tied},11);
            }
            if(tied_buffer && !memory.load(GuestAddress{op.tie_ios+l.state},32))return callback(ptr(ptr(tied_buffer)+13*l.p),{tied_buffer},11);
        }
    }
    if(op.kind=="seekg"){
        if(returned==1){if(memory.load(GuestAddress{op.scratch},64)==UINT64_MAX && !memory.load(GuestAddress{op.scratch+8},64))setstate(2);return finish(op.object);}
        if(memory.load(GuestAddress{op.ios+l.state},32)&6U)return finish(op.object);
        std::vector<std::uint64_t> args{op.scratch};
        if(x86_)for(unsigned i=0;i<6;++i)args.push_back(memory.load(GuestAddress{op.scratch+24+4*i},32));else args.push_back(op.scratch+24);
        args.push_back(1);return virt(11,std::move(args),1);
    }
    if(op.kind=="seekp" || op.kind=="tellp"){
        const bool query=op.kind=="tellp";
        if(returned==1){if(query){memory.move(GuestAddress{op.output},GuestAddress{op.scratch},24);return finish(op.output);}
            if(memory.load(GuestAddress{op.scratch},64)+memory.load(GuestAddress{op.scratch+8},64)==UINT64_MAX)setstate(2);return finish(op.object);}
        if(memory.load(GuestAddress{op.ios+l.state},32)&6U || !op.buffer){if(query){memory.write(GuestAddress{op.output},std::array<std::uint8_t,24>{});memory.store(GuestAddress{op.output},64,UINT64_MAX);}return finish(query?op.output:op.object);}
        if(query)return virt(10,{op.scratch,0,1,2},1);
        std::vector<std::uint64_t> args{op.scratch};if(x86_)for(unsigned i=0;i<6;++i)args.push_back(memory.load(GuestAddress{op.scratch+24+i*4},32));else args.push_back(op.scratch+24);
        args.push_back(2);return virt(11,std::move(args),1);
    }
    if(op.kind.starts_with("ostream_")){
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
        if(returned==1){const auto count=value<=op.limit?value:0;memory.store(GuestAddress{op.object+8},64,count);if(count!=op.limit)setstate(3);return finish(op.object);}
        if(!op.limit || op.limit>INT64_MAX)return finish(op.object);return virt(8,{op.output,op.limit},1);
    }
    if(op.kind=="tellg" || op.kind=="seekg_offset"){
        const bool query=op.kind=="tellg";
        if(returned==1){if(query){memory.move(GuestAddress{op.output},GuestAddress{op.scratch},24);return finish(op.output);}
            if(memory.load(GuestAddress{op.scratch},64)+memory.load(GuestAddress{op.scratch+8},64)==UINT64_MAX)setstate(2);return finish(op.object);}
        return virt(10,{op.scratch,query?0:op.output,query?1:op.limit,1},1);
    }
    if(op.kind=="peek"){
        const auto c=returned==21?value&l.int_mask():available(false)>0?memory.load(GuestAddress{ptr(position_address(false))},l.character_bits()):UINT64_MAX;
        if(c==UINT64_MAX)return virt(6,{},21);
        if(c==l.eof())setstate(1);return finish(c);
    }
    if(op.kind=="ignore"){
        if(returned==20){
            if((value&l.int_mask())==l.eof()){setstate(1);return finish(op.object);}
            ++op.count;memory.store(GuestAddress{op.object+8},64,op.count);
            if((value&l.int_mask())==op.output)return finish(op.object);
        }
        while(op.limit==INT64_MAX || op.count<op.limit){
            if(available(false)<=0)return virt(7,{},20);
            const auto c=memory.load(GuestAddress{ptr(position_address(false))},l.character_bits());consume(false);
            ++op.count;memory.store(GuestAddress{op.object+8},64,op.count);
            if(c==op.output)return finish(op.object);
        }
        return finish(op.object);
    }
    const auto complete_input=[&](bool eof)->std::optional<std::uint32_t>{
        if(eof)setstate(1);
        if(prefix){if(eof)setstate(2);return finish(eof?0:1);}
        if(line){if(op.limit)memory.store(GuestAddress{op.output+op.count*l.element},l.character_bits(),0);if(!op.count)setstate(2);return finish(op.object);}
        if(number){
            if(op.text.empty()){setstate(2);memory.store(GuestAddress{op.output},32,0);return finish(op.object);}
            auto text=std::string_view(op.text);if(text.front()=='+')text.remove_prefix(1);
            if(op.floating){
                static const auto locale=newlocale(LC_NUMERIC_MASK,"C",nullptr);if(!locale)throw std::bad_alloc();
                const std::string terminated(text);char* end=nullptr;const auto saved_errno=errno;errno=0;
                auto f=strtof_l(terminated.c_str(),&end,locale);const auto error=errno;errno=saved_errno;
                if(!std::isfinite(f))f=text.starts_with('-')?-std::numeric_limits<float>::max():std::numeric_limits<float>::max();
                if(error==ERANGE || end!=terminated.c_str()+terminated.size())setstate(2);memory.store(GuestAddress{op.output},32,std::bit_cast<std::uint32_t>(f));
            }else {
                const auto flags=memory.load(GuestAddress{op.ios+l.flags},32)&0xe00U;const bool negative=text.starts_with('-');if(negative)text.remove_prefix(1);
                int base=flags==0x800?16:flags==0x400?8:10;
                if((flags==0 || base==16) && (text.starts_with("0x") || text.starts_with("0X"))){base=16;text.remove_prefix(2);}else if(!flags && text.starts_with('0'))base=8;
                std::uint64_t magnitude{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),magnitude,base);
                const auto maximum=op.unsigned_integer?UINT64_C(4294967295):negative?UINT64_C(2147483648):UINT64_C(2147483647);
                const bool overflow=parsed.ec==std::errc::result_out_of_range || magnitude>maximum;
                if(overflow){magnitude=maximum;setstate(2);}
                else if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())setstate(2);
                const auto n=negative && !(op.unsigned_integer && overflow)?std::uint32_t{0}-static_cast<std::uint32_t>(magnitude):static_cast<std::uint32_t>(magnitude);memory.store(GuestAddress{op.output},32,n);
            }
            return finish(op.object);
        }
        throw HostCallFault("VC10 輸入狀態無效");
    };
    if(prefix && !op.skip)return complete_input(false);
    if(returned==2){if((value&l.int_mask())==l.eof())return complete_input(true);}
    bool have=returned==1;
    for(;;){
        std::uint64_t c=0;
        if(have){c=value&l.int_mask();have=false;}
        else if(available(false)>0)c=memory.load(GuestAddress{ptr(position_address(false))},l.character_bits());
        else return virt(6,{},1);
        if(c==l.eof())return complete_input(true);
        if(op.skip && (character_mask(static_cast<char16_t>(c))&8U)){}
        else{
            op.skip=false;if(prefix)return complete_input(false);
            if(line){
                if(c=='\n'){
                    memory.store(GuestAddress{op.object+8},64,op.count+1);
                    if(available(false)>0){consume(false);if(op.limit)memory.store(GuestAddress{op.output+op.count*l.element},l.character_bits(),0);return finish(op.object);}
                    op.kind="getline_delimiter";return virt(7,{},3);
                }
                if(!op.limit || op.count+1>=op.limit){setstate(2);return complete_input(false);}
                memory.store(GuestAddress{op.output+op.count*l.element},l.character_bits(),c);++op.count;memory.store(GuestAddress{op.object+8},64,op.count);
            }else if(number){
                const bool digit=c>='0' && c<='9';const bool sign=(c=='+' || c=='-') && (op.text.empty() || (op.floating && (op.text.back()=='e' || op.text.back()=='E')));
                const auto flags=memory.load(GuestAddress{op.ios+l.flags},32)&0xe00U;
                const auto unsigned_text=std::string_view(op.text).substr(!op.text.empty() && (op.text.front()=='+' || op.text.front()=='-')?1:0);
                const bool hex=flags==0x800 || (!flags && (unsigned_text.starts_with("0x") || unsigned_text.starts_with("0X")));
                const bool extra=op.floating?
                    ((c=='.' && op.text.find_first_of(".eE")==std::string::npos) || ((c=='e' || c=='E') && op.text.find_first_of("eE")==std::string::npos && op.text.find_first_of("0123456789")!=std::string::npos)):
                    ((hex && ((c>='a' && c<='f') || (c>='A' && c<='F'))) || ((flags==0 || flags==0x800) && unsigned_text=="0" && (c=='x' || c=='X')));
                if(!digit && !sign && !extra)return complete_input(false);
                if(op.text.size()>=4096)throw HostCallFault("VC10 數字文字超出上限");op.text.push_back(static_cast<char>(c));
            }
        }
        if(available(false)>0)consume(false);else return virt(7,{},2);
    }
}
}
