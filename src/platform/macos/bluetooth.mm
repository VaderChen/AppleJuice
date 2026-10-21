#include "x64arm/bluetooth.hpp"
#import <Foundation/Foundation.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <IOBluetooth/IOBluetooth.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <utility>

namespace x64arm::bluetooth::native {struct State;}
@interface X64ARMBluetoothDelegate:NSObject<CBCentralManagerDelegate,CBPeripheralDelegate>
{@public std::weak_ptr<x64arm::bluetooth::native::State> owner;}
@end

namespace x64arm::bluetooth::native {
std::u16string text(NSString* value){
    if(!value)return {};std::u16string result(value.length,0);[value getCharacters:reinterpret_cast<unichar*>(result.data()) range:NSMakeRange(0,value.length)];return result;
}
DeviceId identifier(CBPeripheral* value){return value.identifier.UUIDString.UTF8String?:"";}
DeviceId normalized_id(const DeviceId& value){NSUUID* uuid=[[NSUUID alloc]initWithUUIDString:[NSString stringWithUTF8String:value.c_str()]];return uuid?DeviceId(uuid.UUIDString.UTF8String):value;}
std::uint64_t address(NSString* value){
    if(!value)return 0;std::uint64_t result=0;unsigned count=0;
    for(const char* p=value.UTF8String;p && *p;++p){unsigned digit;if(*p>='0' && *p<='9')digit=static_cast<unsigned>(*p-'0');else if(*p>='a' && *p<='f')digit=static_cast<unsigned>(*p-'a'+10);else if(*p>='A' && *p<='F')digit=static_cast<unsigned>(*p-'A'+10);else if(*p==':' || *p=='-')continue;else return 0;if(++count>12)return 0;result=(result<<4)|digit;}
    return count==12?result:0;
}
Uuid uuid(CBUUID* value){
    const auto length=value.data.length;const auto* p=static_cast<const std::uint8_t*>(value.data.bytes);
    if(length==2)return Uuid::short_uuid(static_cast<std::uint16_t>((unsigned(p[0])<<8)|p[1]));
    Uuid result;
    if(length==4){result=Uuid::short_uuid(0);for(unsigned i=0;i<4;++i)result.bytes[i]=p[3-i];return result;}
    if(length==16){constexpr unsigned order[]{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};for(unsigned i=0;i<16;++i)result.bytes[i]=p[order[i]];}
    return result;
}
Bytes bytes(NSData* value){if(!value || !value.length)return {};const auto* first=static_cast<const std::uint8_t*>(value.bytes);return {first,first+value.length};}
Result<Bytes> descriptor_bytes(id value,std::uint32_t type){
    Bytes result;
    if([value isKindOfClass:[NSData class]]){NSData* data=value;if(data.length>512)return {att_error(13),{}};result=bytes(data);}
    else if(type==1 && [value isKindOfClass:[NSString class]]){NSData* data=[value dataUsingEncoding:NSUTF8StringEncoding];if(!data)return {hresult(58),{}};if(data.length>512)return {att_error(13),{}};result=bytes(data);}
    else if((type==0 || type==2 || type==3) && [value isKindOfClass:[NSNumber class]]){const auto word=[value unsignedLongLongValue];if(word>UINT16_MAX)return {hresult(58),{}};result={static_cast<std::uint8_t>(word),static_cast<std::uint8_t>(word>>8)};}
    else return {hresult(58),{}};
    if(((type==0 || type==2 || type==3) && result.size()!=2) || (type==4 && result.size()!=7) || (type==5 && result.size()%2) || type>6)return {hresult(58),{}};
    return {0,std::move(result)};
}
std::uint32_t error(NSError* value){
    if(!value)return 0;if([value.domain isEqualToString:CBATTErrorDomain]){const auto code=static_cast<std::uint32_t>(value.code);return !code?0U:att_error(code<=0x11U?code:0x1000U);}
    if([value.domain isEqualToString:CBErrorDomain])switch(value.code){
        case CBErrorInvalidParameters:return hresult(87);case CBErrorInvalidHandle:return att_error(1);
        case CBErrorNotConnected:case CBErrorPeripheralDisconnected:case CBErrorUnknownDevice:return hresult(1167);
        case CBErrorOutOfSpace:case CBErrorConnectionLimitReached:return hresult(1450);
        case CBErrorOperationCancelled:return hresult(995);case CBErrorConnectionTimeout:case CBErrorEncryptionTimedOut:return hresult(121);
        case CBErrorOperationNotSupported:return hresult(50);case CBErrorPeerRemovedPairingInformation:return att_error(5);
        default:break;
    }
    return hresult(58);
}
std::array<std::uint16_t,8> timestamp(NSDate* value){
    if(!value)return {};NSCalendar* calendar=[[NSCalendar alloc]initWithCalendarIdentifier:NSCalendarIdentifierGregorian];calendar.timeZone=[NSTimeZone timeZoneForSecondsFromGMT:0];
    const auto mask=NSCalendarUnitYear|NSCalendarUnitMonth|NSCalendarUnitWeekday|NSCalendarUnitDay|NSCalendarUnitHour|NSCalendarUnitMinute|NSCalendarUnitSecond|NSCalendarUnitNanosecond;
    NSDateComponents* c=[calendar components:mask fromDate:value];return {static_cast<std::uint16_t>(c.year),static_cast<std::uint16_t>(c.month),static_cast<std::uint16_t>(c.weekday-1),static_cast<std::uint16_t>(c.day),static_cast<std::uint16_t>(c.hour),static_cast<std::uint16_t>(c.minute),static_cast<std::uint16_t>(c.second),static_cast<std::uint16_t>(c.nanosecond/1000000)};
}
std::uint8_t properties(CBCharacteristicProperties value){
    unsigned result=0;
    for(const auto& item:std::array<std::pair<CBCharacteristicProperties,unsigned>,8>{{
        {CBCharacteristicPropertyBroadcast,Characteristic::Broadcast},{CBCharacteristicPropertyRead,Characteristic::Read},
        {CBCharacteristicPropertyWrite,Characteristic::Write},{CBCharacteristicPropertyWriteWithoutResponse,Characteristic::WriteWithoutResponse},
        {CBCharacteristicPropertyAuthenticatedSignedWrites,Characteristic::SignedWrite},{CBCharacteristicPropertyNotify,Characteristic::Notify},
        {CBCharacteristicPropertyIndicate,Characteristic::Indicate},{CBCharacteristicPropertyExtendedProperties,Characteristic::Extended}}})if(value&item.first)result|=item.second;
    return static_cast<std::uint8_t>(result);
}
template<class T> Pending<T> completed(std::uint32_t error,T value={}){auto result=std::make_shared<Request<T>>();result->complete({error,std::move(value)});return result;}
enum class Action {Services,Characteristics,Descriptors,Read,Write,DescriptorWrite,Notify,WithoutResponse,IncludedServices,DescriptorRead};
struct Peer;
struct Job {
    std::uint64_t serial{};Action action{};const void* target{};
    std::function<void(Peer&)> start;
    std::function<void(std::uint32_t,Peer&)> finish;
    std::function<bool()> cancelled;
};
struct Peer {
    __strong CBPeripheral* peripheral{nil};bool active{},connecting{},disconnecting{},owns_connection{},scan_observed{};std::uint64_t serial{};
    std::deque<Job> jobs;std::uint32_t next_handle{1};
    struct S {__strong CBService* native{nil};Service value;};
    struct C {__strong CBCharacteristic* native{nil};Characteristic value;std::optional<Bytes> cache;};
    struct D {__strong CBDescriptor* native{nil};Descriptor value;std::optional<Bytes> cache;};
    std::map<std::uint16_t,S> services;std::map<std::uint16_t,C> characteristics;std::map<std::uint16_t,D> descriptors;
    std::map<const void*,std::uint16_t> handles;std::set<std::uint16_t> notifying;
    std::uint16_t handle(id object,unsigned count=1){
        const auto key=(__bridge const void*)object;if(const auto found=handles.find(key);found!=handles.end())return found->second;
        if(!object || next_handle+count>65536)throw std::bad_alloc{};const auto value=static_cast<std::uint16_t>(next_handle);next_handle+=count;handles.emplace(key,value);return value;
    }
    void clear(){services.clear();characteristics.clear();descriptors.clear();handles.clear();notifying.clear(); /* 舊 handle 不重用。 */}
};
struct State:std::enable_shared_from_this<State> {
    __strong dispatch_queue_t queue{dispatch_queue_create("x64arm.bluetooth",DISPATCH_QUEUE_SERIAL)};
    __strong CBCentralManager* central{nil};__strong X64ARMBluetoothDelegate* delegate{nil};
    std::function<std::uint32_t()> authorize;bool initializing{},ready{};std::uint32_t status{};std::uint64_t next_waiter{1},next_job{1};
    std::map<std::uint64_t,std::function<void(std::uint32_t)>> waiters;
    std::map<DeviceId,Peer> peers;std::vector<ValueEvent> events;
    struct Inquiry {Pending<std::vector<Device>> request;Search search;__strong IOBluetoothDeviceInquiry* native{nil};};
    std::map<const void*,Inquiry> inquiries;
    std::map<std::uint64_t,Pending<std::vector<Device>>> classic_waiters;
    std::map<std::uint64_t,Pending<std::vector<Device>>> scans;
    std::uint64_t next_scan{1};
    static inline char queue_key;
    bool closed{};
    explicit State(NativeOptions options){authorize=[options]{
        const auto authorization=CBManager.authorization;
        if(authorization==CBManagerAuthorizationDenied || authorization==CBManagerAuthorizationRestricted)return hresult(5);
        if(authorization==CBManagerAuthorizationNotDetermined){
            NSString* usage=[[NSBundle mainBundle]objectForInfoDictionaryKey:@"NSBluetoothAlwaysUsageDescription"];
            if(!options.request_authorization || ![usage isKindOfClass:[NSString class]] || !usage.length)return hresult(5);
        }return 0U;
    };dispatch_queue_set_specific(queue,&queue_key,this,nullptr);}
    ~State(){
        if(closed)return;
        for(auto& [key,peer]:peers){(void)key;for(auto& job:peer.jobs)job.finish(hresult(995),peer);}
        for(auto& [key,item]:inquiries){(void)key;item.request->complete({hresult(995),{}});IOBluetoothDeviceInquiry* inquiry=item.native;dispatch_async(dispatch_get_main_queue(),^{inquiry.delegate=nil;[inquiry stop];});}
        CBCentralManager* manager=central;NSArray* peripherals=manager?[peers_array() copy]:@[];
        dispatch_async(queue,^{manager.delegate=nil;[manager stopScan];for(CBPeripheral* peripheral in peripherals){peripheral.delegate=nil;[manager cancelPeripheralConnection:peripheral];}});
    }
    NSArray* peers_array(){NSMutableArray* values=[NSMutableArray array];for(auto& [id,peer]:peers){(void)id;if(peer.peripheral)[values addObject:peer.peripheral];}return values;}
    void initialize(){if(delegate)return;delegate=[[X64ARMBluetoothDelegate alloc]init];delegate->owner=weak_from_this();}
    void when_ready(std::function<void(std::uint32_t)> callback){
        if(closed){callback(hresult(995));return;}
        if(ready){callback(status);return;}if(const auto denied=authorize()){callback(denied);return;}
        const auto id=next_waiter++;waiters.emplace(id,std::move(callback));
        if(!initializing){initializing=true;initialize();central=[[CBCentralManager alloc]initWithDelegate:delegate queue:queue options:@{CBCentralManagerOptionShowPowerAlertKey:@NO}];}
        const auto weak=weak_from_this();dispatch_after(dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC),queue,^{if(auto owner_state=weak.lock()){const auto it=owner_state->waiters.find(id);if(it!=owner_state->waiters.end()){auto cb=std::move(it->second);owner_state->waiters.erase(it);cb(hresult(121));}}});
    }
    void state_changed(){
        if(closed)return;
        switch(central.state){case CBManagerStatePoweredOn:status=0;break;case CBManagerStatePoweredOff:status=hresult(1167);break;case CBManagerStateUnauthorized:status=hresult(5);break;case CBManagerStateUnsupported:status=hresult(50);break;default:ready=false;return;}
        ready=true;auto pending=std::move(waiters);waiters.clear();for(auto& [id,callback]:pending){(void)id;callback(status);}
        if(status){for(auto& [id,peer]:peers){fail(peer,status);peer.clear();events.push_back({id,0,{},ValueEvent::Kind::Disconnected});}auto pending_scans=std::move(scans);scans.clear();for(auto& [id,request]:pending_scans){(void)id;request->complete({status,{}});}}
    }
    Peer* peer(const DeviceId& id){
        if(const auto found=peers.find(id);found!=peers.end())return &found->second;
        NSUUID* uuid=[[NSUUID alloc]initWithUUIDString:[NSString stringWithUTF8String:id.c_str()]];if(!uuid)return nullptr;
        NSArray<CBPeripheral*>* found=[central retrievePeripheralsWithIdentifiers:@[uuid]];if(!found.count)return nullptr;
        auto& value=peers[id];value.peripheral=found[0];value.peripheral.delegate=delegate;return &value;
    }
    void adopt(CBPeripheral* peripheral){auto& item=peers[identifier(peripheral)];item.peripheral=peripheral;item.scan_observed=true;peripheral.delegate=delegate;}
    void fail(Peer& peer,std::uint32_t code){auto pending=std::move(peer.jobs);peer.jobs.clear();peer.active=peer.connecting=false;++peer.serial;for(auto& job:pending)job.finish(code,peer);}
    void start(Peer& peer){
        if(peer.active || peer.connecting || peer.disconnecting)return;
        while(!peer.jobs.empty() && peer.jobs.front().cancelled()){auto job=std::move(peer.jobs.front());peer.jobs.pop_front();job.finish(hresult(995),peer);}
        if(peer.jobs.empty())return;
        peer.active=true;const auto serial=peer.serial=peer.jobs.front().serial;const auto id=identifier(peer.peripheral);const auto weak=weak_from_this();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,30*NSEC_PER_SEC),queue,^{if(auto owner_state=weak.lock())owner_state->expire(id,serial);});
        if(peer.peripheral.state!=CBPeripheralStateConnected){peer.connecting=peer.owns_connection=true;[central connectPeripheral:peer.peripheral options:nil];return;}
        auto callback=peer.jobs.front().start;callback(peer);
    }
    void expire(const DeviceId& id,std::uint64_t serial){
        const auto it=peers.find(id);if(it==peers.end() || !it->second.active || it->second.serial!=serial)return;
        auto& item=it->second;fail(item,hresult(121));item.disconnecting=true;[central cancelPeripheralConnection:item.peripheral];
    }
    void shutdown(){
        if(closed)return;closed=true;ready=true;status=hresult(995);
        auto pending=std::move(waiters);waiters.clear();for(auto& [id,callback]:pending){(void)id;callback(status);}
        for(auto& [id,peer]:peers){(void)id;fail(peer,status);for(const auto handle:peer.notifying){const auto found=peer.characteristics.find(handle);if(found!=peer.characteristics.end())[peer.peripheral setNotifyValue:NO forCharacteristic:found->second.native];}peer.peripheral.delegate=nil;if(peer.owns_connection)[central cancelPeripheralConnection:peer.peripheral];peer.clear();}
        for(auto& [id,request]:classic_waiters){(void)id;request->complete({status,{}});}classic_waiters.clear();
        for(auto& [id,item]:inquiries){(void)id;item.request->complete({status,{}});IOBluetoothDeviceInquiry* inquiry=item.native;dispatch_async(dispatch_get_main_queue(),^{inquiry.delegate=nil;[inquiry stop];});}inquiries.clear();
        for(auto& [id,request]:scans){(void)id;request->complete({status,{}});}scans.clear();
        [central stopScan];central.delegate=nil;
    }
    std::vector<Device> le_snapshot()const{
        // CoreBluetooth 也能解析 BR/EDR GATT；僅憑 retrievePeripherals 不能宣稱是 LE 裝置。
        std::vector<Device> result;for(const auto& [id,peer]:peers){if(!peer.scan_observed)continue;Device value;value.id=id;value.name=text(peer.peripheral.name);value.connected=peer.peripheral.state==CBPeripheralStateConnected;value.low_energy=true;result.push_back(std::move(value));}return result;
    }
    void scan_finished(std::uint64_t id){
        const auto found=scans.find(id);if(found==scans.end())return;auto request=found->second;scans.erase(found);if(scans.empty())[central stopScan];request->complete({0,le_snapshot()});
    }
    void cancel_scans(){
        for(auto item=scans.begin();item!=scans.end();){if(item->second->poll())item=scans.erase(item);else ++item;}if(scans.empty())[central stopScan];
    }
    void connected(CBPeripheral* peripheral,NSError* error_value){
        if(closed)return;
        const auto found=peers.find(identifier(peripheral));if(found==peers.end())return;auto& item=found->second;item.connecting=false;
        if(error_value){fail(item,error(error_value));return;}if(item.jobs.empty()){item.active=false;return;}auto callback=item.jobs.front().start;callback(item);
    }
    void disconnected(CBPeripheral* peripheral,NSError* error_value){
        if(closed)return;
        const auto found=peers.find(identifier(peripheral));if(found==peers.end())return;auto& item=found->second;item.disconnecting=false;fail(item,error_value?error(error_value):hresult(1167));item.clear();events.push_back({found->first,0,{},ValueEvent::Kind::Disconnected});
    }
    void respond(CBPeripheral* peripheral,Action action,const void* target,NSError* error_value){
        if(closed)return;
        const auto found=peers.find(identifier(peripheral));if(found==peers.end())return;auto& item=found->second;
        if(!item.active || item.jobs.empty() || item.jobs.front().action!=action || item.jobs.front().target!=target)return;
        auto job=std::move(item.jobs.front());item.jobs.pop_front();item.active=false;
        try{job.finish(error(error_value),item);}catch(const std::bad_alloc&){job.finish(hresult(1450),item);}start(item);
    }
    void enqueue(const DeviceId& id,Job job){
        when_ready([owner_state=shared_from_this(),id,job=std::move(job)](std::uint32_t code)mutable{
            if(code){Peer empty;job.finish(code,empty);return;}auto* item=owner_state->peer(id);if(!item){Peer empty;job.finish(hresult(1167),empty);return;}
            if(item->disconnecting){job.finish(hresult(1167),*item);return;}job.serial=owner_state->next_job++;item->jobs.push_back(std::move(job));owner_state->start(*item);
        });
    }
    std::uint16_t remember(CBService* service,Peer& peer){const auto id=peer.handle(service);peer.services.insert_or_assign(id,Peer::S{service,{uuid(service.UUID),id}});return id;}
    std::uint16_t remember(CBCharacteristic* characteristic,Peer& peer){
        const auto service=remember(characteristic.service,peer),id=peer.handle(characteristic,2);const Characteristic value{service,uuid(characteristic.UUID),id,static_cast<std::uint16_t>(id+1),properties(characteristic.properties)};
        if(auto found=peer.characteristics.find(id);found!=peer.characteristics.end()){found->second.native=characteristic;found->second.value=value;}else peer.characteristics.emplace(id,Peer::C{characteristic,value,{}});return id;
    }
    std::uint16_t remember(CBDescriptor* descriptor,Peer& peer){
        const auto characteristic=remember(descriptor.characteristic,peer),id=peer.handle(descriptor);const auto value=uuid(descriptor.UUID);const auto short_id=value.short_value();const auto type=short_id && *short_id>=0x2900 && *short_id<=0x2905?*short_id-0x2900:6U;
        const Descriptor info{peer.characteristics.at(characteristic).value.service,characteristic,type,value,id};
        if(auto found=peer.descriptors.find(id);found!=peer.descriptors.end()){found->second.native=descriptor;found->second.value=info;}else peer.descriptors.emplace(id,Peer::D{descriptor,info,{}});return id;
    }
    void value(CBPeripheral* peripheral,CBCharacteristic* characteristic,NSError* error_value){
        if(closed)return;
        const auto found=peers.find(identifier(peripheral));if(found==peers.end())return;auto& item=found->second;
        const auto known=item.handles.find((__bridge const void*)characteristic);if(known==item.handles.end())return;const auto handle=known->second;
        if(!error_value){auto& attribute=item.characteristics.at(handle);attribute.cache=bytes(characteristic.value);
            const bool requested_read=item.active && !item.jobs.empty() && item.jobs.front().action==Action::Read && item.jobs.front().target==(__bridge const void*)characteristic;
            if(!requested_read && item.notifying.contains(handle))events.push_back({found->first,handle,*attribute.cache});}
        respond(peripheral,Action::Read,(__bridge const void*)characteristic,error_value);
    }
    void changed(CBPeripheral* peripheral){if(closed)return;const auto found=peers.find(identifier(peripheral));if(found==peers.end())return;fail(found->second,hresult(22));found->second.clear();events.push_back({found->first,0,{},ValueEvent::Kind::ServicesChanged});}
};
#include "bluetooth_native_backend.inc"
}

