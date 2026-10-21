#include "x64arm/windows_service_control.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <ServiceManagement/ServiceManagement.h>
#include <limits>
#include <pwd.h>
#include <unistd.h>
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"

namespace x64arm {
namespace {
std::optional<NativeServiceStatus> decode(CFDictionaryRef job,bool system) {
    if(!job || CFGetTypeID(job)!=CFDictionaryGetTypeID())return {};
    const auto label=static_cast<CFStringRef>(CFDictionaryGetValue(job,CFSTR("Label")));
    if(!label || CFGetTypeID(label)!=CFStringGetTypeID())return {};
    const auto length=CFStringGetLength(label);if(length<=0 || length>32767)return {};
    NativeServiceStatus value;value.system=system;value.label.resize(static_cast<std::size_t>(length));
    CFStringGetCharacters(label,CFRangeMake(0,length),reinterpret_cast<UniChar*>(value.label.data()));
    const auto number=[&](CFStringRef key,std::int64_t& output){const auto raw=CFDictionaryGetValue(job,key);return !raw || (CFGetTypeID(raw)==CFNumberGetTypeID() && CFNumberGetValue(static_cast<CFNumberRef>(raw),kCFNumberSInt64Type,&output));};
    std::int64_t pid=0,exit=0;
    if(!number(CFSTR("PID"),pid) || !number(CFSTR("LastExitStatus"),exit) || pid<0 || pid>std::numeric_limits<std::uint32_t>::max() || exit<std::numeric_limits<std::int32_t>::min() || exit>std::numeric_limits<std::int32_t>::max())return {};
    value.pid=static_cast<std::uint32_t>(pid);value.exit_status=static_cast<std::int32_t>(exit);
    const auto string=[&](CFTypeRef raw)->std::optional<std::u16string>{
        if(!raw)return std::u16string{};if(CFGetTypeID(raw)!=CFStringGetTypeID())return {};
        const auto text=static_cast<CFStringRef>(raw);const auto count=CFStringGetLength(text);if(count<0 || count>32767)return {};
        std::u16string result(static_cast<std::size_t>(count),u'\0');CFStringGetCharacters(text,CFRangeMake(0,count),reinterpret_cast<UniChar*>(result.data()));return result;
    };
    std::vector<std::u16string> arguments;const auto array=CFDictionaryGetValue(job,CFSTR("ProgramArguments"));
    if(array && CFGetTypeID(array)==CFArrayGetTypeID()){
        const auto list=static_cast<CFArrayRef>(array);const auto count=CFArrayGetCount(list);
        if(count>=0 && count<=1024)for(CFIndex i=0;i<count;++i){const auto part=string(CFArrayGetValueAtIndex(list,i));if(!part){arguments.clear();break;}arguments.push_back(*part);}
    }
    const auto program=string(CFDictionaryGetValue(job,CFSTR("Program")));
    if(arguments.empty() && program && !program->empty())arguments.push_back(*program);
    if(!arguments.empty()){
        for(const auto& part:arguments){if(!value.command.empty())value.command+=u' ';value.command+=quote_windows_argument(part);}
        value.configured=true;
    }
    const auto run=CFDictionaryGetValue(job,CFSTR("RunAtLoad")),keep=CFDictionaryGetValue(job,CFSTR("KeepAlive"));
    value.automatic=run==kCFBooleanTrue || keep==kCFBooleanTrue || (keep && CFGetTypeID(keep)==CFDictionaryGetTypeID() && CFDictionaryGetCount(static_cast<CFDictionaryRef>(keep))>0);
    const auto account=string(CFDictionaryGetValue(job,CFSTR("UserName")));
    if(account && !account->empty())value.account=*account==u"root"?u"LocalSystem":*account;
    else if(system)value.account=u"LocalSystem";
    else if(const auto user=::getpwuid(::geteuid());user && user->pw_name)value.account=utf8_to_utf16(user->pw_name);
    return value;
}
}
std::optional<NativeServiceCatalog> native_service_catalog() {
    NativeServiceCatalog catalog;
    for(unsigned domain=0;domain<2;++domain){
        const auto jobs=SMCopyAllJobDictionaries(domain?kSMDomainSystemLaunchd:kSMDomainUserLaunchd);
        if(!jobs)continue;
        catalog.domains[domain]=true;const auto count=CFArrayGetCount(jobs);
        if(count<0 || count>16384){CFRelease(jobs);return {};}
        try{for(CFIndex i=0;i<count;++i){const auto job=decode(static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(jobs,i)),domain!=0);if(!job){CFRelease(jobs);return {};}catalog.jobs.push_back(*job);}}
        catch(...){CFRelease(jobs);throw;}
        CFRelease(jobs);
    }
    if(!catalog.domains[0] && !catalog.domains[1])return {};
    return catalog;
}
std::optional<NativeServiceStatus> native_service_status(std::u16string_view name,bool system) {
    if(name.empty() || name.size()>32767)return {};
    const auto label=CFStringCreateWithCharacters(nullptr,reinterpret_cast<const UniChar*>(name.data()),static_cast<CFIndex>(name.size()));if(!label)return {};
    const auto job=SMJobCopyDictionary(system?kSMDomainSystemLaunchd:kSMDomainUserLaunchd,label);CFRelease(label);
    try{auto value=decode(job,system);if(job)CFRelease(job);return value;}catch(...){if(job)CFRelease(job);throw;}
}
}
