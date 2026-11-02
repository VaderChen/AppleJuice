#include "x64arm/wlan.hpp"
#include "x64arm/shared_wake.hpp"
#import <CoreWLAN/CoreWLAN.h>
#include <algorithm>
#include <cstdio>
#include <mutex>

namespace x64arm::wlan {
namespace {
struct Events {std::mutex mutex;std::vector<Event> pending;std::weak_ptr<SharedWake> wake;bool closed{};};
void complete(const std::shared_ptr<Operation>& op,const std::shared_ptr<Events>& events){
    op->done.store(true,std::memory_order_release);std::shared_ptr<SharedWake> wake;
    {std::lock_guard lock(events->mutex);wake=events->wake.lock();}if(wake)wake->notify();
}
std::string text(NSString* value){return value.UTF8String?value.UTF8String:"";}
Bytes bytes(NSData* value){if(!value.length)return {};const auto* p=static_cast<const std::uint8_t*>(value.bytes);return {p,p+value.length};}
std::uint32_t error(NSError* value){
    if(!value)return 31;
    switch(value.code){case kCWInvalidParameterErr:case kCWInvalidFormatErr:return 87;case kCWNoMemoryErr:return 8;
    case kCWNotSupportedErr:case kCWUnsupportedCapabilitiesErr:return 50;case kCWTimeoutErr:return 1460;
    case kCWOperationNotPermittedErr:return 5;default:return 31;}
}
std::uint32_t phy(CWPHYMode mode){switch(mode){case kCWPHYMode11a:return 4;case kCWPHYMode11b:return 5;case kCWPHYMode11g:return 6;case kCWPHYMode11n:return 7;case kCWPHYMode11ac:return 8;case kCWPHYMode11ax:return 10;case kCWPHYMode11be:return 11;default:return 0;}}
std::uint32_t frequency(CWChannel* ch){
    if(!ch)return 0;const auto n=static_cast<std::uint32_t>(ch.channelNumber);
    switch(ch.channelBand){case kCWChannelBand2GHz:return (n==14?2484:2407+5*n)*1000;
    case kCWChannelBand5GHz:return (5000+5*n)*1000;case kCWChannelBand6GHz:return (n==2?5935:5950+5*n)*1000;default:return 0;}
}
bool mac(NSString* value,std::array<std::uint8_t,6>& output){
    if(!value)return false;unsigned b[6]{};char tail{};
    if(std::sscanf(value.UTF8String,"%2x:%2x:%2x:%2x:%2x:%2x%c",&b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&tail)!=6)return false;
    for(unsigned i=0;i<6;++i)output[i]=static_cast<std::uint8_t>(b[i]);return true;
}
void security(Network& n,CWSecurity value){
    switch(value){case kCWSecurityNone:n.auth=1;n.cipher=0;break;
    case kCWSecurityWEP:n.auth=1;n.cipher=0x101;break;
    case kCWSecurityWPAPersonal:n.auth=4;n.cipher=2;break;
    case kCWSecurityWPAEnterprise:n.auth=3;n.cipher=2;n.one_x=true;break;
    case kCWSecurityDynamicWEP:n.auth=1;n.cipher=0x101;n.one_x=true;break;
    case kCWSecurityWPAEnterpriseMixed:case kCWSecurityWPA2Enterprise:case kCWSecurityEnterprise:n.auth=6;n.cipher=4;n.one_x=true;break;
    case kCWSecurityWPA3Enterprise:n.auth=8;n.cipher=9;n.one_x=true;break;
    case kCWSecurityWPA3Personal:case kCWSecurityWPA3Transition:n.auth=9;n.cipher=4;break;
    case kCWSecurityOWE:case kCWSecurityOWETransition:n.auth=10;n.cipher=4;break;
    case kCWSecurityWPAPersonalMixed:case kCWSecurityWPA2Personal:case kCWSecurityPersonal:n.auth=7;n.cipher=4;break;
    default:n.auth=0;n.cipher=0;break;}
}
std::uint32_t network(CWNetwork* value,Network& n){
    const auto ssid=value.ssidData;if(!ssid || !mac(value.bssid,n.bssid))return 5;
    n.ssid=bytes(ssid);if(n.ssid.size()>32)return 13;n.bss_type=value.ibss?2:1;
    n.rssi=static_cast<std::int32_t>(value.rssiValue);n.frequency_khz=frequency(value.wlanChannel);
    // CoreWLAN 的 beaconInterval 單位為 ms；Windows 為 1024 μs 的 TU。
    n.beacon_period=static_cast<std::uint32_t>(std::clamp<NSInteger>((value.beaconInterval*1000+512)/1024,0,65535));
    n.information_elements=bytes(value.informationElementData);n.in_reg_domain=value.countryCode!=nil;
    for(const auto p:{kCWPHYMode11a,kCWPHYMode11b,kCWPHYMode11g,kCWPHYMode11n,kCWPHYMode11ac,kCWPHYMode11ax,kCWPHYMode11be})if([value supportsPHYMode:p])n.phys.push_back(phy(p));
    for(const auto s:{kCWSecurityWPA3Personal,kCWSecurityWPA3Enterprise,kCWSecurityOWE,kCWSecurityWPA2Personal,kCWSecurityWPA2Enterprise,kCWSecurityWPAPersonal,kCWSecurityWPAEnterprise,kCWSecurityWEP,kCWSecurityNone})
        if([value supportsSecurity:s]){security(n,s);return 0;}
    return 50;
}
std::uint32_t snapshot(CWInterface* value,Interface& out,bool details){
    if(!value || !value.interfaceName)return 1168;
    out={};out.name=text(value.interfaceName);out.description=out.name;out.power=value.powerOn;
    const auto mode=value.interfaceMode;out.state=!out.power?0:mode==kCWInterfaceModeIBSS?2:mode==kCWInterfaceModeStation?1:4;
    out.channel=static_cast<std::uint32_t>(value.wlanChannel.channelNumber);out.phy=phy(value.activePHYMode);
    out.tx_rate=static_cast<std::uint32_t>(std::clamp(value.transmitRate*1000.0,0.0,double(UINT32_MAX)));
    // 這是 emulated_802_11 介面：一個由宿主選擇的 PHY（dot11_phy_type_any），
    // 公告轉接層確實可提出的認證組合；不將目前 AP 的 PHY 清單冒充完整硬體能力。
    out.capabilities_known=true;out.phys={0};out.auth_cipher_pairs={{1,0},{1,0x101},{4,2},{7,4},{9,4}};
    // 公開 CoreWLAN 沒有可靠的 RX rate 查詢，保留未知 0。
    if(!details)return 0;
    if(out.state==1 || out.state==2){Network n;const auto ssid=value.ssidData;
        if(!ssid || !mac(value.bssid,n.bssid))out.connection_error=5;
        else {n.ssid=bytes(ssid);n.bss_type=out.state==2?2:1;n.rssi=static_cast<std::int32_t>(value.rssiValue);n.frequency_khz=frequency(value.wlanChannel);
            if(out.phy)n.phys={out.phy};security(n,value.security);out.connection=std::move(n);}
    }
    if(!out.power){out.networks_error=0x80342002U;return 0;}
    NSSet<CWNetwork*>* cached=value.cachedScanResults;
    if(!cached){out.networks_error=31;return 0;}
    for(CWNetwork* item in cached){Network n;if(const auto status=network(item,n)){out.networks_error=status;out.networks.clear();break;}out.networks.push_back(std::move(n));}
    std::sort(out.networks.begin(),out.networks.end(),[](const auto& a,const auto& b){return std::tie(a.ssid,a.bssid)<std::tie(b.ssid,b.bssid);});return 0;
}
std::shared_ptr<Operation> failed(std::uint32_t code){auto op=std::make_shared<Operation>();op->submission_error=op->error=code;op->done=true;return op;}
}
}

