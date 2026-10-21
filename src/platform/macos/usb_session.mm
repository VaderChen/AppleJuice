#include "x64arm/usb.hpp"
#import <Foundation/Foundation.h>
#import <IOUSBHost/IOUSBHost.h>
#include <algorithm>
#include <map>

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
    std::uint32_t abort(unsigned number,std::uint8_t endpoint)override{
        std::uint32_t error=0;auto* pipe=pipe_for(number,endpoint,error);if(!pipe)return error;NSError* failure=nil;
        return [pipe abortWithOption:IOUSBHostAbortOptionAsynchronous error:&failure]?0:win32(failure);
    }
    std::uint32_t reset(unsigned number,std::uint8_t endpoint)override{
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
