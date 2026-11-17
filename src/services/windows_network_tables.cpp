#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
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
#if defined(__APPLE__)
// 列舉 session 內各引擎程序的 socket（TCP／UDP 表共用）；程序已結束或 descriptor 已關閉時略過，權限不足等錯誤轉成 Win32 錯誤。
template<class Visit> void each_session_socket(const std::vector<std::uint32_t>& pids,std::uint32_t self,Visit&& visit){
    for(const auto owner:pids){const auto pid=owner==self?::getpid():static_cast<pid_t>(owner);int size=::proc_pidinfo(pid,PROC_PIDLISTFDS,0,nullptr,0);if(size<=0){if(errno==ESRCH)continue;throw Error{errno==EACCES || errno==EPERM?5U:31U};}require(size<=2*1024*1024,8);std::vector<proc_fdinfo> descriptors;
        for(unsigned attempt=0;attempt<4;++attempt){descriptors.resize(static_cast<std::size_t>(size)/sizeof(proc_fdinfo)+32);require(descriptors.size()*sizeof(proc_fdinfo)<=2*1024*1024,8);const auto count=::proc_pidinfo(pid,PROC_PIDLISTFDS,0,descriptors.data(),static_cast<int>(descriptors.size()*sizeof(proc_fdinfo)));if(count<=0){if(errno==ESRCH){descriptors.clear();break;}throw Error{errno==EACCES || errno==EPERM?5U:31U};}require(count%static_cast<int>(sizeof(proc_fdinfo))==0,13);if(static_cast<std::size_t>(count)<descriptors.size()*sizeof(proc_fdinfo)){descriptors.resize(static_cast<std::size_t>(count)/sizeof(proc_fdinfo));break;}require(attempt<3,8);size=count*2;}
        for(const auto& descriptor:descriptors){if(descriptor.proc_fdtype!=PROX_FDTYPE_SOCKET)continue;socket_fdinfo info{};const auto count=::proc_pidfdinfo(pid,descriptor.proc_fd,PROC_PIDFDSOCKETINFO,&info,sizeof(info));if(count!=static_cast<int>(sizeof(info))){if(errno==EBADF || errno==ENOENT || errno==ESRCH)continue;throw Error{errno==EACCES || errno==EPERM?5U:31U};}
            visit(owner,info);
        }
    }
}
// 連結本機 IPv6 位址在 BSD 核心中把 scope 放在第 2～3 位元組；Windows 表格的位址不含 scope，另列 scope ID。
std::uint32_t link_local_scope(std::array<std::uint8_t,16>& address,const in_sockinfo& ip){
    if(address[0]==0xfe && (address[1]&0xc0)==0x80){const auto index=static_cast<std::uint32_t>((unsigned(address[2])<<8)|address[3]);address[2]=address[3]=0;return index?index:static_cast<std::uint32_t>(ip.insi_v6.in6_ifindex);}
    return 0U;
}
#endif
}
std::uint32_t WindowsServices::tcp_table_call(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    try {
        // GetTcpTable（MIB_TCPROW）、GetTcpTable2（MIB_TCPROW2）、GetTcp6Table（MIB_TCP6ROW，State 在前）、GetTcp6Table2（MIB_TCP6ROW2）與
        // GetExtendedTcpTable 的 BASIC／OWNER_PID／OWNER_MODULE 類別；OWNER_MODULE 表頭對齊 8，建立時間與模組資訊欄位為 0（GetOwnerModuleFromTcp*Entry 依 PID 查詢）。
        require(arg(1));const bool extended=symbol=="GetExtendedTcpTable",version2=symbol=="GetTcpTable2" || symbol=="GetTcp6Table2",ipv6_named=symbol.starts_with("GetTcp6");
        const auto family=extended?static_cast<std::uint32_t>(arg(3)):ipv6_named?23U:2U,kind=extended?static_cast<std::uint32_t>(arg(4)):version2?5U:2U;
        require(family==2 || family==23);require(kind<=8 && (!extended || !static_cast<std::uint32_t>(arg(5))));if(extended && family==23 && kind<3)return 50;
        const auto capacity=memory.load(GuestAddress{arg(1)},32);memory.check(GuestAddress{arg(1)},4,Access::Write);if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生 TCP 表查詢");std::vector<std::uint32_t> pids;const auto status=session_process_ids(pids);require(!status,status);std::vector<Row> rows;
#if defined(__APPLE__)
        constexpr std::array<std::uint32_t,11> states{1,2,3,4,5,8,6,9,10,7,11};
        // 範圍與 Toolhelp 相同：目前 Windows session 的引擎程序，不納入其他 macOS 應用。
        each_session_socket(pids,process_id(),[&](std::uint32_t owner,const socket_fdinfo& info){
                if(info.psi.soi_kind!=SOCKINFO_TCP)return;
                const auto& tcp=info.psi.soi_proto.pri_tcp;const auto& ip=tcp.tcpsi_ini;const bool ipv6=info.psi.soi_family==AF_INET6;if((family==23)!=ipv6 || tcp.tcpsi_state<0 || static_cast<std::size_t>(tcp.tcpsi_state)>=states.size())return;const auto state=states[static_cast<std::size_t>(tcp.tcpsi_state)];if(kind%3==0 && state!=2)return;if(kind%3==1 && (state==1 || state==2 || state==12))return;
                Row row;row.state=state;row.pid=owner;row.ipv6=ipv6;row.local_port=static_cast<std::uint16_t>(ip.insi_lport);row.remote_port=static_cast<std::uint16_t>(ip.insi_fport);
                if(ipv6){std::memcpy(row.local.data(),&ip.insi_laddr.ina_6,16);std::memcpy(row.remote.data(),&ip.insi_faddr.ina_6,16);row.local_scope=link_local_scope(row.local,ip);row.remote_scope=link_local_scope(row.remote,ip);}
                else {std::memcpy(row.local.data(),&ip.insi_laddr.ina_46.i46a_addr4,4);std::memcpy(row.remote.data(),&ip.insi_faddr.ina_46.i46a_addr4,4);}if(row.state==2){row.remote={};row.remote_port=0;row.remote_scope=0;}rows.push_back(row);require(rows.size()<=65536,8);
        });
#else
        return 50;
#endif
        if(static_cast<std::uint32_t>(arg(2)))std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return std::tuple{a.local,a.local_scope,static_cast<unsigned>(ntohs(a.local_port)),a.remote,a.remote_scope,static_cast<unsigned>(ntohs(a.remote_port))}<std::tuple{b.local,b.local_scope,static_cast<unsigned>(ntohs(b.local_port)),b.remote,b.remote_scope,static_cast<unsigned>(ntohs(b.remote_port))};});
        // 相同 descriptor 被 dup 時只列一個 TCP endpoint。
        std::vector<Row> unique;for(const auto& row:rows)if(std::none_of(unique.begin(),unique.end(),[&](const auto& old){return old.pid==row.pid && old.local==row.local && old.remote==row.remote && old.local_port==row.local_port && old.remote_port==row.remote_port && old.local_scope==row.local_scope && old.remote_scope==row.remote_scope;}))unique.push_back(row);
        const bool module=kind>=6,v6_row=symbol=="GetTcp6Table";const unsigned header=module?8:4;
        const unsigned stride=family==23?(v6_row?52U:module?192U:version2?60U:56U):(module?160U:version2?28U:kind<3?20U:24U);
        std::vector<std::uint8_t> bytes(header+unique.size()*stride);put(bytes,0,unique.size());
        for(std::size_t i=0;i<unique.size();++i){
            const auto at=header+i*stride;const auto& row=unique[i];
            const auto copy=[&](const std::array<std::uint8_t,16>& address,std::size_t offset,unsigned count){std::copy_n(address.begin(),count,bytes.begin()+static_cast<std::ptrdiff_t>(at+offset));};
            if(v6_row){put(bytes,at,row.state);copy(row.local,4,16);put(bytes,at+20,row.local_scope);put(bytes,at+24,row.local_port);copy(row.remote,28,16);put(bytes,at+44,row.remote_scope);put(bytes,at+48,row.remote_port);}
            else if(family==23){copy(row.local,0,16);put(bytes,at+16,row.local_scope);put(bytes,at+20,row.local_port);copy(row.remote,24,16);put(bytes,at+40,row.remote_scope);put(bytes,at+44,row.remote_port);put(bytes,at+48,row.state);put(bytes,at+52,row.pid);}
            else{put(bytes,at,row.state);copy(row.local,4,4);put(bytes,at+8,row.local_port);copy(row.remote,12,4);put(bytes,at+16,row.remote_port);if(kind>=3)put(bytes,at+20,row.pid);}
            // Table2 的 dwOffloadState 為 TcpConnectionOffloadStateInHost（0），已由清零的列提供。
        }
        memory.store(GuestAddress{arg(1)},32,bytes.size());require(arg(0) && capacity>=bytes.size(),122);memory.write(GuestAddress{arg(0)},bytes);return 0;
    }catch(const Error& error){return error.code;}catch(const MemoryFault&){return 87;}catch(const std::bad_alloc&){return 8;}
}
// GetOwnerModuleFrom{Tcp,Tcp6,Udp,Udp6}Entry（列, Class, pBuffer, pdwSize）與 GetOwnerModuleFromPidAndInfo（pid, pInfo, Class, pBuffer, pdwSize）：
// 只有 TCPIP_OWNER_MODULE_INFO_BASIC（0）；輸出 {pModuleName, pModulePath} 後接兩個 UTF-16 字串。PID 取自列的 dwOwningPid
// （MIB_TCPROW_OWNER_MODULE 位移 20、MIB_TCP6ROW_OWNER_MODULE 52、MIB_UDPROW_OWNER_MODULE 8、MIB_UDP6ROW_OWNER_MODULE 24）。
std::uint32_t WindowsServices::owner_module_call(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    try {
        const bool by_pid=symbol=="GetOwnerModuleFromPidAndInfo";const unsigned first=by_pid?2:1;
        const auto kind=static_cast<std::uint32_t>(arg(first));const auto buffer=arg(first+1),size_at=arg(first+2);
        require(kind==0 && size_at && (by_pid?arg(1)!=0:arg(0)!=0));
        std::uint32_t pid=0;
        if(by_pid)pid=static_cast<std::uint32_t>(arg(0));
        else {const unsigned offset=symbol=="GetOwnerModuleFromTcpEntry"?20U:symbol=="GetOwnerModuleFromTcp6Entry"?52U:symbol=="GetOwnerModuleFromUdpEntry"?8U:24U;
            pid=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+offset},32));}
        const auto capacity=memory.load(GuestAddress{size_at},32);memory.check(GuestAddress{size_at},4,Access::Write);
        require(pid!=0,1168);std::u16string path;if(const auto error=process_image_path(memory,pid,path))return error==1168?1168U:error;
        auto name=path;if(const auto slash=name.find_last_of(u"\\/");slash!=name.npos)name.erase(0,slash+1);
        const unsigned unit=x86_?4:8;const auto name_bytes=utf16_bytes(name),path_bytes=utf16_bytes(path);
        const auto total=2*unit+name_bytes.size()+path_bytes.size();require(total<=UINT32_MAX,8);
        memory.store(GuestAddress{size_at},32,total);require(buffer && capacity>=total,122);
        std::vector<std::uint8_t> bytes(total);const auto name_at=buffer+2*unit,path_at=name_at+name_bytes.size();
        put(bytes,0,name_at,unit);put(bytes,unit,path_at,unit);
        std::copy(name_bytes.begin(),name_bytes.end(),bytes.begin()+2*unit);std::copy(path_bytes.begin(),path_bytes.end(),bytes.begin()+static_cast<std::ptrdiff_t>(2*unit+name_bytes.size()));
        memory.write(GuestAddress{buffer},bytes);return 0;
    }catch(const Error& error){return error.code;}catch(const MemoryFault&){return 87;}catch(const std::bad_alloc&){return 8;}
}
// GetExtendedUdpTable／GetUdpTable／GetUdp6Table：與 TCP 表同一列舉範圍；UDP 沒有連線狀態，只列本機端點。
// 列格式：BASIC（位址、埠）、OWNER_PID（再加 PID）、OWNER_MODULE（再加建立時間、旗標與 16 個模組資訊欄位，表頭對齊 8）。
std::uint32_t WindowsServices::udp_table_call(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    try {
        require(arg(1));const bool extended=symbol=="GetExtendedUdpTable";
        const auto family=extended?static_cast<std::uint32_t>(arg(3)):symbol=="GetUdp6Table"?23U:2U,kind=extended?static_cast<std::uint32_t>(arg(4)):0U;
        require(family==2 || family==23);require(kind<=2 && (!extended || !static_cast<std::uint32_t>(arg(5))));
        const auto capacity=memory.load(GuestAddress{arg(1)},32);memory.check(GuestAddress{arg(1)},4,Access::Write);
        if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生 UDP 表查詢");
        std::vector<std::uint32_t> pids;const auto status=session_process_ids(pids);require(!status,status);std::vector<Row> rows;
#if defined(__APPLE__)
        each_session_socket(pids,process_id(),[&](std::uint32_t owner,const socket_fdinfo& info){
            if(info.psi.soi_kind!=SOCKINFO_IN || info.psi.soi_protocol!=IPPROTO_UDP)return;
            const auto& ip=info.psi.soi_proto.pri_in;const bool ipv6=info.psi.soi_family==AF_INET6;if((family==23)!=ipv6)return;
            Row row;row.pid=owner;row.ipv6=ipv6;row.local_port=static_cast<std::uint16_t>(ip.insi_lport);
            if(ipv6){std::memcpy(row.local.data(),&ip.insi_laddr.ina_6,16);row.local_scope=link_local_scope(row.local,ip);}
            else std::memcpy(row.local.data(),&ip.insi_laddr.ina_46.i46a_addr4,4);
            rows.push_back(row);require(rows.size()<=65536,8);
        });
#else
        return 50;
#endif
        if(static_cast<std::uint32_t>(arg(2)))std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return std::tuple{a.local,a.local_scope,static_cast<unsigned>(ntohs(a.local_port))}<std::tuple{b.local,b.local_scope,static_cast<unsigned>(ntohs(b.local_port))};});
        std::vector<Row> unique;for(const auto& row:rows)if(std::none_of(unique.begin(),unique.end(),[&](const auto& old){return old.pid==row.pid && old.local==row.local && old.local_port==row.local_port && old.local_scope==row.local_scope;}))unique.push_back(row);
        const bool ipv6=family==23;const unsigned header=kind==2?8:4;
        const unsigned stride=ipv6?(kind==0?24:kind==1?28:176):(kind==0?8:kind==1?12:160);
        std::vector<std::uint8_t> bytes(header+unique.size()*stride);put(bytes,0,unique.size());
        for(std::size_t i=0;i<unique.size();++i){
            const auto at=header+i*stride;const auto& row=unique[i];unsigned next;
            if(ipv6){std::copy(row.local.begin(),row.local.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));put(bytes,at+16,row.local_scope);put(bytes,at+20,row.local_port);next=24;}
            else {std::copy_n(row.local.begin(),4,bytes.begin()+static_cast<std::ptrdiff_t>(at));put(bytes,at+4,row.local_port);next=8;}
            if(kind>=1)put(bytes,at+next,row.pid);
        }
        memory.store(GuestAddress{arg(1)},32,bytes.size());require(arg(0) && capacity>=bytes.size(),122);memory.write(GuestAddress{arg(0)},bytes);return 0;
    }catch(const Error& error){return error.code;}catch(const MemoryFault&){return 87;}catch(const std::bad_alloc&){return 8;}
}
}
