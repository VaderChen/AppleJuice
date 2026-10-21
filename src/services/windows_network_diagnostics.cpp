#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace x64arm {
// ndfapi 網路診斷：Web incident 記錄目標 URL；執行診斷時實際做名稱解析與 TCP 連線測試，
// 以 MessageBoxW 顯示結果（對應 Windows 網路診斷精靈的互動），完成後回傳 S_OK。
namespace {
constexpr std::uint32_t invalid=0x80070057U,no_memory=0x8007000eU;
struct Target {std::string host;std::string port;bool valid{};};
Target parse_target(std::u16string_view url){
    Target result;const auto scheme_end=url.find(u"://");if(scheme_end==url.npos || !scheme_end)return result;
    auto scheme=utf16_to_utf8(url.substr(0,scheme_end));for(auto& c:scheme)c=static_cast<char>(c>='A' && c<='Z'?c+32:c);
    auto rest=url.substr(scheme_end+3);const auto end=rest.find_first_of(u"/?#");auto authority=rest.substr(0,end);
    if(const auto at=authority.rfind(u'@');at!=authority.npos)authority.remove_prefix(at+1);
    std::u16string host,port;
    if(authority.starts_with(u"[")){const auto close=authority.find(u']');if(close==authority.npos)return result;host=std::u16string(authority.substr(1,close-1));
        if(close+1<authority.size() && authority[close+1]==u':')port=std::u16string(authority.substr(close+2));}
    else {const auto colon=authority.rfind(u':');host=std::u16string(authority.substr(0,colon));if(colon!=authority.npos)port=std::u16string(authority.substr(colon+1));}
    if(host.empty())return result;
    result.host=utf16_to_utf8(host);result.port=port.empty()?(scheme=="https"?"443":scheme=="ftp"?"21":"80"):utf16_to_utf8(port);
    result.valid=std::all_of(result.port.begin(),result.port.end(),[](char c){return c>='0' && c<='9';}) && !result.port.empty();
    return result;
}
#if !defined(_WIN32)
// 回傳診斷文字；逾時上限 3 秒，不保留任何連線。
std::u16string diagnose(const Target& target){
    std::u16string report=u"目標主機："+utf8_to_utf16_lossy(target.host)+u"，連接埠 "+utf8_to_utf16_lossy(target.port)+u"\n\n";
    addrinfo hints{};hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;addrinfo* list=nullptr;
    const auto status=::getaddrinfo(target.host.c_str(),target.port.c_str(),&hints,&list);
    if(status || !list){report+=u"名稱解析：失敗（"+utf8_to_utf16_lossy(::gai_strerror(status))+u"）\n\n結論：無法解析主機名稱，請確認網址與 DNS 設定。";return report;}
    unsigned addresses=0;for(auto* item=list;item;item=item->ai_next)++addresses;
    report+=u"名稱解析：成功（"+utf8_to_utf16_lossy(std::to_string(addresses))+u" 個位址）\n";
    bool connected=false;int last_error=0;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    for(auto* item=list;item && !connected && std::chrono::steady_clock::now()<deadline;item=item->ai_next){
        const int fd=::socket(item->ai_family,item->ai_socktype,item->ai_protocol);if(fd<0){last_error=errno;continue;}
        (void)::fcntl(fd,F_SETFL,::fcntl(fd,F_GETFL)|O_NONBLOCK);(void)::fcntl(fd,F_SETFD,FD_CLOEXEC);
        if(::connect(fd,item->ai_addr,item->ai_addrlen)==0)connected=true;
        else if(errno==EINPROGRESS){
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
            pollfd waiting{fd,POLLOUT,0};
            if(remaining>0 && ::poll(&waiting,1,static_cast<int>(remaining))>0){int error=0;socklen_t size=sizeof error;(void)::getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size);if(!error)connected=true;else last_error=error;}
            else last_error=ETIMEDOUT;
        }else last_error=errno;
        ::close(fd);
    }
    ::freeaddrinfo(list);
    if(connected)report+=u"TCP 連線：成功\n\n結論：目前可以連到這個網站；若頁面仍無法載入，問題可能在網站本身或代理設定。";
    else report+=u"TCP 連線：失敗（"+utf8_to_utf16_lossy(last_error==ETIMEDOUT?std::string{"逾時"}:std::string(std::strerror(last_error)))+u"）\n\n結論：主機名稱可解析但無法建立連線，請檢查防火牆、代理或網路連線。";
    return report;
}
#endif
}
std::span<const WindowsServiceDescriptor> network_diagnostic_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ndfapi.dll","NdfCreateWebIncident",2},{"ndfapi.dll","NdfExecuteDiagnosis",2},{"ndfapi.dll","NdfCloseIncident",1}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_network_diagnostics(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    try{
        if(name=="NdfCreateWebIncident"){
            if(!arg(0) || !arg(1))return finish(invalid);memory.check(GuestAddress{arg(1)},unit,Access::Write);memory.store(GuestAddress{arg(1)},bits,0);
            const auto url=read_utf16(memory,GuestAddress{arg(0)},2084);if(url.empty())return finish(invalid);
            if(network_incidents_.size()>=1024)return finish(no_memory);
            const auto handle=runtime_.allocate_wait_handle();network_incidents_.emplace(handle,url);memory.store(GuestAddress{arg(1)},bits,handle);return finish(0);
        }
        const auto found=network_incidents_.find(arg(0));if(found==network_incidents_.end())return finish(invalid);
        if(name=="NdfCloseIncident"){network_incidents_.erase(found);return finish(0);}
        // NdfExecuteDiagnosis：在呼叫端視窗上顯示診斷結果，使用者關閉後回傳。
        const auto target=parse_target(found->second);std::u16string report;
        if(!target.valid)report=u"無法從網址取得主機名稱：\n"+found->second;
        else if(!runtime_.network.external_io)report=u"這個沙箱未允許外部網路存取，無法執行連線診斷。";
        else {
#if !defined(_WIN32)
            report=diagnose(target);
#else
            report=u"此平台未提供網路診斷後端。";
#endif
        }
        const auto owner=gui_.windows.contains(arg(1))?arg(1):0;
        const auto text=utf16_bytes(report),caption=utf16_bytes(u"Windows 網路診斷");
        const auto scratch=allocate_heap(memory,text.size()+caption.size());if(!scratch)return finish(no_memory);
        memory.write(GuestAddress{scratch},text);memory.write(GuestAddress{scratch+text.size()},caption);
        start_callbacks(state,memory,{{resolve("user32.dll","MessageBoxW").value,{owner,scratch,scratch+text.size(),0x40U}}},Continuation::ReturnZeroFreeHeap,scratch);
        return std::nullopt;
    }catch(const MemoryFault&){return finish(invalid);}catch(const std::bad_alloc&){return finish(no_memory);}
}
}