@interface X64ArmWlanEvents : NSObject <CWEventDelegate> {
@public std::shared_ptr<x64arm::wlan::Events> events;
}
@end
@implementation X64ArmWlanEvents
- (void)push:(NSString*)name change:(x64arm::wlan::Change)change {
    const auto state=events;if(!state)return;std::shared_ptr<x64arm::SharedWake> wake;
    {std::lock_guard lock(state->mutex);if(state->closed)return;state->pending.push_back({x64arm::wlan::text(name),change});wake=state->wake.lock();}
    if(wake)wake->notify();
}
- (void)clientConnectionInterrupted {[self push:@"" change:x64arm::wlan::Change::Unavailable];}
- (void)clientConnectionInvalidated {[self push:@"" change:x64arm::wlan::Change::Unavailable];}
- (void)powerStateDidChangeForWiFiInterfaceWithName:(NSString*)name {[self push:name change:x64arm::wlan::Change::Radio];}
- (void)ssidDidChangeForWiFiInterfaceWithName:(NSString*)name {[self push:name change:x64arm::wlan::Change::Link];}
- (void)bssidDidChangeForWiFiInterfaceWithName:(NSString*)name {[self push:name change:x64arm::wlan::Change::Link];}
- (void)linkDidChangeForWiFiInterfaceWithName:(NSString*)name {[self push:name change:x64arm::wlan::Change::Link];}
- (void)modeDidChangeForWiFiInterfaceWithName:(NSString*)name {[self push:name change:x64arm::wlan::Change::Link];}
- (void)countryCodeDidChangeForWiFiInterfaceWithName:(NSString*)name {[self push:name change:x64arm::wlan::Change::Link];}
- (void)linkQualityDidChangeForWiFiInterfaceWithName:(NSString*)name rssi:(NSInteger)rssi transmitRate:(double)rate {(void)rssi;(void)rate;[self push:name change:x64arm::wlan::Change::Signal];}
- (void)scanCacheUpdatedForWiFiInterfaceWithName:(NSString*)name {[self push:name change:x64arm::wlan::Change::ScanCache];}
@end

