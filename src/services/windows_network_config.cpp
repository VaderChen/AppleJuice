#include "x64arm/windows_services.hpp"
#include "x64arm/startup_profile.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/security_descriptor.hpp"
#include "iphelper_legacy.hpp"
#include "ipv4_prefix.h"
#include "http_proxy_configuration.hpp"
#include "x64arm/dhcp_lease.hpp"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <resolv.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <net/if_dl.h>
#include <net/route.h>
#include <sys/sysctl.h>
#include <notify.h>
#else
#include <netpacket/packet.h>
#endif
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <cstdio>
#include <cctype>
#include <chrono>

namespace x64arm {
namespace {
std::uint64_t interface_luid(const NetworkInterface& item){return (std::uint64_t{item.type}<<48)|(std::uint64_t{item.index}<<24);}
constexpr unsigned unicast_row_size=80;
std::array<std::uint8_t,unicast_row_size> unicast_row(const NetworkInterface& item,const InterfaceAddress& address){
    std::array<std::uint8_t,unicast_row_size> bytes{};
    const auto put=[&](unsigned at,std::uint64_t value,unsigned width=4){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
    std::copy(address.bytes.begin(),address.bytes.end(),bytes.begin());
    put(32,interface_luid(item),8);put(40,item.index);
    // 未有可靠來源的 origin／建立時間保持 Other／0；租約沿用共用 DHCP 快照。
    const bool dhcp=address.bytes[0]==2 && item.dhcp_enabled && item.lease_expires>0;
    const auto lifetime=dhcp?static_cast<std::uint32_t>(std::clamp<std::int64_t>(item.lease_expires-std::time(nullptr),0,UINT32_MAX)):UINT32_MAX;
    put(44,dhcp?3:0);put(48,dhcp?3:0);put(52,lifetime);put(56,lifetime);
    bytes[60]=address.prefix;put(64,item.status==1?4:0);
    if(address.bytes[0]==23)std::copy_n(address.bytes.begin()+24,4,bytes.begin()+68);
    return bytes;
}
std::array<std::uint8_t,16> interface_guid(const NetworkInterface& item){
    // 客體介面身分由宿主名稱穩定衍生，並非冒充硬體廠商的 GUID。
    std::array<std::uint8_t,16> value{};std::uint64_t hash=14695981039346656037ULL;
    for(unsigned half=0;half<2;++half){for(const auto c:std::string("x64arm-interface-")+std::to_string(half)+item.name){hash^=static_cast<unsigned char>(c);hash*=1099511628211ULL;}for(unsigned i=0;i<8;++i)value[half*8+i]=static_cast<std::uint8_t>(hash>>(i*8));}
    value[7]=(value[7]&15)|0x80;value[8]=(value[8]&63)|0x80;return value;
}
std::vector<std::uint8_t> interface_row(const NetworkInterface& item){
    std::vector<std::uint8_t> bytes(1352);const auto put=[&](unsigned at,std::uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    put(0,interface_luid(item),8);put(8,item.index,4);const auto guid=interface_guid(item);std::copy(guid.begin(),guid.end(),bytes.begin()+12);
    const auto name=utf16_bytes(utf8_to_utf16(item.name).substr(0,256));std::copy(name.begin(),name.end(),bytes.begin()+28);
    const auto description=utf16_bytes(utf8_to_utf16(item.description.empty()?item.name:item.description).substr(0,256));std::copy(description.begin(),description.end(),bytes.begin()+542);
    put(1056,item.physical.size(),4);std::copy(item.physical.begin(),item.physical.end(),bytes.begin()+1060);
    // 無永久 MAC 的可靠查詢時保留未知，不以目前的隨機 MAC 偽造。
    put(1124,item.mtu,4);put(1128,item.type,4);put(1132,item.type==131?1:0,4);put(1136,item.type==24?17:item.type==131?15:0,4);
    put(1144,item.type==24?1:item.type==131?3:2,4);put(1148,0,4);
    bytes[1152]=static_cast<std::uint8_t>((item.hardware?5:0)|(item.media_state==2?16:0));
    put(1156,item.status,4);put(1160,item.admin_status,4);put(1164,item.media_state,4);put(1184,1,4);
    put(1192,item.speed,8);put(1200,item.speed,8);for(unsigned i=0;i<item.statistics.size();++i)put(1208+i*8,item.statistics[i],8);return bytes;
}
std::vector<std::uint8_t> socket_bytes(const sockaddr* value) {
    if (!value) return {};
    std::vector<std::uint8_t> result;
    if (value->sa_family==AF_INET) {result.resize(16);result[0]=2;std::memcpy(result.data()+4,&reinterpret_cast<const sockaddr_in*>(value)->sin_addr,4);}
    else if (value->sa_family==AF_INET6) {
        result.resize(28);result[0]=23;const auto* address=reinterpret_cast<const sockaddr_in6*>(value);
        std::memcpy(result.data()+8,&address->sin6_addr,16);std::memcpy(result.data()+24,&address->sin6_scope_id,4);
    }
    return result;
}
std::optional<std::vector<std::uint8_t>> ipv4_snapshot(){
    ifaddrs* list=nullptr;if(::getifaddrs(&list))return {};
    struct Free {ifaddrs* p;~Free(){::freeifaddrs(p);}} cleanup{list};
    std::set<std::vector<std::uint8_t>> entries;
    for(auto* p=list;p;p=p->ifa_next){
        if(!p->ifa_name || !p->ifa_addr || p->ifa_addr->sa_family!=AF_INET)continue;
        std::vector<std::uint8_t> value(p->ifa_name,p->ifa_name+std::strlen(p->ifa_name));value.push_back(0);
        for(const auto* at:{p->ifa_addr,p->ifa_netmask}){auto bytes=socket_bytes(at);value.insert(value.end(),bytes.begin(),bytes.end());}
        entries.insert(std::move(value));
    }
    std::vector<std::uint8_t> result;for(const auto& e:entries){for(unsigned i=0;i<4;++i)result.push_back(static_cast<std::uint8_t>(e.size()>>(i*8)));result.insert(result.end(),e.begin(),e.end());}return result;
}
// 宿主網路設定變更：configd 於介面、位址、路由或 DNS 變更時發出此通知；
// notify_check 只讀共享記憶體不進核心，可在每次查詢時核對。無法登錄時每次查詢都視為已變更。
std::uint64_t host_network_generation() {
#if defined(__APPLE__)
    static int token=NOTIFY_TOKEN_INVALID;
    static const bool registered=::notify_register_check("com.apple.system.config.network_change",&token)==NOTIFY_STATUS_OK;
    static std::uint64_t generation=0;
    int changed=1;
    if(!registered || ::notify_check(token,&changed)!=NOTIFY_STATUS_OK || changed)++generation;
    return generation;
#else
    static std::uint64_t generation=0;return ++generation;
#endif
}
// 介面統計、速率與最後變更時間：快照重用期間仍按需重讀，計數器不會停留在擷取當下。
void refresh_interface_statistics(std::vector<NetworkInterface>& interfaces) {
#if defined(__APPLE__)
    // NET_RT_IFLIST2 提供 if_data64；非 platform binary 的 byte 計數仍受
    // 核心 ALIGN_BYTES 的 1 KiB／32-bit 限制，忠實保留宿主快照。
    int mib[]={CTL_NET,PF_ROUTE,0,0,NET_RT_IFLIST2,0};std::size_t required=0;
    timeval boot{};std::size_t boot_size=sizeof(boot);
    const bool has_boot=::sysctlbyname("kern.boottime",&boot,&boot_size,nullptr,0)==0;
    if(::sysctl(mib,6,nullptr,&required,nullptr,0)==0 && required<=16*1024*1024){
        std::vector<std::uint8_t> data(required);if(::sysctl(mib,6,data.data(),&required,nullptr,0)==0){
            for(std::size_t at=0;at+4<=required;){std::uint16_t length=0;std::memcpy(&length,data.data()+at,2);if(length<4 || length>required-at)break;
                if(data[at+3]==RTM_IFINFO2 && length>=sizeof(if_msghdr2)){if_msghdr2 info{};std::memcpy(&info,data.data()+at,sizeof(info));
                    const auto found=std::find_if(interfaces.begin(),interfaces.end(),[&](const auto& p){return p.index==info.ifm_index;});
                    if(found!=interfaces.end()){auto& item=*found;const auto& d=info.ifm_data;item.statistics_available=true;item.speed=d.ifi_baudrate;auto& s=item.statistics;
                        if(d.ifi_type)item.type=d.ifi_type;
                        item.hardware=item.type==6 || item.type==71;
                        if(has_boot && d.ifi_lastchange.tv_sec>=boot.tv_sec){
                            // XNU 只在秒數加上 boottime_sec()；微秒仍是開機後的時間。
                            const auto elapsed=(static_cast<std::int64_t>(d.ifi_lastchange.tv_sec)-boot.tv_sec)*100+
                                static_cast<std::int64_t>(d.ifi_lastchange.tv_usec)/10000;
                            item.last_change=static_cast<std::uint32_t>(std::max<std::int64_t>(0,elapsed));
                        }
                        s[0]=d.ifi_ibytes;s[1]=d.ifi_ipackets>=d.ifi_imcasts?d.ifi_ipackets-d.ifi_imcasts:0;s[2]=d.ifi_imcasts;s[3]=d.ifi_iqdrops;s[4]=d.ifi_ierrors;s[5]=d.ifi_noproto;
                        s[9]=d.ifi_obytes;s[10]=d.ifi_opackets>=d.ifi_omcasts?d.ifi_opackets-d.ifi_omcasts:0;s[11]=d.ifi_omcasts;s[12]=static_cast<std::uint64_t>(std::max(0,info.ifm_snd_drops));s[13]=d.ifi_oerrors;s[17]=static_cast<std::uint64_t>(std::max(0,info.ifm_snd_len));
                    }
                }at+=length;
            }
        }
    }
#endif
}
bool capture(NetworkConfiguration& result) {
    startup_profile::Scope phase("network_startup","capture",true);
    const auto generation=host_network_generation();
    ifaddrs* list=nullptr;if (::getifaddrs(&list)!=0) return false;
    struct FreeList {ifaddrs* value;~FreeList(){::freeifaddrs(value);}} cleanup{list};
    std::map<std::string,NetworkInterface> interfaces;
    const auto control=::socket(AF_INET,SOCK_DGRAM,0);
    struct CloseSocket {int value;~CloseSocket(){if(value>=0)::close(value);}} socket{control};
    for (auto* entry=list;entry;entry=entry->ifa_next) {
        if (!entry->ifa_name || !entry->ifa_addr) continue;
        const auto [position,inserted]=interfaces.try_emplace(entry->ifa_name);
        auto& item=position->second;
        if(inserted){
            item.name=entry->ifa_name;item.index=::if_nametoindex(entry->ifa_name);
            ifreq request{};std::strncpy(request.ifr_name,entry->ifa_name,sizeof(request.ifr_name)-1);
            if(control>=0 && ::ioctl(control,SIOCGIFMTU,&request)==0 && request.ifr_mtu>0)item.mtu=static_cast<std::uint32_t>(request.ifr_mtu);
        }
        item.type=entry->ifa_flags&IFF_LOOPBACK?24U:entry->ifa_flags&IFF_POINTOPOINT?131U:6U;
        item.admin_status=entry->ifa_flags&IFF_UP?1U:2U;item.media_state=entry->ifa_flags&IFF_RUNNING?1U:2U;
        item.status=item.admin_status==1 && item.media_state==1?1U:2U;item.multicast=(entry->ifa_flags&IFF_MULTICAST)!=0;item.hardware=item.type==6;

        auto bytes=socket_bytes(entry->ifa_addr);
        if (!bytes.empty()) {
            const auto mask=socket_bytes(entry->ifa_netmask);unsigned prefix=0;
            if (!mask.empty()) {
                const auto begin=bytes[0]==2?4U:8U,end=bytes[0]==2?8U:24U;bool ended=false;
                for (unsigned i=begin;i<end && !ended;++i) for (int bit=7;bit>=0;--bit) {if (!(mask[i]&(1U<<bit))) {ended=true;break;}++prefix;}
            }
            item.addresses.push_back({std::move(bytes),static_cast<std::uint8_t>(prefix)});
        }
#if defined(__APPLE__)
        if (entry->ifa_addr->sa_family==AF_LINK) {
            const auto* link=reinterpret_cast<const sockaddr_dl*>(entry->ifa_addr);const auto* address=reinterpret_cast<const std::uint8_t*>(LLADDR(link));
            item.physical.assign(address,address+std::min<unsigned>(8,link->sdl_alen));
        }
#else
        if (entry->ifa_addr->sa_family==AF_PACKET) {
            const auto* link=reinterpret_cast<const sockaddr_ll*>(entry->ifa_addr);item.physical.assign(link->sll_addr,link->sll_addr+std::min<unsigned>(8,link->sll_halen));
        }
#endif
    }
    NetworkConfiguration snapshot;
    for (auto& [name,item]:interfaces) {(void)name;if(item.index)snapshot.interfaces.push_back(std::move(item));}
    refresh_interface_statistics(snapshot.interfaces);
    bool dns_captured=false;
    snapshot.metadata_available=iphelper::capture_metadata(snapshot,&dns_captured);
    // SystemConfiguration 的 DNS 陣列原本會取代 resolver 結果（含空陣列）。
    // 只有沒有有效陣列時才讀 resolver，保留缺少／失敗時的原回退行為。
    if(!dns_captured){
    {startup_profile::Scope resolver_phase("network_startup","resolver",true);
    struct __res_state resolver{};
    if (::res_ninit(&resolver)==0) {
#if defined(__APPLE__)
        std::array<res_sockaddr_union,16> servers{};const auto count=::res_getservers(&resolver,servers.data(),static_cast<int>(servers.size()));
        for (int i=0;i<std::min<int>(count,static_cast<int>(servers.size()));++i) {auto bytes=socket_bytes(reinterpret_cast<const sockaddr*>(&servers[static_cast<std::size_t>(i)]));if(!bytes.empty())snapshot.dns.push_back(std::move(bytes));}
        ::res_ndestroy(&resolver);
#else
        for (int i=0;i<resolver.nscount;++i) {auto bytes=socket_bytes(reinterpret_cast<const sockaddr*>(&resolver.nsaddr_list[i]));if(!bytes.empty())snapshot.dns.push_back(std::move(bytes));}
        ::res_nclose(&resolver);
#endif
    }}
    }
    snapshot.captured=true;snapshot.host_generation=generation;result=std::move(snapshot);return true;
}
// 快照重用：宿主設定未變更時沿用先前擷取的介面、DNS 與 DHCP metadata，只在需要時重讀統計；
// 比較模式（external_io 關閉）維持只擷取一次，不讀宿主變更。
bool refresh_configuration(NetworkConfiguration& config,bool external_io,bool statistics,bool descriptions=false) {
    static const bool cached=[] {const auto* setting=std::getenv("X64ARM_NETWORK_SNAPSHOT_CACHE");return !setting || std::string_view(setting)!="0";}();
    const auto ensure_descriptions=[&]{if(descriptions && !config.descriptions_loaded){iphelper::capture_interface_descriptions(config);config.descriptions_loaded=true;}return true;};
    if(!cached)return ((external_io || !config.captured)?capture(config):true) && ensure_descriptions();
    if(!config.captured || (external_io && config.host_generation!=host_network_generation())){if(!capture(config))return false;}
    else if(statistics && external_io)refresh_interface_statistics(config.interfaces);
    return ensure_descriptions();
}
NetworkRoute query_route(const NetworkConfiguration& config,std::uint64_t luid,std::uint32_t index,
                         std::span<const std::uint8_t> source,std::span<const std::uint8_t> destination) {
    NetworkRoute result;
    const auto put=[&](std::size_t at,std::uint64_t value,unsigned count) {for(unsigned i=0;i<count;++i)result.row.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));};
    if(luid) {
        const auto item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& item){return ((std::uint64_t{item.type}<<48)|(std::uint64_t{item.index}<<24))==luid;});
        if(item==config.interfaces.end()){result.status=2;return result;}index=item->index;
    } else if(index && std::none_of(config.interfaces.begin(),config.interfaces.end(),[&](const auto& item){return item.index==index;})) {result.status=2;return result;}
    const auto family=destination[0];
    if((family!=2 && family!=23) || destination[1] || (!source.empty() && (source[0]!=family || source[1]))) {result.status=87;return result;}
#if defined(__APPLE__)
    const auto native_address=[&](std::span<const std::uint8_t> bytes) {
        sockaddr_storage value{};
        if(family==2) {auto& ip=reinterpret_cast<sockaddr_in&>(value);ip.sin_len=sizeof(ip);ip.sin_family=AF_INET;std::memcpy(&ip.sin_addr,bytes.data()+4,4);}
        else {auto& ip=reinterpret_cast<sockaddr_in6&>(value);ip.sin6_len=sizeof(ip);ip.sin6_family=AF_INET6;std::memcpy(&ip.sin6_addr,bytes.data()+8,16);std::memcpy(&ip.sin6_scope_id,bytes.data()+24,4);}
        return value;
    };
    const auto close=[](int* fd){if(*fd>=0)::close(*fd);delete fd;};
    const std::unique_ptr<int,decltype(close)> routing(new int(::socket(PF_ROUTE,SOCK_RAW,0)),close);
    if(*routing<0){result.status=50;return result;}
    timeval timeout{0,250000};::setsockopt(*routing,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    rt_msghdr header{};header.rtm_type=RTM_GET;header.rtm_version=RTM_VERSION;header.rtm_seq=1;header.rtm_pid=::getpid();
    header.rtm_flags=RTF_UP|(index?RTF_IFSCOPE:0);header.rtm_index=static_cast<unsigned short>(index);header.rtm_addrs=RTA_DST|RTA_IFP;
    std::array<std::uint8_t,2048> bytes{};std::size_t used=sizeof(header);
    const auto append=[&](const void* value,std::size_t size){std::memcpy(bytes.data()+used,value,size);used+=(size+3)&~std::size_t{3};};
    auto target=native_address(destination);append(&target,reinterpret_cast<sockaddr*>(&target)->sa_len);
    sockaddr_dl link{};link.sdl_len=sizeof(link);link.sdl_family=AF_LINK;append(&link,sizeof(link));
    header.rtm_msglen=static_cast<unsigned short>(used);std::memcpy(bytes.data(),&header,sizeof(header));
    if(::write(*routing,bytes.data(),used)!=static_cast<ssize_t>(used)){result.status=errno==ESRCH?1168U:1231U;return result;}
    bool received=false;
    for(unsigned attempt=0;attempt<16;++attempt) {
        const auto count=::read(*routing,bytes.data(),bytes.size());
        if(count<0 && errno==EINTR)continue;
        if(count<static_cast<ssize_t>(sizeof(header)))break;
        std::memcpy(&header,bytes.data(),sizeof(header));
        if(header.rtm_seq!=1 || header.rtm_pid!=::getpid() || header.rtm_type!=RTM_GET)continue;
        if(header.rtm_errno || header.rtm_msglen>count || header.rtm_msglen<sizeof(header))break;
        received=true;break;
    }
    if(!received || (header.rtm_flags&(RTF_REJECT|RTF_BLACKHOLE))){result.status=1231;return result;}
    std::array<sockaddr_storage,RTAX_MAX> addresses{};std::size_t at=sizeof(header);
    for(unsigned i=0;i<RTAX_MAX;++i)if(static_cast<unsigned>(header.rtm_addrs)&(1U<<i)) {
        if(at+2>header.rtm_msglen){result.status=13;return result;}
        const auto size=bytes[at];const auto padded=size?(size+3U)&~3U:4U;
        if(at+padded>header.rtm_msglen || size>sizeof(sockaddr_storage)){result.status=13;return result;}
        std::memcpy(&addresses[i],bytes.data()+at,size);at+=padded;
    }
    const auto interface=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& item){return item.index==header.rtm_index;});
    if(interface==config.interfaces.end() || (index && index!=interface->index)){result.status=2;return result;}
    // UDP connect 僅查詢核心選出的本機來源位址，不傳送客體封包。
    const int af=family==2?AF_INET:AF_INET6;
    const std::unique_ptr<int,decltype(close)> probe(new int(::socket(af,SOCK_DGRAM,0)),close);
    if(*probe<0){result.status=50;return result;}
    const auto chosen=interface->index;
    if(::setsockopt(*probe,family==2?IPPROTO_IP:IPPROTO_IPV6,family==2?IP_BOUND_IF:IPV6_BOUND_IF,&chosen,sizeof(chosen))!=0){result.status=1231;return result;}
    if(!source.empty()) {const auto address=native_address(source);if(::bind(*probe,reinterpret_cast<const sockaddr*>(&address),reinterpret_cast<const sockaddr*>(&address)->sa_len)!=0){result.status=87;return result;}}
    if(family==2)reinterpret_cast<sockaddr_in&>(target).sin_port=htons(9);else reinterpret_cast<sockaddr_in6&>(target).sin6_port=htons(9);
    if(::connect(*probe,reinterpret_cast<const sockaddr*>(&target),reinterpret_cast<const sockaddr*>(&target)->sa_len)!=0){result.status=1231;return result;}
    sockaddr_storage selected{};socklen_t size=sizeof(selected);
    if(::getsockname(*probe,reinterpret_cast<sockaddr*>(&selected),&size)!=0){result.status=1231;return result;}
    auto selected_bytes=socket_bytes(reinterpret_cast<sockaddr*>(&selected));std::copy(selected_bytes.begin(),selected_bytes.end(),result.source.begin());
    const auto prefix_address=socket_bytes(reinterpret_cast<const sockaddr*>(&addresses[RTAX_DST]));
    if(prefix_address.empty()){result.status=13;return result;}
    std::copy(prefix_address.begin(),prefix_address.end(),result.row.begin()+12);
    const auto begin=family==2?4U:8U,total=family==2?32U:128U;
    unsigned prefix=header.rtm_flags&RTF_HOST?total:0;
    if(!(header.rtm_flags&RTF_HOST)) {
        const auto* mask=reinterpret_cast<const std::uint8_t*>(&addresses[RTAX_NETMASK]);bool ended=false;
        for(unsigned bit=0;bit<total;++bit) {
            const bool set=(mask[begin+bit/8]&(0x80U>>(bit%8)))!=0;
            if(!set)ended=true;else if(ended){result.status=50;return result;}else ++prefix;
        }
    }
    for(unsigned bit=prefix;bit<total;++bit)result.row[12+begin+bit/8]&=static_cast<std::uint8_t>(~(0x80U>>(bit%8)));
    result.row[40]=static_cast<std::uint8_t>(prefix);result.row[44]=family;
    if(header.rtm_flags&RTF_GATEWAY) {
        const auto gateway=socket_bytes(reinterpret_cast<const sockaddr*>(&addresses[RTAX_GATEWAY]));
        if(!gateway.empty())std::copy(gateway.begin(),gateway.end(),result.row.begin()+44);
    }
    put(0,(std::uint64_t{interface->type}<<48)|(std::uint64_t{interface->index}<<24),8);put(8,interface->index,4);
    result.row[72]=static_cast<std::uint8_t>(prefix);
    const auto expiry=header.rtm_rmx.rmx_expire;
    const auto lifetime=expiry?static_cast<std::uint32_t>(std::max<std::int64_t>(0,static_cast<std::int64_t>(expiry)-std::time(nullptr))):UINT32_MAX;
    put(76,lifetime,4);put(80,lifetime,4);put(84,header.rtm_rmx.rmx_hopcount,4);
    put(88,header.rtm_flags&RTF_GATEWAY?header.rtm_flags&RTF_STATIC?3U:1U:2U,4);
    result.row[92]=interface->type==24;result.row[95]=!expiry;put(100,header.rtm_flags&RTF_GATEWAY?0U:1U,4);
