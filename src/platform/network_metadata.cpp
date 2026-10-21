#include "../services/iphelper_legacy.hpp"
#include "x64arm/startup_profile.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <limits>
#include <net/if.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCDynamicStoreCopyDHCPInfo.h>
#include <net/route.h>
#include <sys/sysctl.h>
#endif

namespace x64arm::iphelper {
namespace {
#if defined(__APPLE__)
struct Owned {
    CFTypeRef value{};
    explicit Owned(CFTypeRef v):value(v){}
    ~Owned(){if(value)CFRelease(value);}
    Owned(const Owned&)=delete;
    Owned& operator=(const Owned&)=delete;
};
std::string string(CFTypeRef value){
    if(!value || CFGetTypeID(value)!=CFStringGetTypeID())return {};
    const auto text=static_cast<CFStringRef>(value);
    const auto size=CFStringGetMaximumSizeForEncoding(CFStringGetLength(text),kCFStringEncodingUTF8)+1;
    if(size<1 || size>65536)return {};
    std::string result(static_cast<std::size_t>(size),0);
    if(!CFStringGetCString(text,result.data(),size,kCFStringEncodingUTF8))return {};
    result.resize(std::strlen(result.c_str()));return result;
}
CFTypeRef field(CFTypeRef value,CFStringRef name){
    return value && CFGetTypeID(value)==CFDictionaryGetTypeID()?CFDictionaryGetValue(static_cast<CFDictionaryRef>(value),name):nullptr;
}
CFTypeRef copy(SCDynamicStoreRef store,CFDictionaryRef snapshot,const std::string& key){
    Owned name(CFStringCreateWithCString(nullptr,key.c_str(),kCFStringEncodingUTF8));
    if(!name.value)return nullptr;
    if(!snapshot)return SCDynamicStoreCopyValue(store,static_cast<CFStringRef>(name.value));
    const auto value=CFDictionaryGetValue(snapshot,name.value);
    return value?CFRetain(value):nullptr;
}
void address(std::vector<std::array<std::uint8_t,4>>& values,std::string_view text){
    std::array<std::uint8_t,4> ip{};const std::string terminated(text);
    if(::inet_pton(AF_INET,terminated.c_str(),ip.data())==1 && std::find(values.begin(),values.end(),ip)==values.end())values.push_back(ip);
}
void addresses(std::vector<std::array<std::uint8_t,4>>& values,CFTypeRef data){
    if(!data || CFGetTypeID(data)!=CFArrayGetTypeID())return;
    const auto array=static_cast<CFArrayRef>(data);
    for(CFIndex i=0;i<std::min<CFIndex>(256,CFArrayGetCount(array));++i)address(values,string(CFArrayGetValueAtIndex(array,i)));
}
void option_addresses(std::vector<std::array<std::uint8_t,4>>& values,CFDictionaryRef info,std::uint8_t code){
    const auto data=DHCPInfoGetOptionData(info,code);if(!data)return;
    const auto size=CFDataGetLength(data);const auto* bytes=CFDataGetBytePtr(data);
    for(CFIndex i=0;i+4<=size && i<1024;i+=4){std::array<std::uint8_t,4> value{};std::copy_n(bytes+i,4,value.begin());if(std::find(values.begin(),values.end(),value)==values.end())values.push_back(value);}
}
std::int64_t epoch(CFDateRef value){
    if(!value)return 0;
    const auto time=CFDateGetAbsoluteTime(value)+kCFAbsoluteTimeIntervalSince1970;
    return time>=0 && time<static_cast<double>(INT64_MAX)?static_cast<std::int64_t>(time):0;
}
bool gateways(NetworkConfiguration& config){
    int mib[]={CTL_NET,PF_ROUTE,0,AF_INET,NET_RT_DUMP,0};std::size_t size=0;
    if(::sysctl(mib,6,nullptr,&size,nullptr,0) || size>16U*1024U*1024U)return false;
    if(!size)return true;
    std::vector<std::uint8_t> bytes(size);
    if(::sysctl(mib,6,bytes.data(),&size,nullptr,0))return false;
    for(std::size_t at=0;at+sizeof(rt_msghdr)<=size;){
        rt_msghdr header{};std::memcpy(&header,bytes.data()+at,sizeof(header));
        if(header.rtm_msglen<sizeof(header) || header.rtm_msglen>size-at)return false;
        if(header.rtm_flags&RTF_PROXY)config.proxy_enabled=true;
        const auto end=at+header.rtm_msglen;std::size_t cursor=at+sizeof(header);
        std::array<std::array<std::uint8_t,4>,RTAX_MAX> ips{};std::array<bool,RTAX_MAX> ipv4{};
        for(unsigned slot=0;slot<RTAX_MAX;++slot)if(static_cast<unsigned>(header.rtm_addrs)&(1U<<slot)){
            if(cursor+2>end)return false;const auto length=bytes[cursor];const auto stride=length?(length+3U)&~3U:4U;
            if(stride>end-cursor)return false;
            if(bytes[cursor+1]==AF_INET && length>=8){std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(cursor+4),4,ips[slot].begin());ipv4[slot]=true;}
            cursor+=stride;
        }
        if((header.rtm_flags&(RTF_UP|RTF_GATEWAY))==(RTF_UP|RTF_GATEWAY) && ipv4[RTAX_DST] &&
           ips[RTAX_DST]==std::array<std::uint8_t,4>{} && ipv4[RTAX_GATEWAY]){
            const auto item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& value){return value.index==header.rtm_index;});
            if(item!=config.interfaces.end() && std::find(item->gateways.begin(),item->gateways.end(),ips[RTAX_GATEWAY])==item->gateways.end())item->gateways.push_back(ips[RTAX_GATEWAY]);
        }
        at=end;
    }
    return true;
}
// IPv6 預設路由（::/0）經由閘道者即為該介面的預設閘道；包含 macOS 依介面限定的預設路由。
bool ipv6_gateways(NetworkConfiguration& config){
    int mib[]={CTL_NET,PF_ROUTE,0,AF_INET6,NET_RT_DUMP,0};std::size_t size=0;
    if(::sysctl(mib,6,nullptr,&size,nullptr,0) || size>16U*1024U*1024U)return false;
    if(!size)return true;
    std::vector<std::uint8_t> bytes(size);
    if(::sysctl(mib,6,bytes.data(),&size,nullptr,0))return false;
    for(std::size_t at=0;at+sizeof(rt_msghdr)<=size;){
        rt_msghdr header{};std::memcpy(&header,bytes.data()+at,sizeof(header));
        if(header.rtm_msglen<sizeof(header) || header.rtm_msglen>size-at)return false;
        const auto end=at+header.rtm_msglen;std::size_t cursor=at+sizeof(header);
        std::array<std::size_t,RTAX_MAX> offsets{};std::array<std::uint8_t,RTAX_MAX> lengths{};
        for(unsigned slot=0;slot<RTAX_MAX;++slot)if(static_cast<unsigned>(header.rtm_addrs)&(1U<<slot)){
            if(cursor+2>end)return false;const auto length=bytes[cursor];const auto stride=length?(length+3U)&~3U:4U;
            if(stride>end-cursor)return false;
            offsets[slot]=cursor;lengths[slot]=length;cursor+=stride;
        }
        const auto zero=[&](unsigned slot){
            // 遮罩可截短；只檢查實際存在的位址位元組。
            for(std::size_t i=8;i<std::min<std::size_t>(lengths[slot],24);++i)if(bytes[offsets[slot]+i])return false;
            return true;
        };
        const auto inet6=[&](unsigned slot){return lengths[slot]>=24 && bytes[offsets[slot]+1]==AF_INET6;};
        const bool masked=!(static_cast<unsigned>(header.rtm_addrs)&RTA_NETMASK) || zero(RTAX_NETMASK);
        if((header.rtm_flags&(RTF_UP|RTF_GATEWAY))==(RTF_UP|RTF_GATEWAY) && inet6(RTAX_DST) && zero(RTAX_DST) && masked && inet6(RTAX_GATEWAY)){
            const auto item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& value){return value.index==header.rtm_index;});
            if(item!=config.interfaces.end()){
                std::vector<std::uint8_t> gateway(28);gateway[0]=23;
                const auto* source=bytes.data()+offsets[RTAX_GATEWAY];std::copy_n(source+8,16,gateway.begin()+8);
                std::uint32_t scope=0;if(lengths[RTAX_GATEWAY]>=28)std::memcpy(&scope,source+24,4);
                // KAME 路由訊息把 link-local 的介面索引嵌在位址第 2～3 位元組；Windows 以 scope id 表示。
                if(gateway[8]==0xfe && (gateway[9]&0xc0)==0x80){
                    const auto embedded=static_cast<std::uint32_t>(gateway[10])<<8|gateway[11];
                    gateway[10]=gateway[11]=0;if(!scope)scope=embedded?embedded:item->index;
                }
                std::memcpy(gateway.data()+24,&scope,4);
                if(std::find(item->ipv6_gateways.begin(),item->ipv6_gateways.end(),gateway)==item->ipv6_gateways.end())item->ipv6_gateways.push_back(std::move(gateway));
            }
        }
        at=end;
    }
    return true;
}
bool dns(NetworkConfiguration& config,CFTypeRef value){
    config.domain_name=string(field(value,kSCPropNetDNSDomainName));
    const auto data=field(value,kSCPropNetDNSServerAddresses);
    if(!data || CFGetTypeID(data)!=CFArrayGetTypeID())return false;
    config.dns.clear();const auto array=static_cast<CFArrayRef>(data);
    for(CFIndex i=0;i<std::min<CFIndex>(256,CFArrayGetCount(array));++i){
        auto name=string(CFArrayGetValueAtIndex(array,i));std::array<std::uint8_t,16> ip{};std::vector<std::uint8_t> bytes;
        if(::inet_pton(AF_INET,name.c_str(),ip.data())==1){bytes.resize(16);bytes[0]=2;std::copy_n(ip.begin(),4,bytes.begin()+4);}
        else {
            std::uint32_t scope=0;const auto separator=name.find('%');
            if(separator!=name.npos){scope=::if_nametoindex(name.c_str()+separator+1);name.resize(separator);}
            if(::inet_pton(AF_INET6,name.c_str(),ip.data())!=1)continue;
            bytes.resize(28);bytes[0]=23;std::copy(ip.begin(),ip.end(),bytes.begin()+8);std::memcpy(bytes.data()+24,&scope,4);
        }
        if(std::find(config.dns.begin(),config.dns.end(),bytes)==config.dns.end())config.dns.push_back(std::move(bytes));
    }
    return true;
}
#endif
}
void capture_interface_descriptions(NetworkConfiguration& config){
#if defined(__APPLE__)
    startup_profile::Scope devices("network_startup","interfaces",true);
    Owned interfaces(SCNetworkInterfaceCopyAll());
    if(interfaces.value){const auto list=static_cast<CFArrayRef>(interfaces.value);
        for(CFIndex i=0;i<CFArrayGetCount(list);++i){
            const auto interface=static_cast<SCNetworkInterfaceRef>(CFArrayGetValueAtIndex(list,i));const auto name=string(SCNetworkInterfaceGetBSDName(interface));
            const auto item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& value){return value.name==name;});
            if(item!=config.interfaces.end())item->description=string(SCNetworkInterfaceGetLocalizedDisplayName(interface));
        }
    }
