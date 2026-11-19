// shell32 的 ITEMIDLIST 家族：IL* 分段操作（大小、下一段、最後一段、移除、複製、合併、比較、父子判定）、SHGetRealIDL、
// SHSimpleIDListFromPath、SHGetPathFromIDListEx，以及 DoEnvironmentSubst 與 Win32DeleteFile。
// 本 Shell 的 PIDL 由磁碟根項目與各路徑段組成（見 shell_make_pidl）；分段操作依 cb 走訪，對任何合法 PIDL 都成立，
// 產生的絕對 PIDL 同時登錄完整路徑，讓既有的 Shell 消費端（SHGetPathFromIDList、SHFileOperation 等）沿用。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include "service_callback_task.hpp"

namespace x64arm {
namespace {
constexpr std::uint32_t invalid_argument=0x80070057U,pointer_error=0x80004003U,out_of_memory=0x8007000eU;
constexpr std::size_t idlist_limit=65536;
}
std::span<const WindowsServiceDescriptor> shell_idlist_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shlwapi.dll","IStream_ReadPidl",2},{"shlwapi.dll","IStream_WritePidl",2},
        {"shell32.dll","ILLoadFromStream",2},{"shell32.dll","ILSaveToStream",2},
        {"windows.storage.dll","ILLoadFromStreamEx",2},
        {"shell32.dll","ILGetSize",1},{"shell32.dll","ILGetNext",1},{"shell32.dll","ILFindLastID",1},{"shell32.dll","ILRemoveLastID",1},
        {"shell32.dll","ILClone",1},{"shell32.dll","ILCloneFirst",1},{"shell32.dll","ILCombine",2},{"shell32.dll","ILAppendID",3,0,0,0x4},{"shell32.dll","ILIsEqual",2},
        {"shell32.dll","ILIsParent",3},{"shell32.dll","ILFindChild",2},{"shell32.dll","SHGetRealIDL",3},{"shell32.dll","SHSimpleIDListFromPath",1},
        {"shell32.dll","SHGetPathFromIDListA",2},{"shell32.dll","SHGetPathFromIDListW",2},{"shell32.dll","SHGetPathFromIDListEx",4,0,0,0xc},{"shell32.dll","DoEnvironmentSubstA",2},{"shell32.dll","DoEnvironmentSubstW",2},{"shell32.dll","Win32DeleteFile",1},
    };return table;
}
// 各項目的（位移, cb）；超過上限或 cb 小於 2 視為格式錯誤。
static std::vector<std::pair<std::uint64_t,std::uint16_t>> idlist_items(GuestMemory& memory,std::uint64_t pidl){
    std::vector<std::pair<std::uint64_t,std::uint16_t>> items;if(!pidl)return items;std::uint64_t offset=0;
    for(;;){const auto cb=static_cast<std::uint16_t>(memory.load(GuestAddress{pidl+offset},16));if(!cb)break;
        if(cb<2 || offset+cb+2>idlist_limit)throw std::invalid_argument("ITEMIDLIST 格式錯誤");items.push_back({offset,cb});offset+=cb;}
    return items;
}
static std::vector<std::uint8_t> idlist_bytes(GuestMemory& memory,std::uint64_t pidl,std::size_t count){
    const auto items=idlist_items(memory,pidl);std::size_t size=0;for(std::size_t i=0;i<std::min(count,items.size());++i)size+=items[i].second;
    std::vector<std::uint8_t> bytes(size);if(size)memory.read(GuestAddress{pidl},bytes);return bytes;
}
std::optional<std::u16string> WindowsServices::shell_pidl_path(GuestMemory& memory,std::uint64_t pidl,std::u16string base) const{
    if(!pidl)return std::nullopt;const bool relative=!base.empty();
    if(!relative)if(const auto found=gui_.shell_pidls.find(pidl);found!=gui_.shell_pidls.end() && allocations_.contains(pidl))return found->second;
    // 已釋放的小配置仍可能與存活物件共用映射頁。
    const auto next_free=free_heap_blocks_.upper_bound(pidl);
    if(next_free!=free_heap_blocks_.begin()){
        const auto& [start,length]=*std::prev(next_free);
        if(pidl-start<length)return std::nullopt;
    }
    try{
        std::u16string path=std::move(base);
        for(const auto& [offset,cb]:idlist_items(memory,pidl)){
            if(cb<6 || (cb-4)%2 || memory.load(GuestAddress{pidl+offset+cb-2},16)!=0)return std::nullopt;const auto type=memory.load(GuestAddress{pidl+offset+2},8);
            std::u16string name;for(std::uint64_t at=4;at+2<cb;at+=2)name.push_back(static_cast<char16_t>(memory.load(GuestAddress{pidl+offset+at},16)));
            if(path.empty()){if(type!=0x2f || name.size()!=3 || name[1]!=u':')return std::nullopt;path=name;}
            else {if(type!=0x31 || name.empty() || name.find(u'\\')!=name.npos)return std::nullopt;if(path.back()!=u'\\')path+=u'\\';path+=name;}
        }
        if(path.empty())return std::nullopt;return path;
    }catch(const std::invalid_argument&){return std::nullopt;}catch(const MemoryFault&){return std::nullopt;}
}
std::optional<std::uint32_t> WindowsServices::invoke_shell_idlist(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const bool shlwapi=service.family=="shlwapi.dll";
    if(shlwapi || name=="ILSaveToStream" || name=="ILLoadFromStream" || name=="ILLoadFromStreamEx"){
        const bool read=name=="IStream_ReadPidl" || name=="ILLoadFromStream" || name=="ILLoadFromStreamEx";
        return nested_continuation(state,memory,"PIDL 串流",[this,service,shlwapi,read](GuestState& s,GuestMemory& m)
            ->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);
            const auto stream=call(0),pidl=call(1);
            const auto finish=[&](unsigned hr){call.return_to(s,hr);return std::optional<std::uint32_t>{};};
            const unsigned width=x86_?4U:8U;
            const auto bits=static_cast<std::uint8_t>(width*8);
            if(read){
                if(!pidl)co_return finish(pointer_error);
                m.store(GuestAddress{pidl},bits,0);
            }
            if(!stream)co_return finish(invalid_argument);
            const auto table=m.load(GuestAddress{stream},bits);
            const auto method=m.load(GuestAddress{table+width*(read?3U:4U)},bits);
            GuestScratch scratch(*this,m,s);
            const auto header=scratch.allocate(8);
            if(!header)co_return finish(out_of_memory);
            const auto transfer=[&](std::uint64_t buffer,unsigned count)->service_callback::Task<unsigned>{
                m.store(GuestAddress{header+4},32,0);
                const auto hr=co_await service_callback::call(method,{stream,buffer,count,header+4});
                if(hr&0x80000000U)co_return hr;
                co_return m.load(GuestAddress{header+4},32)==count?0U:0x80004005U;
            };
            // shell32 使用 WORD 長度，shlwapi 使用 DWORD；兩者都包含最後的零長度項目。
            const unsigned prefix=shlwapi?4U:2U;
            const auto length_bits=static_cast<std::uint8_t>(8*prefix);
            const unsigned limit=shlwapi?UINT32_MAX:UINT16_MAX;
            auto scan=[&](std::uint64_t address,unsigned bound)->unsigned{
                unsigned pos=0;
                while(bound-pos>=2){
                    const auto cb=static_cast<unsigned>(m.load(GuestAddress{address+pos},16));
                    if(!cb)return pos+2;
                    if(cb<2 || cb>bound-pos-2)break;
                    pos+=cb;
                }
                return 0;
            };
            if(!read){
                const unsigned length=pidl?scan(pidl,limit):0;
                if(pidl && !length)co_return finish(invalid_argument);
                m.store(GuestAddress{header},length_bits,length);
                const auto hr=co_await transfer(header,prefix);
                if(hr)co_return finish(hr);
                co_return finish(co_await transfer(pidl,length));
            }
            const auto hr=co_await transfer(header,prefix);
            if(hr)co_return finish(hr);
            const auto length=static_cast<unsigned>(m.load(GuestAddress{header},length_bits));
            if(!length && !shlwapi)co_return finish(0);
            if(length<2)co_return finish(invalid_argument);
            const auto result=nested_service(s,m,"ole32.dll","CoTaskMemAlloc",{length});
            if(!result)co_return finish(out_of_memory);
            auto status=co_await transfer(result,length);
            if(!status && scan(result,length)!=length)status=invalid_argument;
            if(status)(void)nested_service(s,m,"ole32.dll","CoTaskMemFree",{result});
            else{
                if(const auto path=shell_pidl_path(m,result);path && gui_.shell_pidls.size()<8192)
                    gui_.shell_pidls[result]=*path;
                m.store(GuestAddress{pidl},bits,result);
            }
            co_return finish(status);
        });
    }
    // 新的 PIDL：配置後若可解成絕對路徑就登錄，與 shell_make_pidl 建立的 PIDL 相同地被其他 Shell API 接受。
    const auto make=[&](const std::vector<std::uint8_t>& items)->std::uint64_t{
        const auto address=allocate_heap(memory,items.size()+2);if(!address)return 0;
        memory.write(GuestAddress{address},items);memory.store(GuestAddress{address+items.size()},16,0);
        if(const auto path=shell_pidl_path(memory,address);path && gui_.shell_pidls.size()<8192)gui_.shell_pidls[address]=*path;return address;};
    // 項目內容比較：本格式的名稱不分大小寫，其他項目逐位元組比較。
    const auto same_item=[&](std::uint64_t a,std::uint64_t b,std::uint16_t cb_a,std::uint16_t cb_b){
        std::vector<std::uint8_t> x(cb_a),y(cb_b);memory.read(GuestAddress{a},x);memory.read(GuestAddress{b},y);
        if(cb_a==cb_b && cb_a>=6 && (x[2]==0x2f || x[2]==0x31) && x[2]==y[2]){
            for(std::size_t i=4;i+1<cb_a;i+=2){const auto c=static_cast<char16_t>(x[i]|(x[i+1]<<8)),d=static_cast<char16_t>(y[i]|(y[i+1]<<8));if(unicode_case_unit(c,false)!=unicode_case_unit(d,false))return false;}return true;}
        return x==y;};
    try{
        if(name=="ILGetSize"){if(!arg(0))return done(0);std::uint64_t size=2;for(const auto& item:idlist_items(memory,arg(0)))size+=item.second;return done(size);}
        if(name=="ILGetNext"){if(!arg(0))return done(0);const auto cb=memory.load(GuestAddress{arg(0)},16);if(!cb)return done(0);const auto next=arg(0)+cb;return done(memory.load(GuestAddress{next},16)?next:0);}
        if(name=="ILFindLastID"){if(!arg(0))return done(0);const auto items=idlist_items(memory,arg(0));return done(items.empty()?arg(0):arg(0)+items.back().first);}
        if(name=="ILRemoveLastID"){
            // 原地把最後一段改成結尾；已登錄的路徑同步改成上一層。
            if(!arg(0))return done(0);const auto items=idlist_items(memory,arg(0));if(items.empty())return done(0);
            memory.store(GuestAddress{arg(0)+items.back().first},16,0);
            if(gui_.shell_pidls.contains(arg(0))){gui_.shell_pidls.erase(arg(0));if(const auto path=shell_pidl_path(memory,arg(0)))gui_.shell_pidls[arg(0)]=*path;}
            return done(1);
        }
        if(name=="ILClone" || name=="ILCloneFirst"){if(!arg(0))return done(0);return done(make(idlist_bytes(memory,arg(0),name=="ILClone"?SIZE_MAX:1)));}
        if(name=="ILAppendID"){
            // (pidl, SHITEMID*, bAddToEnd)：在尾端或前端加上一個項目，回傳新的 IDList 並釋放原本的 pidl；失敗時保留原 pidl 並回 NULL。
            if(!arg(1))return done(0);const auto cb=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(1)},16));if(cb<2)return done(0);
            std::vector<std::uint8_t> item(cb);memory.read(GuestAddress{arg(1)},item);auto bytes=idlist_bytes(memory,arg(0),SIZE_MAX);
            if(static_cast<std::uint32_t>(arg(2)))bytes.insert(bytes.end(),item.begin(),item.end());else bytes.insert(bytes.begin(),item.begin(),item.end());
            if(bytes.size()+2>idlist_limit)return done(0);const auto result=make(bytes);if(!result)return done(0);
            if(arg(0)){gui_.shell_pidls.erase(arg(0));gui_.task_allocations.erase(arg(0));(void)free_heap(memory,arg(0),state);}return done(result);
        }
        if(name=="ILCombine"){
            if(!arg(0) && !arg(1))return done(0);auto bytes=idlist_bytes(memory,arg(0),SIZE_MAX);const auto tail=idlist_bytes(memory,arg(1),SIZE_MAX);
            bytes.insert(bytes.end(),tail.begin(),tail.end());if(bytes.size()+2>idlist_limit)return done(0);return done(make(bytes));
        }
        if(name=="ILIsEqual" || name=="ILIsParent" || name=="ILFindChild"){
            // IsParent(parent, child, immediate)：parent 為 child 的前段；immediate 時只多一段。FindChild 回傳 child 中 parent 之後的部分。
            if(!arg(0) || !arg(1))return done(0);const auto a=idlist_items(memory,arg(0)),b=idlist_items(memory,arg(1));
            const auto prefix=[&]{if(a.size()>b.size())return false;for(std::size_t i=0;i<a.size();++i)if(!same_item(arg(0)+a[i].first,arg(1)+b[i].first,a[i].second,b[i].second))return false;return true;}();
            if(name=="ILIsEqual")return done(prefix && a.size()==b.size()?1U:0U);
            if(name=="ILIsParent"){if(!prefix || a.size()==b.size())return done(0);return done(!arg(2) || b.size()==a.size()+1?1U:0U);}
            if(!prefix)return done(0);return done(a.size()==b.size()?arg(1)+(b.empty()?0:b.back().first+b.back().second):arg(1)+b[a.size()].first);
        }
        if(name=="SHGetRealIDL"){
            // (folder, simple, real*)：本 Shell 的 PIDL 已含完整資訊，回傳複本。
            if(!arg(2))return done(pointer_error);memory.store(GuestAddress{arg(2)},x86_?32:64,0);if(!arg(1))return done(invalid_argument);
            const auto copy=make(idlist_bytes(memory,arg(1),SIZE_MAX));if(!copy)return done(out_of_memory);memory.store(GuestAddress{arg(2)},x86_?32:64,copy);return done(0);
        }
        if(name=="SHSimpleIDListFromPath"){
            // 與 ILCreateFromPath 不同，不要求路徑存在。
            if(!arg(0))return done(0);auto path=read_utf16(memory,GuestAddress{arg(0)},260);std::replace(path.begin(),path.end(),u'/',u'\\');
            if(path.size()<3 || path[1]!=u':' || path[2]!=u'\\')return done(0);if(path[0]>=u'a' && path[0]<=u'z')path[0]=static_cast<char16_t>(path[0]-32);
            while(path.size()>3 && path.back()==u'\\')path.pop_back();return done(shell_make_pidl(memory,path));
        }
        if(name.starts_with("SHGetPathFromIDList")){
            const bool extended=name=="SHGetPathFromIDListEx",ansi=name.ends_with('A');
            const auto capacity=extended?static_cast<std::uint32_t>(arg(2)):260U;
            const auto flags=extended?static_cast<std::uint32_t>(arg(3)):0U;
            const auto output=GuestAddress{arg(1)};
            if(!output.value || !capacity || (flags&~3U)){last_error_=87;return done(0);}
            memory.store(output,ansi?8:16,0);
            auto path=shell_pidl_path(memory,arg(0));
            if(!path){last_error_=87;return done(0);}
            if(flags&1U){
                GuestScratch scratch(*this,memory,state);
                const auto source=scratch.text(*path),target=scratch.allocate(1040);
                if(!source || !target)return done(0);
                const auto length=nested_service(state,memory,"kernel32.dll","GetShortPathNameW",{source,target,520});
                if(length && length<520)*path=read_utf16(memory,GuestAddress{target},520);
            }
            if(ansi){
                const auto text=utf16_to_utf8(*path);
                if(text.size()>=capacity){last_error_=206;return done(0);}
                memory.write(output,{reinterpret_cast<const std::uint8_t*>(text.c_str()),text.size()+1});
            }else{
                if(path->size()>=capacity){last_error_=206;return done(0);}
                memory.write(output,utf16_bytes(*path));
            }
            return done(1);
        }
        if(name=="DoEnvironmentSubstA" || name=="DoEnvironmentSubstW"){
            // 原地展開環境變數：成功時高位字為 TRUE、低位字為新長度；緩衝不足時不修改並回 FALSE 與原容量。
            const bool wide=name.ends_with('W');const auto capacity=static_cast<std::uint32_t>(arg(1));if(!arg(0) || !capacity)return done(capacity&0xffffU);
            GuestScratch scratch(*this,memory,state);const auto unit=wide?2U:1U;const auto source=scratch.allocate(static_cast<std::size_t>(capacity)*unit+2),target=scratch.allocate(static_cast<std::size_t>(capacity)*unit+2);
            std::vector<std::uint8_t> original(static_cast<std::size_t>(capacity)*unit);memory.read(GuestAddress{arg(0)},original);memory.write(GuestAddress{source},original);memory.store(GuestAddress{source+original.size()},16,0);
            const auto needed=static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll",wide?"ExpandEnvironmentStringsW":"ExpandEnvironmentStringsA",{source,target,capacity}));
            if(!needed || needed>capacity)return done(capacity&0xffffU);
            std::vector<std::uint8_t> expanded(static_cast<std::size_t>(needed)*unit);memory.read(GuestAddress{target},expanded);memory.write(GuestAddress{arg(0)},expanded);
            return done((1U<<16)|((needed-1)&0xffffU));
        }
        if(name=="Win32DeleteFile"){
            // 刪除檔案並送出 SHCNE_DELETE（SHCNF_PATHW）通知。
            if(!arg(0))return done(0);if(!nested_service(state,memory,"kernel32.dll","DeleteFileW",{arg(0)}))return done(0);
            (void)nested_service(state,memory,"shell32.dll","SHChangeNotify",{4,5,arg(0),0});return done(1);
        }
    }catch(const std::invalid_argument&){last_error_=87;return done(0);}catch(const MemoryFault&){last_error_=998;return done(0);}
    throw HostCallFault("未知的 ITEMIDLIST 服務");
}
}
