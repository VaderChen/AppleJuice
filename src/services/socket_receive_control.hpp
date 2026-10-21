#pragma once
#include "socket_multicast_filter.hpp"
#include "socket_receive_options.hpp"
#include <ifaddrs.h>
#include <net/if.h>
#include <span>

namespace x64arm::socket_detail {
inline constexpr std::uint32_t guest_message_truncated=0x100,guest_control_truncated=0x200,
    guest_message_broadcast=0x400,guest_message_multicast=0x800;
inline int destination_broadcast4(const std::array<std::uint8_t,4>& bytes,std::uint32_t index){
    std::uint32_t address{};std::memcpy(&address,bytes.data(),4);if(address==INADDR_BROADCAST)return true;
    ifaddrs* list{};if(::getifaddrs(&list)<0)return -1;bool found=false;
    for(auto* item=list;item;item=item->ifa_next)if(item->ifa_broadaddr && item->ifa_broadaddr->sa_family==AF_INET &&
        (item->ifa_flags&IFF_BROADCAST) && ::if_nametoindex(item->ifa_name)==index &&
        reinterpret_cast<const sockaddr_in*>(item->ifa_broadaddr)->sin_addr.s_addr==address){found=true;break;}
    ::freeifaddrs(list);return found;
}
// 原生內部 metadata 與客體選項分開。產生固定寬度 WSACMSGHDR，不複製宿主 padding／pointer。
inline bool encode_receive_control(const msghdr& message,bool x86,std::uint32_t policy,
                                   std::span<std::uint8_t> output,std::size_t& used,std::uint32_t& flags){
    used=0;flags=(message.msg_flags&MSG_TRUNC)?guest_message_truncated:0;
    if(message.msg_controllen && !message.msg_control){errno=EFAULT;return false;}
    const auto* bytes=static_cast<const std::uint8_t*>(message.msg_control);const auto count=std::size_t(message.msg_controllen);
    std::array<std::uint8_t,4> address4{};std::array<std::uint8_t,16> address6{};
    std::uint32_t index4{},index6{};bool have4=false,have6=false;
    std::array<std::uint32_t,4> scalars{};std::array<bool,4> present{};
    for(std::size_t at=0;at<count;){
        if(count-at<sizeof(cmsghdr)){errno=EPROTO;return false;}cmsghdr control{};std::memcpy(&control,bytes+at,sizeof control);
        if(control.cmsg_len<CMSG_LEN(0) || control.cmsg_len>count-at){errno=EPROTO;return false;}
        const auto size=control.cmsg_len-CMSG_LEN(0);const auto* data=bytes+at+CMSG_LEN(0);
        if(control.cmsg_level==IPPROTO_IP && control.cmsg_type==IP_RECVDSTADDR){
            if(size<4){errno=EPROTO;return false;}std::memcpy(address4.data(),data,4);have4=true;
        }else if(control.cmsg_level==IPPROTO_IP && control.cmsg_type==IP_RECVIF){
            if(size<offsetof(sockaddr_dl,sdl_data)){errno=EPROTO;return false;}sockaddr_dl interface{};std::memcpy(&interface,data,std::min<std::size_t>(size,sizeof interface));index4=interface.sdl_index;
        }else if(control.cmsg_level==IPPROTO_IP && control.cmsg_type==IP_PKTINFO){
            if(size<sizeof(in_pktinfo)){errno=EPROTO;return false;}in_pktinfo info{};std::memcpy(&info,data,sizeof info);std::memcpy(address4.data(),&info.ipi_addr,4);index4=static_cast<std::uint32_t>(info.ipi_ifindex);have4=true;
        }else if(control.cmsg_level==IPPROTO_IPV6 && (control.cmsg_type==19 || control.cmsg_type==46)){
            if(size<20){errno=EPROTO;return false;}in6_addr address{};std::memcpy(&address,data,16);std::uint32_t index{};std::memcpy(&index,data+16,4);
            if(IN6_IS_ADDR_V4MAPPED(&address)){std::memcpy(address4.data(),data+12,4);index4=index;have4=true;}
            else{std::memcpy(address6.data(),data,16);index6=index;have6=true;}
        }else{
            int scalar=-1;
            if(control.cmsg_level==IPPROTO_IP){if(control.cmsg_type==IP_TTL || control.cmsg_type==IP_RECVTTL)scalar=0;else if(control.cmsg_type==IP_TOS || control.cmsg_type==IP_RECVTOS)scalar=1;}
            else if(control.cmsg_level==IPPROTO_IPV6){if(control.cmsg_type==20 || control.cmsg_type==47)scalar=2;else if(control.cmsg_type==IPV6_TCLASS)scalar=3;}
            if(scalar>=0){
                const auto slot=static_cast<unsigned>(scalar);std::uint32_t value{};
                if(size==1 && slot<2)value=*data;
                else if(size==4)std::memcpy(&value,data,4);
                else{errno=EPROTO;return false;}
                if(value>255 || present[slot]){errno=EPROTO;return false;}scalars[slot]=value;present[slot]=true;
            }
        }
        const auto next=std::size_t(CMSG_SPACE(size));if(next>count-at){if(control.cmsg_len!=count-at){errno=EPROTO;return false;}break;}at+=next;
    }
    // Darwin 不提供 BSD MSG_BCAST／MSG_MCAST；不可把同值 MSG_EOF 等誤映射。
    if(have4){std::uint32_t address{};std::memcpy(&address,address4.data(),4);if(IN_MULTICAST(ntohl(address)))flags|=guest_message_multicast;const auto broadcast=destination_broadcast4(address4,index4);if(broadcast<0)return false;if(broadcast)flags|=guest_message_broadcast;}
    if(have6){in6_addr address{};std::memcpy(&address,address6.data(),16);if(IN6_IS_ADDR_MULTICAST(&address))flags|=guest_message_multicast;}
    std::array<std::uint8_t,256> encoded{};std::size_t encoded_size=0,meaningful=0;
    const auto put=[](std::uint8_t* target,unsigned width,std::uint64_t value){for(unsigned i=0;i<width;++i)target[i]=static_cast<std::uint8_t>(value>>(i*8));};
    const unsigned unit=x86?4U:8U,header=x86?12U:16U;
    const auto append=[&](std::uint32_t level,const auto& address,std::uint32_t index){
        const auto start=encoded_size,length=header+address.size()+4;encoded_size+=((length+unit-1)&~std::size_t(unit-1));
        put(encoded.data()+start,unit,length);put(encoded.data()+start+unit,4,level);put(encoded.data()+start+unit+4,4,19);
        std::memcpy(encoded.data()+start+header,address.data(),address.size());put(encoded.data()+start+header+address.size(),4,index);meaningful=start+length;
    };
    if((policy&1) && have4){if(!index4){errno=EPROTO;return false;}append(0,address4,index4);}
    if((policy&2) && have6){if(!index6){errno=EPROTO;return false;}append(41,address6,index6);}
    // 雙堆疊 v4 mapped 封包可由原生 IPv6 metadata 帶回 hop/class；
    // 仍依封包家族及各自的客體選項匯出，不把 IPv6 設定套到 IPv4。
    if(have4 && !have6){for(unsigned slot=0;slot<2;++slot)if(!present[slot] && present[slot+2]){present[slot]=true;scalars[slot]=scalars[slot+2];}}
    const auto append_scalar=[&](std::uint32_t value,std::uint32_t level,std::uint32_t type){
        const auto start=encoded_size,length=std::size_t(header)+4;encoded_size+=(length+unit-1)&~std::size_t(unit-1);
        put(encoded.data()+start,unit,length);put(encoded.data()+start+unit,4,level);put(encoded.data()+start+unit+4,4,type);put(encoded.data()+start+header,4,value);meaningful=start+length;
    };
    if(have4 && (policy&4) && present[0])append_scalar(scalars[0],0,4);
    if(have4 && (policy&8) && present[1])append_scalar(scalars[1],0,3);
    if(have6 && (policy&16) && present[2])append_scalar(scalars[2],41,21);
    if(have6 && (policy&32) && present[3])append_scalar(scalars[3],41,39);
    // ECN 是實際欄位的最低兩個位元；不可布林化或破壞同時匯出的 DSCP。
    if(have4 && (policy&receive_ecn4) && present[1])append_scalar(scalars[1]&3U,0,50);
    if(have6 && (policy&receive_ecn6) && present[3])append_scalar(scalars[3]&3U,41,50);
    if(policy && (message.msg_flags&MSG_CTRUNC))flags|=guest_control_truncated;
    used=std::min(output.size(),encoded_size);if(used)std::memcpy(output.data(),encoded.data(),used);
    if(output.size()<meaningful)flags|=guest_control_truncated;
    return true;
}
inline bool encode_receive_control(const msghdr& message,bool x86,bool requested4,bool requested6,
                                   std::span<std::uint8_t> output,std::size_t& used,std::uint32_t& flags){
    return encode_receive_control(message,x86,(requested4?1U:0U)|(requested6?2U:0U),output,used,flags);
}
} // namespace x64arm::socket_detail
