#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/data_protection.hpp"
#include "wlan_profile.hpp"
#include <algorithm>
#include <deque>

namespace x64arm {
namespace {
using namespace wlan;
using Guid=std::array<std::uint8_t,16>;
void put(Bytes& b,std::size_t at,std::uint64_t value,unsigned width=4){require(at<=b.size() && width<=b.size()-at,13);for(unsigned i=0;i<width;++i)b[at+i]=static_cast<std::uint8_t>(value>>(i*8));}
void wide(Bytes& b,std::size_t at,std::u16string_view value){require(value.size()<256,13);const auto bytes=utf16_bytes(std::u16string(value));require(at+bytes.size()<=b.size(),13);std::copy(bytes.begin(),bytes.end(),b.begin()+static_cast<std::ptrdiff_t>(at));}
void ssid(Bytes& b,std::size_t at,std::span<const std::uint8_t> value){require(value.size()<=32,13);put(b,at,value.size());std::copy(value.begin(),value.end(),b.begin()+static_cast<std::ptrdiff_t>(at+4));}
Bytes dword(std::uint32_t n){Bytes b(4);put(b,0,n);return b;}
std::string guid_key(const Guid& guid){std::string result;for(auto c:guid){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;}
struct Notice {std::uint32_t source{},code{};Guid guid{};Bytes data;bool operator==(const Notice&)const=default;};
struct ConnectionState {std::uint32_t mode{4},bss_type{3};std::u16string profile;std::optional<Profile> temporary;std::optional<Network> network;};
Bytes connection_notice(const ConnectionState& state,std::uint32_t reason,bool msm=false){
    Bytes b(msm?580:572);put(b,0,state.mode);wide(b,4,state.profile);
    if(state.network){const auto& n=*state.network;ssid(b,516,n.ssid);put(b,552,n.bss_type);put(b,msm?564:556,n.cipher!=0);
        if(msm)std::copy(n.bssid.begin(),n.bssid.end(),b.begin()+556);}
    put(b,msm?576:560,reason);
    if(!msm && state.mode==1 && state.temporary){const auto xml=utf16_bytes(utf8_to_utf16(state.temporary->xml));b.resize(568+xml.size());std::copy(xml.begin(),xml.end(),b.begin()+568);}
    return b;
}
Bytes radio(const Interface& i,bool list){Bytes data(list?772:12);const auto at=list?4U:0U;if(list)put(data,0,1);put(data,at,0);put(data,at+4,i.power?1:2);put(data,at+8,0);return data;}
bool match(const Profile& p,const Network& n){return p.connection.ssid==n.ssid && p.connection.bss_type==n.bss_type && p.connection.auth==n.auth && p.connection.cipher==n.cipher;}
Bytes bss_list(const std::vector<Network>& networks){
    require(networks.size()<=4096,8);Bytes data(8+networks.size()*360);put(data,4,networks.size());
    for(std::size_t i=0;i<networks.size();++i){const auto at=8+i*360;const auto& n=networks[i];ssid(data,at,n.ssid);std::copy(n.bssid.begin(),n.bssid.end(),data.begin()+static_cast<std::ptrdiff_t>(at+40));
        put(data,at+48,n.bss_type);put(data,at+52,n.phys.empty()?0:n.phys.back());put(data,at+56,static_cast<std::uint32_t>(n.rssi));put(data,at+60,signal_quality(n.rssi));
        put(data,at+64,n.in_reg_domain?1:0,1);put(data,at+66,n.beacon_period,2);put(data,at+88,(n.bss_type==1?1:2)|(n.cipher?16:0),2);put(data,at+92,n.frequency_khz);
        std::vector<std::uint16_t> rates;for(std::size_t j=0;j+2<=n.information_elements.size();){const auto id=n.information_elements[j],len=n.information_elements[j+1];require(len<=n.information_elements.size()-j-2,13);
            if(id==1 || id==50)for(std::size_t k=0;k<len && rates.size()<126;++k){const auto rate=n.information_elements[j+2+k];rates.push_back(static_cast<std::uint16_t>((rate&0x7fU)|((rate&0x80U)?0x8000U:0U)));}j+=2+len;}
        put(data,at+96,rates.size());for(std::size_t j=0;j<rates.size();++j)put(data,at+100+j*2,rates[j],2);
        require(n.information_elements.size()<=65536 && data.size()+n.information_elements.size()<=16*1024*1024,8);
        if(!n.information_elements.empty()){put(data,at+352,data.size()-at);put(data,at+356,n.information_elements.size());data.insert(data.end(),n.information_elements.begin(),n.information_elements.end());}
    }put(data,0,data.size());return data;
}
}
struct WlanState {
    struct Client {std::uint32_t version{},sources{},previous_sources{};std::uint64_t callback{},context{},work{},cancelling_thread{};bool ignore_duplicates{};std::optional<Notice> last;std::deque<Notice> pending;};
    struct Pending {std::string interface;unsigned kind{};std::shared_ptr<wlan::Operation> operation;ConnectionState connection;};
    std::uint64_t next_handle{0x6f000000};std::map<std::uint64_t,Client> clients;std::set<std::uint64_t> buffers;
    std::map<std::string,ConnectionState> connections;std::vector<Pending> operations;bool monitoring{},wake_ready{};
    void emit(const std::string& interface,std::uint32_t source,std::uint32_t code,Bytes data={}){
        Notice n{source,code,network_interface_guid(interface),std::move(data)};
        for(auto& [handle,c]:clients){(void)handle;if(!(c.sources&source) || c.cancelling_thread || !c.callback)continue;
            if(c.ignore_duplicates && c.last && *c.last==n)continue;c.last=n;c.pending.push_back(n);}
    }
};
std::span<const WindowsServiceDescriptor> wlan_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"wlanapi.dll","WlanOpenHandle",4,0,0,1},{"wlanapi.dll","WlanCloseHandle",2},
        {"wlanapi.dll","WlanEnumInterfaces",3},{"wlanapi.dll","WlanFreeMemory",1},{"wlanapi.dll","WlanAllocateMemory",1,0,0,1},
        {"wlanapi.dll","WlanQueryInterface",7,0,0,4},{"wlanapi.dll","WlanSetInterface",6,0,0,12},
        {"wlanapi.dll","WlanGetInterfaceCapability",4},{"wlanapi.dll","WlanGetAvailableNetworkList",5,0,0,4},
        {"wlanapi.dll","WlanGetNetworkBssList",7,0,0,24},{"wlanapi.dll","WlanScan",5},
        {"wlanapi.dll","WlanConnect",4},{"wlanapi.dll","WlanDisconnect",3},{"wlanapi.dll","WlanRegisterNotification",7,0,0,6},
        {"wlanapi.dll","WlanGetProfile",7},{"wlanapi.dll","WlanGetProfileList",4},{"wlanapi.dll","WlanDeleteProfile",4},
        {"wlanapi.dll","WlanSetProfile",8,0,0,36},{"wlanapi.dll","WlanSaveTemporaryProfile",7,0,0,48}};
    return table;
}
void WindowsServices::set_wlan_backend(std::shared_ptr<wlan::Backend> backend){
    if(wlan_ && (!wlan_->clients.empty() || !wlan_->operations.empty()))throw std::logic_error("WLAN 工作進行中不可替換後端");wlan_backend_=std::move(backend);
}
bool WindowsServices::wlan_waiting()const{return wlan_ && !wlan_->wake_ready && (wlan_->monitoring || !wlan_->operations.empty());}
void WindowsServices::poll_wlan(GuestMemory& memory){
    if(!runtime_.network.external_io || !wlan_ || !wlan_backend_ || (!wlan_->monitoring && wlan_->operations.empty() && std::none_of(wlan_->clients.begin(),wlan_->clients.end(),[](const auto& c){return !c.second.pending.empty();})))return;
    auto& bridge=*wlan_;const auto saved_error=last_error_;
    for(auto it=bridge.operations.begin();it!=bridge.operations.end();){
        if(!it->operation->done.load(std::memory_order_acquire)){++it;continue;}
        const auto code=it->operation->error;auto& current=bridge.connections[it->interface];
        if(it->kind==0)bridge.emit(it->interface,8,code?8:7,code?dword(0x2800a):Bytes{});
        else if(it->kind==1){
            auto result=it->connection;Interface info;if(!code && !wlan_backend_->query(it->interface,info) && info.connection)result.network=info.connection;
            bridge.emit(it->interface,8,10,connection_notice(result,code?0x28009:0));
            if(!code){current=std::move(result);bridge.emit(it->interface,16,4,connection_notice(current,0,true));}
        }else {if(!code){bridge.emit(it->interface,8,21,connection_notice(current,0));bridge.emit(it->interface,16,10,connection_notice(current,0,true));const auto bss=current.bss_type;current={};current.bss_type=bss;}}
        it=bridge.operations.erase(it);
    }
    for(const auto& event:wlan_backend_->poll()){
        if(event.change==Change::Unavailable)continue;Interface info;if(wlan_backend_->query(event.interface,info))continue;
        auto& current=bridge.connections[event.interface];
        if(event.change==Change::Radio)bridge.emit(event.interface,16,7,radio(info,false));
        else if(event.change==Change::Signal && info.connection)bridge.emit(event.interface,16,8,dword(signal_quality(info.connection->rssi)));
        else if(event.change==Change::ScanCache)bridge.emit(event.interface,8,26);
        else if(event.change==Change::Link && std::none_of(bridge.operations.begin(),bridge.operations.end(),[&](const auto& p){return p.interface==event.interface && p.kind!=0;})){
            if(info.connection){const bool changed=!current.network || current.network->bssid!=info.connection->bssid || current.network->ssid!=info.connection->ssid;
                if(changed){current.mode=4;current.profile.clear();current.temporary.reset();current.network=info.connection;bridge.emit(event.interface,8,10,connection_notice(current,0));bridge.emit(event.interface,16,4,connection_notice(current,0,true));}}
            else if(info.state==4 && current.network){bridge.emit(event.interface,8,21,connection_notice(current,0));bridge.emit(event.interface,16,10,connection_notice(current,0,true));const auto bss=current.bss_type;current={};current.bss_type=bss;}
        }
    }
    for(auto& [handle,c]:bridge.clients){
        (void)handle;if(c.work && !registered_waits_.contains(c.work))c.work=0;
        if(c.work || c.pending.empty() || !c.sources || c.cancelling_thread || registered_waits_.size()>=64)continue;
        const auto& n=c.pending.front();const auto unit=x86_?4U:8U,header=x86_?32U:40U;
        const auto worker=acquire_wait_worker(threads_.items.at(threads_.current).context,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));if(!worker)continue;
        const auto allocation=allocate_heap(memory,header+n.data.size());if(!allocation)continue;
        Bytes payload(header+n.data.size());put(payload,0,n.source);put(payload,4,n.code);std::copy(n.guid.begin(),n.guid.end(),payload.begin()+8);put(payload,24,n.data.size());put(payload,x86_?28:32,n.data.empty()?0:allocation+header,unit);
        std::copy(n.data.begin(),n.data.end(),payload.begin()+header);memory.write(GuestAddress{allocation},payload);
        RegisteredWait work;work.worker=worker;work.armed=false;work.ready=true;work.closing=true;work.work_item=true;work.callback_storage={allocation};work.callback_batch={{c.callback,{allocation,c.context}}};
        const auto id=next_registered_wait_++;registered_waits_.emplace(id,std::move(work));wait_workers_[worker]=id;c.work=id;c.pending.pop_front();
        auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};
    }
    last_error_=saved_error;
}
std::optional<std::uint32_t> WindowsServices::invoke_wlan(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto unit=x86_?4U:8U;const std::uint8_t bits=x86_?32:64;
    const auto finish=[&](std::uint64_t value=0){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(!wlan_)wlan_=std::make_shared<WlanState>();auto& bridge=*wlan_;std::uint64_t allocated{};
    try{
        const auto output=[&](std::uint64_t p,unsigned size){require(p!=0);memory.check(GuestAddress{p},size,Access::Write);};
        const auto allocate=[&](const Bytes& b){require(b.size()<=16*1024*1024,8);allocated=allocate_heap(memory,std::max<std::size_t>(1,b.size()));require(allocated,8);memory.write(GuestAddress{allocated},b);bridge.buffers.insert(allocated);const auto result=allocated;allocated=0;return result;};
        const auto blob=[&](std::uint64_t out,const Bytes& b){output(out,unit);memory.store(GuestAddress{out},bits,allocate(b));};
        if(name=="WlanFreeMemory"){if(bridge.buffers.erase(arg(0)))(void)free_heap(memory,arg(0),state);return finish();}
        if(name=="WlanAllocateMemory"){const auto size=static_cast<std::uint32_t>(arg(0));if(!size)return finish();require(size<=16*1024*1024,8);return finish(allocate(Bytes(size)));}
        if(name=="WlanOpenHandle"){
            require(!arg(1));output(arg(2),4);output(arg(3),unit);const auto version=static_cast<std::uint32_t>(arg(0))&0xffffU;require(version,50);require(bridge.clients.size()<4096,8);
            const auto negotiated=std::min(version,2U);const auto handle=bridge.next_handle++;bridge.clients[handle].version=negotiated;
            memory.store(GuestAddress{arg(2)},32,negotiated);memory.store(GuestAddress{arg(3)},bits,handle);return finish();
        }
        require(arg(0)!=0);const auto found=bridge.clients.find(arg(0));require(found!=bridge.clients.end(),6);auto& client=found->second;
        const auto backend=[&]() -> wlan::Backend& {require(runtime_.network.external_io,50);if(!wlan_backend_)wlan_backend_=wlan::native_backend();require(bool(wlan_backend_),50);bridge.wake_ready=wlan_backend_->set_wake(mapping_wake());return *wlan_backend_;};
        if(name=="WlanCloseHandle" || name=="WlanRegisterNotification"){
            const bool close=name=="WlanCloseHandle";require(!arg(close?1:5));
            auto sources=close?0U:static_cast<std::uint32_t>(arg(1));if(sources==UINT32_MAX)sources=0xffffU;
            require(!(sources&~0xffffU));if(client.version==1)require(!(sources&~8U),50);
            // 不存在的事件來源可登錄；只派送真實來源，不合成安全／IHV 事件。
            if(!close){require(!sources || arg(3));if(arg(6))output(arg(6),4);if(sources)memory.check(GuestAddress{arg(3)},1,Access::Execute);}
            if(client.cancelling_thread)require(client.cancelling_thread==threads_.current,170);
            if(client.work && registered_waits_.contains(client.work)){
                auto& work=registered_waits_.at(client.work);require(!(work.active && work.worker==threads_.current),1131);
                if(!client.cancelling_thread){client.previous_sources=client.sources;client.cancelling_thread=threads_.current;client.sources=0;client.pending.clear();}
                if(work.active){auto& caller=threads_.items.at(threads_.current);caller.status=ThreadStatus::Waiting;caller.wait={};caller.wait.kind=ThreadWaitKind::RegisteredWaitCompletion;caller.wait.handles={client.work};schedule(state,memory);return std::nullopt;}
                for(const auto p:work.callback_storage)(void)free_heap(memory,p,state);release_registered_wait(client.work);
            }
            const auto previous=client.cancelling_thread?client.previous_sources:client.sources;
            const bool any_other=std::any_of(bridge.clients.begin(),bridge.clients.end(),[&](const auto& c){return c.first!=arg(0) && c.second.sources;});
            if(sources && !bridge.monitoring){check(backend().monitor(true));bridge.monitoring=true;}
            if(!sources && !any_other && bridge.monitoring){check(backend().monitor(false));bridge.monitoring=false;}
            if(close)bridge.clients.erase(found);
            else {client.sources=sources;client.callback=arg(3);client.context=arg(4);client.ignore_duplicates=static_cast<std::uint32_t>(arg(2))!=0;client.last.reset();client.pending.clear();client.work=0;client.cancelling_thread=0;if(arg(6))memory.store(GuestAddress{arg(6)},32,previous);}
            auto& caller=threads_.items.at(threads_.current);if(caller.wait.kind==ThreadWaitKind::RegisteredWaitCompletion)caller.wait={};return finish();
        }
        if(name=="WlanEnumInterfaces"){
            require(!arg(1));output(arg(2),unit);std::vector<Interface> interfaces;check(backend().enumerate(interfaces));require(interfaces.size()<=4096,8);Bytes data(8+interfaces.size()*532);put(data,0,interfaces.size());
            for(std::size_t i=0;i<interfaces.size();++i){const auto at=8+i*532;const auto id=network_interface_guid(interfaces[i].name);std::copy(id.begin(),id.end(),data.begin()+static_cast<std::ptrdiff_t>(at));wide(data,at+16,utf8_to_utf16(interfaces[i].description).substr(0,255));put(data,at+528,interfaces[i].state);}
            blob(arg(2),data);return finish();
        }
        require(arg(1)!=0);Guid guid{};memory.read(GuestAddress{arg(1)},guid);std::vector<Interface> interfaces;check(backend().enumerate(interfaces));
        const auto iface=std::find_if(interfaces.begin(),interfaces.end(),[&](const auto& i){return network_interface_guid(i.name)==guid;});require(iface!=interfaces.end(),1168);
        auto& current=bridge.connections[iface->name];
        const auto query=[&](){Interface i;check(backend().query(iface->name,i));return i;};
        const auto read_ssid=[&](std::uint64_t at){const auto count=memory.load(GuestAddress{at},32);require(count<=32);Bytes b(static_cast<std::size_t>(count));memory.read(GuestAddress{at+4},b);return b;};
        const auto profiles=[&](){initialize_tokens();return Profiles(filesystem_,runtime_.token_objects.at(effective_token_id()),token_user_sid_string(effective_token_id()),guid_key(guid));};
        if(name=="WlanQueryInterface"){
            require(!arg(3));output(arg(4),4);output(arg(5),unit);if(arg(6))output(arg(6),4);const auto opcode=static_cast<std::uint32_t>(arg(2));const auto info=query();Bytes data;std::uint32_t type=0;
            if(opcode==1 || opcode==2 || opcode==3){const auto option=info.options.find(opcode);require(option!=info.options.end(),50);data=dword(option->second);type=2;}
            else if(opcode==4)data=radio(info,true);
            else if(opcode==5){data=dword(current.bss_type);type=2;}
            else if(opcode==6)data=dword(info.state);
            else if(opcode==7){check(info.connection_error);require(info.connection.has_value(),5023);const auto& n=*info.connection;data.resize(604);put(data,0,info.state);
                const bool owned=current.network && current.network->ssid==n.ssid;put(data,4,owned?current.mode:4);if(owned)wide(data,8,current.profile);
                ssid(data,520,n.ssid);put(data,556,n.bss_type);std::copy(n.bssid.begin(),n.bssid.end(),data.begin()+560);put(data,568,info.phy);put(data,576,signal_quality(n.rssi));put(data,580,info.rx_rate);put(data,584,info.tx_rate);
                put(data,588,n.cipher!=0);put(data,592,n.one_x);put(data,596,n.auth);put(data,600,n.cipher);
            }else if(opcode==8){require(info.state==1 || info.state==2,5023);data=dword(info.channel);}
            else if(opcode==9 || opcode==10){require(info.capabilities_known,50);if(opcode==10)throw Error{50};data.resize(4+info.auth_cipher_pairs.size()*8);put(data,0,info.auth_cipher_pairs.size());for(std::size_t i=0;i<info.auth_cipher_pairs.size();++i){put(data,4+i*8,info.auth_cipher_pairs[i][0]);put(data,8+i*8,info.auth_cipher_pairs[i][1]);}}
            else if(opcode==12)data=dword(4);
            else if(opcode==13 || opcode==14 || opcode==15)data=dword(0);
            else if(opcode==0x10000102U){check(info.connection_error);require(info.connection.has_value(),5023);data=dword(static_cast<std::uint32_t>(info.connection->rssi));}
            else throw Error{50};
            blob(arg(5),data);memory.store(GuestAddress{arg(4)},32,data.size());if(arg(6))memory.store(GuestAddress{arg(6)},32,type);return finish();
        }
        if(name=="WlanSetInterface"){
            require(!arg(5) && arg(4));const auto opcode=static_cast<std::uint32_t>(arg(2)),size=static_cast<std::uint32_t>(arg(3));require(size==(opcode==4?12U:4U));memory.check(GuestAddress{arg(4)},size,Access::Read);const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)},32));
            if(client.version==1)require(opcode==1 || opcode==5,50);
            if(opcode==4){require(value==0);const auto on=memory.load(GuestAddress{arg(4)+4},32);require(on==1 || on==2);check(backend().set_radio(iface->name,on==1));}
            else if(opcode==5){require(value>=1 && value<=3);current.bss_type=value;bridge.emit(iface->name,8,5,dword(value));}
            else if(opcode==1 || opcode==2 || opcode==3){check(backend().set_option(iface->name,opcode,value?1:0));if(opcode!=3)bridge.emit(iface->name,8,opcode==1?(value?1:2):(value?3:4));}
            else if(opcode==12){require(value==4 || value==0x80000000U);require(value==4,50);}
            else throw Error{50};return finish();
        }
        if(name=="WlanGetInterfaceCapability"){
            require(client.version>=2,50);require(!arg(2));output(arg(3),unit);const auto info=query();require(info.capabilities_known,50);require(info.phys.size()<=64,13);Bytes data(276);put(data,0,0);put(data,4,info.dot11d);put(data,8,1);put(data,12,1024);put(data,16,info.phys.size());for(std::size_t i=0;i<info.phys.size();++i)put(data,20+i*4,info.phys[i]);blob(arg(3),data);return finish();
        }
        if(name=="WlanGetNetworkBssList"){
            require(client.version>=2,50);require(!arg(5));output(arg(6),unit);std::optional<Bytes> filter;if(arg(2)){filter=read_ssid(arg(2));require(arg(3)==1 || arg(3)==2);}
            auto info=query();check(info.networks_error);if(filter)std::erase_if(info.networks,[&](const auto& n){return n.ssid!=*filter || n.bss_type!=arg(3) || bool(n.cipher)!=bool(static_cast<std::uint32_t>(arg(4)));});blob(arg(6),bss_list(info.networks));return finish();
        }
        if(name=="WlanGetAvailableNetworkList"){
            require(!arg(3));output(arg(4),unit);const auto flags=static_cast<std::uint32_t>(arg(2));require(!(flags&~3U));if(client.version==1)require(!flags);const auto info=query();check(info.networks_error);auto store=profiles();const auto saved=store.list();
            struct Group {Network network;std::uint32_t bssids{};std::vector<std::uint32_t> phys;const Profile* profile{};bool visible{};};std::vector<Group> groups;
            for(const auto& n:info.networks){if(current.bss_type!=3 && n.bss_type!=current.bss_type)continue;
                auto it=std::find_if(groups.begin(),groups.end(),[&](const auto& g){return g.network.ssid==n.ssid && g.network.bss_type==n.bss_type && g.network.auth==n.auth && g.network.cipher==n.cipher;});
                if(it==groups.end()){groups.push_back({n,0,{},nullptr,true});it=std::prev(groups.end());}++it->bssids;if(n.rssi>it->network.rssi)it->network=n;
                for(auto phy:n.phys)if(std::find(it->phys.begin(),it->phys.end(),phy)==it->phys.end())it->phys.push_back(phy);
            }
            for(const auto& p:saved){auto it=std::find_if(groups.begin(),groups.end(),[&](const auto& g){return match(p,g.network);});if(it!=groups.end()){if(!it->profile)it->profile=&p;continue;}
                if(((flags&1U) && p.connection.bss_type==2) || ((flags&2U) && p.hidden && !p.automatic)){Network n;n.ssid=p.connection.ssid;n.bss_type=p.connection.bss_type;n.auth=p.connection.auth;n.cipher=p.connection.cipher;groups.push_back({n,0,{},&p,false});}}
            Bytes data(8+groups.size()*628);put(data,0,groups.size());
            for(std::size_t i=0;i<groups.size();++i){const auto at=8+i*628;const auto& g=groups[i];const auto& n=g.network;if(g.profile)wide(data,at,g.profile->name);ssid(data,at+512,n.ssid);put(data,at+548,n.bss_type);put(data,at+552,g.bssids);
                const bool connectable=n.auth==1 || n.auth==4 || n.auth==7 || n.auth==9;put(data,at+556,connectable);put(data,at+560,connectable?0:0x20001);put(data,at+564,std::min<std::size_t>(8,g.phys.size()));
                for(std::size_t j=0;j<std::min<std::size_t>(8,g.phys.size());++j)put(data,at+568+j*4,g.phys[j]);put(data,at+600,g.phys.size()>8);put(data,at+604,g.visible?signal_quality(n.rssi):0);put(data,at+608,n.cipher!=0);put(data,at+612,n.auth);put(data,at+616,n.cipher);
                const bool connected=info.connection && info.connection->ssid==n.ssid && info.connection->auth==n.auth && info.connection->bss_type==n.bss_type;put(data,at+620,(connected?1U:0U)|(g.profile?2U:0U));}
            blob(arg(4),data);return finish();
        }
        if(name=="WlanGetProfileList" || name=="WlanGetProfile" || name=="WlanSetProfile" || name=="WlanDeleteProfile" || name=="WlanSaveTemporaryProfile"){
            auto store=profiles();
            if(name=="WlanGetProfileList"){require(!arg(2));output(arg(3),unit);const auto saved=store.list();Bytes data(8+saved.size()*516);put(data,0,saved.size());for(std::size_t i=0;i<saved.size();++i){wide(data,8+i*516,saved[i].name);put(data,520+i*516,saved[i].flags);}blob(arg(3),data);}
            else if(name=="WlanGetProfile"){
                require(arg(2) && !arg(3));output(arg(4),unit);if(arg(5))output(arg(5),4);if(arg(6))output(arg(6),4);const auto flags=arg(5)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(5)},32)):0U;require(!(flags&~4U));
                const auto p=store.get(read_utf16(memory,GuestAddress{arg(2)},256),read_access);const auto xml=profile_xml(p,store.context(),(flags&4U) && store.administrator());blob(arg(4),utf16_bytes(utf8_to_utf16(xml)));
                if(arg(5))memory.store(GuestAddress{arg(5)},32,p.flags);if(arg(6))memory.store(GuestAddress{arg(6)},32,store.access(p,0x2000000U));
            }else if(name=="WlanDeleteProfile"){
                require(arg(2) && !arg(3));store.erase(read_utf16(memory,GuestAddress{arg(2)},256));bridge.emit(iface->name,8,15);
            }else if(name=="WlanSetProfile"){
                require(!arg(6) && arg(3));output(arg(7),4);const auto flags=static_cast<std::uint32_t>(arg(2));require(flags<=2 && (flags!=1 || arg(5)));if(client.version==1)require(!flags && !arg(4));
                try{auto p=parse_profile(utf16_to_utf8(read_utf16(memory,GuestAddress{arg(3)},2*1024*1024)));p.flags=flags;
                    store.save(std::move(p),arg(5)!=0,arg(4) && !(flags&2U)?std::optional<std::string>{utf16_to_utf8(read_utf16(memory,GuestAddress{arg(4)}))}:std::nullopt);memory.store(GuestAddress{arg(7)},32,0);bridge.emit(iface->name,8,15);
                }catch(const Error& e){if(e.code==1206 || e.code==1169)memory.store(GuestAddress{arg(7)},32,e.reason);throw;}
            }else {
                require(client.version>=2,50);require(!arg(6));const auto flags=static_cast<std::uint32_t>(arg(4));require(!(flags&~0x30002U));require(!(flags&0x20000U) || arg(2));
                require(current.mode==1 && current.temporary.has_value() && query().state==1,5023);const auto profile_name=arg(2)?read_utf16(memory,GuestAddress{arg(2)},256):current.temporary->name;
                auto p=parse_profile(rename_profile_xml(*current.temporary,profile_name,(flags&0x10000U)?bool(flags&0x20000U):current.temporary->automatic));p.flags=flags&2U;
                store.save(std::move(p),arg(5)!=0,arg(3) && !(flags&2U)?std::optional<std::string>{utf16_to_utf8(read_utf16(memory,GuestAddress{arg(3)}))}:std::nullopt);bridge.emit(iface->name,8,15);
            }return finish();
        }
        if(name=="WlanScan" || name=="WlanConnect" || name=="WlanDisconnect"){
            const auto kind=name=="WlanScan"?0U:name=="WlanConnect"?1U:2U;require(!arg(kind==0?4:kind==1?3:2));
            require(std::none_of(bridge.operations.begin(),bridge.operations.end(),[&](const auto& p){return p.interface==iface->name && (kind==0?p.kind==0:p.kind!=0);}),170);
            WlanState::Pending pending;pending.interface=iface->name;pending.kind=kind;pending.connection=current;
            if(kind==0){std::optional<Bytes> filter;if(arg(2))filter=read_ssid(arg(2));Bytes ie;
                if(client.version==1)require(!arg(2) && !arg(3));if(arg(3)){const auto count=memory.load(GuestAddress{arg(3)},32);require(count<=240);ie.resize(static_cast<std::size_t>(count));memory.read(GuestAddress{arg(3)+4},ie);}
                pending.operation=backend().scan(iface->name,filter,ie);
            }else if(kind==1){
                require(arg(2));const auto at=arg(2);memory.check(GuestAddress{at},x86_?24:40,Access::Read);const auto mode=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));require(mode<4);
                const auto text=memory.load(GuestAddress{at+unit},bits),ssid_pointer=memory.load(GuestAddress{at+unit*2},bits),bssids=memory.load(GuestAddress{at+unit*3},bits);
                const auto bss=static_cast<std::uint32_t>(memory.load(GuestAddress{at+unit*4},32)),flags=static_cast<std::uint32_t>(memory.load(GuestAddress{at+unit*4+4},32));
                require(bss>=1 && bss<=3 && !(flags&~0x7fU));require(!(bss==1 && (flags&2U)) && !(bss==2 && (flags&1U)) && (!(flags&4U) || (mode==1 && bss!=2)));
                if(client.version==1)require(mode==0 && !flags && !bssids);Connection request;pending.connection={};pending.connection.mode=mode;pending.connection.bss_type=current.bss_type;
                if(mode==0 || mode==1){require(text);auto store=profiles();auto p=mode==0?store.get(read_utf16(memory,GuestAddress{text},256),execute_access):parse_profile(utf16_to_utf8(read_utf16(memory,GuestAddress{text},2*1024*1024)));
                    request=p.connection;request.password=profile_key(p,store.context());require(bss==3 || bss==request.bss_type);if(ssid_pointer)require(read_ssid(ssid_pointer)==request.ssid);
                    pending.connection.profile=p.name;if(mode==1){p.xml=profile_xml(p,store.context(),false);p=parse_profile(p.xml);pending.connection.temporary=p;}
                }else {require(ssid_pointer && bss!=3);request.ssid=read_ssid(ssid_pointer);require(!request.ssid.empty());request.bss_type=bss;
                    const auto info=query();check(info.networks_error);const auto network=std::find_if(info.networks.begin(),info.networks.end(),[&](const auto& n){return n.ssid==request.ssid && n.bss_type==bss && bool(n.cipher)==(mode==2);});require(network!=info.networks.end(),1168);
                    // 安全探索沒有使用者提供的金鑰，不讀取宿主已存憑證。
                    require(mode==3,50);request.auth=network->auth;request.cipher=network->cipher;}
                request.flags|=flags;if(current.bss_type!=3)require(request.bss_type==current.bss_type,5023);
                if(bssids){memory.check(GuestAddress{bssids},12,Access::Read);require(memory.load(GuestAddress{bssids},8)==0x80 && memory.load(GuestAddress{bssids+1},8)==1 && memory.load(GuestAddress{bssids+2},16)>=20);
                    const auto count=memory.load(GuestAddress{bssids+4},32),total=memory.load(GuestAddress{bssids+8},32);require(count<=total && count<=1024);
                    for(std::uint64_t i=0;i<count;++i){std::array<std::uint8_t,6> mac{};memory.read(GuestAddress{bssids+12+i*6},mac);request.bssids.push_back(mac);}}
                Network n;n.ssid=request.ssid;n.auth=request.auth;n.cipher=request.cipher;n.bss_type=request.bss_type;pending.connection.network=n;pending.operation=backend().connect(iface->name,request);
            }else pending.operation=backend().disconnect(iface->name);
            require(bool(pending.operation),8);check(pending.operation->submission_error);
            if(kind==1)bridge.emit(iface->name,8,9,connection_notice(pending.connection,0));else if(kind==2)bridge.emit(iface->name,8,20,connection_notice(current,0));
            bridge.operations.push_back(std::move(pending));return finish();
        }
        throw Error{50};
    }catch(const MemoryFault&){if(allocated)(void)free_heap(memory,allocated,state);return finish(name=="WlanAllocateMemory"?0:998);}
    catch(const Error& e){if(allocated)(void)free_heap(memory,allocated,state);return finish(name=="WlanAllocateMemory"?0:e.code);}
    catch(const DataProtectionError& e){return finish(e.code);}
    catch(const SecurityDescriptorError& e){return finish(e.code);}
    catch(const std::bad_alloc&){if(allocated)(void)free_heap(memory,allocated,state);return finish(name=="WlanAllocateMemory"?0:8);}
}
}
