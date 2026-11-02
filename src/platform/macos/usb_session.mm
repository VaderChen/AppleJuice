#include "x64arm/usb.hpp"
#import <Foundation/Foundation.h>
#import <IOUSBHost/IOUSBHost.h>
#include <algorithm>
#include <map>
#include <mach/mach_time.h>

// WinUSB 的宿主後端：以 IOUSBHost 開啟未被 macOS 驅動接管的介面，傳輸全部非同步送出，
// 完成回呼只寫入 Transfer，不觸碰客體狀態。
namespace x64arm::usb {
namespace {
std::uint32_t win32(IOReturn status){
    switch(status){
    case kIOReturnSuccess:return 0;
    case kIOReturnTimeout:return 121;            // ERROR_SEM_TIMEOUT
    case kIOReturnAborted:return 995;            // ERROR_OPERATION_ABORTED
    case kIOReturnNoDevice:case kIOReturnNotAttached:case kIOReturnNotResponding:case kIOReturnOffline:return 1167; // ERROR_DEVICE_NOT_CONNECTED
    case kIOReturnExclusiveAccess:case kIOReturnNotPrivileged:case kIOReturnNotPermitted:return 5;
    case kIOReturnBadArgument:return 87;
    case kIOReturnNoMemory:case kIOReturnNoResources:return 8;
    case kIOReturnUnsupported:return 50;
    default:return 31;                           // ERROR_GEN_FAILURE（含 stall）
    }
}
std::uint32_t win32(NSError* error){return error?win32(static_cast<IOReturn>(error.code)):31;}
io_service_t service_for(std::uint64_t registry_id){
    CFMutableDictionaryRef matching=IORegistryEntryIDMatching(registry_id);return matching?IOServiceGetMatchingService(MACH_PORT_NULL,matching):IO_OBJECT_NULL;
}
unsigned interface_number(io_service_t service){
    CFTypeRef value=IORegistryEntryCreateCFProperty(service,CFSTR("bInterfaceNumber"),kCFAllocatorDefault,0);int number=-1;
    if(value){if(CFGetTypeID(value)==CFNumberGetTypeID())CFNumberGetValue(static_cast<CFNumberRef>(value),kCFNumberIntType,&number);CFRelease(value);}return static_cast<unsigned>(number);
}
class HostSession final:public Session {
    dispatch_queue_t queue_=dispatch_queue_create("x64arm.winusb",DISPATCH_QUEUE_SERIAL);
    std::map<unsigned,IOUSBHostInterface*> interfaces_;
    std::map<std::pair<unsigned,std::uint8_t>,IOUSBHostPipe*> pipes_;
    std::map<std::pair<unsigned,std::uint8_t>,std::uint64_t> stream_frames_;
    IOUSBHostInterface* find(unsigned number)const{const auto found=interfaces_.find(number);return found==interfaces_.end()?nil:found->second;}
    IOUSBHostPipe* pipe_for(unsigned number,std::uint8_t endpoint,std::uint32_t& error){
        if(const auto found=pipes_.find({number,endpoint});found!=pipes_.end())return found->second;
        auto* interface=find(number);if(!interface){error=6;return nil;}
        NSError* failure=nil;IOUSBHostPipe* pipe=[interface copyPipeWithAddress:endpoint error:&failure];if(!pipe){error=failure?win32(failure):87;return nil;}
        pipes_.emplace(std::pair{number,endpoint},pipe);return pipe;
    }
    static std::shared_ptr<Transfer> failed(std::uint32_t error){auto transfer=std::make_shared<Transfer>();transfer->error=error;transfer->done=true;return transfer;}
public:
    bool add(io_service_t service,std::uint32_t& error){
        NSError* failure=nil;IOUSBHostInterface* interface=[[IOUSBHostInterface alloc] initWithIOService:service options:IOUSBHostObjectInitOptionsNone queue:queue_ error:&failure interestHandler:nil];
        if(!interface){error=failure && failure.code==kIOReturnExclusiveAccess?5:win32(failure);if(!error)error=5;return false;}
        interfaces_.emplace(interface_number(service),interface);return true;
    }
    ~HostSession()override{for(auto& [key,pipe]:pipes_){(void)key;[pipe abortWithOption:IOUSBHostAbortOptionSynchronous error:nil];}pipes_.clear();for(auto& [number,interface]:interfaces_){(void)number;[interface destroy];}}
    std::vector<std::uint8_t> device_descriptor()const override{
        if(interfaces_.empty())return {};const IOUSBDeviceDescriptor* descriptor=interfaces_.begin()->second.deviceDescriptor;if(!descriptor)return {};
        const auto* bytes=reinterpret_cast<const std::uint8_t*>(descriptor);return {bytes,bytes+sizeof(IOUSBDeviceDescriptor)};
    }
    std::vector<std::uint8_t> configuration_descriptor()const override{
        if(interfaces_.empty())return {};const IOUSBConfigurationDescriptor* descriptor=interfaces_.begin()->second.configurationDescriptor;if(!descriptor)return {};
        const auto* bytes=reinterpret_cast<const std::uint8_t*>(descriptor);return {bytes,bytes+USBToHost16(descriptor->wTotalLength)};
    }
    std::vector<unsigned> interfaces()const override{std::vector<unsigned> numbers;for(const auto& [number,interface]:interfaces_){(void)interface;numbers.push_back(number);}return numbers;}
    std::optional<unsigned> alternate(unsigned number)const override{auto* interface=find(number);if(!interface || !interface.interfaceDescriptor)return std::nullopt;return interface.interfaceDescriptor->bAlternateSetting;}
    std::uint32_t select_alternate(unsigned number,unsigned setting)override{
        auto* interface=find(number);if(!interface)return 6;
        std::erase_if(stream_frames_,[&](const auto& item){return item.first.first==number;});
        for(auto it=pipes_.begin();it!=pipes_.end();)it=it->first.first==number?pipes_.erase(it):std::next(it);
        NSError* failure=nil;return [interface selectAlternateSetting:setting error:&failure]?0:win32(failure);
    }
    std::shared_ptr<Transfer> control(unsigned number,std::uint8_t type,std::uint8_t request,std::uint16_t value,std::uint16_t index,std::vector<std::uint8_t> data,std::uint32_t timeout_ms)override{
        auto* interface=find(number);if(!interface)return failed(6);
        auto transfer=std::make_shared<Transfer>();transfer->data=std::move(data);
        IOUSBDeviceRequest setup{type,request,value,index,static_cast<std::uint16_t>(transfer->data.size())};
        NSMutableData* buffer=transfer->data.empty()?nil:[NSMutableData dataWithBytesNoCopy:transfer->data.data() length:transfer->data.size() freeWhenDone:NO];
        NSError* failure=nil;
        const BOOL queued=[interface enqueueDeviceRequest:setup data:buffer completionTimeout:timeout_ms?timeout_ms/1000.0:0 error:&failure completionHandler:^(IOReturn status,NSUInteger bytes){
            transfer->transferred=static_cast<std::uint32_t>(bytes);transfer->error=win32(status);transfer->done=true;}];
        if(!queued)return failed(win32(failure));return transfer;
    }
    std::shared_ptr<Transfer> pipe(unsigned number,std::uint8_t endpoint,std::vector<std::uint8_t> data,std::uint32_t timeout_ms)override{
        std::uint32_t error=0;auto* pipe=pipe_for(number,endpoint,error);if(!pipe)return failed(error);
        auto transfer=std::make_shared<Transfer>();transfer->data=std::move(data);
        NSMutableData* buffer=[NSMutableData dataWithBytesNoCopy:transfer->data.data() length:transfer->data.size() freeWhenDone:NO];
        NSError* failure=nil;
        const BOOL queued=[pipe enqueueIORequestWithData:buffer completionTimeout:timeout_ms?timeout_ms/1000.0:0 error:&failure completionHandler:^(IOReturn status,NSUInteger bytes){
            transfer->transferred=static_cast<std::uint32_t>(bytes);transfer->error=win32(status);transfer->done=true;}];
        if(!queued)return failed(win32(failure));return transfer;
    }
    std::uint32_t frame(unsigned number,std::uint64_t& value)override{
        auto* interface=find(number);if(!interface)return 6;
        value=[interface frameNumberWithTime:nil];return 0;
    }
    std::uint32_t time_sample(unsigned number,TimeSample& sample)override{
        auto* interface=find(number);if(!interface)return 6;
        IOUSBHostTime stamp{};NSError* failure=nil;
        const auto microframe=[interface currentMicroframeWithTime:&stamp error:&failure];
        if(failure)return win32(failure);
        // IOUSBHostTime 是 mach_absolute_time；客體 QPC 使用含睡眠時間的 continuous 軸。
        // 每次取樣重新配對，避免睡眠／喚醒後沿用過期位移；配對誤差計入回報精度。
        const auto before=mach_absolute_time(),continuous=mach_continuous_time(),after=mach_absolute_time();
        static const auto scale=[] {mach_timebase_info_data_t v{};mach_timebase_info(&v);return v;}();
        if(!scale.denom)return 31;
        const auto absolute=before+(after-before)/2;
        const auto mapped=static_cast<__int128>(stamp)+continuous-absolute;
        if(mapped<0 || mapped>UINT64_MAX)return 31;
        sample.microframe=microframe;
        sample.host_ticks=static_cast<std::uint64_t>(mapped*scale.numer/(static_cast<std::uint64_t>(scale.denom)*100));
        const auto uncertainty=(static_cast<unsigned __int128>(after-before)*scale.numer+scale.denom*1000ULL-1)/(scale.denom*1000ULL);
        sample.accuracy_us=static_cast<std::uint32_t>(std::min<unsigned __int128>(UINT32_MAX,125+uncertainty));
        return 0;
    }
    std::shared_ptr<Transfer> isoch(unsigned number,std::uint8_t endpoint,std::vector<std::uint8_t> data,
            std::uint32_t packet_bytes,std::uint32_t interval_microframes,std::uint64_t first_frame,bool asap,bool continue_stream)override{
        std::uint32_t error=0;auto* pipe=pipe_for(number,endpoint,error);if(!pipe)return failed(error);
        if(data.empty() || !packet_bytes || !interval_microframes)return failed(87);
        std::uint64_t current{};if(const auto status=frame(number,current))return failed(status);
        const auto key=std::pair{number,endpoint};const auto previous=stream_frames_.find(key);
        if(asap){
            if(continue_stream && (previous==stream_frames_.end() || previous->second<=current))return failed(31);
            first_frame=previous==stream_frames_.end()?current+1:std::max(current+1,previous->second);
        }
        auto transfer=std::make_shared<Transfer>();transfer->data=std::move(data);
        const auto count=(transfer->data.size()+packet_bytes-1)/packet_bytes;
        // 固定、連續的 packet 區段可用 macOS 10.15 的 frame-list 介面；保留專案 11.4 部署下限。
        auto transactions=std::make_shared<std::vector<IOUSBHostIsochronousFrame>>(count);
        transfer->packets.reserve(count);
        for(std::size_t index=0;index<count;++index){
            auto& item=(*transactions)[index];const auto offset=static_cast<std::uint32_t>(index*packet_bytes);
            item.requestCount=static_cast<std::uint32_t>(std::min<std::size_t>(packet_bytes,transfer->data.size()-offset));
            transfer->packets.push_back({offset,item.requestCount,0,0xc0020000U});
        }
        transfer->next_frame=first_frame+(count*interval_microframes+7)/8;
        NSMutableData* buffer=[NSMutableData dataWithBytesNoCopy:transfer->data.data() length:transfer->data.size() freeWhenDone:NO];
        NSError* failure=nil;
        const BOOL queued=[pipe enqueueIORequestWithData:buffer frameList:transactions->data() frameListCount:count
            firstFrameNumber:first_frame error:&failure
            completionHandler:^(IOReturn status,IOUSBHostIsochronousFrame* completed){
                transfer->error=win32(status);
                for(std::size_t index=0;index<transactions->size();++index){
                    auto& packet=transfer->packets[index];const auto& item=completed[index];
                    packet.transferred=std::min(packet.requested,item.completeCount);
                    switch(item.status){
                    case kIOReturnSuccess:packet.status=0;break;
                    case kIOReturnAborted:packet.status=0xc0010000U;break;
                    case kIOReturnNoDevice:case kIOReturnNotAttached:case kIOReturnOffline:packet.status=0xc0007000U;break;
                    case kIOReturnOverrun:packet.status=0xc0000008U;break;
                    case kIOReturnUnderrun:packet.status=0xc0000009U;break;
                    default:packet.status=0xc0020000U;break;
                    }
                    transfer->transferred+=packet.transferred;
                }
                transfer->done.store(true,std::memory_order_release);
            }];
        if(!queued)return failed(win32(failure));
        transfer->submitted=true;stream_frames_[key]=transfer->next_frame;return transfer;
    }
    std::uint32_t abort(unsigned number,std::uint8_t endpoint)override{
        stream_frames_.erase({number,endpoint});
        std::uint32_t error=0;auto* pipe=pipe_for(number,endpoint,error);if(!pipe)return error;NSError* failure=nil;
        return [pipe abortWithOption:IOUSBHostAbortOptionAsynchronous error:&failure]?0:win32(failure);
    }
    std::uint32_t reset(unsigned number,std::uint8_t endpoint)override{
        stream_frames_.erase({number,endpoint});
        std::uint32_t error=0;auto* pipe=pipe_for(number,endpoint,error);if(!pipe)return error;NSError* failure=nil;
        return [pipe clearStallWithError:&failure]?0:win32(failure);
    }
};
}
std::unique_ptr<Session> open(std::uint64_t registry_id,std::span<const unsigned> numbers,std::uint32_t& error){
    @autoreleasepool{
        error=0;const auto device=service_for(registry_id);if(!device){error=1167;return {};}
        auto session=std::make_unique<HostSession>();io_iterator_t children=IO_OBJECT_NULL;
        if(IORegistryEntryGetChildIterator(device,kIOServicePlane,&children)!=KERN_SUCCESS){IOObjectRelease(device);error=31;return {};}
        unsigned opened=0;
        while(const auto child=IOIteratorNext(children)){
            if(IOObjectConformsTo(child,"IOUSBHostInterface") && std::find(numbers.begin(),numbers.end(),interface_number(child))!=numbers.end()){
                if(!session->add(child,error)){IOObjectRelease(child);break;}++opened;
            }
            IOObjectRelease(child);
        }
        IOObjectRelease(children);IOObjectRelease(device);
        if(error)return {};if(opened!=numbers.size()){error=2;return {};}
        return session;
    }
}
}
