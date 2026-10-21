#include "x64arm/windows_power.hpp"
#include "x64arm/windows_power_notifications.hpp"
#import <Foundation/Foundation.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOKitLib.h>
#include <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <memory>
#include <dlfcn.h>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <cmath>
namespace x64arm {
namespace {
struct Release {void operator()(CFTypeRef p)const{if(p)CFRelease(p);}};
template<class T>using Cf=std::unique_ptr<std::remove_pointer_t<T>,Release>;
std::optional<int> number(CFDictionaryRef d,CFStringRef key){auto p=CFDictionaryGetValue(d,key);int n=0;if(!p || CFGetTypeID(p)!=CFNumberGetTypeID() || !CFNumberGetValue(static_cast<CFNumberRef>(p),kCFNumberIntType,&n))return {};return n;}
}
namespace {
class Assertion final : public NativePowerAssertion {
public:
    IOPMAssertionID id{kIOPMNullAssertionID};
    ~Assertion() override {if(id!=kIOPMNullAssertionID)IOPMAssertionRelease(id);}
};
}
std::unique_ptr<NativePowerAssertion> native_power_assertion(unsigned type,std::u16string_view reason){
    if(type!=0 && type!=1 && type!=3)return {};
    Cf<CFStringRef> text(CFStringCreateWithCharacters(kCFAllocatorDefault,reinterpret_cast<const UniChar*>(reason.data()),static_cast<CFIndex>(reason.size())));
    if(!text)return {};
    auto assertion=std::make_unique<Assertion>();
    const auto kind=type==0?kIOPMAssertionTypePreventUserIdleDisplaySleep:kIOPMAssertionTypePreventUserIdleSystemSleep;
    if(IOPMAssertionCreateWithDescription(kind,CFSTR("Win64Sandbox PowerRequest"),text.get(),nullptr,nullptr,0,nullptr,&assertion->id)!=kIOReturnSuccess)return {};
    return assertion;
}
std::unique_ptr<NativePowerAssertion> native_power_activity(bool display){
    auto assertion=std::make_unique<Assertion>();
    const auto status=display?IOPMAssertionDeclareUserActivity(CFSTR("Win64Sandbox display activity"),kIOPMUserActiveLocal,&assertion->id):IOPMDeclareNetworkClientActivity(CFSTR("Win64Sandbox system activity"),&assertion->id);
    if(status!=kIOReturnSuccess)return {};return assertion;
}
std::optional<std::uint32_t> native_power_role(){
    Cf<CFTypeRef> info(IOPSCopyPowerSourcesInfo());if(!info)return {};
    Cf<CFArrayRef> sources(IOPSCopyPowerSourcesList(info.get()));if(!sources)return {};
    for(CFIndex i=0;i<CFArrayGetCount(sources.get());++i){
        const auto data=IOPSGetPowerSourceDescription(info.get(),CFArrayGetValueAtIndex(sources.get(),i));if(!data)continue;
        const auto type=CFDictionaryGetValue(data,CFSTR(kIOPSTypeKey));
        if(type && CFEqual(type,CFSTR(kIOPSInternalBatteryType)) && CFDictionaryGetValue(data,CFSTR(kIOPSIsPresentKey))!=kCFBooleanFalse)return 2;
    }
    return 1;
}
std::optional<NativePowerPreferences> native_power_preferences(){
    // Apple 公開原始碼中的既有 SPI；動態查詢，缺少時不使引擎無法啟動。
    using CopyPreferences=CFDictionaryRef(*)();
    static const auto copy=reinterpret_cast<CopyPreferences>(dlsym(RTLD_DEFAULT,"IOPMCopyPMPreferences"));
    if(!copy)return {};
    Cf<CFDictionaryRef> preferences(copy());
    if(!preferences || CFGetTypeID(preferences.get())!=CFDictionaryGetTypeID())return {};
    NativePowerPreferences out;
    const std::array keys{CFSTR("Display Sleep Timer"),CFSTR("System Sleep Timer"),CFSTR("Disk Sleep Timer")};
    for(unsigned source=0;source<out.sources.size();++source){
        auto value=CFDictionaryGetValue(preferences.get(),source?CFSTR(kIOPMBatteryPowerKey):CFSTR(kIOPMACPowerKey));
        if(!value && source)value=CFDictionaryGetValue(preferences.get(),CFSTR(kIOPMUPSPowerKey));
        if(!value)continue;
        if(CFGetTypeID(value)!=CFDictionaryGetTypeID())return {};
        auto& target=out.sources[source];target.present=true;
        const auto dictionary=static_cast<CFDictionaryRef>(value);
        for(std::size_t setting=0;setting<keys.size();++setting){
            const auto entry=CFDictionaryGetValue(dictionary,keys[setting]);
            if(!entry)continue;
            std::int64_t minutes=0;
            if(CFGetTypeID(entry)!=CFNumberGetTypeID() ||
                !CFNumberGetValue(static_cast<CFNumberRef>(entry),kCFNumberSInt64Type,&minutes))target.invalid[setting]=true;
            else target.minutes[setting]=minutes;
        }
    }
    if(!out.sources[0].present && !out.sources[1].present)return {};
    return out;
}
std::optional<std::array<std::uint8_t,12>> native_power_status(){
    Cf<CFTypeRef> info(IOPSCopyPowerSourcesInfo());if(!info)return {};
    Cf<CFArrayRef> sources(IOPSCopyPowerSourcesList(info.get()));if(!sources)return {};
    std::array<std::uint8_t,12> out{255,128,255,0,255,255,255,255,255,255,255,255};
    const auto providing=IOPSGetProvidingPowerSourceType(info.get());
    if(providing)out[0]=CFEqual(providing,CFSTR(kIOPSACPowerValue))?1:0;
    std::int64_t current=0,maximum=0;bool known=true,present=false,charging=false;
    for(CFIndex i=0;i<CFArrayGetCount(sources.get());++i){
        const auto d=IOPSGetPowerSourceDescription(info.get(),CFArrayGetValueAtIndex(sources.get(),i));if(!d)continue;
        const auto type=CFDictionaryGetValue(d,CFSTR(kIOPSTypeKey));
        if(!type || !CFEqual(type,CFSTR(kIOPSInternalBatteryType)))continue;
        if(CFDictionaryGetValue(d,CFSTR(kIOPSIsPresentKey))==kCFBooleanFalse)continue;
        present=true;charging|=CFDictionaryGetValue(d,CFSTR(kIOPSIsChargingKey))==kCFBooleanTrue;
        const auto c=number(d,CFSTR(kIOPSCurrentCapacityKey)),m=number(d,CFSTR(kIOPSMaxCapacityKey));
        if(!c || !m || *c<0 || *m<=0)known=false;else {current+=*c;maximum+=*m;}
    }
    if(present){
        if(known && maximum){out[2]=static_cast<std::uint8_t>(std::clamp<std::int64_t>(current*100/maximum,0,100));out[1]=static_cast<std::uint8_t>((out[2]>66?1:out[2]<5?6:out[2]<33?2:0)|(charging?8:0));}
        else out[1]=255;
        const double seconds=IOPSGetTimeRemainingEstimate();
        if(out[0]==0 && seconds>=0 && seconds<UINT32_MAX){const auto value=static_cast<std::uint32_t>(seconds);for(unsigned i=0;i<4;++i)out[4+i]=static_cast<std::uint8_t>(value>>(8*i));}
    }
    if(@available(macOS 12.0,*))out[3]=[NSProcessInfo processInfo].lowPowerModeEnabled?1:0;
    return out;
}
std::optional<std::array<std::uint8_t,76>> native_power_capabilities(){
    Cf<CFTypeRef> info(IOPSCopyPowerSourcesInfo());if(!info)return {};
    Cf<CFArrayRef> sources(IOPSCopyPowerSourcesList(info.get()));if(!sources)return {};
    const auto root=IOServiceGetMatchingService(MACH_PORT_NULL,IOServiceMatching("IOPMrootDomain"));if(!root)return {};
    Cf<CFTypeRef> lid(IORegistryEntryCreateCFProperty(root,CFSTR("AppleClamshellState"),kCFAllocatorDefault,0));
    IOObjectRelease(root);
    std::array<std::uint8_t,76> out{};
    out[2]=lid && CFGetTypeID(lid.get())==CFBooleanGetTypeID();
    // 客體沒有 ACPI 電源控制、S3/S4 或 Windows hiberfile；不能宣告宿主睡眠等於客體支援。
    for(CFIndex i=0;i<CFArrayGetCount(sources.get());++i){
        const auto data=IOPSGetPowerSourceDescription(info.get(),CFArrayGetValueAtIndex(sources.get(),i));if(!data)continue;
        if(CFDictionaryGetValue(data,CFSTR(kIOPSIsPresentKey))==kCFBooleanFalse)continue;
        const auto type=CFDictionaryGetValue(data,CFSTR(kIOPSTypeKey));
        if(type && CFEqual(type,CFSTR(kIOPSInternalBatteryType)))out[30]=1;
        else if(type && CFEqual(type,CFSTR(kIOPSUPSType))){out[12]=1;out[30]=1;out[31]=1;}
    }
    return out;
}
std::optional<std::array<std::uint8_t,32>> native_power_battery_state(){
    Cf<CFTypeRef> info(IOPSCopyPowerSourcesInfo());if(!info)return {};
    Cf<CFArrayRef> sources(IOPSCopyPowerSourcesList(info.get()));if(!sources)return {};
    std::array<std::uint8_t,32> out{};
    const auto store=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)out[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    const auto providing=IOPSGetProvidingPowerSourceType(info.get());if(!providing)return {};
    out[0]=CFEqual(providing,CFSTR(kIOPSACPowerValue));
    for(CFIndex i=0;i<CFArrayGetCount(sources.get());++i){
        const auto data=IOPSGetPowerSourceDescription(info.get(),CFArrayGetValueAtIndex(sources.get(),i));if(!data)continue;
        if(CFDictionaryGetValue(data,CFSTR(kIOPSIsPresentKey))==kCFBooleanFalse)continue;
        const auto type=CFDictionaryGetValue(data,CFSTR(kIOPSTypeKey));
        if(!type || (!CFEqual(type,CFSTR(kIOPSInternalBatteryType)) && !CFEqual(type,CFSTR(kIOPSUPSType))))continue;
        out[1]=1;out[2]|=CFDictionaryGetValue(data,CFSTR(kIOPSIsChargingKey))==kCFBooleanTrue;
        const auto state=CFDictionaryGetValue(data,CFSTR(kIOPSPowerSourceStateKey));
        out[3]|=state && CFEqual(state,CFSTR(kIOPSBatteryPowerValue));
    }
    store(20,UINT32_MAX);
    if(out[1]){
        // IOPS 的容量是百分比，不能當成 Windows mWh；缺少實體量測時使用未知值。
        store(8,UINT32_MAX);store(12,UINT32_MAX);store(16,0x80000000U);store(24,UINT32_MAX);store(28,UINT32_MAX);
        const double seconds=IOPSGetTimeRemainingEstimate();
        if(out[3] && seconds>=0 && seconds<UINT32_MAX)store(20,static_cast<std::uint32_t>(seconds));
    }
    return out;
}
std::optional<std::vector<std::array<std::uint32_t,6>>> native_processor_power_information(unsigned count){
    const auto frequency=[](const char* key)->std::optional<std::uint32_t>{
        std::uint64_t hz=0;std::size_t size=sizeof(hz);
        if(sysctlbyname(key,&hz,&size,nullptr,0) || size!=sizeof(hz) || hz<1000000 || hz/1000000>UINT32_MAX)return {};
        return static_cast<std::uint32_t>(hz/1000000);
    };
    const auto maximum=frequency("hw.cpufrequency_max"),current=frequency("hw.cpufrequency");
    if(!maximum || !current || !count || count>64)return {};
    CFDictionaryRef status=nullptr;if(IOPMCopyCPUPowerStatus(&status)!=kIOReturnSuccess || !status)return {};
    Cf<CFDictionaryRef> owned(status);auto limit=number(status,CFSTR(kIOPMCPUPowerLimitProcessorSpeedKey));
    if(!limit)if(const auto nested=CFDictionaryGetValue(status,CFSTR(kIOPMCPUPowerLimitsKey));nested && CFGetTypeID(nested)==CFDictionaryGetTypeID())
        limit=number(static_cast<CFDictionaryRef>(nested),CFSTR(kIOPMCPUPowerLimitProcessorSpeedKey));
    if(!limit || *limit<0 || *limit>100)return {};
    const auto limited=static_cast<std::uint32_t>(static_cast<std::uint64_t>(*maximum)*static_cast<unsigned>(*limit)/100);
    std::vector<std::array<std::uint32_t,6>> out(count);
    for(unsigned i=0;i<count;++i)out[i]={i,*maximum,*current,limited,0,0}; // 未提供客體 C-state，不冒用宿主的 idle state。
    return out;
}
namespace {
std::optional<std::array<std::uint64_t,2>> cpu_ticks(){
    host_cpu_load_info_data_t data{};mach_msg_type_number_t count=HOST_CPU_LOAD_INFO_COUNT;
    const auto host=mach_host_self();const auto result=host_statistics(host,HOST_CPU_LOAD_INFO,reinterpret_cast<host_info_t>(&data),&count);
    (void)mach_port_deallocate(mach_task_self(),host);if(result!=KERN_SUCCESS || count!=HOST_CPU_LOAD_INFO_COUNT)return {};
    std::uint64_t total=0;for(const auto value:data.cpu_ticks)total+=value;
    return std::array<std::uint64_t,2>{data.cpu_ticks[CPU_STATE_IDLE],total};
}
class CpuIdleWindow {
    std::mutex mutex_;std::condition_variable changed_;bool stopping_{};
    std::optional<std::uint32_t> idle_;std::chrono::steady_clock::time_point sampled_{};
    std::thread worker_;
public:
    CpuIdleWindow():worker_([this]{
        auto previous=cpu_ticks();std::unique_lock lock(mutex_);
        while(!changed_.wait_for(lock,std::chrono::milliseconds(250),[&]{return stopping_;})){
            lock.unlock();const auto current=cpu_ticks();const auto now=std::chrono::steady_clock::now();lock.lock();
            if(current && previous && (*current)[0]>=(*previous)[0] && (*current)[1]>(*previous)[1]){
                const auto idle=(*current)[0]-(*previous)[0],total=(*current)[1]-(*previous)[1];
                idle_=static_cast<std::uint32_t>(std::min<std::uint64_t>(100,idle*100/total));sampled_=now;
            }else idle_.reset();
            previous=current;
        }
    }){}
    ~CpuIdleWindow(){{std::lock_guard lock(mutex_);stopping_=true;}changed_.notify_one();worker_.join();}
    std::optional<std::uint32_t> sample(){std::lock_guard lock(mutex_);return std::chrono::steady_clock::now()-sampled_<=std::chrono::seconds(1)?idle_:std::nullopt;}
};
}
std::optional<std::array<std::uint8_t,16>> native_system_power_information(){
    static CpuIdleWindow cpu;const auto idle=cpu.sample();if(!idle)return {};
    const auto preferences=native_power_preferences();const auto status=native_power_status();if(!preferences || !status || (*status)[0]>1)return {};
    const auto& source=preferences->sources[(*status)[0]?0U:1U];
    if(!source.present || source.invalid[1] || !source.minutes[1] || *source.minutes[1]<0 || static_cast<std::uint64_t>(*source.minutes[1])>UINT32_MAX/60U)return {};
    const auto seconds=static_cast<std::uint64_t>(*source.minutes[1])*60U;
    const auto elapsed=CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState,kCGAnyInputEventType);
    if(!std::isfinite(elapsed) || elapsed<0)return {};
    std::uint32_t remaining=seconds?static_cast<std::uint32_t>(std::ceil(std::max(0.0,static_cast<double>(seconds)-elapsed))):UINT32_MAX;
    CFDictionaryRef assertions=nullptr;if(IOPMCopyAssertionsStatus(&assertions)!=kIOReturnSuccess || !assertions)return {};
    Cf<CFDictionaryRef> owned(assertions);
    const auto blocked=number(assertions,kIOPMAssertionTypePreventUserIdleSystemSleep),system=number(assertions,kIOPMAssertionTypePreventSystemSleep);
    if((blocked && *blocked) || (system && *system))remaining=UINT32_MAX;
    std::array<std::uint8_t,16> out{};const auto store=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)out[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    // macOS 的 user-idle 計時不依 CPU 閒置門檻；0 表示沒有該額外門檻。
    store(0,0);store(4,*idle);store(8,remaining);out[12]=2; // 客體尚無 NT 熱區，PO_TZ_INVALID_MODE。
    return out;
}
namespace {
std::optional<std::vector<std::uint8_t>> power_setting(const power::Guid& guid){
    std::optional<std::uint32_t> value;
    if(guid==power::acdc_source){
        Cf<CFTypeRef> info(IOPSCopyPowerSourcesInfo());if(!info)return {};
        const auto source=IOPSGetProvidingPowerSourceType(info.get());if(!source)return {};
        if(CFEqual(source,CFSTR(kIOPSACPowerValue)))value=0;
        else if(CFEqual(source,CFSTR(kIOPSBatteryPowerValue)))value=1;
        else if(CFEqual(source,CFSTR(kIOPMUPSPowerKey)))value=2;
    }else if(guid==power::battery_remaining){
        const auto status=native_power_status();if(status && (*status)[2]<=100)value=(*status)[2];
    }else if(guid==power::console_display || guid==power::session_display || guid==power::monitor_on){
        const auto display=CGMainDisplayID();if(display)value=CGDisplayIsAsleep(display)?0U:1U;
    }else if(guid==power::power_saving){
        if(@available(macOS 12.0,*))value=[NSProcessInfo processInfo].lowPowerModeEnabled?1U:0U;
    }else if(guid==power::lid_switch){
        const auto root=IOServiceGetMatchingService(MACH_PORT_NULL,IOServiceMatching("IOPMrootDomain"));
        if(!root)return {};
        Cf<CFTypeRef> closed(IORegistryEntryCreateCFProperty(root,CFSTR("AppleClamshellState"),kCFAllocatorDefault,0));IOObjectRelease(root);
        if(closed && CFGetTypeID(closed.get())==CFBooleanGetTypeID())value=CFBooleanGetValue(static_cast<CFBooleanRef>(closed.get()))?0U:1U;
    }
    if(!value)return {};
    std::vector<std::uint8_t> data(4);for(unsigned i=0;i<4;++i)data[i]=static_cast<std::uint8_t>(*value>>(8*i));return data;
}
class PowerNotifications final : public NativePowerNotifications {
    std::mutex mutex_;
    std::condition_variable initialized_;
    std::vector<NativePowerEvent> events_;
    std::map<power::Guid,std::vector<std::uint8_t>> settings_;
    std::thread thread_;
    std::atomic_bool stopping_{};
    CFRunLoopRef loop_{};
    io_connect_t connection_{};
    bool ready_{},available_{},delivery_failed_{},waiting_for_user_{};
    std::chrono::steady_clock::time_point resumed_;
    void enqueue(NativePowerEvent event)noexcept{
        std::lock_guard lock(mutex_);
        try{events_.push_back(std::move(event));}catch(...){delivery_failed_=true;}
    }
    void sample()noexcept{
        @autoreleasepool {
            try{
                for(const auto& guid:power::notification_settings){
                    const auto current=power_setting(guid);if(!current)continue;
                    const auto previous=settings_.find(guid);
                    if(previous!=settings_.end() && previous->second!=*current)enqueue({0x8013,guid,*current});
                    settings_[guid]=*current;
                }
                if(waiting_for_user_){
                    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-resumed_).count();
                    const auto idle=CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState,kCGAnyInputEventType);
                    // 自動喚醒後確實有使用者輸入才發送 RESUMESUSPEND；dark wake 不冒充互動喚醒。
                    if(idle>=0 && idle<=elapsed){waiting_for_user_=false;enqueue({7,{},{}});}
                }
            }catch(...){std::lock_guard lock(mutex_);delivery_failed_=true;}
        }
    }
    static void system_event(void* context,io_service_t,natural_t type,void* argument){
        auto& self=*static_cast<PowerNotifications*>(context);
        if(type==kIOMessageCanSystemSleep)IOAllowPowerChange(self.connection_,reinterpret_cast<intptr_t>(argument));
        else if(type==kIOMessageSystemWillSleep){self.waiting_for_user_=false;self.enqueue({4,{},{}});IOAllowPowerChange(self.connection_,reinterpret_cast<intptr_t>(argument));}
        else if(type==kIOMessageSystemHasPoweredOn){self.enqueue({18,{},{}});self.resumed_=std::chrono::steady_clock::now();self.waiting_for_user_=true;self.sample();}
    }
    static void timer_event(CFRunLoopTimerRef,void* context){static_cast<PowerNotifications*>(context)->sample();}
    static void source_event(void* context){static_cast<PowerNotifications*>(context)->sample();}
    void run(){
        @autoreleasepool {
            IONotificationPortRef port=nullptr;io_object_t notifier=0;
            connection_=IORegisterForSystemPower(this,&port,system_event,&notifier);
            const auto source=port?IONotificationPortGetRunLoopSource(port):nullptr;
            Cf<CFRunLoopSourceRef> power_source(IOPSNotificationCreateRunLoopSource(source_event,this));
            CFRunLoopTimerContext context{};context.info=this;
            Cf<CFRunLoopTimerRef> timer(CFRunLoopTimerCreate(kCFAllocatorDefault,CFAbsoluteTimeGetCurrent()+1,1,0,0,timer_event,&context));
            const bool usable=connection_ && source && timer;
            if(usable){
                sample();const auto loop=CFRunLoopGetCurrent();
                CFRunLoopAddSource(loop,source,kCFRunLoopDefaultMode);
                if(power_source)CFRunLoopAddSource(loop,power_source.get(),kCFRunLoopDefaultMode);
                CFRunLoopAddTimer(loop,timer.get(),kCFRunLoopDefaultMode);
                {std::lock_guard lock(mutex_);loop_=loop;CFRetain(loop_);available_=true;ready_=true;}initialized_.notify_one();
                while(!stopping_.load(std::memory_order_acquire))CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.25,false);
                CFRunLoopRemoveTimer(loop,timer.get(),kCFRunLoopDefaultMode);
                if(power_source)CFRunLoopRemoveSource(loop,power_source.get(),kCFRunLoopDefaultMode);
                CFRunLoopRemoveSource(loop,source,kCFRunLoopDefaultMode);
            }else{{std::lock_guard lock(mutex_);ready_=true;}initialized_.notify_one();}
            if(notifier)IODeregisterForSystemPower(&notifier);
            if(connection_)IOServiceClose(connection_);
            if(port)IONotificationPortDestroy(port);
        }
    }