#else
    (void)config;
#endif
}
bool capture_metadata(NetworkConfiguration& config,bool* dns_captured){
    startup_profile::Scope phase("network_startup","metadata",true);
    if(dns_captured)*dns_captured=false;
    std::array<char,256> host{};if(::gethostname(host.data(),host.size()-1))return false;
    config.host_name=host.data();
    const auto dot=config.host_name.find('.');if(dot!=config.host_name.npos){config.domain_name=config.host_name.substr(dot+1);config.host_name.resize(dot);}
#if defined(__APPLE__)
    int forwarding=0;std::size_t size=sizeof(forwarding);
    if(::sysctlbyname("net.inet.ip.forwarding",&forwarding,&size,nullptr,0))return false;
    config.routing_enabled=forwarding!=0;
    int proxy=0;size=sizeof(proxy);
    if(::sysctlbyname("net.link.ether.inet.proxyall",&proxy,&size,nullptr,0)==0)config.proxy_enabled=proxy!=0;
    {startup_profile::Scope routes("network_startup","routes",true);if(!gateways(config))return false;
        // IPv6 路由表無法讀取時只缺 IPv6 閘道，不使既有 IPv4 metadata 失效。
        if(!ipv6_gateways(config))for(auto& item:config.interfaces)item.ipv6_gateways.clear();}
    Owned owner(SCDynamicStoreCreate(nullptr,CFSTR("MacWin IP Helper"),nullptr,nullptr));if(!owner.value)return false;
    const auto store=static_cast<SCDynamicStoreRef>(const_cast<void*>(owner.value));
    Owned keys(SCDynamicStoreCopyKeyList(store,CFSTR("Setup:/Network/Service/[^/]+/IPv4")));
    Owned requested(CFArrayCreateMutable(nullptr,0,&kCFTypeArrayCallBacks));
    if(requested.value){
        const auto names=static_cast<CFMutableArrayRef>(const_cast<void*>(requested.value));
        CFArrayAppendValue(names,CFSTR("State:/Network/Global/DNS"));
        if(keys.value){const auto list=static_cast<CFArrayRef>(keys.value);
            for(CFIndex i=0;i<CFArrayGetCount(list);++i){
                const auto setup=string(CFArrayGetValueAtIndex(list,i));if(setup.size()<6)continue;
                const auto prefix=setup.substr(0,setup.size()-5),state=std::string("State:")+prefix.substr(6);
                for(const auto& key:{setup,prefix+"/Interface",state+"/IPv4",state+"/SMB",prefix+"/SMB"}){
                    Owned name(CFStringCreateWithCString(nullptr,key.c_str(),kCFStringEncodingUTF8));
                    if(name.value)CFArrayAppendValue(names,name.value);
                }
            }
        }
    }
    // 一次擷取本輪所需設定，避免每項服務反覆 IPC；失敗才沿用逐項查詢。
    Owned values(requested.value?SCDynamicStoreCopyMultiple(store,static_cast<CFArrayRef>(requested.value),nullptr):nullptr);
    const auto snapshot=static_cast<CFDictionaryRef>(values.value);
    Owned global_dns(copy(store,snapshot,"State:/Network/Global/DNS"));
    if(global_dns.value){const auto captured=dns(config,global_dns.value);if(dns_captured)*dns_captured=captured;}
    {startup_profile::Scope services("network_startup","service_settings",true);
    if(keys.value){const auto list=static_cast<CFArrayRef>(keys.value);
        for(CFIndex i=0;i<CFArrayGetCount(list);++i){
            const auto setup=string(CFArrayGetValueAtIndex(list,i));if(setup.size()<6)continue;
            const auto prefix=setup.substr(0,setup.size()-5),id=prefix.substr(prefix.rfind('/')+1);
            const auto state=std::string("State:")+prefix.substr(6);
            Owned settings(copy(store,snapshot,setup)),link(copy(store,snapshot,prefix+"/Interface")),active(copy(store,snapshot,state+"/IPv4"));
            auto device=string(field(active.value,kSCPropInterfaceName));if(device.empty())device=string(field(link.value,kSCPropNetInterfaceDeviceName));
            const auto item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& value){return value.name==device;});
            if(item==config.interfaces.end())continue;
            const auto mode=field(settings.value,kSCPropNetIPv4ConfigMethod);
            item->dhcp_enabled=item->dhcp_enabled || (mode && CFEqual(mode,kSCValNetIPv4ConfigMethodDHCP));
            address(item->gateways,string(field(active.value,kSCPropNetIPv4Router)));
            Owned smb(copy(store,snapshot,state+"/SMB"));addresses(item->wins,field(smb.value,kSCPropNetSMBWINSAddresses));
            Owned configured_smb(copy(store,snapshot,prefix+"/SMB"));addresses(item->wins,field(configured_smb.value,kSCPropNetSMBWINSAddresses));
            Owned service(CFStringCreateWithCString(nullptr,id.c_str(),kCFStringEncodingUTF8));
            Owned dhcp(service.value?SCDynamicStoreCopyDHCPInfo(store,static_cast<CFStringRef>(service.value)):nullptr);
            if(!dhcp.value)continue;const auto info=static_cast<CFDictionaryRef>(dhcp.value);
            for(unsigned code=1;code<255;++code){const auto value=DHCPInfoGetOptionData(info,static_cast<std::uint8_t>(code));if(value && CFDataGetLength(value)>=0 && CFDataGetLength(value)<=65535)item->dhcp_options[static_cast<std::uint8_t>(code)]={CFDataGetBytePtr(value),CFDataGetBytePtr(value)+CFDataGetLength(value)};}
            const auto start=DHCPInfoGetLeaseStartTime(info);if(start)item->dhcp_enabled=true;
            item->lease_obtained=epoch(start);item->lease_expires=epoch(DHCPInfoGetLeaseExpirationTime(info));
            const auto server=DHCPInfoGetOptionData(info,54);if(server && CFDataGetLength(server)==4)std::copy_n(CFDataGetBytePtr(server),4,item->dhcp_server.begin());
            option_addresses(item->gateways,info,3);option_addresses(item->wins,info,44);
            if(config.scope_id.empty())if(const auto scope=DHCPInfoGetOptionData(info,47);scope && CFDataGetLength(scope)<=259){
                config.scope_id.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(scope)),static_cast<std::size_t>(CFDataGetLength(scope)));
                if(const auto end=config.scope_id.find('\0');end!=config.scope_id.npos)config.scope_id.resize(end);
            }
        }
    }}
#else
    // 其他宿主尚未提供 DHCP／路由 metadata；不可把未知設定宣告為停用。
    return false;
#endif
    return true;
}
}
