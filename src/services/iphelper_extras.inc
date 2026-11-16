// iphlpapi 其餘查詢入口，與既有介面／路由快照（windows_network_config.cpp）共用同一份 NetworkConfiguration：
// - GetNumberOfInterfaces（介面數）、GetAdapterIndex（"\DEVICE\TCPIP_" 加介面 GUID 或 GetAdaptersInfo 的 AdapterName，也可不加前綴，大小寫不分）、GetFriendlyIfIndex（只保留低 24 位元）。
// - GetPerAdapterInfo：IP_PER_ADAPTER_INFO，AutoconfigEnabled 1、AutoconfigActive 依介面是否只有 169.254／16 位址，DNS 伺服器串成 IP_ADDR_STRING 串列；
//   緩衝不足 ERROR_BUFFER_OVERFLOW 並回報所需大小，未知介面 ERROR_INVALID_PARAMETER。
// - GetUniDirectionalAdapterInfo：沒有單向介面，NumAdapters＝0（所需大小 8）。
// - ConvertGuidToStringA／W（"{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"，長度不足 ERROR_INSUFFICIENT_BUFFER）、ConvertStringToGuidW。
// - GetBestRoute：自 GetIpForwardTable 的列中取最長前置相符、Metric1 最小者；沒有路由 ERROR_NETWORK_UNREACHABLE。
// - Get{Ip,Icmp,Tcp,Udp}Statistics 與 Ex 版（AF_INET／AF_INET6，其他 ERROR_INVALID_PARAMETER）：協定計數取自宿主（platform/network_statistics），
//   介面／位址／路由數、TCP 已建立與全部連線數、UDP 端點數取自同一份介面快照與 GetExtendedTcpTable／GetExtendedUdpTable；比較模式不讀宿主計數。
//   ICMP Ex 版為各型別計數（MIBICMPSTATS_EX）。宿主 TCP／UDP 計數不分 IPv4／IPv6，兩個家族使用同一份。
// - AllocateAndGetIfTable／IpAddrTable／IpForwardTable／IpNetTable／TcpTable／UdpTable／TcpExTable FromStack：
//   以對應的 Get* 入口查詢大小後由指定 heap（HeapAlloc 旗標）配置並填入，失敗時釋放；TcpEx 為 TCP_TABLE_OWNER_PID_ALL。
// 由 windows_network_config.cpp 尾端引入（同一翻譯單元），沿用該檔的 refresh_configuration 與 interface_guid。
namespace {
std::u16string guid_text(std::span<const std::uint8_t> g){
    constexpr char16_t digits[]=u"0123456789ABCDEF";std::u16string out=u"{";
    const auto hex=[&](std::uint64_t value,unsigned count){for(int i=static_cast<int>(count)-1;i>=0;--i)out.push_back(digits[(value>>(i*4))&15]);};
    hex(static_cast<std::uint64_t>(g[0])|static_cast<std::uint64_t>(g[1])<<8|static_cast<std::uint64_t>(g[2])<<16|static_cast<std::uint64_t>(g[3])<<24,8);out+=u"-";
    hex(static_cast<std::uint64_t>(g[4])|static_cast<std::uint64_t>(g[5])<<8,4);out+=u"-";hex(static_cast<std::uint64_t>(g[6])|static_cast<std::uint64_t>(g[7])<<8,4);out+=u"-";
    hex(g[8],2);hex(g[9],2);out+=u"-";for(unsigned i=10;i<16;++i)hex(g[i],2);out+=u"}";return out;
}
std::optional<std::array<std::uint8_t,16>> parse_guid(std::u16string_view text){
    if(text.size()!=38 || text.front()!=u'{' || text.back()!=u'}')return std::nullopt;
    const auto nibble=[](char16_t c)->int{if(c>=u'0' && c<=u'9')return c-u'0';if(c>=u'a' && c<=u'f')return c-u'a'+10;if(c>=u'A' && c<=u'F')return c-u'A'+10;return -1;};
    std::array<std::uint8_t,16> out{};std::size_t at=1;
    const auto read=[&](unsigned count)->std::optional<std::uint64_t>{std::uint64_t v=0;for(unsigned i=0;i<count;++i){const auto n=nibble(text[at++]);if(n<0)return std::nullopt;v=v<<4|static_cast<unsigned>(n);}return v;};
    const auto dash=[&]{return text[at++]==u'-';};
    const auto d1=read(8);if(!d1 || !dash())return std::nullopt;const auto d2=read(4);if(!d2 || !dash())return std::nullopt;const auto d3=read(4);if(!d3 || !dash())return std::nullopt;
    for(unsigned i=0;i<4;++i)out[i]=static_cast<std::uint8_t>(*d1>>(8*i));for(unsigned i=0;i<2;++i){out[4+i]=static_cast<std::uint8_t>(*d2>>(8*i));out[6+i]=static_cast<std::uint8_t>(*d3>>(8*i));}
    for(unsigned i=0;i<2;++i){const auto b=read(2);if(!b)return std::nullopt;out[8+i]=static_cast<std::uint8_t>(*b);}if(!dash())return std::nullopt;
    for(unsigned i=0;i<6;++i){const auto b=read(2);if(!b)return std::nullopt;out[10+i]=static_cast<std::uint8_t>(*b);}
    return out;
}
}
std::span<const WindowsServiceDescriptor> iphelper_extra_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"iphlpapi.dll","GetNumberOfInterfaces",1},{"iphlpapi.dll","GetAdapterIndex",2},{"iphlpapi.dll","GetFriendlyIfIndex",1,0,0,0x1},
        {"iphlpapi.dll","GetPerAdapterInfo",3,0,0,0x1},{"iphlpapi.dll","GetUniDirectionalAdapterInfo",2},
        {"iphlpapi.dll","ConvertGuidToStringA",3,0,0,0x4},{"iphlpapi.dll","ConvertGuidToStringW",3,0,0,0x4},{"iphlpapi.dll","ConvertStringToGuidW",2},
        {"iphlpapi.dll","GetBestRoute",3,0,0,0x3},
        {"iphlpapi.dll","AllocateAndGetIfTableFromStack",4,0,0,0xa},{"iphlpapi.dll","AllocateAndGetIpAddrTableFromStack",4,0,0,0xa},
        {"iphlpapi.dll","AllocateAndGetIpForwardTableFromStack",4,0,0,0xa},{"iphlpapi.dll","AllocateAndGetIpNetTableFromStack",4,0,0,0xa},
        {"iphlpapi.dll","AllocateAndGetTcpTableFromStack",4,0,0,0xa},{"iphlpapi.dll","AllocateAndGetUdpTableFromStack",4,0,0,0xa},
        {"iphlpapi.dll","AllocateAndGetTcpExTableFromStack",5,0,0,0x1a},
        {"iphlpapi.dll","GetIpStatistics",1},{"iphlpapi.dll","GetIpStatisticsEx",2,0,0,0x2},{"iphlpapi.dll","GetIcmpStatistics",1},{"iphlpapi.dll","GetIcmpStatisticsEx",2,0,0,0x2},
        {"iphlpapi.dll","GetTcpStatistics",1},{"iphlpapi.dll","GetTcpStatisticsEx",2,0,0,0x2},{"iphlpapi.dll","GetUdpStatistics",1},{"iphlpapi.dll","GetUdpStatisticsEx",2,0,0,0x2}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_iphelper_extras(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const unsigned unit=x86_?4:8;
    const auto config=[&]()->NetworkConfiguration*{auto& c=*runtime_.network.configuration;return refresh_configuration(c,runtime_.network.external_io,false,false)?&c:nullptr;};
    try{
        if(name=="GetFriendlyIfIndex")return finish(d(0)&0x00ffffffU);
        if(name=="ConvertGuidToStringA" || name=="ConvertGuidToStringW"){
            if(!arg(0) || !arg(1))return finish(87);std::array<std::uint8_t,16> guid{};memory.read(GuestAddress{arg(0)},guid);
            const auto text=guid_text(guid);if(std::bit_cast<std::int32_t>(d(2))<static_cast<std::int32_t>(text.size()+1))return finish(122);
            if(name.ends_with('W'))memory.write(GuestAddress{arg(1)},utf16_bytes(text));else{auto s=utf16_to_utf8(text);std::vector<std::uint8_t> b(s.begin(),s.end());b.push_back(0);memory.write(GuestAddress{arg(1)},b);}
            return finish(0);
        }
        if(name=="ConvertStringToGuidW"){
            if(!arg(0) || !arg(1))return finish(87);const auto guid=parse_guid(read_utf16(memory,GuestAddress{arg(0)},64));if(!guid)return finish(87);
            memory.write(GuestAddress{arg(1)},*guid);return finish(0);
        }
        if(name=="GetUniDirectionalAdapterInfo"){
            if(!arg(1))return finish(87);const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));memory.check(GuestAddress{arg(1)},4,Access::Write);
            if(!arg(0) || capacity<8){memory.store(GuestAddress{arg(1)},32,8);return finish(111);}
            memory.store(GuestAddress{arg(0)},32,0);return finish(0);
        }
        if(name=="GetNumberOfInterfaces"){
            if(!arg(0))return finish(87);memory.check(GuestAddress{arg(0)},4,Access::Write);const auto c=config();if(!c)return finish(232);
            memory.store(GuestAddress{arg(0)},32,c->interfaces.size());return finish(0);
        }
        if(name=="GetAdapterIndex"){
            if(!arg(0) || !arg(1))return finish(87);memory.check(GuestAddress{arg(1)},4,Access::Write);
            auto wanted=read_utf16(memory,GuestAddress{arg(0)},512);for(auto& c:wanted)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
            if(wanted.starts_with(u"\\DEVICE\\TCPIP_"))wanted.erase(0,14);
            const auto c=config();if(!c)return finish(232);
            // 名稱可為介面 GUID 或 GetAdaptersInfo 回報的 AdapterName（宿主介面名稱），皆不分大小寫。
            for(const auto& item:c->interfaces){
                auto adapter=utf8_to_utf16_lossy(item.name);for(auto& ch:adapter)if(ch>=u'a' && ch<=u'z')ch=static_cast<char16_t>(ch-32);
                if(guid_text(interface_guid(item))==wanted || adapter==wanted){memory.store(GuestAddress{arg(1)},32,item.index);return finish(0);}
            }
            return finish(87);
        }
        if(name=="GetPerAdapterInfo"){
            if(!arg(2))return finish(87);const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));memory.check(GuestAddress{arg(2)},4,Access::Write);
            const auto c=config();if(!c)return finish(232);
            const auto item=std::find_if(c->interfaces.begin(),c->interfaces.end(),[&](const auto& i){return i.index==d(0);});if(item==c->interfaces.end())return finish(87);
            std::vector<std::array<std::uint8_t,4>> dns;
            for(const auto& address:c->dns)if(address.size()>=8 && address[0]==2){std::array<std::uint8_t,4> ip{};std::copy_n(address.begin()+4,4,ip.begin());if(std::find(dns.begin(),dns.end(),ip)==dns.end())dns.push_back(ip);}
            bool any=false,autoconfig=true;for(const auto& a:item->addresses)if(a.bytes.size()==4){any=true;if(!(a.bytes[0]==169 && a.bytes[1]==254))autoconfig=false;}
            // IP_PER_ADAPTER_INFO＝AutoconfigEnabled、AutoconfigActive、CurrentDnsServer、DnsServerList（IP_ADDR_STRING：Next、IpAddress[16]、IpMask[16]、Context）。
            const unsigned entry=x86_?40:48,head=x86_?12:16;const auto extra=dns.size()>1?dns.size()-1:0;const auto size=static_cast<std::uint32_t>(head+entry+extra*entry);
            if(!arg(1) || capacity<size){memory.store(GuestAddress{arg(2)},32,size);return finish(111);}
            std::vector<std::uint8_t> bytes(size);const auto put=[&](std::size_t at,std::uint64_t value,unsigned n){for(unsigned i=0;i<n;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
            put(0,1,4);put(4,any && autoconfig?1:0,4);put(8,arg(1)+head,unit);
            for(std::size_t i=0;i<std::max<std::size_t>(dns.size(),1);++i){
                const auto at=head+i*entry;if(i+1<dns.size())put(at,arg(1)+head+(i+1)*entry,unit);
                if(i<dns.size()){const auto text=std::to_string(dns[i][0])+"."+std::to_string(dns[i][1])+"."+std::to_string(dns[i][2])+"."+std::to_string(dns[i][3]);std::copy(text.begin(),text.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at+unit));}
            }
            memory.write(GuestAddress{arg(1)},bytes);memory.store(GuestAddress{arg(2)},32,size);return finish(0);
        }
        if(name=="GetBestRoute"){
            if(!arg(2))return finish(87);memory.check(GuestAddress{arg(2)},56,Access::Write);
            const auto size_cell=allocate_heap(memory,4);if(!size_cell)return finish(8);memory.store(GuestAddress{size_cell},32,0);
            std::uint64_t buffer=0;std::uint32_t status=0;
            for(unsigned attempt=0;attempt<4;++attempt){
                status=static_cast<std::uint32_t>(nested_service(state,memory,"iphlpapi.dll","GetIpForwardTable",{buffer,size_cell,0}));
                if(status!=122)break;if(buffer)(void)free_heap(memory,buffer,state);buffer=allocate_heap(memory,memory.load(GuestAddress{size_cell},32));if(!buffer){status=8;break;}
            }
            std::optional<std::array<std::uint8_t,56>> best;unsigned best_length=0;std::uint32_t best_metric=UINT32_MAX;
            if(!status && buffer){
                const auto rows=static_cast<std::uint32_t>(memory.load(GuestAddress{buffer},32));const auto destination=d(0);
                for(std::uint32_t i=0;i<rows;++i){
                    std::array<std::uint8_t,56> row{};memory.read(GuestAddress{buffer+4+i*56ULL},row);
                    const auto word=[&](unsigned at){return static_cast<std::uint32_t>(row[at])|static_cast<std::uint32_t>(row[at+1])<<8|static_cast<std::uint32_t>(row[at+2])<<16|static_cast<std::uint32_t>(row[at+3])<<24;};
                    const auto net=word(0),mask=word(4),metric=word(36);if((destination&mask)!=(net&mask))continue;
                    const auto length=static_cast<unsigned>(std::popcount(mask));if(!best || length>best_length || (length==best_length && metric<best_metric)){best=row;best_length=length;best_metric=metric;}
                }
            }
            if(buffer)(void)free_heap(memory,buffer,state);(void)free_heap(memory,size_cell,state);
            if(status)return finish(status);if(!best)return finish(1231);
            memory.write(GuestAddress{arg(2)},*best);return finish(0);
        }
        if(name.find("Statistics")!=std::string_view::npos){
            const bool ex=name.ends_with("Ex");const auto family=ex?d(1):2U;if(!arg(0) || (family!=2 && family!=23))return finish(87);const bool v6=family==23;
            const bool host=runtime_.network.external_io;
            // 以 Get* 入口查詢表格（暫存區由宿主 heap 配置），回傳筆數與資料。
            const auto table=[&](std::string_view symbol,std::initializer_list<std::uint64_t> extra)->std::optional<std::vector<std::uint8_t>>{
                const auto size_cell=allocate_heap(memory,4);if(!size_cell)return std::nullopt;memory.store(GuestAddress{size_cell},32,0);std::uint64_t buffer=0;std::uint32_t status=0;
                for(unsigned attempt=0;attempt<4;++attempt){
                    std::vector<std::uint64_t> args{buffer,size_cell};args.insert(args.end(),extra.begin(),extra.end());
                    status=static_cast<std::uint32_t>(nested_service(state,memory,"iphlpapi.dll",symbol,std::span<const std::uint64_t>(args)));
                    if(status!=122)break;if(buffer)(void)free_heap(memory,buffer,state);buffer=allocate_heap(memory,std::max<std::uint64_t>(memory.load(GuestAddress{size_cell},32),4));if(!buffer){status=8;break;}
                }
                std::optional<std::vector<std::uint8_t>> result;
                if(!status && buffer){std::vector<std::uint8_t> bytes(static_cast<std::size_t>(memory.load(GuestAddress{size_cell},32)));memory.read(GuestAddress{buffer},bytes);result=std::move(bytes);}
                else if(status==232)result=std::vector<std::uint8_t>(4,0);
                if(buffer)(void)free_heap(memory,buffer,state);(void)free_heap(memory,size_cell,state);return result;
            };
            const auto entries=[](const std::vector<std::uint8_t>& bytes){return bytes.size()>=4?static_cast<std::uint32_t>(bytes[0])|static_cast<std::uint32_t>(bytes[1])<<8|static_cast<std::uint32_t>(bytes[2])<<16|static_cast<std::uint32_t>(bytes[3])<<24:0U;};
            const auto store=[&](std::span<const std::uint32_t> values){for(std::size_t i=0;i<values.size();++i)memory.store(GuestAddress{arg(0)+i*4},32,values[i]);};
            if(name.starts_with("GetIpStatistics")){
                memory.check(GuestAddress{arg(0)},92,Access::Write);
                auto values=host?host_network_statistics::ip(v6).value_or(std::array<std::uint32_t,23>{}):std::array<std::uint32_t,23>{};
                if(!host){values[0]=2;values[1]=v6?64:128;values[13]=60;}
                if(const auto c=config()){
                    std::uint32_t addresses=0,routes=0;for(const auto& item:c->interfaces){std::set<std::vector<std::uint8_t>> prefixes;
                        for(const auto& a:item.addresses)if(a.bytes.size()==(v6?16U:4U)){++addresses;if(v6){auto p=a.bytes;for(std::size_t b=a.prefix;b<128;++b)p[b/8]=static_cast<std::uint8_t>(p[b/8]&~(0x80U>>(b%8)));prefixes.insert(p);}}
                        if(v6)routes+=static_cast<std::uint32_t>(prefixes.size()+item.ipv6_gateways.size());}
                    if(!v6){const auto forward=table("GetIpForwardTable",{0});routes=forward?entries(*forward):0;}
                    values[20]=static_cast<std::uint32_t>(c->interfaces.size());values[21]=addresses;values[22]=routes;
                }
                store(values);return finish(0);
            }
            if(name.starts_with("GetTcpStatistics")){
                memory.check(GuestAddress{arg(0)},60,Access::Write);
                auto values=host?host_network_statistics::tcp().value_or(std::array<std::uint32_t,15>{4,200,64000,UINT32_MAX}):std::array<std::uint32_t,15>{4,200,64000,UINT32_MAX};
                // TCP_TABLE_OWNER_PID_ALL：IPv4 列 24 位元組（狀態在 0），IPv6 列 56 位元組（狀態在 48）；已建立＝ESTABLISHED（5）或 CLOSE_WAIT（8）。
                if(const auto rows=table("GetExtendedTcpTable",{0,family,5,0})){
                    const auto count=entries(*rows);const std::size_t row=v6?56:24,at=v6?48:0;std::uint32_t established=0;
                    for(std::uint32_t i=0;i<count && 4+(i+1)*row<=rows->size();++i){const auto* p=rows->data()+4+i*row+at;const auto s=static_cast<std::uint32_t>(p[0])|static_cast<std::uint32_t>(p[1])<<8;established+=s==5 || s==8;}
                    values[8]=established;values[14]=count;
                }
                store(values);return finish(0);
            }
            if(name.starts_with("GetUdpStatistics")){
                memory.check(GuestAddress{arg(0)},20,Access::Write);
                auto values=host?host_network_statistics::udp().value_or(std::array<std::uint32_t,5>{}):std::array<std::uint32_t,5>{};
                if(const auto rows=table("GetExtendedUdpTable",{0,family,0,0}))values[4]=entries(*rows);
                store(values);return finish(0);
            }
            // ICMP：MIB_ICMP 為 13 欄位的收、送 MIBICMPSTATS；Ex 版為 dwMsgs、dwErrors 與 256 個型別計數。
            const auto stats=host?host_network_statistics::icmp(v6).value_or(host_network_statistics::Icmp{}):host_network_statistics::Icmp{};
            if(ex){
                memory.check(GuestAddress{arg(0)},2*1032,Access::Write);std::vector<std::uint32_t> values;
                for(const bool in:{true,false}){values.push_back(in?stats.in_messages:stats.out_messages);values.push_back(in?stats.in_errors:stats.out_errors);const auto& h=in?stats.in:stats.out;values.insert(values.end(),h.begin(),h.end());}
                store(values);return finish(0);
            }
            memory.check(GuestAddress{arg(0)},104,Access::Write);std::vector<std::uint32_t> values;
            for(const bool in:{true,false}){const auto& h=in?stats.in:stats.out;
                values.insert(values.end(),{in?stats.in_messages:stats.out_messages,in?stats.in_errors:stats.out_errors,h[3],h[11],h[12],h[4],h[5],h[8],h[0],h[13],h[14],h[17],h[18]});}
            store(values);return finish(0);
        }
        if(name.starts_with("AllocateAndGet")){
            if(!arg(0))return finish(87);memory.check(GuestAddress{arg(0)},unit,Access::Write);
            const bool ex=name=="AllocateAndGetTcpExTableFromStack";const std::uint32_t order=d(1),flags=d(3);const std::uint64_t heap=arg(2);
            const std::string_view target=name=="AllocateAndGetIfTableFromStack"?"GetIfTable":name=="AllocateAndGetIpAddrTableFromStack"?"GetIpAddrTable":
                name=="AllocateAndGetIpForwardTableFromStack"?"GetIpForwardTable":name=="AllocateAndGetIpNetTableFromStack"?"GetIpNetTable":
                name=="AllocateAndGetTcpTableFromStack"?"GetTcpTable":name=="AllocateAndGetUdpTableFromStack"?"GetUdpTable":"GetExtendedTcpTable";
            const auto size_cell=allocate_heap(memory,4);if(!size_cell)return finish(8);memory.store(GuestAddress{size_cell},32,0);
            const auto query=[&](std::uint64_t buffer){return static_cast<std::uint32_t>(ex?nested_service(state,memory,"iphlpapi.dll",target,{buffer,size_cell,order,d(4),5,0}):nested_service(state,memory,"iphlpapi.dll",target,{buffer,size_cell,order}));};
            // 由呼叫端指定的 heap 配置（與 HeapAlloc 同一配置器；x64 的 HeapAlloc 入口是客體常式，不經服務呼叫）；HEAP_ZERO_MEMORY 時清零。
            const auto allocate=[&](std::uint64_t size)->std::uint64_t{const auto block=allocate_heap(memory,size,heap);if(block && (flags&8U))memory.write(GuestAddress{block},std::vector<std::uint8_t>(static_cast<std::size_t>(size)));return block;};
            std::uint64_t table=0;std::uint32_t status=query(0);
            for(unsigned attempt=0;attempt<4 && (status==122 || status==111);++attempt){
                if(table)(void)free_heap(memory,table,state);
                const auto size=std::max<std::uint64_t>(memory.load(GuestAddress{size_cell},32),4);
                table=allocate(size);if(!table){status=8;break;}
                status=query(table);
            }
            // 空表格（例如沒有 ARP 項目回 ERROR_NO_DATA）仍配置只含筆數 0 的表格。
            if(status==232 || (!status && !table)){if(!table){table=allocate(8);if(table)memory.store(GuestAddress{table},32,0);}status=table?0:8U;}
            (void)free_heap(memory,size_cell,state);
            if(status){if(table)(void)free_heap(memory,table,state);return finish(status);}
            memory.store(GuestAddress{arg(0)},static_cast<std::uint8_t>(unit*8),table);return finish(0);
        }
    }catch(const MemoryFault&){return finish(998);}catch(const std::bad_alloc&){return finish(8);}
    return finish(50);
}
