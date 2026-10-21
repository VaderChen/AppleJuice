#include "x64arm/usb.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach.h>
#include <algorithm>
#include <mutex>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>

namespace x64arm::usb {
namespace {
std::optional<std::int64_t> number(io_registry_entry_t entry,const char* key){
    CFStringRef name=CFStringCreateWithCString(nullptr,key,kCFStringEncodingUTF8);if(!name)return {};
    CFTypeRef value=IORegistryEntryCreateCFProperty(entry,name,kCFAllocatorDefault,0);CFRelease(name);if(!value)return {};
    std::optional<std::int64_t> result;std::int64_t out=0;
    if(CFGetTypeID(value)==CFNumberGetTypeID() && CFNumberGetValue(static_cast<CFNumberRef>(value),kCFNumberSInt64Type,&out))result=out;
    CFRelease(value);return result;
}
std::u16string text(io_registry_entry_t entry,const char* key){
    CFStringRef name=CFStringCreateWithCString(nullptr,key,kCFStringEncodingUTF8);if(!name)return {};
    CFTypeRef value=IORegistryEntryCreateCFProperty(entry,name,kCFAllocatorDefault,0);CFRelease(name);if(!value)return {};
    std::u16string out;
    if(CFGetTypeID(value)==CFStringGetTypeID()){const auto string=static_cast<CFStringRef>(value);const auto length=CFStringGetLength(string);
        if(length>0 && length<1024){out.resize(static_cast<std::size_t>(length));CFStringGetCharacters(string,CFRangeMake(0,length),reinterpret_cast<UniChar*>(out.data()));}}
    CFRelease(value);return out;
}
std::string service_path(io_registry_entry_t entry){io_string_t path{};return IORegistryEntryGetPath(entry,kIOServicePlane,path)==KERN_SUCCESS?std::string(path):std::string{};}
std::uint64_t identifier(io_registry_entry_t entry){std::uint64_t id=0;IORegistryEntryGetRegistryEntryID(entry,&id);return id;}
bool has_children(io_registry_entry_t entry){
    io_iterator_t children=IO_OBJECT_NULL;if(IORegistryEntryGetChildIterator(entry,kIOServicePlane,&children)!=KERN_SUCCESS)return false;
    const auto child=IOIteratorNext(children);const bool found=child!=IO_OBJECT_NULL;if(child)IOObjectRelease(child);IOObjectRelease(children);return found;
}
// 宿主裝置變更監看：IOKit 註冊表有服務發布或終止（裝置插拔、驅動接管）時才重新列舉，其間重用快照。
// 通知以非阻塞 mach_msg 取回，不需要 run loop；無法建立通知時每次查詢都重新列舉。
class RegistryWatch {
    IONotificationPortRef port_{};
    io_iterator_t published_{IO_OBJECT_NULL},terminated_{IO_OBJECT_NULL};
    bool changed_{true},available_{};
    static void drain(void* context,io_iterator_t iterator){
        while(const auto object=IOIteratorNext(iterator))IOObjectRelease(object);
        static_cast<RegistryWatch*>(context)->changed_=true;
    }
public:
    RegistryWatch(){
        port_=IONotificationPortCreate(MACH_PORT_NULL);if(!port_)return;
        if(IOServiceAddMatchingNotification(port_,kIOPublishNotification,IOServiceMatching("IOService"),drain,this,&published_)!=KERN_SUCCESS)return;
        drain(this,published_); // 先消耗現有項目，通知才會啟用。
        if(IOServiceAddMatchingNotification(port_,kIOTerminatedNotification,IOServiceMatching("IOService"),drain,this,&terminated_)!=KERN_SUCCESS)return;
        drain(this,terminated_);
        available_=true;
    }
    RegistryWatch(const RegistryWatch&)=delete;RegistryWatch& operator=(const RegistryWatch&)=delete;
    ~RegistryWatch(){
        if(published_!=IO_OBJECT_NULL)IOObjectRelease(published_);if(terminated_!=IO_OBJECT_NULL)IOObjectRelease(terminated_);
        if(port_)IONotificationPortDestroy(port_);
    }
    // 自上次查詢後註冊表是否有變更；首次查詢為真。
    bool changed(){
        if(!available_)return true;
        const auto port=IONotificationPortGetMachPort(port_);
        for(;;){
            struct {mach_msg_header_t header;std::uint8_t body[4096];} message{};
            const auto status=mach_msg(&message.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(message),port,0,MACH_PORT_NULL);
            if(status==MACH_RCV_TOO_LARGE){changed_=true;continue;} // 過大的訊息已被丟棄；內容只代表有變更。
            if(status!=MACH_MSG_SUCCESS)break;
            IODispatchCalloutFromMessage(nullptr,&message.header,port_);
        }
        return std::exchange(changed_,false);
    }
};
std::vector<Device> enumerate(){
    std::vector<Device> result;io_iterator_t iterator=IO_OBJECT_NULL;
    if(IOServiceGetMatchingServices(MACH_PORT_NULL,IOServiceMatching("IOUSBHostDevice"),&iterator)!=KERN_SUCCESS)return result;
    while(const auto service=IOIteratorNext(iterator)){
        Device device;device.registry_id=identifier(service);device.path=service_path(service);
        device.vendor=static_cast<unsigned>(number(service,"idVendor").value_or(0));device.product=static_cast<unsigned>(number(service,"idProduct").value_or(0));
        device.revision=static_cast<unsigned>(number(service,"bcdDevice").value_or(0));device.cls=static_cast<unsigned>(number(service,"bDeviceClass").value_or(0));
        device.subclass=static_cast<unsigned>(number(service,"bDeviceSubClass").value_or(0));device.protocol=static_cast<unsigned>(number(service,"bDeviceProtocol").value_or(0));
        device.location=static_cast<std::uint32_t>(number(service,"locationID").value_or(0));device.port=static_cast<unsigned>(number(service,"PortNum").value_or(0));
        device.speed=static_cast<unsigned>(number(service,"Device Speed").value_or(0));device.address=static_cast<unsigned>(number(service,"USB Address").value_or(0));
        device.product_name=text(service,"USB Product Name");device.vendor_name=text(service,"USB Vendor Name");device.serial=text(service,"USB Serial Number");
        // 上層：同為 IOUSBHostDevice 者為集線器，否則為主控制器。
        io_registry_entry_t parent=IO_OBJECT_NULL;
        if(IORegistryEntryGetParentEntry(service,kIOServicePlane,&parent)==KERN_SUCCESS){
            if(IOObjectConformsTo(parent,"IOUSBHostDevice"))device.parent_id=identifier(parent);
            device.controller=device.location&0xff000000U;
            IOObjectRelease(parent);
        }
        io_iterator_t children=IO_OBJECT_NULL;
        if(IORegistryEntryGetChildIterator(service,kIOServicePlane,&children)==KERN_SUCCESS){
            while(const auto child=IOIteratorNext(children)){
                if(IOObjectConformsTo(child,"IOUSBHostInterface")){
                    Interface item;item.number=static_cast<unsigned>(number(child,"bInterfaceNumber").value_or(0));item.cls=static_cast<unsigned>(number(child,"bInterfaceClass").value_or(0));
                    item.subclass=static_cast<unsigned>(number(child,"bInterfaceSubClass").value_or(0));item.protocol=static_cast<unsigned>(number(child,"bInterfaceProtocol").value_or(0));
                    item.claimed=has_children(child);item.path=service_path(child);device.interfaces.push_back(item);
                }
                IOObjectRelease(child);
            }
            IOObjectRelease(children);
        }
        std::sort(device.interfaces.begin(),device.interfaces.end(),[](const auto& a,const auto& b){return a.number<b.number;});
        result.push_back(std::move(device));IOObjectRelease(service);
        if(result.size()>=256)break;
    }
    IOObjectRelease(iterator);
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.location<b.location;});
    return result;
}
}
std::vector<Device> devices(){
    static std::mutex mutex;static RegistryWatch watch;static std::vector<Device> cached;
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_USB_REGISTRY_CACHE");return !setting || std::string_view(setting)!="0";}();
    if(!enabled)return enumerate();
    std::lock_guard<std::mutex> lock(mutex);
    if(watch.changed())cached=enumerate();
    return cached;
}
}
