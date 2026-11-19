// ole32 的 OLE 內嵌輔助：CreateOleAdviseHolder、CreateDataAdviseHolder（宿主物件，連線清單與 IEnumSTATDATA）、OleRegEnumVerbs（IEnumOLEVERB）、
// OleRegEnumFormatEtc（沿用 SHCreateStdEnumFmtEtc 的 IEnumFORMATETC）、OleDraw、OleLoad、OleSave。
// - IOleAdviseHolder：Advise 保存 IAdviseSink（AddRef）並回傳從 1 起的連線；SendOnRename／SendOnSave／SendOnClose 依序呼叫各 sink；Unadvise 不存在時 OLE_E_NOCONNECTION。
// - IDataAdviseHolder：Advise 複製 FORMATETC（含 DVTARGETDEVICE）；ADVF_PRIMEFIRST 時立即以 IDataObject::GetData 的結果（ADVF_NODATA 時為空 STGMEDIUM）呼叫 OnDataChange，
//   ADVF_ONLYONCE 送出一次後移除；SendOnDataChange 對每個連線取資料並通知（取資料失敗的連線略過），用畢 ReleaseStgMedium。
// - EnumAdvise 的 STATDATA：FORMATETC 的 ptd 以 CoTaskMemAlloc 複製、pAdvSink 已 AddRef，由呼叫者釋放；OLE advise holder 的 FORMATETC 為零。
// - OleRegEnumVerbs：HKCR\CLSID\{clsid}\Verb 的子機碼名稱為動詞編號、預設值為「名稱,選單旗標,屬性」，依編號排序；沒有 CLSID 為 REGDB_E_CLASSNOTREG，沒有 Verb 為 OLEOBJ_E_NOVERBS。
// - OleRegEnumFormatEtc：HKCR\CLSID\{clsid}\DataFormats\GetSet 的值「格式,aspect,tymed,方向」，格式為數字或剪貼簿格式名稱，只列出方向符合 dwDirection 者。
// - OleLoad：ReadClassStg＋CoCreateInstance(in-process)，OLEMISC_SETCLIENTSITEFIRST 時先 SetClientSite，再 IPersistStorage::Load；OleSave：WriteClassStg＋IPersistStorage::Save。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>

