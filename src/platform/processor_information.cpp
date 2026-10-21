#include "x64arm/processor_information.hpp"
#include "x64arm/memory.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>
#endif

namespace x64arm {
namespace {
#if defined(__APPLE__)
template<class T> class Cf {
    T value_;
public:
    explicit Cf(T value):value_(value){}
    ~Cf(){if(value_)CFRelease(value_);}
    T get()const{return value_;}
    Cf(const Cf&)=delete;
    Cf& operator=(const Cf&)=delete;
};
std::string sysctl_text(const char* name){
    std::array<char,256> text{};std::size_t size=text.size();
    if(sysctlbyname(name,text.data(),&size,nullptr,0) || !size || size>text.size() || text[size-1])return {};
    return text.data();
}
std::uint32_t sysctl_mhz(const char* name){
    std::uint64_t value=0;std::size_t size=sizeof(value);
    if(sysctlbyname(name,&value,&size,nullptr,0) || (size!=4 && size!=8) || value<1000000 || value/1000000>UINT32_MAX)return 0;
    return static_cast<std::uint32_t>(value/1000000);
}
[[maybe_unused]] std::uint32_t apple_dvfs_maximum(){
    // Apple Silicon 不提供 hw.cpufrequency_max。動態列舉電源管理器的
    // SRAM 電壓／頻率表，不依晶片型號或固定的 voltage-states 編號分支。
    io_iterator_t iterator=0;
    if(IOServiceGetMatchingServices(MACH_PORT_NULL,IOServiceMatching("AppleARMIODevice"),&iterator)!=KERN_SUCCESS)return 0;
    std::uint32_t maximum=0;
    while(const auto entry=IOIteratorNext(iterator)){
        io_name_t name{};const bool manager=IORegistryEntryGetName(entry,name)==KERN_SUCCESS && std::string_view(name)=="pmgr";
        CFMutableDictionaryRef dictionary=nullptr;
        const auto status=manager?IORegistryEntryCreateCFProperties(entry,&dictionary,kCFAllocatorDefault,0):KERN_FAILURE;
        IOObjectRelease(entry);Cf owned(dictionary);
        if(status!=KERN_SUCCESS || !dictionary)continue;
        const auto count=CFDictionaryGetCount(dictionary);if(count<0 || count>4096)continue;
        std::vector<const void*> keys(static_cast<std::size_t>(count)),values(keys.size());
        CFDictionaryGetKeysAndValues(dictionary,keys.data(),values.data());
        for(std::size_t i=0;i<keys.size();++i){
            if(CFGetTypeID(keys[i])!=CFStringGetTypeID() || CFGetTypeID(values[i])!=CFDataGetTypeID())continue;
            std::array<char,128> buffer{};
            if(!CFStringGetCString(static_cast<CFStringRef>(keys[i]),buffer.data(),buffer.size(),kCFStringEncodingUTF8))continue;
            const std::string_view key(buffer.data());
            if(!key.starts_with("voltage-states") || !key.ends_with("-sram") || key.size()<=19)continue;
            const auto index=key.substr(14,key.size()-19);
            if(!std::all_of(index.begin(),index.end(),[](char c){return c>='0' && c<='9';}))continue;
            const auto data=static_cast<CFDataRef>(values[i]);const auto length=CFDataGetLength(data);
            if(length<16 || length>8192 || length%8)continue;
            const auto* bytes=CFDataGetBytePtr(data);std::uint32_t peak=0;bool valid=true;
            for(CFIndex offset=0;offset<length;offset+=8){
                std::uint32_t raw=0;std::memcpy(&raw,bytes+offset,sizeof(raw));
                if(!raw)continue;
                // 已知表格單位為 Hz 或 kHz，依合理值域辨識，不綁定 M 系列代數。
                const auto mhz=raw>=100000000U?raw/1000000U:raw/1000U;
                if(mhz<500 || mhz>20000){valid=false;break;}
                peak=std::max(peak,mhz);
            }
            // 非 CPU 的 SRAM 表也存在。保守採用 psutil 相同的 CPU 上限門檻；
            // 低於 2 GHz 的 GPU／NPU 表不公告為 CPU。此來源僅供最高標稱頻率。
            if(valid && peak>=2000)maximum=std::max(maximum,peak);
        }
    }
    IOObjectRelease(iterator);return maximum;
}
#endif
}
const ProcessorInformation& native_processor_information(){
    static const ProcessorInformation information=[] {
        ProcessorInformation result;
#if defined(__APPLE__)
        const auto brand=sysctl_text("machdep.cpu.brand_string");
        if(!brand.empty()){
            auto display=brand;
            if(display.size()>41){std::size_t end=41;while(end && (static_cast<unsigned char>(display[end])&0xc0U)==0x80U)--end;display.resize(end);}
            result.name=display+" (x64)";
        }
        const auto vendor=sysctl_text("machdep.cpu.vendor");
        if(brand.starts_with("Apple "))result.manufacturer="Apple Inc.";
        else if(!vendor.empty())result.manufacturer=vendor;
        result.nominal_mhz=sysctl_mhz("hw.cpufrequency");
        result.maximum_mhz=sysctl_mhz("hw.cpufrequency_max");
#if defined(__aarch64__) || defined(__arm64__)
        if(!result.maximum_mhz)result.maximum_mhz=apple_dvfs_maximum();
#endif
#endif
        return result;
    }();return information;
}
ProcessorInformation guest_processor_information(const GuestMemory& memory){
    auto result=native_processor_information();const auto& identity=memory.processor_identity();
    result.name=identity.brand.data();result.nominal_mhz=identity.base_mhz;result.maximum_mhz=identity.maximum_mhz;
    return result;
}
}
