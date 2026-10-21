#pragma once
#include "x64arm/bluetooth.hpp"
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace x64arm::bluetooth {
using Handle=std::uint64_t;
template<class T> struct Found {Handle search{},object{};T value{};};
struct Delivery {
    Handle registration{},token{},callback{},context{};
    DeviceId device;Characteristic characteristic;Bytes value;
};

// 由 Windows dispatcher 的單一客體執行緒輪詢；原生執行緒只完成 Request，不觸碰 handle／快取。
class Family {
    struct Server {
        Device info;std::uint64_t epoch{1};
        std::optional<std::vector<Service>> services;
        std::map<std::uint16_t,std::vector<Service>> included;
        std::map<std::uint16_t,std::vector<Characteristic>> characteristics;
        std::map<std::uint16_t,std::vector<Descriptor>> descriptors;
        std::map<std::uint16_t,Bytes> values;
        std::map<std::uint16_t,Bytes> descriptor_values;
    };
    enum class Kind {Radio,Device,Service};
    struct Endpoint {Kind kind{};DeviceId radio;std::shared_ptr<Server> server;std::optional<Service> service;std::uint64_t epoch{};unsigned access{};};
    struct Cursor {std::vector<Radio> radios;std::vector<Device> devices;std::size_t next{1};bool radio{};};
    struct Job {Handle owner{};std::shared_ptr<Server> server;std::function<bool()> poll;std::function<void()> cancel;};
    struct Registration {
        Handle handle{},callback{},context{};std::shared_ptr<Endpoint> endpoint;std::uint64_t epoch{};
        std::vector<Characteristic> characteristics;std::deque<std::pair<Characteristic,Bytes>> pending;
        std::set<Handle> active;bool closing{},invalidated{};Handle exclude{};Pending<bool> unregister;
    };
    std::shared_ptr<Backend> backend_;
    std::function<Handle()> allocate_;
    std::map<Handle,std::shared_ptr<Endpoint>> handles_;
    std::map<Handle,Cursor> cursors_;
    std::map<DeviceId,std::shared_ptr<Server>> servers_;
    std::map<Handle,std::shared_ptr<Registration>> registrations_;
    std::map<Handle,std::shared_ptr<Registration>> deliveries_;
    std::map<std::uint64_t,Job> jobs_;
    std::uint64_t next_job_{1},next_delivery_{1};bool closed_{};
    static constexpr unsigned read_access=1,write_access=2;

    Handle allocate(){const auto value=allocate_();if(!value || value==UINT64_MAX || handles_.contains(value) || cursors_.contains(value) || registrations_.contains(value))throw std::bad_alloc{};return value;}
    template<class T> static Pending<T> immediate(std::uint32_t error,T value={}){auto result=std::make_shared<Request<T>>();result->complete({error,std::move(value)});return result;}
    std::shared_ptr<Endpoint> endpoint(Handle handle,unsigned access=read_access)const{
        const auto found=handles_.find(handle);if(found==handles_.end() || found->second->kind==Kind::Radio)return {};
        const auto& value=found->second;if((value->access&access)!=access)return {};return value;
    }
    std::uint32_t endpoint_error(Handle handle,unsigned access=read_access)const{
        const auto found=handles_.find(handle);if(found==handles_.end() || found->second->kind==Kind::Radio)return hresult(6);
        if((found->second->access&access)!=access)return hresult(5);
        if(found->second->kind==Kind::Service && found->second->epoch!=found->second->server->epoch)return att_error(1);return 0;
    }
    static bool known(const Server& server,const Service& value){
        if(server.services && std::find(server.services->begin(),server.services->end(),value)!=server.services->end())return true;
        for(const auto& [parent,items]:server.included){(void)parent;if(std::find(items.begin(),items.end(),value)!=items.end())return true;}return false;
    }
    static bool descends(const Server& server,std::uint16_t ancestor,std::uint16_t target){
        std::vector<std::uint16_t> pending{ancestor};std::set<std::uint16_t> visited;
        while(!pending.empty()){const auto parent=pending.back();pending.pop_back();if(parent==target)return true;if(!visited.insert(parent).second)continue;
            if(const auto found=server.included.find(parent);found!=server.included.end())for(const auto& item:found->second)pending.push_back(item.handle);
        }return false;
    }
    static bool known(const Server& server,const Characteristic& value){const auto found=server.characteristics.find(value.service);return found!=server.characteristics.end() && std::find(found->second.begin(),found->second.end(),value)!=found->second.end();}
    static bool known(const Server& server,const Descriptor& value){const auto found=server.descriptors.find(value.characteristic);return found!=server.descriptors.end() && std::find(found->second.begin(),found->second.end(),value)!=found->second.end();}
    static std::uint32_t scope(const Endpoint& endpoint,std::uint16_t service){return endpoint.service && !descends(*endpoint.server,endpoint.service->handle,service)?hresult(5):0;}

    template<class Native,class Output,class Finish> Pending<Output> collect(Handle owner,std::shared_ptr<Server> server,Pending<Native> native,Finish finish){
        if(!native)return immediate<Output>(hresult(50));
        if(closed_){native->cancel();return immediate<Output>(hresult(995));}
        if(jobs_.size()>=4096){native->cancel();return immediate<Output>(hresult(1450));}
        Pending<Output> result;try{result=std::make_shared<Request<Output>>([native]{native->cancel();});}catch(...){native->cancel();throw;}const auto epoch=server?server->epoch:0;
        Job job;job.owner=owner;job.server=server;job.cancel=[native,result]{native->cancel();result->complete({hresult(995),{}});};
        job.poll=[server,epoch,native,result,finish=std::move(finish)]()mutable{
            if(result->poll()){native->cancel();return true;}const auto ready=native->poll();if(!ready)return false;
            if(server && server->epoch!=epoch){result->complete({hresult(22),{}});return true;}
            try{result->complete(finish(*ready));}catch(const std::bad_alloc&){result->complete({hresult(1450),{}});}return true;
        };
        try{jobs_.emplace(next_job_++,std::move(job));}catch(...){native->cancel();throw;}return result;
    }
    void retire(const std::shared_ptr<Registration>& registration){
        if(!registration->closing)return;
        const auto active=registration->active.size()-(registration->exclude && registration->active.contains(registration->exclude)?1U:0U);
        if(active)return;if(registration->unregister)registration->unregister->complete({0,true});registrations_.erase(registration->handle);
    }
    void changed(const std::shared_ptr<Server>& server){
        ++server->epoch;server->services.reset();server->included.clear();server->characteristics.clear();server->descriptors.clear();server->values.clear();server->descriptor_values.clear();
        for(auto& [handle,registration]:registrations_){(void)handle;if(registration->endpoint->server==server){registration->invalidated=true;registration->pending.clear();}}
    }
public:
    Family(std::shared_ptr<Backend> backend,std::function<Handle()> allocate):backend_(std::move(backend)),allocate_(std::move(allocate)){if(!backend_ || !allocate_)throw std::invalid_argument("Bluetooth family 缺少後端或 handle allocator");}
    Family(const Family&)=delete;Family& operator=(const Family&)=delete;
    ~Family(){shutdown();}
    bool owns(Handle handle)const{return handles_.contains(handle) || cursors_.contains(handle) || registrations_.contains(handle);}
    bool has_resources()const{return !handles_.empty() || !cursors_.empty() || !registrations_.empty() || !deliveries_.empty() || !jobs_.empty();}
    bool delivery_pending()const{return std::any_of(registrations_.begin(),registrations_.end(),[](const auto& item){const auto& value=*item.second;return !value.closing && !value.invalidated && !value.pending.empty();});}
    std::vector<Device> device_snapshot()const{std::vector<Device> result;for(const auto& [id,server]:servers_){(void)id;result.push_back(server->info);}return result;}
    std::vector<Service> service_snapshot(const DeviceId& id)const{const auto found=servers_.find(id);return found!=servers_.end() && found->second->services?*found->second->services:std::vector<Service>{};}
    std::size_t pending_count()const{return jobs_.size();}
    void shutdown(){
        if(closed_)return;closed_=true;for(auto& [id,job]:jobs_){(void)id;job.cancel();}jobs_.clear();
        for(auto& [id,registration]:registrations_){(void)id;if(registration->unregister)registration->unregister->complete({hresult(995),false});}
        registrations_.clear();deliveries_.clear();handles_.clear();cursors_.clear();servers_.clear();
    }
    void pump(){
        if(closed_)return;
        for(auto& event:backend_->drain_events()){
            const auto found=servers_.find(event.device);if(found==servers_.end())continue;auto& server=*found->second;
            if(event.kind!=ValueEvent::Kind::Value){changed(found->second);continue;}
            std::optional<Characteristic> attribute;
            for(const auto& [service,values]:server.characteristics){(void)service;const auto item=std::find_if(values.begin(),values.end(),[&](const auto& value){return value.handle==event.handle;});if(item!=values.end()){attribute=*item;break;}}
            if(!attribute || event.value.size()>512)continue;server.values.insert_or_assign(attribute->handle,event.value);
            for(auto& [handle,registration]:registrations_){(void)handle;
                if(registration->closing || registration->invalidated || registration->endpoint->server!=found->second || registration->epoch!=server.epoch)continue;
                if(std::find(registration->characteristics.begin(),registration->characteristics.end(),*attribute)!=registration->characteristics.end())registration->pending.emplace_back(*attribute,event.value);
            }
        }
        for(auto item=jobs_.begin();item!=jobs_.end();)if(item->second.poll())item=jobs_.erase(item);else ++item;
    }
    Result<Found<Radio>> first_radio(){
        if(closed_)return {hresult(995),{}};const auto snapshot=backend_->radios();if(snapshot.error)return {snapshot.error,{}};if(snapshot.value.empty())return {hresult(259),{}};
        const auto find=allocate(),radio=allocate();if(find==radio)throw std::bad_alloc{};
        auto endpoint=std::make_shared<Endpoint>();endpoint->kind=Kind::Radio;endpoint->radio=snapshot.value.front().id;
        Cursor cursor;cursor.radio=true;cursor.radios=snapshot.value;
        cursors_.emplace(find,std::move(cursor));try{handles_.emplace(radio,std::move(endpoint));}catch(...){cursors_.erase(find);throw;}
        return {0,{find,radio,snapshot.value.front()}};
    }
    Result<Found<Radio>> next_radio(Handle find){
        const auto found=cursors_.find(find);if(found==cursors_.end() || !found->second.radio)return {hresult(6),{}};auto& cursor=found->second;
        if(cursor.next>=cursor.radios.size())return {hresult(259),{}};const auto& value=cursor.radios[cursor.next];const auto radio=allocate();
        auto endpoint=std::make_shared<Endpoint>();endpoint->kind=Kind::Radio;endpoint->radio=value.id;handles_.emplace(radio,std::move(endpoint));++cursor.next;return {0,{find,radio,value}};
    }
    Result<Handle> open_radio(const DeviceId& id){
        const auto snapshot=backend_->radios();if(snapshot.error)return {snapshot.error,0};
        if(std::none_of(snapshot.value.begin(),snapshot.value.end(),[&](const auto& value){return value.id==id;}))return {hresult(1167),0};
        const auto handle=allocate();auto value=std::make_shared<Endpoint>();value->kind=Kind::Radio;value->radio=id;handles_.emplace(handle,std::move(value));return {0,handle};
    }
    Result<bool> close_search(Handle find,bool radio){const auto found=cursors_.find(find);if(found==cursors_.end() || found->second.radio!=radio)return {hresult(6),false};cursors_.erase(found);return {0,true};}
    Result<Radio> radio_info(Handle handle)const{
        const auto found=handles_.find(handle);if(found==handles_.end() || found->second->kind!=Kind::Radio)return {hresult(6),{}};
        const auto snapshot=backend_->radios();if(snapshot.error)return {snapshot.error,{}};
        const auto radio=std::find_if(snapshot.value.begin(),snapshot.value.end(),[&](const auto& value){return value.id==found->second->radio;});return radio==snapshot.value.end()?Result<Radio>{hresult(1167),{}}:Result<Radio>{0,*radio};
    }
    Result<bool> radio_state(Handle handle,bool discovery)const{
        const auto snapshot=handle?[&]{const auto info=radio_info(handle);return Result<std::vector<Radio>>{info.error,info.error?std::vector<Radio>{}:std::vector<Radio>{info.value}};}():backend_->radios();
        if(snapshot.error)return {snapshot.error,false};bool unknown=false;
        for(const auto& value:snapshot.value){const bool known=discovery?value.discoverable_known:value.connectable_known;const bool enabled=discovery?value.discoverable:value.connectable;if(known && enabled)return {0,true};unknown|=!known;}
        return {unknown?hresult(50):0U,false};
    }
    Pending<bool> set_radio(Handle handle,bool discovery,bool enabled){
        const auto snapshot=handle?[&]{const auto info=radio_info(handle);return Result<std::vector<Radio>>{info.error,info.error?std::vector<Radio>{}:std::vector<Radio>{info.value}};}():backend_->radios();
        if(snapshot.error)return immediate<bool>(snapshot.error);if(snapshot.value.empty())return immediate<bool>(hresult(259));
        struct Group {std::vector<Pending<bool>> requests;};auto group=std::make_shared<Group>();
        for(const auto& radio:snapshot.value){
            if((discovery && enabled && radio.connectable_known && !radio.connectable) || (!discovery && !enabled && radio.discoverable_known && radio.discoverable))group->requests.push_back(immediate<bool>(hresult(87)));
            else group->requests.push_back(discovery?backend_->discoverable(radio.id,enabled):backend_->connectable(radio.id,enabled));
        }
        auto result=std::make_shared<Request<bool>>([group]{for(const auto& request:group->requests)request->cancel();});
        Job job;job.owner=handle;job.cancel=[group,result]{for(const auto& request:group->requests)request->cancel();result->complete({hresult(995),false});};
        job.poll=[group,result]{if(result->poll())return true;bool any=false;std::uint32_t error=hresult(31);for(const auto& request:group->requests){const auto value=request->poll();if(!value)return false;if(!value->error && value->value)any=true;else if(value->error)error=value->error;}result->complete({any?0U:error,any});return true;};jobs_.emplace(next_job_++,std::move(job));return result;
    }
    Pending<Found<Device>> first_device(Search search,Handle radio=0){
        if(radio){const auto info=radio_info(radio);if(info.error)return immediate<Found<Device>>(info.error);search.radio=info.value.id;}
        if(search.inquiry && (!search.timeout_multiplier || search.timeout_multiplier>48))return immediate<Found<Device>>(hresult(87));
        return collect<std::vector<Device>,Found<Device>>(radio,{},backend_->devices(search),[this,search](const auto& result)->Result<Found<Device>>{
            if(result.error)return {result.error,{}};std::vector<Device> values;for(const auto& device:result.value)if(search.matches(device))values.push_back(device);if(values.empty())return {hresult(259),{}};
            const auto find=allocate();Cursor cursor;cursor.devices=std::move(values);const auto first=cursor.devices.front();cursors_.emplace(find,std::move(cursor));return {0,{find,0,first}};
        });
    }
    Result<Found<Device>> next_device(Handle find){const auto found=cursors_.find(find);if(found==cursors_.end() || found->second.radio)return {hresult(6),{}};auto& cursor=found->second;if(cursor.next>=cursor.devices.size())return {hresult(259),{}};return {0,{find,0,cursor.devices[cursor.next++]}};}
    Pending<std::vector<Device>> discover(unsigned milliseconds){return collect<std::vector<Device>,std::vector<Device>>(0,{},backend_->low_energy_devices(milliseconds),[this](const auto& result)->Result<std::vector<Device>>{
        if(result.error)return {result.error,{}};for(const auto& value:result.value){auto& server=servers_[value.id];if(!server)server=std::make_shared<Server>();server->info=value;}return result;
    });}
    Result<Handle> open_device(const DeviceId& id,unsigned access){
        const auto found=servers_.find(id);if(found==servers_.end())return {hresult(2),0};if(access&~3U)return {hresult(87),0};
        const auto handle=allocate();auto item=std::make_shared<Endpoint>();item->kind=Kind::Device;item->server=found->second;item->access=access;handles_.emplace(handle,std::move(item));return {0,handle};
    }
    Result<Handle> open_service(Handle handle,const Service& value,unsigned access){
        if(const auto error=endpoint_error(handle))return {error,0};const auto parent=endpoint(handle);if(const auto error=scope(*parent,value.handle))return {error,0};if(!known(*parent->server,value))return {att_error(1),0};if((access&~3U) || (access&parent->access)!=access)return {hresult(5),0};
        const auto output=allocate();auto item=std::make_shared<Endpoint>(*parent);item->kind=Kind::Service;item->service=value;item->epoch=parent->server->epoch;item->access=access;handles_.emplace(output,std::move(item));return {0,output};
    }
    Result<Handle> duplicate(Handle handle,std::optional<unsigned> access={}){
        const auto found=handles_.find(handle);if(found==handles_.end())return {hresult(6),0};
        if(access && found->second->kind!=Kind::Radio && ((*access&~3U) || (*access&found->second->access)!=*access))return {hresult(5),0};
        auto item=found->second;if(access && item->kind!=Kind::Radio){item=std::make_shared<Endpoint>(*item);item->access=*access;}
        const auto value=allocate();handles_.emplace(value,std::move(item));return {0,value};
    }
    Result<bool> close(Handle handle){
        pump();const auto found=handles_.find(handle);if(found==handles_.end())return {hresult(6),false};handles_.erase(found);
        for(auto item=jobs_.begin();item!=jobs_.end();){if(item->second.owner==handle){item->second.cancel();item=jobs_.erase(item);}else ++item;}return {0,true};
    }
    Pending<std::vector<Service>> services(Handle handle,std::uint32_t flags=0){
        if(flags)return immediate<std::vector<Service>>(hresult(87));if(const auto error=endpoint_error(handle))return immediate<std::vector<Service>>(error);const auto item=endpoint(handle);const auto server=item->server;
        if(server->services){if(item->service)return immediate<std::vector<Service>>(0,{*item->service});return immediate<std::vector<Service>>(0,*server->services);}
        return collect<std::vector<Service>,std::vector<Service>>(handle,server,backend_->services(server->info.id),[server](const auto& result)->Result<std::vector<Service>>{if(!result.error)server->services=result.value;return result;});
    }
    Pending<std::vector<Characteristic>> characteristics(Handle handle,std::optional<Service> parent,std::uint32_t flags=0){
        if(flags)return immediate<std::vector<Characteristic>>(hresult(87));if(const auto error=endpoint_error(handle))return immediate<std::vector<Characteristic>>(error);const auto item=endpoint(handle);if(!parent)parent=item->service;if(!parent)return immediate<std::vector<Characteristic>>(hresult(87));
        if(const auto error=scope(*item,parent->handle))return immediate<std::vector<Characteristic>>(error);const auto server=item->server;if(!known(*server,*parent))return immediate<std::vector<Characteristic>>(att_error(1));
        if(const auto found=server->characteristics.find(parent->handle);found!=server->characteristics.end())return immediate<std::vector<Characteristic>>(0,found->second);
        return collect<std::vector<Characteristic>,std::vector<Characteristic>>(handle,server,backend_->characteristics(server->info.id,*parent),[server,parent](const auto& result)->Result<std::vector<Characteristic>>{if(!result.error)server->characteristics[parent->handle]=result.value;return result;});
    }
    Pending<std::vector<Service>> included_services(Handle handle,std::optional<Service> parent,std::uint32_t flags=0){
        if(flags)return immediate<std::vector<Service>>(hresult(87));if(const auto error=endpoint_error(handle))return immediate<std::vector<Service>>(error);const auto item=endpoint(handle);if(!parent)parent=item->service;if(!parent)return immediate<std::vector<Service>>(hresult(87));
        if(const auto error=scope(*item,parent->handle))return immediate<std::vector<Service>>(error);const auto server=item->server;if(!known(*server,*parent))return immediate<std::vector<Service>>(att_error(1));
        if(const auto found=server->included.find(parent->handle);found!=server->included.end())return immediate<std::vector<Service>>(0,found->second);
        return collect<std::vector<Service>,std::vector<Service>>(handle,server,backend_->included_services(server->info.id,*parent),[server,parent](const auto& result)->Result<std::vector<Service>>{
            if(result.error)return result;if(result.value.size()>UINT16_MAX)return {hresult(1450),{}};std::map<std::uint16_t,Uuid> handles;
            for(const auto& value:result.value){
                if(!value.handle || descends(*server,value.handle,parent->handle))return {hresult(22),{}};
                const auto [held,inserted]=handles.emplace(value.handle,value.uuid);if(!inserted && held->second!=value.uuid)return {hresult(22),{}};
                if(server->services)for(const auto& existing:*server->services)if(existing.handle==value.handle && existing!=value)return {hresult(22),{}};
                for(const auto& [ancestor,items]:server->included){(void)ancestor;for(const auto& existing:items)if(existing.handle==value.handle && existing!=value)return {hresult(22),{}};}
            }
            server->included[parent->handle]=result.value;return result;
        });
    }
    Pending<std::vector<Descriptor>> descriptors(Handle handle,const Characteristic& value,std::uint32_t flags=0){
        if(flags)return immediate<std::vector<Descriptor>>(hresult(87));if(const auto error=endpoint_error(handle))return immediate<std::vector<Descriptor>>(error);const auto item=endpoint(handle);if(const auto error=scope(*item,value.service))return immediate<std::vector<Descriptor>>(error);const auto server=item->server;if(!known(*server,value))return immediate<std::vector<Descriptor>>(att_error(1));
        if(const auto found=server->descriptors.find(value.handle);found!=server->descriptors.end())return immediate<std::vector<Descriptor>>(0,found->second);
        return collect<std::vector<Descriptor>,std::vector<Descriptor>>(handle,server,backend_->descriptors(server->info.id,value),[server,value](const auto& result)->Result<std::vector<Descriptor>>{if(!result.error)server->descriptors[value.handle]=result.value;return result;});
    }
    Pending<Bytes> read(Handle handle,const Characteristic& value,std::uint32_t flags=0){
        if((flags&~15U) || (flags&12U)==12U)return immediate<Bytes>(hresult(87));if(const auto error=endpoint_error(handle))return immediate<Bytes>(error);const auto item=endpoint(handle);if(const auto error=scope(*item,value.service))return immediate<Bytes>(error);const auto server=item->server;if(!known(*server,value))return immediate<Bytes>(att_error(1));
        if(!(value.properties&Characteristic::Read))return immediate<Bytes>(hresult(1314));
        if(!(flags&7U)){const auto cached=server->values.find(value.handle);if(cached!=server->values.end())return immediate<Bytes>(0,cached->second);if(flags&8U)return immediate<Bytes>(hresult(1));}
        return collect<Bytes,Bytes>(handle,server,backend_->read(server->info.id,value,flags),[server,value](const auto& result)->Result<Bytes>{if(!result.error){if(result.value.size()>512)return {att_error(13),{}};server->values[value.handle]=result.value;}return result;});
    }
    Pending<Bytes> read_descriptor(Handle handle,const Descriptor& value,std::uint32_t flags=0){
        if((flags&~15U) || (flags&12U)==12U)return immediate<Bytes>(hresult(87));if(const auto error=endpoint_error(handle))return immediate<Bytes>(error);const auto item=endpoint(handle);if(const auto error=scope(*item,value.service))return immediate<Bytes>(error);const auto server=item->server;if(!known(*server,value))return immediate<Bytes>(att_error(1));
        if(!(flags&7U)){const auto found=server->descriptor_values.find(value.handle);if(found!=server->descriptor_values.end())return immediate<Bytes>(0,found->second);if(flags&8U)return immediate<Bytes>(hresult(1));}
        return collect<Bytes,Bytes>(handle,server,backend_->read_descriptor(server->info.id,value,flags),[server,value](const auto& result)->Result<Bytes>{
            if(result.error)return result;const auto size=result.value.size();if(size>512)return {att_error(13),{}};
            if(((value.type==0 || value.type==2 || value.type==3) && size!=2) || (value.type==4 && size!=7) || (value.type==5 && size%2) || value.type>6)return {hresult(58),{}};
            server->descriptor_values[value.handle]=result.value;return result;
        });
    }
    Pending<bool> write(Handle handle,const Characteristic& value,Bytes data,std::uint32_t flags=0,std::uint64_t reliable=0){
        if((flags&~51U) || reliable)return immediate<bool>(hresult(87));if(data.size()>512)return immediate<bool>(att_error(13));if(const auto error=endpoint_error(handle,write_access))return immediate<bool>(error);const auto item=endpoint(handle,write_access);if(const auto error=scope(*item,value.service))return immediate<bool>(error);const auto server=item->server;if(!known(*server,value))return immediate<bool>(att_error(1));
        const auto permission=(flags&16U)?Characteristic::SignedWrite:(flags&32U)?Characteristic::WriteWithoutResponse:Characteristic::Write;
        if(!(value.properties&permission))return immediate<bool>(att_error(3));
        return collect<bool,bool>(handle,server,backend_->write(server->info.id,value,std::move(data),flags),[server,value](const auto& result)->Result<bool>{if(!result.error)server->values.erase(value.handle);return result;});
    }
    Pending<bool> write_descriptor(Handle handle,const Descriptor& value,Bytes data,std::uint32_t flags=0){
        if(flags&~3U)return immediate<bool>(hresult(87));if(data.size()>512)return immediate<bool>(att_error(13));if(const auto error=endpoint_error(handle,write_access))return immediate<bool>(error);const auto item=endpoint(handle,write_access);if(const auto error=scope(*item,value.service))return immediate<bool>(error);const auto server=item->server;if(!known(*server,value))return immediate<bool>(att_error(1));
        return collect<bool,bool>(handle,server,backend_->write_descriptor(server->info.id,value,std::move(data),flags),[server,value](const auto& result)->Result<bool>{if(!result.error)server->descriptor_values.erase(value.handle);return result;});
    }
    Result<Handle> register_event(Handle handle,std::vector<Characteristic> attributes,Handle callback,Handle context,std::uint32_t flags=0){
        if(flags || !callback || attributes.empty() || attributes.size()>65535)return {hresult(87),0};if(const auto error=endpoint_error(handle))return {error,0};const auto item=endpoint(handle);if(!item->service)return {hresult(87),0};
        for(const auto& value:attributes){if(const auto error=scope(*item,value.service))return {error,0};if(!known(*item->server,value))return {att_error(1),0};}
        const auto id=allocate();auto registration=std::make_shared<Registration>();registration->handle=id;registration->callback=callback;registration->context=context;registration->endpoint=item;registration->epoch=item->server->epoch;registration->characteristics=std::move(attributes);registrations_.emplace(id,std::move(registration));return {0,id};
    }
    Pending<bool> unregister_event(Handle handle,std::uint32_t flags=0,Handle callback_token=0){
        if(flags)return immediate<bool>(hresult(87));const auto found=registrations_.find(handle);if(found==registrations_.end() || found->second->closing)return immediate<bool>(hresult(87));
        auto registration=found->second;auto result=std::make_shared<Request<bool>>();registration->closing=true;registration->pending.clear();registration->exclude=registration->active.contains(callback_token)?callback_token:0;registration->unregister=result;retire(registration);return result;
    }
    std::optional<Delivery> take_delivery(){
        pump();for(auto& [handle,registration]:registrations_){(void)handle;if(registration->closing || registration->invalidated || registration->pending.empty())continue;
            const auto token=next_delivery_++;deliveries_.emplace(token,registration);try{registration->active.insert(token);}catch(...){deliveries_.erase(token);throw;}
            auto value=std::move(registration->pending.front());registration->pending.pop_front();return Delivery{registration->handle,token,registration->callback,registration->context,registration->endpoint->server->info.id,value.first,std::move(value.second)};
        }return {};
    }
    bool finish_delivery(Handle token){const auto found=deliveries_.find(token);if(found==deliveries_.end())return false;auto registration=found->second;deliveries_.erase(found);registration->active.erase(token);retire(registration);return true;}
};
}
