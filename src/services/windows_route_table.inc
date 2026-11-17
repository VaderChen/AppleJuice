// windows_network_config.cpp 的宿主路由表與鄰居快取讀取（sysctl PF_ROUTE）：GetIpForwardTable2、GetIpNetTable2 與 NotifyRouteChange 共用；不修改宿主路由表。
// - 路由：NET_RT_DUMP，略過鄰居項目（RTF_LLINFO）、核心複製的主機路由（RTF_WASCLONED）、未啟用、reject／blackhole 與不屬於快照介面的路由；
//   每筆以 encode_route_row 轉成 MIB_IPFORWARD_ROW2，與 GetBestRoute2 同一格式。
// - 鄰居：NET_RT_FLAGS＋RTF_LLINFO（IPv4 ARP、IPv6 NDP）；IPv6 狀態與路由器旗標以 SIOCGNBRINFO_IN6 讀宿主 ND 快取，IPv4 依連結位址與存活時間判斷。
struct RouteMessage {rt_msghdr header{};std::array<sockaddr_storage,RTAX_MAX> addresses{};};
#if defined(__APPLE__)
// in6_nbrinfo（netinet6/nd6.h 需要核心私有結構，無法直接引入）：ndp(8) 使用的 SIOCGNBRINFO_IN6 公開介面。
struct NeighborInfo {char ifname[IFNAMSIZ];in6_addr addr;long asked;int isrouter;int state;int expire;};
constexpr unsigned long neighbor_info_request=_IOWR('i',78,NeighborInfo);
#endif
std::uint32_t route_messages(int family,int operation,int flags,std::vector<RouteMessage>& result){
#if defined(__APPLE__)
    int mib[]{CTL_NET,PF_ROUTE,0,family,operation,flags};std::vector<std::uint8_t> bytes;bool captured=false;
    for(unsigned attempt=0;attempt<3;++attempt){
        std::size_t size=0;if(::sysctl(mib,6,nullptr,&size,nullptr,0))return 31;
        if(size>16*1024*1024)return 8;bytes.resize(size+size/8+1024);size=bytes.size();
        if(!::sysctl(mib,6,bytes.data(),&size,nullptr,0)){bytes.resize(size);captured=true;break;}
        if(errno!=ENOMEM)return 31;
    }
    if(!captured)return 1237;
    for(std::size_t at=0;at<bytes.size();){
        if(bytes.size()-at<sizeof(rt_msghdr))return 13;RouteMessage message;std::memcpy(&message.header,bytes.data()+at,sizeof(rt_msghdr));
        const auto& header=message.header;if(header.rtm_msglen<sizeof(rt_msghdr) || header.rtm_msglen>bytes.size()-at)return 13;
        const auto end=at+header.rtm_msglen;std::size_t cursor=at+sizeof(rt_msghdr);
        for(unsigned i=0;i<RTAX_MAX;++i)if(static_cast<unsigned>(header.rtm_addrs)&(1U<<i)){
            if(end-cursor<1)return 13;const auto size=bytes[cursor];const auto padded=size?(size+3U)&~3U:4U;
            if(padded>end-cursor)return 13;std::memcpy(&message.addresses[i],bytes.data()+cursor,std::min<std::size_t>(size,sizeof(sockaddr_storage)));cursor+=padded;
        }
        if(header.rtm_version==RTM_VERSION){result.push_back(message);if(result.size()>65536)return 8;}
        at=end;
    }
    return 0;
#else
    (void)family;(void)operation;(void)flags;(void)result;return 50;
#endif
}
#if defined(__APPLE__)
bool listed_route(const RouteMessage& message){
    const auto flags=message.header.rtm_flags;
    return (flags&RTF_UP) && !(flags&(RTF_LLINFO|RTF_WASCLONED|RTF_REJECT|RTF_BLACKHOLE)) && (static_cast<unsigned>(message.header.rtm_addrs)&RTA_DST);
}
#endif
// family：AF_UNSPEC（0）、AF_INET（2）、AF_INET6（23）；依位址家族、前置位址、前置長度與介面索引排序，相同列只留一筆。
std::uint32_t route_table_rows(const NetworkConfiguration& config,std::uint16_t family,std::vector<std::array<std::uint8_t,104>>& rows){
#if defined(__APPLE__)
    for(const auto [windows,native]:std::array<std::pair<std::uint16_t,int>,2>{{{2,AF_INET},{23,AF_INET6}}}){
        if(family && family!=windows)continue;
        std::vector<RouteMessage> messages;if(const auto error=route_messages(native,NET_RT_DUMP,0,messages))return error;
        for(const auto& message:messages){
            if(!listed_route(message) || message.addresses[RTAX_DST].ss_family!=native)continue;
            const auto interface=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& item){return item.index==message.header.rtm_index;});
            if(interface==config.interfaces.end())continue;
            std::array<std::uint8_t,104> row{};if(encode_route_row(*interface,message.header,message.addresses,static_cast<std::uint8_t>(windows),row))continue;
            if(std::find(rows.begin(),rows.end(),row)==rows.end())rows.push_back(row);
        }
    }
    const auto word=[](const std::array<std::uint8_t,104>& row,unsigned at){return static_cast<std::uint32_t>(row[at])|static_cast<std::uint32_t>(row[at+1])<<8|static_cast<std::uint32_t>(row[at+2])<<16|static_cast<std::uint32_t>(row[at+3])<<24;};
    std::stable_sort(rows.begin(),rows.end(),[&](const auto& a,const auto& b){
        if(a[12]!=b[12])return a[12]<b[12];
        if(!std::equal(a.begin()+16,a.begin()+40,b.begin()+16))return std::lexicographical_compare(a.begin()+16,a.begin()+40,b.begin()+16,b.begin()+40);
        if(a[40]!=b[40])return a[40]<b[40];
        return word(a,8)<word(b,8);});
    return 0;
