// OLE 系統 moniker（ole32）：file、item、class、pointer、anti 與 generic composite，MonikerCommonPrefixWith、MkParseDisplayName、CoGetObject、
// 系統 moniker 的 class factory（ole32 DllGetClassObject 與 CoCreateInstance／CoGetClassObject，供 OleLoadFromStream 還原），以及 oleaut32 的
// RegisterActiveObject／RevokeActiveObject／GetActiveObject（以 "!" 分隔的 {CLSID} item moniker 登錄到既有的執行中物件表）。
// - 每個 moniker 是宿主物件：IMoniker（含 IPersist／IPersistStream）與 IROTData 兩個介面；generic composite 以攤平的元件清單保存，
//   元件可以是客體實作的 moniker。比較資料為 CLSID＋各類內容（item 名稱轉大寫、composite 串接各元件資料），供 ROT 比對。
// - 方法語義依 IMoniker 各方法的系統 moniker 說明：item 需要左側容器（IOleItemContainer）、class 以 CoGetClassObject 或左側的 IClassActivator、
//   pointer 對包裝的指標 QueryInterface、anti 不能繫結；composite 先查 ROT，再以最右元件、其餘元件為左側遞迴。
// - CreateGenericComposite：左右兩邊攤平後，在交界處以 ComposeWith(fOnlyIfNotGeneric＝TRUE) 嘗試非泛型組合（item／class／pointer 遇到 anti 互相抵銷），
//   其餘串接；結果為空時回 NULL。
// - 持久化：item 為「長度＋ANSI（NUL 結尾）＋選用 Unicode」的分隔字元與名稱，anti 為個數，class 為 CLSID＋附加資料長度，composite 為個數＋各元件
//   OleSaveToStream；pointer 不能持久化（E_NOTIMPL）。file 依 [MS-OSHARED] FileMoniker：cAnti、ANSI 路徑、endServer（非 UNC 為 0xFFFF）、0xDEAD、保留欄位與選用 Unicode。
// - file：沒有左側時先查 ROT，否則 GetClassFile＋CoCreateInstance(IPersistFile)::Load（左側提供 IClassFactory 時改用它）；BindToStorage 支援 IStorage／IStream；
//   相對 file 組合會以 ".." 逐一去掉左側路徑的末段；CommonPrefixWith／RelativePathTo 以路徑元件（\\server\share 與磁碟代號各算一個）比較。
// - MkParseDisplayName：@ProgID 與「ProgID:」交給類別的 IParseDisplayName，"clsid:" 建 class moniker，否則取最長的現存檔案路徑建 file moniker，
//   剩餘部分交給前一段 moniker 的 ParseDisplayName 後組合。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <cctype>
#include <array>