namespace x64arm::wlan {
namespace {
class Native final:public Backend {
    CWWiFiClient* client_=[[CWWiFiClient alloc] init];
    X64ArmWlanEvents* delegate_=[X64ArmWlanEvents new];
    dispatch_queue_t queue_=dispatch_queue_create("org.x64arm.wlan.operations",DISPATCH_QUEUE_SERIAL);
    std::shared_ptr<Events> events_=std::make_shared<Events>();
public:
    Native(){delegate_->events=events_;client_.delegate=delegate_;}
    ~Native(){client_.delegate=nil;[client_ stopMonitoringAllEventsAndReturnError:nil];std::lock_guard lock(events_->mutex);events_->closed=true;events_->pending.clear();}
    std::uint32_t enumerate(std::vector<Interface>& output)override{@autoreleasepool {
        NSArray<CWInterface*>* list=client_.interfaces;if(!list)return 1062;
        std::vector<Interface> result;for(CWInterface* i in list){Interface value;if(const auto status=snapshot(i,value,false))return status;result.push_back(std::move(value));}
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.name<b.name;});output=std::move(result);return 0;
    }}
    std::uint32_t query(const std::string& name,Interface& out)override{@autoreleasepool {return snapshot([client_ interfaceWithName:@(name.c_str())],out,true);}}
    std::uint32_t set_radio(const std::string& name,bool on)override{@autoreleasepool {
        CWInterface* i=[client_ interfaceWithName:@(name.c_str())];if(!i)return 1168;NSError* issue=nil;return [i setPower:on error:&issue]?0:error(issue);
    }}
    std::shared_ptr<Operation> scan(const std::string& name,const std::optional<Bytes>& ssid,std::span<const std::uint8_t> ie)override {
        if(!ie.empty())return failed(50);auto op=std::make_shared<Operation>();CWWiFiClient* client=client_;const auto events=events_;
        dispatch_async(queue_,^{@autoreleasepool {CWInterface* i=[client interfaceWithName:@(name.c_str())];NSError* issue=nil;
            if(!i)op->error=1168;else if(!i.powerOn)op->error=0x80342002U;
            else {NSData* filter=ssid?[NSData dataWithBytes:ssid->data() length:ssid->size()]:nil;
                NSSet<CWNetwork*>* found=[i scanForNetworksWithSSID:filter includeHidden:YES error:&issue];
                if(!found)op->error=error(issue);else for(CWNetwork* item in found){Network n;if(const auto code=network(item,n)){op->error=code;break;}}}
            complete(op,events);
        }});return op;
    }
    std::shared_ptr<Operation> connect(const std::string& name,const Connection& request)override {
        if(request.bss_type!=1 || (request.flags&~1U))return failed(50);
        if(request.auth!=1 && request.auth!=4 && request.auth!=7 && request.auth!=9)return failed(50);
        if(request.cipher && request.password.empty())return failed(87); // 不要求 CoreWLAN 從宿主鑰匙圈補密碼。
        auto op=std::make_shared<Operation>();CWWiFiClient* client=client_;const auto events=events_;
        dispatch_async(queue_,^{@autoreleasepool {CWInterface* i=[client interfaceWithName:@(name.c_str())];NSError* issue=nil;
            if(!i)op->error=1168;else if(!i.powerOn)op->error=0x80342002U;
            else {NSData* filter=[NSData dataWithBytes:request.ssid.data() length:request.ssid.size()];
                NSSet<CWNetwork*>* found=[i scanForNetworksWithSSID:filter includeHidden:(request.flags&1U)!=0 error:&issue];CWNetwork* selected=nil;
                if(!found)op->error=error(issue);else {op->error=1168;
                    for(CWNetwork* candidate in found){Network n;const auto status=network(candidate,n);if(status){op->error=status;continue;}
                        if(n.ssid!=request.ssid || (n.auth!=request.auth && !(request.auth==7 && n.auth==9 && [candidate supportsSecurity:kCWSecurityWPA2Personal])) || n.cipher!=request.cipher)continue;
                        if(!request.bssids.empty() && std::find(request.bssids.begin(),request.bssids.end(),n.bssid)==request.bssids.end())continue;
                        if(!selected || candidate.rssiValue>selected.rssiValue)selected=candidate;}
                    if(selected){NSString* password=request.cipher?@(request.password.c_str()):nil;op->error=[i associateToNetwork:selected password:password error:&issue]?0:error(issue);}}
            }complete(op,events);
        }});return op;
    }
    std::shared_ptr<Operation> disconnect(const std::string& name)override {
        auto op=std::make_shared<Operation>();CWWiFiClient* client=client_;const auto events=events_;
        dispatch_async(queue_,^{@autoreleasepool {CWInterface* i=[client interfaceWithName:@(name.c_str())];if(!i)op->error=1168;
            else {[i disassociate];if(i.interfaceMode!=kCWInterfaceModeNone)op->error=31;}complete(op,events);
        }});return op;
    }
    std::uint32_t monitor(bool enabled)override{@autoreleasepool {
        NSError* issue=nil;if(!enabled)return [client_ stopMonitoringAllEventsAndReturnError:&issue]?0:error(issue);
        for(const auto type:{CWEventTypePowerDidChange,CWEventTypeSSIDDidChange,CWEventTypeBSSIDDidChange,CWEventTypeLinkDidChange,CWEventTypeLinkQualityDidChange,CWEventTypeModeDidChange,CWEventTypeScanCacheUpdated})
            if(![client_ startMonitoringEventWithType:type error:&issue]){const auto code=error(issue);[client_ stopMonitoringAllEventsAndReturnError:nil];return code;}
        return 0;
    }}
    std::vector<Event> poll()override {std::lock_guard lock(events_->mutex);std::vector<Event> result;result.swap(events_->pending);return result;}
    bool set_wake(std::shared_ptr<SharedWake> wake)override {std::lock_guard lock(events_->mutex);events_->wake=wake;return bool(wake);}
};
}
std::shared_ptr<Backend> native_backend(){return std::make_shared<Native>();}
}
