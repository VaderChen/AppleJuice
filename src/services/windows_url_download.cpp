// urlmon 下載與繫結資訊（以既有 WinINet、HGLOBAL 串流、URL 快取與 COM 記憶體服務轉接）：
// - URLDownloadToFile／URLDownloadToCacheFile／URLOpenBlockingStream／URLOpenStream／URLOpenPullStream（A／W）：
//   http／https／ftp 以 InternetOpenUrlW 讀取，file: URL 以 PathCreateFromUrlW 轉成路徑後讀檔；整份資料讀完才寫入目的地（中途失敗不留下半份檔案）。
//   有 IBindStatusCallback 時依序呼叫 GetBindInfo、OnStartBinding（傳入內部 IBinding）、OnProgress（FINDINGRESOURCE、CONNECTING、SENDINGREQUEST、
//   MIMETYPEAVAILABLE、BEGINDOWNLOADDATA、每段 DOWNLOADINGDATA、ENDDOWNLOADDATA，快取版另有 CACHEFILENAMEAVAILABLE）、串流版的 OnDataAvailable（一次交付整份資料）
//   與 OnStopBinding；OnProgress 回 E_ABORT 或呼叫 IBinding::Abort 時中止並回 E_ABORT。HTTP 狀態碼 400 以上為 INET_E_DOWNLOAD_FAILURE，
//   WinINet 的名稱解析、連線、逾時與通訊協定錯誤轉成對應的 INET_E_*。
// - IBinding：Abort、SetPriority／GetPriority、GetBindResult；Suspend／Resume 與 Windows 相同回 E_NOTIMPL。
// - CopyStgMedium：依 tymed 複製 HGLOBAL、檔名、IStream／IStorage（加參考）、HBITMAP、METAFILEPICT 與 HENHMETAFILE，pUnkForRelease 加參考。
// - CopyBindInfo／ReleaseBindInfo：BINDINFO（x86 84、x64 128 位元組）的深層複製與釋放（字串為 CoTaskMem、STGMEDIUM 走 CopyStgMedium／ReleaseStgMedium、pUnk 參考計數）。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>