public:
    PowerNotifications():thread_([this]{run();}){
        std::unique_lock lock(mutex_);initialized_.wait(lock,[&]{return ready_;});
    }
    ~PowerNotifications()override{
        stopping_.store(true,std::memory_order_release);
        {std::lock_guard lock(mutex_);if(loop_){CFRunLoopStop(loop_);CFRunLoopWakeUp(loop_);}}
        if(thread_.joinable())thread_.join();if(loop_)CFRelease(loop_);
    }
    bool available()const{return available_;}
    std::vector<NativePowerEvent> poll()override{
        std::lock_guard lock(mutex_);if(delivery_failed_)throw std::runtime_error("原生電源通知佇列配置失敗");
        std::vector<NativePowerEvent> events;events.swap(events_);return events;
    }
    std::optional<std::vector<std::uint8_t>> current_setting(const power::Guid& guid)override{
        @autoreleasepool{return power_setting(guid);}
    }
};
}
std::shared_ptr<NativePowerNotifications> native_power_notifications(){
    try{auto observer=std::make_shared<PowerNotifications>();return observer->available()?observer:nullptr;}
    catch(const std::system_error&){return {};}
}
namespace {
class PowerMonitor final : public NativePowerMonitor {
public:
    NativePowerNotificationSnapshot sample() override {
        NativePowerNotificationSnapshot out;
        if(const auto status=native_power_status()){
            if((*status)[2]<=100)out.battery=(*status)[2];
            if(@available(macOS 12.0,*))out.saving=(*status)[3];
        }
        // SYSTEM_POWER_CONDITION 須區分 UPS；SYSTEM_POWER_STATUS 只有 AC／非 AC。
        Cf<CFTypeRef> info(IOPSCopyPowerSourcesInfo());
        if(info)if(const auto source=IOPSGetProvidingPowerSourceType(info.get())){
            if(CFEqual(source,CFSTR(kIOPMACPowerKey)))out.source=0;
            else if(CFEqual(source,CFSTR(kIOPMBatteryPowerKey)))out.source=1;
            else if(CFEqual(source,CFSTR(kIOPMUPSPowerKey)))out.source=2;
        }
        // IOKit 的預設 port 為 MACH_PORT_NULL，保留 macOS 11.4 的部署支援。
        const auto root=IOServiceGetMatchingService(MACH_PORT_NULL,IOServiceMatching("IOPMrootDomain"));
        if(root){
            Cf<CFTypeRef> lid(IORegistryEntryCreateCFProperty(root,CFSTR("AppleClamshellState"),kCFAllocatorDefault,0));IOObjectRelease(root);
            if(lid && CFGetTypeID(lid.get())==CFBooleanGetTypeID())out.lid=CFBooleanGetValue(static_cast<CFBooleanRef>(lid.get()))?0:1;
        }
        std::uint32_t count=0;
        if(CGGetOnlineDisplayList(0,nullptr,&count)==kCGErrorSuccess && count)out.display=CGDisplayIsAsleep(CGMainDisplayID())?0:1;
        return out;
    }
};
}
std::shared_ptr<NativePowerMonitor> make_native_power_monitor(){return std::make_shared<PowerMonitor>();}
}
