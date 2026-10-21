#pragma once
#include "socket_address.hpp"
#include "x64arm/network.hpp"
#include <netinet/in.h>
#include <cstdio>
#include <cstdlib>
namespace x64arm::socket_detail {
#if defined(__APPLE__)
inline constexpr int packet_info6=IPV6_2292PKTINFO,hop_limit6=IPV6_2292HOPLIMIT;
#else
inline constexpr int packet_info6=IPV6_PKTINFO,hop_limit6=IPV6_HOPLIMIT;
#endif
struct MessageError {std::uint32_t code;};
struct SendMessage {std::uint64_t buffers{},name{};std::uint32_t count{},name_size{};std::vector<std::uint8_t> control;};
struct ReceiveMessage {
    std::uint64_t pointer{},name{},control{};
    std::uint32_t name_size{},control_size{},flags{};
    std::uint64_t total{};std::vector<SocketBuffer> buffers;
};
inline ReceiveMessage read_receive_message(const GuestMemory& memory,std::uint64_t pointer,bool x86,int family,std::uint64_t limit){
    const auto require=[](bool value,std::uint32_t error){if(!value)throw MessageError{error};};
    require(pointer!=0,10014);const unsigned unit=x86?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);ReceiveMessage result;result.pointer=pointer;
    memory.check(GuestAddress{pointer},x86?28:56,Access::Read);memory.check(GuestAddress{pointer},x86?28:56,Access::Write);
    result.name=memory.load(GuestAddress{pointer},bits);result.name_size=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+unit},32));
    require(result.name || !result.name_size,10014);
    if(result.name){const unsigned required=family==AF_INET6?28U:16U;require(result.name_size>=required && result.name_size<=INT32_MAX,10014);memory.check(GuestAddress{result.name},required,Access::Write);}
    const auto buffers=memory.load(GuestAddress{pointer+unit*2},bits),count=memory.load(GuestAddress{pointer+unit*3},32);
    require(buffers && count && count<=1024,10022);result.buffers.reserve(static_cast<std::size_t>(count));
    for(unsigned i=0;i<count;++i){const auto at=buffers+i*(x86?8U:16U);SocketBuffer buffer{memory.load(GuestAddress{at+unit},bits),static_cast<std::uint32_t>(memory.load(GuestAddress{at},32))};
        require(buffer.length<=limit-result.total,10055);memory.check(GuestAddress{buffer.pointer},buffer.length,Access::Write);result.total+=buffer.length;result.buffers.push_back(buffer);}
    result.control_size=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+unit*4},32));result.control=memory.load(GuestAddress{pointer+unit*5},bits);
    require(result.control || !result.control_size,10014);memory.check(GuestAddress{result.control},result.control_size,Access::Write);
    result.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+unit*6},32));require(!(result.flags&~2U),10022);return result;
}
inline void write_receive_message(GuestMemory& memory,std::uint64_t pointer,bool x86,std::uint64_t name,std::uint64_t control,
                                  std::span<const std::uint8_t> address,std::span<const std::uint8_t> ancillary,std::uint32_t flags){
    const unsigned unit=x86?4:8;
    if(name && !address.empty())memory.write(GuestAddress{name},address);
    if(!ancillary.empty())memory.write(GuestAddress{control},ancillary);
    memory.store(GuestAddress{pointer+unit},32,name?address.size():0);
    memory.store(GuestAddress{pointer+unit*4},32,ancillary.size());memory.store(GuestAddress{pointer+unit*6},32,flags);
}
inline SendMessage read_send_message(const GuestMemory& memory,std::uint64_t pointer,bool x86){
    const auto require=[](bool value,std::uint32_t error){if(!value)throw MessageError{error};};require(pointer!=0,10014);const unsigned unit=x86?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);SendMessage result;
    memory.check(GuestAddress{pointer},x86?28:56,Access::Read);result.name=memory.load(GuestAddress{pointer},bits);result.name_size=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+unit},32));require(result.name || !result.name_size,10014);
    if(result.name){const auto address=read_address(memory,result.name,result.name_size);require(address.size!=0,10014);}
    result.buffers=memory.load(GuestAddress{pointer+unit*2},bits);result.count=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+unit*3},32));require(result.buffers && result.count && result.count<=1024,10022);
    const auto control_size=memory.load(GuestAddress{pointer+unit*4},32),control_pointer=memory.load(GuestAddress{pointer+unit*5},bits);require(control_size<=65536 && (!control_size || control_pointer),10014);std::vector<std::uint8_t> control(static_cast<std::size_t>(control_size));memory.read(GuestAddress{control_pointer},control);
    const auto number=[&](std::size_t offset,unsigned count){require(offset<=control.size() && count<=control.size()-offset,10022);std::uint64_t value=0;for(unsigned i=0;i<count;++i)value|=std::uint64_t(control[offset+i])<<(8*i);return value;};
    const auto append=[&](int level,int type,const void* data,std::size_t size){const auto at=result.control.size();result.control.resize(at+CMSG_SPACE(static_cast<unsigned>(size)));cmsghdr header{};header.cmsg_level=level;header.cmsg_type=type;header.cmsg_len=static_cast<decltype(header.cmsg_len)>(CMSG_LEN(static_cast<unsigned>(size)));std::memcpy(result.control.data()+at,&header,sizeof(header));std::memcpy(result.control.data()+at+CMSG_LEN(0),data,size);};
    std::array<bool,2> ecn_seen{};const std::size_t header=x86?12:16;for(std::size_t at=0;at<control.size();){const auto length=number(at,unit);require(length>=header && length<=control.size()-at,10022);const auto level=number(at+unit,4),type=number(at+unit+4,4),size=length-header;const auto* data=control.data()+at+header;if(std::getenv("X64ARM_TRACE_NETWORK_STATE"))std::fprintf(stderr,"[net] WSACMSG length=%llu level=%llu type=%llu size=%llu buffer_size=%zu\n",static_cast<unsigned long long>(length),static_cast<unsigned long long>(level),static_cast<unsigned long long>(type),static_cast<unsigned long long>(size),control.size());
        if(level==0 && type==19){require(size==8,10022);in_pktinfo info{};std::memcpy(&info.ipi_spec_dst,data,4);std::memcpy(&info.ipi_ifindex,data+4,4);append(IPPROTO_IP,IP_PKTINFO,&info,sizeof(info));}
        else if(level==41 && type==19){require(size==20,10022);in6_pktinfo info{};std::memcpy(&info.ipi6_addr,data,16);std::memcpy(&info.ipi6_ifindex,data+16,4);append(IPPROTO_IPV6,packet_info6,&info,sizeof(info));}
        else if((level==0 || level==41) && type==50){require(size==4,10022);int value{};std::memcpy(&value,data,4);const unsigned slot=level==0?0U:1U;require(value>=0 && value<=2 && !ecn_seen[slot],10022);ecn_seen[slot]=true;append(SOL_SOCKET,slot?send_ecn_control6:send_ecn_control4,&value,4);}
        else if((level==0 && (type==3 || type==4)) || (level==41 && (type==21 || type==39))){require(size==4,10022);int value=0;std::memcpy(&value,data,4);require(value>=-1 && value<=255,10022);append(level==0?IPPROTO_IP:IPPROTO_IPV6,level==0?(type==3?IP_TOS:IP_TTL):(type==39?IPV6_TCLASS:hop_limit6),&value,sizeof(value));}
        else throw MessageError{10042};
        const auto rounded=(length+unit-1)&~std::uint64_t(unit-1);if(rounded>control.size()-at){require(length==control.size()-at,10022);at=control.size();}else at+=static_cast<std::size_t>(rounded);
    }return result;
}
}