namespace x64arm {
namespace {
constexpr std::uint32_t e_invalidarg=0x80070057U,e_outofmemory=0x8007000eU,e_abort=0x80004004U,e_pointer=0x80004003U,e_notimpl=0x80004001U,e_nointerface=0x80004002U;
constexpr std::uint32_t inet_invalid_url=0x800C0002U,inet_cannot_connect=0x800C0004U,inet_resource_not_found=0x800C0005U,inet_download_failure=0x800C0008U,
    inet_connection_timeout=0x800C000BU,inet_unknown_protocol=0x800C000DU;
constexpr std::array<std::uint8_t,16> iid_unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr std::array<std::uint8_t,16> iid_binding{0xc0,0xc9,0xea,0x79,0xf9,0xba,0xce,0x11,0x8c,0x82,0x00,0xaa,0x00,0x4b,0xa9,0x0b};
std::uint32_t inet_status(std::uint32_t error){
    switch(error){
    case 12005:return inet_invalid_url;case 12006:return inet_unknown_protocol;case 12007:return inet_resource_not_found;
    case 12002:return inet_connection_timeout;case 12029:case 12030:return inet_cannot_connect;default:return inet_download_failure;}
}
}
std::span<const WindowsServiceDescriptor> url_download_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"urlmon.dll","URLDownloadToFileA",5,0,0,0x8},{"urlmon.dll","URLDownloadToFileW",5,0,0,0x8},
        {"urlmon.dll","URLDownloadToCacheFileA",6,0,0,0x18},{"urlmon.dll","URLDownloadToCacheFileW",6,0,0,0x18},
        {"urlmon.dll","URLOpenBlockingStreamA",5,0,0,0x8},{"urlmon.dll","URLOpenBlockingStreamW",5,0,0,0x8},
        {"urlmon.dll","URLOpenStreamA",4,0,0,0x4},{"urlmon.dll","URLOpenStreamW",4,0,0,0x4},
        {"urlmon.dll","URLOpenPullStreamA",4,0,0,0x4},{"urlmon.dll","URLOpenPullStreamW",4,0,0,0x4},
        {"urlmon.dll","CopyStgMedium",2},{"urlmon.dll","CopyBindInfo",2},{"urlmon.dll","ReleaseBindInfo",1},
        {"internal-urlmon-binding","QueryInterface",3},{"internal-urlmon-binding","AddRef",1},{"internal-urlmon-binding","Release",1},
        {"internal-urlmon-binding","Abort",1},{"internal-urlmon-binding","Suspend",1},{"internal-urlmon-binding","Resume",1},
        {"internal-urlmon-binding","SetPriority",2,0,0,0x2},{"internal-urlmon-binding","GetPriority",2},{"internal-urlmon-binding","GetBindResult",5}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_url_download(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(service.family=="internal-urlmon-binding"){
        const WindowsCall arg(state,memory,service);const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto found=url_bindings_.find(arg(0));if(found==url_bindings_.end())return finish(e_pointer);
        auto& binding=found->second;const auto name=service.symbol;
        if(name=="QueryInterface"){
            if(!arg(2))return finish(e_pointer);std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{arg(1)},iid);
            if(iid!=iid_unknown && iid!=iid_binding){memory.store(GuestAddress{arg(2)},bits,0);return finish(e_nointerface);}
            ++binding.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);
        }
        if(name=="AddRef")return finish(++binding.references);
        if(name=="Release"){const auto left=--binding.references;if(!left){url_bindings_.erase(found);(void)free_heap(memory,arg(0),state);}return finish(left);}
        if(name=="Abort"){if(binding.completed)return finish(0x80004005U);binding.aborted=true;return finish(0);}
        if(name=="Suspend" || name=="Resume")return finish(e_notimpl);
        if(name=="SetPriority"){binding.priority=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1)));return finish(0);}
        if(name=="GetPriority"){if(!arg(1))return finish(e_invalidarg);memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(binding.priority));return finish(0);}
        // GetBindResult(pclsidProtocol, pdwResult, pszResult, pdwReserved)
        if(!arg(1) || !arg(2))return finish(e_invalidarg);
        if(!binding.completed)return finish(0x8000FFFFU); // E_UNEXPECTED：繫結尚未結束
        memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(16));memory.store(GuestAddress{arg(2)},32,binding.result);if(arg(3))memory.store(GuestAddress{arg(3)},bits,0);
        return finish(0);
    }
    return nested_continuation(state,memory,"urlmon 下載",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto name=service.symbol;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto text=[&](std::u16string_view value){const auto bytes=utf16_bytes(value);const auto block=scratch(bytes.size());m.write(GuestAddress{block},bytes);return block;};
        const auto api=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve(dll,symbol).value,std::move(arguments));
        };
        const auto method=[&](std::uint64_t object,unsigned index){return m.load(GuestAddress{m.load(GuestAddress{object},bits)+std::uint64_t{index}*unit},bits);};
        const auto com=[&](std::uint64_t object,unsigned index,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            arguments.insert(arguments.begin(),object);co_return co_await service_callback::call_value(method(object,index),std::move(arguments));
        };
        const auto task_copy=[&](std::uint64_t source)->service_callback::Task<std::uint64_t>{
            if(!source)co_return 0;const auto value=read_utf16(m,GuestAddress{source},32768);const auto bytes=utf16_bytes(value);
            const auto target=co_await api("ole32.dll","CoTaskMemAlloc",{bytes.size()});if(target)m.write(GuestAddress{target},bytes);co_return target;
        };
        // STGMEDIUM：tymed、聯集、pUnkForRelease（x86 0／4／8、x64 0／8／16）。
        const auto copy_medium=[&](std::uint64_t source,std::uint64_t target)->service_callback::Task<std::uint32_t>{
            const auto tymed=static_cast<std::uint32_t>(m.load(GuestAddress{source},32));const auto handle=m.load(GuestAddress{source+unit},bits),owner=m.load(GuestAddress{source+2*unit},bits);
            std::uint64_t copy=0;
            switch(tymed){
            case 0:break;
            case 1:{ // TYMED_HGLOBAL
                const auto size=co_await api("kernel32.dll","GlobalSize",{handle});copy=co_await api("kernel32.dll","GlobalAlloc",{0x2,size});if(!copy)co_return e_outofmemory;
                const auto from=co_await api("kernel32.dll","GlobalLock",{handle}),to=co_await api("kernel32.dll","GlobalLock",{copy});
                if(size && from && to){std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));m.read(GuestAddress{from},bytes);m.write(GuestAddress{to},bytes);}
                (void)co_await api("kernel32.dll","GlobalUnlock",{handle});(void)co_await api("kernel32.dll","GlobalUnlock",{copy});break;}
            case 2:copy=co_await task_copy(handle);if(handle && !copy)co_return e_outofmemory;break; // TYMED_FILE
            case 4:case 8:if(handle)(void)co_await com(handle,1,{});copy=handle;break; // TYMED_ISTREAM／ISTORAGE
            case 16:copy=co_await api("user32.dll","CopyImage",{handle,0,0,0,0});if(handle && !copy)co_return e_outofmemory;break; // TYMED_GDI
            case 32:{ // TYMED_MFPICT：HGLOBAL 內的 METAFILEPICT（hMF 在 x86 12、x64 16）
                const unsigned size=x86_?16U:24U,offset=x86_?12U:16U;copy=co_await api("kernel32.dll","GlobalAlloc",{0x2,size});if(!copy)co_return e_outofmemory;
                const auto from=co_await api("kernel32.dll","GlobalLock",{handle}),to=co_await api("kernel32.dll","GlobalLock",{copy});
                if(from && to){std::vector<std::uint8_t> bytes(size);m.read(GuestAddress{from},bytes);m.write(GuestAddress{to},bytes);
                    const auto metafile=co_await api("gdi32.dll","CopyMetaFileW",{m.load(GuestAddress{from+offset},bits),0});m.store(GuestAddress{to+offset},bits,metafile);}
                (void)co_await api("kernel32.dll","GlobalUnlock",{handle});(void)co_await api("kernel32.dll","GlobalUnlock",{copy});break;}
            case 64:copy=co_await api("gdi32.dll","CopyEnhMetaFileW",{handle,0});if(handle && !copy)co_return e_outofmemory;break; // TYMED_ENHMF
            default:co_return e_invalidarg;
            }
            if(owner)(void)co_await com(owner,1,{});
            m.store(GuestAddress{target},32,tymed);m.store(GuestAddress{target+unit},bits,copy);m.store(GuestAddress{target+2*unit},bits,owner);co_return 0;
        };
        // BINDINFO 欄位位移：szExtraInfo、stgmedData、szCustomVerb、securityAttributes.lpSecurityDescriptor、pUnk。
        const unsigned extra_at=unit,medium_at=x86_?8U:16U,verb_at=x86_?28U:48U,descriptor_at=x86_?52U:80U,unk_at=x86_?76U:112U,bindinfo_size=x86_?84U:128U;
        try{
            if(name=="CopyStgMedium"){
                if(!call(0) || !call(1))co_return finish(e_pointer);
                co_return finish(co_await copy_medium(call(0),call(1)));
            }
            if(name=="ReleaseBindInfo"){
                const auto info=call(0);if(!info)co_return finish(0);
                const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{info},32));if(size<bindinfo_size)co_return finish(0);
                (void)co_await api("ole32.dll","CoTaskMemFree",{m.load(GuestAddress{info+extra_at},bits)});
                (void)co_await api("ole32.dll","ReleaseStgMedium",{info+medium_at});
                (void)co_await api("ole32.dll","CoTaskMemFree",{m.load(GuestAddress{info+verb_at},bits)});
                if(const auto unk=m.load(GuestAddress{info+unk_at},bits))(void)co_await com(unk,2,{});
                m.fill(GuestAddress{info+4},0,bindinfo_size-4);
                co_return finish(0);
            }
            if(name=="CopyBindInfo"){
                const auto source=call(0),target=call(1);if(!source || !target)co_return finish(e_pointer);
                const auto source_size=static_cast<std::uint32_t>(m.load(GuestAddress{source},32)),target_size=static_cast<std::uint32_t>(m.load(GuestAddress{target},32));
                if(source_size<bindinfo_size || target_size<bindinfo_size)co_return finish(e_invalidarg);
                std::vector<std::uint8_t> bytes(bindinfo_size);m.read(GuestAddress{source},bytes);m.write(GuestAddress{target},bytes);m.store(GuestAddress{target},32,target_size);
                m.store(GuestAddress{target+extra_at},bits,co_await task_copy(m.load(GuestAddress{source+extra_at},bits)));
                m.store(GuestAddress{target+verb_at},bits,co_await task_copy(m.load(GuestAddress{source+verb_at},bits)));
                if(const auto status=co_await copy_medium(source+medium_at,target+medium_at))co_return finish(status);
                // 安全描述元以 LocalAlloc 複製；pUnk 加參考。
                if(const auto descriptor=m.load(GuestAddress{source+descriptor_at},bits)){
                    const auto length=static_cast<std::uint32_t>(co_await api("advapi32.dll","GetSecurityDescriptorLength",{descriptor}));
                    const auto copy=length?co_await api("kernel32.dll","LocalAlloc",{0x40,length}):0;
                    if(copy){std::vector<std::uint8_t> sd(length);m.read(GuestAddress{descriptor},sd);m.write(GuestAddress{copy},sd);}
                    m.store(GuestAddress{target+descriptor_at},bits,copy);
                }
                if(const auto unk=m.load(GuestAddress{source+unk_at},bits))(void)co_await com(unk,1,{});
                co_return finish(0);
            }
            // 下載家族：(pCaller, szURL, …, lpfnCB)
            const bool wide=name.ends_with('W');
            const bool to_file=name.starts_with("URLDownloadToFile"),to_cache=name.starts_with("URLDownloadToCacheFile"),blocking=name.starts_with("URLOpenBlockingStream");
            const unsigned callback_index=to_cache?5U:(to_file || blocking)?4U:3U;const auto callback=call(callback_index);
            if(!call(1))co_return finish(e_invalidarg);
            const auto read_text=[&](std::uint64_t at){return wide?read_utf16(m,GuestAddress{at},32768):utf8_to_utf16_lossy(read_ansi(m,at,32768));};
            const auto url=read_text(call(1));if(url.empty())co_return finish(e_invalidarg);
            if(to_file && !call(2))co_return finish(e_invalidarg);
            if(to_cache && (!call(2) || !static_cast<std::uint32_t>(call(3))))co_return finish(e_outofmemory);
            if(blocking){if(!call(2))co_return finish(e_invalidarg);m.store(GuestAddress{call(2)},bits,0);}
            // 內部 IBinding 物件與虛擬表。
            std::uint64_t binding=0;
            if(callback){
                if(!url_binding_vtable_){
                    const auto table=runtime_allocate(m,{},9*unit);unsigned index=0;
                    for(const auto& descriptor:url_download_service_descriptors())if(descriptor.family=="internal-urlmon-binding")m.store(GuestAddress{table+unit*index++},bits,resolve(descriptor.family,descriptor.symbol).value);
                    url_binding_vtable_=table;
                }
                binding=allocate_heap(m,unit);if(!binding)co_return finish(e_outofmemory);m.store(GuestAddress{binding},bits,url_binding_vtable_);url_bindings_.emplace(binding,UrlBinding{});
                (void)co_await com(callback,1,{}); // AddRef
                const auto bindf=scratch(4),info=scratch(bindinfo_size);m.store(GuestAddress{info},32,bindinfo_size);
                (void)co_await com(callback,8,{bindf,info}); // GetBindInfo
                (void)co_await api("urlmon.dll","ReleaseBindInfo",{info});
            }
            const auto url_text=text(url);
            const auto complete=[&](std::uint32_t result)->service_callback::Task<std::uint32_t>{
                if(callback){
                    if(const auto found=url_bindings_.find(binding);found!=url_bindings_.end()){found->second.completed=true;found->second.result=result;}
                    (void)co_await com(callback,7,{result,0}); // OnStopBinding
                    (void)co_await com(callback,2,{}); // Release
                    if(url_bindings_.contains(binding))(void)co_await com(binding,2,{});
                }
                co_return result;
            };
            if(callback){const auto started=static_cast<std::uint32_t>(co_await com(callback,3,{0,binding}));if(started&0x80000000U)co_return finish(co_await complete(started));}
            // OnProgress(ulProgress, ulProgressMax, ulStatusCode, szStatusText)；回 E_ABORT 或已呼叫 Abort 時中止。
            const auto progress=[&](std::uint32_t status,std::uint64_t done,std::uint64_t total,std::uint64_t status_text)->service_callback::Task<bool>{
                if(!callback)co_return true;
                const auto result=static_cast<std::uint32_t>(co_await com(callback,6,{std::min<std::uint64_t>(done,UINT32_MAX),std::min<std::uint64_t>(total,UINT32_MAX),status,status_text}));
                const auto found=url_bindings_.find(binding);co_return result!=e_abort && (found==url_bindings_.end() || !found->second.aborted);
            };
            std::vector<std::uint8_t> data;std::u16string mime;std::uint32_t result=0;bool aborted=false;
            auto lower=url;for(auto& c:lower)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
            const auto chunk=scratch(65536),count_at=scratch(4);
            if(lower.starts_with(u"file:")){
                const auto path=scratch(2*32768),length_at=scratch(4);m.store(GuestAddress{length_at},32,32768);
                if(static_cast<std::uint32_t>(co_await api("shlwapi.dll","PathCreateFromUrlW",{url_text,path,length_at,0})))co_return finish(co_await complete(inet_invalid_url));
                const auto file=co_await api("kernel32.dll","CreateFileW",{path,0x80000000U,1,0,3,0x80,0});
                if(file==(x86_?0xffffffffULL:~0ULL))co_return finish(co_await complete(inet_resource_not_found));
                const auto size_at=scratch(8);(void)co_await api("kernel32.dll","GetFileSizeEx",{file,size_at});const auto total=m.load(GuestAddress{size_at},64);
                aborted=!co_await progress(1,0,0,url_text) || !co_await progress(4,0,total,url_text);
                while(!aborted){
                    m.store(GuestAddress{count_at},32,0);
                    if(!(co_await api("kernel32.dll","ReadFile",{file,chunk,65536,count_at,0})&UINT32_MAX)){result=inet_download_failure;break;}
                    const auto read=static_cast<std::uint32_t>(m.load(GuestAddress{count_at},32));if(!read)break;
                    const auto at=data.size();data.resize(at+read);m.read(GuestAddress{chunk},std::span(data).subspan(at,read));
                    if(data.size()>(std::size_t{1}<<31)){result=e_outofmemory;break;}
                    aborted=!co_await progress(5,data.size(),total,url_text);
                }
                (void)co_await api("kernel32.dll","CloseHandle",{file});
            }else{
                const auto agent_size=scratch(4),agent=scratch(512);m.store(GuestAddress{agent_size},32,511);
                std::u16string user_agent=u"Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 10.0)";
                if(!static_cast<std::uint32_t>(co_await api("urlmon.dll","ObtainUserAgentString",{0,agent,agent_size}))){auto value=read_ansi(m,agent,512);if(!value.empty())user_agent=utf8_to_utf16_lossy(value);}
                const auto host_begin=url.find(u"://");const auto host=host_begin==url.npos?url:url.substr(host_begin+3,url.find_first_of(u"/?#",host_begin+3)-host_begin-3);
                aborted=!co_await progress(1,0,0,text(host)) || !co_await progress(2,0,0,text(host));
                const auto session=aborted?0:co_await api("wininet.dll","InternetOpenW",{text(user_agent),0,0,0,0});
                if(!aborted && !session)result=inet_download_failure;
                std::uint64_t request=0;
                if(session){
                    if(!co_await progress(11,0,0,url_text))aborted=true;
                    else {request=co_await api("wininet.dll","InternetOpenUrlW",{session,url_text,0,0,0,0}); // 旗標維持預設（使用 WinINet 快取與 Cookie）
                        if(!request)result=inet_status(static_cast<std::uint32_t>(co_await api("kernel32.dll","GetLastError",{})));}
                }
                std::uint64_t total=0;
                if(request && (lower.starts_with(u"http:") || lower.starts_with(u"https:"))){
                    const auto value=scratch(4),size=scratch(4);
                    m.store(GuestAddress{size},32,4);if(co_await api("wininet.dll","HttpQueryInfoW",{request,0x20000013U,value,size,0})&UINT32_MAX){if(m.load(GuestAddress{value},32)>=400)result=inet_download_failure;}
                    m.store(GuestAddress{size},32,4);if(co_await api("wininet.dll","HttpQueryInfoW",{request,0x20000005U,value,size,0})&UINT32_MAX)total=m.load(GuestAddress{value},32);
                    const auto type=scratch(512);m.store(GuestAddress{size},32,510);if(co_await api("wininet.dll","HttpQueryInfoW",{request,1,type,size,0})&UINT32_MAX)mime=read_utf16(m,GuestAddress{type},255);
                }
                if(request && !result){
                    if(!mime.empty() && !co_await progress(13,0,total,text(mime)))aborted=true;
                    if(!aborted && !co_await progress(4,0,total,url_text))aborted=true;
                    while(!aborted){
                        m.store(GuestAddress{count_at},32,0);
                        if(!(co_await api("wininet.dll","InternetReadFile",{request,chunk,65536,count_at})&UINT32_MAX)){result=inet_status(static_cast<std::uint32_t>(co_await api("kernel32.dll","GetLastError",{})));break;}
                        const auto read=static_cast<std::uint32_t>(m.load(GuestAddress{count_at},32));if(!read)break;
                        const auto at=data.size();data.resize(at+read);m.read(GuestAddress{chunk},std::span(data).subspan(at,read));
                        if(data.size()>(std::size_t{1}<<31)){result=e_outofmemory;break;}
                        aborted=!co_await progress(5,data.size(),std::max<std::uint64_t>(total,data.size()),url_text);
                    }
                }
                if(request)(void)co_await api("wininet.dll","InternetCloseHandle",{request});
                if(session)(void)co_await api("wininet.dll","InternetCloseHandle",{session});
            }
            if(aborted)co_return finish(co_await complete(e_abort));
            if(result)co_return finish(co_await complete(result));
            if(!co_await progress(6,data.size(),data.size(),url_text))co_return finish(co_await complete(e_abort));
            // 交付結果。
            const auto write_file=[&](std::uint64_t path)->service_callback::Task<std::uint32_t>{
                const auto file=co_await api("kernel32.dll","CreateFileW",{path,0x40000000U,0,0,2,0x80,0});
                if(file==(x86_?0xffffffffULL:~0ULL))co_return 0x80070000U|(static_cast<std::uint32_t>(co_await api("kernel32.dll","GetLastError",{}))&0xffffU);
                std::uint32_t status=0;
                for(std::size_t at=0;at<data.size() && !status;at+=65536){
                    const auto size=static_cast<std::uint32_t>(std::min<std::size_t>(65536,data.size()-at));m.write(GuestAddress{chunk},std::span(data).subspan(at,size));
                    if(!(co_await api("kernel32.dll","WriteFile",{file,chunk,size,count_at,0})&UINT32_MAX))status=0x80070000U|(static_cast<std::uint32_t>(co_await api("kernel32.dll","GetLastError",{}))&0xffffU);
                }
                (void)co_await api("kernel32.dll","CloseHandle",{file});co_return status;
            };
            if(to_file){
                const auto path=wide?call(2):text(read_text(call(2)));
                co_return finish(co_await complete(co_await write_file(path)));
            }
            if(to_cache){
                // URL 快取：CreateUrlCacheEntryW 取得檔名、寫入後 CommitUrlCacheEntryW（NORMAL_CACHE_ENTRY）；檔名寫回呼叫端緩衝區。
                std::u16string extension;
                {const auto path=url.substr(0,url.find_first_of(u"?#"));const auto slash=path.rfind(u'/'),dot=path.rfind(u'.');
                    if(dot!=path.npos && (slash==path.npos || dot>slash) && path.size()-dot-1<=7)extension=path.substr(dot+1);}
                const auto cache_path=scratch(2*260);
                if(!(co_await api("wininet.dll","CreateUrlCacheEntryW",{url_text,data.size(),extension.empty()?0:text(extension),cache_path,0})&UINT32_MAX))co_return finish(co_await complete(inet_download_failure));
                if(const auto status=co_await write_file(cache_path))co_return finish(co_await complete(status));
                if(!(co_await api("wininet.dll","CommitUrlCacheEntryW",x86_?std::vector<std::uint64_t>{url_text,cache_path,0,0,0,0,1,0,0,0,0}:std::vector<std::uint64_t>{url_text,cache_path,0,0,1,0,0,0,0})&UINT32_MAX))
                    co_return finish(co_await complete(inet_download_failure));
                const auto path=read_utf16(m,GuestAddress{cache_path},260);const auto capacity=static_cast<std::uint32_t>(call(3));
                if(path.size()+1>capacity)co_return finish(co_await complete(e_outofmemory));
                if(wide)m.write(GuestAddress{call(2)},utf16_bytes(path));else{auto bytes=utf16_to_utf8(path);bytes.push_back(0);m.write(GuestAddress{call(2)},std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()));}
                (void)co_await progress(14,data.size(),data.size(),cache_path);
                co_return finish(co_await complete(0));
            }
            // 串流版：HGLOBAL 串流裝入整份資料並回到開頭。
            const auto stream_at=scratch(unit);
            if(static_cast<std::uint32_t>(co_await api("ole32.dll","CreateStreamOnHGlobal",{0,1,stream_at})))co_return finish(co_await complete(e_outofmemory));
            const auto stream=m.load(GuestAddress{stream_at},bits);
            for(std::size_t at=0;at<data.size();at+=65536){const auto size=static_cast<std::uint32_t>(std::min<std::size_t>(65536,data.size()-at));m.write(GuestAddress{chunk},std::span(data).subspan(at,size));
                (void)co_await com(stream,4,{chunk,size,0});}
            (void)co_await com(stream,5,x86_?std::vector<std::uint64_t>{0,0,0,0}:std::vector<std::uint64_t>{0,0,0}); // Seek(0, STREAM_SEEK_SET)
            if(blocking){m.store(GuestAddress{call(2)},bits,stream);co_return finish(co_await complete(0));}
            if(callback){
                // OnDataAvailable(BSCF_FIRSTDATANOTIFICATION | BSCF_LASTDATANOTIFICATION | BSCF_DATAFULLYAVAILABLE, 大小, FORMATETC, STGMEDIUM{TYMED_ISTREAM})
                const auto format=scratch(x86_?20U:32U),medium=scratch(3*unit);
                m.store(GuestAddress{format+2*unit},32,1);m.store(GuestAddress{format+2*unit+4},32,UINT32_MAX);m.store(GuestAddress{format+2*unit+8},32,4);
                m.store(GuestAddress{medium},32,4);m.store(GuestAddress{medium+unit},bits,stream);
                (void)co_await com(callback,9,{0x1|0x4|0x8,std::min<std::size_t>(data.size(),UINT32_MAX),format,medium});
            }
            (void)co_await com(stream,2,{});
            co_return finish(co_await complete(0));
        }catch(const MemoryFault&){co_return finish(e_invalidarg);}catch(const std::bad_alloc&){co_return finish(e_outofmemory);}
    });
}
}