namespace x64arm {
namespace {
using Guid=std::array<std::uint8_t,16>;
constexpr Guid ole_guid(std::uint32_t a){Guid g{};for(unsigned i=0;i<4;++i)g[i]=static_cast<std::uint8_t>(a>>(8*i));g[8]=0xc0;g[15]=0x46;return g;}
constexpr Guid make_guid(std::uint32_t a,std::uint16_t b,std::uint16_t c,std::array<std::uint8_t,8> d){
    Guid g{};for(unsigned i=0;i<4;++i)g[i]=static_cast<std::uint8_t>(a>>(8*i));
    for(unsigned i=0;i<2;++i){g[4+i]=static_cast<std::uint8_t>(b>>(8*i));g[6+i]=static_cast<std::uint8_t>(c>>(8*i));}
    for(unsigned i=0;i<8;++i)g[8+i]=d[i];return g;
}
constexpr auto iid_unknown=ole_guid(0),iid_moniker=ole_guid(0xf),iid_persist=ole_guid(0x10c),iid_persist_stream=ole_guid(0x109),iid_enum_moniker=ole_guid(0x102),
    iid_item_container=ole_guid(0x11c),iid_parse_display_name=ole_guid(0x11a),iid_class_activator=ole_guid(0x140),iid_class_factory=ole_guid(1),
    iid_storage=ole_guid(0xb),iid_stream=ole_guid(0xc),iid_persist_file=ole_guid(0x10b);
constexpr auto iid_rot_data=make_guid(0xf29f6bc0,0x5021,0x11ce,{0xaa,0x15,0,0,0x69,1,0x29,0x3f});
constexpr auto clsid_file=ole_guid(0x303),clsid_item=ole_guid(0x304),clsid_anti=ole_guid(0x305),clsid_pointer=ole_guid(0x306),clsid_composite=ole_guid(0x309),clsid_class=ole_guid(0x31a);
constexpr std::uint32_t s_ok=0,s_false=1,e_notimpl=0x80004001U,e_nointerface=0x80004002U,e_pointer=0x80004003U,e_unexpected=0x8000ffffU,e_invalidarg=0x80070057U,
    e_outofmemory=0x8007000eU,mk_s_reduced_to_self=0x000401e2U,mk_s_me=0x000401e4U,mk_s_him=0x000401e5U,mk_s_us=0x000401e6U,
    mk_e_needgeneric=0x800401e2U,mk_e_unavailable=0x800401e3U,mk_e_syntax=0x800401e4U,mk_e_intermediate=0x800401e7U,mk_e_notbindable=0x800401e8U,
    mk_e_noinverse=0x800401ecU,mk_e_noprefix=0x800401eeU,mk_e_noobject=0x800401e5U,mk_e_nostorage=0x800401edU,class_e_noaggregation=0x80040110U,
    class_e_classnotavailable=0x80040111U,regdb_e_classnotreg=0x80040154U;
// IsSystemMoniker 的 MKSYS 值。
constexpr std::uint8_t mksys_composite=1,mksys_file=2,mksys_anti=3,mksys_item=4,mksys_pointer=5,mksys_class=7;
bool failed(std::uint64_t hr){return (hr&0x80000000U)!=0;}
std::u16string upper(std::u16string_view text){std::u16string out(text);for(auto& c:out)c=unicode_case_unit(c,true);return out;}
std::u16string guid_text(const Guid& g){
    constexpr char16_t digits[]=u"0123456789ABCDEF";constexpr std::array<unsigned,16> order{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};std::u16string out;
    for(unsigned i=0;i<16;++i){if(i==4 || i==6 || i==8 || i==10)out.push_back(u'-');out.push_back(digits[g[order[i]]>>4]);out.push_back(digits[g[order[i]]&15]);}
    return out;
}
void append32(std::vector<std::uint8_t>& out,std::uint32_t v){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(v>>(8*i)));}
void append16(std::vector<std::uint8_t>& out,std::uint16_t v){out.push_back(static_cast<std::uint8_t>(v));out.push_back(static_cast<std::uint8_t>(v>>8));}
// 路徑元件：\\server\share 與 "C:" 各為一個元件，其餘以反斜線分隔（斜線視同反斜線）。
std::vector<std::u16string> path_parts(std::u16string path){
    for(auto& c:path)if(c==u'/')c=u'\\';std::vector<std::u16string> out;std::size_t at=0;
    if(path.starts_with(u"\\\\")){auto end=path.find(u'\\',2);if(end!=path.npos)end=path.find(u'\\',end+1);out.push_back(path.substr(0,end));at=end==path.npos?path.size():end+1;}
    while(at<path.size()){auto end=path.find(u'\\',at);if(end==path.npos)end=path.size();if(end>at)out.push_back(path.substr(at,end-at));else if(!at)out.push_back(u"");at=end+1;}
    return out;
}
std::u16string join_parts(const std::vector<std::u16string>& parts,std::size_t first,std::size_t last){
    std::u16string out;for(auto i=first;i<last;++i){if(i>first)out+=u"\\";out+=parts[i];}return out;
}
bool absolute_path(std::u16string_view path){return path.starts_with(u"\\") || path.starts_with(u"/") || (path.size()>=2 && path[1]==u':');}
bool same_part(const std::u16string& a,const std::u16string& b){return upper(a)==upper(b);}
// item moniker 字串的持久格式：DWORD 長度（ANSI＋Unicode 位元組數）、NUL 結尾 ANSI、只有含非 ASCII 字元時才附加的 Unicode。
void append_item_string(std::vector<std::uint8_t>& out,std::u16string_view text){
    const bool ascii=std::all_of(text.begin(),text.end(),[](char16_t c){return c<0x80;});
    std::vector<std::uint8_t> body;for(const auto c:text)body.push_back(c<0x80?static_cast<std::uint8_t>(c):'?');body.push_back(0);
    if(!ascii)for(const auto c:text){body.push_back(static_cast<std::uint8_t>(c));body.push_back(static_cast<std::uint8_t>(c>>8));}
    append32(out,static_cast<std::uint32_t>(body.size()));out.insert(out.end(),body.begin(),body.end());
}
}
std::span<const WindowsServiceDescriptor> com_moniker_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ole32.dll","CreateItemMoniker",3},{"ole32.dll","CreateClassMoniker",2},{"ole32.dll","CreatePointerMoniker",2},{"ole32.dll","CreateAntiMoniker",1},
        {"ole32.dll","CreateGenericComposite",3},{"ole32.dll","MonikerCommonPrefixWith",3},{"ole32.dll","CreateFileMoniker",2},
        {"ole32.dll","MkParseDisplayName",4},{"ole32.dll","CoGetObject",4},{"ole32.dll","DllGetClassObject",3},{"ole32.dll","Ole32DllGetClassObject",3},
        {"oleaut32.dll","RegisterActiveObject",4,0,0,0x4},{"oleaut32.dll","RevokeActiveObject",2,0,0,0x1},{"oleaut32.dll","GetActiveObject",3},
        {"internal-ole32-moniker","QueryInterface",3},{"internal-ole32-moniker","AddRef",1},{"internal-ole32-moniker","Release",1},
        {"internal-ole32-moniker","GetClassID",2},{"internal-ole32-moniker","IsDirty",1},{"internal-ole32-moniker","Load",2},
        {"internal-ole32-moniker","Save",3,0,0,0x4},{"internal-ole32-moniker","GetSizeMax",2},
        {"internal-ole32-moniker","BindToObject",5},{"internal-ole32-moniker","BindToStorage",5},{"internal-ole32-moniker","Reduce",5,0,0,0x4},
        {"internal-ole32-moniker","ComposeWith",4,0,0,0x4},{"internal-ole32-moniker","Enum",3,0,0,0x2},{"internal-ole32-moniker","IsEqual",2},
        {"internal-ole32-moniker","Hash",2},{"internal-ole32-moniker","IsRunning",4},{"internal-ole32-moniker","GetTimeOfLastChange",4},
        {"internal-ole32-moniker","Inverse",2},{"internal-ole32-moniker","CommonPrefixWith",3},{"internal-ole32-moniker","RelativePathTo",3},
        {"internal-ole32-moniker","GetDisplayName",4},{"internal-ole32-moniker","ParseDisplayName",6},{"internal-ole32-moniker","IsSystemMoniker",2},
        {"internal-ole32-moniker-rotdata","QueryInterface",3},{"internal-ole32-moniker-rotdata","AddRef",1},{"internal-ole32-moniker-rotdata","Release",1},
        {"internal-ole32-moniker-rotdata","GetComparisonData",4,0,0,0x4},
        {"internal-ole32-moniker-enum","QueryInterface",3},{"internal-ole32-moniker-enum","AddRef",1},{"internal-ole32-moniker-enum","Release",1},
        {"internal-ole32-moniker-enum","Next",4,0,0,0x2},{"internal-ole32-moniker-enum","Skip",2,0,0,0x2},{"internal-ole32-moniker-enum","Reset",1},
        {"internal-ole32-moniker-enum","Clone",2},
        {"internal-ole32-moniker-factory","QueryInterface",3},{"internal-ole32-moniker-factory","AddRef",1},{"internal-ole32-moniker-factory","Release",1},
        {"internal-ole32-moniker-factory","CreateInstance",4},{"internal-ole32-moniker-factory","LockServer",2,0,0,0x2}};
    return table;
}
namespace {
// 系統 moniker 類別：CoCreateInstance／class factory 建立空的 moniker，之後以 IPersistStream::Load 填入。
std::uint8_t moniker_kind_of_class(const Guid& clsid){
    if(clsid==clsid_file)return mksys_file;if(clsid==clsid_item)return mksys_item;if(clsid==clsid_anti)return mksys_anti;
    if(clsid==clsid_pointer)return mksys_pointer;if(clsid==clsid_composite)return mksys_composite;if(clsid==clsid_class)return mksys_class;return 0;
}
}
std::uint64_t WindowsServices::com_moniker_table(GuestMemory& memory,unsigned which){
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    auto& table=com_moniker_vtables_.at(which);if(table)return table;
    static constexpr std::array<std::string_view,4> families{"internal-ole32-moniker","internal-ole32-moniker-rotdata","internal-ole32-moniker-enum","internal-ole32-moniker-factory"};
    std::vector<std::uint64_t> entries;for(const auto& d:com_moniker_service_descriptors())if(d.family==families[which])entries.push_back(resolve(d.family,d.symbol).value);
    table=runtime_allocate(memory,{},entries.size()*unit);for(std::size_t i=0;i<entries.size();++i)memory.store(GuestAddress{table+i*unit},bits,entries[i]);return table;
}
std::uint64_t WindowsServices::create_com_moniker(GuestMemory& memory,ComMoniker value){
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(com_monikers_.size()>=65536)throw std::bad_alloc{};
    const auto object=allocate_heap(memory,2*unit);if(!object)throw std::bad_alloc{};
    memory.store(GuestAddress{object},bits,com_moniker_table(memory,0));memory.store(GuestAddress{object+unit},bits,com_moniker_table(memory,1));
    value.references=1;com_monikers_.emplace(object,std::move(value));return object;
}
// CoCreateInstance／CoGetClassObject 的系統 moniker 類別（同步完成，與其他內建類別共用分派鏈）。
bool WindowsServices::begin_moniker_com(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);if(!arg(0))return false;
    Guid clsid{};memory.read(GuestAddress{arg(0)},clsid);const auto kind=moniker_kind_of_class(clsid);if(!kind)return false;
    const bool create=service.symbol=="CoCreateInstance";const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto output=arg(4);const auto done=[&](std::uint32_t hr){arg.return_to(state,hr);return true;};
    if(!output)return done(e_pointer);memory.store(GuestAddress{output},bits,0);if(!arg(3))return done(e_invalidarg);
    if(!(static_cast<std::uint32_t>(arg(create?2:1))&1U))return done(regdb_e_classnotreg); // 只有 in-process 伺服器
    Guid iid{};memory.read(GuestAddress{arg(3)},iid);
    if(!create){
        if(iid!=iid_unknown && iid!=iid_class_factory)return done(e_nointerface);
        const auto factory=allocate_heap(memory,unit);if(!factory)return done(e_outofmemory);
        memory.store(GuestAddress{factory},bits,com_moniker_table(memory,3));com_moniker_factories_.emplace(factory,std::pair<std::uint8_t,std::uint32_t>{kind,1});
        memory.store(GuestAddress{output},bits,factory);return done(s_ok);
    }
    if(arg(1))return done(class_e_noaggregation);
    const bool moniker=iid==iid_unknown || iid==iid_moniker || iid==iid_persist || iid==iid_persist_stream;if(!moniker && iid!=iid_rot_data)return done(e_nointerface);
    ComMoniker value;value.kind=kind;const auto object=create_com_moniker(memory,std::move(value));
    memory.store(GuestAddress{output},bits,moniker?object:object+unit);return done(s_ok);
}
std::optional<std::uint32_t> WindowsServices::invoke_com_monikers(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"OLE moniker",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        using service_callback::Task;using Pair=std::pair<std::uint32_t,std::uint64_t>;
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto family=service.family;const auto name=service.symbol;
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
        const auto out_cell=[&]{const auto cell=scratch(unit);return cell;};
        const auto vtable=[&](unsigned which){return com_moniker_table(m,which);};
        const auto ours=[&](std::uint64_t p)->ComMoniker*{
            if(!p || !com_moniker_vtables_[0] || m.load(GuestAddress{p},bits)!=com_moniker_vtables_[0])return nullptr;
            const auto found=com_monikers_.find(p);return found==com_monikers_.end()?nullptr:&found->second;};
        const auto create=[&](ComMoniker value){return create_com_moniker(m,std::move(value));};
        const auto addref=[&](std::uint64_t p)->Task<std::uint32_t>{if(!p)co_return 0;if(auto* k=ours(p))co_return ++k->references;co_return static_cast<std::uint32_t>(co_await com(p,1,{}));};
        // 釋放：自己的物件以工作清單逐一拆解（元件與包裝的指標），避免遞迴。
        const auto release=[&](std::uint64_t p)->Task<std::uint32_t>{
            std::vector<std::uint64_t> pending{p};std::uint32_t first=0;bool head=true;
            while(!pending.empty()){
                const auto item=pending.back();pending.pop_back();if(!item){head=false;continue;}
                if(auto* k=ours(item)){
                    const auto left=--k->references;if(head)first=left;head=false;
                    if(!left){auto value=std::move(*k);com_monikers_.erase(item);(void)free_heap(m,item,s);
                        for(const auto part:value.parts)pending.push_back(part);if(value.pointer)pending.push_back(value.pointer);}
                }else {const auto left=static_cast<std::uint32_t>(co_await com(item,2,{}));if(head)first=left;head=false;}
            }
            co_return first;
        };
        const auto release_all=[&](const std::vector<std::uint64_t>& items)->Task<std::uint32_t>{for(const auto p:items)(void)co_await release(p);co_return 0;};
        const auto kind_of=[&](std::uint64_t p)->Task<std::uint8_t>{
            if(!p)co_return 0;if(auto* k=ours(p))co_return k->kind;
            const auto cell=scratch(4);if(failed(co_await com(p,22,{cell})))co_return 0;co_return static_cast<std::uint8_t>(m.load(GuestAddress{cell},32));};
        // 元件清單（每個元件都已 AddRef，呼叫者負責釋放）：composite 為元件，其他為自己。
        const auto parts_of=[&](std::uint64_t p)->Task<std::vector<std::uint64_t>>{
            std::vector<std::uint64_t> out;if(!p)co_return out;
            if(auto* k=ours(p)){if(k->kind==mksys_composite){out=k->parts;for(const auto part:out)(void)co_await addref(part);}else {(void)co_await addref(p);out.push_back(p);}co_return out;}
            if(co_await kind_of(p)==mksys_composite){
                const auto enum_at=out_cell();
                if(!failed(co_await com(p,12,{1,enum_at}))){const auto enumerator=m.load(GuestAddress{enum_at},bits);
                    if(enumerator){const auto cell=out_cell(),fetched=scratch(4);
                        for(unsigned guard=0;guard<4096;++guard){m.store(GuestAddress{cell},bits,0);if(co_await com(enumerator,3,{1,cell,fetched})!=s_ok)break;out.push_back(m.load(GuestAddress{cell},bits));}
                        (void)co_await com(enumerator,2,{});}}
                if(!out.empty())co_return out;
            }
            (void)co_await addref(p);out.push_back(p);co_return out;
        };
        // 單一（非 composite）moniker 的相等判定；客體 moniker 交給它自己的 IsEqual。
        const auto equal_simple=[&](std::uint64_t a,std::uint64_t b)->Task<bool>{
            if(a==b)co_return true;
            auto* x=ours(a);auto* y=ours(b);
            if(x && y){
                if(x->kind!=y->kind)co_return false;
                if(x->kind==mksys_item || x->kind==mksys_file)co_return upper(x->delimiter+x->name)==upper(y->delimiter+y->name);
                if(x->kind==mksys_class)co_return x->clsid==y->clsid;
                if(x->kind==mksys_pointer)co_return x->pointer==y->pointer;
                if(x->kind==mksys_anti)co_return x->count==y->count;
                co_return false;
            }
            if(!x)co_return co_await com(a,13,{b})==s_ok;
            // 自己的 moniker 與客體 moniker：同類型時以顯示名稱（不分大小寫）比較。
            if(co_await kind_of(b)!=x->kind || x->kind==mksys_pointer)co_return false;
            const auto context=out_cell();if(failed(co_await api("ole32.dll","CreateBindCtx",{0,context})))co_return false;
            const auto bind=m.load(GuestAddress{context},bits);const auto cell=out_cell();bool same=false;
            if(!failed(co_await com(b,20,{bind,0,cell}))){const auto pointer=m.load(GuestAddress{cell},bits);
                if(pointer){const auto other=read_utf16(m,GuestAddress{pointer},32768);(void)co_await api("ole32.dll","CoTaskMemFree",{pointer});
                    const auto mine=x->kind==mksys_item || x->kind==mksys_file?x->delimiter+x->name:x->kind==mksys_class?u"clsid:"+guid_text(x->clsid)+u":":std::u16string{};
                    std::u16string anti;for(std::uint32_t i=0;i<x->count;++i)anti+=u"\\..";
                    same=upper(other)==upper(x->kind==mksys_anti?anti:mine);}}
            (void)co_await com(bind,2,{});co_return same;
        };
        const auto equal=[&](std::uint64_t a,std::uint64_t b)->Task<bool>{
            if(!a || !b)co_return a==b;
            const auto ka=co_await kind_of(a),kb=co_await kind_of(b);
            if(ka!=mksys_composite && kb!=mksys_composite)co_return co_await equal_simple(a,b);
            if(ka!=kb)co_return false;
            auto pa=co_await parts_of(a);auto pb=co_await parts_of(b);bool same=pa.size()==pb.size();
            for(std::size_t i=0;same && i<pa.size();++i)same=co_await equal_simple(pa[i],pb[i]);
            (void)co_await release_all(pa);(void)co_await release_all(pb);co_return same;
        };
        const auto make_composite=[&](std::vector<std::uint64_t> parts)->std::uint64_t{
            if(parts.empty())return 0;if(parts.size()==1)return parts.front();
            ComMoniker value;value.kind=mksys_composite;value.parts=std::move(parts);return create(std::move(value));};
        // 非泛型組合（fOnlyIfNotGeneric＝TRUE）：自己的 item／class／pointer 與右側 anti 互相抵銷（anti 個數大於 1 時減一）。
        const auto compose_nongeneric=[&](std::uint64_t left,std::uint64_t right)->Task<Pair>{
            auto* k=ours(left);
            if(!k){const auto cell=out_cell();const auto hr=static_cast<std::uint32_t>(co_await com(left,11,{right,1,cell}));co_return Pair{hr,failed(hr)?0:m.load(GuestAddress{cell},bits)};}
            if(k->kind==mksys_composite || k->kind==mksys_anti)co_return Pair{mk_e_needgeneric,0};
            const auto rk=co_await kind_of(right);
            if(k->kind==mksys_file && rk==mksys_file){
                // 相對 file 組合：右側每個 ".." 去掉左側一段，其餘接在後面；右側是絕對路徑時不能組合（MK_E_SYNTAX）。
                auto* r=ours(right);if(!r)co_return Pair{mk_e_needgeneric,0};
                if(absolute_path(r->name))co_return Pair{mk_e_syntax,0};
                auto parts=path_parts(k->name);std::size_t anti=0;
                for(const auto& piece:path_parts(r->name)){if(piece==u".."){if(parts.empty())++anti;else parts.pop_back();}else if(piece!=u".")parts.push_back(piece);}
                ComMoniker joined;joined.kind=mksys_file;for(std::size_t i=0;i<anti;++i)joined.name+=u"..\\";joined.name+=join_parts(parts,0,parts.size());
                co_return Pair{s_ok,create(std::move(joined))};
            }
            if(rk==mksys_anti){
                if(auto* a=ours(right);a && a->count>1){ComMoniker rest;rest.kind=mksys_anti;rest.count=a->count-1;co_return Pair{s_ok,create(std::move(rest))};}
                co_return Pair{s_ok,0};
            }
            if(rk==mksys_composite){
                auto parts=co_await parts_of(right);
                if(!parts.empty() && co_await kind_of(parts.front())==mksys_anti){
                    const auto first=parts.front();parts.erase(parts.begin());
                    if(auto* a=ours(first);a && a->count>1){ComMoniker rest;rest.kind=mksys_anti;rest.count=a->count-1;parts.insert(parts.begin(),create(std::move(rest)));}
                    (void)co_await release(first);co_return Pair{s_ok,make_composite(std::move(parts))};
                }
                (void)co_await release_all(parts);
            }
            co_return Pair{mk_e_needgeneric,0};
        };
        const auto generic=[&](std::uint64_t left,std::uint64_t right)->Task<Pair>{
            if(!left && !right)co_return Pair{e_invalidarg,0};
            if(!left || !right){const auto only=left?left:right;(void)co_await addref(only);co_return Pair{s_ok,only};}
            auto lp=co_await parts_of(left);auto rp=co_await parts_of(right);
            while(!lp.empty() && !rp.empty()){
                const auto [hr,merged]=co_await compose_nongeneric(lp.back(),rp.front());
                if(failed(hr))break;
                (void)co_await release(lp.back());lp.pop_back();(void)co_await release(rp.front());rp.erase(rp.begin());
                if(merged){auto mp=co_await parts_of(merged);(void)co_await release(merged);rp.insert(rp.begin(),mp.begin(),mp.end());}
            }
            lp.insert(lp.end(),rp.begin(),rp.end());co_return Pair{s_ok,make_composite(std::move(lp))};
        };
        const auto rot_of=[&](std::uint64_t pbc)->Task<Pair>{
            const auto cell=out_cell();
            const auto hr=static_cast<std::uint32_t>(pbc?co_await com(pbc,8,{cell}):co_await api("ole32.dll","GetRunningObjectTable",{0,cell}));
            co_return Pair{hr,failed(hr)?0:m.load(GuestAddress{cell},bits)};
        };
        const auto hash_of=[&](std::uint64_t p)->Task<std::uint32_t>{
            auto* k=ours(p);
            if(!k){const auto cell=scratch(4);(void)co_await com(p,14,{cell});co_return static_cast<std::uint32_t>(m.load(GuestAddress{cell},32));}
            std::uint32_t h=k->kind;
            if(k->kind==mksys_item || k->kind==mksys_file)for(const auto c:upper(k->delimiter+k->name))h=h*31+c;
            else if(k->kind==mksys_class)for(const auto b:k->clsid)h=h*31+b;
            else if(k->kind==mksys_pointer)h^=static_cast<std::uint32_t>(k->pointer^(k->pointer>>32));
            else if(k->kind==mksys_anti)h=h*31+k->count;
            co_return h;
        };
        const auto alloc_text=[&](std::u16string_view value,std::uint64_t output)->Task<std::uint32_t>{
            const auto data=utf16_bytes(value);const auto pointer=co_await api("ole32.dll","CoTaskMemAlloc",{data.size()});if(!pointer)co_return e_outofmemory;
            m.write(GuestAddress{pointer},data);m.store(GuestAddress{output},bits,pointer);co_return s_ok;};
        try{
            // ---- 匯出函式 ----
            if(family=="ole32.dll" || family=="oleaut32.dll"){
                if(name=="CreateItemMoniker" || name=="CreateClassMoniker" || name=="CreatePointerMoniker" || name=="CreateAntiMoniker"){
                    const auto output=call(name=="CreateAntiMoniker"?0:name=="CreateItemMoniker"?2:1);if(!output)co_return finish(e_invalidarg);
                    m.store(GuestAddress{output},bits,0);ComMoniker value;
                    if(name=="CreateItemMoniker"){value.kind=mksys_item;if(call(0))value.delimiter=read_utf16(m,GuestAddress{call(0)},32767);if(call(1))value.name=read_utf16(m,GuestAddress{call(1)},32767);}
                    else if(name=="CreateClassMoniker"){if(!call(0))co_return finish(e_invalidarg);value.kind=mksys_class;m.read(GuestAddress{call(0)},value.clsid);}
                    else if(name=="CreatePointerMoniker"){value.kind=mksys_pointer;value.pointer=call(0);if(value.pointer)(void)co_await addref(value.pointer);}
                    else value.kind=mksys_anti;
                    m.store(GuestAddress{output},bits,create(std::move(value)));co_return finish(s_ok);
                }
                if(name=="CreateFileMoniker"){
                    if(!call(1))co_return finish(e_invalidarg);m.store(GuestAddress{call(1)},bits,0);if(!call(0))co_return finish(e_invalidarg);
                    ComMoniker value;value.kind=mksys_file;value.name=read_utf16(m,GuestAddress{call(0)},32767);for(auto& c:value.name)if(c==u'/')c=u'\\';
                    m.store(GuestAddress{call(1)},bits,create(std::move(value)));co_return finish(s_ok);
                }
                if(name=="DllGetClassObject" || name=="Ole32DllGetClassObject"){
                    // (rclsid, riid, ppv)：系統 moniker 類別的 IClassFactory。
                    if(!call(2))co_return finish(e_pointer);m.store(GuestAddress{call(2)},bits,0);if(!call(0) || !call(1))co_return finish(e_invalidarg);
                    Guid clsid{},iid{};m.read(GuestAddress{call(0)},clsid);m.read(GuestAddress{call(1)},iid);const auto kind=moniker_kind_of_class(clsid);
                    if(!kind)co_return finish(class_e_classnotavailable);if(iid!=iid_unknown && iid!=iid_class_factory)co_return finish(e_nointerface);
                    const auto factory=allocate_heap(m,unit);if(!factory)co_return finish(e_outofmemory);
                    m.store(GuestAddress{factory},bits,vtable(3));com_moniker_factories_.emplace(factory,std::pair<std::uint8_t,std::uint32_t>{kind,1});
                    m.store(GuestAddress{call(2)},bits,factory);co_return finish(s_ok);
                }
                if(name=="MkParseDisplayName" || name=="CoGetObject"){
                    // MkParseDisplayName(pbc, szUserName, pchEaten, ppmk)／CoGetObject(pszName, pBindOptions, riid, ppv)
                    const bool object=name=="CoGetObject";
                    const auto output=call(3),display=call(object?0:1);if(!output)co_return finish(e_invalidarg);m.store(GuestAddress{output},bits,0);
                    if(!display)co_return finish(e_invalidarg);
                    std::uint64_t pbc=object?0:call(0);const auto eaten=object?scratch(4):call(2);if(!pbc && !object)co_return finish(e_invalidarg);if(!eaten)co_return finish(e_invalidarg);
                    m.store(GuestAddress{eaten},32,0);
                    if(object){const auto context=out_cell();if(const auto hr=static_cast<std::uint32_t>(co_await api("ole32.dll","CreateBindCtx",{0,context}));failed(hr))co_return finish(hr);pbc=m.load(GuestAddress{context},bits);
                        if(call(1))if(const auto hr=static_cast<std::uint32_t>(co_await com(pbc,6,{call(1)}));failed(hr)){(void)co_await com(pbc,2,{});co_return finish(hr);}}
                    const auto whole=read_utf16(m,GuestAddress{display},32767);
                    // 解析：@ProgID 或「ProgID:」→ 類別的 IParseDisplayName；"clsid:" → class moniker；否則最長的現存檔案路徑 → file moniker。
                    std::uint32_t hr=mk_e_syntax;std::uint64_t result=0;std::size_t consumed=0;
                    const auto class_parser=[&](std::u16string progid)->Task<std::uint32_t>{
                        const auto clsid=scratch(16);if(failed(co_await api("ole32.dll","CLSIDFromProgID",{text(progid),clsid})))co_return mk_e_syntax;
                        const auto cell=out_cell();const auto got=static_cast<std::uint32_t>(co_await api("ole32.dll","CoGetClassObject",{clsid,0x17,0,bytes(iid_parse_display_name),cell}));
                        if(failed(got))co_return got;const auto parser=m.load(GuestAddress{cell},bits);const auto moniker_at=out_cell();
                        const auto parsed=static_cast<std::uint32_t>(co_await com(parser,3,{pbc,display,eaten,moniker_at}));(void)co_await com(parser,2,{});
                        if(!failed(parsed))result=m.load(GuestAddress{moniker_at},bits);co_return parsed;
                    };
                    const auto progid_end=[&](std::size_t from){std::size_t at=from;while(at<whole.size() && (std::isalnum(whole[at]<0x80?static_cast<int>(whole[at]):0) || whole[at]==u'.' || whole[at]==u'_' || whole[at]==u'-'))++at;return at;};
                    bool handled=false;
                    if(whole.starts_with(u"@")){handled=true;hr=co_await class_parser(whole.substr(1,progid_end(1)-1));consumed=m.load(GuestAddress{eaten},32);}
                    else if(upper(whole.substr(0,6))==u"CLSID:"){
                        handled=true;auto guid=whole.substr(6);const auto colon=guid.find(u':');if(colon!=guid.npos)guid.resize(colon);
                        const auto clsid=scratch(16);
                        if(!failed(co_await api("ole32.dll","CLSIDFromString",{text(guid.starts_with(u"{")?guid:u"{"+guid+u"}"),clsid}))){
                            ComMoniker value;value.kind=mksys_class;m.read(GuestAddress{clsid},value.clsid);result=create(std::move(value));
                            consumed=6+guid.size()+(colon!=whole.substr(6).npos?1:0);hr=s_ok;
                        }
                    }else if(const auto end=progid_end(0);end && end<whole.size() && whole[end]==u':' && end>1){
                        const auto clsid=scratch(16);
                        if(!failed(co_await api("ole32.dll","CLSIDFromProgID",{text(whole.substr(0,end)),clsid}))){handled=true;hr=co_await class_parser(whole.substr(0,end));consumed=m.load(GuestAddress{eaten},32);}
                    }
                    if(!handled){
                        // 候選邊界：每個 '!' 前與字串結尾，由長到短。
                        std::vector<std::size_t> cuts{whole.size()};for(std::size_t i=whole.size();i-- >0;)if(whole[i]==u'!')cuts.push_back(i);
                        for(const auto cut:cuts){if(!cut)continue;
                            if(static_cast<std::uint32_t>(co_await api("kernel32.dll","GetFileAttributesW",{text(whole.substr(0,cut))}))==0xffffffffU)continue;
                            ComMoniker value;value.kind=mksys_file;value.name=whole.substr(0,cut);result=create(std::move(value));consumed=cut;hr=s_ok;break;}
                    }
                    // 剩餘部分交給目前 moniker 的 ParseDisplayName，逐段組合。
                    while(!failed(hr) && result && !handled && consumed<whole.size()){
                        const auto piece=scratch(4);const auto next_at=out_cell();
                        const auto parsed=static_cast<std::uint32_t>(co_await com(result,21,{pbc,0,display+consumed*2,piece,next_at}));
                        const auto count=m.load(GuestAddress{piece},32);const auto next=m.load(GuestAddress{next_at},bits);
                        if(failed(parsed) || !count){hr=failed(parsed)?parsed:mk_e_syntax;(void)co_await release(next);break;}
                        const auto [chr,composite]=co_await generic(result,next);(void)co_await release(result);(void)co_await release(next);result=failed(chr)?0:composite;consumed+=count;
                    }
                    if(handled && !failed(hr) && result && consumed<whole.size() && upper(whole.substr(0,6))==u"CLSID:"){
                        const auto piece=scratch(4);const auto next_at=out_cell();
                        const auto parsed=static_cast<std::uint32_t>(co_await com(result,21,{pbc,0,display+consumed*2,piece,next_at}));
                        if(failed(parsed))hr=parsed;else{const auto next=m.load(GuestAddress{next_at},bits);const auto [chr,composite]=co_await generic(result,next);(void)co_await release(result);(void)co_await release(next);result=failed(chr)?0:composite;consumed+=m.load(GuestAddress{piece},32);}
                    }
                    m.store(GuestAddress{eaten},32,consumed);
                    if(failed(hr)){(void)co_await release(result);if(object)(void)co_await com(pbc,2,{});co_return finish(hr);}
                    if(!object){m.store(GuestAddress{output},bits,result);co_return finish(s_ok);}
                    const auto bound=co_await com(result,8,{pbc,0,call(2),output});(void)co_await release(result);(void)co_await com(pbc,2,{});co_return finish(bound);
                }
                if(name=="CreateGenericComposite"){
                    if(!call(2))co_return finish(e_invalidarg);m.store(GuestAddress{call(2)},bits,0);
                    const auto [hr,result]=co_await generic(call(0),call(1));if(!failed(hr))m.store(GuestAddress{call(2)},bits,result);co_return finish(hr);
                }
                if(name=="MonikerCommonPrefixWith"){
                    // (pmkThis, pmkOther, ppmkCommon)：元件由左至右比較共同前綴。
                    if(!call(2))co_return finish(e_invalidarg);m.store(GuestAddress{call(2)},bits,0);if(!call(0) || !call(1))co_return finish(e_invalidarg);
                    auto a=co_await parts_of(call(0));auto b=co_await parts_of(call(1));std::size_t n=0;
                    while(n<a.size() && n<b.size() && co_await equal_simple(a[n],b[n]))++n;
                    std::uint32_t hr=mk_e_noprefix;std::uint64_t result=0;
                    if(n==a.size() && n==b.size()){hr=mk_s_us;result=call(0);(void)co_await addref(result);}
                    else if(n && n==b.size()){hr=mk_s_him;result=call(1);(void)co_await addref(result);}
                    else if(n && n==a.size()){hr=mk_s_me;result=call(0);(void)co_await addref(result);}
                    else if(n){hr=s_ok;std::vector<std::uint64_t> prefix(a.begin(),a.begin()+static_cast<std::ptrdiff_t>(n));for(const auto p:prefix)(void)co_await addref(p);result=make_composite(std::move(prefix));}
                    (void)co_await release_all(a);(void)co_await release_all(b);
                    m.store(GuestAddress{call(2)},bits,result);co_return finish(hr);
                }
                if(name=="RegisterActiveObject" || name=="GetActiveObject" || name=="RevokeActiveObject"){
                    // (punk, rclsid, dwFlags, pdwRegister)／(rclsid, pvReserved, ppunk)／(dwRegister, pvReserved)：item moniker "!" + "{CLSID}"。
                    const auto [hr,rot]=co_await rot_of(0);if(failed(hr))co_return finish(hr);
                    std::uint32_t result=0;
                    if(name=="RevokeActiveObject")result=static_cast<std::uint32_t>(co_await com(rot,4,{static_cast<std::uint32_t>(call(0))}));
                    else{
                        const auto clsid_at=call(name=="RegisterActiveObject"?1:0);const auto output=call(name=="RegisterActiveObject"?3:2);
                        if(!clsid_at || (name=="RegisterActiveObject" && !call(0)))result=e_invalidarg;
                        else{
                            Guid clsid{};m.read(GuestAddress{clsid_at},clsid);ComMoniker value;value.kind=mksys_item;value.delimiter=u"!";value.name=u"{"+guid_text(clsid)+u"}";
                            const auto moniker=create(std::move(value));
                            if(name=="RegisterActiveObject"){const auto weak=static_cast<std::uint32_t>(call(2))==1;
                                result=static_cast<std::uint32_t>(co_await com(rot,3,{weak?0U:1U,call(0),moniker,output?output:scratch(4)}));} // ROTFLAGS_REGISTRATIONKEEPSALIVE
                            else{if(output)m.store(GuestAddress{output},bits,0);result=output?static_cast<std::uint32_t>(co_await com(rot,6,{moniker,output})):e_invalidarg;}
                            (void)co_await release(moniker);
                        }
                    }
                    (void)co_await com(rot,2,{});co_return finish(result);
                }
                throw HostCallFault("未知的 moniker 匯出");
            }
            // ---- 介面方法 ----
            const auto self_pointer=call(0);const auto table=m.load(GuestAddress{self_pointer},bits);
            if(family=="internal-ole32-moniker-factory"){
                const auto found=com_moniker_factories_.find(self_pointer);if(table!=com_moniker_vtables_[3] || found==com_moniker_factories_.end())co_return finish(e_unexpected);
                if(name=="QueryInterface"){
                    if(!call(2))co_return finish(e_pointer);Guid iid{};m.read(GuestAddress{call(1)},iid);
                    if(iid!=iid_unknown && iid!=iid_class_factory){m.store(GuestAddress{call(2)},bits,0);co_return finish(e_nointerface);}
                    ++found->second.second;m.store(GuestAddress{call(2)},bits,self_pointer);co_return finish(s_ok);
                }
                if(name=="AddRef")co_return finish(++found->second.second);
                if(name=="Release"){const auto left=--found->second.second;if(!left){com_moniker_factories_.erase(found);(void)free_heap(m,self_pointer,s);}co_return finish(left);}
                if(name=="LockServer")co_return finish(s_ok);
                // CreateInstance(pUnkOuter, riid, ppvObject)
                if(!call(3))co_return finish(e_pointer);m.store(GuestAddress{call(3)},bits,0);if(call(1))co_return finish(class_e_noaggregation);if(!call(2))co_return finish(e_invalidarg);
                Guid iid{};m.read(GuestAddress{call(2)},iid);
                const bool moniker=iid==iid_unknown || iid==iid_moniker || iid==iid_persist || iid==iid_persist_stream;if(!moniker && iid!=iid_rot_data)co_return finish(e_nointerface);
                ComMoniker value;value.kind=found->second.first;const auto object=create(std::move(value));m.store(GuestAddress{call(3)},bits,moniker?object:object+unit);co_return finish(s_ok);
            }
            if(family=="internal-ole32-moniker-enum"){
                const auto found=com_moniker_enums_.find(self_pointer);if(table!=com_moniker_vtables_[2] || found==com_moniker_enums_.end())co_return finish(e_unexpected);
                if(name=="QueryInterface"){
                    if(!call(2))co_return finish(e_pointer);Guid iid{};m.read(GuestAddress{call(1)},iid);
                    if(iid!=iid_unknown && iid!=iid_enum_moniker){m.store(GuestAddress{call(2)},bits,0);co_return finish(e_nointerface);}
                    ++found->second.references;m.store(GuestAddress{call(2)},bits,self_pointer);co_return finish(s_ok);
                }
                if(name=="AddRef")co_return finish(++found->second.references);
                if(name=="Release"){const auto left=--found->second.references;if(!left){auto items=std::move(found->second.items);com_moniker_enums_.erase(found);(void)free_heap(m,self_pointer,s);(void)co_await release_all(items);}co_return finish(left);}
                auto& e=com_moniker_enums_.at(self_pointer);
                if(name=="Next"){
                    // (celt, rgelt, pceltFetched)
                    const auto count=static_cast<std::uint32_t>(call(1));if(!call(2) || (count>1 && !call(3)))co_return finish(e_invalidarg);std::uint32_t got=0;
                    while(got<count && e.position<e.items.size()){const auto item=e.items[e.position++];(void)co_await addref(item);m.store(GuestAddress{call(2)+std::uint64_t{got}*unit},bits,item);++got;}
                    if(call(3))m.store(GuestAddress{call(3)},32,got);co_return finish(got==count?s_ok:s_false);
                }
                if(name=="Skip"){const auto count=static_cast<std::uint32_t>(call(1));const auto skip=std::min<std::size_t>(count,e.items.size()-e.position);e.position+=skip;co_return finish(skip==count?s_ok:s_false);}
                if(name=="Reset"){e.position=0;co_return finish(s_ok);}
                if(name=="Clone"){
                    if(!call(1))co_return finish(e_invalidarg);ComMonikerEnum copy{e.items,e.position,1};for(const auto item:copy.items)(void)co_await addref(item);
                    const auto object=allocate_heap(m,unit);if(!object)co_return finish(e_outofmemory);m.store(GuestAddress{object},bits,vtable(2));com_moniker_enums_.emplace(object,std::move(copy));
                    m.store(GuestAddress{call(1)},bits,object);co_return finish(s_ok);
                }
                throw HostCallFault("未知的 IEnumMoniker 方法");
            }
            const auto base=table==com_moniker_vtables_[0]?self_pointer:table==com_moniker_vtables_[1]?self_pointer-unit:0;
            if(!base || !com_monikers_.contains(base))co_return finish(e_unexpected);
            if(name=="QueryInterface"){
                if(!call(2))co_return finish(e_pointer);m.store(GuestAddress{call(2)},bits,0);if(!call(1))co_return finish(e_invalidarg);
                Guid iid{};m.read(GuestAddress{call(1)},iid);
                const auto pointer=iid==iid_unknown || iid==iid_moniker || iid==iid_persist || iid==iid_persist_stream?base:iid==iid_rot_data?base+unit:0;
                if(!pointer)co_return finish(e_nointerface);++com_monikers_.at(base).references;m.store(GuestAddress{call(2)},bits,pointer);co_return finish(s_ok);
            }
            if(name=="AddRef")co_return finish(++com_monikers_.at(base).references);
            if(name=="Release")co_return finish(co_await release(base));
            const auto me=com_monikers_.at(base);  // 複本：呼叫客體期間映射表可能改變
            const auto kind=me.kind;
            const auto comparison=[&](std::uint64_t p)->Task<std::pair<bool,std::vector<std::uint8_t>>>{
                std::vector<std::uint8_t> out;
                if(auto* k=ours(p)){
                    const auto& id=k->kind==mksys_file?clsid_file:k->kind==mksys_item?clsid_item:k->kind==mksys_class?clsid_class:k->kind==mksys_pointer?clsid_pointer:k->kind==mksys_anti?clsid_anti:clsid_composite;
                    out.insert(out.end(),id.begin(),id.end());
                    if(k->kind==mksys_item || k->kind==mksys_file){const auto data=utf16_bytes(upper(k->delimiter+k->name));out.insert(out.end(),data.begin(),data.end());}
                    else if(k->kind==mksys_class)out.insert(out.end(),k->clsid.begin(),k->clsid.end());
                    else if(k->kind==mksys_anti)append32(out,k->count);
                    else if(k->kind==mksys_pointer){append32(out,static_cast<std::uint32_t>(k->pointer));append32(out,static_cast<std::uint32_t>(k->pointer>>32));}
                    co_return std::pair{true,std::move(out)};
                }
                const auto cell=out_cell();if(failed(co_await com(p,0,{bytes(iid_rot_data),cell})))co_return std::pair{false,std::move(out)};
                const auto data=m.load(GuestAddress{cell},bits);const auto buffer=scratch(2048),length=scratch(4);
                const auto hr=co_await com(data,3,{buffer,2048,length});(void)co_await com(data,2,{});if(failed(hr))co_return std::pair{false,std::move(out)};
                out.resize(std::min<std::uint64_t>(m.load(GuestAddress{length},32),2048));m.read(GuestAddress{buffer},std::span<std::uint8_t>(out));co_return std::pair{true,std::move(out)};
            };
            if(family=="internal-ole32-moniker-rotdata"){
                if(name!="GetComparisonData")throw HostCallFault("未知的 IROTData 方法");
                // (pbData, cbMax, pcbData)：composite 串接各元件（各自前置長度）。
                std::vector<std::uint8_t> data;
                if(kind==mksys_composite){data.insert(data.end(),clsid_composite.begin(),clsid_composite.end());
                    for(const auto part:me.parts){auto [ok,piece]=co_await comparison(part);if(!ok)co_return finish(e_notimpl);append32(data,static_cast<std::uint32_t>(piece.size()));data.insert(data.end(),piece.begin(),piece.end());}}
                else data=(co_await comparison(base)).second;
                if(!call(3))co_return finish(e_invalidarg);m.store(GuestAddress{call(3)},32,data.size());
                if(data.size()>static_cast<std::uint32_t>(call(2)))co_return finish(e_outofmemory);if(!call(1))co_return finish(e_invalidarg);
                m.write(GuestAddress{call(1)},data);co_return finish(s_ok);
            }
            const auto& my_clsid=kind==mksys_file?clsid_file:kind==mksys_item?clsid_item:kind==mksys_class?clsid_class:kind==mksys_pointer?clsid_pointer:kind==mksys_anti?clsid_anti:clsid_composite;
            const auto display_of=[&](std::uint64_t p,std::uint64_t pbc,std::uint64_t left)->Task<std::pair<std::uint32_t,std::u16string>>{
                if(auto* k=ours(p)){
                    if(k->kind==mksys_item || k->kind==mksys_file)co_return std::pair{s_ok,k->delimiter+k->name};
                    if(k->kind==mksys_class)co_return std::pair{s_ok,u"clsid:"+guid_text(k->clsid)+u":"};
                    if(k->kind==mksys_anti){std::u16string out;for(std::uint32_t i=0;i<k->count;++i)out+=u"\\..";co_return std::pair{s_ok,out};}
                    if(k->kind==mksys_pointer)co_return std::pair{e_notimpl,std::u16string{}};
                }
                const auto cell=out_cell();const auto hr=static_cast<std::uint32_t>(co_await com(p,20,{pbc,left,cell}));if(failed(hr))co_return std::pair{hr,std::u16string{}};
                const auto pointer=m.load(GuestAddress{cell},bits);std::u16string out=pointer?read_utf16(m,GuestAddress{pointer},65536):std::u16string{};
                if(pointer)(void)co_await api("ole32.dll","CoTaskMemFree",{pointer});co_return std::pair{s_ok,out};
            };
            // composite 的左側部分（左側 moniker＋除了最後一個以外的元件）與最右元件。
            const auto split=[&](std::uint64_t left)->Task<std::pair<std::uint64_t,std::uint64_t>>{
                std::vector<std::uint64_t> rest(me.parts.begin(),me.parts.end()-1);for(const auto p:rest)(void)co_await addref(p);
                auto rest_moniker=make_composite(std::move(rest));
                if(left){const auto [hr,full]=co_await generic(left,rest_moniker);(void)co_await release(rest_moniker);rest_moniker=failed(hr)?0:full;}
                co_return std::pair{rest_moniker,me.parts.back()};
            };
            const auto bind_speed=[&](std::uint64_t pbc)->Task<std::uint32_t>{
                if(!pbc)co_return 1;const auto options=scratch(16);m.store(GuestAddress{options},32,16);
                if(failed(co_await com(pbc,7,{options})))co_return 1;co_return m.load(GuestAddress{options+12},32)?2U:1U; // BINDSPEED_INDEFINITE／MODERATE
            };
            const auto bind_mode=[&](std::uint64_t pbc)->Task<std::uint32_t>{
                if(!pbc)co_return 2;const auto options=scratch(16);m.store(GuestAddress{options},32,16);
                if(failed(co_await com(pbc,7,{options})))co_return 2;co_return static_cast<std::uint32_t>(m.load(GuestAddress{options+8},32)); // STGM_READWRITE
            };
            const auto container_of=[&](std::uint64_t pbc,std::uint64_t left)->Task<Pair>{
                const auto cell=out_cell();const auto hr=static_cast<std::uint32_t>(co_await com(left,8,{pbc,0,bytes(iid_item_container),cell}));
                if(failed(hr))co_return Pair{hr==e_nointerface?mk_e_intermediate:hr,0};co_return Pair{s_ok,m.load(GuestAddress{cell},bits)};
            };
            if(name=="GetClassID"){if(!call(1))co_return finish(e_pointer);m.write(GuestAddress{call(1)},my_clsid);co_return finish(s_ok);}
            if(name=="IsDirty")co_return finish(s_false);
            if(name=="IsSystemMoniker"){if(!call(1))co_return finish(e_pointer);m.store(GuestAddress{call(1)},32,kind);co_return finish(s_ok);}
            if(name=="Hash"){if(!call(1))co_return finish(e_pointer);std::uint32_t h=0;
                if(kind==mksys_composite)for(const auto part:me.parts)h^=co_await hash_of(part);else h=co_await hash_of(base);
                m.store(GuestAddress{call(1)},32,h);co_return finish(s_ok);}
            if(name=="IsEqual")co_return finish(call(1) && co_await equal(base,call(1))?s_ok:s_false);
            if(name=="Reduce"){
                // (pbc, dwReduceHowFar, ppmkToLeft, ppmkReduced)
                if(!call(4))co_return finish(e_invalidarg);m.store(GuestAddress{call(4)},bits,0);
                if(kind!=mksys_composite){(void)co_await addref(base);m.store(GuestAddress{call(4)},bits,base);co_return finish(mk_s_reduced_to_self);}
                std::vector<std::uint64_t> reduced;bool changed=false;
                for(const auto part:me.parts){const auto cell=out_cell();const auto hr=static_cast<std::uint32_t>(co_await com(part,10,{call(1),static_cast<std::uint32_t>(call(2)),0,cell}));
                    if(failed(hr)){(void)co_await release_all(reduced);co_return finish(hr);}const auto r=m.load(GuestAddress{cell},bits);
                    if(hr!=mk_s_reduced_to_self || r!=part)changed=true;if(r)reduced.push_back(r);}
                if(!changed){(void)co_await release_all(reduced);(void)co_await addref(base);m.store(GuestAddress{call(4)},bits,base);co_return finish(mk_s_reduced_to_self);}
                std::uint64_t result=0;for(const auto r:reduced){const auto [hr,next]=co_await generic(result,r);(void)co_await release(result);(void)co_await release(r);result=failed(hr)?0:next;}
                m.store(GuestAddress{call(4)},bits,result);co_return finish(s_ok);
            }
            if(name=="ComposeWith"){
                // (pmkRight, fOnlyIfNotGeneric, ppmkComposite)
                if(!call(3))co_return finish(e_invalidarg);m.store(GuestAddress{call(3)},bits,0);if(!call(1))co_return finish(e_invalidarg);
                const auto [hr,result]=co_await compose_nongeneric(base,call(1));
                if(!failed(hr)){m.store(GuestAddress{call(3)},bits,result);co_return finish(s_ok);}
                if(hr==mk_e_syntax)co_return finish(mk_e_syntax); // 兩個絕對路徑的 file moniker 不能組合
                if(static_cast<std::uint32_t>(call(2)))co_return finish(mk_e_needgeneric);
                const auto [ghr,composite]=co_await generic(base,call(1));if(!failed(ghr))m.store(GuestAddress{call(3)},bits,composite);co_return finish(ghr);
            }
            if(name=="Enum"){
                // (fForward, ppenumMoniker)
                if(!call(2))co_return finish(e_invalidarg);m.store(GuestAddress{call(2)},bits,0);
                if(kind==mksys_pointer)co_return finish(e_notimpl);if(kind!=mksys_composite)co_return finish(s_ok);
                ComMonikerEnum e;e.items=me.parts;if(!static_cast<std::uint32_t>(call(1)))std::reverse(e.items.begin(),e.items.end());for(const auto item:e.items)(void)co_await addref(item);
                const auto object=allocate_heap(m,unit);if(!object){(void)co_await release_all(e.items);co_return finish(e_outofmemory);}
                m.store(GuestAddress{object},bits,vtable(2));com_moniker_enums_.emplace(object,std::move(e));m.store(GuestAddress{call(2)},bits,object);co_return finish(s_ok);
            }
            if(name=="Inverse"){
                if(!call(1))co_return finish(e_invalidarg);m.store(GuestAddress{call(1)},bits,0);
                if(kind==mksys_anti)co_return finish(mk_e_noinverse);
                if(kind!=mksys_composite){ComMoniker anti;anti.kind=mksys_anti;m.store(GuestAddress{call(1)},bits,create(std::move(anti)));co_return finish(s_ok);}
                std::uint64_t result=0;
                for(auto it=me.parts.rbegin();it!=me.parts.rend();++it){
                    const auto cell=out_cell();const auto hr=static_cast<std::uint32_t>(co_await com(*it,17,{cell}));if(failed(hr)){(void)co_await release(result);co_return finish(hr);}
                    const auto inverse=m.load(GuestAddress{cell},bits);const auto [chr,next]=co_await generic(result,inverse);(void)co_await release(result);(void)co_await release(inverse);result=failed(chr)?0:next;
                }
                m.store(GuestAddress{call(1)},bits,result);co_return finish(s_ok);
            }
            if(name=="CommonPrefixWith" || name=="RelativePathTo"){
                // (pmkOther, ppmkResult)
                if(!call(2))co_return finish(e_invalidarg);m.store(GuestAddress{call(2)},bits,0);if(!call(1))co_return finish(e_invalidarg);
                const bool relative=name=="RelativePathTo";
                if(auto* other=ours(call(1));kind==mksys_file && other && other->kind==mksys_file){
                    const auto a=path_parts(me.name),b=path_parts(other->name);std::size_t n=0;while(n<a.size() && n<b.size() && same_part(a[n],b[n]))++n;
                    const auto reply=[&](std::uint64_t pointer,std::uint32_t hr)->std::optional<std::uint32_t>{m.store(GuestAddress{call(2)},bits,pointer);return finish(hr);};
                    if(relative){
                        if(!n){(void)co_await addref(call(1));co_return reply(call(1),mk_s_him);}
                        ComMoniker path;path.kind=mksys_file;for(auto i=n;i<a.size();++i)path.name+=u"..\\";path.name+=join_parts(b,n,b.size());co_return reply(create(std::move(path)),s_ok);
                    }
                    if(!n)co_return finish(mk_e_noprefix);
                    if(n==a.size() && n==b.size()){(void)co_await addref(base);co_return reply(base,mk_s_us);}
                    if(n==b.size()){(void)co_await addref(call(1));co_return reply(call(1),mk_s_him);}
                    if(n==a.size()){(void)co_await addref(base);co_return reply(base,mk_s_me);}
                    ComMoniker path;path.kind=mksys_file;path.name=join_parts(a,0,n);co_return reply(create(std::move(path)),s_ok);
                }
                if(relative && kind==mksys_item)co_return finish(mk_e_notbindable);
                if(relative && kind==mksys_pointer)co_return finish(e_notimpl);
                if(relative && kind==mksys_anti){(void)co_await addref(call(1));m.store(GuestAddress{call(2)},bits,call(1));co_return finish(mk_s_him);}
                if(!relative && kind==mksys_pointer){if(co_await equal(base,call(1))){(void)co_await addref(base);m.store(GuestAddress{call(2)},bits,base);co_return finish(mk_s_us);}co_return finish(mk_e_noprefix);}
                if(!relative && kind==mksys_class && co_await kind_of(call(1))==mksys_class && !co_await equal(base,call(1)))co_return finish(mk_e_noprefix);
                auto a=co_await parts_of(base);auto b=co_await parts_of(call(1));std::size_t n=0;
                while(n<a.size() && n<b.size() && co_await equal_simple(a[n],b[n]))++n;
                std::uint32_t hr=mk_e_noprefix;std::uint64_t result=0;
                if(!relative){
                    if(n==a.size() && n==b.size()){hr=mk_s_us;result=base;(void)co_await addref(result);}
                    else if(n && n==b.size()){hr=mk_s_him;result=call(1);(void)co_await addref(result);}
                    else if(n && n==a.size()){hr=mk_s_me;result=base;(void)co_await addref(result);}
                    else if(n){hr=s_ok;std::vector<std::uint64_t> prefix(a.begin(),a.begin()+static_cast<std::ptrdiff_t>(n));for(const auto p:prefix)(void)co_await addref(p);result=make_composite(std::move(prefix));}
                }else if(!n){hr=mk_s_him;result=call(1);(void)co_await addref(result);}
                else{
                    // Comp(Inv(myTail), otherTail)
                    std::vector<std::uint64_t> mine(a.begin()+static_cast<std::ptrdiff_t>(n),a.end()),theirs(b.begin()+static_cast<std::ptrdiff_t>(n),b.end());
                    std::uint64_t inverse=0;hr=s_ok;
                    for(auto it=mine.rbegin();it!=mine.rend();++it){const auto cell=out_cell();const auto ihr=static_cast<std::uint32_t>(co_await com(*it,17,{cell}));if(failed(ihr)){hr=ihr;break;}
                        const auto inv=m.load(GuestAddress{cell},bits);const auto [chr,next]=co_await generic(inverse,inv);(void)co_await release(inverse);(void)co_await release(inv);inverse=failed(chr)?0:next;}
                    if(!failed(hr)){result=inverse;inverse=0;for(const auto t:theirs){const auto [chr,next]=co_await generic(result,t);(void)co_await release(result);result=failed(chr)?0:next;}}
                    (void)co_await release(inverse);
                }
                (void)co_await release_all(a);(void)co_await release_all(b);
                m.store(GuestAddress{call(2)},bits,result);co_return finish(hr);
            }
            if(name=="GetDisplayName"){
                // (pbc, pmkToLeft, ppszDisplayName)
                if(!call(3))co_return finish(e_invalidarg);m.store(GuestAddress{call(3)},bits,0);
                std::u16string out;
                if(kind==mksys_composite){std::uint64_t left=call(2);if(left)(void)co_await addref(left);
                    for(const auto part:me.parts){const auto [hr,piece]=co_await display_of(part,call(1),left);if(failed(hr)){(void)co_await release(left);co_return finish(hr);}out+=piece;
                        const auto [chr,next]=co_await generic(left,part);(void)co_await release(left);left=failed(chr)?0:next;}
                    (void)co_await release(left);}
                else{const auto [hr,piece]=co_await display_of(base,call(1),call(2));if(failed(hr))co_return finish(hr);out=piece;}
                co_return finish(co_await alloc_text(out,call(3)));
            }
            if(name=="BindToObject" || name=="BindToStorage"){
                // (pbc, pmkToLeft, riid, ppv)
                const bool storage=name=="BindToStorage";const auto pbc=call(1),left=call(2),riid=call(3),output=call(4);
                if(!output)co_return finish(e_invalidarg);m.store(GuestAddress{output},bits,0);if(!riid)co_return finish(e_invalidarg);
                if(kind==mksys_anti)co_return finish(e_notimpl);
                if(kind==mksys_pointer){if(!me.pointer)co_return finish(e_unexpected);co_return finish(co_await com(me.pointer,0,{riid,output}));}
                if(kind==mksys_file){
                    const auto path=text(me.name);const auto mode=co_await bind_mode(pbc);Guid iid{};m.read(GuestAddress{riid},iid);
                    if(storage){
                        if(iid==iid_storage)co_return finish(co_await api("ole32.dll","StgOpenStorage",{path,0,mode,0,0,output}));
                        if(iid==iid_stream)co_return finish(co_await api("shlwapi.dll","SHCreateStreamOnFileW",{path,mode,output}));
                        co_return finish(mk_e_nostorage);
                    }
                    if(!left){const auto [hr,rot]=co_await rot_of(pbc);
                        if(!failed(hr)){const auto cell=out_cell();const auto found=co_await com(rot,6,{base,cell});(void)co_await com(rot,2,{});
                            if(found==s_ok){const auto object=m.load(GuestAddress{cell},bits);const auto result=co_await com(object,0,{riid,output});(void)co_await com(object,2,{});co_return finish(result);}}}
                    // 左側提供 IClassFactory 時以它建立，否則依 GetClassFile 判定類別；以 IPersistFile::Load 載入後回傳要求的介面。
                    const auto cell=out_cell();
                    if(left){
                        const auto factory_at=out_cell();const auto hr=static_cast<std::uint32_t>(co_await com(left,8,{pbc,0,bytes(iid_class_factory),factory_at}));
                        if(failed(hr))co_return finish(hr==e_nointerface?mk_e_intermediate:hr);const auto factory=m.load(GuestAddress{factory_at},bits);
                        const auto created=static_cast<std::uint32_t>(co_await com(factory,3,{0,bytes(iid_persist_file),cell}));(void)co_await com(factory,2,{});if(failed(created))co_return finish(created);
                    }else{
                        const auto clsid=scratch(16);if(const auto hr=static_cast<std::uint32_t>(co_await api("ole32.dll","GetClassFile",{path,clsid}));failed(hr))co_return finish(hr);
                        if(const auto hr=static_cast<std::uint32_t>(co_await api("ole32.dll","CoCreateInstance",{clsid,0,0x17,bytes(iid_persist_file),cell}));failed(hr))co_return finish(hr);
                    }
                    const auto file=m.load(GuestAddress{cell},bits);
                    const auto loaded=static_cast<std::uint32_t>(co_await com(file,5,{path,mode}));
                    const auto result=failed(loaded)?loaded:static_cast<std::uint32_t>(co_await com(file,0,{riid,output}));(void)co_await com(file,2,{});
                    if(!failed(result) && pbc)if(const auto object=m.load(GuestAddress{output},bits))(void)co_await com(pbc,3,{object}); // RegisterObjectBound
                    co_return finish(result);
                }
                if(kind==mksys_item){
                    if(!left)co_return finish(e_invalidarg);const auto [hr,container]=co_await container_of(pbc,left);if(failed(hr))co_return finish(hr);
                    const auto item=text(me.name);
                    const auto result=storage?co_await com(container,7,{item,pbc,riid,output}):co_await com(container,6,{item,co_await bind_speed(pbc),pbc,riid,output});
                    (void)co_await com(container,2,{});co_return finish(result);
                }
                if(kind==mksys_class){
                    if(storage)co_return finish(e_notimpl);
                    // BIND_OPTS2 的 dwClassContext（未指定時 CLSCTX_ALL）與 locale。
                    std::uint32_t context=0x17,locale=0;
                    if(pbc){const auto options=scratch(48);m.store(GuestAddress{options},32,x86_?32U:40U);if(!failed(co_await com(pbc,7,{options})) && m.load(GuestAddress{options},32)>=28){
                        if(const auto c=static_cast<std::uint32_t>(m.load(GuestAddress{options+20},32)))context=c;locale=static_cast<std::uint32_t>(m.load(GuestAddress{options+24},32));}}
                    const auto clsid=bytes(me.clsid);
                    if(!left)co_return finish(co_await api("ole32.dll","CoGetClassObject",{clsid,context,0,riid,output}));
                    const auto cell=out_cell();const auto hr=static_cast<std::uint32_t>(co_await com(left,8,{pbc,0,bytes(iid_class_activator),cell}));
                    if(failed(hr))co_return finish(hr==e_nointerface?mk_e_intermediate:hr);const auto activator=m.load(GuestAddress{cell},bits);
                    const auto result=co_await com(activator,3,{clsid,context,locale,riid,output});(void)co_await com(activator,2,{});co_return finish(result);
                }
                // composite：沒有左側時先查 ROT，再由最右元件以其餘部分為左側繫結。
                if(!left && !storage){const auto [hr,rot]=co_await rot_of(pbc);
                    if(!failed(hr)){const auto cell=out_cell();const auto found=co_await com(rot,6,{base,cell});(void)co_await com(rot,2,{});
                        if(found==s_ok){const auto object=m.load(GuestAddress{cell},bits);const auto result=co_await com(object,0,{riid,output});(void)co_await com(object,2,{});co_return finish(result);}}}
                const auto [rest,last]=co_await split(left);
                const auto result=co_await com(last,storage?9:8,{pbc,rest,riid,output});(void)co_await release(rest);co_return finish(result);
            }
            if(name=="IsRunning"){
                // (pbc, pmkToLeft, pmkNewlyRunning)
                const auto pbc=call(1),left=call(2),newly=call(3);
                if(kind==mksys_class)co_return finish(e_notimpl);
                if(kind==mksys_pointer)co_return finish(s_ok);
                if(kind==mksys_item && left){const auto [hr,container]=co_await container_of(pbc,left);if(failed(hr))co_return finish(hr);
                    const auto result=co_await com(container,8,{text(me.name)});(void)co_await com(container,2,{});co_return finish(result);}
                if(kind==mksys_composite && left){const auto [hr,full]=co_await generic(left,base);if(failed(hr))co_return finish(hr);
                    const auto result=co_await com(full,15,{pbc,0,newly});(void)co_await release(full);co_return finish(result);}
                if(newly && co_await equal(newly,base))co_return finish(s_ok);
                const auto [hr,rot]=co_await rot_of(pbc);if(failed(hr))co_return finish(hr);
                const auto running=co_await com(rot,5,{base});(void)co_await com(rot,2,{});
                if(running==s_ok || kind!=mksys_composite)co_return finish(running==s_ok?s_ok:s_false);
                const auto [rest,last]=co_await split(0);const auto result=co_await com(last,15,{pbc,rest,newly});(void)co_await release(rest);co_return finish(result);
            }
            if(name=="GetTimeOfLastChange"){
                // (pbc, pmkToLeft, pFileTime)
                const auto pbc=call(1),left=call(2),output=call(3);
                if(kind==mksys_class)co_return finish(mk_e_unavailable);
                if(kind==mksys_anti || kind==mksys_pointer)co_return finish(e_notimpl);
                if(!output)co_return finish(e_invalidarg);
                if(kind==mksys_file){
                    // ROT 有登錄時用其變更時間，否則為檔案最後寫入時間（找不到檔案 MK_E_NOOBJECT）。
                    const auto [hr,rot]=co_await rot_of(pbc);std::uint64_t found=1;if(!failed(hr)){found=co_await com(rot,8,{base,output});(void)co_await com(rot,2,{});}
                    if(found==s_ok)co_return finish(s_ok);
                    const auto data=scratch(40);if(!static_cast<std::uint32_t>(co_await api("kernel32.dll","GetFileAttributesExW",{text(me.name),0,data})))co_return finish(mk_e_noobject);
                    m.store(GuestAddress{output},64,m.load(GuestAddress{data+20},64));co_return finish(s_ok);
                }
                if(kind==mksys_item && !left)co_return finish(mk_e_notbindable);
                std::uint64_t full=base;if(left){const auto [hr,composite]=co_await generic(left,base);if(failed(hr))co_return finish(hr);full=composite;}else (void)co_await addref(full);
                const auto [hr,rot]=co_await rot_of(pbc);std::uint64_t found=1;
                if(!failed(hr)){found=co_await com(rot,8,{full,output});(void)co_await com(rot,2,{});}
                (void)co_await release(full);if(found==s_ok)co_return finish(s_ok);
                if(kind==mksys_item)co_return finish(co_await com(left,16,{pbc,0,output}));
                const auto [rest,last]=co_await split(left);const auto result=co_await com(last,16,{pbc,rest,output});(void)co_await release(rest);co_return finish(result);
            }
            if(name=="ParseDisplayName"){
                // (pbc, pmkToLeft, pszDisplayName, pchEaten, ppmkOut)
                const auto pbc=call(1),left=call(2),display=call(3),eaten=call(4),output=call(5);
                if(!output || !eaten)co_return finish(e_invalidarg);m.store(GuestAddress{output},bits,0);m.store(GuestAddress{eaten},32,0);
                if(kind==mksys_anti)co_return finish(e_notimpl);
                std::uint64_t parser=0;const auto cell=out_cell();
                if(kind==mksys_composite){const auto [rest,last]=co_await split(left);const auto result=co_await com(last,21,{pbc,rest,display,eaten,output});(void)co_await release(rest);co_return finish(result);}
                if(kind==mksys_item){if(!left)co_return finish(mk_e_syntax);const auto [hr,container]=co_await container_of(pbc,left);if(failed(hr))co_return finish(hr);
                    const auto got=static_cast<std::uint32_t>(co_await com(container,6,{text(me.name),co_await bind_speed(pbc),pbc,bytes(iid_parse_display_name),cell}));(void)co_await com(container,2,{});
                    if(failed(got))co_return finish(got);}
                else if(kind==mksys_file){if(left)co_return finish(mk_e_syntax);
                    // 先取類別的 IParseDisplayName（GetClassFile＋CoGetClassObject），再嘗試繫結到物件本身。
                    const auto clsid=scratch(16);std::uint32_t got=static_cast<std::uint32_t>(co_await api("ole32.dll","GetClassFile",{text(me.name),clsid}));
                    if(!failed(got))got=static_cast<std::uint32_t>(co_await api("ole32.dll","CoGetClassObject",{clsid,0x17,0,bytes(iid_parse_display_name),cell}));
                    if(failed(got))got=static_cast<std::uint32_t>(co_await com(base,8,{pbc,0,bytes(iid_parse_display_name),cell}));
                    if(failed(got))co_return finish(got);}
                else if(kind==mksys_class){if(left)co_return finish(mk_e_syntax);
                    const auto got=static_cast<std::uint32_t>(co_await api("ole32.dll","CoGetClassObject",{bytes(me.clsid),0x17,0,bytes(iid_parse_display_name),cell}));if(failed(got))co_return finish(got);}
                else{if(!me.pointer)co_return finish(e_unexpected);const auto got=static_cast<std::uint32_t>(co_await com(me.pointer,0,{bytes(iid_parse_display_name),cell}));if(failed(got))co_return finish(got);}
                parser=m.load(GuestAddress{cell},bits);
                const auto result=co_await com(parser,3,{pbc,display,eaten,output});(void)co_await com(parser,2,{});co_return finish(result);
            }
            if(name=="Save" || name=="GetSizeMax" || name=="Load"){
                if(kind==mksys_pointer)co_return finish(e_notimpl);
                const auto stream=call(1);if(!stream)co_return finish(e_pointer);
                if(name=="Load"){
                    const auto read=[&](std::size_t size)->Task<std::pair<bool,std::vector<std::uint8_t>>>{
                        std::vector<std::uint8_t> out(size);if(!size)co_return std::pair{true,std::move(out)};const auto buffer=scratch(size),got=scratch(4);
                        const auto hr=co_await com(stream,3,{buffer,size,got});if(failed(hr) || m.load(GuestAddress{got},32)!=size)co_return std::pair{false,std::move(out)};
                        m.read(GuestAddress{buffer},std::span<std::uint8_t>(out));co_return std::pair{true,std::move(out)};};
                    const auto dword=[&]()->Task<std::optional<std::uint32_t>>{auto [ok,data]=co_await read(4);if(!ok)co_return std::nullopt;co_return std::uint32_t{data[0]}|(std::uint32_t{data[1]}<<8)|(std::uint32_t{data[2]}<<16)|(std::uint32_t{data[3]}<<24);};
                    const auto item_string=[&]()->Task<std::optional<std::u16string>>{
                        const auto length=co_await dword();if(!length || *length>65536)co_return std::nullopt;auto [ok,data]=co_await read(*length);if(!ok)co_return std::nullopt;
                        std::size_t end=0;while(end<data.size() && data[end])++end;std::u16string out;
                        if(end+1<data.size()){for(std::size_t i=end+1;i+1<data.size();i+=2)out.push_back(static_cast<char16_t>(data[i]|(data[i+1]<<8)));}
                        else for(std::size_t i=0;i<end;++i)out.push_back(data[i]);
                        co_return out;};
                    ComMoniker loaded=me;loaded.parts.clear();
                    if(kind==mksys_file){
                        // [MS-OSHARED] FileMoniker：cAnti、ansiLength＋ansiPath、endServer、versionNumber、reserved1（16）、reserved2（4）、cbUnicodePathSize 與選用 Unicode。
                        auto [ok,head]=co_await read(2);const auto length=ok?co_await dword():std::nullopt;if(!ok || !length || *length>32767)co_return finish(0x80030008U);
                        const auto anti=static_cast<std::uint32_t>(head[0]|(head[1]<<8));auto [path_ok,ansi]=co_await read(*length);auto [tail_ok,tail]=co_await read(24);
                        const auto unicode_size=path_ok && tail_ok?co_await dword():std::nullopt;if(!unicode_size)co_return finish(0x80030008U);
                        std::u16string path;for(const auto c:ansi){if(!c)break;path.push_back(c);}
                        if(*unicode_size){const auto bytes_count=co_await dword();auto [key_ok,key]=co_await read(2);if(!bytes_count || !key_ok || *bytes_count>65534)co_return finish(0x80030008U);
                            auto [unicode_ok,unicode]=co_await read(*bytes_count);if(!unicode_ok)co_return finish(0x80030008U);path.clear();
                            for(std::size_t i=0;i+1<unicode.size();i+=2)path.push_back(static_cast<char16_t>(unicode[i]|(unicode[i+1]<<8)));}
                        std::u16string prefix;for(std::uint32_t i=0;i<anti;++i)prefix+=u"..\\";if(!path.starts_with(prefix))path=prefix+path;loaded.name=path;
                    }
                    else if(kind==mksys_item){const auto delimiter=co_await item_string();if(!delimiter)co_return finish(0x80030008U);const auto item=co_await item_string();if(!item)co_return finish(0x80030008U);loaded.delimiter=*delimiter;loaded.name=*item;}
                    else if(kind==mksys_anti){const auto count=co_await dword();if(!count || !*count)co_return finish(0x80030008U);loaded.count=*count;}
                    else if(kind==mksys_class){auto [ok,data]=co_await read(16);const auto extra=ok?co_await dword():std::nullopt;if(!ok || !extra || *extra>65536)co_return finish(0x80030008U);
                        std::copy(data.begin(),data.end(),loaded.clsid.begin());if(*extra)(void)co_await read(*extra);}
                    else{const auto count=co_await dword();if(!count || *count>4096)co_return finish(0x80030008U);
                        for(std::uint32_t i=0;i<*count;++i){const auto cell=out_cell();const auto hr=static_cast<std::uint32_t>(co_await api("ole32.dll","OleLoadFromStream",{stream,bytes(iid_moniker),cell}));
                            if(failed(hr)){(void)co_await release_all(loaded.parts);co_return finish(hr);}loaded.parts.push_back(m.load(GuestAddress{cell},bits));}}
                    auto& target=com_monikers_.at(base);const auto old=std::move(target.parts);const auto references=target.references;target=std::move(loaded);target.references=references;
                    (void)co_await release_all(old);co_return finish(s_ok);
                }
                std::vector<std::uint8_t> data;
                if(kind==mksys_file){
                    std::uint16_t anti=0;std::u16string_view rest=me.name;while(rest.starts_with(u"..\\")){++anti;rest.remove_prefix(3);}
                    const bool ascii=std::all_of(me.name.begin(),me.name.end(),[](char16_t c){return c<0x80;});
                    append16(data,anti);append32(data,static_cast<std::uint32_t>(me.name.size()+1));for(const auto c:me.name)data.push_back(c<0x80?static_cast<std::uint8_t>(c):'?');data.push_back(0);
                    std::uint16_t server=0xffff;if(me.name.starts_with(u"\\\\")){const auto end=me.name.find(u'\\',2);server=static_cast<std::uint16_t>(end==me.name.npos?me.name.size():end);}
                    append16(data,server);append16(data,0xdead);data.insert(data.end(),20,0);
                    if(ascii)append32(data,0);
                    else{append32(data,static_cast<std::uint32_t>(me.name.size()*2+6));append32(data,static_cast<std::uint32_t>(me.name.size()*2));append16(data,3);
                        for(const auto c:me.name){data.push_back(static_cast<std::uint8_t>(c));data.push_back(static_cast<std::uint8_t>(c>>8));}}
                }
                else if(kind==mksys_item){append_item_string(data,me.delimiter);append_item_string(data,me.name);}
                else if(kind==mksys_anti)append32(data,me.count);
                else if(kind==mksys_class){data.insert(data.end(),me.clsid.begin(),me.clsid.end());append32(data,0);}
                if(name=="GetSizeMax"){
                    std::uint64_t size=data.size();
                    if(kind==mksys_composite){size=4;for(const auto part:me.parts){const auto cell=scratch(8);if(failed(co_await com(part,7,{cell})))co_return finish(0x80004005U);size+=16+m.load(GuestAddress{cell},64);}}
                    m.store(GuestAddress{stream},64,size);co_return finish(s_ok);
                }
                if(kind==mksys_composite){append32(data,static_cast<std::uint32_t>(me.parts.size()));}
                if(!data.empty()){const auto buffer=bytes(data),written=scratch(4);const auto hr=co_await com(stream,4,{buffer,data.size(),written});if(failed(hr))co_return finish(hr);}
                if(kind==mksys_composite)for(const auto part:me.parts){const auto hr=static_cast<std::uint32_t>(co_await api("ole32.dll","OleSaveToStream",{part,stream}));if(failed(hr))co_return finish(hr);}
                co_return finish(s_ok);
            }
            throw HostCallFault("未知的 IMoniker 方法");
        }catch(const MemoryFault&){co_return finish(e_invalidarg);}catch(const std::bad_alloc&){co_return finish(e_outofmemory);}
    });
}
}
