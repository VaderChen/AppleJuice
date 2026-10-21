#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <tuple>
#include <unistd.h>
#include <arpa/inet.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#endif
namespace x64arm {
namespace {
struct Error {std::uint32_t code;};
void require(bool value,std::uint32_t code=87){if(!value)throw Error{code};}
struct Row {std::array<std::uint8_t,16> local{},remote{};std::uint32_t local_scope{},remote_scope{},state{},pid{};std::uint16_t local_port{},remote_port{};bool ipv6{};};
void put(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint64_t value,unsigned width=4){for(unsigned i=0;i<width;++i)bytes.at(offset+i)=static_cast<std::uint8_t>(value>>(8*i));}
}
std::uint32_t WindowsServices::tcp_table_call(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    try {
        require(arg(1));const bool extended=symbol=="GetExtendedTcpTable";const auto family=extended?static_cast<std::uint32_t>(arg(3)):2U,kind=extended?static_cast<std::uint32_t>(arg(4)):2U;
        require(family==2 || family==23);require(kind<=8 && (!extended || !static_cast<std::uint32_t>(arg(5))));if(kind>=6 || (family==23 && kind<3))return 50;
        const auto capacity=memory.load(GuestAddress{arg(1)},32);memory.check(GuestAddress{arg(1)},4,Access::Write);if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生 TCP 表查詢");std::vector<std::uint32_t> pids;const auto status=session_process_ids(pids);require(!status,status);std::vector<Row> rows;
#if defined(__APPLE__)
        constexpr std::array<std::uint32_t,11> states{1,2,3,4,5,8,6,9,10,7,11};
        // 範圍與 Toolhelp 相同：目前 Windows session 的引擎程序，不納入其他 macOS 應用。
        for(const auto owner:pids){const auto pid=owner==process_id()?::getpid():static_cast<pid_t>(owner);int size=::proc_pidinfo(pid,PROC_PIDLISTFDS,0,nullptr,0);if(size<=0){if(errno==ESRCH)continue;throw Error{errno==EACCES || errno==EPERM?5U:31U};}require(size<=2*1024*1024,8);std::vector<proc_fdinfo> descriptors;
            for(unsigned attempt=0;attempt<4;++attempt){descriptors.resize(static_cast<std::size_t>(size)/sizeof(proc_fdinfo)+32);require(descriptors.size()*sizeof(proc_fdinfo)<=2*1024*1024,8);const auto count=::proc_pidinfo(pid,PROC_PIDLISTFDS,0,descriptors.data(),static_cast<int>(descriptors.size()*sizeof(proc_fdinfo)));if(count<=0){if(errno==ESRCH){descriptors.clear();break;}throw Error{errno==EACCES || errno==EPERM?5U:31U};}require(count%static_cast<int>(sizeof(proc_fdinfo))==0,13);if(static_cast<std::size_t>(count)<descriptors.size()*sizeof(proc_fdinfo)){descriptors.resize(static_cast<std::size_t>(count)/sizeof(proc_fdinfo));break;}require(attempt<3,8);size=count*2;}
            for(const auto& descriptor:descriptors){if(descriptor.proc_fdtype!=PROX_FDTYPE_SOCKET)continue;socket_fdinfo info{};const auto count=::proc_pidfdinfo(pid,descriptor.proc_fd,PROC_PIDFDSOCKETINFO,&info,sizeof(info));if(count!=static_cast<int>(sizeof(info))){if(errno==EBADF || errno==ENOENT || errno==ESRCH)continue;throw Error{errno==EACCES || errno==EPERM?5U:31U};}if(info.psi.soi_kind!=SOCKINFO_TCP)continue;
                const auto& tcp=info.psi.soi_proto.pri_tcp;const auto& ip=tcp.tcpsi_ini;const bool ipv6=info.psi.soi_family==AF_INET6;if((family==23)!=ipv6 || tcp.tcpsi_state<0 || static_cast<std::size_t>(tcp.tcpsi_state)>=states.size())continue;const auto state=states[static_cast<std::size_t>(tcp.tcpsi_state)];if(kind%3==0 && state!=2)continue;if(kind%3==1 && (state==1 || state==2 || state==12))continue;
                Row row;row.state=state;row.pid=owner;row.ipv6=ipv6;row.local_port=static_cast<std::uint16_t>(ip.insi_lport);row.remote_port=static_cast<std::uint16_t>(ip.insi_fport);
                if(ipv6){std::memcpy(row.local.data(),&ip.insi_laddr.ina_6,16);std::memcpy(row.remote.data(),&ip.insi_faddr.ina_6,16);const auto scope=[&](auto& address){if(address[0]==0xfe && (address[1]&0xc0)==0x80){const auto index=static_cast<std::uint32_t>((unsigned(address[2])<<8)|address[3]);address[2]=address[3]=0;return index?index:static_cast<std::uint32_t>(ip.insi_v6.in6_ifindex);}return 0U;};row.local_scope=scope(row.local);row.remote_scope=scope(row.remote);}
                else {std::memcpy(row.local.data(),&ip.insi_laddr.ina_46.i46a_addr4,4);std::memcpy(row.remote.data(),&ip.insi_faddr.ina_46.i46a_addr4,4);}if(row.state==2){row.remote={};row.remote_port=0;row.remote_scope=0;}rows.push_back(row);require(rows.size()<=65536,8);
            }
        }
#else
        return 50;
#endif
        if(static_cast<std::uint32_t>(arg(2)))std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return std::tuple{a.local,a.local_scope,static_cast<unsigned>(ntohs(a.local_port)),a.remote,a.remote_scope,static_cast<unsigned>(ntohs(a.remote_port))}<std::tuple{b.local,b.local_scope,static_cast<unsigned>(ntohs(b.local_port)),b.remote,b.remote_scope,static_cast<unsigned>(ntohs(b.remote_port))};});
        // 相同 descriptor 被 dup 時只列一個 TCP endpoint。
        std::vector<Row> unique;for(const auto& row:rows)if(std::none_of(unique.begin(),unique.end(),[&](const auto& old){return old.pid==row.pid && old.local==row.local && old.remote==row.remote && old.local_port==row.local_port && old.remote_port==row.remote_port && old.local_scope==row.local_scope && old.remote_scope==row.remote_scope;}))unique.push_back(row);
        const unsigned stride=family==23?56:kind<3?20:24;std::vector<std::uint8_t> bytes(4+unique.size()*stride);put(bytes,0,unique.size());for(std::size_t i=0;i<unique.size();++i){const auto at=4+i*stride;const auto& row=unique[i];if(family==23){std::copy(row.local.begin(),row.local.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));put(bytes,at+16,row.local_scope);put(bytes,at+20,row.local_port);std::copy(row.remote.begin(),row.remote.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at+24));put(bytes,at+40,row.remote_scope);put(bytes,at+44,row.remote_port);put(bytes,at+48,row.state);put(bytes,at+52,row.pid);}else{put(bytes,at,row.state);std::copy_n(row.local.begin(),4,bytes.begin()+static_cast<std::ptrdiff_t>(at+4));put(bytes,at+8,row.local_port);std::copy_n(row.remote.begin(),4,bytes.begin()+static_cast<std::ptrdiff_t>(at+12));put(bytes,at+16,row.remote_port);if(kind>=3)put(bytes,at+20,row.pid);}}
        memory.store(GuestAddress{arg(1)},32,bytes.size());require(arg(0) && capacity>=bytes.size(),122);memory.write(GuestAddress{arg(0)},bytes);return 0;
    }catch(const Error& error){return error.code;}catch(const MemoryFault&){return 87;}catch(const std::bad_alloc&){return 8;}
}
}
