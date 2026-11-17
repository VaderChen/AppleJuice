// iphlpapi 第二代表格與排序入口，由 iphelper_extras.inc 的 invoke_iphelper_extras 引入（同一翻譯單元），共用同一份介面快照：
// - GetIfEntry2Ex：MibIfEntryNormal（0）同 GetIfEntry2；MibIfEntryNormalWithoutStatistics（2）不要求統計可用，計數欄位為 0。
// - GetIpInterfaceEntry／GetIpInterfaceTable：MIB_IPINTERFACE_ROW（168 位元組）。介面有該家族位址才有對應的 IP 介面（與 GetAdaptersAddresses 的
//   Ipv4Enabled／Ipv6IfIndex 判斷相同）；轉送開關取宿主 net.inet.ip.forwarding／net.inet6.ip6.forwarding，弱主機接收取 net.inet.ip.check_interface，
//   卸載能力取 SIOCGIFCAP；ND 計時沒有公開的宿主查詢，使用 RFC 4861 預設值。
// - GetIpForwardTable2／GetIpNetTable2：宿主路由表與鄰居快取（windows_route_table.inc），表格由 FreeMibTable 釋放；沒有項目回 ERROR_NOT_FOUND。
// - GetTcpTable2／GetTcp6Table／GetTcp6Table2 交給 tcp_table_call；GetOwnerModuleFrom*Entry／PidAndInfo 交給 owner_module_call。
// - CreateSortedAddressPairs：依 RFC 3484 目的位址選擇規則（1 不可用者最後、2 範圍相符、5 標籤相符、6 優先序、8 範圍較小、9 最長相符前置）排序，
//   前置原則表為 Windows 預設值；來源位址由宿主路由選出，IPv4 以 IPv4 對應 IPv6 位址表示。
if(name=="GetTcpTable2" || name=="GetTcp6Table" || name=="GetTcp6Table2")return finish(tcp_table_call(memory,arg,name));
if(name.starts_with("GetOwnerModuleFrom"))return finish(owner_module_call(memory,arg,name));
if(name=="GetIfEntry2Ex"){
    const auto level=d(0);const auto row=arg(1);if(!row || (level!=0 && level!=2))return finish(87);
    memory.check(GuestAddress{row},1352,Access::Write);const auto luid=memory.load(GuestAddress{row},64),index=memory.load(GuestAddress{row+8},32);if(!luid && !index)return finish(87);
    auto& c=*runtime_.network.configuration;if(!refresh_configuration(c,runtime_.network.external_io,level==0))return finish(232);
    const auto item=std::find_if(c.interfaces.begin(),c.interfaces.end(),[&](const auto& p){return luid?interface_luid(p)==luid:p.index==index;});
    if(item==c.interfaces.end())return finish(2);
    if(level==0){if(!item->statistics_available)return finish(50);memory.write(GuestAddress{row},interface_row(*item));return finish(0);}
    auto copy=*item;copy.statistics={};memory.write(GuestAddress{row},interface_row(copy));return finish(0);
}
// 由 FreeMibTable 釋放的表格：header 位元組（筆數）後接各列。
const auto publish_table=[&](std::uint64_t output,unsigned header,std::size_t count,std::span<const std::uint8_t> rows)->std::uint64_t{
    const auto base=allocate_heap(memory,header+rows.size());if(!base)return 8;
    std::vector<std::uint8_t> bytes(header+rows.size());for(unsigned i=0;i<4;++i)bytes[i]=static_cast<std::uint8_t>(count>>(8*i));
    std::copy(rows.begin(),rows.end(),bytes.begin()+header);memory.write(GuestAddress{base},bytes);runtime_.network.addresses.insert(base);
    memory.store(GuestAddress{output},static_cast<std::uint8_t>(unit*8),base);return 0;
};
if(name=="GetIpInterfaceEntry" || name=="GetIpInterfaceTable"){
    const bool table=name=="GetIpInterfaceTable";const auto output=arg(table?1:0);if(!output)return finish(87);
    memory.check(GuestAddress{output},table?unit:168U,Access::Write);
    const auto family=table?static_cast<std::uint16_t>(arg(0)):static_cast<std::uint16_t>(memory.load(GuestAddress{output},16));
    if((family!=2 && family!=23 && (!table || family!=0)))return finish(87);
    if(table)memory.store(GuestAddress{output},static_cast<std::uint8_t>(unit*8),0);
    std::uint64_t luid=0;std::uint32_t index=0;
    if(!table){luid=memory.load(GuestAddress{output+8},64);index=static_cast<std::uint32_t>(memory.load(GuestAddress{output+16},32));if(!luid && !index)return finish(87);}
    auto* c=config();if(!c)return finish(232);
    if(!table){
        const auto item=std::find_if(c->interfaces.begin(),c->interfaces.end(),[&](const auto& p){return luid?interface_luid(p)==luid:p.index==index;});
        if(item==c->interfaces.end())return finish(2);if(!ip_interface_bound(*item,family))return finish(1168);
        memory.write(GuestAddress{output},ip_interface_row(*item,family,IpInterfaceHost::read()));return finish(0);
    }
    std::vector<std::array<std::uint8_t,168>> items;ip_interface_rows(*c,family,items);
    std::vector<std::uint8_t> rows;for(const auto& row:items)rows.insert(rows.end(),row.begin(),row.end());
    if(items.empty())return finish(1168);return finish(publish_table(output,8,items.size(),rows));
}
if(name=="GetIpForwardTable2" || name=="GetIpNetTable2"){
    const auto family=static_cast<std::uint16_t>(arg(0));const auto output=arg(1);
    if(!output || (family!=0 && family!=2 && family!=23))return finish(87);
    memory.check(GuestAddress{output},unit,Access::Write);memory.store(GuestAddress{output},static_cast<std::uint8_t>(unit*8),0);
    if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生路由表查詢");
    auto* c=config();if(!c)return finish(232);
    std::vector<std::uint8_t> rows;std::size_t count=0;
    if(name=="GetIpForwardTable2"){std::vector<std::array<std::uint8_t,104>> items;if(const auto error=route_table_rows(*c,family,items))return finish(error);
        for(const auto& row:items)rows.insert(rows.end(),row.begin(),row.end());count=items.size();}
    else {std::vector<std::array<std::uint8_t,88>> items;if(const auto error=neighbor_table_rows(*c,family,items))return finish(error);
        for(const auto& row:items)rows.insert(rows.end(),row.begin(),row.end());count=items.size();}
    if(!count)return finish(1168);return finish(publish_table(output,8,count,rows));
}
if(name=="NotifyIpInterfaceChange" || name=="NotifyRouteChange2" || name=="NotifyUnicastIpAddressChange"){
    // (Family, Callback, CallerContext, InitialNotification, NotificationHandle*)：註冊當下的列作為基準；InitialNotification 時先送一次 Row＝NULL、MibInitialNotification（3）。
    const auto family=static_cast<std::uint16_t>(arg(0));const auto callback=arg(1),context=arg(2),handle_at=arg(4);const bool initial=(arg(3)&0xff)!=0;
    if((family!=0 && family!=2 && family!=23) || !callback || !handle_at)return finish(87);
    memory.check(GuestAddress{handle_at},unit,Access::Write);memory.check(GuestAddress{callback},1,Access::Execute);
    if(!runtime_.network.external_io)throw HostCallFault("compare 不支援原生網路變更通知");
    if(mib_notifications_.size()>=256)return finish(8);
    auto* c=config();if(!c)return finish(232);
    MibNotification n;n.kind=name=="NotifyIpInterfaceChange"?0U:name=="NotifyRouteChange2"?1U:2U;n.family=family;n.callback=callback;n.context=context;
    if(const auto error=mib_snapshot(*c,n.kind,family,n.rows))return finish(error);
    if(initial)n.pending.push_back({3,{}});
    const auto handle=next_mib_notification_++;mib_notifications_.emplace(handle,std::move(n));
    memory.store(GuestAddress{handle_at},static_cast<std::uint8_t>(unit*8),handle);return finish(0);
}
if(name=="CancelMibChangeNotify2"){
    // 執行中的回呼由其他執行緒取消時等它返回；在回呼內取消則立即返回，回呼結束後移除註冊。
    if(!arg(0))return finish(87);
    const auto found=mib_notifications_.find(arg(0));if(found==mib_notifications_.end() || (found->second.closing && !found->second.waiter))return finish(6);
    auto& n=found->second;
    if(n.work){
        const auto work=registered_waits_.find(n.work);
        if(work!=registered_waits_.end() && work->second.active){
            n.closing=true;n.pending.clear();
            if(work->second.worker==threads_.current){n.waiter=false;return finish(0);}
            n.waiter=true;auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;
            thread.wait={};thread.wait.kind=ThreadWaitKind::RegisteredWaitCompletion;thread.wait.handles={n.work};schedule(state,memory);return std::nullopt;
        }
        // 尚未開始的回呼直接撤銷。
        const auto token=n.work;if(n.payload)(void)free_heap(memory,n.payload,state);n.payload=0;n.work=0;
        if(work!=registered_waits_.end())release_registered_wait(token);
    }
    mib_notifications_.erase(found);threads_.items.at(threads_.current).wait={};return finish(0);
}
if(name=="NotifyStableUnicastIpAddressTable"){
    // (Family, Table**, CallerCallback, CallerContext, NotificationHandle*)：宿主位址沒有暫定（DAD 中）狀態可查，表格立即視為穩定，
    // 直接回傳 GetUnicastIpAddressTable 的結果、NotificationHandle 為 NULL，不呼叫回呼。
    const auto family=static_cast<std::uint16_t>(arg(0));const auto table_at=arg(1),handle_at=arg(4);
    if((family!=0 && family!=2 && family!=23) || !table_at || !arg(2) || !handle_at)return finish(87);
    memory.check(GuestAddress{handle_at},unit,Access::Write);memory.store(GuestAddress{handle_at},static_cast<std::uint8_t>(unit*8),0);
    return finish(static_cast<std::uint32_t>(nested_service(state,memory,"iphlpapi.dll","GetUnicastIpAddressTable",{family,table_at})));
}
if(name=="GetRTTAndHopCount"){
    // (DestIpAddress, HopCount*, MaxHops, RTT*)：以遞增 TTL 送 ICMP echo（宿主非特權 ICMP datagram socket）；途中路由器回 Time Exceeded（11）
    // 或逾時就加一，收到目的地的 echo reply 時寫入躍點數與往返毫秒並回 TRUE；目的地不可達（3）或超過 MaxHops 回 FALSE。每個躍點最多等 1 秒。
    const auto destination=d(0),max_hops=d(2);const auto hops_at=arg(1),rtt_at=arg(3);
    if(!hops_at || !rtt_at || !max_hops)return finish(0);
    memory.check(GuestAddress{hops_at},4,Access::Write);memory.check(GuestAddress{rtt_at},4,Access::Write);
    if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生 ICMP 探測");
#if defined(__APPLE__)
    const int fd=::socket(AF_INET,SOCK_DGRAM,IPPROTO_ICMP);if(fd<0)return finish(0);
    struct Close{int fd;~Close(){::close(fd);}} close_socket{fd};
    sockaddr_in target{};target.sin_len=sizeof target;target.sin_family=AF_INET;target.sin_addr.s_addr=destination;
    static std::uint16_t sequence=0x5000;const auto identifier=static_cast<std::uint16_t>(process_id());
    for(std::uint32_t ttl=1;ttl<=std::min<std::uint32_t>(max_hops,255);++ttl){
        const int hop_limit=static_cast<int>(ttl);if(::setsockopt(fd,IPPROTO_IP,IP_TTL,&hop_limit,sizeof hop_limit))return finish(0);
        const auto serial=++sequence;std::array<std::uint8_t,40> packet{};packet[0]=8;
        packet[4]=static_cast<std::uint8_t>(identifier>>8);packet[5]=static_cast<std::uint8_t>(identifier);packet[6]=static_cast<std::uint8_t>(serial>>8);packet[7]=static_cast<std::uint8_t>(serial);
        std::uint32_t sum=0;for(std::size_t i=0;i+1<packet.size();i+=2)sum+=static_cast<std::uint32_t>(packet[i]<<8|packet[i+1]);while(sum>>16)sum=(sum&0xffffU)+(sum>>16);
        sum=~sum&0xffffU;packet[2]=static_cast<std::uint8_t>(sum>>8);packet[3]=static_cast<std::uint8_t>(sum);
        const auto started=std::chrono::steady_clock::now();
        if(::sendto(fd,packet.data(),packet.size(),0,reinterpret_cast<const sockaddr*>(&target),sizeof target)<0)return finish(0);
        for(;;){
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();if(elapsed>=1000)break;
            pollfd waiter{fd,POLLIN,0};if(::poll(&waiter,1,static_cast<int>(1000-elapsed))<=0)break;
            std::array<std::uint8_t,1500> reply{};sockaddr_in from{};socklen_t from_size=sizeof from;
            const auto count=::recvfrom(fd,reply.data(),reply.size(),0,reinterpret_cast<sockaddr*>(&from),&from_size);if(count<8)continue;
            const std::size_t skip=(reply[0]>>4)==4?static_cast<std::size_t>(reply[0]&15U)*4:0;if(skip+8>static_cast<std::size_t>(count))continue;
            const auto type=reply[skip];
            // 序號比對：echo reply 在 ICMP 標頭；錯誤訊息內含原 IP 標頭與原 ICMP 前 8 位元組。
            const auto ours=[&](std::size_t at){return at+8<=static_cast<std::size_t>(count) && reply[at+6]==packet[6] && reply[at+7]==packet[7];};
            if(type==0 && ours(skip) && from.sin_addr.s_addr==destination){
                const auto rtt=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
                memory.store(GuestAddress{hops_at},32,ttl);memory.store(GuestAddress{rtt_at},32,static_cast<std::uint32_t>(rtt));return finish(1);
            }
            if(type==11 || type==3){
                const auto inner=skip+8;if(inner>=static_cast<std::size_t>(count))continue;const auto original=inner+static_cast<std::size_t>(reply[inner]&15U)*4;
                if(!ours(original) || reply[original]!=8)continue;
                if(type==3)return finish(0);
                break;
            }
        }
    }
    return finish(0);
#else
    return finish(0);
#endif
}
if(name=="CreateSortedAddressPairs"){
    // (SourceAddressList, SourceAddressCount, DestinationAddressList, DestinationAddressCount, AddressSortOptions, SortedAddressPairList, SortedAddressPairCount)
    const auto destinations=arg(2),list_out=arg(5),count_out=arg(6);const auto count=d(3);
    if(arg(0) || d(1) || !destinations || !count || count>500 || d(4) || !list_out || !count_out)return finish(87);
    memory.check(GuestAddress{list_out},unit,Access::Write);memory.check(GuestAddress{count_out},4,Access::Write);
    auto* c=config();if(!c)return finish(232);
    struct Pair {std::array<std::uint8_t,28> source{},destination{};bool usable{};std::size_t order{};};
    std::vector<Pair> pairs(count);
    const auto mapped=[](const std::array<std::uint8_t,28>& a){return std::all_of(a.begin()+8,a.begin()+18,[](auto b){return b==0;}) && a[18]==0xff && a[19]==0xff;};
    for(std::uint32_t i=0;i<count;++i){
        auto& pair=pairs[i];pair.order=i;memory.read(GuestAddress{destinations+28ULL*i},pair.destination);
        if(pair.destination[0]!=23 || pair.destination[1])return finish(87);
        pair.source[0]=23;
        std::array<std::uint8_t,28> target{};
        if(mapped(pair.destination)){target[0]=2;std::copy_n(pair.destination.begin()+20,4,target.begin()+4);}else target=pair.destination;
        if(!runtime_.network.external_io)continue;
        const auto route=query_route(*c,0,0,{},target);if(route.status)continue;
        pair.usable=true;
        if(route.source[0]==2){std::fill(pair.source.begin()+8,pair.source.begin()+28,0);pair.source[18]=pair.source[19]=0xff;std::copy_n(route.source.begin()+4,4,pair.source.begin()+20);}
        else std::copy(route.source.begin(),route.source.end(),pair.source.begin());
    }
    // 位址 16 位元組在 SOCKADDR_IN6 的位移 8。
    const auto address=[](const std::array<std::uint8_t,28>& a){std::array<std::uint8_t,16> v{};std::copy_n(a.begin()+8,16,v.begin());return v;};
    const auto scope=[&](const std::array<std::uint8_t,28>& a)->unsigned{
        const auto v=address(a);
        if(mapped(a)){if(v[12]==127 || (v[12]==169 && v[13]==254))return 2;if(v[12]==10 || (v[12]==172 && (v[13]&0xf0)==16) || (v[12]==192 && v[13]==168))return 5;return 14;}
        if(v[0]==0xff)return v[1]&15U;
        if(v[0]==0xfe && (v[1]&0xc0)==0x80)return 2;if(v[0]==0xfe && (v[1]&0xc0)==0xc0)return 5;
        if(std::all_of(v.begin(),v.begin()+15,[](auto b){return b==0;}) && v[15]==1)return 2;
        return 14;};
    // Windows 預設前置原則表：（前置位元組、長度、優先序、標籤），取最長相符。
    struct Policy {std::array<std::uint8_t,16> prefix;unsigned length,precedence,label;};
    static const std::array<Policy,9> policies{{
        {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},128,50,0},{{},0,40,1},{{0,0,0,0,0,0,0,0,0,0,0xff,0xff},96,35,4},{{0x20,0x02},16,30,2},{{0x20,0x01},32,5,5},
        {{0xfc},7,3,13},{{0xfe,0xc0},10,1,11},{{0x3f,0xfe},16,1,12},{{},96,1,3}}};
    const auto matches=[](const std::array<std::uint8_t,16>& v,const std::array<std::uint8_t,16>& prefix,unsigned length){
        for(unsigned bit=0;bit<length;++bit){const auto mask=static_cast<std::uint8_t>(0x80U>>(bit%8));if((v[bit/8]&mask)!=(prefix[bit/8]&mask))return false;}return true;};
    const auto policy=[&](const std::array<std::uint8_t,28>& a)->const Policy&{
        const auto v=address(a);const Policy* best=nullptr;for(const auto& p:policies)if(matches(v,p.prefix,p.length) && (!best || p.length>best->length))best=&p;return *best;};
    const auto common=[&](const std::array<std::uint8_t,28>& a,const std::array<std::uint8_t,28>& b){
        const auto x=address(a),y=address(b);unsigned length=0;for(unsigned bit=0;bit<128;++bit){const auto mask=static_cast<std::uint8_t>(0x80U>>(bit%8));if((x[bit/8]&mask)!=(y[bit/8]&mask))break;++length;}return length;};
    std::stable_sort(pairs.begin(),pairs.end(),[&](const Pair& a,const Pair& b){
        if(a.usable!=b.usable)return a.usable;
        if(!a.usable)return false;
        const bool scope_a=scope(a.destination)==scope(a.source),scope_b=scope(b.destination)==scope(b.source);if(scope_a!=scope_b)return scope_a;
        const bool label_a=policy(a.destination).label==policy(a.source).label,label_b=policy(b.destination).label==policy(b.source).label;if(label_a!=label_b)return label_a;
        const auto precedence_a=policy(a.destination).precedence,precedence_b=policy(b.destination).precedence;if(precedence_a!=precedence_b)return precedence_a>precedence_b;
        const auto s_a=scope(a.destination),s_b=scope(b.destination);if(s_a!=s_b)return s_a<s_b;
        if(mapped(a.destination)==mapped(b.destination)){const auto l_a=common(a.source,a.destination),l_b=common(b.source,b.destination);if(l_a!=l_b)return l_a>l_b;}
        return false;});
    // 輸出：SOCKADDR_IN6_PAIR 陣列後接各組來源與目的 SOCKADDR_IN6；整塊由 FreeMibTable 釋放。
    const auto pair_size=2ULL*unit,size=count*pair_size+count*56ULL;const auto base=allocate_heap(memory,size);if(!base)return finish(8);
    std::vector<std::uint8_t> bytes(size);
    for(std::uint32_t i=0;i<count;++i){
        const auto source_at=count*pair_size+i*56ULL,destination_at=source_at+28;
        for(unsigned k=0;k<unit;++k){bytes[i*pair_size+k]=static_cast<std::uint8_t>((base+source_at)>>(8*k));bytes[i*pair_size+unit+k]=static_cast<std::uint8_t>((base+destination_at)>>(8*k));}
        std::copy(pairs[i].source.begin(),pairs[i].source.end(),bytes.begin()+static_cast<std::ptrdiff_t>(source_at));
        std::copy(pairs[i].destination.begin(),pairs[i].destination.end(),bytes.begin()+static_cast<std::ptrdiff_t>(destination_at));
    }
    memory.write(GuestAddress{base},bytes);runtime_.network.addresses.insert(base);
    memory.store(GuestAddress{list_out},static_cast<std::uint8_t>(unit*8),base);memory.store(GuestAddress{count_out},32,count);return finish(0);
}
