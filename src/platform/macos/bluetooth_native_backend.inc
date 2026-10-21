// IOBluetooth／CoreBluetooth 共同後端；所有 GATT 作業在同一個原生序列佇列完成。
Result<std::vector<Radio>> radio_snapshot(){
    IOBluetoothHostController* controller=[IOBluetoothHostController defaultController];if(!controller)return {};
    NSString* raw=[controller addressAsString];const auto numeric=address(raw);if(!numeric)return {hresult(1167),{}};
    Radio result;result.id="local:"+std::string(raw.UTF8String?:"");result.address=numeric;result.name=text([controller nameAsString]);result.device_class=controller.classOfDevice;
    result.powered=controller.powerState==kBluetoothHCIPowerStateON;result.manufacturer=0xffff;
    // 公開 IOBluetooth API 沒有 scan-enable 的查詢／設定，不把 powerState 當成 connectable。
    return {0,{std::move(result)}};
}
std::vector<Device> classic_snapshot(const Search& search,NSArray* found){
    const auto radios=radio_snapshot();if(radios.error || radios.value.empty())return {};
    const auto& radio=radios.value.front();std::map<std::uint64_t,Device> result;
    const NSArray* groups[]={ [IOBluetoothDevice pairedDevices],[IOBluetoothDevice recentDevices:4096],found };
    for(const NSArray* group:groups)for(IOBluetoothDevice* device in group){
        const auto* raw=[device getAddress];if(!raw)continue;std::uint64_t number=0;for(unsigned i=0;i<6;++i)number=(number<<8)|raw->data[i];if(!number)continue;
        Device value;value.id="classic:"+std::to_string(number);value.radio=radio.id;value.address=number;value.name=text(device.nameOrAddress);value.device_class=device.classOfDevice;
        value.connected=device.isConnected;value.remembered=device.isPaired;value.authenticated=value.remembered;value.last_seen=timestamp([device getLastInquiryUpdate]);value.last_used=timestamp([device recentAccessDate]);
        if(search.matches(value))result.insert_or_assign(number,std::move(value));
    }
    std::vector<Device> values;for(auto& [address,device]:result){(void)address;values.push_back(std::move(device));}return values;
}
void finish_inquiry(State& state,IOBluetoothDeviceInquiry* inquiry,IOReturn error_value,BOOL aborted){
    const auto found=state.inquiries.find((__bridge const void*)inquiry);if(found==state.inquiries.end())return;
    auto item=std::move(found->second);state.inquiries.erase(found);
    if(error_value || aborted)item.request->complete({hresult(aborted?995:error_value==kIOReturnNotPermitted?5:error_value==kIOReturnTimeout?121:58),{}});
    else item.request->complete({0,classic_snapshot(item.search,[inquiry foundDevices])});
}
class NativeBackend final:public Backend {
    std::shared_ptr<State> state_;
    template<class T,class Setup> Pending<T> operation(const DeviceId& supplied_id,Setup setup){
        const auto id=normalized_id(supplied_id);
        const auto weak=std::weak_ptr<State>(state_);
        auto request=std::make_shared<Request<T>>([weak,id]{if(auto state=weak.lock())dispatch_async(state->queue,^{
            const auto found=state->peers.find(id);if(found==state->peers.end())return;auto& peer=found->second;
            if(peer.active && !peer.jobs.empty() && peer.jobs.front().cancelled()){
                state->fail(peer,hresult(995));peer.disconnecting=true;[state->central cancelPeripheralConnection:peer.peripheral];
            }else state->start(peer);
        });});
        const auto state=state_;dispatch_async(state->queue,^{
            state->when_ready([state,id,request,setup](std::uint32_t code){
                if(request->poll())return;if(code){request->complete({code,{}});return;}
                auto* peer=state->peer(id);if(!peer || peer->disconnecting){request->complete({hresult(1167),{}});return;}
                try{auto job=setup(*peer,request);if(!job)return;job->serial=state->next_job++;job->cancelled=[request]{return request->poll().has_value();};peer->jobs.push_back(std::move(*job));state->start(*peer);}
                catch(const std::bad_alloc&){request->complete({hresult(1450),{}});}
            });
        });return request;
    }
    template<class T> static std::optional<Job> rejected(const Pending<T>& request,std::uint32_t error){request->complete({error,{}});return {};}
    static Peer::C* attribute(Peer& peer,const Characteristic& value){const auto found=peer.characteristics.find(value.handle);return found!=peer.characteristics.end() && found->second.value==value?&found->second:nullptr;}
public:
    explicit NativeBackend(std::shared_ptr<State> state):state_(std::move(state)){}
    ~NativeBackend()override{
        const auto state=state_;if(dispatch_get_specific(&State::queue_key)==state.get())state->shutdown();else dispatch_sync(state->queue,^{state->shutdown();});
    }
    Result<std::vector<Radio>> radios()override{if(const auto denied=state_->authorize())return {denied,{}};return radio_snapshot();}
    Pending<std::vector<Device>> devices(const Search& search)override{
        if(search.inquiry && (!search.timeout_multiplier || search.timeout_multiplier>48))return completed<std::vector<Device>>(hresult(87));
        if(const auto denied=state_->authorize())return completed<std::vector<Device>>(denied);
        const auto radios=radio_snapshot();if(radios.error)return completed<std::vector<Device>>(radios.error);
        if(search.radio && std::none_of(radios.value.begin(),radios.value.end(),[&](const Radio& value){return value.id==*search.radio;}))return completed<std::vector<Device>>(hresult(6));
        if(!search.inquiry)return completed<std::vector<Device>>(0,classic_snapshot(search,nil));
        const auto state=state_;const auto weak=std::weak_ptr<State>(state);auto request=std::make_shared<Request<std::vector<Device>>>([weak]{if(auto state=weak.lock())dispatch_async(state->queue,^{
            for(auto item=state->classic_waiters.begin();item!=state->classic_waiters.end();){if(item->second->poll())item=state->classic_waiters.erase(item);else ++item;}
            for(auto item=state->inquiries.begin();item!=state->inquiries.end();){const auto result=item->second.request->poll();if(result && result->error==hresult(995)){IOBluetoothDeviceInquiry* inquiry=item->second.native;item=state->inquiries.erase(item);dispatch_async(dispatch_get_main_queue(),^{[inquiry stop];});}else ++item;}
        });});
        dispatch_async(state->queue,^{if(state->closed){request->complete({hresult(995),{}});return;}if(request->poll())return;const auto token=state->next_waiter++;state->classic_waiters.emplace(token,request);state->initialize();dispatch_async(dispatch_get_main_queue(),^{
            if(request->poll())return;IOBluetoothDeviceInquiry* inquiry=[IOBluetoothDeviceInquiry inquiryWithDelegate:state->delegate];
            inquiry.inquiryLength=static_cast<std::uint8_t>((search.timeout_multiplier*128U+99U)/100U);inquiry.updateNewDeviceNames=NO;
            dispatch_sync(state->queue,^{state->classic_waiters.erase(token);if(!request->poll())state->inquiries.emplace((__bridge const void*)inquiry,State::Inquiry{request,search,inquiry});});if(request->poll())return;
            const auto status=[inquiry start];if(status)dispatch_async(state->queue,^{finish_inquiry(*state,inquiry,status,NO);});
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(search.timeout_multiplier*128U+500U)*NSEC_PER_SEC/100U),dispatch_get_main_queue(),^{
                if(request->poll())return;[inquiry stop];dispatch_async(state->queue,^{finish_inquiry(*state,inquiry,kIOReturnTimeout,NO);});
            });
        });});return request;
    }
    Pending<std::vector<Device>> low_energy_devices(unsigned milliseconds)override{
        if(milliseconds>60000)return completed<std::vector<Device>>(hresult(87));const auto state=state_;const auto weak=std::weak_ptr<State>(state);
        auto request=std::make_shared<Request<std::vector<Device>>>([weak]{if(auto state=weak.lock())dispatch_async(state->queue,^{state->cancel_scans();});});
        dispatch_async(state->queue,^{state->when_ready([state,request,milliseconds](std::uint32_t code){
            if(request->poll())return;if(code){request->complete({code,{}});return;}if(!milliseconds){request->complete({0,state->le_snapshot()});return;}
            if(state->scans.empty())[state->central scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey:@NO}];
            const auto id=state->next_scan++;state->scans.emplace(id,request);const auto weak=std::weak_ptr<State>(state);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,static_cast<std::int64_t>(milliseconds)*static_cast<std::int64_t>(NSEC_PER_MSEC)),state->queue,^{if(auto active_state=weak.lock())active_state->scan_finished(id);});
        });});return request;
    }
    Pending<bool> discoverable(const DeviceId& id,bool enabled)override{(void)enabled;const auto list=radios();if(list.error)return completed<bool>(list.error);return completed<bool>(std::any_of(list.value.begin(),list.value.end(),[&](const Radio& radio){return radio.id==id;})?hresult(50):hresult(6));}
    Pending<bool> connectable(const DeviceId& id,bool enabled)override{(void)enabled;const auto list=radios();if(list.error)return completed<bool>(list.error);return completed<bool>(std::any_of(list.value.begin(),list.value.end(),[&](const Radio& radio){return radio.id==id;})?hresult(50):hresult(6));}
    Pending<std::vector<Service>> services(const DeviceId& id)override{
        const auto state=state_;return operation<std::vector<Service>>(id,[state](Peer& peer,auto request)->std::optional<Job>{
            (void)peer;
            Job job;job.action=Action::Services;job.start=[](Peer& peer){[peer.peripheral discoverServices:nil];};
            job.finish=[state,request](std::uint32_t code,Peer& peer){std::vector<Service> result;if(!code)for(CBService* service in peer.peripheral.services)if(service.isPrimary)result.push_back(peer.services.at(state->remember(service,peer)).value);request->complete({code,std::move(result)});};return job;
        });
    }
    Pending<std::vector<Service>> included_services(const DeviceId& id,const Service& value)override{
        const auto state=state_;return operation<std::vector<Service>>(id,[state,value](Peer& peer,auto request)->std::optional<Job>{
            const auto found=peer.services.find(value.handle);if(found==peer.services.end() || found->second.value!=value)return rejected(request,att_error(1));CBService* service=found->second.native;
            Job job;job.action=Action::IncludedServices;job.target=(__bridge const void*)service;job.start=[service](Peer& peer){[peer.peripheral discoverIncludedServices:nil forService:service];};
            job.finish=[state,service,request](std::uint32_t code,Peer& peer){std::vector<Service> result;if(!code)for(CBService* child in service.includedServices)result.push_back(peer.services.at(state->remember(child,peer)).value);request->complete({code,std::move(result)});};return job;
        });
    }
    Pending<std::vector<Characteristic>> characteristics(const DeviceId& id,const Service& value)override{
        const auto state=state_;return operation<std::vector<Characteristic>>(id,[state,value](Peer& peer,auto request)->std::optional<Job>{
            const auto found=peer.services.find(value.handle);if(found==peer.services.end() || found->second.value!=value)return rejected(request,att_error(1));CBService* service=found->second.native;
            Job job;job.action=Action::Characteristics;job.target=(__bridge const void*)service;job.start=[service](Peer& peer){[peer.peripheral discoverCharacteristics:nil forService:service];};
            job.finish=[state,service,request](std::uint32_t code,Peer& peer){std::vector<Characteristic> result;if(!code)for(CBCharacteristic* value in service.characteristics)result.push_back(peer.characteristics.at(state->remember(value,peer)).value);request->complete({code,std::move(result)});};return job;
        });
    }
    Pending<std::vector<Descriptor>> descriptors(const DeviceId& id,const Characteristic& value)override{
        const auto state=state_;return operation<std::vector<Descriptor>>(id,[state,value](Peer& peer,auto request)->std::optional<Job>{
            const auto found=attribute(peer,value);if(!found)return rejected(request,att_error(1));CBCharacteristic* characteristic=found->native;
            Job job;job.action=Action::Descriptors;job.target=(__bridge const void*)characteristic;job.start=[characteristic](Peer& peer){[peer.peripheral discoverDescriptorsForCharacteristic:characteristic];};
            job.finish=[state,characteristic,request](std::uint32_t code,Peer& peer){std::vector<Descriptor> result;if(!code)for(CBDescriptor* value in characteristic.descriptors)result.push_back(peer.descriptors.at(state->remember(value,peer)).value);request->complete({code,std::move(result)});};return job;
        });
    }
    Pending<Bytes> read(const DeviceId& id,const Characteristic& value,std::uint32_t flags)override{
        if((flags&~15U) || (flags&12U)==12U)return completed<Bytes>(hresult(87));if(flags&3U)return completed<Bytes>(hresult(50));
        return operation<Bytes>(id,[value,flags](Peer& peer,auto request)->std::optional<Job>{
            const auto found=attribute(peer,value);if(!found)return rejected(request,att_error(1));if(!(value.properties&Characteristic::Read))return rejected(request,hresult(1314));
            if(!(flags&4U) && found->cache){request->complete({0,*found->cache});return {};}
            if(flags&8U)return rejected(request,hresult(1));CBCharacteristic* characteristic=found->native;
            Job job;job.action=Action::Read;job.target=(__bridge const void*)characteristic;job.start=[characteristic](Peer& peer){[peer.peripheral readValueForCharacteristic:characteristic];};
            job.finish=[request,characteristic](std::uint32_t code,Peer& peer){(void)peer;request->complete({code,code?Bytes{}:bytes(characteristic.value)});};return job;
        });
    }
    Pending<Bytes> read_descriptor(const DeviceId& id,const Descriptor& value,std::uint32_t flags)override{
        if((flags&~15U) || (flags&12U)==12U)return completed<Bytes>(hresult(87));if(flags&3U)return completed<Bytes>(hresult(50));
        return operation<Bytes>(id,[value,flags](Peer& peer,auto request)->std::optional<Job>{
            const auto found=peer.descriptors.find(value.handle);if(found==peer.descriptors.end() || found->second.value!=value)return rejected(request,att_error(1));
            if(!(flags&4U) && found->second.cache){request->complete({0,*found->second.cache});return {};}if(flags&8U)return rejected(request,hresult(1));CBDescriptor* descriptor=found->second.native;
            Job job;job.action=Action::DescriptorRead;job.target=(__bridge const void*)descriptor;job.start=[descriptor](Peer& peer){[peer.peripheral readValueForDescriptor:descriptor];};
            job.finish=[request,descriptor,value](std::uint32_t code,Peer& peer){auto result=code?Result<Bytes>{code,{}}:descriptor_bytes(descriptor.value,value.type);if(!result.error)if(const auto item=peer.descriptors.find(value.handle);item!=peer.descriptors.end())item->second.cache=result.value;request->complete(std::move(result));};return job;
        });
    }
    Pending<bool> write(const DeviceId& id,const Characteristic& value,Bytes data,std::uint32_t flags)override{
        if(flags&~51U)return completed<bool>(hresult(87));if(flags&19U)return completed<bool>(hresult(50));if(data.size()>512)return completed<bool>(att_error(13));
        const auto state=state_;return operation<bool>(id,[state,value,data=std::move(data),flags](Peer& peer,auto request)->std::optional<Job>{
            const auto found=attribute(peer,value);if(!found)return rejected(request,att_error(1));const bool without=(flags&32U)!=0;
            if(!(value.properties&(without?Characteristic::WriteWithoutResponse:Characteristic::Write)))return rejected(request,att_error(3));
            const auto mode=without?CBCharacteristicWriteWithoutResponse:CBCharacteristicWriteWithResponse;if(data.size()>[peer.peripheral maximumWriteValueLengthForType:mode])return rejected(request,att_error(13));
            CBCharacteristic* characteristic=found->native;NSData* payload=[NSData dataWithBytes:data.data() length:data.size()];
            Job job;job.action=without?Action::WithoutResponse:Action::Write;job.target=(__bridge const void*)characteristic;
            job.start=[state,characteristic,payload,mode,without](Peer& peer){
                if(without && !peer.peripheral.canSendWriteWithoutResponse)return;
                [peer.peripheral writeValue:payload forCharacteristic:characteristic type:mode];
                if(without)state->respond(peer.peripheral,Action::WithoutResponse,(__bridge const void*)characteristic,nil);
            };
            job.finish=[request,value](std::uint32_t code,Peer& peer){if(!code)if(auto found=peer.characteristics.find(value.handle);found!=peer.characteristics.end())found->second.cache.reset();request->complete({code,!code});};return job;
        });
    }
    Pending<bool> write_descriptor(const DeviceId& id,const Descriptor& value,Bytes data,std::uint32_t flags)override{
        if(flags&~3U)return completed<bool>(hresult(87));if(flags)return completed<bool>(hresult(50));if(data.size()>512)return completed<bool>(att_error(13));
        return operation<bool>(id,[value,data=std::move(data)](Peer& peer,auto request)->std::optional<Job>{
            const auto found=peer.descriptors.find(value.handle);if(found==peer.descriptors.end() || found->second.value!=value)return rejected(request,att_error(1));
            if(value.type==0 || value.type==4 || value.type==5)return rejected(request,att_error(3));
            Job job;
            if(value.type==2){
                if(data.size()!=2 || data[1] || (data[0]&~3U))return rejected(request,att_error(13));const auto characteristic=peer.characteristics.find(value.characteristic);if(characteristic==peer.characteristics.end())return rejected(request,att_error(1));
                const auto properties=characteristic->second.value.properties;if((data[0]&1U) && !(properties&Characteristic::Notify))return rejected(request,att_error(3));if((data[0]&2U) && !(properties&Characteristic::Indicate))return rejected(request,att_error(3));
                if((data[0]&2U) && (properties&Characteristic::Notify))return rejected(request,hresult(50));CBCharacteristic* native=characteristic->second.native;const bool enabled=data[0]!=0;
                job.action=Action::Notify;job.target=(__bridge const void*)native;job.start=[native,enabled](Peer& peer){[peer.peripheral setNotifyValue:enabled forCharacteristic:native];};
                job.finish=[request,value,enabled](std::uint32_t code,Peer& peer){if(!code){if(enabled)peer.notifying.insert(value.characteristic);else peer.notifying.erase(value.characteristic);for(auto& [handle,item]:peer.descriptors){(void)handle;if(item.value.type==2 && item.value.characteristic==value.characteristic)item.cache.reset();}}request->complete({code,!code});};
            }else{
                CBDescriptor* descriptor=found->second.native;NSData* payload=[NSData dataWithBytes:data.data() length:data.size()];job.action=Action::DescriptorWrite;job.target=(__bridge const void*)descriptor;
                job.start=[descriptor,payload](Peer& peer){[peer.peripheral writeValue:payload forDescriptor:descriptor];};job.finish=[request,value](std::uint32_t code,Peer& peer){if(!code)if(const auto found=peer.descriptors.find(value.handle);found!=peer.descriptors.end())found->second.cache.reset();request->complete({code,!code});};
            }return job;
        });
    }
    Pending<bool> notifications(const DeviceId& id,const Characteristic& value,bool enabled)override{
        return operation<bool>(id,[value,enabled](Peer& peer,auto request)->std::optional<Job>{const auto found=attribute(peer,value);if(!found)return rejected(request,att_error(1));
            if(!(value.properties&(Characteristic::Notify|Characteristic::Indicate)))return rejected(request,att_error(6));CBCharacteristic* characteristic=found->native;
            Job job;job.action=Action::Notify;job.target=(__bridge const void*)characteristic;job.start=[characteristic,enabled](Peer& peer){[peer.peripheral setNotifyValue:enabled forCharacteristic:characteristic];};
            job.finish=[request,value,enabled](std::uint32_t code,Peer& peer){if(!code){if(enabled)peer.notifying.insert(value.handle);else peer.notifying.erase(value.handle);for(auto& [handle,item]:peer.descriptors){(void)handle;if(item.value.type==2 && item.value.characteristic==value.handle)item.cache.reset();}}request->complete({code,!code});};return job;
        });
    }
    std::vector<ValueEvent> drain_events()override{
        auto result=std::make_shared<std::vector<ValueEvent>>();const auto state=state_;auto collect=^{*result=std::move(state->events);state->events.clear();};
        if(dispatch_get_specific(&State::queue_key)==state.get())collect();else dispatch_sync(state->queue,collect);return std::move(*result);
    }
};
