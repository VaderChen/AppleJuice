// 宿主網路協定統計：IPv4／ICMP／TCP／UDP 使用 SDK 公開的 ipstat、icmpstat、tcpstat、udpstat，ICMPv6 使用 icmp6stat；
// IPv6 的 ip6stat 未公開，只取 BSD 系列一致的前 14 個 64 位元欄位（total～reassembled）。macOS 對一般使用者的 TCP 計數回傳 0。
#include "network_statistics.hpp"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp_var.h>
#include <netinet/icmp6.h>
#include <algorithm>
#include <vector>

namespace x64arm::host_network_statistics {
namespace {
template<class T> bool read(const char* name,T& value){std::size_t size=sizeof value;return sysctlbyname(name,&value,&size,nullptr,0)==0;}
std::uint32_t clamp(std::uint64_t value){return static_cast<std::uint32_t>(std::min<std::uint64_t>(value,UINT32_MAX));}
}
std::optional<std::array<std::uint32_t,23>> ip(bool ipv6){
    std::array<std::uint32_t,23> v{};int forwarding=0,ttl=64;
    if(!ipv6){
        ipstat s{};if(!read("net.inet.ip.stats",s))return std::nullopt;(void)read("net.inet.ip.forwarding",forwarding);(void)read("net.inet.ip.ttl",ttl);
        v={forwarding?1U:2U,static_cast<std::uint32_t>(ttl),s.ips_total,clamp(std::uint64_t{s.ips_badsum}+s.ips_tooshort+s.ips_toosmall+s.ips_badhlen+s.ips_badlen+s.ips_badvers),
           clamp(std::uint64_t{s.ips_cantforward}+s.ips_badaddr),s.ips_forward,s.ips_noproto,s.ips_fragdropped,s.ips_delivered,s.ips_localout,0,s.ips_odropped,s.ips_noroute,
           60,s.ips_fragments,s.ips_reassembled,clamp(std::uint64_t{s.ips_fragdropped}+s.ips_fragtimeout),s.ips_fragmented,s.ips_cantfrag,s.ips_ofragments,0,0,0};
        return v;
    }
    std::vector<std::uint64_t> raw(2048);std::size_t size=raw.size()*8;if(sysctlbyname("net.inet6.ip6.stats",raw.data(),&size,nullptr,0)!=0 || size<14*8)return std::nullopt;(void)read("net.inet6.ip6.forwarding",forwarding);(void)read("net.inet6.ip6.hlim",ttl);
    // ip6stat：total、tooshort、toosmall、fragments、fragdropped、fragtimeout、fragoverflow、forward、cantforward、redirectsent、delivered、localout、odropped、reassembled。
    v={forwarding?1U:2U,static_cast<std::uint32_t>(ttl),clamp(raw[0]),clamp(raw[1]+raw[2]),clamp(raw[8]),clamp(raw[7]),0,0,clamp(raw[10]),clamp(raw[11]),0,clamp(raw[12]),0,
       60,clamp(raw[3]),clamp(raw[13]),clamp(raw[4]+raw[5]+raw[6]),0,0,0,0,0,0};
    return v;
}
std::optional<std::array<std::uint32_t,15>> tcp(){
    tcpstat s{};if(!read("net.inet.tcp.stats",s))return std::nullopt;
    // RtoAlgorithm＝MIB_TCP_RTO_VANJ（4），RTO 範圍與 BSD 預設一致（毫秒），MaxConn＝-1（動態）。
    return std::array<std::uint32_t,15>{4,200,64000,UINT32_MAX,s.tcps_connattempt,s.tcps_accepts,s.tcps_conndrops,s.tcps_drops,0,s.tcps_rcvtotal,
        s.tcps_sndtotal>=s.tcps_sndrexmitpack?s.tcps_sndtotal-s.tcps_sndrexmitpack:0,s.tcps_sndrexmitpack,clamp(std::uint64_t{s.tcps_rcvbadsum}+s.tcps_rcvbadoff+s.tcps_rcvshort),0,0};
}
std::optional<std::array<std::uint32_t,5>> udp(){
    udpstat s{};if(!read("net.inet.udp.stats",s))return std::nullopt;
    const std::uint64_t errors=std::uint64_t{s.udps_hdrops}+s.udps_badsum+s.udps_badlen+s.udps_fullsock,unreached=std::uint64_t{s.udps_noport}+s.udps_noportbcast;
    const std::uint64_t delivered=s.udps_ipackets>errors+unreached?s.udps_ipackets-errors-unreached:0;
    return std::array<std::uint32_t,5>{clamp(delivered),clamp(unreached),clamp(errors),s.udps_opackets,0};
}
std::optional<Icmp> icmp(bool ipv6){
    Icmp result;
    if(!ipv6){
        icmpstat s{};if(!read("net.inet.icmp.stats",s))return std::nullopt;
        for(unsigned t=0;t<=ICMP_MAXTYPE;++t){result.in[t]=s.icps_inhist[t];result.out[t]=s.icps_outhist[t];}
        result.in_errors=clamp(std::uint64_t{s.icps_badcode}+s.icps_tooshort+s.icps_checksum+s.icps_badlen);
    }else{
        icmp6stat s{};if(!read("net.inet6.icmp6.stats",s))return std::nullopt;
        for(unsigned t=0;t<256;++t){result.in[t]=clamp(s.icp6s_inhist[t]);result.out[t]=clamp(s.icp6s_outhist[t]);}
        result.in_errors=clamp(s.icp6s_badcode+s.icp6s_tooshort+s.icp6s_checksum+s.icp6s_badlen);
    }
    std::uint64_t in=result.in_errors,out=0;for(unsigned t=0;t<256;++t){in+=result.in[t];out+=result.out[t];}
    result.in_messages=clamp(in);result.out_messages=clamp(out);return result;
}
}
