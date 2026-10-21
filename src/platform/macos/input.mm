#include "x64arm/input.hpp"
#import <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <ForceFeedback/ForceFeedback.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <pthread.h>
#include <pthread/qos.h>

namespace x64arm::input {
namespace {
#include "gamepad.inc"
// Apple ANSI 實體位置對應 Windows DIK，不依文字輸入或 PostMessage。
constexpr std::array<unsigned,128> scan_codes{
    0x1e,0x1f,0x20,0x21,0x23,0x22,0x2c,0x2d,0x2e,0x2f,0x56,0x30,0x10,0x11,0x12,0x13,
    0x15,0x14,2,3,4,5,7,6,0x0d,0x0a,8,0x0c,9,0x0b,0x1b,0x18,
    0x16,0x1a,0x17,0x19,0x1c,0x26,0x24,0x28,0x25,0x27,0x2b,0x33,0x35,0x31,0x32,0x34,
    0x0f,0x39,0x29,0x0e,0,1,0xdc,0xdb,0x2a,0x3a,0x38,0x1d,0x36,0xb8,0x9d,0,
    0x64,0x53,0,0x37,0,0x4e,0,0x45,0xb0,0xae,0xa0,0xb5,0x9c,0,0x4a,0x65,
    0x66,0x8d,0x52,0x4f,0x50,0x51,0x4b,0x4c,0x4d,0x47,0x67,0x48,0x49,0x7d,0x73,0,
    0x3f,0x40,0x41,0x3d,0x42,0x43,0,0x57,0,0x68,0x6b,0x69,0,0x44,0xdd,0x58,
    0,0x6a,0xd2,0xc7,0xc9,0xd3,0x3e,0xcf,0x3c,0xd1,0x3b,0xcb,0xcd,0xd0,0xc8,0};
std::string text(CFTypeRef value){
    if(!value || CFGetTypeID(value)!=CFStringGetTypeID())return {};
    char bytes[1024]{};if(!CFStringGetCString(static_cast<CFStringRef>(value),bytes,sizeof(bytes),kCFStringEncodingUTF8))return {};return bytes;
}
std::int64_t number(CFTypeRef value){
    std::int64_t n=0;if(value && CFGetTypeID(value)==CFNumberGetTypeID())CFNumberGetValue(static_cast<CFNumberRef>(value),kCFNumberSInt64Type,&n);return n;
}
struct Link {
    IOHIDDeviceRef hid{};FFDeviceObjectReference ff{};FFCAPABILITIES caps{};
    std::vector<std::uint8_t> report_buffer{};
    ~Link(){if(ff)FFReleaseDevice(ff);if(hid)CFRelease(hid);}
};
std::uint32_t native_result(HRESULT value){
    // 將原生 HRESULT 明確轉成 Windows 的 32 位元回傳值。
    const auto code=static_cast<std::uint32_t>(value);
    if(code==static_cast<std::uint32_t>(FFERR_INCOMPLETEEFFECT))return 0x80040206U;
    if(code==static_cast<std::uint32_t>(FFERR_EFFECTPLAYING))return 0x80040208U;
    return code;
}
struct Definition {
    FFEFFECT value{};FFENVELOPE envelope{};FFCUSTOMFORCE custom{};
    std::vector<DWORD> axes;std::vector<LONG> direction,samples;std::vector<std::uint8_t> bytes;
    explicit Definition(const EffectParameters& p){
        value.dwSize=sizeof(value);value.dwFlags=p.flags;value.dwDuration=p.duration;value.dwSamplePeriod=p.sample_period;value.dwGain=p.gain;value.dwTriggerButton=p.trigger;value.dwTriggerRepeatInterval=p.repeat;value.dwStartDelay=p.delay;
        axes.assign(p.axes.begin(),p.axes.end());direction.assign(p.direction.begin(),p.direction.end());value.cAxes=static_cast<DWORD>(axes.size());value.rgdwAxes=axes.data();value.rglDirection=direction.data();
        if(p.envelope){envelope.dwSize=sizeof(envelope);envelope.dwAttackLevel=(*p.envelope)[0];envelope.dwAttackTime=(*p.envelope)[1];envelope.dwFadeLevel=(*p.envelope)[2];envelope.dwFadeTime=(*p.envelope)[3];value.lpEnvelope=&envelope;}
        if(p.channels){samples.assign(p.samples.begin(),p.samples.end());custom.cChannels=p.channels;custom.dwSamplePeriod=p.custom_period;custom.cSamples=static_cast<DWORD>(samples.size());custom.rglForceData=samples.data();value.cbTypeSpecificParams=sizeof(custom);value.lpvTypeSpecificParams=&custom;}
        else {bytes=p.specific;value.cbTypeSpecificParams=static_cast<DWORD>(bytes.size());value.lpvTypeSpecificParams=bytes.data();}
    }
};
std::uint32_t escape_device(FFDeviceObjectReference device,FFEffectObjectReference effect,std::uint32_t command,std::span<const std::uint8_t> in,std::vector<std::uint8_t>& out){
    FFEFFESCAPE e{};e.dwSize=sizeof(e);e.dwCommand=command;e.lpvInBuffer=const_cast<std::uint8_t*>(in.data());e.cbInBuffer=static_cast<DWORD>(in.size());e.lpvOutBuffer=out.data();e.cbOutBuffer=static_cast<DWORD>(out.size());
    const auto code=native_result(effect?FFEffectEscape(effect,&e):FFDeviceEscape(device,&e));if(e.cbOutBuffer<=out.size())out.resize(e.cbOutBuffer);return code;
}
class NativeEffect final:public Effect {
    std::shared_ptr<Link> owner_;FFEffectObjectReference handle_{};
public:
    NativeEffect(std::shared_ptr<Link> owner,FFEffectObjectReference handle):owner_(std::move(owner)),handle_(handle){}
    ~NativeEffect()override{FFDeviceReleaseEffect(owner_->ff,handle_);}
    std::uint32_t set(const EffectParameters& p,std::uint32_t flags)override{Definition d(p);return native_result(FFEffectSetParameters(handle_,&d.value,flags));}
    std::uint32_t start(std::uint32_t n,std::uint32_t flags)override{return native_result(FFEffectStart(handle_,n,flags));}
    std::uint32_t stop()override{return native_result(FFEffectStop(handle_));}
    std::uint32_t download()override{return native_result(FFEffectDownload(handle_));}
    std::uint32_t unload()override{return native_result(FFEffectUnload(handle_));}
    std::uint32_t status(std::uint32_t& out)override{DWORD result=0;const auto code=native_result(FFEffectGetEffectStatus(handle_,&result));out=result;return code;}
    std::uint32_t escape(std::uint32_t command,std::span<const std::uint8_t> in,std::vector<std::uint8_t>& out)override{return escape_device(owner_->ff,handle_,command,in,out);}
};
struct Hid {
    Device device;std::shared_ptr<Link> link;std::map<IOHIDElementCookie,unsigned> elements;
    void* parent{};std::optional<PointerDevice> pointer{};
    std::array<std::optional<std::array<std::int32_t,2>>,2> physical{};
};
class NativeInput final:public Backend {
    NativeGamepads gamepads_;
    IOHIDManagerRef manager_{};CFRunLoopRef loop_{};CFMachPortRef tap_{};CFRunLoopSourceRef source_{};
    Device keyboard_{keyboard()},mouse_{mouse()};std::map<IOHIDDeviceRef,Hid> hid_;std::vector<Event> events_;
    std::vector<Report> reports_;
    std::set<Guid> pointer_dirty_;
    CGPoint cursor_{};bool have_cursor_{},exclusive_mouse_{};
    void change(Device& d,unsigned object,std::int64_t value){
        if(object>=d.values.size())return;
        if(d.objects[object].relative){if(!value)return;d.values[object]+=value;}
        else {if(d.values[object]==value)return;d.values[object]=value;}
        if(events_.size()<65536)events_.push_back({d.instance,object,value});
        pointer_dirty_.insert(d.instance);
    }
    void sample(){
        for(unsigned code=0;code<scan_codes.size();++code)if(scan_codes[code])change(keyboard_,scan_codes[code],CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState,static_cast<CGKeyCode>(code))?1:0);
        for(unsigned button=0;button<8;++button)change(mouse_,button+3,CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,static_cast<CGMouseButton>(button))?1:0);
        const auto event=CGEventCreate(nullptr);if(event){const auto point=CGEventGetLocation(event);CFRelease(event);if(have_cursor_ && !exclusive_mouse_){change(mouse_,0,static_cast<std::int64_t>(std::llround(point.x-cursor_.x)));change(mouse_,1,static_cast<std::int64_t>(std::llround(point.y-cursor_.y)));}cursor_=point;have_cursor_=true;}
    }
    static CGEventRef event(CGEventTapProxy,CGEventType type,CGEventRef value,void* context){
        auto& self=*static_cast<NativeInput*>(context);
        if(type==kCGEventTapDisabledByTimeout || type==kCGEventTapDisabledByUserInput){CGEventTapEnable(self.tap_,true);return value;}
        const auto first=self.events_.size();
        if(type==kCGEventKeyDown || type==kCGEventKeyUp){const auto code=CGEventGetIntegerValueField(value,kCGKeyboardEventKeycode);if(code>=0 && code<128 && scan_codes[static_cast<std::size_t>(code)]){
            const auto scan=scan_codes[static_cast<std::size_t>(code)];const auto extra=static_cast<std::uint32_t>(CGEventGetIntegerValueField(value,kCGEventSourceUserData));
            if(type==kCGEventKeyDown && self.keyboard_.values[scan] && self.events_.size()<65536)self.events_.push_back({self.keyboard_.instance,scan,1,extra,true});
            else {const auto before=self.events_.size();self.change(self.keyboard_,scan,type==kCGEventKeyDown?1:0);if(self.events_.size()>before)self.events_.back().extra=extra;}}}
        else if(type==kCGEventFlagsChanged)self.sample();
        else if(type==kCGEventScrollWheel){self.change(self.mouse_,2,CGEventGetIntegerValueField(value,kCGScrollWheelEventDeltaAxis1)*120);const auto horizontal=CGEventGetIntegerValueField(value,kCGScrollWheelEventDeltaAxis2)*120;if(horizontal && self.events_.size()<65536)self.events_.push_back({self.mouse_.instance,11,horizontal});}
        else if(type==kCGEventMouseMoved || type==kCGEventLeftMouseDragged || type==kCGEventRightMouseDragged || type==kCGEventOtherMouseDragged){self.change(self.mouse_,0,CGEventGetIntegerValueField(value,kCGMouseEventDeltaX));self.change(self.mouse_,1,CGEventGetIntegerValueField(value,kCGMouseEventDeltaY));self.cursor_=CGEventGetLocation(value);self.have_cursor_=true;}
        else {const auto button=CGEventGetIntegerValueField(value,kCGMouseEventButtonNumber);if(button>=0 && button<8)self.change(self.mouse_,static_cast<unsigned>(button)+3,type==kCGEventLeftMouseDown || type==kCGEventRightMouseDown || type==kCGEventOtherMouseDown?1:0);}
        const auto extra=static_cast<std::uint32_t>(CGEventGetIntegerValueField(value,kCGEventSourceUserData));
        for(auto i=first;i<self.events_.size();++i)self.events_[i].extra=extra;
        return value;
    }
    void add(IOHIDDeviceRef ref){
        if(hid_.contains(ref)){hid_.at(ref).device.attached=true;return;}
        Hid h;h.link=std::make_shared<Link>();h.link->hid=ref;CFRetain(ref);auto& d=h.device;
        const auto usage=number(IOHIDDeviceGetProperty(ref,CFSTR(kIOHIDPrimaryUsageKey))),primary_page=number(IOHIDDeviceGetProperty(ref,CFSTR(kIOHIDPrimaryUsagePageKey)));
        // 鍵盤／滑鼠沿用既有聚合裝置，其餘 TLC 保留真實 HID descriptor 與 reports。
        if(primary_page==1 && (usage==2 || usage==6 || usage==7))return;
        d.usage_page=static_cast<std::uint16_t>(primary_page);d.usage=static_cast<std::uint16_t>(usage);
        d.kind=primary_page==1 && usage==5?Kind::Gamepad:primary_page==1 && (usage==4 || usage==8)?Kind::Joystick:Kind::Hid;
        d.manufacturer=text(IOHIDDeviceGetProperty(ref,CFSTR(kIOHIDManufacturerKey)));d.serial=text(IOHIDDeviceGetProperty(ref,CFSTR(kIOHIDSerialNumberKey)));d.version=static_cast<std::uint16_t>(number(IOHIDDeviceGetProperty(ref,CFSTR(kIOHIDVersionNumberKey))));
        // USB 裝置節點登錄的字串描述元索引，供 HidD_GetIndexedString 對應已知字串。
        if(const auto owner=IOHIDDeviceGetService(ref)){const CFStringRef keys[]{CFSTR("iManufacturer"),CFSTR("iProduct"),CFSTR("iSerialNumber")};
            for(unsigned i=0;i<3;++i)if(const auto value=IORegistryEntrySearchCFProperty(owner,kIOServicePlane,keys[i],kCFAllocatorDefault,kIORegistryIterateRecursively|kIORegistryIterateParents)){d.string_indices[i]=static_cast<std::uint8_t>(number(value));CFRelease(value);}}
        const auto descriptor=IOHIDDeviceGetProperty(ref,CFSTR(kIOHIDReportDescriptorKey));if(descriptor && CFGetTypeID(descriptor)==CFDataGetTypeID()){
            const auto data=static_cast<CFDataRef>(descriptor);const auto size=CFDataGetLength(data);if(size>0 && size<=(1<<20))d.hid_descriptor.assign(CFDataGetBytePtr(data),CFDataGetBytePtr(data)+size);}
        d.name=text(IOHIDDeviceGetProperty(ref,CFSTR(kIOHIDProductKey)));if(d.name.empty())d.name="HID Game Controller";
        d.vendor=static_cast<std::uint32_t>(number(IOHIDDeviceGetProperty(ref,CFSTR(kIOHIDVendorIDKey))));d.product_id=static_cast<std::uint32_t>(number(IOHIDDeviceGetProperty(ref,CFSTR(kIOHIDProductIDKey))));
        auto service=IOHIDDeviceGetService(ref);io_string_t path{};if(service && IORegistryEntryGetPath(service,kIOServicePlane,path)==KERN_SUCCESS)d.path=path;
        // 固定識別值來自裝置的 registry 路徑／序號／VID/PID，與列舉順序無關。
        const auto identity=d.path+text(IOHIDDeviceGetProperty(ref,CFSTR(kIOHIDSerialNumberKey)))+std::to_string(d.vendor)+":"+std::to_string(d.product_id);
        std::uint64_t hash=1469598103934665603ULL;for(char c:identity){hash^=static_cast<unsigned char>(c);hash*=1099511628211ULL;}
        d.instance={0x68,0x69,0x64,0x2d,0x64,0x69,0x40,0x00,0x80,0,0,0,0,0,0,0};for(unsigned i=0;i<8;++i)d.instance[8+i]=static_cast<std::uint8_t>(hash>>(i*8));d.instance[8]=(d.instance[8]&0x3f)|0x80;
        d.product={static_cast<std::uint8_t>(d.vendor),static_cast<std::uint8_t>(d.vendor>>8),static_cast<std::uint8_t>(d.product_id),static_cast<std::uint8_t>(d.product_id>>8),0,0,0,0,0,0,0x50,0x49,0x44,0x56,0x49,0x44};
        if(service && FFIsForceFeedback(service)==0 && FFCreateDevice(service,&h.link->ff)==0 && FFDeviceGetForceFeedbackCapabilities(h.link->ff,&h.link->caps)==0){d.ff_effects=h.link->caps.supportedEffects;d.ff_storage=h.link->caps.storageCapacity;}
        CFArrayRef elements=IOHIDDeviceCopyMatchingElements(ref,nullptr,kIOHIDOptionsTypeNone);std::set<unsigned> axes;unsigned button=0,pov=0;
        if(elements){for(CFIndex i=0;i<CFArrayGetCount(elements);++i){auto element=static_cast<IOHIDElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(elements,i)));const auto type=IOHIDElementGetType(element);if(type!=kIOHIDElementTypeInput_Misc && type!=kIOHIDElementTypeInput_Button && type!=kIOHIDElementTypeInput_Axis)continue;
                Object o;const auto page=IOHIDElementGetUsagePage(element),u=IOHIDElementGetUsage(element);o.usage_page=static_cast<std::uint16_t>(page);o.usage=static_cast<std::uint16_t>(u);o.relative=IOHIDElementIsRelative(element);o.minimum=static_cast<std::int32_t>(IOHIDElementGetLogicalMin(element));o.maximum=static_cast<std::int32_t>(IOHIDElementGetLogicalMax(element));o.physical_minimum=static_cast<std::int32_t>(IOHIDElementGetPhysicalMin(element));o.physical_maximum=static_cast<std::int32_t>(IOHIDElementGetPhysicalMax(element));
                if(page==9 && button<128){o.kind=ObjectKind::Button;o.instance=button++;o.name="Button "+std::to_string(o.instance+1);}
                else if(page==1 && u==0x39 && pov<4){o.kind=ObjectKind::Pov;o.instance=pov++;o.name="POV "+std::to_string(o.instance+1);}
                else if((page==1 && u>=0x30 && u<=0x38) || (page==2 && u>=0xb0 && u<=0xb2)){o.kind=ObjectKind::Axis;unsigned axis=page==1 && u<=0x35?u-0x30:6;while(axes.contains(axis) && axis<8)++axis;if(axis>=8)continue;axes.insert(axis);o.instance=o.axis=axis;o.name="Axis "+std::to_string(axis+1);for(unsigned a=0;a<h.link->caps.numFfAxes && a<32;++a)if(h.link->caps.ffAxes[a]==axis*4)o.actuator=true;}
                else if(primary_page==0x0d && (usage==1 || usage==2) && page==0x0d && (u==0x30 || u==0x32 || u==0x3d || u==0x3e || u==0x42 || u==0x44 || u==0x45)){
                    o.kind=(u==0x32 || u==0x42 || u==0x44 || u==0x45)?ObjectKind::Button:ObjectKind::Axis;
                    o.instance=o.axis=static_cast<unsigned>(d.objects.size());o.name="Digitizer "+std::to_string(u);
                }else continue;
                if(primary_page==0x0d && (usage==1 || usage==2) && page==1 && (u==0x30 || u==0x31)){
                    const auto unit=IOHIDElementGetUnit(element);int exponent=static_cast<int>(IOHIDElementGetUnitExponent(element)&15U);if(exponent>=8)exponent-=16;
                    const auto scale=(unit==0x11?1000.0:unit==0x13?2540.0:0.0)*std::pow(10.0,exponent);
                    const double lo=o.physical_minimum*scale,hi=o.physical_maximum*scale;
                    if(scale!=0 && lo<hi && lo>=INT32_MIN && hi<=INT32_MAX)h.physical[u-0x30]=std::array<std::int32_t,2>{static_cast<std::int32_t>(std::llround(lo)),static_cast<std::int32_t>(std::llround(hi))};
                }
                const auto index=static_cast<unsigned>(d.objects.size());h.elements.emplace(IOHIDElementGetCookie(element),index);IOHIDValueRef value=nullptr;std::int64_t initial=0;if(IOHIDDeviceGetValue(ref,element,&value)==kIOReturnSuccess && value)initial=IOHIDValueGetIntegerValue(value);if(o.kind==ObjectKind::Pov){const auto count=static_cast<std::int64_t>(o.maximum)-o.minimum+1;initial=initial<o.minimum || initial>o.maximum || count<=0?-1:(initial-o.minimum)*36000/count;}d.objects.push_back(std::move(o));d.values.push_back(initial);
            }CFRelease(elements);}
        // 目前可逐接觸追蹤原生單筆輸入。觸控多接觸 TLC 不冒用成單筆裝置。
        if(primary_page==0x0d && (usage==1 || usage==2)){
            const auto axis=[&](unsigned u){return std::find_if(d.objects.begin(),d.objects.end(),[&](const auto& o){return o.usage_page==1 && o.usage==u && !o.relative && o.minimum<o.maximum;});};
            const auto x=axis(0x30),y=axis(0x31);const bool tip=std::any_of(d.objects.begin(),d.objects.end(),[](const auto& o){return o.usage_page==0x0d && o.usage==0x42;});
            if(x!=d.objects.end() && y!=d.objects.end() && tip){PointerDevice pointer;pointer.device=d.instance;pointer.type=2;pointer.name=d.name;pointer.logical_rectangle={x->minimum,y->minimum,x->maximum,y->maximum};
                if(h.physical[0] && h.physical[1])pointer.himetric_rectangle=std::array<std::int32_t,4>{(*h.physical[0])[0],(*h.physical[1])[0],(*h.physical[0])[1],(*h.physical[1])[1]};
                h.pointer=std::move(pointer);pointer_dirty_.insert(d.instance);}
        }
        if(d.objects.empty() && d.hid_descriptor.empty())return;
        h.parent=this;const auto result=hid_.emplace(ref,std::move(h));auto& stored=result.first->second;
        const auto size=number(IOHIDDeviceGetProperty(ref,CFSTR(kIOHIDMaxInputReportSizeKey)));
        if(size>0 && size<=65535){stored.link->report_buffer.resize(static_cast<std::size_t>(size));IOHIDDeviceRegisterInputReportCallback(ref,stored.link->report_buffer.data(),size,
            [](void* context,IOReturn status,void*,IOHIDReportType type,std::uint32_t id,std::uint8_t* bytes,CFIndex length){
                auto& entry=*static_cast<Hid*>(context);auto& self=*static_cast<NativeInput*>(entry.parent);
                if(status==kIOReturnSuccess && type==kIOHIDReportTypeInput && length>0 && length<=65535 && self.reports_.size()<10000)self.reports_.push_back({entry.device.instance,id,{bytes,bytes+length}});
            },&stored);}
    }
    void value(IOHIDValueRef value){
        const auto element=IOHIDValueGetElement(value);const auto device=IOHIDElementGetDevice(element);const auto found=hid_.find(device);if(found==hid_.end())return;auto& h=found->second;const auto object=h.elements.find(IOHIDElementGetCookie(element));if(object==h.elements.end())return;
        auto n=static_cast<std::int64_t>(IOHIDValueGetIntegerValue(value));const auto& o=h.device.objects[object->second];if(o.kind==ObjectKind::Pov){const auto count=static_cast<std::int64_t>(o.maximum)-o.minimum+1;n=n<o.minimum || n>o.maximum || count<=0?-1:(n-o.minimum)*36000/count;}
        change(h.device,object->second,n);
    }
    std::shared_ptr<Link> link(const Guid& id){for(auto& [ref,h]:hid_){(void)ref;if(h.device.instance==id && h.device.attached && h.link->ff)return h.link;}return {};}