#else
    (void)config;(void)family;(void)rows;return 50;
#endif
}
// NotifyRouteChange 的比較快照：只取 IPv4 路由的穩定欄位（目的、遮罩、閘道、介面、旗標），不含使用計數與存活時間。
std::optional<std::vector<std::uint8_t>> ipv4_route_snapshot(){
#if defined(__APPLE__)
    std::vector<RouteMessage> messages;if(route_messages(AF_INET,NET_RT_DUMP,0,messages))return std::nullopt;
    std::set<std::vector<std::uint8_t>> entries;
    for(const auto& message:messages){
        if(!listed_route(message))continue;
        std::vector<std::uint8_t> entry;const auto flags=static_cast<std::uint32_t>(message.header.rtm_flags)&(RTF_UP|RTF_GATEWAY|RTF_HOST|RTF_STATIC);
        for(unsigned i=0;i<4;++i)entry.push_back(static_cast<std::uint8_t>(flags>>(8*i)));
        entry.push_back(static_cast<std::uint8_t>(message.header.rtm_index));entry.push_back(static_cast<std::uint8_t>(message.header.rtm_index>>8));
        for(const auto slot:{RTAX_DST,RTAX_NETMASK,RTAX_GATEWAY}){
            const auto* bytes=reinterpret_cast<const std::uint8_t*>(&message.addresses[static_cast<std::size_t>(slot)]);const auto length=std::min<unsigned>(bytes[0],sizeof(sockaddr_storage));
            entry.push_back(static_cast<std::uint8_t>(length));entry.insert(entry.end(),bytes,bytes+length);
        }
        entries.insert(std::move(entry));
    }
    std::vector<std::uint8_t> result;for(const auto& e:entries){result.push_back(static_cast<std::uint8_t>(e.size()));result.push_back(static_cast<std::uint8_t>(e.size()>>8));result.insert(result.end(),e.begin(),e.end());}
    return result;
#else
    return std::nullopt;
#endif
}
// MIB_IPNET_ROW2（88 位元組）：Address（SOCKADDR_INET）、InterfaceIndex、InterfaceLuid、PhysicalAddress[32]、長度、State、旗標、ReachabilityTime。
std::uint32_t neighbor_table_rows(const NetworkConfiguration& config,std::uint16_t family,std::vector<std::array<std::uint8_t,88>>& rows){
#if defined(__APPLE__)
    for(const auto [windows,native]:std::array<std::pair<std::uint16_t,int>,2>{{{2,AF_INET},{23,AF_INET6}}}){
        if(family && family!=windows)continue;
        std::vector<RouteMessage> messages;if(const auto error=route_messages(native,NET_RT_FLAGS,RTF_LLINFO,messages))return error;
        const int probe=native==AF_INET6?::socket(AF_INET6,SOCK_DGRAM,0):-1;struct Close{int fd;~Close(){if(fd>=0)::close(fd);}} close{probe};
        for(const auto& message:messages){
            const auto& header=message.header;const auto& target=message.addresses[RTAX_DST];
            const auto* link=reinterpret_cast<const std::uint8_t*>(&message.addresses[RTAX_GATEWAY]);
            if(target.ss_family!=native || link[1]!=AF_LINK)continue;
            const auto interface=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& item){return item.index==header.rtm_index;});
            if(interface==config.interfaces.end())continue;
            std::array<std::uint8_t,88> row{};const auto put=[&](unsigned at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)row[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
            auto address=socket_bytes(reinterpret_cast<const sockaddr*>(&target));clear_embedded_scope(address);
            // 連結本機鄰居沒有帶 scope 時以所在介面為 scope。
            if(windows==23 && address.size()>=28 && !address[24] && !address[25] && !address[26] && !address[27] && address[8]==0xfe && (address[9]&0xc0)==0x80)
                for(unsigned i=0;i<4;++i)address[24+i]=static_cast<std::uint8_t>(interface->index>>(8*i));
            std::copy(address.begin(),address.end(),row.begin());
            put(28,interface->index,4);put(32,(std::uint64_t{interface->type}<<48)|(std::uint64_t{interface->index}<<24),8);
            const unsigned name_length=link[5],length=link[6];const auto link_size=std::min<unsigned>(link[0],sizeof(sockaddr_storage));
            const unsigned physical=8U+name_length+length<=link_size?std::min(length,32U):0U;
            std::copy_n(link+8+name_length,physical,row.begin()+40);put(72,physical,4);
            const auto expire=header.rtm_rmx.rmx_expire;const auto now=std::time(nullptr);
            // NL_NEIGHBOR_STATE：Unreachable 0、Incomplete 1、Probe 2、Delay 3、Stale 4、Reachable 5、Permanent 6。
            std::uint32_t state=!physical?1U:(header.rtm_flags&RTF_STATIC) || !expire?6U:expire>now?5U:4U;bool router=false;
            if(windows==23 && probe>=0 && physical && !(header.rtm_flags&RTF_STATIC)){
                NeighborInfo info{};char name[IF_NAMESIZE]{};
                if(::if_indextoname(header.rtm_index,name)){
                    std::memcpy(info.ifname,name,std::min(sizeof(info.ifname),sizeof(name)));info.addr=reinterpret_cast<const sockaddr_in6&>(target).sin6_addr;
                    if(!::ioctl(probe,neighbor_info_request,&info)){
                        constexpr std::array<std::uint32_t,5> states{1,5,4,3,2}; // ND6_LLINFO_INCOMPLETE、REACHABLE、STALE、DELAY、PROBE
                        if(info.state>=0 && static_cast<std::size_t>(info.state)<states.size())state=states[static_cast<std::size_t>(info.state)];router=info.isrouter!=0;
                    }
                }
            }
            put(76,state,4);row[80]=static_cast<std::uint8_t>((router?1U:0U)|(state==0?2U:0U));
            if(state==5 && expire>now)put(84,static_cast<std::uint64_t>(std::min<std::int64_t>(static_cast<std::int64_t>(expire-now)*1000,UINT32_MAX)),4);
            rows.push_back(row);if(rows.size()>65536)return 8;
        }
    }
    return 0;
