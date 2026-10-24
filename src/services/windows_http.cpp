#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/security_descriptor.hpp"
#include "http_proxy_configuration.hpp"
#include "internet_connection.hpp"
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <system_error>

namespace x64arm {
HttpState::HttpState(const HttpState& other):next(other.next),nearest_server(other.nearest_server){if(!other.handles.empty() || !other.connection_probes.empty() || other.cache_active)throw std::logic_error("無法複製使用中的 WinHTTP session");}
HttpState& HttpState::operator=(const HttpState& other){if(this!=&other)*this=HttpState(other);return *this;}
namespace {
struct HttpError {std::uint32_t code;};
std::string lower(std::string value){for(auto& c:value)if(c>='A' && c<='Z')c=static_cast<char>(c+32);return value;}
std::string latin(std::u16string_view text){std::string result;for(auto c:text){if(c>255 || !c)throw HttpError{12153};result.push_back(static_cast<char>(c));}return result;}
std::u16string wide_latin(std::string_view text){std::u16string result;for(char c:text)result.push_back(static_cast<unsigned char>(c));return result;}
std::string url_path(std::string_view path){constexpr char digits[]="0123456789ABCDEF";std::string result;for(char c:path){auto byte=static_cast<unsigned char>(c);if(byte<=32 || byte>=127 || c=='"' || c=='<' || c=='>'){result.push_back('%');result.push_back(digits[byte>>4]);result.push_back(digits[byte&15]);}else result.push_back(c);}return result;}
bool token(std::string_view text){return !text.empty() && std::all_of(text.begin(),text.end(),[](unsigned char c){return (c>='0' && c<='9') || (c>='a' && c<='z') || (c>='A' && c<='Z') || std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c))!=std::string_view::npos;});}
void add_headers(HttpHeaders& headers,std::u16string_view text,std::uint32_t flags){
    if(flags&0x00ffffffU)throw HttpError{50};if(flags&~0xf1000000U)throw HttpError{87};
    const auto raw=latin(text);auto updated=headers;
    for(std::size_t begin=0;begin<raw.size();){const auto end=raw.find("\r\n",begin);auto line=raw.substr(begin,end==std::string::npos?raw.size()-begin:end-begin);begin=end==std::string::npos?raw.size():end+2;if(line.empty())continue;
        const auto colon=line.find(':');if(colon==std::string::npos || !token(line.substr(0,colon)))throw HttpError{12153};
        auto name=line.substr(0,colon),value=line.substr(colon+1);const auto first=value.find_first_not_of(" \t");value=first==std::string::npos?"":value.substr(first);
        if(value.find_first_of("\r\n")!=std::string::npos)throw HttpError{12153};
        const auto normalized=lower(name);auto existing=std::find_if(updated.begin(),updated.end(),[&](const auto& field){return lower(field.first)==normalized;});
        if((flags&0x10000000U) && existing!=updated.end())throw HttpError{12155};
        if(flags&0x80000000U){if(existing==updated.end() && !(flags&0x20000000U))throw HttpError{12150};std::erase_if(updated,[&](const auto& field){return lower(field.first)==normalized;});if(!value.empty())updated.emplace_back(std::move(name),std::move(value));}
        else if((flags&0x41000000U) && existing!=updated.end())existing->second+=((flags&0x01000000U)?"; ":", ")+value;
        else updated.emplace_back(std::move(name),std::move(value));
    }
    if(updated.size()>1024)throw HttpError{8};headers=std::move(updated);
}
}
std::span<const WindowsServiceDescriptor> http_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"wininet.dll","InternetCheckConnectionA",3},{"wininet.dll","InternetCheckConnectionW",3},
        {"wininet.dll","InternetGetConnectedState",2},{"wininet.dll","InternetGetConnectedStateEx",4},{"wininet.dll","InternetGetConnectedStateExA",4},{"wininet.dll","InternetGetConnectedStateExW",4},
        {"wininet.dll","CreateUrlCacheEntryA",5},{"wininet.dll","CreateUrlCacheEntryW",5},{"wininet.dll","CommitUrlCacheEntryA",9,12},{"wininet.dll","CommitUrlCacheEntryW",9,12},{"wininet.dll","GetUrlCacheEntryInfoA",3},{"wininet.dll","GetUrlCacheEntryInfoW",3},{"wininet.dll","GetUrlCacheEntryInfoExA",7},{"wininet.dll","GetUrlCacheEntryInfoExW",7},{"wininet.dll","RetrieveUrlCacheEntryFileA",4},{"wininet.dll","RetrieveUrlCacheEntryFileW",4},{"wininet.dll","UnlockUrlCacheEntryFileA",2},{"wininet.dll","UnlockUrlCacheEntryFileW",2},{"wininet.dll","DeleteUrlCacheEntryA",1},{"wininet.dll","DeleteUrlCacheEntryW",1},{"wininet.dll","SetUrlCacheEntryInfoA",3},{"wininet.dll","SetUrlCacheEntryInfoW",3},{"wininet.dll","RetrieveUrlCacheEntryStreamA",5},{"wininet.dll","RetrieveUrlCacheEntryStreamW",5},{"wininet.dll","FindFirstUrlCacheEntryA",3},{"wininet.dll","FindFirstUrlCacheEntryW",3},{"wininet.dll","FindNextUrlCacheEntryA",3},{"wininet.dll","FindNextUrlCacheEntryW",3},{"wininet.dll","UnlockUrlCacheEntryFile",2},{"wininet.dll","ReadUrlCacheEntryStream",5},{"wininet.dll","ReadUrlCacheEntryStreamEx",4,2},{"wininet.dll","UnlockUrlCacheEntryStream",2},{"wininet.dll","FindCloseUrlCache",1},
        {"wininet.dll","InternetOpenA",5},{"wininet.dll","InternetOpenW",5},{"wininet.dll","InternetOpenUrlA",6},{"wininet.dll","InternetOpenUrlW",6},
        {"wininet.dll","InternetConnectA",8},{"wininet.dll","InternetConnectW",8},{"wininet.dll","HttpOpenRequestA",8},{"wininet.dll","HttpOpenRequestW",8},
        {"wininet.dll","HttpSendRequestA",5},{"wininet.dll","HttpSendRequestW",5},{"wininet.dll","HttpSendRequestExA",5},{"wininet.dll","HttpSendRequestExW",5},
        {"wininet.dll","HttpEndRequestA",4},{"wininet.dll","HttpEndRequestW",4},{"wininet.dll","InternetWriteFile",4},{"wininet.dll","InternetCrackUrlA",4},{"wininet.dll","InternetCrackUrlW",4},
        {"wininet.dll","HttpAddRequestHeadersA",4},{"wininet.dll","HttpAddRequestHeadersW",4},
        {"wininet.dll","InternetSetOptionA",4},{"wininet.dll","InternetSetOptionW",4},{"wininet.dll","InternetQueryOptionA",4},{"wininet.dll","InternetQueryOptionW",4},
        {"wininet.dll","InternetReadFile",4},{"wininet.dll","InternetQueryDataAvailable",4},{"wininet.dll","HttpQueryInfoA",5},{"wininet.dll","HttpQueryInfoW",5},{"wininet.dll","InternetCloseHandle",1},
        {"winhttp.dll","WinHttpOpen",5},{"winhttp.dll","WinHttpConnect",4},{"winhttp.dll","WinHttpOpenRequest",7},
        {"winhttp.dll","WinHttpSetTimeouts",5},{"winhttp.dll","WinHttpAddRequestHeaders",4},{"winhttp.dll","WinHttpSendRequest",7},
        {"winhttp.dll","WinHttpSetOption",4},{"winhttp.dll","WinHttpQueryOption",4},
        {"winhttp.dll","WinHttpWriteData",4},{"winhttp.dll","WinHttpReceiveResponse",2},{"winhttp.dll","WinHttpReadData",4},
        {"winhttp.dll","WinHttpQueryDataAvailable",2},{"winhttp.dll","WinHttpQueryHeaders",6},{"winhttp.dll","WinHttpCloseHandle",1},
        {"winhttp.dll","WinHttpCrackUrl",4},{"winhttp.dll","WinHttpGetProxyForUrl",4},{"winhttp.dll","WinHttpDetectAutoProxyConfigUrl",2},
        {"winhttp.dll","WinHttpCreateProxyResolver",2},{"winhttp.dll","WinHttpGetProxyForUrlEx",4},
        {"winhttp.dll","WinHttpGetProxyResult",2},{"winhttp.dll","WinHttpFreeProxyResult",1},
        {"winhttp.dll","WinHttpSetStatusCallback",4}
    };return table;
}
namespace {
void harvest_http_io(const std::shared_ptr<NativeHttp>& task,process_io::Counters& accounted){
    if(!task)return;const auto snapshot=task->io_counters();process_io::Counters delta;
    for(unsigned i=0;i<6;++i)delta.values[i]=snapshot.values[i]-accounted.values[i];
    process_io::record(delta);accounted=snapshot;
}
}
void WindowsServices::account_http_io(HttpHandle& request){harvest_http_io(request.pending,request.io_accounted);}
void WindowsServices::cancel_http_connection_probe(std::uint64_t thread){
    const auto found=runtime_.http.connection_probes.find(thread);if(found==runtime_.http.connection_probes.end())return;
    const auto handle=found->second;runtime_.http.connection_probes.erase(found);const auto entry=runtime_.http.handles.find(handle);if(entry==runtime_.http.handles.end())return;
    if(entry->second.pending){auto task=entry->second.pending;task->cancel();account_http_io(entry->second);
        if(!task->done())canceled_http_io_.push_back({task,entry->second.io_accounted});else account_http_io(entry->second);
    }runtime_.http.handles.erase(entry);
}
void WindowsServices::poll_http_io(){
    for(auto& [id,request]:runtime_.http.handles){(void)id;account_http_io(request);}
    for(auto it=canceled_http_io_.begin();it!=canceled_http_io_.end();){
        // done acquire 之後再取最後快照，涵蓋取消與完成之間已實際傳輸的資料。
        const bool done=it->task->done();harvest_http_io(it->task,it->accounted);
        if(done)it=canceled_http_io_.erase(it);else ++it;
    }
}
bool WindowsServices::http_ready(std::uint64_t handle) const {
    if(const auto found=autoproxy_.find(handle);found!=autoproxy_.end())return (found->second.session && !runtime_.http.handles.contains(found->second.session)) || found->second.task->done();
    const auto found=runtime_.http.handles.find(handle);return found==runtime_.http.handles.end() || !found->second.pending || found->second.pending->done();
}
bool WindowsServices::http_waiting() const {
    return std::any_of(threads_.items.begin(),threads_.items.end(),[](const auto& item){return item.second.status==ThreadStatus::Waiting && item.second.wait.kind==ThreadWaitKind::Http;});
}
std::optional<std::uint32_t> WindowsServices::invoke_http(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol=="WinHttpGetProxyForUrl" || service.symbol=="WinHttpDetectAutoProxyConfigUrl" || service.symbol=="WinHttpCreateProxyResolver" || service.symbol=="WinHttpGetProxyForUrlEx" || service.symbol=="WinHttpGetProxyResult" || service.symbol=="WinHttpFreeProxyResult" || service.symbol=="WinHttpSetStatusCallback")return invoke_autoproxy(state,memory,service);
    if(service.symbol.find("UrlCache")!=std::string_view::npos)return invoke_url_cache(state,memory,service);
    const WindowsCall arg(state,memory,service);const bool inet=service.family=="wininet.dll",ansi=inet && (service.symbol.ends_with('A') || service.symbol=="InternetGetConnectedStateEx");
    std::string_view symbol=service.symbol;
    if(inet){
        if(symbol.starts_with("InternetOpenUrl"))symbol="InternetOpenUrl";
        else if(symbol.starts_with("InternetOpen"))symbol="WinHttpOpen";
        else if(symbol.starts_with("InternetConnect"))symbol="WinHttpConnect";
        else if(symbol.starts_with("HttpOpenRequest"))symbol="WinHttpOpenRequest";
        else if(symbol.starts_with("HttpSendRequestEx"))symbol="HttpSendRequestEx";
        else if(symbol.starts_with("HttpSendRequest"))symbol="HttpSendRequest";
        else if(symbol.starts_with("HttpEndRequest"))symbol="HttpEndRequest";
        else if(symbol.starts_with("HttpAddRequestHeaders"))symbol="WinHttpAddRequestHeaders";
        else if(symbol.starts_with("InternetSetOption"))symbol="InternetSetOption";
        else if(symbol.starts_with("InternetQueryOption"))symbol="InternetQueryOption";
        else if(symbol.starts_with("InternetCrackUrl"))symbol="WinHttpCrackUrl";
        else if(symbol.starts_with("HttpQueryInfo"))symbol="WinHttpQueryHeaders";
        else if(symbol=="InternetCloseHandle")symbol="WinHttpCloseHandle";
        else if(symbol=="InternetReadFile")symbol="WinHttpReadData";
        else if(symbol=="InternetQueryDataAvailable")symbol="WinHttpQueryDataAvailable";
    }
    const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};const unsigned unit=x86_?4:8;const std::uint8_t pointer_bits=x86_?32:64;std::uint64_t result=0;
    auto& handles=runtime_.http.handles;
    const auto text=[&](std::uint64_t address,std::uint32_t length=UINT32_MAX){
        if(!address){if(length && length!=UINT32_MAX)throw HttpError{87};return std::u16string{};}
        if(ansi){std::string value;if(length==UINT32_MAX)value=read_ansi(memory,address,65536);else {if(length>65536)throw HttpError{87};value.resize(length);memory.read(GuestAddress{address},{reinterpret_cast<std::uint8_t*>(value.data()),value.size()});}return wide_latin(value);}
        if(length==UINT32_MAX)return read_utf16(memory,GuestAddress{address},65536);if(length>65536)throw HttpError{87};
        std::u16string value;value.reserve(length);for(unsigned i=0;i<length;++i)value.push_back(static_cast<char16_t>(memory.load(GuestAddress{address+i*2},16)));return value;
    };
    const auto insert=[&](HttpHandle object){if(handles.size()>=4096)throw HttpError{8};const auto handle=runtime_.http.next++;handles.emplace(handle,std::move(object));return handle;};
    const auto start=[&](HttpHandle& request){
        request.io_accounted={};
        if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許 WinHTTP 外部 I/O");
        if(request.inet){request.pending=read_http_cache(request);if(request.pending){request.cache_hit=true;return;}if(request.cache_flags&0x01000000U)throw HttpError{2};}
        request.cache_hit=false;request.pending=start_native_http(request.input);if(!request.pending)throw HttpError{50};
    };
    const auto remember_server=[&](const HttpHandle& request){
        if(request.inet && request.received && !request.response.error){const auto host=internet_connection::url_host(request.response.effective_url.empty()?request.input.url:request.response.effective_url);if(host)runtime_.http.nearest_server=*host;}
    };
    try {
        if(service.symbol=="InternetQueryDataAvailable" && (d(2) || arg(3)))throw HttpError{87};
        if(service.symbol=="InternetReadFile" || service.symbol=="InternetWriteFile"){if(!arg(3))throw HttpError{87};memory.store(GuestAddress{arg(3)},32,0);}
        if(symbol.starts_with("InternetGetConnectedState")){
            // 依宿主網路設定：有已啟用、非回送且有預設閘道的介面即為 LAN 連線（一般 PC 回報 LAN｜RAS_INSTALLED）；否則 OFFLINE。
            const bool extended=symbol.starts_with("InternetGetConnectedStateEx");
            if(!arg(0) || d(extended?3:1))throw HttpError{87};
            NetworkConfiguration config;bool online=false;
            if(runtime_.network.external_io && snapshot_network_configuration(config))
                online=std::any_of(config.interfaces.begin(),config.interfaces.end(),[](const auto& item){return item.status==1 && item.type!=24 && !item.gateways.empty();});
            memory.check(GuestAddress{arg(0)},4,Access::Write);memory.store(GuestAddress{arg(0)},32,online?0x12U:0x20U);
            if(extended && arg(1) && d(2)){
                const std::u16string name=online?u"LAN Connection":u"";const auto count=std::min<std::size_t>(name.size(),d(2)-1);
                if(ansi){std::vector<std::uint8_t> bytes(name.begin(),name.begin()+static_cast<std::ptrdiff_t>(count));bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);}
                else memory.write(GuestAddress{arg(1)},utf16_bytes(name.substr(0,count)));
            }
            result=online?1:0;
        }else if(symbol.starts_with("InternetCheckConnection")){
            if(d(2) || (d(1)&~1U))throw HttpError{87};if(!runtime_.network.external_io)throw HttpError{2250};
            auto pending=runtime_.http.connection_probes.find(threads_.current);std::uint64_t id=0;
            if(pending==runtime_.http.connection_probes.end()){
                std::string host=runtime_.http.nearest_server;
                if(arg(0)){const auto url=ansi?read_ansi(memory,arg(0),65536):utf16_to_utf8(text(arg(0)));const auto extracted=internet_connection::url_host(url);if(!extracted)throw HttpError{12005};host=*extracted;}
                if(host.empty())throw HttpError{2250};if(handles.size()>=4096)throw HttpError{8};
                HttpHandle request;request.kind=HttpHandle::Kind::Probe;request.inet=true;request.host=host;
                try{request.pending=std::make_shared<internet_connection::Probe>(std::move(host));}catch(const std::system_error&){throw HttpError{8};}
                id=insert(std::move(request));try{runtime_.http.connection_probes.emplace(threads_.current,id);}catch(...){handles.erase(id);throw;}
            }else id=pending->second;
            const auto entry=handles.find(id);if(entry==handles.end()){runtime_.http.connection_probes.erase(threads_.current);throw HttpError{6};}auto& request=entry->second;
            if(!request.pending->done()){auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::Http;thread.wait.handles={id};threads_.yield=true;notify_concurrency_changed();return std::nullopt;}
            const auto response=request.pending->take();account_http_io(request);if(!response.error)runtime_.http.nearest_server=request.host;
            runtime_.http.connection_probes.erase(threads_.current);handles.erase(id);if(response.error)throw HttpError{response.error};result=1;
        }else if(symbol=="WinHttpCrackUrl"){
            if(!arg(0) || !arg(3) || (d(2)&~0x4000U))throw HttpError{(d(2)&~0x4000U)?50U:87U};
            const auto size=x86_?60U:104U;memory.check(GuestAddress{arg(3)},size,Access::Read);memory.check(GuestAddress{arg(3)},size,Access::Write);
            if(memory.load(GuestAddress{arg(3)},32)!=size)throw HttpError{87};const auto url=text(arg(0),d(1)?d(1):UINT32_MAX);
            const auto colon=url.find(u"://");if(colon==std::u16string::npos)throw HttpError{12005};auto scheme=lower(utf16_to_utf8(url.substr(0,colon)));if(scheme!="http" && scheme!="https")throw HttpError{12006};
            auto end=url.find_first_of(u"/?#",colon+3);if(end==std::u16string::npos)end=url.size();auto host=colon+3;const auto at=url.rfind(u'@',end?end-1:0);
            std::array<std::pair<std::size_t,std::size_t>,6> parts{};parts[0]={0,colon};
            if(at!=std::u16string::npos && at>=host){if(d(2)&0x4000)throw HttpError{12005};const auto pass=url.find(u':',host);parts[2]={host,(pass<at?pass:at)-host};if(pass<at)parts[3]={pass+1,at-pass-1};host=at+1;}
            if(host>=end)throw HttpError{12005};auto host_end=end;std::size_t port_at=std::u16string::npos;
            if(url[host]==u'['){const auto close=url.find(u']',host);if(close==std::u16string::npos || close>=end)throw HttpError{12005};host_end=close+1;if(host_end<end){if(url[host_end]!=u':')throw HttpError{12005};port_at=host_end+1;}}
            else {const auto port=url.find(u':',host);if(port<end){host_end=port;port_at=port+1;}}
            std::uint32_t port=scheme=="https"?443U:80U;if(port_at!=std::u16string::npos){port=0;if(port_at>=end)throw HttpError{12005};for(auto i=port_at;i<end;++i){if(url[i]<u'0' || url[i]>u'9' || port>6553)throw HttpError{12005};port=port*10+static_cast<unsigned>(url[i]-u'0');}if(port>65535)throw HttpError{12005};}
            parts[1]={host,host_end-host};const auto extra=url.find_first_of(u"?#",end);const auto path_end=extra==std::u16string::npos?url.size():extra;
            const std::array<unsigned,6> offsets=x86_?std::array<unsigned,6>{4,16,28,36,44,52}:std::array<unsigned,6>{8,24,40,56,72,88};
            const auto extra_length=memory.load(GuestAddress{arg(3)+offsets[5]+unit},32);parts[4]={end,(extra_length?path_end:url.size())-end};if(extra!=std::u16string::npos)parts[5]={extra,url.size()-extra};
            bool short_buffer=false;
            for(unsigned i=0;i<parts.size();++i){const auto cell=arg(3)+offsets[i],capacity=memory.load(GuestAddress{cell+unit},32),pointer=memory.load(GuestAddress{cell},pointer_bits);if(!capacity)continue;
                const auto [begin,length]=parts[i];(void)begin;if(pointer && capacity<=length){memory.store(GuestAddress{cell+unit},32,length+1);short_buffer=true;}else if(pointer)memory.check(GuestAddress{pointer},(length+1)*(ansi?1:2),Access::Write);}
            if(short_buffer)throw HttpError{122};
            for(unsigned i=0;i<parts.size();++i){const auto cell=arg(3)+offsets[i],capacity=memory.load(GuestAddress{cell+unit},32),pointer=memory.load(GuestAddress{cell},pointer_bits);if(!capacity)continue;const auto [begin,length]=parts[i];
                if(pointer){if(ansi){const auto part=latin(url.substr(begin,length));std::vector<std::uint8_t> bytes(part.begin(),part.end());bytes.push_back(0);memory.write(GuestAddress{pointer},bytes);}else memory.write(GuestAddress{pointer},utf16_bytes(url.substr(begin,length)));}else memory.store(GuestAddress{cell},pointer_bits,length?arg(0)+begin*(ansi?1:2):0);memory.store(GuestAddress{cell+unit},32,length);}
            memory.store(GuestAddress{arg(3)+(x86_?12:20)},32,scheme=="https"?2:1);memory.store(GuestAddress{arg(3)+(x86_?24:36)},16,port);result=1;
        }else if(symbol=="WinHttpOpen"){
            if(d(4)&~(inet?0U:0x10000000U))throw HttpError{50};if(d(1)==4)throw HttpError{50};if(d(1)!=0 && d(1)!=1 && d(1)!=3)throw HttpError{87};
            HttpHandle session;session.inet=inet;session.asynchronous=(d(4)&0x10000000U)!=0;session.agent=latin(text(arg(0)));if(session.agent.find_first_of("\r\n")!=std::string::npos)throw HttpError{87};
            if(d(1)==3){session.input.proxy=utf16_to_utf8(text(arg(2)));if(session.input.proxy.empty() || session.input.proxy.find_first_of("\r\n")!=std::string::npos)throw HttpError{87};session.input.bypass=utf16_to_utf8(text(arg(3)));if(session.input.bypass.find_first_of("\r\n")!=std::string::npos)throw HttpError{87};}
            if(d(1)==0){
                if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
                RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw HttpError{transaction.error};
                if(!inet){
                    const auto path=http_proxy::key(x86_);if(runtime_.registry.keys.contains(path))(void)registry_security_access(path,1);
                    const auto config=http_proxy::read(runtime_.registry,x86_);session.input.proxy=config.proxy;session.input.bypass=config.bypass;
                }else{
                    const auto key=runtime_.registry.keys.find(std::u16string(1,u'\1')+u"\\software\\microsoft\\windows\\currentversion\\internet settings");
                    if(key!=runtime_.registry.keys.end())for(const auto& [name,value]:key->second)if((name==u"proxyenable" || name==u"autodetect" || name==u"autoconfigurl") && std::any_of(value.bytes.begin(),value.bytes.end(),[](auto byte){return byte!=0;}))throw HttpError{50};
                }
            }
            result=insert(std::move(session));
        }else{
            const auto found=handles.find(arg(0));if(found==handles.end() || found->second.kind==HttpHandle::Kind::Probe)throw HttpError{6};auto& object=found->second;
            const auto require=[&](HttpHandle::Kind kind){if(object.kind!=kind)throw HttpError{12018};};
            if(symbol=="WinHttpCloseHandle"){
                std::vector<std::uint64_t> closing{arg(0)};for(std::size_t i=0;i<closing.size();++i)for(const auto& [id,item]:handles)if(item.parent==closing[i])closing.push_back(id);for(auto id:closing){
                    close_http_callbacks(state,memory,id);
                    auto& request=handles.at(id);if(request.pending){request.pending->cancel();account_http_io(request);
                        if(!request.pending->done())canceled_http_io_.push_back({request.pending,request.io_accounted});
                        else account_http_io(request);
                    }handles.erase(id);
                }result=1;
            }else if(symbol=="InternetOpenUrl"){
                require(HttpHandle::Kind::Session);if(!arg(1))throw HttpError{87};
                if(d(4)&~0x85e00300U)throw HttpError{50};
                auto pending=object.opening.find(threads_.current);std::uint64_t id=0;
                if(pending==object.opening.end()){
                    HttpHandle request;request.kind=HttpHandle::Kind::Request;request.inet=inet;request.cache_flags=d(symbol=="InternetOpenUrl"?4:6);request.parent=arg(0);request.input=object.input;request.input.url=ansi?read_ansi(memory,arg(1)):utf16_to_utf8(text(arg(1)));request.input.method="GET";request.context=arg(5);request.input.follow_redirects=!(d(4)&0x200000U);
                    const auto authority=request.input.url.find("://");const auto path_at=authority==std::string::npos?authority:request.input.url.find('/',authority+3);request.path=path_at==std::string::npos?"/":request.input.url.substr(path_at);
                    const auto scheme=request.input.url.substr(0,request.input.url.find(':'));if(lower(scheme)!="http" && lower(scheme)!="https")throw HttpError{12006};
                    if((d(4)&0x800000U) && lower(scheme)!="https")throw HttpError{12005};
                    if(!object.agent.empty())request.input.headers.emplace_back("User-Agent",object.agent);if(arg(2))add_headers(request.input.headers,text(arg(2),d(3)),0x20000000);
                    if(d(4)&0x80000100U)request.input.headers.emplace_back("Cache-Control","no-cache");
                    start(request);id=insert(std::move(request));object.opening.emplace(threads_.current,id);
                }else id=pending->second;
                auto item=handles.find(id);if(item==handles.end()){object.opening.erase(threads_.current);throw HttpError{6};}auto& request=item->second;
                if(!request.pending->done()){auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::Http;thread.wait.handles={id};threads_.yield=true;notify_concurrency_changed();return std::nullopt;}
                request.response=request.pending->take();account_http_io(request);request.pending.reset();object.opening.erase(threads_.current);if(request.response.error){const auto error=request.response.error;handles.erase(id);throw HttpError{error};}
                request.received=request.sent=true;cache_http_response(request);remember_server(request);result=id;
            }else if(symbol=="WinHttpSetOption" || symbol=="WinHttpQueryOption"){
                #include "winhttp_options.inc"
            }else if(symbol=="InternetSetOption" || symbol=="InternetQueryOption"){
                const bool setting=symbol=="InternetSetOption";const auto option=d(1);const bool timeout=option==2 || option==5 || option==6;
                const unsigned timeout_index=option==2?1:option==5?2:3;
                const bool string=option==41 || option==28 || option==29;
                if(!timeout && option!=45 && option!=9 && !string)throw HttpError{12009};
                if(option==41)require(HttpHandle::Kind::Session);
                if(option==28 || option==29)require(HttpHandle::Kind::Connection);
                if(setting){
                    if(option==9)throw HttpError{87};if(object.pending)throw HttpError{12019};if(!arg(2))throw HttpError{87};
                    if(string){
                        auto value=text(arg(2),d(3));if(!value.empty() && !value.back())value.pop_back();
                        if(value.find(u'\0')!=value.npos || value.find_first_of(u"\r\n")!=value.npos)throw HttpError{87};
                        if(option==41)object.agent=latin(value);
                        else (option==28?object.input.username:object.input.password)=utf16_to_utf8(value);
                    }else{
                        const auto width=option==45?unit:4U;if(d(3)!=width)throw HttpError{12010};
                        const auto value=memory.load(GuestAddress{arg(2)},static_cast<std::uint8_t>(width*8));
                        if(timeout){if(value>INT32_MAX && value!=UINT32_MAX)throw HttpError{87};object.input.timeouts[timeout_index]=static_cast<std::int32_t>(value);}
                        else object.context=value;
                    }
                }else{
                    if(!arg(3))throw HttpError{87};const auto capacity=memory.load(GuestAddress{arg(3)},32);memory.check(GuestAddress{arg(3)},4,Access::Write);
                    std::vector<std::uint8_t> bytes;
                    if(string){
                        const auto& value=option==41?object.agent:option==28?object.input.username:object.input.password;
                        if(ansi){const auto narrow=option==41?value:latin(utf8_to_utf16(value));bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);}else bytes=utf16_bytes(option==41?wide_latin(value):utf8_to_utf16(value));
                    }else{
                        const auto value=timeout?static_cast<std::uint32_t>(object.input.timeouts[timeout_index]):option==45?object.context:object.kind==HttpHandle::Kind::Session?1U:object.kind==HttpHandle::Kind::Connection?4U:13U;
                        bytes.resize(option==45?unit:4U);for(unsigned i=0;i<bytes.size();++i)bytes[i]=static_cast<std::uint8_t>(value>>(i*8));
                    }
                    if(!arg(2) || capacity<bytes.size()){memory.store(GuestAddress{arg(3)},32,bytes.size());throw HttpError{122};}
                    memory.write(GuestAddress{arg(2)},bytes);memory.store(GuestAddress{arg(3)},32,bytes.size()-(string?(ansi?1U:2U):0U));
                }
                result=1;
            }else if(symbol=="WinHttpSetTimeouts"){
                if(object.kind==HttpHandle::Kind::Connection)throw HttpError{12018};auto timeouts=object.input.timeouts;
                for(unsigned i=0;i<4;++i){const auto value=static_cast<std::int32_t>(d(i+1));if(value< -1)throw HttpError{87};timeouts[i]=value;}if(object.pending)throw HttpError{12019};object.input.timeouts=timeouts;result=1;
            }else if(symbol=="WinHttpConnect"){
                require(HttpHandle::Kind::Session);
                if(object.asynchronous)throw HttpError{50}; // 非同步 session 現階段提供代理解析；一般 HTTP 串流仍需獨立的非同步後端。
                if((inet?(d(5)!=3 || d(6)!=0):d(3)!=0) || !arg(1) || d(2)>65535)throw HttpError{87};HttpHandle connection;connection.kind=HttpHandle::Kind::Connection;connection.parent=arg(0);connection.inet=inet;connection.context=object.context;connection.input=object.input;connection.agent=object.agent;connection.host=utf16_to_utf8(text(arg(1)));connection.port=static_cast<std::uint16_t>(d(2));if(inet){connection.context=arg(7);connection.input.username=utf16_to_utf8(text(arg(3)));connection.input.password=utf16_to_utf8(text(arg(4)));}
                if(connection.host.empty() || connection.host.find_first_of("/@?# \r\n")!=std::string::npos)throw HttpError{12005};result=insert(std::move(connection));
            }else if(symbol=="WinHttpOpenRequest"){
                require(HttpHandle::Kind::Connection);if(d(6)&~(inet?0x85e00300U:0x00800100U))throw HttpError{50};HttpHandle request;request.kind=HttpHandle::Kind::Request;request.inet=inet;request.cache_flags=d(symbol=="InternetOpenUrl"?4:6);request.parent=arg(0);request.context=object.context;request.agent=object.agent;request.input=object.input;request.secure=(d(6)&0x00800000U)!=0;if(inet){request.context=arg(7);request.input.follow_redirects=!(d(6)&0x200000U);}
                request.input.method=arg(1)?latin(text(arg(1))):"GET";if(!token(request.input.method))throw HttpError{87};
                if(arg(3) && text(arg(3))!=u"HTTP/1.1")throw HttpError{50};request.path=arg(2)?utf16_to_utf8(text(arg(2))):"/";if(request.path.empty())request.path="/";if(request.path[0]!='/')request.path="/"+request.path;
                const auto port=object.port?object.port:request.secure?443:80;auto host=object.host;if(host.find(':')!=std::string::npos && host.front()!='[')host="["+host+"]";
                request.input.url=std::string(request.secure?"https://":"http://")+host+":"+std::to_string(port)+url_path(request.path);
                if(d(6)&(inet?0x80000100U:0x100U)){request.input.headers.emplace_back("Cache-Control","no-cache");request.input.headers.emplace_back("Pragma","no-cache");}
                if(!object.agent.empty())request.input.headers.emplace_back("User-Agent",object.agent);if(arg(4))add_headers(request.input.headers,u"Referer: "+text(arg(4)),0x20000000);
                if(arg(5)){std::string accepts;for(unsigned n=0;;++n){if(n>=128)throw HttpError{87};const auto at=memory.load(GuestAddress{arg(5)+n*unit},pointer_bits);if(!at)break;if(!accepts.empty())accepts+=", ";accepts+=latin(text(at));}if(accepts.find_first_of("\r\n")!=std::string::npos)throw HttpError{87};if(!accepts.empty())request.input.headers.emplace_back("Accept",accepts);}result=insert(std::move(request));
            }else{
                require(HttpHandle::Kind::Request);
                if(symbol=="WinHttpAddRequestHeaders"){if(inet && !arg(1))throw HttpError{87};if(object.pending)throw HttpError{12019};add_headers(object.input.headers,text(arg(1),d(2)),d(3));result=1;}
                else if(symbol=="HttpSendRequest"){
                    if(!object.pending){
                        if(d(4)>16*1024*1024 || (d(4) && !arg(3)) || (!ansi && arg(1) && d(2)==UINT32_MAX))throw HttpError{87};
                        auto headers=object.input.headers;if(arg(1))add_headers(headers,text(arg(1),d(2)),0xa0000000U);
                        std::vector<std::uint8_t> body(d(4));if(d(4))memory.read(GuestAddress{arg(3)},body);
                        object.input.headers=std::move(headers);object.input.body=std::move(body);object.total=d(4);object.cursor=0;object.response={};object.sent=true;object.received=false;start(object);
                    }
                    if(!object.pending->done()){auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::Http;thread.wait.handles={arg(0)};threads_.yield=true;notify_concurrency_changed();return std::nullopt;}
                    object.response=object.pending->take();account_http_io(object);object.pending.reset();if(object.response.error)throw HttpError{object.response.error};object.received=true;cache_http_response(object);remember_server(object);result=1;
                }
                else if(symbol=="HttpSendRequestEx"){
                    // INTERNET_BUFFERS 提供首段標頭、資料與 dwBufferTotal；其餘資料由 InternetWriteFile 補齊，HttpEndRequest 才送出。dwBufferTotal 為 0 時以實際寫入量為準。
                    if(object.pending || (object.sent && !object.received))throw HttpError{12019};if(arg(2))throw HttpError{87};
                    auto headers=object.input.headers;std::vector<std::uint8_t> body;std::uint32_t total=0;
                    if(const auto in=arg(1)){
                        if(memory.load(GuestAddress{in},32)!=(x86_?40U:56U))throw HttpError{87};if(memory.load(GuestAddress{in+unit},pointer_bits))throw HttpError{50};
                        const auto header=memory.load(GuestAddress{in+2*unit},pointer_bits),data=memory.load(GuestAddress{in+3*unit+8},pointer_bits);
                        const auto header_length=static_cast<std::uint32_t>(memory.load(GuestAddress{in+3*unit},32)),length=static_cast<std::uint32_t>(memory.load(GuestAddress{in+4*unit+8},32));
                        total=static_cast<std::uint32_t>(memory.load(GuestAddress{in+4*unit+12},32));
                        if(total>16*1024*1024 || length>16*1024*1024 || (total && length>total) || (length && !data))throw HttpError{87};
                        if(header)add_headers(headers,text(header,header_length?header_length:UINT32_MAX),0xa0000000U);
                        body.resize(length);if(length)memory.read(GuestAddress{data},body);
                    }
                    object.input.headers=std::move(headers);object.input.body=std::move(body);object.total=total;object.cursor=0;object.response={};object.sent=true;object.received=false;object.context=arg(4)?arg(4):object.context;result=1;
                }else if(symbol=="InternetWriteFile"){
                    if(!object.sent || object.received || object.pending)throw HttpError{12019};const std::uint32_t limit=object.total?object.total:16*1024*1024;
                    if(d(2)>limit-std::min<std::size_t>(limit,object.input.body.size()) || (d(2) && !arg(1)))throw HttpError{87};
                    std::vector<std::uint8_t> data(d(2));if(d(2))memory.read(GuestAddress{arg(1)},data);object.input.body.insert(object.input.body.end(),data.begin(),data.end());memory.store(GuestAddress{arg(3)},32,d(2));result=1;
                }else if(symbol=="HttpEndRequest"){
                    if(arg(1))throw HttpError{87};
                    if(!object.pending){if(!object.sent || object.received || (object.total && object.input.body.size()!=object.total))throw HttpError{12019};object.total=static_cast<std::uint32_t>(object.input.body.size());start(object);}
                    if(!object.pending->done()){auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::Http;thread.wait.handles={arg(0)};threads_.yield=true;notify_concurrency_changed();return std::nullopt;}
                    object.response=object.pending->take();account_http_io(object);object.pending.reset();if(object.response.error)throw HttpError{object.response.error};object.received=true;cache_http_response(object);remember_server(object);result=1;
                }
                else if(symbol=="WinHttpSendRequest"){
                    if(object.pending || (object.sent && object.total!=d(5)))throw HttpError{12019};if(d(5)>16*1024*1024 || d(4)>d(5) || (d(4) && !arg(3)))throw HttpError{87};
                    auto headers=object.input.headers;if(arg(1))add_headers(headers,text(arg(1),d(2)),0xa0000000);
                    for(const auto& [name,value]:headers){const auto key=lower(name);if(key=="transfer-encoding")throw HttpError{50};if(key=="content-length"){std::uint64_t amount=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),amount);if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || amount!=d(5))throw HttpError{87};}}
                    std::vector<std::uint8_t> body(d(4));if(d(4))memory.read(GuestAddress{arg(3)},body);object.input.headers=std::move(headers);object.input.body=std::move(body);object.total=d(5);object.context=arg(6);object.response={};object.cursor=0;object.sent=true;object.received=false;if(object.input.body.size()==object.total)start(object);result=1;
                }else if(symbol=="WinHttpWriteData"){
                    if(!object.sent || object.received || object.pending)throw HttpError{12019};if(!arg(3) || d(2)>object.total-object.input.body.size())throw HttpError{87};memory.check(GuestAddress{arg(3)},4,Access::Write);
                    std::vector<std::uint8_t> data(d(2));if(d(2))memory.read(GuestAddress{arg(1)},data);object.input.body.insert(object.input.body.end(),data.begin(),data.end());if(object.input.body.size()==object.total)start(object);memory.store(GuestAddress{arg(3)},32,d(2));result=1;
                }else if(symbol=="WinHttpReceiveResponse"){
                    if(arg(1))throw HttpError{87};if(!object.sent || object.received || object.input.body.size()!=object.total)throw HttpError{12019};
                    if(!object.pending)throw HttpError{12019};if(!object.pending->done()){auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::Http;thread.wait.handles={arg(0)};threads_.yield=true;notify_concurrency_changed();return std::nullopt;}
                    object.response=object.pending->take();account_http_io(object);object.pending.reset();if(object.response.error)throw HttpError{object.response.error};object.received=true;cache_http_response(object);remember_server(object);result=1;
                }else if(symbol=="WinHttpReadData" || symbol=="WinHttpQueryDataAvailable"){
                    if(!object.received)throw HttpError{12019};const bool read=symbol=="WinHttpReadData";const auto count=read?std::min<std::size_t>(d(2),object.response.body.size()-object.cursor):object.response.body.size()-object.cursor;const auto output=arg(read?3:1);if(!output && (read || !inet))throw HttpError{87};if(output)memory.check(GuestAddress{output},4,Access::Write);
                    if(read && count){memory.check(GuestAddress{arg(1)},count,Access::Write);memory.write(GuestAddress{arg(1)},std::span(object.response.body).subspan(object.cursor,count));object.cursor+=count;}if(output)memory.store(GuestAddress{output},32,count);result=1;
                }else if(symbol=="WinHttpQueryHeaders"){
                    const auto buffer=arg(inet?2:3),length_pointer=arg(inet?3:4),index_pointer=arg(inet?4:5);
                    const auto flags=d(1),query=flags&0xffffU;const bool request=(flags&0x80000000U)!=0,number=(flags&0x20000000U)!=0;
                    if(flags&~0xa000ffffU)throw HttpError{50};if(!request && !object.received)throw HttpError{12019};if(!length_pointer)throw HttpError{87};
                    const auto capacity=memory.load(GuestAddress{length_pointer},32);memory.check(GuestAddress{length_pointer},4,Access::Write);auto index=index_pointer?memory.load(GuestAddress{index_pointer},32):0;if(index_pointer)memory.check(GuestAddress{index_pointer},4,Access::Write);
                    const auto& headers=request?object.input.headers:object.response.headers;std::string value;
                    if(query==19 && !request)value=std::to_string(object.response.status);
                    else if(query==45)value=object.input.method;
                    else if(query==18)value=request?"HTTP/1.1":object.response.status_line.substr(0,object.response.status_line.find(' '));
                    else if(query==20 && !request){const auto space=object.response.status_line.find(' '),reason=space==std::string::npos?space:object.response.status_line.find(' ',space+1);value=reason==std::string::npos?"":object.response.status_line.substr(reason+1);}
                    else if(query==21 || query==22){value=request?object.input.method+" "+object.path+" HTTP/1.1":object.response.status_line;const std::string separator=query==22?"\r\n":std::string(1,'\0');value+=separator;for(const auto& [name,item]:headers)value+=name+": "+item+separator;value+=separator;}
                    else {static const std::map<std::uint32_t,std::string> names{{0,"mime-version"},{1,"content-type"},{2,"content-transfer-encoding"},{3,"content-id"},{4,"content-description"},{5,"content-length"},{6,"content-language"},{7,"allow"},{8,"public"},{9,"date"},{10,"expires"},{11,"last-modified"},{12,"message-id"},{13,"uri"},{14,"derived-from"},{15,"cost"},{16,"link"},{17,"pragma"},{23,"connection"},{24,"accept"},{25,"accept-charset"},{26,"accept-encoding"},{27,"accept-language"},{28,"authorization"},{29,"content-encoding"},{30,"forwarded"},{31,"from"},{32,"if-modified-since"},{33,"location"},{34,"orig-uri"},{35,"referer"},{36,"retry-after"},{37,"server"},{38,"title"},{39,"user-agent"},{40,"www-authenticate"},{41,"proxy-authenticate"},{42,"accept-ranges"},{43,"set-cookie"},{44,"cookie"},{46,"refresh"},{47,"content-disposition"},{48,"age"},{49,"cache-control"},{50,"content-base"},{51,"content-location"},{53,"content-range"},{54,"etag"},{55,"host"},{56,"if-match"},{57,"if-none-match"},{58,"if-range"},{59,"if-unmodified-since"},{60,"max-forwards"},{61,"proxy-authorization"},{62,"range"},{63,"transfer-encoding"},{64,"upgrade"},{65,"vary"},{66,"via"},{67,"warning"},{68,"expect"},{69,"proxy-connection"},{70,"unless-modified-since"},{75,"proxy-support"},{76,"authentication-info"},{77,"passport-urls"},{78,"passport-config"},{78,"max"}};
                        const auto field=query==65535?lower(latin(text(inet?buffer:arg(2)))):names.contains(query)?names.at(query):"";if(field.empty())throw HttpError{50};std::uint64_t n=0;bool matched=false;for(const auto& [name,item]:headers)if(lower(name)==field && n++==index){value=item;matched=true;break;}if(!matched)throw HttpError{12150};}
                    if(number){std::uint32_t numeric=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),numeric);if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())throw HttpError{12153};if(!buffer || capacity<4){memory.store(GuestAddress{length_pointer},32,4);throw HttpError{122};}memory.store(GuestAddress{buffer},32,numeric);memory.store(GuestAddress{length_pointer},32,4);}
                    else {std::vector<std::uint8_t> bytes;if(ansi){bytes.assign(value.begin(),value.end());bytes.push_back(0);}else bytes=utf16_bytes(wide_latin(value));const unsigned unit_size=ansi?1:2;if(query==21 && bytes.size()>=unit_size)bytes.resize(bytes.size()-unit_size);if(!buffer || capacity<bytes.size()){memory.store(GuestAddress{length_pointer},32,bytes.size());throw HttpError{122};}memory.write(GuestAddress{buffer},bytes);memory.store(GuestAddress{length_pointer},32,bytes.size()-(query==21?0:unit_size));}
                    if(index_pointer)memory.store(GuestAddress{index_pointer},32,index+1);result=1;
                }else throw HttpError{50};
            }
        }
    }catch(const HttpError& error){last_error_=error.code;}catch(const http_proxy::Error& error){last_error_=error.code;}catch(const SecurityDescriptorError& error){last_error_=error.code;}catch(const MemoryFault&){last_error_=998;}catch(const std::bad_alloc&){last_error_=8;}
    arg.return_to(state,result);return std::nullopt;
}
}
