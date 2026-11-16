// windows_network_config.cpp 的共用 IPv4 鄰居擷取與解析；不修改宿主路由表。
struct Ipv4Neighbor {std::uint32_t index{},address{},type{};std::vector<std::uint8_t> physical;};
std::uint32_t ipv4_neighbors(std::vector<Ipv4Neighbor>& result){
#if defined(__APPLE__)
    int mib[]{CTL_NET,PF_ROUTE,0,AF_INET,NET_RT_FLAGS,RTF_LLINFO};std::vector<std::uint8_t> bytes;bool captured=false;
    for(unsigned attempt=0;attempt<3;++attempt){
        std::size_t size=0;if(::sysctl(mib,6,nullptr,&size,nullptr,0))return 31;
        if(size>16*1024*1024)return 8;bytes.resize(size);if(!size){captured=true;break;}
        if(!::sysctl(mib,6,bytes.data(),&size,nullptr,0)){bytes.resize(size);captured=true;break;}
        if(errno!=ENOMEM)return 31;
    }
    if(!captured)return 1237;
    for(std::size_t at=0;at<bytes.size();){
        if(bytes.size()-at<sizeof(rt_msghdr))return 13;rt_msghdr header{};std::memcpy(&header,bytes.data()+at,sizeof(header));
        if(header.rtm_msglen<sizeof(header) || header.rtm_msglen>bytes.size()-at)return 13;
        const auto end=at+header.rtm_msglen;std::size_t cursor=at+sizeof(header);std::array<std::span<const std::uint8_t>,RTAX_MAX> addresses{};
        for(unsigned i=0;i<RTAX_MAX;++i)if(static_cast<unsigned>(header.rtm_addrs)&(1U<<i)){
            if(end-cursor<2)return 13;const auto size=bytes[cursor];const auto padded=size?(size+3U)&~3U:4U;
            if(padded>end-cursor)return 13;addresses[i]=std::span(bytes).subspan(cursor,size);cursor+=padded;
        }
        const auto ip=addresses[RTAX_DST],link=addresses[RTAX_GATEWAY];
        if(ip.size()>=8 && ip[1]==AF_INET && link.size()>=8 && link[1]==AF_LINK){
            const unsigned length=link[6],offset=8U+link[5];if(offset+length>link.size())return 13;
            Ipv4Neighbor item;item.index=header.rtm_index;std::memcpy(&item.address,ip.data()+4,4);item.type=length?((header.rtm_flags&RTF_STATIC)?4U:3U):2U;
            item.physical.assign(link.begin()+offset,link.begin()+offset+std::min<unsigned>(length,8));result.push_back(std::move(item));
        }
        at=end;
    }
    return 0;
#else
    (void)result;return 50;
#endif
}
std::uint32_t resolve_ipv4_neighbor(const NetworkConfiguration& config,std::uint32_t destination,std::uint32_t source,std::vector<std::uint8_t>& physical){
    const auto owns=[](const NetworkInterface& item,std::uint32_t address){return std::any_of(item.addresses.begin(),item.addresses.end(),[&](const auto& ip){return ip.bytes.size()>=8 && ip.bytes[0]==2 && std::memcmp(ip.bytes.data()+4,&address,4)==0;});};
    std::uint32_t index=0;
    if(source){const auto item=std::find_if(config.interfaces.begin(),config.interfaces.end(),[&](const auto& value){return owns(value,source);});if(item==config.interfaces.end())return 1168;index=item->index;}
    for(const auto& item:config.interfaces)if(owns(item,destination) && (!index || index==item.index)){
        if(item.physical.empty())return 67;physical=item.physical;return 0;
    }
    const auto host=ntohl(destination);if(!host || host==UINT32_MAX || (host>>28)==14 || (host>>24)==127)return 67;
    std::array<std::uint8_t,28> target{},origin{};target[0]=origin[0]=2;std::memcpy(target.data()+4,&destination,4);std::memcpy(origin.data()+4,&source,4);
    const auto route=query_route(config,0,index,source?std::span<const std::uint8_t>(origin):std::span<const std::uint8_t>{},target);
    if(route.status)return 67;
    // ARP 只能取得同一鏈路的 MAC，不可把遠端 IP 回成閘道的 MAC。
    if(std::any_of(route.row.begin()+48,route.row.begin()+52,[](auto byte){return byte!=0;}))return 67;
    std::memcpy(&index,route.row.data()+8,4);
    const auto lookup=[&]()->std::uint32_t{
        std::vector<Ipv4Neighbor> entries;const auto error=ipv4_neighbors(entries);if(error)return error;
        for(const auto& entry:entries)if(entry.index==index && entry.address==destination && entry.type!=2 && !entry.physical.empty()){physical=entry.physical;return 0;}return 67;
    };
    if(const auto error=lookup();error!=67)return error;
#if defined(__APPLE__)
    // 讓宿主網路堆疊執行真正的鄰居解析；不自行生成 MAC。
    const int descriptor=::socket(AF_INET,SOCK_DGRAM,0);if(descriptor<0)return 31;
    struct Close{int value;~Close(){::close(value);}} close{descriptor};
    if(::setsockopt(descriptor,IPPROTO_IP,IP_BOUND_IF,&index,sizeof(index)))return 67;
    sockaddr_in local{},remote{};local.sin_len=remote.sin_len=sizeof(sockaddr_in);local.sin_family=remote.sin_family=AF_INET;
    local.sin_addr.s_addr=source;remote.sin_addr.s_addr=destination;remote.sin_port=htons(9);
    if(source && ::bind(descriptor,reinterpret_cast<sockaddr*>(&local),sizeof(local)))return 1168;
    if(::sendto(descriptor,"",0,MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&remote),sizeof(remote))<0)return 67;
    for(unsigned attempt=0;attempt<20;++attempt){std::this_thread::sleep_for(std::chrono::milliseconds(50));if(const auto error=lookup();error!=67)return error;}return 67;
#else
    return 50;
#endif
}