#else
    (void)config;(void)family;(void)rows;return 50;
#endif
}
// MIB_IPINTERFACE_ROW（168 位元組）：GetIpInterfaceEntry／Table 與 NotifyIpInterfaceChange 共用。介面有該家族位址才有對應的 IP 介面
// （與 GetAdaptersAddresses 的 Ipv4Enabled／Ipv6IfIndex 判斷相同）；轉送、弱主機接收與 DAD 次數取宿主 sysctl，卸載能力取 SIOCGIFCAP，
// ND 計時沒有公開的宿主查詢，使用 RFC 4861 預設值；計量依 Windows 10 自動計量的速度分級，回送介面 75。
struct IpInterfaceHost {
    bool forward4{},forward6{},weak_receive{true};int dad6{1};
    static IpInterfaceHost read(){
        IpInterfaceHost host;
#if defined(__APPLE__)
        const auto flag=[](const char* key,int fallback){int value=fallback;std::size_t size=sizeof(value);if(::sysctlbyname(key,&value,&size,nullptr,0))value=fallback;return value;};
        host.forward4=flag("net.inet.ip.forwarding",0)!=0;host.forward6=flag("net.inet6.ip6.forwarding",0)!=0;
        host.weak_receive=flag("net.inet.ip.check_interface",0)==0;host.dad6=flag("net.inet6.ip6.dad_count",1);
#endif
        return host;
    }
};
bool ip_interface_bound(const NetworkInterface& item,std::uint16_t family){
    return std::any_of(item.addresses.begin(),item.addresses.end(),[&](const auto& a){return a.bytes.size()>=2 && a.bytes[0]==family;});
}
std::array<std::uint8_t,168> ip_interface_row(const NetworkInterface& item,std::uint16_t family,const IpInterfaceHost& host){
    std::array<std::uint8_t,168> row{};const auto put=[&](unsigned at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)row[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
    const bool v6=family==23,loopback=item.type==24;
    put(0,family,2);put(8,interface_luid(item),8);put(16,item.index,4);put(20,65535,4);
    if(v6)for(const auto& address:item.addresses)if(address.bytes.size()>=28 && address.bytes[0]==23 && address.bytes[8]==0xfe && (address.bytes[9]&0xc0)==0x80){std::copy_n(address.bytes.begin()+16,8,row.begin()+24);break;}
    put(32,200,4);put(36,600,4);row[41]=v6?host.forward6:host.forward4;row[42]=host.weak_receive;row[43]=host.weak_receive;row[44]=1;row[45]=1;
    put(52,v6?1U:2U,4);put(56,v6?static_cast<std::uint32_t>(std::max(host.dad6,0)):3U,4);put(60,30000,4);put(64,1000,4);put(68,600000,4);
    put(72,v6?2U:1U,4);put(76,v6?0U:6000U,4);
    for(unsigned level=1;level<14;++level)put(80+level*4,level<=3?item.index:1U,4);
    std::uint32_t metric=65;
    if(loopback)metric=75;
    else {constexpr std::array<std::pair<std::uint64_t,std::uint32_t>,12> levels{{{200000000000ULL,5},{80000000000ULL,10},{20000000000ULL,15},{10000000000ULL,20},{4000000000ULL,25},
            {2000000000ULL,30},{500000000ULL,35},{200000000ULL,40},{80000000ULL,45},{20000000ULL,50},{4000000ULL,55},{500000ULL,60}}};
        for(const auto& [threshold,value]:levels)if(item.speed>=threshold){metric=value;break;}}
    put(144,v6?64U:0U,4);put(148,metric,4);put(152,item.mtu,4);row[156]=item.status==1;row[158]=!loopback;row[159]=!loopback;put(160,30000,4);
#if defined(__APPLE__)
    // NL_INTERFACE_OFFLOAD_ROD 位元：0 NlChecksum、2 TlDatagramChecksum、3 TlStreamChecksum、6 TlLargeSendOffload。
    if(const int s=::socket(AF_INET,SOCK_DGRAM,0);s>=0){
        ifreq request{};std::strncpy(request.ifr_name,item.name.c_str(),sizeof(request.ifr_name)-1);const int cap=::ioctl(s,SIOCGIFCAP,&request)?0:request.ifr_curcap;::close(s);
        row[164]=static_cast<std::uint8_t>(((cap&IFCAP_TXCSUM)?0x0dU:0U)|((cap&(v6?IFCAP_TSO6:IFCAP_TSO4))?0x40U:0U));
        row[165]=static_cast<std::uint8_t>((cap&IFCAP_RXCSUM)?0x0dU:0U);
    }
#endif
    return row;
}
// IPv4 列在前、IPv6 列在後，各自依介面快照順序。
void ip_interface_rows(const NetworkConfiguration& config,std::uint16_t family,std::vector<std::array<std::uint8_t,168>>& rows){
    const auto host=IpInterfaceHost::read();
    for(const std::uint16_t f:{std::uint16_t{2},std::uint16_t{23}}){if(family && family!=f)continue;
        for(const auto& item:config.interfaces)if(ip_interface_bound(item,f))rows.push_back(ip_interface_row(item,f,host));}
}
// 變更通知的比較快照：鍵 → 列。IP 介面以家族＋LUID、路由以 LUID＋前置＋下一躍點、單播位址以位址＋LUID 為鍵。
std::uint32_t mib_snapshot(const NetworkConfiguration& config,std::uint32_t kind,std::uint16_t family,std::map<std::vector<std::uint8_t>,std::vector<std::uint8_t>>& result){
    result.clear();
    const auto add=[&](std::span<const std::uint8_t> row,std::initializer_list<std::pair<unsigned,unsigned>> ranges){
        std::vector<std::uint8_t> key;for(const auto& [at,size]:ranges)key.insert(key.end(),row.begin()+at,row.begin()+at+size);
        result.emplace(std::move(key),std::vector<std::uint8_t>(row.begin(),row.end()));};
    if(kind==0){std::vector<std::array<std::uint8_t,168>> rows;ip_interface_rows(config,family,rows);for(const auto& row:rows)add(row,{{0,2},{8,8}});return 0;}
    if(kind==1){std::vector<std::array<std::uint8_t,104>> rows;if(const auto error=route_table_rows(config,family,rows))return error;for(const auto& row:rows)add(row,{{0,8},{12,60}});return 0;}
    for(const auto& item:config.interfaces)for(const auto& address:item.addresses){
        if((address.bytes.size()!=16 && address.bytes.size()!=28) || (family && address.bytes[0]!=family))continue;
        const auto row=unicast_row(item,address);add(row,{{0,28},{32,8}});
    }
    return 0;
}
// 參數變更只比較穩定欄位：路由與單播位址的有效／偏好存留時間會隨時間遞減，不視為變更。
std::vector<std::uint8_t> mib_stable(std::uint32_t kind,std::vector<std::uint8_t> row){
    const auto clear=[&](unsigned at,unsigned size){if(row.size()>=at+size)std::fill(row.begin()+at,row.begin()+at+size,0);};
    if(kind==1)clear(76,8);else if(kind==2)clear(52,8);
    return row;
}