@implementation X64ARMBluetoothDelegate
- (void)centralManagerDidUpdateState:(CBCentralManager*)central{(void)central;if(auto owner_state=owner.lock())owner_state->state_changed();}
- (void)centralManager:(CBCentralManager*)central didDiscoverPeripheral:(CBPeripheral*)peripheral advertisementData:(NSDictionary*)data RSSI:(NSNumber*)rssi{(void)central;(void)data;(void)rssi;if(auto owner_state=owner.lock())owner_state->adopt(peripheral);}
- (void)centralManager:(CBCentralManager*)central didConnectPeripheral:(CBPeripheral*)peripheral{(void)central;if(auto owner_state=owner.lock())owner_state->connected(peripheral,nil);}
- (void)centralManager:(CBCentralManager*)central didFailToConnectPeripheral:(CBPeripheral*)peripheral error:(NSError*)error{(void)central;if(auto owner_state=owner.lock())owner_state->connected(peripheral,error);}
- (void)centralManager:(CBCentralManager*)central didDisconnectPeripheral:(CBPeripheral*)peripheral error:(NSError*)error{(void)central;if(auto owner_state=owner.lock())owner_state->disconnected(peripheral,error);}
- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error{if(auto owner_state=owner.lock())owner_state->respond(peripheral,x64arm::bluetooth::native::Action::Services,nullptr,error);}
- (void)peripheral:(CBPeripheral*)peripheral didDiscoverIncludedServicesForService:(CBService*)service error:(NSError*)error{if(auto owner_state=owner.lock())owner_state->respond(peripheral,x64arm::bluetooth::native::Action::IncludedServices,(__bridge const void*)service,error);}
- (void)peripheral:(CBPeripheral*)peripheral didDiscoverCharacteristicsForService:(CBService*)service error:(NSError*)error{if(auto owner_state=owner.lock())owner_state->respond(peripheral,x64arm::bluetooth::native::Action::Characteristics,(__bridge const void*)service,error);}
- (void)peripheral:(CBPeripheral*)peripheral didDiscoverDescriptorsForCharacteristic:(CBCharacteristic*)characteristic error:(NSError*)error{if(auto owner_state=owner.lock())owner_state->respond(peripheral,x64arm::bluetooth::native::Action::Descriptors,(__bridge const void*)characteristic,error);}
- (void)peripheral:(CBPeripheral*)peripheral didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic error:(NSError*)error{if(auto owner_state=owner.lock())owner_state->value(peripheral,characteristic,error);}
- (void)peripheral:(CBPeripheral*)peripheral didUpdateValueForDescriptor:(CBDescriptor*)descriptor error:(NSError*)error{if(auto owner_state=owner.lock())owner_state->respond(peripheral,x64arm::bluetooth::native::Action::DescriptorRead,(__bridge const void*)descriptor,error);}
- (void)peripheral:(CBPeripheral*)peripheral didWriteValueForCharacteristic:(CBCharacteristic*)characteristic error:(NSError*)error{if(auto owner_state=owner.lock())owner_state->respond(peripheral,x64arm::bluetooth::native::Action::Write,(__bridge const void*)characteristic,error);}
- (void)peripheral:(CBPeripheral*)peripheral didWriteValueForDescriptor:(CBDescriptor*)descriptor error:(NSError*)error{if(auto owner_state=owner.lock())owner_state->respond(peripheral,x64arm::bluetooth::native::Action::DescriptorWrite,(__bridge const void*)descriptor,error);}
- (void)peripheral:(CBPeripheral*)peripheral didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic error:(NSError*)error{if(auto owner_state=owner.lock())owner_state->respond(peripheral,x64arm::bluetooth::native::Action::Notify,(__bridge const void*)characteristic,error);}
- (void)peripheral:(CBPeripheral*)peripheral didModifyServices:(NSArray<CBService*>*)services{(void)services;if(auto owner_state=owner.lock())owner_state->changed(peripheral);}
- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral*)peripheral{if(auto owner_state=owner.lock()){const auto found=owner_state->peers.find(x64arm::bluetooth::native::identifier(peripheral));if(found!=owner_state->peers.end() && found->second.active && !found->second.jobs.empty() && found->second.jobs.front().action==x64arm::bluetooth::native::Action::WithoutResponse)found->second.jobs.front().start(found->second);}}
- (void)deviceInquiryComplete:(IOBluetoothDeviceInquiry*)inquiry error:(IOReturn)error aborted:(BOOL)aborted{
    if(auto owner_state=owner.lock()){const auto weak=owner;dispatch_async(owner_state->queue,^{if(auto state=weak.lock())x64arm::bluetooth::native::finish_inquiry(*state,inquiry,error,aborted);});}
}
@end

namespace x64arm::bluetooth {std::shared_ptr<Backend> make_bluetooth_backend(NativeOptions options){return std::make_shared<native::NativeBackend>(std::make_shared<native::State>(options));}}
