// oleaut32 標準字型物件（StdFont，OleCreateFontIndirect）：一個宿主物件提供 IFont、IFontDisp（IDispatch）、IPersistStream、
// IConnectionPointContainer 與兩個連接點（IPropertyNotifySink、IFontEventsDisp）。
// - 屬性：Name、Size（CY，點數×10000）、Bold（Weight > 550；設定時 Weight 為 700／400）、Italic、Underline、Strikethrough、Weight、Charset。
//   變更後呼叫每個 IPropertyNotifySink::OnChanged(dispid) 與 IFontEventsDisp 的 FontChanged（DISPID 9，參數為屬性名稱）。
// - get_hFont 依目前屬性以 CreateFontIndirectW 建立（屬性或 SetRatio 改變後重建），高度＝−點數×2540／72×cyLogical／cyHimetric，
//   預設比例 72／2540 即 −點數。AddRefHfont／ReleaseHfont 管理外部參照：屬性變更或物件釋放時，沒有外部參照的舊 HFONT 立即刪除。
// - IPersistStream 版面：版本（1）、Charset（WORD）、旗標（BYTE：0x02 斜體、0x04 底線、0x08 刪除線）、Weight（WORD）、
//   Size（DWORD，點數×10000）、名稱長度（BYTE）與 ANSI 名稱；CLSID_StdFont。
// - IFontDisp：Name（0）、Size（2）、Bold（3）、Italic（4）、Underline（5）、Strikethrough（6）、Weight（7）、Charset（8）屬性；沒有型別程式庫。
// - IConnectionPoint::EnumConnections 不提供列舉（E_NOTIMPL，介面規格允許）；EnumConnectionPoints 回傳兩個連接點的列舉器。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>
#include <bit>