namespace x64arm {
namespace {
using Guid=std::array<std::uint8_t,16>;
constexpr Guid ole_guid(std::uint32_t a){Guid g{};for(unsigned i=0;i<4;++i)g[i]=static_cast<std::uint8_t>(a>>(8*i));g[8]=0xc0;g[15]=0x46;return g;}
constexpr auto iid_unknown=ole_guid(0),iid_ole_advise_holder=ole_guid(0x111),iid_data_advise_holder=ole_guid(0x110),iid_enum_statdata=ole_guid(0x105),
    iid_enum_oleverb=ole_guid(0x104),iid_view_object=ole_guid(0x10d),iid_persist_storage=ole_guid(0x10a),iid_ole_object=ole_guid(0x112);
constexpr std::uint32_t s_ok=0,s_false=1,e_nointerface=0x80004002U,e_pointer=0x80004003U,e_unexpected=0x8000ffffU,e_invalidarg=0x80070057U,e_outofmemory=0x8007000eU,
    ole_e_noconnection=0x80040004U,oleobj_e_noverbs=0x80040180U,regdb_e_classnotreg=0x80040154U,regdb_e_keymissing=0x80040152U;
constexpr std::uint32_t advf_nodata=1,advf_primefirst=2,advf_onlyonce=4;
bool failed(std::uint64_t hr){return (hr&0x80000000U)!=0;}
std::u16string guid_text(const Guid& g){
    constexpr char16_t digits[]=u"0123456789ABCDEF";constexpr std::array<unsigned,16> order{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};std::u16string out=u"{";
    for(unsigned i=0;i<16;++i){if(i==4 || i==6 || i==8 || i==10)out.push_back(u'-');out.push_back(digits[g[order[i]]>>4]);out.push_back(digits[g[order[i]]&15]);}
    return out+u"}";
}
}
std::span<const WindowsServiceDescriptor> ole_advise_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ole32.dll","CreateOleAdviseHolder",1},{"ole32.dll","CreateDataAdviseHolder",1},{"ole32.dll","OleRegEnumVerbs",2},
        {"ole32.dll","OleRegEnumFormatEtc",3,0,0,0x2},{"ole32.dll","OleDraw",4,0,0,0x2},{"ole32.dll","OleLoad",4},{"ole32.dll","OleSave",3,0,0,0x4},
        {"internal-ole32-oleadvise","QueryInterface",3},{"internal-ole32-oleadvise","AddRef",1},{"internal-ole32-oleadvise","Release",1},
        {"internal-ole32-oleadvise","Advise",3},{"internal-ole32-oleadvise","Unadvise",2,0,0,0x2},{"internal-ole32-oleadvise","EnumAdvise",2},
        {"internal-ole32-oleadvise","SendOnRename",2},{"internal-ole32-oleadvise","SendOnSave",1},{"internal-ole32-oleadvise","SendOnClose",1},
        {"internal-ole32-dataadvise","QueryInterface",3},{"internal-ole32-dataadvise","AddRef",1},{"internal-ole32-dataadvise","Release",1},
        {"internal-ole32-dataadvise","Advise",6,0,0,0x8},{"internal-ole32-dataadvise","Unadvise",2,0,0,0x2},{"internal-ole32-dataadvise","EnumAdvise",2},
        {"internal-ole32-dataadvise","SendOnDataChange",4,0,0,0xc},
        {"internal-ole32-statenum","QueryInterface",3},{"internal-ole32-statenum","AddRef",1},{"internal-ole32-statenum","Release",1},
        {"internal-ole32-statenum","Next",4,0,0,0x2},{"internal-ole32-statenum","Skip",2,0,0,0x2},{"internal-ole32-statenum","Reset",1},{"internal-ole32-statenum","Clone",2},
        {"internal-ole32-verbenum","QueryInterface",3},{"internal-ole32-verbenum","AddRef",1},{"internal-ole32-verbenum","Release",1},
        {"internal-ole32-verbenum","Next",4,0,0,0x2},{"internal-ole32-verbenum","Skip",2,0,0,0x2},{"internal-ole32-verbenum","Reset",1},{"internal-ole32-verbenum","Clone",2}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_ole_advise(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"OLE advise",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        using service_callback::Task;
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto family=service.family;const auto name=service.symbol;
        // FORMATETC：cfFormat(2)、ptd(unit)、dwAspect、lindex、tymed；STATDATA：FORMATETC、advf、pAdvSink、dwConnection；STGMEDIUM：tymed、聯集、pUnkForRelease。
        const unsigned format_size=x86_?20U:32U,stat_size=x86_?32U:56U,medium_size=x86_?12U:24U,verb_size=x86_?16U:24U;
        const unsigned stat_advf=format_size,stat_sink=x86_?24U:40U,stat_connection=x86_?28U:48U;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} cleanup{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto bytes=[&](std::span<const std::uint8_t> value){const auto block=scratch(value.size());m.write(GuestAddress{block},value);return block;};
        const auto text=[&](std::u16string_view value){return bytes(utf16_bytes(value));};
        const auto api=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve(dll,symbol).value,std::move(arguments));};
        const auto method=[&](std::uint64_t object,unsigned index){return m.load(GuestAddress{m.load(GuestAddress{object},bits)+std::uint64_t{index}*unit},bits);};
        const auto com=[&](std::uint64_t object,unsigned index,std::vector<std::uint64_t> arguments)->Task<std::uint64_t>{
            arguments.insert(arguments.begin(),object);co_return co_await service_callback::call_value(method(object,index),std::move(arguments));};
        const auto out_cell=[&]{return scratch(unit);};
        const auto vtable=[&](unsigned which){
            auto& table=ole_advise_vtables_[which];if(table)return table;
            static constexpr std::array<std::string_view,4> families{"internal-ole32-oleadvise","internal-ole32-dataadvise","internal-ole32-statenum","internal-ole32-verbenum"};
            std::vector<std::uint64_t> entries;for(const auto& d:ole_advise_service_descriptors())if(d.family==families[which])entries.push_back(resolve(d.family,d.symbol).value);
            table=runtime_allocate(m,{},entries.size()*unit);for(std::size_t i=0;i<entries.size();++i)m.store(GuestAddress{table+i*unit},bits,entries[i]);return table;
        };
        const auto object_of=[&](unsigned which)->std::uint64_t{const auto object=allocate_heap(m,unit);if(!object)throw std::bad_alloc{};m.store(GuestAddress{object},bits,vtable(which));return object;};
        // FORMATETC 複本（ptd 內容另存）與寫回。
        const auto read_format=[&](std::uint64_t at,OleAdviseEntry& entry){
            entry.format.resize(format_size);m.read(GuestAddress{at},std::span<std::uint8_t>(entry.format));
            const auto ptd=m.load(GuestAddress{at+unit},bits);entry.target.clear();
            if(ptd){const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{ptd},32));if(size>=4 && size<=65536){entry.target.resize(size);m.read(GuestAddress{ptd},std::span<std::uint8_t>(entry.target));}}
        };
        const auto write_format=[&](std::uint64_t at,const OleAdviseEntry& entry,bool task_memory)->Task<std::uint32_t>{
            if(entry.format.empty()){m.fill(GuestAddress{at},0,format_size);co_return s_ok;}
            m.write(GuestAddress{at},entry.format);std::uint64_t ptd=0;
            if(!entry.target.empty()){ptd=task_memory?co_await api("ole32.dll","CoTaskMemAlloc",{entry.target.size()}):scratch(entry.target.size());if(!ptd)co_return e_outofmemory;m.write(GuestAddress{ptd},entry.target);}
            m.store(GuestAddress{at+unit},bits,ptd);co_return s_ok;
        };
        const auto addref=[&](std::uint64_t p)->Task<std::uint32_t>{if(!p)co_return 0;co_return static_cast<std::uint32_t>(co_await com(p,1,{}));};
        const auto release=[&](std::uint64_t p)->Task<std::uint32_t>{if(!p)co_return 0;co_return static_cast<std::uint32_t>(co_await com(p,2,{}));};
        // 對一個資料連線送出 OnDataChange；回傳是否送出。
        const auto notify_data=[&](const OleAdviseEntry& entry,std::uint64_t data_object)->Task<bool>{
            const auto format=scratch(format_size);(void)co_await write_format(format,entry,false);const auto medium=scratch(medium_size);
            bool obtained=false;
            if(!(entry.advf&advf_nodata)){if(!data_object || failed(co_await com(data_object,3,{format,medium})))co_return false;obtained=true;}
            (void)co_await com(entry.sink,3,{format,medium});
            if(obtained)(void)co_await api("ole32.dll","ReleaseStgMedium",{medium});
            co_return true;
        };
        try{
            if(family=="ole32.dll"){
                if(name=="CreateOleAdviseHolder" || name=="CreateDataAdviseHolder"){
                    if(!call(0))co_return finish(e_invalidarg);m.store(GuestAddress{call(0)},bits,0);
                    const bool data=name=="CreateDataAdviseHolder";const auto object=object_of(data?1:0);OleAdviseHolder holder;holder.data=data;
                    ole_advise_holders_.emplace(object,std::move(holder));m.store(GuestAddress{call(0)},bits,object);co_return finish(s_ok);
                }
                if(name=="OleRegEnumVerbs" || name=="OleRegEnumFormatEtc"){
                    const bool verbs=name=="OleRegEnumVerbs";const auto output=call(verbs?1:2);if(!output)co_return finish(e_invalidarg);m.store(GuestAddress{output},bits,0);
                    if(!call(0))co_return finish(e_invalidarg);Guid clsid{};m.read(GuestAddress{call(0)},clsid);
                    const auto root=x86_?UINT64_C(0x80000000):UINT64_C(0xffffffff80000000); // HKEY_CLASSES_ROOT
                    const auto open=[&](std::uint64_t parent,std::u16string_view path)->Task<std::uint64_t>{const auto cell=out_cell();
                        if(co_await api("advapi32.dll","RegOpenKeyExW",{parent,text(path),0,0x20019,cell}))co_return 0;co_return m.load(GuestAddress{cell},bits);};
                    const auto close=[&](std::uint64_t key)->Task<std::uint32_t>{if(key)(void)co_await api("advapi32.dll","RegCloseKey",{key});co_return 0;};
                    const auto class_key=co_await open(root,u"CLSID\\"+guid_text(clsid));if(!class_key)co_return finish(regdb_e_classnotreg);
                    const auto list_key=co_await open(class_key,verbs?u"Verb":u"DataFormats\\GetSet");(void)co_await close(class_key);
                    if(!list_key)co_return finish(verbs?oleobj_e_noverbs:regdb_e_keymissing);
                    // 子機碼名稱與預設值。
                    std::vector<std::pair<std::u16string,std::u16string>> rows;const auto name_buffer=scratch(512),value_buffer=scratch(2048),length=scratch(4),type=scratch(4);
                    for(std::uint32_t index=0;index<4096;++index){
                        m.store(GuestAddress{length},32,256);if(co_await api("advapi32.dll","RegEnumKeyExW",{list_key,index,name_buffer,length,0,0,0,0}))break;
                        const auto sub=read_utf16(m,GuestAddress{name_buffer},256);m.store(GuestAddress{length},32,2048);
                        std::u16string value;if(!co_await api("advapi32.dll","RegGetValueW",{list_key,text(sub),0,0x2,type,value_buffer,length}))value=read_utf16(m,GuestAddress{value_buffer},1024); // RRF_RT_REG_SZ
                        rows.push_back({sub,value});
                    }
                    (void)co_await close(list_key);
                    const auto number=[](std::u16string_view v)->std::optional<std::int64_t>{std::int64_t n=0;bool negative=false;std::size_t i=0;while(i<v.size() && v[i]==u' ')++i;
                        if(i<v.size() && (v[i]==u'-' || v[i]==u'+')){negative=v[i]==u'-';++i;}if(i==v.size())return std::nullopt;
                        for(;i<v.size();++i){if(v[i]==u' ')break;if(v[i]<u'0' || v[i]>u'9')return std::nullopt;n=n*10+(v[i]-u'0');}return negative?-n:n;};
                    const auto fields=[](std::u16string_view v){std::vector<std::u16string> out;std::size_t at=0;while(true){const auto comma=v.find(u',',at);out.emplace_back(v.substr(at,comma==v.npos?v.npos:comma-at));if(comma==v.npos)break;at=comma+1;}return out;};
                    if(verbs){
                        OleVerbEnum e;
                        for(const auto& [sub,value]:rows){const auto verb=number(sub);if(!verb)continue;
                            // 「名稱,選單旗標,屬性」：名稱本身可含逗號，旗標與屬性取最後兩欄。
                            const auto parts=fields(value);OleVerbEnum::Verb row{static_cast<std::int32_t>(*verb),value,0,0};
                            if(parts.size()>=3){const auto flags=number(parts[parts.size()-2]),attributes=number(parts.back());
                                if(flags && attributes){row.flags=static_cast<std::uint32_t>(*flags);row.attributes=static_cast<std::uint32_t>(*attributes);
                                    row.name=value.substr(0,value.size()-parts.back().size()-parts[parts.size()-2].size()-2);}}
                            e.verbs.push_back(std::move(row));}
                        std::stable_sort(e.verbs.begin(),e.verbs.end(),[](const auto& a,const auto& b){return a.verb<b.verb;});
                        const auto object=object_of(3);ole_verb_enums_.emplace(object,std::move(e));m.store(GuestAddress{output},bits,object);co_return finish(s_ok);
                    }
                    const auto direction=static_cast<std::uint32_t>(call(1));std::vector<std::uint8_t> formats;std::uint32_t count=0;
                    for(const auto& [sub,value]:rows){(void)sub;const auto parts=fields(value);if(parts.size()<4)continue;
                        const auto aspect=number(parts[1]),tymed=number(parts[2]),flags=number(parts[3]);if(!aspect || !tymed || !flags || !(static_cast<std::uint32_t>(*flags)&direction))continue;
                        std::uint32_t cf=0;if(const auto n=number(parts[0]))cf=static_cast<std::uint32_t>(*n);else cf=static_cast<std::uint32_t>(co_await api("user32.dll","RegisterClipboardFormatW",{text(parts[0])}))&0xffffU;
                        if(!cf)continue;std::vector<std::uint8_t> one(format_size);one[0]=static_cast<std::uint8_t>(cf);one[1]=static_cast<std::uint8_t>(cf>>8);
                        const auto put=[&](unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)one[at+i]=static_cast<std::uint8_t>(v>>(8*i));};
                        put(2*unit,static_cast<std::uint32_t>(*aspect));put(2*unit+4,0xffffffffU);put(2*unit+8,static_cast<std::uint32_t>(*tymed));
                        formats.insert(formats.end(),one.begin(),one.end());++count;}
                    co_return finish(co_await api("shell32.dll","SHCreateStdEnumFmtEtc",{count,formats.empty()?0:bytes(formats),output}));
                }
                if(name=="OleDraw"){
                    // (pUnk, dwAspect, hdcDraw, lprcBounds)：IViewObject::Draw(dwAspect, -1, NULL, NULL, NULL, hdc, RECTL, NULL, NULL, 0)。
                    if(!call(0) || !call(3))co_return finish(e_invalidarg);const auto cell=out_cell();
                    if(const auto hr=static_cast<std::uint32_t>(co_await com(call(0),0,{bytes(iid_view_object),cell}));failed(hr))co_return finish(hr);
                    const auto view=m.load(GuestAddress{cell},bits);
                    const auto result=co_await com(view,3,{static_cast<std::uint32_t>(call(1)),static_cast<std::uint64_t>(static_cast<std::int64_t>(-1)),0,0,0,call(2),call(3),0,0,0});
                    (void)co_await release(view);co_return finish(result);
                }
                if(name=="OleSave"){
                    // (pPS, pStg, fSameAsLoad)
                    if(!call(0) || !call(1))co_return finish(e_invalidarg);const auto clsid=scratch(16);
                    if(const auto hr=static_cast<std::uint32_t>(co_await com(call(0),3,{clsid}));failed(hr))co_return finish(hr);
                    if(const auto hr=static_cast<std::uint32_t>(co_await api("ole32.dll","WriteClassStg",{call(1),clsid}));failed(hr))co_return finish(hr);
                    co_return finish(co_await com(call(0),7,{call(1),static_cast<std::uint32_t>(call(2))?1U:0U}));
                }
                if(name=="OleLoad"){
                    // (pStg, riid, pClientSite, ppvObj)
                    if(!call(3))co_return finish(e_invalidarg);m.store(GuestAddress{call(3)},bits,0);if(!call(0) || !call(1))co_return finish(e_invalidarg);
                    const auto clsid=scratch(16);if(const auto hr=static_cast<std::uint32_t>(co_await api("ole32.dll","ReadClassStg",{call(0),clsid}));failed(hr))co_return finish(hr);
                    const auto cell=out_cell();
                    if(const auto hr=static_cast<std::uint32_t>(co_await api("ole32.dll","CoCreateInstance",{clsid,0,3,bytes(iid_unknown),cell}));failed(hr))co_return finish(hr);
                    const auto object=m.load(GuestAddress{cell},bits);std::uint64_t ole_object=0;bool site_set=false;
                    if(call(2)){const auto ole_cell=out_cell();if(!failed(co_await com(object,0,{bytes(iid_ole_object),ole_cell})))ole_object=m.load(GuestAddress{ole_cell},bits);
                        if(ole_object){const auto status=scratch(4);
                            if(!failed(co_await com(ole_object,22,{1,status})) && (m.load(GuestAddress{status},32)&0x20000U)){(void)co_await com(ole_object,3,{call(2)});site_set=true;}}} // OLEMISC_SETCLIENTSITEFIRST
                    const auto persist_cell=out_cell();std::uint32_t hr=static_cast<std::uint32_t>(co_await com(object,0,{bytes(iid_persist_storage),persist_cell}));
                    if(!failed(hr)){const auto persist=m.load(GuestAddress{persist_cell},bits);hr=static_cast<std::uint32_t>(co_await com(persist,6,{call(0)}));(void)co_await release(persist);}
                    if(!failed(hr) && ole_object && !site_set)(void)co_await com(ole_object,3,{call(2)});
                    if(!failed(hr))hr=static_cast<std::uint32_t>(co_await com(object,0,{call(1),call(3)}));
                    (void)co_await release(ole_object);(void)co_await release(object);co_return finish(hr);
                }
                throw HostCallFault("未知的 OLE advise 匯出");
            }
            const auto self=call(0);
            if(family=="internal-ole32-statenum" || family=="internal-ole32-verbenum"){
                const bool stat=family=="internal-ole32-statenum";
                const bool known=stat?ole_stat_enums_.contains(self):ole_verb_enums_.contains(self);if(!known)co_return finish(e_unexpected);
                auto& position=stat?ole_stat_enums_.at(self).position:ole_verb_enums_.at(self).position;
                auto& references=stat?ole_stat_enums_.at(self).references:ole_verb_enums_.at(self).references;
                const auto total=stat?ole_stat_enums_.at(self).entries.size():ole_verb_enums_.at(self).verbs.size();
                if(name=="QueryInterface"){
                    if(!call(2))co_return finish(e_pointer);Guid iid{};m.read(GuestAddress{call(1)},iid);
                    if(iid!=iid_unknown && iid!=(stat?iid_enum_statdata:iid_enum_oleverb)){m.store(GuestAddress{call(2)},bits,0);co_return finish(e_nointerface);}
                    ++references;m.store(GuestAddress{call(2)},bits,self);co_return finish(s_ok);
                }
                if(name=="AddRef")co_return finish(++references);
                if(name=="Release"){
                    const auto left=--references;
                    if(!left){std::vector<std::uint64_t> sinks;if(stat){for(const auto& e:ole_stat_enums_.at(self).entries)sinks.push_back(e.sink);ole_stat_enums_.erase(self);}else ole_verb_enums_.erase(self);
                        (void)free_heap(m,self,s);for(const auto sink:sinks)(void)co_await release(sink);}
                    co_return finish(left);
                }
                if(name=="Reset"){position=0;co_return finish(s_ok);}
                if(name=="Skip"){const auto count=static_cast<std::uint32_t>(call(1));const auto skip=std::min<std::size_t>(count,total-position);position+=skip;co_return finish(skip==count?s_ok:s_false);}
                if(name=="Clone"){
                    if(!call(1))co_return finish(e_invalidarg);const auto object=object_of(stat?2:3);
                    if(stat){auto copy=ole_stat_enums_.at(self);copy.references=1;for(const auto& e:copy.entries)(void)co_await addref(e.sink);ole_stat_enums_.emplace(object,std::move(copy));}
                    else{auto copy=ole_verb_enums_.at(self);copy.references=1;ole_verb_enums_.emplace(object,std::move(copy));}
                    m.store(GuestAddress{call(1)},bits,object);co_return finish(s_ok);
                }
                // Next(celt, rgelt, pceltFetched)
                const auto count=static_cast<std::uint32_t>(call(1));if(!call(2) || (count>1 && !call(3)))co_return finish(e_invalidarg);std::uint32_t got=0;
                while(got<count){
                    if(stat){auto& e=ole_stat_enums_.at(self);if(e.position>=e.entries.size())break;const auto entry=e.entries[e.position++];
                        const auto at=call(2)+std::uint64_t{got}*stat_size;m.fill(GuestAddress{at},0,stat_size);
                        if(const auto hr=co_await write_format(at,entry,true);failed(hr))co_return finish(hr);
                        m.store(GuestAddress{at+stat_advf},32,entry.advf);(void)co_await addref(entry.sink);m.store(GuestAddress{at+stat_sink},bits,entry.sink);m.store(GuestAddress{at+stat_connection},32,entry.connection);}
                    else{auto& e=ole_verb_enums_.at(self);if(e.position>=e.verbs.size())break;const auto verb=e.verbs[e.position++];
                        const auto at=call(2)+std::uint64_t{got}*verb_size;m.fill(GuestAddress{at},0,verb_size);
                        const auto data=utf16_bytes(verb.name);const auto pointer=co_await api("ole32.dll","CoTaskMemAlloc",{data.size()});if(!pointer)co_return finish(e_outofmemory);m.write(GuestAddress{pointer},data);
                        m.store(GuestAddress{at},32,static_cast<std::uint32_t>(verb.verb));m.store(GuestAddress{at+unit},bits,pointer);m.store(GuestAddress{at+2*unit},32,verb.flags);m.store(GuestAddress{at+2*unit+4},32,verb.attributes);}
                    ++got;
                }
                if(call(3))m.store(GuestAddress{call(3)},32,got);co_return finish(got==count?s_ok:s_false);
            }
            // advise holder
            const auto found=ole_advise_holders_.find(self);if(found==ole_advise_holders_.end())co_return finish(e_unexpected);
            const bool data=found->second.data;
            if(name=="QueryInterface"){
                if(!call(2))co_return finish(e_pointer);Guid iid{};m.read(GuestAddress{call(1)},iid);
                if(iid!=iid_unknown && iid!=(data?iid_data_advise_holder:iid_ole_advise_holder)){m.store(GuestAddress{call(2)},bits,0);co_return finish(e_nointerface);}
                ++found->second.references;m.store(GuestAddress{call(2)},bits,self);co_return finish(s_ok);
            }
            if(name=="AddRef")co_return finish(++found->second.references);
            if(name=="Release"){
                const auto left=--found->second.references;
                if(!left){const auto entries=std::move(found->second.entries);ole_advise_holders_.erase(found);(void)free_heap(m,self,s);for(const auto& e:entries)(void)co_await release(e.sink);}
                co_return finish(left);
            }
            if(name=="Unadvise"){
                const auto connection=static_cast<std::uint32_t>(call(1));auto& entries=found->second.entries;
                const auto it=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.connection==connection;});if(it==entries.end())co_return finish(ole_e_noconnection);
                const auto sink=it->sink;entries.erase(it);(void)co_await release(sink);co_return finish(s_ok);
            }
            if(name=="EnumAdvise"){
                if(!call(1))co_return finish(e_invalidarg);m.store(GuestAddress{call(1)},bits,0);
                OleStatEnum e;e.entries=found->second.entries;for(const auto& entry:e.entries)(void)co_await addref(entry.sink);
                const auto object=object_of(2);ole_stat_enums_.emplace(object,std::move(e));m.store(GuestAddress{call(1)},bits,object);co_return finish(s_ok);
            }
            if(!data){
                if(name=="Advise"){
                    // (pAdvise, pdwConnection)
                    if(!call(1) || !call(2))co_return finish(e_invalidarg);m.store(GuestAddress{call(2)},32,0);
                    (void)co_await addref(call(1));auto& holder=ole_advise_holders_.at(self);OleAdviseEntry entry;entry.sink=call(1);entry.connection=holder.next++;
                    holder.entries.push_back(entry);m.store(GuestAddress{call(2)},32,entry.connection);co_return finish(s_ok);
                }
                // SendOnRename(pmk)、SendOnSave、SendOnClose：IAdviseSink::OnRename／OnSave／OnClose。
                const unsigned slot=name=="SendOnRename"?5U:name=="SendOnSave"?6U:7U;const auto entries=found->second.entries;
                for(const auto& e:entries){if(slot==5)(void)co_await com(e.sink,5,{call(1)});else (void)co_await com(e.sink,slot,{});}
                co_return finish(s_ok);
            }
            if(name=="Advise"){
                // (pDataObject, pFetc, advf, pAdvise, pdwConnection)
                if(!call(2) || !call(4) || !call(5))co_return finish(e_invalidarg);m.store(GuestAddress{call(5)},32,0);
                OleAdviseEntry entry;read_format(call(2),entry);entry.advf=static_cast<std::uint32_t>(call(3));entry.sink=call(4);
                (void)co_await addref(entry.sink);entry.connection=ole_advise_holders_.at(self).next++;
                bool keep=true;
                if((entry.advf&advf_primefirst) && call(1)){const bool sent=co_await notify_data(entry,call(1));if(sent && (entry.advf&advf_onlyonce))keep=false;}
                if(keep && ole_advise_holders_.contains(self))ole_advise_holders_.at(self).entries.push_back(entry);else (void)co_await release(entry.sink);
                m.store(GuestAddress{call(5)},32,entry.connection);co_return finish(s_ok);
            }
            // SendOnDataChange(pDataObject, dwReserved, advf)
            const auto entries=found->second.entries;std::vector<std::uint32_t> once;
            for(const auto& e:entries)if(co_await notify_data(e,call(1)) && (e.advf&advf_onlyonce))once.push_back(e.connection);
            if(!once.empty() && ole_advise_holders_.contains(self)){auto& list=ole_advise_holders_.at(self).entries;std::vector<std::uint64_t> sinks;
                std::erase_if(list,[&](const auto& e){if(std::find(once.begin(),once.end(),e.connection)==once.end())return false;sinks.push_back(e.sink);return true;});
                for(const auto sink:sinks)(void)co_await release(sink);}
            co_return finish(s_ok);
        }catch(const MemoryFault&){co_return finish(e_invalidarg);}catch(const std::bad_alloc&){co_return finish(e_outofmemory);}
    });
}
}