public:
    NativeInput(){
        loop_=CFRunLoopGetCurrent();CFRetain(loop_);manager_=IOHIDManagerCreate(kCFAllocatorDefault,kIOHIDOptionsTypeNone);
        if(manager_){IOHIDManagerSetDeviceMatching(manager_,nullptr);
            IOHIDManagerRegisterDeviceMatchingCallback(manager_,[](void* c,IOReturn,void*,IOHIDDeviceRef d){static_cast<NativeInput*>(c)->add(d);},this);
            IOHIDManagerRegisterDeviceRemovalCallback(manager_,[](void* c,IOReturn,void*,IOHIDDeviceRef d){auto& h=static_cast<NativeInput*>(c)->hid_;if(h.contains(d))h.at(d).device.attached=false;},this);
            IOHIDManagerRegisterInputValueCallback(manager_,[](void* c,IOReturn,void*,IOHIDValueRef v){static_cast<NativeInput*>(c)->value(v);},this);
            IOHIDManagerScheduleWithRunLoop(manager_,loop_,kCFRunLoopDefaultMode);IOHIDManagerOpen(manager_,kIOHIDOptionsTypeNone);auto devices=IOHIDManagerCopyDevices(manager_);if(devices){std::vector<const void*> refs(static_cast<std::size_t>(CFSetGetCount(devices)));CFSetGetValues(devices,refs.data());for(auto ref:refs)add(static_cast<IOHIDDeviceRef>(const_cast<void*>(ref)));CFRelease(devices);}}
        if(CGPreflightListenEventAccess()){const auto mask=CGEventMaskBit(kCGEventKeyDown)|CGEventMaskBit(kCGEventKeyUp)|CGEventMaskBit(kCGEventFlagsChanged)|CGEventMaskBit(kCGEventLeftMouseDown)|CGEventMaskBit(kCGEventLeftMouseUp)|CGEventMaskBit(kCGEventRightMouseDown)|CGEventMaskBit(kCGEventRightMouseUp)|CGEventMaskBit(kCGEventOtherMouseDown)|CGEventMaskBit(kCGEventOtherMouseUp)|CGEventMaskBit(kCGEventMouseMoved)|CGEventMaskBit(kCGEventLeftMouseDragged)|CGEventMaskBit(kCGEventRightMouseDragged)|CGEventMaskBit(kCGEventOtherMouseDragged)|CGEventMaskBit(kCGEventScrollWheel);
            tap_=CGEventTapCreate(kCGSessionEventTap,kCGHeadInsertEventTap,kCGEventTapOptionListenOnly,mask,event,this);if(tap_){source_=CFMachPortCreateRunLoopSource(nullptr,tap_,0);CFRunLoopAddSource(loop_,source_,kCFRunLoopDefaultMode);}}
        sample();events_.clear();
    }
    ~NativeInput()override{if(exclusive_mouse_)cooperative(mouse_.instance,false);if(source_){CFRunLoopRemoveSource(loop_,source_,kCFRunLoopDefaultMode);CFRelease(source_);}if(tap_){CFMachPortInvalidate(tap_);CFRelease(tap_);}if(manager_){IOHIDManagerUnscheduleFromRunLoop(manager_,loop_,kCFRunLoopDefaultMode);IOHIDManagerClose(manager_,kIOHIDOptionsTypeNone);CFRelease(manager_);}if(loop_)CFRelease(loop_);}
    std::vector<Device> devices()override{std::vector<Device> out{keyboard_,mouse_};for(const auto& [ref,h]:hid_){(void)ref;out.push_back(h.device);}return out;}
    std::vector<Event> poll()override{for(unsigned i=0;i<1024 && CFRunLoopRunInMode(kCFRunLoopDefaultMode,0,true)==kCFRunLoopRunHandledSource;++i){}sample();auto out=std::move(events_);events_.clear();return out;}
    std::vector<PointerDevice> pointer_devices()override{
        std::vector<PointerDevice> out;for(const auto& [ref,h]:hid_){(void)ref;if(h.pointer && h.device.attached)out.push_back(*h.pointer);}return out;
    }
    std::vector<PointerSample> pointer_samples()override{
        std::vector<PointerSample> out;
        for(const auto& [ref,h]:hid_){(void)ref;if(!h.pointer || !h.device.attached || !pointer_dirty_.contains(h.device.instance))continue;
            const auto find=[&](unsigned page,unsigned usage)->std::optional<unsigned>{for(unsigned i=0;i<h.device.objects.size();++i)if(h.device.objects[i].usage_page==page && h.device.objects[i].usage==usage)return i;return {};};
            const auto value=[&](unsigned page,unsigned usage,std::int64_t fallback=0){const auto i=find(page,usage);return i?h.device.values[*i]:fallback;};
            PointerSample sample;sample.device=h.device.instance;sample.x=static_cast<std::int32_t>(value(1,0x30));sample.y=static_cast<std::int32_t>(value(1,0x31));
            sample.in_contact=value(0x0d,0x42)!=0;sample.in_range=value(0x0d,0x32,sample.in_contact?1:0)!=0;sample.barrel=value(0x0d,0x44)!=0;sample.eraser=value(0x0d,0x45)!=0;
            if(const auto i=find(0x0d,0x30)){const auto& o=h.device.objects[*i];if(o.minimum<o.maximum){sample.mask|=1;sample.pressure=static_cast<std::uint32_t>(std::clamp<std::int64_t>((h.device.values[*i]-o.minimum)*1024/(static_cast<std::int64_t>(o.maximum)-o.minimum),0,1024));}}
            if(find(0x0d,0x3d)){sample.mask|=4;sample.tilt_x=static_cast<std::int32_t>(std::clamp<std::int64_t>(value(0x0d,0x3d),-90,90));}
            if(find(0x0d,0x3e)){sample.mask|=8;sample.tilt_y=static_cast<std::int32_t>(std::clamp<std::int64_t>(value(0x0d,0x3e),-90,90));}
            out.push_back(sample);
        }
        pointer_dirty_.clear();return out;
    }
    std::vector<Report> reports()override{auto out=std::move(reports_);reports_.clear();return out;}
    std::vector<Gamepad> gamepads()override{return gamepads_.sample();}
    std::uint32_t rumble(const Guid& id,std::uint16_t left,std::uint16_t right)override{return gamepads_.rumble(id,left,right);}
    std::uint32_t set_report(const Guid& id,unsigned type,std::span<const std::uint8_t> bytes)override{
        if(type>2 || bytes.empty())return 87;
        if(id==keyboard_.instance || id==mouse_.instance)return 50;
        for(auto& [ref,h]:hid_){if(h.device.instance!=id)continue;if(!h.device.attached)return 1167;
            const auto report_id=bytes.front();const auto data=report_id?bytes:bytes.subspan(1);
            const auto result=IOHIDDeviceSetReport(ref,static_cast<IOHIDReportType>(type),report_id,data.data(),static_cast<CFIndex>(data.size()));
            return result==kIOReturnSuccess?0U:result==kIOReturnNotPermitted?5U:result==kIOReturnUnsupported?50U:31U;}
        return 1167;
    }
    std::uint32_t get_report(const Guid& id,unsigned type,std::span<std::uint8_t> bytes)override{
        if(type>2 || bytes.empty())return 87;
        if(id==keyboard_.instance || id==mouse_.instance)return 50;
        for(auto& [ref,h]:hid_){if(h.device.instance!=id)continue;if(!h.device.attached)return 1167;
            // 未編號報告不含 report ID 位元組；編號報告由 IOKit 連同 ID 一併傳回。
            const auto report_id=bytes.front();const auto data=report_id?bytes:bytes.subspan(1);CFIndex length=static_cast<CFIndex>(data.size());
            const auto result=IOHIDDeviceGetReport(ref,static_cast<IOHIDReportType>(type),report_id,data.data(),&length);
            if(result!=kIOReturnSuccess)return result==kIOReturnNotPermitted?5U:result==kIOReturnUnsupported?50U:31U;
            if(length>=0 && static_cast<std::size_t>(length)<data.size())std::fill(data.begin()+length,data.end(),std::uint8_t{});if(report_id)bytes.front()=report_id;return 0U;}
        return 1167;
    }
    bool available(const Guid& id)override{if(id==keyboard_.instance || id==mouse_.instance)return true;for(auto& [ref,h]:hid_){(void)ref;if(h.device.instance==id)return h.device.attached;}return false;}
    void mouse_wheel(std::int32_t delta)override{if(!tap_)change(mouse_,2,delta);}
    void horizontal_wheel(std::int32_t delta)override{if(!tap_ && delta && events_.size()<65536)events_.push_back({mouse_.instance,11,delta});}
    void cursor_warped()override{const auto event=CGEventCreate(nullptr);if(event){cursor_=CGEventGetLocation(event);have_cursor_=true;CFRelease(event);}}
    bool observes_wheel()const override{return tap_!=nullptr;}
    std::uint32_t cooperative(const Guid& id,bool exclusive)override{
        if(id==mouse_.instance && exclusive!=exclusive_mouse_){if(exclusive){if(!tap_ || CGAssociateMouseAndMouseCursorPosition(false)!=kCGErrorSuccess)return 0x80070005U;if(CGDisplayHideCursor(CGMainDisplayID())!=kCGErrorSuccess){CGAssociateMouseAndMouseCursorPosition(true);return 0x80070005U;}}else {CGAssociateMouseAndMouseCursorPosition(true);CGDisplayShowCursor(CGMainDisplayID());have_cursor_=false;}exclusive_mouse_=exclusive;}
        auto l=link(id);return l?native_result(FFDeviceSetCooperativeLevel(l->ff,nullptr,exclusive?FFSCL_EXCLUSIVE|FFSCL_BACKGROUND:FFSCL_NONEXCLUSIVE|FFSCL_BACKGROUND)):0;
    }
    static std::uint32_t control_panel_ui(const Guid& id){@autoreleasepool{static const auto keyboard_id=keyboard().instance,mouse_id=mouse().instance;const char* pane=id==keyboard_id?"com.apple.Keyboard-Settings.extension":id==mouse_id?"com.apple.Mouse-Settings.extension":"com.apple.Game-Controller-Settings.extension";const auto url=[NSURL URLWithString:[NSString stringWithFormat:@"x-apple.systempreferences:%s",pane]];return [[NSWorkspace sharedWorkspace] openURL:url]?0:0x80004005U;}}
    static std::uint32_t configure_ui(std::vector<Action>& actions,bool edit){
        @autoreleasepool {
            NSAlert* alert=[[NSAlert alloc] init];alert.messageText=@"DirectInput 輸入設定";alert.informativeText=edit?@"選擇各動作使用的實體控制項。":@"目前動作與實體控制項的映射。";[alert addButtonWithTitle:edit?@"儲存":@"關閉"];if(edit)[alert addButtonWithTitle:@"取消"];
            NSScrollView* scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,620,300)];scroll.hasVerticalScroller=YES;NSView* view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,600,static_cast<CGFloat>(actions.size()*36))];NSMutableArray<NSPopUpButton*>* menus=[NSMutableArray array];
            for(unsigned i=0;i<actions.size();++i){const auto& action=actions[i];const CGFloat y=static_cast<CGFloat>((actions.size()-i-1)*36);NSTextField* label=[NSTextField labelWithString:[NSString stringWithUTF8String:action.label.c_str()]];label.frame=NSMakeRect(0,y,240,28);[view addSubview:label];NSPopUpButton* menu=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(245,y,350,28) pullsDown:NO];for(const auto& control:action.controls)[menu addItemWithTitle:[NSString stringWithUTF8String:control.label.c_str()]];if(action.selected<action.controls.size())[menu selectItemAtIndex:static_cast<NSInteger>(action.selected)];menu.enabled=edit;[view addSubview:menu];[menus addObject:menu];}
            scroll.documentView=view;alert.accessoryView=scroll;const auto response=[alert runModal];if(edit && response!=NSAlertFirstButtonReturn)return 1;for(unsigned i=0;i<actions.size();++i)actions[i].selected=static_cast<unsigned>(menus[i].indexOfSelectedItem);return 0;
        }
    }
    std::uint32_t ff_property(const Guid& id,unsigned property,std::uint32_t& data,bool set)override{auto l=link(id);if(!l)return 0x80004001U;DWORD value=data;const auto p=property==7?FFPROP_FFGAIN:FFPROP_AUTOCENTER;const auto code=native_result(set?FFDeviceSetForceFeedbackProperty(l->ff,p,&value):FFDeviceGetForceFeedbackProperty(l->ff,p,&value,sizeof(value)));data=value;return code;}
    std::uint32_t ff_state(const Guid& id,std::uint32_t& out)override{auto l=link(id);if(!l)return 0x80004001U;DWORD value=0;const auto code=native_result(FFDeviceGetForceFeedbackState(l->ff,&value));out=value;return code;}
    std::uint32_t ff_command(const Guid& id,std::uint32_t command)override{auto l=link(id);return l?native_result(FFDeviceSendForceFeedbackCommand(l->ff,command)):0x80004001U;}
    std::uint32_t escape(const Guid& id,std::uint32_t command,std::span<const std::uint8_t> in,std::vector<std::uint8_t>& out)override{auto l=link(id);return l?escape_device(l->ff,nullptr,command,in,out):0x80004001U;}
    std::shared_ptr<Effect> effect(const Guid& id,unsigned type,const EffectParameters* parameters,std::uint32_t& error)override{
        auto l=link(id);if(!l || type>=12){error=0x80004001U;return {};}auto uuid=CFUUIDGetConstantUUIDWithBytes(nullptr,0xe5,0x59,0xc4,static_cast<UInt8>(0x60+type),0xc5,0xcd,0x11,0xd6,0x8a,0x1c,0,3,0x93,0x53,0xbd,0);FFEffectObjectReference handle=nullptr;Definition d(parameters?*parameters:EffectParameters{});error=native_result(FFDeviceCreateEffect(l->ff,uuid,parameters?&d.value:nullptr,&handle));return (error&0x80000000U)?std::shared_ptr<Effect>{}:std::make_shared<NativeEffect>(std::move(l),handle);
    }
};
#include "input_worker.inc"
}
std::shared_ptr<Backend> make_backend(){return std::make_shared<WorkerInput>();}
}
