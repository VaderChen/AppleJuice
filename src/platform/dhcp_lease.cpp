#include "x64arm/dhcp_lease.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#if defined(__APPLE__)
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCDynamicStoreCopyDHCPInfo.h>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace x64arm::dhcp {
namespace {
#if defined(__APPLE__)
struct Owned {
    CFTypeRef value{};
    explicit Owned(CFTypeRef v):value(v){}
    ~Owned(){if(value)CFRelease(value);}
    Owned(const Owned&)=delete;
    Owned& operator=(const Owned&)=delete;
};
std::string text(CFTypeRef value){
    if(!value || CFGetTypeID(value)!=CFStringGetTypeID())return {};
    const auto s=static_cast<CFStringRef>(value);
    const auto size=CFStringGetMaximumSizeForEncoding(CFStringGetLength(s),kCFStringEncodingUTF8)+1;
    if(size<1 || size>65536)return {};
    std::string result(static_cast<std::size_t>(size),0);
    if(!CFStringGetCString(s,result.data(),size,kCFStringEncodingUTF8))return {};
    result.resize(std::strlen(result.c_str()));return result;
}
CFTypeRef field(CFTypeRef value,CFStringRef key){return value && CFGetTypeID(value)==CFDictionaryGetTypeID()?CFDictionaryGetValue(static_cast<CFDictionaryRef>(value),key):nullptr;}
CFStringRef string(std::string_view name){const std::string terminated(name);return CFStringCreateWithCString(nullptr,terminated.c_str(),kCFStringEncodingUTF8);}
CFTypeRef copy(SCDynamicStoreRef store,const std::string& key){Owned name(string(key));return name.value?SCDynamicStoreCopyValue(store,static_cast<CFStringRef>(name.value)):nullptr;}
std::uint32_t error(){switch(SCError()){case kSCStatusAccessError:case kSCStatusLocked:case kSCStatusNeedLock:return 5;case kSCStatusInvalidArgument:return 87;case kSCStatusNoKey:return 1168;case kSCStatusKeyExists:return 1237;case kSCStatusNoStoreSession:case kSCStatusNoStoreServer:return 127;default:return 31;}}
void addresses(std::vector<std::array<std::uint8_t,4>>& out,CFTypeRef value){
    if(!value || CFGetTypeID(value)!=CFArrayGetTypeID())return;
    const auto a=static_cast<CFArrayRef>(value);
    for(CFIndex i=0;i<std::min<CFIndex>(256,CFArrayGetCount(a));++i){const auto s=text(CFArrayGetValueAtIndex(a,i));std::array<std::uint8_t,4> ip{};if(::inet_pton(AF_INET,s.c_str(),ip.data())==1)out.push_back(ip);}
}
double epoch(CFDateRef value){if(!value)return 0;const auto seconds=CFDateGetAbsoluteTime(value)+kCFAbsoluteTimeIntervalSince1970;return std::isfinite(seconds) && seconds>0?seconds:0;}
std::uint32_t refresh_service(const Service& service){
    // bootp 的逐服務 renew 僅作用於指定 IPv4 服務，保留 DHCPv6。
    if(::geteuid()!=0)return 5;
    int pipe[2];if(::pipe(pipe))return errno==EMFILE || errno==ENFILE?8U:31U;
    struct Close {int a,b;~Close(){::close(a);::close(b);}} close{pipe[0],pipe[1]};
    (void)::fcntl(pipe[0],F_SETFD,FD_CLOEXEC);(void)::fcntl(pipe[1],F_SETFD,FD_CLOEXEC);
    (void)::fcntl(pipe[0],F_SETFL,O_NONBLOCK);
    posix_spawn_file_actions_t actions;if(::posix_spawn_file_actions_init(&actions))return 8;
    struct Free {posix_spawn_file_actions_t* p;~Free(){::posix_spawn_file_actions_destroy(p);}} cleanup{&actions};
    if(::posix_spawn_file_actions_adddup2(&actions,pipe[1],STDOUT_FILENO) || ::posix_spawn_file_actions_adddup2(&actions,pipe[1],STDERR_FILENO) ||
       ::posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0) || ::posix_spawn_file_actions_addclose(&actions,pipe[0]))return 8;
    std::string path="/usr/sbin/ipconfig",command="refreshService",id=service.id,name=service.name;
    std::array<char*,5> args{path.data(),command.data(),id.data(),name.data(),nullptr};pid_t pid=-1;
    const auto spawned=::posix_spawn(&pid,path.c_str(),&actions,nullptr,args.data(),environ);
    if(spawned)return spawned==ENOENT?127U:spawned==EACCES?5U:31U;
    std::string output;int status{};const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;){
        std::array<char,1024> bytes{};for(;;){const auto n=::read(pipe[0],bytes.data(),bytes.size());if(n<0 && errno==EINTR)continue;if(n<=0)break;if(output.size()<16384)output.append(bytes.data(),std::min<std::size_t>(static_cast<std::size_t>(n),16384-output.size()));}
        const auto done=::waitpid(pid,&status,WNOHANG);
        if(done==pid)break;
        if(done<0 && errno!=EINTR){(void)::kill(pid,SIGKILL);while(::waitpid(pid,&status,0)<0 && errno==EINTR){}return 31;}
        if(std::chrono::steady_clock::now()>=deadline){(void)::kill(pid,SIGKILL);while(::waitpid(pid,&status,0)<0 && errno==EINTR){}return 1460;}
        pollfd watched{pipe[0],POLLIN,0};(void)::poll(&watched,1,10);
    }
    if(WIFEXITED(status) && !WEXITSTATUS(status))return 0;
    if(output.find("permission")!=output.npos || output.find("not authorized")!=output.npos)return 5;
    if(output.find("no such service")!=output.npos)return 1168;
    if(output.find("does not exist")!=output.npos || output.find("invalid parameter")!=output.npos)return 87;
    if(output.find("usage:")!=output.npos || output.find("unknown command")!=output.npos)return 127;
    return 31;
}
class Native final:public Platform {
    Owned store_owner_{SCDynamicStoreCreate(nullptr,CFSTR("MacWin DHCP 租約"),nullptr,nullptr)};
    SCDynamicStoreRef store()const{return static_cast<SCDynamicStoreRef>(const_cast<void*>(store_owner_.value));}
    Service service(std::string id,std::string name,CFTypeRef settings,bool configured,CFTypeRef preferences=nullptr){
        Service s;s.id=std::move(id);s.name=std::move(name);s.configured=configured;
        const auto method=field(settings,kSCPropNetIPv4ConfigMethod);s.dhcp=method && CFEqual(method,kSCValNetIPv4ConfigMethodDHCP);
        if(settings){CFRetain(settings);s.settings={settings,[](const void* p){CFRelease(p);}};}
        if(preferences){CFRetain(preferences);s.preferences={preferences,[](const void* p){CFRelease(p);}};}
        Owned state(copy(store(),"State:/Network/Service/"+s.id+"/IPv4"));addresses(s.addresses,field(state.value,kSCPropNetIPv4Addresses));
        Owned key(string(s.id));Owned lease(key.value?SCDynamicStoreCopyDHCPInfo(store(),static_cast<CFStringRef>(key.value)):nullptr);
        if(lease.value && CFGetTypeID(lease.value)==CFDictionaryGetTypeID()){
            const auto dict=static_cast<CFDictionaryRef>(lease.value);s.obtained=epoch(DHCPInfoGetLeaseStartTime(dict));s.expires=epoch(DHCPInfoGetLeaseExpirationTime(dict));
        }
        return s;
    }
public:
    Snapshot snapshot(std::string_view name) override {
        Snapshot result;const std::string interface(name);result.interface_exists=::if_nametoindex(interface.c_str())!=0;
        if(!result.interface_exists)return result;if(!store()){result.error=127;return result;}
        ifaddrs* list=nullptr;if(::getifaddrs(&list)){result.error=31;return result;}
        struct Free {ifaddrs* p;~Free(){::freeifaddrs(p);}} cleanup{list};
        for(auto* p=list;p;p=p->ifa_next)if(p->ifa_name && p->ifa_addr && interface==p->ifa_name && p->ifa_addr->sa_family==AF_INET){std::array<std::uint8_t,4> ip{};std::memcpy(ip.data(),&reinterpret_cast<const sockaddr_in*>(p->ifa_addr)->sin_addr,4);result.addresses.push_back(ip);}
        // 讀取目前 location 的原設定，讓另一次客體啟動也能續租已釋放的介面。
        Owned prefs(SCPreferencesCreate(nullptr,CFSTR("MacWin DHCP 原設定"),nullptr));
        if(!prefs.value){result.error=error();return result;}
        Owned active(SCNetworkSetCopyCurrent(static_cast<SCPreferencesRef>(const_cast<void*>(prefs.value))));
        Owned services(active.value?SCNetworkSetCopyServices(static_cast<SCNetworkSetRef>(active.value)):nullptr);
        if(services.value){const auto array=static_cast<CFArrayRef>(services.value);
            for(CFIndex i=0;i<CFArrayGetCount(array);++i){const auto entry=static_cast<SCNetworkServiceRef>(CFArrayGetValueAtIndex(array,i));if(!SCNetworkServiceGetEnabled(entry))continue;
                const auto link=SCNetworkServiceGetInterface(entry);if(!link || text(SCNetworkInterfaceGetBSDName(link))!=interface)continue;
                Owned protocol(SCNetworkServiceCopyProtocol(entry,kSCNetworkProtocolTypeIPv4));if(!protocol.value || !SCNetworkProtocolGetEnabled(static_cast<SCNetworkProtocolRef>(protocol.value)))continue;
                const auto id=text(SCNetworkServiceGetServiceID(entry));Owned settings(copy(store(),"Setup:/Network/Service/"+id+"/IPv4"));
                const auto original=SCNetworkProtocolGetConfiguration(static_cast<SCNetworkProtocolRef>(protocol.value));
                result.services.push_back(service(id,interface,settings.value?settings.value:original,settings.value!=nullptr,original));
            }
        }
        // 補上不在偏好設定內、由其他客戶端建立的 runtime 服務。
        Owned keys(SCDynamicStoreCopyKeyList(store(),CFSTR("Setup:/Network/Service/[^/]+/IPv4")));
        if(!keys.value){result.error=error();return result;}
        const auto array=static_cast<CFArrayRef>(keys.value);
        for(CFIndex i=0;i<CFArrayGetCount(array);++i){const auto key=text(CFArrayGetValueAtIndex(array,i));const auto prefix=key.substr(0,key.size()-5),id=prefix.substr(prefix.rfind('/')+1);
            if(std::any_of(result.services.begin(),result.services.end(),[&](const auto& s){return s.id==id;}))continue;
            Owned link(copy(store(),prefix+"/Interface")),state(copy(store(),"State:"+prefix.substr(6)+"/IPv4"));
            auto device=text(field(link.value,kSCPropNetInterfaceDeviceName));if(device.empty())device=text(field(state.value,kSCPropInterfaceName));if(device!=interface)continue;
            Owned settings(copy(store(),key));result.services.push_back(service(id,interface,settings.value,true));
        }
        return result;
    }
    std::uint32_t remove(const std::vector<Service>& services) override {
        if(::geteuid()!=0)return 5;if(!store())return 127;
        Owned keys(CFArrayCreateMutable(nullptr,0,&kCFTypeArrayCallBacks));if(!keys.value)return 8;
        for(const auto& s:services){if(!s.dhcp || !s.settings)return 87;Owned key(string("Setup:/Network/Service/"+s.id+"/IPv4"));if(!key.value)return 8;
            Owned current(SCDynamicStoreCopyValue(store(),static_cast<CFStringRef>(key.value)));if(!current.value || !CFEqual(current.value,s.settings.get()))return 1237;
            CFArrayAppendValue(static_cast<CFMutableArrayRef>(const_cast<void*>(keys.value)),key.value);
        }
        return SCDynamicStoreSetMultiple(store(),nullptr,static_cast<CFArrayRef>(keys.value),nullptr)?0:error();
    }
    std::uint32_t restore(const Service& s) override {
        if(::geteuid()!=0)return 5;if(!store())return 127;if(!s.dhcp || !s.settings)return 87;
        if(s.preferences){
            const auto current=snapshot(s.name);if(current.error)return current.error;
            const auto item=std::find_if(current.services.begin(),current.services.end(),[&](const auto& p){return p.id==s.id;});
            if(item==current.services.end() || !item->dhcp || !item->preferences || !CFEqual(item->preferences.get(),s.preferences.get()))return 1237;
        }
        Owned key(string("Setup:/Network/Service/"+s.id+"/IPv4"));if(!key.value)return 8;
        Owned current(SCDynamicStoreCopyValue(store(),static_cast<CFStringRef>(key.value)));
        if(current.value)return CFEqual(current.value,s.settings.get())?0U:1237U;
        return SCDynamicStoreAddValue(store(),static_cast<CFStringRef>(key.value),s.settings.get())?0:error();
    }
    std::uint32_t refresh(const Service& s) override {return s.dhcp && s.configured?refresh_service(s):87;}
    void wait() override {std::this_thread::sleep_for(std::chrono::milliseconds(50));}
};
#else
class Native final:public Platform {
public:
    Snapshot snapshot(std::string_view) override {Snapshot s;s.error=50;return s;}
    std::uint32_t remove(const std::vector<Service>&) override {return 50;}
    std::uint32_t restore(const Service&) override {return 50;}
    std::uint32_t refresh(const Service&) override {return 50;}
    void wait() override {}
};
#endif
}
std::shared_ptr<Platform> native_platform(){return std::make_shared<Native>();}
}