#else
    (void)put;result.status=50;
#endif
    return result;
}
}
bool snapshot_network_configuration(NetworkConfiguration& result){return capture(result);}
bool WindowsServices::address_notification_waiting() const {
    return std::any_of(address_notifications_.begin(),address_notifications_.end(),[](const auto& item){return item.second.pending;});
}
void WindowsServices::poll_address_notifications(GuestMemory& memory){
    if(!address_notification_waiting())return;
    const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if(now<address_poll_time_)return;address_poll_time_=now+100;
    const auto snapshot=ipv4_snapshot();if(!snapshot)return;
    for(auto& [handle,watch]:address_notifications_){
        (void)handle;if(!watch.pending || watch.snapshot==*snapshot)continue;watch.pending=false;
        memory.store(GuestAddress{watch.overlapped},x86_?32:64,0);
        if(const auto e=runtime_.wait_objects.find(watch.event);e!=runtime_.wait_objects.end())e->second.set_signal(true);
        runtime_.network.configuration->captured=false;runtime_.network.configuration->routes.clear();
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_network_config(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);std::uint64_t result=0;
    if(service.symbol=="ConvertLengthToIpv4Mask" || service.symbol=="ConvertIpv4MaskToLength"){
        try {
            if(!arg(1))result=87;
            else if(service.symbol=="ConvertLengthToIpv4Mask"){
                const auto length=static_cast<std::uint32_t>(arg(0));
                memory.store(GuestAddress{arg(1)},32,x64arm_ipv4_mask(length));
                if(length>32)result=87;
            }else {
                const auto length=x64arm_ipv4_length(static_cast<std::uint32_t>(arg(0)));
                if(length==UINT32_MAX)result=87;else memory.store(GuestAddress{arg(1)},8,length);
            }
        }catch(const MemoryFault&){result=87;}
        arg.return_to(state,result);return std::nullopt;
    }
    if(service.symbol=="GetExtendedTcpTable" || service.symbol=="GetTcpTable"){arg.return_to(state,tcp_table_call(memory,arg,service.symbol));return std::nullopt;}
    if(service.family=="dhcpcsvc.dll"){
        struct Error {std::uint32_t code;};const auto require=[](bool ok,std::uint32_t code=87){if(!ok)throw Error{code};};
        try {
            if(service.symbol=="DhcpCApiInitialize"){require(arg(0));memory.check(GuestAddress{arg(0)},4,Access::Write);require(runtime_.dhcp_startups<UINT32_MAX,8);++runtime_.dhcp_startups;memory.store(GuestAddress{arg(0)},32,2);}
            else if(service.symbol=="DhcpCApiCleanup"){if(runtime_.dhcp_startups)--runtime_.dhcp_startups;}
            else {
                require(runtime_.dhcp_startups!=0,5023);require(static_cast<std::uint32_t>(arg(0))==2 && !arg(1) && !arg(8),static_cast<std::uint32_t>(arg(0))&1?50:87);require(arg(2) && arg(7));
                const auto name=utf16_to_utf8(read_utf16(memory,GuestAddress{arg(2)},256));require(!name.empty() && name.size()<256);const unsigned unit=x86_?4:8,bits=x86_?32:64;
                struct Param {std::uint64_t record;std::uint8_t option;bool vendor;};std::vector<Param> received;std::map<std::uint8_t,std::vector<std::uint8_t>> send;std::vector<std::uint8_t> vendor_send,class_id,requested;
                const auto array=[&](unsigned index,bool output){const auto value=arg(index);const auto count=x86_?static_cast<std::uint32_t>(value):memory.load(GuestAddress{value},32);const auto pointer=x86_?value>>32:memory.load(GuestAddress{value+8},64);require(count<=255 && (!count || pointer));const unsigned stride=x86_?20:32;
                    for(std::uint32_t i=0;i<count;++i){const auto at=pointer+i*stride;const auto flags=memory.load(GuestAddress{at},32),option=memory.load(GuestAddress{at+4},32),vendor=memory.load(GuestAddress{at+8},32);const auto data=memory.load(GuestAddress{at+(x86_?12:16)},static_cast<std::uint8_t>(bits)),size=memory.load(GuestAddress{at+(x86_?16:24)},32);require(!flags && option>0 && option<255 && vendor<=1);
                        if(output){require(!data && !size);memory.check(GuestAddress{at+(x86_?12:16)},x86_?8:16,Access::Write);received.push_back({at,static_cast<std::uint8_t>(option),vendor!=0});requested.push_back(vendor?43:static_cast<std::uint8_t>(option));}
                        else {require(size<=255 && (!size || data));std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.read(GuestAddress{data},bytes);if(vendor){require(vendor_send.size()+2+bytes.size()<=255);vendor_send.push_back(static_cast<std::uint8_t>(option));vendor_send.push_back(static_cast<std::uint8_t>(size));vendor_send.insert(vendor_send.end(),bytes.begin(),bytes.end());}else{require(option!=53 && option!=54 && option!=55 && option!=57 && option!=61);require(!send.contains(static_cast<std::uint8_t>(option)));send[static_cast<std::uint8_t>(option)]=std::move(bytes);}}
                    }};array(4,false);array(5,true);if(!vendor_send.empty()){require(!send.contains(43));send[43]=vendor_send;}
                if(arg(3)){const auto flags=memory.load(GuestAddress{arg(3)},32),size=memory.load(GuestAddress{arg(3)+unit*2},32),pointer=memory.load(GuestAddress{arg(3)+unit},static_cast<std::uint8_t>(bits));require(!flags && size<=255 && (!size || pointer));class_id.resize(static_cast<std::size_t>(size));memory.read(GuestAddress{pointer},class_id);require(!send.contains(60));}
                const auto capacity=memory.load(GuestAddress{arg(7)},32);memory.check(GuestAddress{arg(7)},4,Access::Write);if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生 DHCP 查詢");auto& config=*runtime_.network.configuration;require(capture(config),232);require(config.metadata_available,50);
                const auto adapter=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& item){if(item.name==name)return true;const auto guid=interface_guid(item);char text[39];std::snprintf(text,sizeof(text),"{%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x}",guid[3],guid[2],guid[1],guid[0],guid[5],guid[4],guid[7],guid[6],guid[8],guid[9],guid[10],guid[11],guid[12],guid[13],guid[14],guid[15]);std::string normalized=name;std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});return normalized==text;});require(adapter!=config.interfaces.end(),1168);require(adapter->dhcp_enabled,232);auto options=adapter->dhcp_options;
                const auto vendor_options=[](const auto& options){std::map<std::uint8_t,std::vector<std::uint8_t>> values;const auto found=options.find(43);if(found==options.end())return values;const auto& bytes=found->second;for(std::size_t at=0;at<bytes.size();){const auto code=bytes[at++];if(!code)continue;if(code==255)break;if(at>=bytes.size() || bytes[at]>bytes.size()-at-1)throw Error{13};const auto size=bytes[at++];auto& value=values[code];value.insert(value.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+size));at+=size;}return values;};auto vendor=vendor_options(options);
                if(!send.empty() || !class_id.empty() || std::any_of(received.begin(),received.end(),[&](const auto& item){return !(item.vendor?vendor:options).contains(item.option);})){std::sort(requested.begin(),requested.end());requested.erase(std::unique(requested.begin(),requested.end()),requested.end());const auto status=iphelper::dhcp_inform(*adapter,requested,send,class_id,options);require(status==0,status);vendor=vendor_options(options);}
                std::size_t needed=0;for(const auto& item:received){const auto& values=item.vendor?vendor:options;const auto found=values.find(item.option);if(found!=values.end())needed+=found->second.size();}require(needed<=output_limit,8);memory.store(GuestAddress{arg(7)},32,needed);require(needed<=capacity && (!needed || arg(6)),234);if(needed)memory.check(GuestAddress{arg(6)},needed,Access::Write);std::size_t offset=0;
                for(const auto& item:received){const auto& values=item.vendor?vendor:options;const auto found=values.find(item.option);const auto size=found==values.end()?0:found->second.size();memory.store(GuestAddress{item.record+(x86_?12:16)},static_cast<std::uint8_t>(bits),size?arg(6)+offset:0);memory.store(GuestAddress{item.record+(x86_?16:24)},32,size);if(size){memory.write(GuestAddress{arg(6)+offset},found->second);offset+=size;}}
            }
        }catch(const Error& error){result=error.code;}catch(const MemoryFault&){result=87;}catch(const std::bad_alloc&){result=8;}
        arg.return_to(state,result);return std::nullopt;
    }
    if(service.symbol=="WinHttpGetDefaultProxyConfiguration" || service.symbol=="WinHttpSetDefaultProxyConfiguration"){
        const bool setting=service.symbol=="WinHttpSetDefaultProxyConfiguration";
        const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
        using http_proxy::Error;
        try {
            if(!arg(0))throw Error{87};memory.check(GuestAddress{arg(0)},unit*3,setting?Access::Read:Access::Write);
            http_proxy::Configuration config;bool named=false;
            if(setting){
                const auto access=memory.load(GuestAddress{arg(0)},32);if(access!=1 && access!=3)throw Error{87};named=access==3;
                if(named)for(unsigned i=0;i<2;++i){const auto p=memory.load(GuestAddress{arg(0)+unit*(i+1)},bits);if(!p && !i)throw Error{87};
                    const auto value=p?read_utf16(memory,GuestAddress{p},65536):std::u16string{};auto& text=i?config.bypass:config.proxy;
                    for(auto c:value){if(c>127)throw Error{87};text.push_back(static_cast<char>(c));}}
            }
            if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
            auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_,!setting);
            if(transaction.error)throw Error{transaction.error};
            if(setting){
                auto before=registry;bool accepted=false;
                struct Rollback {RegistryState& target;RegistryState& before;bool& accepted;~Rollback(){if(!accepted)target=std::move(before);}} rollback{registry,before,accepted};
                std::vector<std::u16string> created;const auto path=http_proxy::key(x86_);
                for(std::size_t cursor=0;;){
                    const auto slash=path.find(u'\\',cursor+1),end=slash==path.npos?path.size():slash;const auto part=path.substr(0,end);
                    if(!registry.keys.contains(part)){
                        if(registry.keys.size()>=4096)throw Error{8};const auto parent=part.substr(0,part.find_last_of(u'\\'));
                        if(part.size()>1){(void)registry_security_access(parent,4);registry.security_descriptors[part]=registry_creation_descriptor(registry_security_descriptor(parent),{});}
                        registry.keys.try_emplace(part);registry.display_names.try_emplace(part,part.substr(part.find_last_of(u'\\')+1));
                        registry.modified_times[part]=runtime_.system_filetime();registry.modified_times[parent]=runtime_.system_filetime();created.push_back(parent);
                    }
                    if(end==path.size())break;cursor=end;
                }
                (void)registry_security_access(path,2);auto data=http_proxy::encode(config,named);auto& values=registry.keys.at(path);
                const auto old=values.find(std::u16string(http_proxy::value_name));const auto previous=old==values.end()?0:old->second.bytes.size();
                if(registry.bytes-previous+data.size()>output_limit || (old==values.end() && registry.values>=4096))throw Error{8};
                registry.bytes=registry.bytes-previous+data.size();if(old==values.end())++registry.values;
                values[std::u16string(http_proxy::value_name)]={3,std::move(data),u"WinHttpSettings"};registry.modified_times[path]=runtime_.system_filetime();
                if(const auto error=transaction.commit())throw Error{error};accepted=true;
                for(const auto& parent:created)poll_registry_notifications(false,parent,1);poll_registry_notifications(false,path,4);
            }else{
                const auto path=http_proxy::key(x86_);if(registry.keys.contains(path))(void)registry_security_access(path,1);
                config=http_proxy::read(registry,x86_);std::array<std::uint64_t,2> pointers{};
                try {
                    for(unsigned i=0;i<2;++i){const auto& text=i?config.bypass:config.proxy;if(text.empty())continue;std::u16string wide(text.begin(),text.end());const auto bytes=utf16_bytes(wide);
                        const auto p=allocate_heap(memory,bytes.size());if(!p)throw Error{8};pointers[i]=p;memory.write(GuestAddress{p},bytes);gui_.global_memory.emplace(p,GuiGlobalMemory{p,bytes.size(),0,0,false});}
                    memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(unit*3));memory.store(GuestAddress{arg(0)},32,config.proxy.empty()?1:3);
                    for(unsigned i=0;i<2;++i)memory.store(GuestAddress{arg(0)+unit*(i+1)},bits,pointers[i]);
                }catch(...){for(auto p:pointers)if(p){gui_.global_memory.erase(p);(void)free_heap(memory,p,state);}throw;}
            }
            result=1;
        }catch(const Error& error){last_error_=error.code;}catch(const SecurityDescriptorError& error){last_error_=error.code;}catch(const MemoryFault&){last_error_=998;}catch(const std::bad_alloc&){last_error_=8;}
        arg.return_to(state,result);return std::nullopt;
    }
    if(service.symbol=="WinHttpGetIEProxyConfigForCurrentUser"){
        if(!arg(0))last_error_=87;
        else {
            const auto size=x86_?16U:32U,stride=x86_?4U:8U;
            memory.check(GuestAddress{arg(0)},size,Access::Write);
            if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
            if(transaction.error)last_error_=transaction.error;
            else {
                const auto key=runtime_.registry.keys.find(std::u16string(1,u'\1')+u"\\software\\microsoft\\windows\\currentversion\\internet settings");
                const auto value=[&](const char16_t* name)->const RegistryValue*{if(key==runtime_.registry.keys.end())return nullptr;const auto entry=key->second.find(name);return entry==key->second.end()?nullptr:&entry->second;};
                const auto enabled=[&](const char16_t* name){const auto v=value(name);return v && v->type==4 && v->bytes.size()==4 && std::any_of(v->bytes.begin(),v->bytes.end(),[](auto byte){return byte!=0;});};
                std::array<std::uint64_t,3> pointers{};std::vector<std::uint64_t> allocated;allocated.reserve(3);bool valid=true;
                try {
                    const std::array names{u"autoconfigurl",u"proxyserver",u"proxyoverride"};
                    for(unsigned i=0;i<3;++i){if(i && !enabled(u"proxyenable"))continue;const auto v=value(names[i]);if(!v)continue;
                        if(v->type!=1 || v->bytes.size()%2){last_error_=13;valid=false;break;}
                        if(v->bytes.empty() || (v->bytes.size()>=2 && !v->bytes[0] && !v->bytes[1]))continue;
                        auto data=v->bytes;if(data.size()<2 || data[data.size()-1] || data[data.size()-2]){data.push_back(0);data.push_back(0);}
                        const auto pointer=allocate_heap(memory,data.size());if(!pointer){last_error_=8;valid=false;break;}allocated.push_back(pointer);memory.write(GuestAddress{pointer},data);gui_.global_memory.emplace(pointer,GuiGlobalMemory{pointer,data.size(),0,0,false});pointers[i]=pointer;
                    }
                    if(valid){memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(size));memory.store(GuestAddress{arg(0)},32,enabled(u"autodetect"));for(unsigned i=0;i<3;++i)memory.store(GuestAddress{arg(0)+stride*(i+1)},x86_?32:64,pointers[i]);result=1;}
                }catch(...){for(const auto p:allocated){gui_.global_memory.erase(p);(void)free_heap(memory,p,state);}throw;}
                if(!valid)for(const auto p:allocated){gui_.global_memory.erase(p);(void)free_heap(memory,p,state);}
            }
        }
        arg.return_to(state,result);return std::nullopt;
    }
    if(service.symbol=="GetOverlappedResult" || service.symbol=="GetOverlappedResultEx")return invoke_overlapped_result(state,memory,service);
    if(service.symbol=="NotifyAddrChange" || service.symbol=="CancelIPChangeNotify"){
        const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto unit=x86_?4U:8U;
        if(!runtime_.network.external_io)throw HostCallFault("compare 不支援原生網路變更監聽");
        try {
            if(service.symbol=="NotifyAddrChange"){
                if(!arg(0) && !arg(1))result=50; // 同步 API 尚未提供；非同步通知使用共用客體排程器。
                else if(!arg(0) || !arg(1))result=87;
                else {
                    memory.check(GuestAddress{arg(0)},unit,Access::Write);memory.check(GuestAddress{arg(1)},x86_?20:32,Access::Write);
                    auto old=std::find_if(address_notifications_.begin(),address_notifications_.end(),[&](const auto& p){return p.second.overlapped==arg(1);});
                    const auto event=memory.load(GuestAddress{arg(1)+(x86_?16:24)},bits)&~UINT64_C(1);
                    const auto e=runtime_.wait_objects.find(event);
                    if(old!=address_notifications_.end() && old->second.pending)result=87;
                    else if(event && (e==runtime_.wait_objects.end() || e->second.kind!=WaitObject::Kind::Event))result=87;
                    else if(address_notifications_.size()>=1024 && old==address_notifications_.end())result=8;
                    else if(const auto snapshot=ipv4_snapshot()){
                        const auto handle=old==address_notifications_.end()?next_address_notification_++:old->first;
                        address_notifications_[handle]={arg(1),event,*snapshot,true};
                        memory.store(GuestAddress{arg(1)},bits,0x103);memory.store(GuestAddress{arg(1)+unit},bits,0);memory.store(GuestAddress{arg(0)},bits,handle);
                        if(e!=runtime_.wait_objects.end())e->second.set_signal(false);result=997;
                    }else result=232;
                }
                last_error_=static_cast<std::uint32_t>(result);
            }else if(service.symbol=="CancelIPChangeNotify"){
                const auto found=std::find_if(address_notifications_.begin(),address_notifications_.end(),[&](const auto& p){return p.second.overlapped==arg(0) && p.second.pending;});
                if(found==address_notifications_.end())last_error_=1168;
                else {auto& watch=found->second;memory.store(GuestAddress{watch.overlapped},bits,0xc0000120U);watch.pending=false;
                    if(const auto e=runtime_.wait_objects.find(watch.event);e!=runtime_.wait_objects.end())e->second.set_signal(true);result=1;}
            }
        }catch(const MemoryFault&){if(service.symbol=="NotifyAddrChange")result=87;else result=0;last_error_=87;}
        arg.return_to(state,result);return std::nullopt;
    }
    const auto symbol=service.symbol;
    if(symbol=="IpReleaseAddress" || symbol=="IpRenewAddress"){
        if(!runtime_.network.external_io)throw HostCallFault("compare 不執行 DHCP 租約修改");
        result=87;
        try{
            if(arg(0)){
                memory.check(GuestAddress{arg(0)},260,Access::Read);
                std::u16string name;bool terminated=false;
                for(unsigned i=0;i<128;++i){const auto ch=static_cast<char16_t>(memory.load(GuestAddress{arg(0)+4+2*i},16));if(!ch){terminated=true;break;}name.push_back(ch);}
                if(terminated && !name.empty()){
                    if(!dhcp_backend_)dhcp_backend_=dhcp::native_backend();
                    // 即使宿主已部分修改後遇到配置失敗，也不可沿用舊路由快照。
                    runtime_.network.configuration->captured=false;runtime_.network.configuration->routes.clear();
                    const auto changed=dhcp_backend_->change(utf16_to_utf8(name),symbol=="IpRenewAddress");result=changed.error;
                }
            }
        }catch(const MemoryFault&){result=87;}catch(const std::bad_alloc&){result=8;}
        arg.return_to(state,result);return std::nullopt;
    }
    if(symbol=="GetAdaptersInfo" || symbol=="GetNetworkParams" || symbol=="GetIfEntry" || symbol=="GetIfTable"){
        try{
            auto& config=*runtime_.network.configuration;
            // GetAdaptersInfo／GetIfEntry／GetIfTable 的輸出含介面描述字串；GetNetworkParams 不含。
            if(!refresh_configuration(config,runtime_.network.external_io,symbol=="GetIfEntry" || symbol=="GetIfTable",symbol!="GetNetworkParams"))result=232;
            else if(symbol=="GetIfEntry"){
                if(!arg(0))result=87;
                else {
                    const auto index=memory.load(GuestAddress{arg(0)+512},32);
                    const auto item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& value){return value.index==index;});
                    if(!index)result=87;else if(item==config.interfaces.end())result=2;
                    else if(!item->statistics_available)result=50;
                    else memory.write(GuestAddress{arg(0)},iphelper::interface_row(*item));
                }
            }else if(!arg(1))result=87;
            else {
                const auto capacity=memory.load(GuestAddress{arg(1)},32);
                memory.check(GuestAddress{arg(1)},4,Access::Write);
                if(symbol!="GetIfTable" && !config.metadata_available)result=232;
                else if(symbol=="GetIfTable" && std::any_of(config.interfaces.begin(),config.interfaces.end(),[](const auto& item){return !item.statistics_available;}))result=50;
                else{
                    auto blob=symbol=="GetAdaptersInfo"?iphelper::adapters(config,x86_):symbol=="GetNetworkParams"?iphelper::fixed_info(config,x86_):iphelper::interface_table(config,arg(2)!=0);
                    if(blob.bytes.empty()){memory.store(GuestAddress{arg(1)},32,0);result=232;}
                    else if(!arg(0) || capacity<blob.bytes.size()){memory.store(GuestAddress{arg(1)},32,blob.bytes.size());result=symbol=="GetIfTable"?122:111;}
                    else {
                        memory.check(GuestAddress{arg(0)},blob.bytes.size(),Access::Write);
                        blob.relocate(arg(0),x86_?4:8);memory.write(GuestAddress{arg(0)},blob.bytes);
                        memory.store(GuestAddress{arg(1)},32,blob.bytes.size());
                    }
                }
            }
        }catch(const MemoryFault&){result=87;}catch(const std::invalid_argument&){result=87;}
        catch(const std::length_error&){result=8;}catch(const std::bad_alloc&){result=8;}
        arg.return_to(state,result);return std::nullopt;
    }
    if(symbol=="FreeMibTable"){if(arg(0) && runtime_.network.addresses.erase(arg(0)))(void)free_heap(memory,arg(0),state);arg.return_to(state,0);return std::nullopt;}
    if(symbol=="InitializeUnicastIpAddressEntry"){
        std::array<std::uint8_t,unicast_row_size> row{};
        row[44]=row[48]=16;row[60]=0xff;
        std::fill(row.begin()+52,row.begin()+60,0xff);
        memory.write(GuestAddress{arg(0)},row);arg.return_to(state,0);return std::nullopt;
    }
    if(symbol=="if_indextoname" || symbol=="if_nametoindex"){
        std::uint64_t value=0;
        try{
            auto& config=*runtime_.network.configuration;
            if(arg(0) && (symbol=="if_nametoindex" || arg(1)) && refresh_configuration(config,runtime_.network.external_io,false)){
                const auto name=symbol=="if_nametoindex"?read_ansi(memory,arg(0),256):std::string{};
                const auto item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& p){return symbol=="if_nametoindex"?p.name==name:p.index==static_cast<std::uint32_t>(arg(0));});
                if(item!=config.interfaces.end()){
                    if(symbol=="if_nametoindex")value=item->index;
                    else if(item->name.size()<256){std::vector<std::uint8_t> bytes(item->name.begin(),item->name.end());bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);value=arg(1);}
                }
            }
        }catch(const MemoryFault&){}catch(const std::invalid_argument&){}catch(const std::bad_alloc&){}
        arg.return_to(state,value);return std::nullopt;
    }
    if(symbol=="GetUnicastIpAddressTable" || symbol=="GetUnicastIpAddressEntry"){
        try{
            const bool table=symbol=="GetUnicastIpAddressTable";const auto output=arg(table?1:0);
            if(!output){arg.return_to(state,87);return std::nullopt;}
            memory.check(GuestAddress{output},table?(x86_?4:8):unicast_row_size,Access::Write);
            const auto family=table?static_cast<std::uint16_t>(arg(0)):static_cast<std::uint16_t>(memory.load(GuestAddress{output},16));
            if((family!=2 && family!=23 && (!table || family!=0))){arg.return_to(state,87);return std::nullopt;}
            if(table)memory.store(GuestAddress{output},x86_?32:64,0);
            std::uint64_t luid=0;std::uint32_t index=0;std::array<std::uint8_t,28> wanted{};
            if(!table){
                luid=memory.load(GuestAddress{output+32},64);index=static_cast<std::uint32_t>(memory.load(GuestAddress{output+40},32));memory.read(GuestAddress{output},wanted);
                const auto offset=family==2?4U:8U,count=family==2?4U:16U;
                const bool unspecified=std::all_of(wanted.begin()+offset,wanted.begin()+offset+count,[](auto byte){return byte==0;});
                if((!luid && !index) || unspecified || (family==2?wanted[4]>=224:wanted[8]==255)){arg.return_to(state,87);return std::nullopt;}
            }
            auto& config=*runtime_.network.configuration;
            if(!refresh_configuration(config,runtime_.network.external_io,false))result=232;
            else{
                std::vector<std::uint8_t> rows;bool interface_found=table;
                for(const auto& item:config.interfaces){
                    if(!table && (luid?interface_luid(item)!=luid:item.index!=index))continue;
                    interface_found=true;
                    for(const auto& address:item.addresses){
                        if(address.bytes.size()!=16 && address.bytes.size()!=28)continue;
                        if(family && address.bytes[0]!=family)continue;
                        const auto offset=family==2?4U:8U,count=family==2?4U:16U;
                        if(!table && !std::equal(wanted.begin()+offset,wanted.begin()+offset+count,address.bytes.begin()+offset))continue;
                        const auto row=unicast_row(item,address);rows.insert(rows.end(),row.begin(),row.end());if(!table)break;
                    }
                    if(!table && !rows.empty())break;
                }
                if(!interface_found)result=2;
                else if(rows.empty())result=1168;
                else if(!table)memory.write(GuestAddress{output},rows);
                else{
                    const auto base=allocate_heap(memory,8+rows.size());if(!base)result=8;
                    else{memory.store(GuestAddress{base},64,rows.size()/unicast_row_size);memory.write(GuestAddress{base+8},rows);runtime_.network.addresses.insert(base);memory.store(GuestAddress{output},x86_?32:64,base);}
                }
            }
        }catch(const MemoryFault&){result=87;}catch(const std::invalid_argument&){result=87;}catch(const std::bad_alloc&){result=8;}
        arg.return_to(state,result);return std::nullopt;
    }
    if(symbol.starts_with("ConvertInterface") || symbol=="GetIfTable2" || symbol=="GetIfTable2Ex" || symbol=="GetIfEntry2" || symbol=="GetInterfaceInfo"){
        try{
            auto& config=*runtime_.network.configuration;
            // 介面表與單筆查詢回傳流量計數，重用快照時仍重讀統計；LUID／名稱轉換只用穩定的識別資料。
            const bool statistics=symbol=="GetIfTable2" || symbol=="GetIfTable2Ex" || symbol=="GetIfEntry2";
            if(!refresh_configuration(config,runtime_.network.external_io,statistics))result=232;
            else if(symbol.starts_with("ConvertInterface")){
                if(!arg(0) || !arg(1))result=87;
                else{
                    auto item=config.interfaces.end();const bool to_luid=symbol.ends_with("ToLuid") || symbol.ends_with("ToLuidA") || symbol.ends_with("ToLuidW");
                    if(symbol=="ConvertInterfaceIndexToLuid")item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& p){return p.index==static_cast<std::uint32_t>(arg(0));});
                    else if(symbol=="ConvertInterfaceGuidToLuid"){std::array<std::uint8_t,16> guid{};memory.read(GuestAddress{arg(0)},guid);item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& p){return interface_guid(p)==guid;});}
                    else if(to_luid){const auto name=symbol.ends_with('A')?read_ansi(memory,arg(0),257):utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)},257));item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& p){return p.name==name;});}
                    else {const auto luid=memory.load(GuestAddress{arg(0)},64);item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& p){return interface_luid(p)==luid;});}
                    if(item==config.interfaces.end())result=87;
                    else if(to_luid)memory.store(GuestAddress{arg(1)},64,interface_luid(*item));
                    else if(symbol=="ConvertInterfaceLuidToIndex")memory.store(GuestAddress{arg(1)},32,item->index);
                    else if(symbol=="ConvertInterfaceLuidToGuid")memory.write(GuestAddress{arg(1)},interface_guid(*item));
                    else {const bool ansi=symbol.ends_with('A');const auto wide_name=utf16_bytes(utf8_to_utf16(item->name));std::vector<std::uint8_t> text=ansi?std::vector<std::uint8_t>(item->name.begin(),item->name.end()):wide_name;if(ansi)text.push_back(0);
                        if(arg(2)<text.size()/(ansi?1:2))result=8;else memory.write(GuestAddress{arg(1)},text);}
                }
            }else if(symbol=="GetIfEntry2"){
                if(!arg(0))result=87;else {memory.check(GuestAddress{arg(0)},1352,Access::Write);const auto luid=memory.load(GuestAddress{arg(0)},64),index=memory.load(GuestAddress{arg(0)+8},32);
                    const auto item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& p){return luid?interface_luid(p)==luid:p.index==index;});
                    if(!luid && !index)result=87;else if(item==config.interfaces.end())result=2;else if(!item->statistics_available)result=50;else memory.write(GuestAddress{arg(0)},interface_row(*item));}
            }else if(symbol=="GetInterfaceInfo"){
                if(!arg(1))result=87;else {std::vector<const NetworkInterface*> items;for(const auto& item:config.interfaces)if(item.type!=24 && std::any_of(item.addresses.begin(),item.addresses.end(),[](const auto& a){return a.bytes[0]==2;}))items.push_back(&item);
                    const auto capacity=memory.load(GuestAddress{arg(1)},32);const auto size=4+std::max<std::size_t>(1,items.size())*260;memory.store(GuestAddress{arg(1)},32,size);
                    if(items.empty())result=232;else if(!arg(0) || capacity<size)result=122;
                    else {memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(size));memory.store(GuestAddress{arg(0)},32,items.size());for(std::size_t i=0;i<items.size();++i){const auto at=arg(0)+4+i*260;memory.store(GuestAddress{at},32,items[i]->index);memory.write(GuestAddress{at+4},utf16_bytes(utf8_to_utf16(items[i]->name).substr(0,127)));}}
                }
            }else {
                const bool ex=symbol=="GetIfTable2Ex";const auto output=arg(ex?1:0);
                if(!output || (ex && static_cast<std::uint32_t>(arg(0))>1))result=87;
                else if(std::any_of(config.interfaces.begin(),config.interfaces.end(),[](const auto& item){return !item.statistics_available;}))result=50;
                else{memory.check(GuestAddress{output},x86_?4:8,Access::Write);memory.store(GuestAddress{output},x86_?32:64,0);
                    const auto size=8+config.interfaces.size()*1352;const auto base=allocate_heap(memory,size);if(!base)result=8;
                    else{memory.write(GuestAddress{base},std::vector<std::uint8_t>(8));memory.store(GuestAddress{base},32,config.interfaces.size());for(std::size_t i=0;i<config.interfaces.size();++i)memory.write(GuestAddress{base+8+i*1352},interface_row(config.interfaces[i]));
                        runtime_.network.addresses.insert(base);memory.store(GuestAddress{output},x86_?32:64,base);}
                }
            }
        }catch(const MemoryFault&){result=87;}catch(const std::invalid_argument&){result=87;}
        arg.return_to(state,result);return std::nullopt;
    }
    if(service.symbol=="GetBestInterface" || service.symbol=="GetBestInterfaceEx") {
        try {
            if(!arg(1)){arg.return_to(state,87);return std::nullopt;}
            memory.check(GuestAddress{arg(1)},4,Access::Write);
            std::array<std::uint8_t,28> destination{};
            if(service.symbol=="GetBestInterface"){
                destination[0]=2;const auto address=static_cast<std::uint32_t>(arg(0));
                for(unsigned i=0;i<4;++i)destination[4+i]=static_cast<std::uint8_t>(address>>(8*i));
            }else{
                if(!arg(0)){arg.return_to(state,87);return std::nullopt;}
                const auto family=memory.load(GuestAddress{arg(0)},16);
                if(family!=2 && family!=23){arg.return_to(state,87);return std::nullopt;}
                memory.read(GuestAddress{arg(0)},std::span(destination).first(family==2?16:28));
                // 選路 API 不以 socket port／IPv6 flow label 決定介面。
                std::fill(destination.begin()+2,destination.begin()+4,0);
                if(family==23)std::fill(destination.begin()+4,destination.begin()+8,0);
            }
            auto& config=*runtime_.network.configuration;
            if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生路由查詢");
            if(!refresh_configuration(config,true,false))result=232;
            else{
                const auto route=query_route(config,0,0,{},destination);result=route.status;
                if(!result){std::uint32_t index=0;for(unsigned i=0;i<4;++i)index|=std::uint32_t(route.row[8+i])<<(8*i);memory.store(GuestAddress{arg(1)},32,index);}
            }
        }catch(const MemoryFault&){result=87;}catch(const std::invalid_argument&){result=87;}catch(const std::bad_alloc&){result=8;}
        arg.return_to(state,result);return std::nullopt;
    }
    if(service.symbol=="GetBestRoute2") {
        if(!arg(3) || !arg(5) || !arg(6)) {arg.return_to(state,87);return std::nullopt;}
        const auto luid=arg(0)?memory.load(GuestAddress{arg(0)},64):0;
        const auto index=static_cast<std::uint32_t>(arg(1));
        std::vector<std::uint8_t> source(arg(2)?28:0),destination(28),key;
        if(arg(2))memory.read(GuestAddress{arg(2)},source);memory.read(GuestAddress{arg(3)},destination);
        memory.check(GuestAddress{arg(5)},104,Access::Write);memory.check(GuestAddress{arg(6)},28,Access::Write);
        auto& config=*runtime_.network.configuration;
        if(!config.captured && !capture(config))result=232;
        else {
            for(unsigned i=0;i<8;++i)key.push_back(static_cast<std::uint8_t>(luid>>(i*8)));
            for(unsigned i=0;i<4;++i)key.push_back(static_cast<std::uint8_t>(index>>(i*8)));
            key.insert(key.end(),source.begin(),source.end());key.insert(key.end(),destination.begin(),destination.end());
            auto found=config.routes.find(key);
            if(found==config.routes.end()) {
                if(config.routes.size()>=4096){arg.return_to(state,8);return std::nullopt;}
                found=config.routes.emplace(std::move(key),query_route(config,luid,index,source,destination)).first;
            }
            result=found->second.status;
            if(!result){memory.write(GuestAddress{arg(5)},found->second.row);memory.write(GuestAddress{arg(6)},found->second.source);}
        }
        arg.return_to(state,result);return std::nullopt;
    }
    const auto family=static_cast<std::uint32_t>(arg(0)),flags_value=static_cast<std::uint32_t>(arg(1));
    if ((family!=0 && family!=2 && family!=23) || arg(2) || !arg(4)) result=87;
    // 0x40 WINS、0x80 閘道；0x200 所有 compartment 與 0x400 tunnel 綁定順序在單一 compartment、
    // 依介面索引排序的宿主上與預設結果相同。
    else if (flags_value&~0x7ffU) result=50;
    else {
        auto& config=*runtime_.network.configuration;
        if (!config.captured && !capture(config)) result=232;
        else {
            std::vector<std::uint8_t> bytes;std::vector<std::size_t> pointers;
            const unsigned unit=x86_?4:8,adapter_size=x86_?376:448,physical=8+9*unit;
            const unsigned node_socket=8+unit,node_fields=8+3*unit;
            const auto allocate=[&](std::size_t size) {const auto offset=(bytes.size()+7)&~std::size_t{7};bytes.resize(offset+size);return offset;};
            const auto put=[&](std::size_t at,std::uint64_t value,unsigned count) {for(unsigned i=0;i<count;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));};
            const auto pointer=[&](std::size_t at,std::size_t offset) {put(at,offset,unit);pointers.push_back(at);};
            const auto append=[&](std::span<const std::uint8_t> content) {const auto offset=allocate(content.size());std::copy(content.begin(),content.end(),bytes.begin()+static_cast<std::ptrdiff_t>(offset));return offset;};
            std::optional<std::size_t> previous;
            for (const auto& item:config.interfaces) {
                const auto matches=[&](const auto& address){return !family || address.bytes[0]==family;};
                if (!(flags_value&0x100) && std::none_of(item.addresses.begin(),item.addresses.end(),matches)) continue;
                const auto at=allocate(adapter_size);if(previous)pointer(*previous+8,at);previous=at;
                put(at,adapter_size,4);put(at+4,item.index,4);
                std::vector<std::uint8_t> name(item.name.begin(),item.name.end());name.push_back(0);const auto name_at=append(name);pointer(at+8+unit,name_at);
                const auto wide_name=utf16_bytes(utf8_to_utf16(item.name)),empty=utf16_bytes(u"");const auto wide_at=append(wide_name),empty_at=append(empty);
                const auto description=utf16_bytes(utf8_to_utf16(item.description.empty()?item.name:item.description));const auto description_at=append(description);
                pointer(at+8+6*unit,empty_at);pointer(at+8+7*unit,description_at);if (!(flags_value&32))pointer(at+8+8*unit,wide_at);
                std::copy(item.physical.begin(),item.physical.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at+physical));put(at+physical+8,item.physical.size(),4);
                unsigned enabled=0;for(const auto& address:item.addresses) enabled|=address.bytes[0]==2?128U:256U;
                put(at+physical+12,enabled|(item.multicast?0U:16U)|(item.dhcp_enabled?4U:0U),4);put(at+physical+16,item.mtu,4);put(at+physical+20,item.type,4);put(at+physical+24,item.status,4);
                // 有 IPv6 位址才有 IPv6 介面索引；ZoneIndices 依 SCOPE_LEVEL：介面／鏈路／子網為該介面，
                // 管理至組織層級使用預設區域 1，全域層級為 0。
                const bool ipv6=std::any_of(item.addresses.begin(),item.addresses.end(),[](const auto& address){return address.bytes[0]==23;});
                put(at+physical+28,ipv6?item.index:0,4);
                if(ipv6)for(unsigned level=1;level<14;++level)put(at+physical+32+level*4,level<=3?item.index:1,4);
                put(at+(x86_?176:224),interface_luid(item),8);put(at+(x86_?192:248),1,4);
                put(at+(x86_?144:184),item.speed,8);put(at+(x86_?152:192),item.speed,8);put(at+(x86_?212:268),1,4);put(at+(x86_?216:272),item.type==131?1:0,4);
                std::optional<std::size_t> last_unicast,last_prefix,last_dns;
                for(const auto& address:item.addresses) {
                    if(!matches(address))continue;
                    if (!(flags_value&1)) {
                        const auto node=allocate(x86_?48:64);pointer(last_unicast?*last_unicast+8:at+8+2*unit,node);last_unicast=node;
                        put(node,x86_?48:64,4);put(node+4,1,4);const auto socket_at=append(address.bytes);pointer(node+node_socket,socket_at);put(node+node_socket+unit,address.bytes.size(),4);
                        put(node+node_fields+8,4,4);put(node+node_fields+12,UINT32_MAX,4);put(node+node_fields+16,UINT32_MAX,4);put(node+node_fields+20,UINT32_MAX,4);put(node+node_fields+24,address.prefix,1);
                    }
                    if (flags_value&16) {
                        const auto node=allocate(x86_?24:40);pointer(last_prefix?*last_prefix+8:at+physical+96,node);last_prefix=node;put(node,x86_?24:40,4);
                        auto network_address=address.bytes;const auto start=network_address[0]==2?4U:8U,total=network_address[0]==2?32U:128U;
                        for(unsigned bit=address.prefix;bit<total;++bit)network_address[start+bit/8]&=static_cast<std::uint8_t>(~(0x80U>>(bit%8)));
                        const auto socket_at=append(network_address);pointer(node+node_socket,socket_at);put(node+node_socket+unit,network_address.size(),4);put(node+node_fields,address.prefix,4);
                    }
                }
                if (!(flags_value&8) && item.type!=24 && item.status==1) for(const auto& address:config.dns) {
                    if(family && address[0]!=family)continue;
                    const auto node=allocate(x86_?24:32);pointer(last_dns?*last_dns+8:at+8+5*unit,node);last_dns=node;put(node,x86_?24:32,4);
                    const auto socket_at=append(address);pointer(node+node_socket,socket_at);put(node+node_socket+unit,address.size(),4);
                }
                // WINS 與閘道節點和 DNS 節點同形：Length／Reserved、Next、SOCKET_ADDRESS。
                const auto address_list=[&](std::size_t head,const std::vector<std::vector<std::uint8_t>>& values){
                    std::optional<std::size_t> last;
                    for(const auto& address:values){
                        if(family && address[0]!=family)continue;
                        const auto node=allocate(x86_?24:32);pointer(last?*last+8:head,node);last=node;put(node,x86_?24:32,4);
                        const auto socket_at=append(address);pointer(node+node_socket,socket_at);put(node+node_socket+unit,address.size(),4);
                    }
                };
                const auto ipv4_list=[](const std::vector<std::array<std::uint8_t,4>>& values){
                    std::vector<std::vector<std::uint8_t>> result;
                    for(const auto& ip:values){std::vector<std::uint8_t> bytes(16);bytes[0]=2;std::copy(ip.begin(),ip.end(),bytes.begin()+4);result.push_back(std::move(bytes));}
                    return result;
                };
                if (flags_value&0x40) address_list(at+(x86_?160:200),ipv4_list(item.wins));
                if (flags_value&0x80) {
                    auto gateways=ipv4_list(item.gateways);gateways.insert(gateways.end(),item.ipv6_gateways.begin(),item.ipv6_gateways.end());
                    address_list(at+(x86_?164:208),gateways);
                }
            }
            const auto capacity=memory.load(GuestAddress{arg(4)},32);memory.check(GuestAddress{arg(4)},4,Access::Write);
            if(bytes.empty())result=232;
            else if(!arg(3) || capacity<bytes.size()) {memory.store(GuestAddress{arg(4)},32,bytes.size());result=111;}
            else {
                for(const auto at:pointers) {std::uint64_t offset=0;for(unsigned i=0;i<unit;++i)offset|=std::uint64_t{bytes[at+i]}<<(i*8);put(at,arg(3)+offset,unit);}
                memory.write(GuestAddress{arg(3)},bytes);memory.store(GuestAddress{arg(4)},32,bytes.size());
            }
        }
    }
    arg.return_to(state,result);return std::nullopt;
}
}