namespace x64arm {
namespace {
using Guid=std::array<std::uint8_t,16>;
constexpr Guid make_guid(std::uint32_t a,std::uint16_t b,std::uint16_t c,std::array<std::uint8_t,8> d){
    Guid out{};for(unsigned i=0;i<4;++i)out[i]=static_cast<std::uint8_t>(a>>(8*i));
    for(unsigned i=0;i<2;++i){out[4+i]=static_cast<std::uint8_t>(b>>(8*i));out[6+i]=static_cast<std::uint8_t>(c>>(8*i));}
    for(unsigned i=0;i<8;++i)out[8+i]=d[i];return out;
}
constexpr auto iid_unknown=make_guid(0,0,0,{0xc0,0,0,0,0,0,0,0x46}),iid_dispatch=make_guid(0x00020400,0,0,{0xc0,0,0,0,0,0,0,0x46});
constexpr auto iid_persist=make_guid(0x0000010c,0,0,{0xc0,0,0,0,0,0,0,0x46}),iid_persist_stream=make_guid(0x00000109,0,0,{0xc0,0,0,0,0,0,0,0x46});
constexpr auto iid_font=make_guid(0xbef6e002,0xa874,0x101a,{0x8b,0xba,0x00,0xaa,0x00,0x30,0x0c,0xab});
constexpr auto iid_font_disp=make_guid(0xbef6e003,0xa874,0x101a,{0x8b,0xba,0x00,0xaa,0x00,0x30,0x0c,0xab});
constexpr auto iid_font_events=make_guid(0x4ef6100a,0xaf88,0x11d0,{0x98,0x46,0x00,0xc0,0x4f,0xc2,0x99,0x93});
constexpr auto iid_property_sink=make_guid(0x9bfbbc02,0xeff1,0x101a,{0x84,0xed,0x00,0xaa,0x00,0x34,0x1d,0x07});
constexpr auto iid_container=make_guid(0xb196b284,0xbab4,0x101a,{0xb6,0x9c,0x00,0xaa,0x00,0x34,0x1d,0x07});
constexpr auto iid_enum_points=make_guid(0xb196b285,0xbab4,0x101a,{0xb6,0x9c,0x00,0xaa,0x00,0x34,0x1d,0x07});
constexpr auto iid_point=make_guid(0xb196b286,0xbab4,0x101a,{0xb6,0x9c,0x00,0xaa,0x00,0x34,0x1d,0x07});
constexpr auto clsid_std_font=make_guid(0x0be35203,0x8f91,0x11ce,{0x9d,0xe3,0x00,0xaa,0x00,0x4b,0xb8,0x51});
constexpr std::uint32_t s_ok=0,s_false=1,e_notimpl=0x80004001U,e_pointer=0x80004003U,e_nointerface=0x80004002U,e_invalidarg=0x80070057U,e_outofmemory=0x8007000eU,
    e_fail=0x80004005U,e_unexpected=0x8000ffffU,ctl_invalid_value=0x800a017cU,connect_no_connection=0x80040200U,connect_cannot_connect=0x80040202U,
    disp_member_not_found=0x80020003U,disp_type_mismatch=0x80020005U,disp_unknown_name=0x80020006U,disp_bad_index=0x8002000bU,disp_bad_param_count=0x8002000eU;
std::int32_t long_arg(std::uint64_t v){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(v));}
std::u16string folded(std::u16string text){for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return text;}
constexpr std::array<std::u16string_view,9> property_names{u"Name",u"",u"Size",u"Bold",u"Italic",u"Underline",u"Strikethrough",u"Weight",u"Charset"};
}
std::span<const WindowsServiceDescriptor> ole_font_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"oleaut32.dll","OleCreateFontIndirect",3},
        {"internal-oleaut32-font","QueryInterface",3},{"internal-oleaut32-font","AddRef",1},{"internal-oleaut32-font","Release",1},
        {"internal-oleaut32-font","get_Name",2},{"internal-oleaut32-font","put_Name",2},{"internal-oleaut32-font","get_Size",2},{"internal-oleaut32-font","put_Size",2,2},
        {"internal-oleaut32-font","get_Bold",2},{"internal-oleaut32-font","put_Bold",2,0,0,0x2},{"internal-oleaut32-font","get_Italic",2},{"internal-oleaut32-font","put_Italic",2,0,0,0x2},
        {"internal-oleaut32-font","get_Underline",2},{"internal-oleaut32-font","put_Underline",2,0,0,0x2},{"internal-oleaut32-font","get_Strikethrough",2},
        {"internal-oleaut32-font","put_Strikethrough",2,0,0,0x2},{"internal-oleaut32-font","get_Weight",2},{"internal-oleaut32-font","put_Weight",2,0,0,0x2},
        {"internal-oleaut32-font","get_Charset",2},{"internal-oleaut32-font","put_Charset",2,0,0,0x2},{"internal-oleaut32-font","get_hFont",2},
        {"internal-oleaut32-font","Clone",2},{"internal-oleaut32-font","IsEqual",2},{"internal-oleaut32-font","SetRatio",3,0,0,0x6},
        {"internal-oleaut32-font","QueryTextMetrics",2},{"internal-oleaut32-font","AddRefHfont",2},{"internal-oleaut32-font","ReleaseHfont",2},{"internal-oleaut32-font","SetHdc",2},
        {"internal-oleaut32-font-dispatch","QueryInterface",3},{"internal-oleaut32-font-dispatch","AddRef",1},{"internal-oleaut32-font-dispatch","Release",1},
        {"internal-oleaut32-font-dispatch","GetTypeInfoCount",2},{"internal-oleaut32-font-dispatch","GetTypeInfo",4,0,0,0x6},
        {"internal-oleaut32-font-dispatch","GetIDsOfNames",6,0,0,0x18},{"internal-oleaut32-font-dispatch","Invoke",9,0,0,0x1a},
        {"internal-oleaut32-font-persist","QueryInterface",3},{"internal-oleaut32-font-persist","AddRef",1},{"internal-oleaut32-font-persist","Release",1},
        {"internal-oleaut32-font-persist","GetClassID",2},{"internal-oleaut32-font-persist","IsDirty",1},{"internal-oleaut32-font-persist","Load",2},
        {"internal-oleaut32-font-persist","Save",3,0,0,0x4},{"internal-oleaut32-font-persist","GetSizeMax",2},
        {"internal-oleaut32-font-container","QueryInterface",3},{"internal-oleaut32-font-container","AddRef",1},{"internal-oleaut32-font-container","Release",1},
        {"internal-oleaut32-font-container","EnumConnectionPoints",2},{"internal-oleaut32-font-container","FindConnectionPoint",3},
        {"internal-oleaut32-font-point","QueryInterface",3},{"internal-oleaut32-font-point","AddRef",1},{"internal-oleaut32-font-point","Release",1},
        {"internal-oleaut32-font-point","GetConnectionInterface",2},{"internal-oleaut32-font-point","GetConnectionPointContainer",2},
        {"internal-oleaut32-font-point","Advise",3},{"internal-oleaut32-font-point","Unadvise",2,0,0,0x2},{"internal-oleaut32-font-point","EnumConnections",2},
        {"internal-oleaut32-font-enum","QueryInterface",3},{"internal-oleaut32-font-enum","AddRef",1},{"internal-oleaut32-font-enum","Release",1},
        {"internal-oleaut32-font-enum","Next",4,0,0,0x2},{"internal-oleaut32-font-enum","Skip",2,0,0,0x2},{"internal-oleaut32-font-enum","Reset",1},
        {"internal-oleaut32-font-enum","Clone",2}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_ole_font(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"oleaut32 字型",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        using service_callback::Task;
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto family=service.family;const auto name=service.symbol;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto api=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve(dll,symbol).value,std::move(arguments));};
        const auto method=[&](std::uint64_t object,unsigned index){return m.load(GuestAddress{m.load(GuestAddress{object},bits)+std::uint64_t{index}*unit},bits);};
        const auto com=[&](std::uint64_t object,unsigned index,std::vector<std::uint64_t> arguments)->Task<std::uint64_t>{
            arguments.insert(arguments.begin(),object);co_return co_await service_callback::call_value(method(object,index),std::move(arguments));};
        const auto load_guid=[&](std::uint64_t at){Guid v{};m.read(GuestAddress{at},v);return v;};
        const auto font=[&](std::uint64_t base)->OleFont*{const auto found=ole_fonts_.find(base);return found==ole_fonts_.end()?nullptr:&found->second;};
        const auto vtable=[&](unsigned which){
            auto& table=ole_font_vtables_[which];if(table)return table;
            static constexpr std::array<std::string_view,6> families{"internal-oleaut32-font","internal-oleaut32-font-dispatch","internal-oleaut32-font-persist",
                "internal-oleaut32-font-container","internal-oleaut32-font-point","internal-oleaut32-font-enum"};
            std::vector<std::uint64_t> entries;for(const auto& d:ole_font_service_descriptors())if(d.family==families[which])entries.push_back(resolve(d.family,d.symbol).value);
            table=runtime_allocate(m,{},entries.size()*unit);for(std::size_t i=0;i<entries.size();++i)m.store(GuestAddress{table+i*unit},bits,entries[i]);
            return table;
        };
        // 物件版面：IFont、IFontDisp、IPersistStream、IConnectionPointContainer、兩個 IConnectionPoint（屬性通知、字型事件）。
        const auto create=[&](OleFont value)->std::uint64_t{
            if(ole_fonts_.size()>=65536)throw std::bad_alloc{};
            const auto object=allocate_heap(m,6*unit);if(!object)throw std::bad_alloc{};
            for(unsigned i=0;i<6;++i)m.store(GuestAddress{object+i*unit},bits,vtable(std::min(i,4U)));
            value.references=1;value.sinks.clear();value.next_cookie=1;value.font=0;ole_fonts_.emplace(object,std::move(value));return object;
        };
        const auto interface_of=[&](std::uint64_t base,const Guid& iid)->std::uint64_t{
            if(iid==iid_unknown || iid==iid_font)return base;
            if(iid==iid_font_disp || iid==iid_dispatch)return base+unit;
            if(iid==iid_persist || iid==iid_persist_stream)return base+2*unit;
            if(iid==iid_container)return base+3*unit;
            return 0;
        };
        // 不再是任何字型目前 HFONT 且沒有外部參照的 HFONT 刪除。
        const auto drop_font=[&](std::uint64_t handle)->Task<std::uint32_t>{
            if(!handle)co_return 0;
            if(const auto found=ole_font_handles_.find(handle);found!=ole_font_handles_.end() && found->second)co_return 0;
            if(std::any_of(ole_fonts_.begin(),ole_fonts_.end(),[&](const auto& item){return item.second.font==handle;}))co_return 0;
            ole_font_handles_.erase(handle);(void)co_await api("gdi32.dll","DeleteObject",{handle});co_return 0;
        };
        const auto release_object=[&](std::uint64_t base)->Task<std::uint32_t>{
            auto* f=font(base);if(!f)co_return 0;if(f->references>1)co_return --f->references;
            const auto sinks=f->sinks;const auto handle=f->font;ole_fonts_.erase(base);
            for(const auto& [cookie,sink]:sinks){(void)cookie;(void)co_await com(sink.second,2,{});}
            (void)co_await drop_font(handle);(void)free_heap(m,base,s);co_return 0;
        };
        // 屬性改變：舊 HFONT 失效（下次 get_hFont 重建），通知兩種連接點。
        const auto changed=[&](std::uint64_t base,std::int32_t dispid)->Task<std::uint32_t>{
            auto* f=font(base);if(!f)co_return 0;f->dirty=true;const auto old=f->font;f->font=0;(void)co_await drop_font(old);
            if(!(f=font(base)))co_return 0;const auto sinks=f->sinks;
            for(const auto& [cookie,sink]:sinks){
                (void)cookie;
                if(!sink.first){(void)co_await com(sink.second,3,{static_cast<std::uint32_t>(dispid)});continue;}
                // IFontEventsDisp::FontChanged(BSTR name)：DISPID 9、DISPATCH_METHOD，DISPPARAMS 一個 VT_BSTR 參數。
                const auto text=utf16_bytes(property_names[static_cast<std::size_t>(dispid)]);
                const auto bstr=co_await api("oleaut32.dll","SysAllocString",{[&]{const auto b=scratch(text.size());m.write(GuestAddress{b},text);return b;}()});
                const unsigned variant=x86_?16U:24U;const auto arguments=scratch(variant),params=scratch(2*unit+8),iid_null=scratch(16);
                m.store(GuestAddress{arguments},16,8);m.store(GuestAddress{arguments+8},bits,bstr);
                m.store(GuestAddress{params},bits,arguments);m.store(GuestAddress{params+2*unit},32,1);
                (void)co_await com(sink.second,6,{9,iid_null,0,1,params,0,0,0});
                (void)co_await api("oleaut32.dll","SysFreeString",{bstr});
            }
            co_return 0;
        };
        const auto realize=[&](std::uint64_t base)->Task<std::uint64_t>{
            auto* f=font(base);if(!f)co_return 0;if(f->font)co_return f->font;
            // LOGFONTW：高度依比例換算，其他欄位對應屬性。
            const auto numerator=f->size*2540*f->ratio_logical,denominator=std::int64_t{720000}*f->ratio_himetric;
            const auto height=denominator?-((numerator+denominator/2)/denominator):0;
            const auto logfont=scratch(92);m.store(GuestAddress{logfont},32,static_cast<std::uint32_t>(static_cast<std::int32_t>(height)));
            m.store(GuestAddress{logfont+16},32,static_cast<std::uint32_t>(f->weight));m.store(GuestAddress{logfont+20},8,f->italic?1:0);m.store(GuestAddress{logfont+21},8,f->underline?1:0);
            m.store(GuestAddress{logfont+22},8,f->strike?1:0);m.store(GuestAddress{logfont+23},8,static_cast<std::uint8_t>(f->charset));
            auto face=f->name;face.resize(std::min<std::size_t>(face.size(),31));for(std::size_t i=0;i<face.size();++i)m.store(GuestAddress{logfont+28+i*2},16,face[i]);
            const auto handle=co_await api("gdi32.dll","CreateFontIndirectW",{logfont});
            if((f=font(base)))f->font=handle&(x86_?UINT32_MAX:UINT64_MAX);co_return f?f->font:0;
        };
        const auto ansi_to_wide=[&](std::span<const std::uint8_t> text)->Task<std::u16string>{
            if(text.empty())co_return std::u16string{};const auto source=scratch(text.size());m.write(GuestAddress{source},text);const auto target=scratch(text.size()*2+2);
            const auto count=static_cast<std::uint32_t>(co_await api("kernel32.dll","MultiByteToWideChar",{0,0,source,text.size(),target,text.size()+1}));
            co_return read_utf16_counted(m,GuestAddress{target},count);
        };
        const auto wide_to_ansi=[&](const std::u16string& text)->Task<std::vector<std::uint8_t>>{
            if(text.empty())co_return std::vector<std::uint8_t>{};const auto bytes=utf16_bytes(text);const auto source=scratch(bytes.size());m.write(GuestAddress{source},bytes);
            const auto target=scratch(text.size()*4+4);
            const auto count=static_cast<std::uint32_t>(co_await api("kernel32.dll","WideCharToMultiByte",{0,0,source,text.size(),target,text.size()*4+4,0,0}));
            std::vector<std::uint8_t> out(count);if(count)m.read(GuestAddress{target},out);co_return out;
        };
        try{
            if(family=="oleaut32.dll"){
                // OleCreateFontIndirect(LPFONTDESC, riid, ppvObj)：FONTDESC＝cbSizeofstruct、lpstrName、cySize（CY）、sWeight、sCharset、fItalic、fUnderline、fStrikethrough。
                if(!call(2))co_return finish(e_pointer);m.store(GuestAddress{call(2)},bits,0);if(!call(0) || !call(1))co_return finish(e_invalidarg);
                const auto desc=call(0);const unsigned name_at=x86_?4U:8U,size_at=x86_?8U:16U,weight_at=size_at+8;
                OleFont value;const auto text=m.load(GuestAddress{desc+name_at},bits);if(text)value.name=read_utf16(m,GuestAddress{text},256);
                value.size=static_cast<std::int64_t>(m.load(GuestAddress{desc+size_at},64));value.weight=static_cast<std::int16_t>(m.load(GuestAddress{desc+weight_at},16));
                value.charset=static_cast<std::int16_t>(m.load(GuestAddress{desc+weight_at+2},16));value.italic=m.load(GuestAddress{desc+weight_at+4},32)!=0;
                value.underline=m.load(GuestAddress{desc+weight_at+8},32)!=0;value.strike=m.load(GuestAddress{desc+weight_at+12},32)!=0;
                const auto object=create(std::move(value));const auto out=interface_of(object,load_guid(call(1)));
                if(!out){(void)co_await release_object(object);co_return finish(e_nointerface);}
                m.store(GuestAddress{call(2)},bits,out);co_return finish(s_ok);
            }
            const auto self=call(0);
            if(family=="internal-oleaut32-font-enum"){
                // IEnumConnectionPoints：{字型, 位置}；兩個連接點依序列出。
                const auto found=ole_font_enums_.find(self);if(found==ole_font_enums_.end())co_return finish(e_unexpected);
                auto& [owner,position]=found->second;
                if(name=="QueryInterface"){
                    if(!call(2))co_return finish(e_pointer);m.store(GuestAddress{call(2)},bits,0);const auto iid=load_guid(call(1));
                    if(iid!=iid_unknown && iid!=iid_enum_points)co_return finish(e_nointerface);++ole_font_enum_references_[self];m.store(GuestAddress{call(2)},bits,self);co_return finish(s_ok);
                }
                if(name=="AddRef")co_return finish(++ole_font_enum_references_[self]);
                if(name=="Release"){
                    auto& count=ole_font_enum_references_[self];if(count>1)co_return finish(--count);
                    const auto base=owner;ole_font_enums_.erase(self);ole_font_enum_references_.erase(self);(void)free_heap(m,self,s);(void)co_await release_object(base);co_return finish(0);
                }
                if(name=="Next"){
                    const auto count=static_cast<std::uint32_t>(call(1));if(!call(2) || (count>1 && !call(3)))co_return finish(e_pointer);
                    std::uint32_t fetched=0;const auto base=owner;
                    while(fetched<count && position<2){if(auto* f=font(base))++f->references;m.store(GuestAddress{call(2)+fetched*unit},bits,base+(4+position)*unit);++position;++fetched;}
                    if(call(3))m.store(GuestAddress{call(3)},32,fetched);co_return finish(fetched==count?s_ok:s_false);
                }
                if(name=="Skip"){const auto count=static_cast<std::uint32_t>(call(1));const auto moved=std::min<std::uint32_t>(count,2-position);position+=moved;co_return finish(moved==count?s_ok:s_false);}
                if(name=="Reset"){position=0;co_return finish(s_ok);}
                if(name=="Clone"){
                    if(!call(1))co_return finish(e_pointer);const auto copy=allocate_heap(m,unit);if(!copy)co_return finish(e_outofmemory);
                    m.store(GuestAddress{copy},bits,vtable(5));const auto base=owner;const auto at=position;if(auto* f=font(base))++f->references;
                    ole_font_enums_.emplace(copy,std::pair{base,at});ole_font_enum_references_[copy]=1;m.store(GuestAddress{call(1)},bits,copy);co_return finish(s_ok);
                }
                co_return finish(e_notimpl);
            }
            // 介面方法：由 vtable 指標判定介面位移後找回物件；兩個連接點共用 vtable，以位移區分。
            const auto table=m.load(GuestAddress{self},bits);std::uint64_t base=0;unsigned slot=0;
            for(unsigned i=0;i<5 && !base;++i)if(table==ole_font_vtables_[i]){
                if(i<4){base=self-i*unit;slot=i;}
                else for(unsigned point=4;point<6 && !base;++point)if(font(self-point*unit)){base=self-point*unit;slot=point;}
            }
            if(!base || !font(base))co_return finish(e_unexpected);
            if(name=="QueryInterface"){
                if(!call(2))co_return finish(e_pointer);m.store(GuestAddress{call(2)},bits,0);if(!call(1))co_return finish(e_invalidarg);
                const auto iid=load_guid(call(1));std::uint64_t pointer=0;
                if(slot>=4)pointer=iid==iid_unknown || iid==iid_point?self:0;else pointer=interface_of(base,iid);
                if(!pointer)co_return finish(e_nointerface);++font(base)->references;m.store(GuestAddress{call(2)},bits,pointer);co_return finish(s_ok);
            }
            if(name=="AddRef")co_return finish(++font(base)->references);
            if(name=="Release")co_return finish(co_await release_object(base));
            auto* f=font(base);
            const auto store_bool=[&](std::uint64_t at,bool value){m.store(GuestAddress{at},32,value?1:0);};
            if(family=="internal-oleaut32-font"){
                if(name.starts_with("get_") && name!="get_hFont" && !call(1))co_return finish(e_pointer);
                if(name=="get_Name"){const auto text=utf16_bytes(f->name);const auto source=scratch(text.size());m.write(GuestAddress{source},text);
                    const auto bstr=co_await api("oleaut32.dll","SysAllocString",{source});if(!bstr)co_return finish(e_outofmemory);m.store(GuestAddress{call(1)},bits,bstr);co_return finish(s_ok);}
                if(name=="put_Name"){if(!call(1))co_return finish(ctl_invalid_value);f->name=read_utf16(m,GuestAddress{call(1)},256);(void)co_await changed(base,0);co_return finish(s_ok);}
                if(name=="get_Size"){m.store(GuestAddress{call(1)},64,static_cast<std::uint64_t>(f->size));co_return finish(s_ok);}
                if(name=="put_Size"){const auto value=static_cast<std::int64_t>(call(1));if(value<0)co_return finish(ctl_invalid_value);f->size=value;(void)co_await changed(base,2);co_return finish(s_ok);}
                if(name=="get_Bold"){store_bool(call(1),f->weight>550);co_return finish(s_ok);}
                if(name=="put_Bold"){f->weight=static_cast<std::uint32_t>(call(1))?700:400;(void)co_await changed(base,3);co_return finish(s_ok);}
                if(name=="get_Italic"){store_bool(call(1),f->italic);co_return finish(s_ok);}
                if(name=="put_Italic"){f->italic=static_cast<std::uint32_t>(call(1))!=0;(void)co_await changed(base,4);co_return finish(s_ok);}
                if(name=="get_Underline"){store_bool(call(1),f->underline);co_return finish(s_ok);}
                if(name=="put_Underline"){f->underline=static_cast<std::uint32_t>(call(1))!=0;(void)co_await changed(base,5);co_return finish(s_ok);}
                if(name=="get_Strikethrough"){store_bool(call(1),f->strike);co_return finish(s_ok);}
                if(name=="put_Strikethrough"){f->strike=static_cast<std::uint32_t>(call(1))!=0;(void)co_await changed(base,6);co_return finish(s_ok);}
                if(name=="get_Weight"){m.store(GuestAddress{call(1)},16,static_cast<std::uint16_t>(f->weight));co_return finish(s_ok);}
                if(name=="put_Weight"){f->weight=static_cast<std::int16_t>(call(1));(void)co_await changed(base,7);co_return finish(s_ok);}
                if(name=="get_Charset"){m.store(GuestAddress{call(1)},16,static_cast<std::uint16_t>(f->charset));co_return finish(s_ok);}
                if(name=="put_Charset"){f->charset=static_cast<std::int16_t>(call(1));(void)co_await changed(base,8);co_return finish(s_ok);}
                if(name=="get_hFont"){if(!call(1))co_return finish(e_pointer);const auto handle=co_await realize(base);if(!handle)co_return finish(e_fail);m.store(GuestAddress{call(1)},bits,handle);co_return finish(s_ok);}
                if(name=="Clone"){
                    if(!call(1))co_return finish(e_pointer);OleFont copy=*f;copy.dirty=false;const auto object=create(std::move(copy));
                    m.store(GuestAddress{call(1)},bits,object);co_return finish(s_ok);
                }
                if(name=="IsEqual"){
                    if(!call(1))co_return finish(e_pointer);const auto other_self=call(1);if(other_self==base)co_return finish(s_ok);
                    if(m.load(GuestAddress{other_self},bits)!=ole_font_vtables_[0])co_return finish(s_false);const auto* other=font(other_self);if(!other)co_return finish(s_false);
                    const bool equal=folded(other->name)==folded(f->name) && other->size==f->size && other->weight==f->weight && other->charset==f->charset &&
                        other->italic==f->italic && other->underline==f->underline && other->strike==f->strike;
                    co_return finish(equal?s_ok:s_false);
                }
                if(name=="SetRatio"){
                    const auto logical=long_arg(call(1)),himetric=long_arg(call(2));if(!logical || !himetric)co_return finish(e_fail);
                    f->ratio_logical=logical;f->ratio_himetric=himetric;const auto old=f->font;f->font=0;(void)co_await drop_font(old);co_return finish(s_ok);
                }
                if(name=="QueryTextMetrics"){
                    if(!call(1))co_return finish(e_pointer);const auto handle=co_await realize(base);if(!handle)co_return finish(e_fail);
                    const auto dc=co_await api("user32.dll","GetDC",{0});if(!dc)co_return finish(e_fail);
                    const auto previous=co_await api("gdi32.dll","SelectObject",{dc,handle});
                    const auto ok=static_cast<std::uint32_t>(co_await api("gdi32.dll","GetTextMetricsW",{dc,call(1)}));
                    if(previous)(void)co_await api("gdi32.dll","SelectObject",{dc,previous});(void)co_await api("user32.dll","ReleaseDC",{0,dc});
                    co_return finish(ok?s_ok:e_fail);
                }
                if(name=="AddRefHfont" || name=="ReleaseHfont"){
                    const auto handle=static_cast<std::uint64_t>(static_cast<std::uint32_t>(call(1)));if(!handle)co_return finish(e_invalidarg);
                    const bool current=std::any_of(ole_fonts_.begin(),ole_fonts_.end(),[&](const auto& item){return item.second.font==handle;});
                    auto found=ole_font_handles_.find(handle);
                    if(name=="AddRefHfont"){if(!current && found==ole_font_handles_.end())co_return finish(e_invalidarg);++ole_font_handles_[handle];co_return finish(s_ok);}
                    if(found==ole_font_handles_.end() || !found->second)co_return finish(current?s_false:e_invalidarg);
                    if(!--found->second)(void)co_await drop_font(handle);co_return finish(s_ok);
                }
                if(name=="SetHdc"){f->dc=call(1);co_return finish(s_ok);}
            }else if(family=="internal-oleaut32-font-dispatch"){
                if(name=="GetTypeInfoCount"){if(!call(1))co_return finish(e_invalidarg);m.store(GuestAddress{call(1)},32,0);co_return finish(s_ok);}
                if(name=="GetTypeInfo"){if(call(3))m.store(GuestAddress{call(3)},bits,0);co_return finish(disp_bad_index);}
                if(name=="GetIDsOfNames"){
                    const auto names=call(2),ids=call(5);const auto count=static_cast<std::uint32_t>(call(3));if(!names || !ids || !count)co_return finish(e_invalidarg);
                    const auto first=folded(read_utf16(m,GuestAddress{m.load(GuestAddress{names},bits)},256));std::int32_t id=-1;
                    for(std::size_t i=0;i<property_names.size();++i)if(!property_names[i].empty() && first==folded(std::u16string(property_names[i])))id=static_cast<std::int32_t>(i);
                    m.store(GuestAddress{ids},32,static_cast<std::uint32_t>(id));for(std::uint32_t i=1;i<count && i<64;++i)m.store(GuestAddress{ids+i*4},32,0xffffffffU);
                    co_return finish(id<0 || count>1?disp_unknown_name:s_ok);
                }
                if(name=="Invoke"){
                    // (dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr)：屬性讀取回 VARIANT，設定以 VariantChangeType 轉型後走同一個 put。
                    const auto dispid=long_arg(call(1));const auto flags=static_cast<std::uint32_t>(call(4))&0xffffU;const auto params=call(5),result=call(6);
                    if(!params)co_return finish(e_invalidarg);if(dispid<0 || dispid>8 || dispid==1)co_return finish(disp_member_not_found);
                    const unsigned variant=x86_?16U:24U;const auto args=m.load(GuestAddress{params},bits);const auto count=static_cast<std::uint32_t>(m.load(GuestAddress{params+2*unit},32));
                    if(flags&2){
                        if(count)co_return finish(disp_bad_param_count);if(!result)co_return finish(s_ok);
                        m.fill(GuestAddress{result},0,variant);
                        if(dispid==0){const auto text=utf16_bytes(f->name);const auto source=scratch(text.size());m.write(GuestAddress{source},text);
                            const auto bstr=co_await api("oleaut32.dll","SysAllocString",{source});m.store(GuestAddress{result},16,8);m.store(GuestAddress{result+8},bits,bstr);}
                        else if(dispid==2){m.store(GuestAddress{result},16,6);m.store(GuestAddress{result+8},64,static_cast<std::uint64_t>(f->size));}
                        else if(dispid==7 || dispid==8){m.store(GuestAddress{result},16,2);m.store(GuestAddress{result+8},16,static_cast<std::uint16_t>(dispid==7?f->weight:f->charset));}
                        else {const bool value=dispid==3?f->weight>550:dispid==4?f->italic:dispid==5?f->underline:f->strike;m.store(GuestAddress{result},16,11);m.store(GuestAddress{result+8},16,value?0xffffU:0U);}
                        co_return finish(s_ok);
                    }
                    if(!(flags&4))co_return finish(disp_member_not_found);if(count!=1)co_return finish(disp_bad_param_count);
                    const std::uint16_t wanted=dispid==0?8:dispid==2?6:dispid==7 || dispid==8?2:11;const auto converted=scratch(variant);
                    if(static_cast<std::uint32_t>(co_await api("oleaut32.dll","VariantChangeType",{converted,args,0,wanted}))){if(call(8))m.store(GuestAddress{call(8)},32,0);co_return finish(disp_type_mismatch);}
                    if(!(f=font(base)))co_return finish(e_unexpected);
                    if(dispid==0){const auto bstr=m.load(GuestAddress{converted+8},bits);f->name=bstr?read_utf16(m,GuestAddress{bstr},256):std::u16string{};(void)co_await api("oleaut32.dll","SysFreeString",{bstr});}
                    else if(dispid==2){const auto value=static_cast<std::int64_t>(m.load(GuestAddress{converted+8},64));if(value<0)co_return finish(ctl_invalid_value);f->size=value;}
                    else if(dispid==7)f->weight=static_cast<std::int16_t>(m.load(GuestAddress{converted+8},16));
                    else if(dispid==8)f->charset=static_cast<std::int16_t>(m.load(GuestAddress{converted+8},16));
                    else {const bool value=m.load(GuestAddress{converted+8},16)!=0;if(dispid==3)f->weight=value?700:400;else if(dispid==4)f->italic=value;else if(dispid==5)f->underline=value;else f->strike=value;}
                    (void)co_await changed(base,dispid);co_return finish(s_ok);
                }
            }else if(family=="internal-oleaut32-font-persist"){
                if(name=="GetClassID"){if(!call(1))co_return finish(e_pointer);m.write(GuestAddress{call(1)},clsid_std_font);co_return finish(s_ok);}
                if(name=="IsDirty")co_return finish(f->dirty?s_ok:s_false);
                if(name=="GetSizeMax"){if(!call(1))co_return finish(e_pointer);const auto ansi=co_await wide_to_ansi(f->name);
                    m.store(GuestAddress{call(1)},64,11+std::min<std::size_t>(ansi.size(),255));co_return finish(s_ok);}
                if(name=="Save"){
                    if(!call(1))co_return finish(e_pointer);auto ansi=co_await wide_to_ansi(f->name);ansi.resize(std::min<std::size_t>(ansi.size(),255));
                    if(!(f=font(base)))co_return finish(e_unexpected);
                    std::vector<std::uint8_t> data{1,static_cast<std::uint8_t>(f->charset),static_cast<std::uint8_t>(static_cast<std::uint16_t>(f->charset)>>8),
                        static_cast<std::uint8_t>((f->italic?2:0)|(f->underline?4:0)|(f->strike?8:0)),static_cast<std::uint8_t>(f->weight),static_cast<std::uint8_t>(static_cast<std::uint16_t>(f->weight)>>8)};
                    const auto size=static_cast<std::uint32_t>(std::clamp<std::int64_t>(f->size,0,UINT32_MAX));for(unsigned i=0;i<4;++i)data.push_back(static_cast<std::uint8_t>(size>>(8*i)));
                    data.push_back(static_cast<std::uint8_t>(ansi.size()));data.insert(data.end(),ansi.begin(),ansi.end());
                    const auto buffer=scratch(data.size()),written=scratch(4);m.write(GuestAddress{buffer},data);
                    const auto status=static_cast<std::uint32_t>(co_await com(call(1),4,{buffer,data.size(),written}));if(status&0x80000000U)co_return finish(status);
                    if(m.load(GuestAddress{written},32)!=data.size())co_return finish(0x80030070U); // STG_E_MEDIUMFULL
                    if(static_cast<std::uint32_t>(call(2)) && (f=font(base)))f->dirty=false;co_return finish(s_ok);
                }
                if(name=="Load"){
                    if(!call(1))co_return finish(e_pointer);
                    const auto read=[&](std::size_t size)->Task<std::pair<std::uint32_t,std::vector<std::uint8_t>>>{
                        std::vector<std::uint8_t> data(size);if(!size)co_return std::pair{s_ok,std::move(data)};const auto buffer=scratch(size),got=scratch(4);
                        const auto status=static_cast<std::uint32_t>(co_await com(call(1),3,{buffer,size,got}));if(status&0x80000000U)co_return std::pair{status,std::move(data)};
                        if(m.load(GuestAddress{got},32)!=size)co_return std::pair{e_fail,std::move(data)};m.read(GuestAddress{buffer},data);co_return std::pair{s_ok,std::move(data)};
                    };
                    auto [status,header]=co_await read(11);if(status)co_return finish(status);if(header[0]!=1)co_return finish(e_unexpected);
                    auto [name_status,text]=co_await read(header[10]);if(name_status)co_return finish(name_status);
                    auto loaded=co_await ansi_to_wide(text);if(!(f=font(base)))co_return finish(e_unexpected);
                    f->charset=static_cast<std::int16_t>(header[1]|(header[2]<<8));f->italic=(header[3]&2)!=0;f->underline=(header[3]&4)!=0;f->strike=(header[3]&8)!=0;
                    f->weight=static_cast<std::int16_t>(header[4]|(header[5]<<8));
                    f->size=static_cast<std::int64_t>(std::uint32_t{header[6]}|(std::uint32_t{header[7]}<<8)|(std::uint32_t{header[8]}<<16)|(std::uint32_t{header[9]}<<24));
                    f->name=std::move(loaded);const auto old=f->font;f->font=0;f->dirty=false;(void)co_await drop_font(old);co_return finish(s_ok);
                }
            }else if(family=="internal-oleaut32-font-container"){
                if(name=="FindConnectionPoint"){
                    if(!call(2))co_return finish(e_pointer);m.store(GuestAddress{call(2)},bits,0);if(!call(1))co_return finish(e_pointer);
                    const auto iid=load_guid(call(1));const unsigned point=iid==iid_property_sink?4U:iid==iid_font_events?5U:0U;
                    if(!point)co_return finish(connect_no_connection);++f->references;m.store(GuestAddress{call(2)},bits,base+point*unit);co_return finish(s_ok);
                }
                if(name=="EnumConnectionPoints"){
                    if(!call(1))co_return finish(e_pointer);const auto object=allocate_heap(m,unit);if(!object)co_return finish(e_outofmemory);
                    m.store(GuestAddress{object},bits,vtable(5));++f->references;ole_font_enums_.emplace(object,std::pair{base,0U});ole_font_enum_references_[object]=1;
                    m.store(GuestAddress{call(1)},bits,object);co_return finish(s_ok);
                }
            }else{
                const bool events=slot==5;
                if(name=="GetConnectionInterface"){if(!call(1))co_return finish(e_pointer);m.write(GuestAddress{call(1)},events?iid_font_events:iid_property_sink);co_return finish(s_ok);}
                if(name=="GetConnectionPointContainer"){if(!call(1))co_return finish(e_pointer);++f->references;m.store(GuestAddress{call(1)},bits,base+3*unit);co_return finish(s_ok);}
                if(name=="EnumConnections"){if(call(1))m.store(GuestAddress{call(1)},bits,0);co_return finish(e_notimpl);}
                if(name=="Advise"){
                    // 以連接點介面查詢接收端（IPropertyNotifySink 或 IFontEventsDisp，後者亦接受 IDispatch）；保存查詢得到的參照。
                    if(!call(1) || !call(2))co_return finish(e_pointer);m.store(GuestAddress{call(2)},32,0);
                    const auto out=scratch(unit),iid=scratch(16);m.write(GuestAddress{iid},events?iid_font_events:iid_property_sink);
                    auto status=static_cast<std::uint32_t>(co_await com(call(1),0,{iid,out}));
                    if((status&0x80000000U) && events){m.write(GuestAddress{iid},iid_dispatch);status=static_cast<std::uint32_t>(co_await com(call(1),0,{iid,out}));}
                    const auto sink=m.load(GuestAddress{out},bits);if((status&0x80000000U) || !sink)co_return finish(connect_cannot_connect);
                    if(!(f=font(base))){(void)co_await com(sink,2,{});co_return finish(e_unexpected);}
                    const auto cookie=f->next_cookie++;f->sinks.emplace(cookie,std::pair{events,sink});m.store(GuestAddress{call(2)},32,cookie);co_return finish(s_ok);
                }
                if(name=="Unadvise"){
                    const auto cookie=static_cast<std::uint32_t>(call(1));const auto found=f->sinks.find(cookie);if(found==f->sinks.end() || found->second.first!=events)co_return finish(connect_no_connection);
                    const auto sink=found->second.second;f->sinks.erase(found);(void)co_await com(sink,2,{});co_return finish(s_ok);
                }
            }
            co_return finish(e_notimpl);
        }catch(const MemoryFault&){co_return finish(e_pointer);}catch(const std::bad_alloc&){co_return finish(e_outofmemory);}
    });
}
}
