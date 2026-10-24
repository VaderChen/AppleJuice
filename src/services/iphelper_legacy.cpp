#include "ipv4_prefix.h"
#include "iphelper_legacy.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <limits>
#include <optional>
#include <stdexcept>

namespace x64arm::iphelper {
namespace {
void put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint64_t value,unsigned size){
    for(unsigned i=0;i<size;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));
}
void text(std::vector<std::uint8_t>& bytes,std::size_t at,std::size_t capacity,std::string_view value){
    auto size=std::min(capacity-1,value.size());
    while(size && size<value.size() && (static_cast<unsigned char>(value[size])&0xc0U)==0x80U)--size;
    std::copy_n(value.begin(),size,bytes.begin()+static_cast<std::ptrdiff_t>(at));
}
std::string ipv4(const std::array<std::uint8_t,4>& value){
    std::array<char,INET_ADDRSTRLEN> buffer{};
    if(!::inet_ntop(AF_INET,value.data(),buffer.data(),buffer.size()))throw std::invalid_argument("IPv4 位址轉換失敗");
    return buffer.data();
}
struct Writer {
    Blob result;unsigned unit,node;
    explicit Writer(bool small):unit(small?4:8),node(small?40:48){}
    std::size_t allocate(std::size_t size){
        const auto at=(result.bytes.size()+unit-1)&~std::size_t{unit-1};
        if(at>16U*1024U*1024U || size>16U*1024U*1024U-at)throw std::length_error("IP Helper 輸出超過容量");
        result.bytes.resize(at+size);return at;
    }
    void pointer(std::size_t at,std::size_t target){put(result.bytes,at,target,unit);result.pointers.push_back(at);}
    void address(std::size_t at,const std::array<std::uint8_t,4>& value,const std::array<std::uint8_t,4>& mask={},std::uint32_t context=0){
        text(result.bytes,at+unit,16,ipv4(value));text(result.bytes,at+unit+16,16,ipv4(mask));put(result.bytes,at+unit+32,context,4);
    }
    void list(std::size_t first,const std::vector<std::array<std::uint8_t,4>>& values){
        address(first,{});auto previous=first;
        for(std::size_t i=0;i<values.size();++i){const auto at=i?allocate(node):first;if(i)pointer(previous,at);address(at,values[i]);previous=at;}
    }
};
}
void Blob::relocate(std::uint64_t base,unsigned unit){
    const auto limit=unit==4?UINT64_C(0xffffffff):UINT64_MAX;
    if(base>limit || bytes.size()>limit-base)throw std::invalid_argument("IP Helper 客體位址溢位");
    for(auto at:pointers){std::uint64_t offset=0;for(unsigned i=0;i<unit;++i)offset|=std::uint64_t{bytes.at(at+i)}<<(i*8);put(bytes,at,base+offset,unit);}
}
Blob adapters(const NetworkConfiguration& config,bool x86){
    Writer w(x86);std::optional<std::size_t> previous;std::uint32_t context=0;
    for(const auto& item:config.interfaces){
        if(item.type==24)continue; // GetAdaptersInfo 排除 loopback，只提供 IPv4 資料。
        const auto at=w.allocate(x86?640:704);if(previous)w.pointer(*previous,at);previous=at;
        text(w.result.bytes,at+(x86?8:12),260,item.name);
        text(w.result.bytes,at+(x86?268:272),132,item.description.empty()?item.name:item.description);
        const auto physical=x86?404U:408U;
        put(w.result.bytes,at+physical-4,std::min<std::size_t>(8,item.physical.size()),4);
        std::copy_n(item.physical.begin(),std::min<std::size_t>(8,item.physical.size()),w.result.bytes.begin()+static_cast<std::ptrdiff_t>(at+physical));
        put(w.result.bytes,at+(x86?412:416),item.index,4);put(w.result.bytes,at+(x86?416:420),item.type,4);
        put(w.result.bytes,at+(x86?420:424),item.dhcp_enabled,4);
        const auto first=at+(x86?428:440);w.address(first,{});
        auto last=first;bool used=false;
        for(const auto& address:item.addresses){
            if(address.bytes.size()<8 || address.bytes[0]!=2)continue;
            const auto node=used?w.allocate(w.node):first;if(used)w.pointer(last,node);used=true;last=node;
            std::array<std::uint8_t,4> ip{},mask{};std::copy_n(address.bytes.begin()+4,4,ip.begin());
            const auto packed_mask=x64arm_ipv4_mask(address.prefix);
            for(unsigned i=0;i<4;++i)mask[i]=static_cast<std::uint8_t>(packed_mask>>(i*8));
            w.address(node,ip,mask,++context); // 本快照中的 IPv4 NTE 身分；不冒用宿主指標。
        }
        w.list(at+(x86?468:488),item.gateways);w.address(at+(x86?508:536),item.dhcp_server);
        put(w.result.bytes,at+(x86?548:584),!item.wins.empty(),4);
        w.address(at+(x86?552:592),item.wins.empty()?std::array<std::uint8_t,4>{}:item.wins[0]);
        w.address(at+(x86?592:640),item.wins.size()>1?item.wins[1]:std::array<std::uint8_t,4>{});
        put(w.result.bytes,at+(x86?632:688),static_cast<std::uint64_t>(item.lease_obtained),x86?4:8);
        put(w.result.bytes,at+(x86?636:696),static_cast<std::uint64_t>(item.lease_expires),x86?4:8);
    }
    return std::move(w.result);
}
Blob fixed_info(const NetworkConfiguration& config,bool x86){
    Writer w(x86);w.allocate(x86?584:600);
    text(w.result.bytes,0,132,config.host_name);text(w.result.bytes,132,132,config.domain_name);
    std::vector<std::array<std::uint8_t,4>> dns;
    for(const auto& address:config.dns)if(address.size()>=8 && address[0]==2){std::array<std::uint8_t,4> ip{};std::copy_n(address.begin()+4,4,ip.begin());if(std::find(dns.begin(),dns.end(),ip)==dns.end())dns.push_back(ip);}
    w.list(x86?268:272,dns);
    // 依 WINS 設定選擇相容層 NetBIOS 查詢順序；ARP proxy 狀態由宿主提供。
    const bool wins=std::any_of(config.interfaces.begin(),config.interfaces.end(),[](const auto& item){return !item.wins.empty();});
    put(w.result.bytes,x86?308:320,wins?8:1,4);text(w.result.bytes,x86?312:324,260,config.scope_id);
    put(w.result.bytes,x86?572:584,config.routing_enabled,4);
    put(w.result.bytes,x86?576:588,config.proxy_enabled,4);
    put(w.result.bytes,x86?580:592,!dns.empty(),4);
    return std::move(w.result);
}
std::vector<std::uint8_t> interface_row(const NetworkInterface& item){
    std::vector<std::uint8_t> bytes(860);
    const auto name=utf16_bytes(utf8_to_utf16(item.name).substr(0,255));std::copy(name.begin(),name.end(),bytes.begin());
    put(bytes,512,item.index,4);put(bytes,516,item.type,4);put(bytes,520,item.mtu,4);
    put(bytes,524,std::min<std::uint64_t>(UINT32_MAX,item.speed),4);
    put(bytes,528,std::min<std::size_t>(8,item.physical.size()),4);
    std::copy_n(item.physical.begin(),std::min<std::size_t>(8,item.physical.size()),bytes.begin()+532);
    put(bytes,540,item.admin_status,4);put(bytes,544,item.admin_status!=1?0:item.media_state!=1?2:5,4);
    put(bytes,548,item.last_change,4);
    constexpr std::array<unsigned,12> counters{0,1,2,3,4,5,9,10,11,12,13,17};
    for(unsigned i=0;i<counters.size();++i)put(bytes,552+4*i,item.statistics[counters[i]],4);
    const auto description=item.description.empty()?item.name:item.description;
    const auto size=std::min<std::size_t>(256,description.size());put(bytes,600,size,4);
    std::copy_n(description.begin(),size,bytes.begin()+604);
    return bytes;
}
Blob interface_table(const NetworkConfiguration& config,bool sorted){
    Blob result;result.bytes.resize(4+config.interfaces.size()*860);put(result.bytes,0,config.interfaces.size(),4);
    std::vector<const NetworkInterface*> rows;for(const auto& item:config.interfaces)rows.push_back(&item);
    if(sorted)std::sort(rows.begin(),rows.end(),[](const auto* a,const auto* b){return a->index<b->index;});
    for(std::size_t i=0;i<rows.size();++i){const auto row=interface_row(*rows[i]);std::copy(row.begin(),row.end(),result.bytes.begin()+static_cast<std::ptrdiff_t>(4+860*i));}
    return result;
}
}
