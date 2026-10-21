#include "shared_policy.hpp"
#include "x64arm/shared_wake.hpp"
#include <chrono>
#include "x64arm/windows_services.hpp"
#include "x64arm/cpu_features.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/windows_cpu_time.hpp"
#include "x64arm/windows_power.hpp"
#include "x64arm/windows_default_environment.hpp"
#include "x64arm/registry_store.hpp"
#include "appcontainer_identity.hpp"
#include "x64arm/system_performance.hpp"
#include <algorithm>
#include <bit>
#include <functional>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif
namespace x64arm {
namespace {
// 與虛擬 Windows 的本機帳號／電腦名稱一致；沒有網域加入或 AD 升級作業。
struct VirtualDomainState {
    std::u16string_view computer{u"X64ARM"};
    std::uint32_t join_status{1},role{},flags{},upgrade{},operation{};
};
constexpr VirtualDomainState virtual_domain;
std::u16string environment_key(std::u16string value) {
    for (auto& c : value) if (c>=u'a' && c<=u'z') c=static_cast<char16_t>(c-32);
    return value;
}
// 設定檔根目錄共用 ProfileList；不受程序自行 SetEnvironmentVariable 的值影響。
std::u16string profile_directory(const RegistryState& registry,std::u16string sid,unsigned kind,bool current) {
    const auto defaults=guest_default_environment(true);auto variables=defaults;
    variables[u"SYSTEMDRIVE"]={u"SystemDrive",u"C:"};variables[u"PROGRAMDATA"]={u"ProgramData",u"C:\\ProgramData"};
    const auto expand=[&](std::u16string value){
        std::u16string output;
        for(std::size_t at=0;at<value.size();){if(value[at]!=u'%'){output.push_back(value[at++]);continue;}const auto end=value.find(u'%',at+1);if(end==value.npos){output.append(value.substr(at));break;}const auto key=environment_key(value.substr(at+1,end-at-1));const auto found=variables.find(key);output.append(found==variables.end()?value.substr(at,end-at+1):found->second.second);at=end+1;}
        if(output.size()>32767)throw std::length_error("設定檔路徑過長");return output;
    };
    const auto root=std::u16string(1,2)+u"\\software\\microsoft\\windows nt\\currentversion\\profilelist";
    const auto read=[&](const std::u16string& path,const std::u16string& name,std::u16string fallback){
        const auto key=registry.keys.find(path);if(key==registry.keys.end())return fallback;const auto found=key->second.find(name);if(found==key->second.end())return fallback;const auto& value=found->second;
        if((value.type!=1 && value.type!=2) || value.bytes.size()%2)throw std::invalid_argument("設定檔登錄值格式錯誤");
        std::u16string text;for(std::size_t at=0;at+1<value.bytes.size();at+=2){const auto c=static_cast<char16_t>(value.bytes[at]|(static_cast<unsigned>(value.bytes[at+1])<<8));if(!c)break;text.push_back(c);}if(text.size()>32767)throw std::length_error("設定檔路徑過長");return value.type==2?expand(text):text;
    };
    const auto profiles=read(root,u"profilesdirectory",u"C:\\Users");if(kind==0)return profiles;
    if(kind==1){auto value=read(root,u"default",u"Default");if(!value.empty() && value.find(u':')==value.npos && value.front()!=u'\\' && value.front()!=u'/')value=profiles+u"\\"+value;return value;}
    if(kind==2)return read(root,u"programdata",u"C:\\ProgramData");
    for(auto& c:sid)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
    return read(root+u"\\"+sid,u"profileimagepath",current?defaults.at(u"USERPROFILE").second:std::u16string{});
}
// 與 CPUID、GetSystemInfo 及排程器一致：每個客體邏輯 CPU 為一核心、共用單一群組。
// 不把宿主的核心或快取資訊當成客體可平行執行的資源。
std::vector<std::uint8_t> processor_relationships(std::uint32_t requested,unsigned processors,bool x86,bool legacy=false) {
    std::vector<std::uint8_t> bytes;
    const auto append=[&](std::uint32_t relation,std::uint64_t mask,const VirtualProcessorCache* cache=nullptr) {
        const auto base=bytes.size();const auto size=legacy?(x86?24U:32U):relation==4?(x86?76U:80U):relation==2?(x86?52U:56U):(x86?44U:48U);bytes.resize(base+size);
        const auto put=[&](std::size_t offset,std::uint64_t value,unsigned count) {
            for(unsigned i=0;i<count;++i)bytes[base+offset+i]=static_cast<std::uint8_t>(value>>(i*8));
        };
        const auto cache_fields=[&](unsigned offset){
            put(offset,cache->level,1);put(offset+1,cache->ways,1);put(offset+2,cache->line_size,2);put(offset+4,cache->size,4);
            put(offset+8,cache->type==3?0:cache->type==2?1:2,4); // Windows 的 Unified／Instruction／Data。
        };
        if(legacy){put(0,mask,x86?4:8);put(x86?4:8,relation,4);if(cache)cache_fields(x86?8:16);return;}
        put(0,relation,4);put(4,size,4);
        if(cache){cache_fields(8);put(38,1,2);put(40,mask,x86?4:8);}
        else if(relation==4) {
            put(8,1,2);put(10,1,2); // GROUP_RELATIONSHIP 的最大／啟用群組數。
            put(32,processors,1);put(33,processors,1);put(72,mask,x86?4:8);
        } else {put(30,1,2);put(32,mask,x86?4:8);} // GroupCount / GROUP_AFFINITY.Mask。
    };
    const auto mask=processors==64?UINT64_MAX:(UINT64_C(1)<<processors)-1;
    const auto relation=[&](std::uint32_t kind){
        if(kind==0)for(unsigned cpu=0;cpu<processors;++cpu)append(0,UINT64_C(1)<<cpu);
        else if(kind==2)for(const auto& cache:virtual_processor_caches(processors)){
            if(cache.level==3)append(2,mask,&cache);
            else for(unsigned cpu=0;cpu<processors;++cpu)append(2,UINT64_C(1)<<cpu,&cache);
        }else append(kind,mask);
    };
    if(requested==0xffff){for(const auto kind:{0U,1U,2U,3U})relation(kind);if(!legacy)relation(4);}
    else if(requested==6)relation(1);
    else relation(requested);
    return bytes;
}
}
std::span<const WindowsServiceDescriptor> environment_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"userenv.dll","GetProfilesDirectoryA",2},{"userenv.dll","GetProfilesDirectoryW",2},
        {"userenv.dll","GetDefaultUserProfileDirectoryA",2},{"userenv.dll","GetDefaultUserProfileDirectoryW",2},
        {"userenv.dll","GetAllUsersProfileDirectoryA",2},{"userenv.dll","GetAllUsersProfileDirectoryW",2},
        {"userenv.dll","GetUserProfileDirectoryA",3},{"userenv.dll","GetUserProfileDirectoryW",3},
        {"userenv.dll","CreateEnvironmentBlock",3},{"userenv.dll","DestroyEnvironmentBlock",1},
        {"userenv.dll","EnterCriticalPolicySection",1},{"userenv.dll","LeaveCriticalPolicySection",1},
        {"userenv.dll","CreateAppContainerProfile",6},{"userenv.dll","DeleteAppContainerProfile",1},
        {"userenv.dll","DeriveAppContainerSidFromAppContainerName",2},{"userenv.dll","GetAppContainerFolderPath",2},
        {"userenv.dll","ExpandEnvironmentStringsForUserA",4},{"userenv.dll","ExpandEnvironmentStringsForUserW",4},
        {"crt","getenv",1},{"crt","_wgetenv",1},{"crt","_putenv",1},{"crt","_wputenv",1},
        {"crt","_dupenv_s",3},{"crt","_wdupenv_s",3},{"crt","getenv_s",4},{"crt","_wgetenv_s",4},
        {"crt","_putenv_s",2},{"crt","_wputenv_s",2},
        {"kernel32.dll","GetEnvironmentVariableA",3},{"kernel32.dll","GetEnvironmentVariableW",3},
        {"kernel32.dll","SetEnvironmentVariableA",2},{"kernel32.dll","SetEnvironmentVariableW",2},
        {"kernel32.dll","NeedCurrentDirectoryForExePathA",1},{"kernel32.dll","NeedCurrentDirectoryForExePathW",1},
        {"kernel32.dll","GetEnvironmentStrings",0},{"kernel32.dll","GetEnvironmentStringsA",0},{"kernel32.dll","GetEnvironmentStringsW",0},
        {"kernel32.dll","FreeEnvironmentStringsA",1},{"kernel32.dll","FreeEnvironmentStringsW",1},
        {"kernel32.dll","SetCurrentDirectoryA",1},{"kernel32.dll","SetCurrentDirectoryW",1},{"crt","_chdir",1},{"crt","_wchdir",1},
        {"kernel32.dll","GetNumaHighestNodeNumber",1},{"kernel32.dll","GetPhysicallyInstalledSystemMemory",1},
        {"kernel32.dll","GetNumaNodeProcessorMask",2},{"kernel32.dll","GetNumaNodeProcessorMaskEx",2},
        {"kernel32.dll","GetNumaAvailableMemoryNode",2},{"kernel32.dll","GetNumaAvailableMemoryNodeEx",2},
        {"kernel32.dll","GetSystemCpuSetInformation",5},{"kernel32.dll","GetLogicalProcessorInformationEx",3},{"kernel32.dll","GetLogicalProcessorInformation",2},
        {"kernel32.dll","GetActiveProcessorGroupCount",0},{"kernel32.dll","GetMaximumProcessorGroupCount",0},
        {"kernel32.dll","GetActiveProcessorCount",1},{"kernel32.dll","GetMaximumProcessorCount",1},
        {"kernel32.dll","GetCurrentProcessorNumberEx",1},{"kernel32.dll","GetCurrentProcessorNumber",0},
        {"kernel32.dll","SetThreadGroupAffinity",3},{"kernel32.dll","GetThreadGroupAffinity",2},{"kernel32.dll","GetProcessGroupAffinity",3},
        {"kernel32.dll","GetComputerNameA",2},{"kernel32.dll","GetComputerNameW",2},
        {"kernel32.dll","GetComputerNameExA",3},{"kernel32.dll","GetComputerNameExW",3},
        {"netapi32.dll","DsRoleGetPrimaryDomainInformation",3},{"netapi32.dll","DsRoleFreeMemory",1},
        {"netapi32.dll","NetUserGetInfo",4},{"netapi32.dll","NetGetJoinInformation",3},{"netapi32.dll","NetGetAadJoinInformation",2},{"netapi32.dll","NetFreeAadJoinInformation",1},{"netapi32.dll","NetApiBufferFree",1},{"userenv.dll","RegisterGPNotification",2},{"userenv.dll","UnregisterGPNotification",1},
        {"wtsapi32.dll","WTSRegisterSessionNotification",2},{"wtsapi32.dll","WTSUnRegisterSessionNotification",1},
        {"wtsapi32.dll","WTSQuerySessionInformationA",5},{"wtsapi32.dll","WTSQuerySessionInformationW",5},{"wtsapi32.dll","WTSFreeMemory",1},
        {"kernel32.dll","WTSGetActiveConsoleSessionId",0},
        {"ntdll.dll","NtQuerySystemInformation",4},{"kernel32.dll","GetSystemPowerStatus",1},
        {"powrprof.dll","PowerDeterminePlatformRole",0},{"powrprof.dll","PowerDeterminePlatformRoleEx",1},{"powrprof.dll","GetPwrCapabilities",1},
        {"powrprof.dll","PowerReadFriendlyName",6},{"powrprof.dll","PowerReadACValue",7},{"powrprof.dll","PowerReadDCValue",7},
        {"powrprof.dll","PowerReadACValueIndex",5},{"powrprof.dll","PowerReadDCValueIndex",5},
        {"powrprof.dll","PowerEnumerate",7},{"powrprof.dll","PowerReadDescription",6},{"powrprof.dll","GetActivePwrScheme",1},{"powrprof.dll","EnumPwrSchemes",2},{"kernel32.dll","GetDevicePowerState",2},
        {"kernel32.dll","SetThreadExecutionState",1,0,0,1},{"kernel32.dll","SetSystemPowerState",2,0,0,3},
        {"kernel32.dll","PowerCreateRequest",1},{"kernel32.dll","PowerSetRequest",2},{"kernel32.dll","PowerClearRequest",2},
        {"advapi32.dll","EventRegister",4},{"advapi32.dll","EventUnregister",1,1},
        {"advapi32.dll","EventEnabled",2,1},{"advapi32.dll","EventProviderEnabled",3,5},
        {"advapi32.dll","EventWrite",4,1},{"advapi32.dll","EventWriteTransfer",6,1},
        {"advapi32.dll","EventSetInformation",4,1},
        {"advapi32.dll","RegisterTraceGuidsA",8},{"advapi32.dll","RegisterTraceGuidsW",8},
        {"advapi32.dll","UnregisterTraceGuids",1,1},
        {"advapi32.dll","GetTraceEnableFlags",1,1},{"advapi32.dll","GetTraceEnableLevel",1,1},
        {"advapi32.dll","GetTraceLoggerHandle",1},{"advapi32.dll","TraceEvent",2,1},{"advapi32.dll","TraceMessage",4,1,0,0,true},{"advapi32.dll","TraceMessageVa",5,1},
        {"kernel32.dll","ExpandEnvironmentStringsA",3},{"kernel32.dll","ExpandEnvironmentStringsW",3}
    };return table;
}
void WindowsServices::import_guest_environment(GuestMemory& memory,const GuestState& state,int descriptor){
#if !defined(_WIN32)
    struct stat info{};if(::fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_size<4 || info.st_size>static_cast<off_t>(output_limit))throw HostCallFault("子程序環境資料無效");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));std::size_t offset=0;
    while(offset<bytes.size()){const auto count=::pread(descriptor,bytes.data()+offset,bytes.size()-offset,static_cast<off_t>(offset));if(count<0 && errno==EINTR)continue;if(count<=0)throw HostCallFault("子程序環境資料截斷");offset+=static_cast<std::size_t>(count);}
    const auto environment=decode_guest_environment(bytes,true);if(!environment)throw HostCallFault("子程序環境格式無效");runtime_.environment=*environment;runtime_.crt_environment=*environment;rebuild_crt_environment(memory,state);
    // 匯入發生在子程序啟動前；以啟動資料取代建構時的預設快照，之後 _putenv 不再改動初始環境。
    const auto previous_initial=runtime_.initial_environ;
    runtime_.initial_environ=memory.load(GuestAddress{runtime_.environ_cell},x86_?32:64);
    runtime_.initial_wenviron=memory.load(GuestAddress{runtime_.wenviron_cell},x86_?32:64);
    memory.store(GuestAddress{runtime_base_.value+112},x86_?32:64,runtime_.initial_environ);
    memory.store(GuestAddress{runtime_base_.value+120},x86_?32:64,runtime_.initial_wenviron);
    if(previous_initial && previous_initial!=runtime_.initial_environ)(void)free_heap(memory,previous_initial,state);
#else
    (void)memory;(void)state;(void)descriptor;throw HostCallFault("此平台未支援子程序環境");
#endif
}
void WindowsServices::rebuild_crt_environment(GuestMemory& memory,const GuestState& state) {
    const auto& environment=runtime_.crt_environment;const unsigned unit=x86_?4:8;const auto pointers=(environment.size()+1)*unit;
    std::vector<std::uint8_t> bytes(pointers*2);std::vector<std::pair<std::uint64_t,std::uint64_t>> positions;
    for (const auto& [key,entry] : environment) {
        (void)key;auto text=entry.first+u"="+entry.second;const auto ansi=utf16_to_utf8(text);const auto wide=utf16_bytes(text);
        const auto narrow_offset=bytes.size();bytes.insert(bytes.end(),ansi.begin(),ansi.end());bytes.push_back(0);
        const auto wide_offset=bytes.size();bytes.insert(bytes.end(),wide.begin(),wide.end());positions.emplace_back(narrow_offset,wide_offset);
    }
    const auto base=allocate_heap(memory,bytes.size());if (!base) throw HostCallFault("CRT 環境變數配置失敗");
    for (std::size_t i=0;i<positions.size();++i) for (unsigned byte=0;byte<unit;++byte) {
        bytes[i*unit+byte]=static_cast<std::uint8_t>((base+positions[i].first)>>(byte*8));
        bytes[pointers+i*unit+byte]=static_cast<std::uint8_t>((base+positions[i].second)>>(byte*8));
    }
    std::map<std::u16string,std::pair<std::uint64_t,std::uint64_t>> values;
    try{
        std::size_t index=0;
        for (const auto& [key,entry] : environment) {
            const auto name=utf16_to_utf8(entry.first);
            values.emplace(key,std::pair{base+positions[index].first+name.size()+1,base+positions[index].second+(entry.first.size()+1)*2});++index;
        }
        memory.write(GuestAddress{base},bytes);
        memory.store(GuestAddress{runtime_.environ_cell},x86_?32:64,base);
        memory.store(GuestAddress{runtime_.wenviron_cell},x86_?32:64,base+pointers);
        if(!runtime_.initial_environ){
            runtime_.initial_environ=base;runtime_.initial_wenviron=base+pointers;
            memory.store(GuestAddress{runtime_base_.value+112},x86_?32:64,base);
            memory.store(GuestAddress{runtime_base_.value+120},x86_?32:64,base+pointers);
        }
    }catch(...){(void)free_heap(memory,base,state);throw;}
    runtime_.environment_values=std::move(values);
    if (runtime_.environment_allocation && runtime_.environment_allocation!=runtime_.initial_environ) free_heap(memory,runtime_.environment_allocation,state);
    runtime_.environment_allocation=base;
}
void WindowsServices::discard_policy_thread(std::uint64_t thread){
    const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    for(auto it=policy_sections_.begin();it!=policy_sections_.end();){
        if(it->second.thread!=thread){++it;continue;}
        try{if(it->second.generation)(void)it->second.policy->release(thread,it->second.generation,now);}catch(...){}
        it=policy_sections_.erase(it);
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_environment(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(service.symbol.starts_with("Power") || service.symbol=="GetPwrCapabilities" || service.symbol=="SetThreadExecutionState" || service.symbol=="SetSystemPowerState" || service.symbol=="GetActivePwrScheme" || service.symbol=="EnumPwrSchemes" || service.symbol=="GetDevicePowerState")return invoke_power(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
#include "policy_handlers.inc"
#include "appcontainer_handlers.inc"
    const bool wide=symbol.back()=='W' || symbol.starts_with("_w");
    if(symbol=="EventRegister" || symbol=="EventUnregister" || symbol=="EventEnabled" || symbol=="EventProviderEnabled" || symbol=="EventWrite" || symbol=="EventWriteTransfer" ||
       symbol=="RegisterTraceGuidsA" || symbol=="RegisterTraceGuidsW" || symbol=="UnregisterTraceGuids" || symbol=="GetTraceEnableFlags" || symbol=="GetTraceEnableLevel" || symbol=="GetTraceLoggerHandle" || symbol=="TraceEvent" || symbol=="TraceMessage" || symbol=="TraceMessageVa")return invoke_etw_provider(state,memory,service);
    const auto text=[&](unsigned i){return wide?read_utf16(memory,GuestAddress{arg(i)}):utf8_to_utf16(read_ansi(memory,arg(i),32767));};
    const auto encode=[&](std::u16string_view value) {
        if (wide) return utf16_bytes(value);
        const auto ansi=utf16_to_utf8(value);std::vector<std::uint8_t> bytes(ansi.begin(),ansi.end());bytes.push_back(0);return bytes;
    };
    const auto crt_environment_value=[&](unsigned index){
        const auto found=runtime_.environment_values.find(environment_key(text(index)));
        return found==runtime_.environment_values.end()?0:wide?found->second.second:found->second.first;
    };
    const auto processor_mask=runtime_.processor_count==64?UINT64_MAX:(UINT64_C(1)<<runtime_.processor_count)-1;
    std::uint64_t result=0;
    if(symbol.starts_with("NeedCurrentDirectoryForExePath")){
        if(!arg(0)){last_error_=87;arg.return_to(state,0);return std::nullopt;}
        result=executable_needs_current_directory(text(0));arg.return_to(state,result);return std::nullopt;
    }
    if(symbol.starts_with("GetProfilesDirectory") || symbol.starts_with("GetDefaultUserProfileDirectory") || symbol.starts_with("GetAllUsersProfileDirectory") || symbol.starts_with("GetUserProfileDirectory")){
        const auto finish=[&](bool success,unsigned error=0){if(!success)last_error_=error;arg.return_to(state,success);return std::optional<std::uint32_t>{};};
        initialize_tokens();
        const bool user=symbol.starts_with("GetUserProfileDirectory");const auto buffer=arg(user?1:0),size=arg(user?2:1);std::uint64_t token_id=1;
        if(user){const auto handle=runtime_.token_handles.find(arg(0));if(handle==runtime_.token_handles.end())return finish(false,6);if(!(handle->second&8))return finish(false,5);token_id=runtime_.token_handle_objects.at(arg(0));}
        if(!size)return finish(false,87);
        try{
            const auto capacity=memory.load(GuestAddress{size},32);RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return finish(false,transaction.error);
            const auto kind=user?3U:symbol.starts_with("GetDefault")?1U:symbol.starts_with("GetAll")?2U:0U;
            const auto directory=profile_directory(runtime_.registry,utf8_to_utf16(token_user_sid_string(token_id)),kind,token_user_sid_string(token_id)==token_user_sid_string(1));if(directory.empty())return finish(false,2);
            const auto bytes=encode(directory);const auto count=bytes.size()/(wide?2U:1U);memory.store(GuestAddress{size},32,count);
            if(!buffer || capacity<count)return finish(false,122);memory.write(GuestAddress{buffer},bytes);return finish(true);
        }catch(const MemoryFault&){return finish(false,998);}catch(const std::invalid_argument&){return finish(false,13);}catch(const std::length_error&){return finish(false,206);}catch(const std::bad_alloc&){return finish(false,8);}
    }
    else if(symbol=="CreateEnvironmentBlock" || symbol=="DestroyEnvironmentBlock" || symbol.starts_with("ExpandEnvironmentStringsForUser")){
        const auto finish=[&](bool success,unsigned error=0){if(!success)last_error_=error;arg.return_to(state,success);return std::optional<std::uint32_t>{};};
        if(symbol=="DestroyEnvironmentBlock"){const auto found=runtime_.environment_blocks.find(arg(0));if(found==runtime_.environment_blocks.end() || !found->second)return finish(false,87);if(!free_heap(memory,arg(0),state))return finish(false,87);runtime_.environment_blocks.erase(found);return finish(true);}
        const bool creating=symbol=="CreateEnvironmentBlock";const auto handle=arg(creating?1:0);bool user=handle!=0;
        if(creating){if(!arg(0))return finish(false,87);memory.store(GuestAddress{arg(0)},x86_?32:64,0);}
        if(user){const auto access=runtime_.token_handles.find(handle);if(access==runtime_.token_handles.end())return finish(false,6);const auto& token=runtime_.token_objects.at(runtime_.token_handle_objects.at(handle));const auto required=token.type==1?10U:8U;if((access->second&required)!=required)return finish(false,5);if(token.user.sid!=runtime_.token_objects.at(1).user.sid)return finish(false,50);}
        try{
            GuestEnvironment values=creating && static_cast<std::uint32_t>(arg(2))?runtime_.environment:GuestEnvironment{};
            for(const auto& [key,value]:guest_default_environment(user))values[key]=value;
            const auto expand=[&](std::u16string_view source,const GuestEnvironment& environment){std::u16string output;for(std::size_t at=0;at<source.size();){if(source[at]!=u'%'){output.push_back(source[at++]);continue;}const auto end=source.find(u'%',at+1);if(end==source.npos){output.append(source.substr(at));break;}const auto found=environment.find(environment_key(std::u16string(source.substr(at+1,end-at-1))));output.append(found==environment.end()?source.substr(at,end-at+1):std::u16string_view(found->second.second));at=end+1;if(output.size()>32767)throw std::length_error("環境變數過長");}return output;};
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return finish(false,transaction.error);
            if(user){
                const auto profile=profile_directory(runtime_.registry,utf8_to_utf16(token_user_sid_string(1)),3,true);if(profile.empty())return finish(false,2);
                for(const auto& [key,suffix]:std::array{std::pair{u"USERPROFILE",u""},std::pair{u"APPDATA",u"\\AppData\\Roaming"},std::pair{u"LOCALAPPDATA",u"\\AppData\\Local"},std::pair{u"TEMP",u"\\AppData\\Local\\Temp"},std::pair{u"TMP",u"\\AppData\\Local\\Temp"}})values[key]={key,profile+suffix};
            }
            const auto merge=[&](std::u16string path,bool append_path){
                const auto found=runtime_.registry.keys.find(path);if(found==runtime_.registry.keys.end())return;
                GuestEnvironment expanding;
                for(const auto& [key,value]:found->second){if((value.type!=1 && value.type!=2) || key.empty() || key.find(u'=')!=key.npos || value.bytes.size()%2)continue;std::u16string content;for(std::size_t at=0;at+1<value.bytes.size();at+=2){const auto c=static_cast<char16_t>(value.bytes[at]|(static_cast<unsigned>(value.bytes[at+1])<<8));if(!c)break;content.push_back(c);}if(content.size()>32767)throw std::length_error("環境變數過長");const auto name=value.display_name.empty()?key:value.display_name,canonical=environment_key(name);
                    if(value.type==2){expanding[canonical]={name,std::move(content)};}
                    else {if(append_path && canonical==u"PATH" && values.contains(canonical) && !values[canonical].second.empty() && !content.empty())content=values[canonical].second+u";"+content;values[canonical]={name,std::move(content)};}}
                std::map<std::u16string,std::u16string> resolved;std::set<std::u16string> active;
                std::function<std::u16string(const std::u16string&)> resolve;
                resolve=[&](const std::u16string& key)->std::u16string{
                    if(resolved.contains(key))return resolved.at(key);const auto pending=expanding.find(key);
                    if(pending==expanding.end() || active.contains(key)){const auto existing=values.find(key);return existing==values.end()?u"%"+key+u"%":existing->second.second;}
                    if(active.size()>=32)throw std::length_error("環境展開巢狀過深");active.insert(key);const auto& raw=pending->second.second;std::u16string content;
                    for(std::size_t at=0;at<raw.size();){if(raw[at]!=u'%'){content.push_back(raw[at++]);continue;}const auto end=raw.find(u'%',at+1);if(end==raw.npos){content.append(raw.substr(at));break;}const auto reference=environment_key(raw.substr(at+1,end-at-1));if(!values.contains(reference) && !expanding.contains(reference))content.append(raw.substr(at,end-at+1));else content+=resolve(reference);at=end+1;if(content.size()>32767)throw std::length_error("環境變數過長");}
                    active.erase(key);resolved[key]=content;return content;
                };
                for(const auto& [key,value]:expanding){(void)value;(void)resolve(key);}
                for(const auto& [key,value]:expanding){auto content=resolved.at(key);if(append_path && key==u"PATH" && values.contains(key) && !values[key].second.empty() && !content.empty())content=values[key].second+u";"+content;values[key]={value.first,std::move(content)};}

            };
            merge(std::u16string(1,2)+u"\\system\\currentcontrolset\\control\\session manager\\environment",false);
            if(user){merge(std::u16string(1,1)+u"\\environment",true);merge(std::u16string(1,1)+u"\\volatile environment",false);}
            if(!creating){if(!arg(1) || !arg(2))return finish(false,87);const auto source=wide?read_utf16(memory,GuestAddress{arg(1)}):utf8_to_utf16(read_ansi(memory,arg(1),32767));const auto expanded=expand(source,values);const auto bytes=wide?utf16_bytes(expanded):encode(expanded);const auto count=static_cast<std::uint32_t>(arg(3));if(bytes.size()>(wide?static_cast<std::uint64_t>(count)*2:count))return finish(false,122);memory.write(GuestAddress{arg(2)},bytes);return finish(true);}
            if(runtime_.environment_blocks.size()>=4096)return finish(false,8);std::vector<std::uint8_t> bytes;for(const auto& [key,value]:values){(void)key;const auto entry=utf16_bytes(value.first+u"="+value.second);if(entry.size()>output_limit-bytes.size())return finish(false,8);bytes.insert(bytes.end(),entry.begin(),entry.end());}bytes.push_back(0);bytes.push_back(0);if(values.empty()){bytes.push_back(0);bytes.push_back(0);}const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)return finish(false,8);
            try{memory.write(GuestAddress{pointer},bytes);runtime_.environment_blocks.emplace(pointer,true);memory.store(GuestAddress{arg(0)},x86_?32:64,pointer);}catch(...){runtime_.environment_blocks.erase(pointer);(void)free_heap(memory,pointer,state);throw;}return finish(true);
        }catch(const std::bad_alloc&){return finish(false,8);}catch(const std::invalid_argument&){return finish(false,13);}catch(const std::length_error&){return finish(false,203);}
    }
    else if(symbol=="WTSGetActiveConsoleSessionId")result=1;
    else if(symbol=="WTSRegisterSessionNotification" || symbol=="WTSUnRegisterSessionNotification"){
        const auto window=gui_.windows.find(arg(0));
        if(window==gui_.windows.end())last_error_=1400;
        else {
            auto& subscription=window->second.session_notification;
            if(symbol=="WTSUnRegisterSessionNotification"){
                if(!subscription)last_error_=1168;
                else {if(!--subscription->second)subscription.reset();result=1;}
            }else if(subscription){
                if(subscription->second==UINT32_MAX)last_error_=8;else {++subscription->second;result=1;}
            }else if(static_cast<std::uint32_t>(arg(1))>1)last_error_=87;
            else {
                // 客體目前只有固定、連線中的本機 console session 1；註冊不合成狀態轉換。
                subscription=std::pair{static_cast<std::uint32_t>(arg(1)),1U};result=1;
            }
        }
    }
    else if(symbol=="WTSFreeMemory"){
        if(arg(0)){if(!runtime_.wts_allocations.contains(arg(0)) || !free_heap(memory,arg(0),state))last_error_=87;else runtime_.wts_allocations.erase(arg(0));}
    }else if(symbol.starts_with("WTSQuerySessionInformation")){
        if(!arg(3) || !arg(4))last_error_=87;
        else{
            memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);memory.check(GuestAddress{arg(4)},4,Access::Write);
            memory.store(GuestAddress{arg(3)},x86_?32:64,0);memory.store(GuestAddress{arg(4)},32,0);
            const auto session=static_cast<std::uint32_t>(arg(1)),info=static_cast<std::uint32_t>(arg(2));
            if(arg(0))last_error_=6;
            else if(session!=1 && session!=UINT32_MAX)last_error_=7022;
            else{
                std::vector<std::uint8_t> bytes;
                if(info==4)bytes={1,0,0,0};
                else if(info==5 || info==7)bytes=encode(info==5?u"user":u"X64ARM");
                else if(info==8 || info==29)bytes={0,0,0,0}; // 本機 active console，沒有 RDP 連線。
                else if(info==16)bytes={0,0};
                else last_error_=info>31?87:50;
                if(!bytes.empty()){
                    const auto buffer=allocate_heap(memory,bytes.size());
                    if(!buffer)last_error_=8;
                    else{memory.write(GuestAddress{buffer},bytes);runtime_.wts_allocations.insert(buffer);
                        memory.store(GuestAddress{arg(3)},x86_?32:64,buffer);memory.store(GuestAddress{arg(4)},32,bytes.size());result=1;}
                }
            }
        }
    }else if(symbol=="RegisterGPNotification" || symbol=="UnregisterGPNotification"){
        const auto event=runtime_.wait_objects.find(arg(0));
        if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event)last_error_=6;
        else if(!(event->second.access&2))last_error_=5;
        else last_error_=50; // 此虛擬系統尚無 Group Policy 套用服務，不能假註冊通知。
    }else if(symbol=="GetSystemPowerStatus"){
        if(!arg(0))last_error_=87;
        else {
            if(!runtime_.network.external_io)throw HostCallFault("原生電源狀態不支援 deterministic compare");
            memory.check(GuestAddress{arg(0)},12,Access::Write);
            if(const auto power=native_power_status()){memory.write(GuestAddress{arg(0)},*power);result=1;}else last_error_=50;
        }
    }else if(symbol=="NtQuerySystemInformation") {
        if(static_cast<std::uint32_t>(arg(0))!=8)result=0xc0000003; // STATUS_INVALID_INFO_CLASS。
        else if(runtime_.processor_count>1)result=0xc0000002; // 尚未分攤各 vCPU 的宿主 CPU 時間，不能回傳單核心大小的假資料。
        else {
            if(arg(3))memory.store(GuestAddress{arg(3)},32,48);
            if(static_cast<std::uint32_t>(arg(2))<48)result=0xc0000004;
            else if(!arg(1))result=0xc0000005;
            else {
                memory.check(GuestAddress{arg(1)},48,Access::Write);
                std::uint64_t user=runtime_.clock_ticks,kernel=0,idle=0;
                if(runtime_.host_clock) {
                    const auto cpu=host_process_cpu_time();
                    if(!cpu)result=0xc0000002;
                    else {
                        const auto current_user=cpu->user,current_kernel=cpu->kernel;
                        if(runtime_.processor_sample_initialized) {
                            const auto user_delta=current_user-runtime_.processor_sample_user,kernel_delta=current_kernel-runtime_.processor_sample_kernel;
                            const auto elapsed=runtime_.clock_ticks-runtime_.processor_sample_clock;
                            runtime_.processor_user+=user_delta;runtime_.processor_kernel+=kernel_delta;
                            // 這個程序是單一 vCPU 的執行器；等待與未獲宿主 CPU 的時間算 idle。
                            runtime_.processor_idle+=elapsed>user_delta+kernel_delta?elapsed-user_delta-kernel_delta:0;
                        }
                        runtime_.processor_sample_initialized=true;runtime_.processor_sample_clock=runtime_.clock_ticks;
                        runtime_.processor_sample_user=current_user;runtime_.processor_sample_kernel=current_kernel;
                        user=runtime_.processor_user;idle=runtime_.processor_idle;kernel=runtime_.processor_kernel+idle;
                    }
                }
                if(!result) {
                    memory.write(GuestAddress{arg(1)},std::array<std::uint8_t,48>{});
                    memory.store(GuestAddress{arg(1)},64,idle);memory.store(GuestAddress{arg(1)+8},64,kernel);memory.store(GuestAddress{arg(1)+16},64,user);
                }
            }
        }
    } else if(symbol=="RegisterTraceGuidsA" || symbol=="RegisterTraceGuidsW") {
        const auto count=static_cast<std::uint32_t>(arg(3));const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
        if(!arg(0) || !arg(2) || !arg(7) || (count && !arg(4)) || arg(5) || arg(6) || count>1024)result=87;
        else if(runtime_.event_providers.size()>=1024)result=8;
        else {
            memory.check(GuestAddress{arg(7)},8,Access::Write);
            EventProviderRegistration registration;memory.read(GuestAddress{arg(2)},registration.id);
            registration.classic=true;registration.callback=arg(0);registration.context=arg(1);
            if(count)memory.check(GuestAddress{arg(4)},static_cast<std::size_t>(count)*unit*2,Access::Write);
            for(std::uint32_t i=0;i<count;++i){
                const auto guid=memory.load(GuestAddress{arg(4)+i*unit*2},bits);
                if(!guid){result=87;break;}
                std::array<std::uint8_t,16> id{};memory.read(GuestAddress{guid},id);registration.trace_classes.emplace_back(0,id);
            }
            if(!result){
                const auto handle=runtime_.next_event_provider++;
                for(std::uint32_t i=0;i<count;++i){registration.trace_classes[i].first=runtime_.next_event_provider++;memory.store(GuestAddress{arg(4)+i*unit*2+unit},bits,registration.trace_classes[i].first);}
                runtime_.event_providers.emplace(handle,std::move(registration));
                memory.store(GuestAddress{arg(7)},64,handle);
            }
        }
    } else if(symbol=="GetTraceEnableFlags" || symbol=="GetTraceEnableLevel" || symbol=="GetTraceLoggerHandle" || symbol=="TraceEvent") {
        // 尚無啟用的 classic ETW session；provider registration handle 不是 logger handle。
        if(symbol=="TraceEvent")result=6;
        else {last_error_=6;result=symbol=="GetTraceLoggerHandle"?UINT64_MAX:0;}
    } else if(symbol=="EventRegister") {
        if(!arg(3))result=87;
        else {
            memory.check(GuestAddress{arg(3)},8,Access::Write);
            if(!arg(0))result=87;
            else if(runtime_.event_providers.size()>=1024)result=8;
            else {
                EventProviderRegistration registration;memory.read(GuestAddress{arg(0)},registration.id);
                registration.callback=arg(1);registration.context=arg(2);
                const auto handle=runtime_.next_event_provider++;
                runtime_.event_providers.emplace(handle,std::move(registration));
                memory.store(GuestAddress{arg(3)},64,handle);
            }
            if(result)memory.store(GuestAddress{arg(3)},64,0);
        }
    } else if(symbol=="EventUnregister" || symbol=="UnregisterTraceGuids") {
        const auto found=runtime_.event_providers.find(arg(0));
        if(!arg(0) && symbol=="EventUnregister")result=0;
        else if(found==runtime_.event_providers.end() || found->second.classic!=(symbol=="UnregisterTraceGuids"))result=6;
        else runtime_.event_providers.erase(found);
    } else if(symbol=="EventEnabled" || symbol=="EventProviderEnabled") {
        // 沒有啟用的 ETW session；註冊 provider 不等於開啟事件收集。
        result=0;
    } else if(symbol=="EventWrite" || symbol=="EventWriteTransfer") {
        // Windows 允許零 handle 與未啟用 provider；此時不用解參照 payload。
        if(arg(0) && (!runtime_.event_providers.contains(arg(0)) || runtime_.event_providers.at(arg(0)).classic))result=6;
    } else if(symbol=="EventSetInformation") {
        const auto found=runtime_.event_providers.find(arg(0));
        if(found==runtime_.event_providers.end() || found->second.classic)result=6;
        else if(static_cast<std::uint32_t>(arg(1))==3) {
            if(!arg(2) || static_cast<std::uint32_t>(arg(3))!=1)result=87;
            else found->second.use_descriptor_type=memory.load(GuestAddress{arg(2)},8)!=0;
        } else if(static_cast<std::uint32_t>(arg(1))!=2)result=50;
        else {
            const auto size=static_cast<std::uint32_t>(arg(3));
            if(!arg(2) || size<3 || size>65535 || memory.load(GuestAddress{arg(2)},16)!=size)result=87;
            else {
                std::vector<std::uint8_t> traits(size);memory.read(GuestAddress{arg(2)},traits);
                const auto end=std::find(traits.begin()+2,traits.end(),0);
                if(end==traits.end())result=87;
                else {
                    auto cursor=static_cast<std::size_t>(end-traits.begin())+1;
                    while(cursor<traits.size()) {
                        if(traits.size()-cursor<3){result=87;break;}
                        const auto extent=std::size_t{traits[cursor]}|(std::size_t{traits[cursor+1]}<<8);
                        if(extent<3 || extent>traits.size()-cursor){result=87;break;}cursor+=extent;
                    }
                    if(!result){found->second.traits=std::move(traits);found->second.use_descriptor_type=true;}
                }
            }
        }
    } else if(symbol=="GetProcessGroupAffinity") {
        const auto handle=arg(0);const auto found=runtime_.process_thread_handles.find(handle);
        if(handle!=(x86_?UINT32_MAX:UINT64_MAX) && (found==runtime_.process_thread_handles.end() || found->second.first!=(x86_?UINT32_MAX:UINT64_MAX)))last_error_=6;
        else if(!arg(1))last_error_=87;
        else {const auto capacity=memory.load(GuestAddress{arg(1)},16);memory.check(GuestAddress{arg(1)},2,Access::Write);
            if(!capacity){memory.store(GuestAddress{arg(1)},16,1);last_error_=122;}
            else if(!arg(2))last_error_=87;
            else {memory.store(GuestAddress{arg(2)},16,0);memory.store(GuestAddress{arg(1)},16,1);result=1;}
        }
    } else if(symbol=="SetThreadGroupAffinity" || symbol=="GetThreadGroupAffinity") {
        const bool setting=symbol.starts_with("Set");const auto id=thread_id(arg(0));
        const unsigned unit=x86_?4:8;const std::uint8_t width=x86_?32:64;
        if(!id)last_error_=6;
        else if(!(thread_access(arg(0))&(setting?0x420U:0x840U)))last_error_=5;
        else if(!arg(1))last_error_=87;
        else {
            auto& thread=threads_.items.at(id);const auto previous=thread.affinity&processor_mask;auto selected=previous;bool valid=true;
            if(setting){memory.check(GuestAddress{arg(1)},unit+8,Access::Read);selected=memory.load(GuestAddress{arg(1)},width);
                valid=selected && !(selected&~runtime_.process_affinity) && memory.load(GuestAddress{arg(1)+unit},64)==0;}
            if(!valid)last_error_=87;
            else {
                const auto output=arg(setting?2:1);
                if(output){memory.write(GuestAddress{output},std::vector<std::uint8_t>(unit+8));memory.store(GuestAddress{output},width,previous);}
                if(setting){thread.affinity=selected;if(id==threads_.current && !(selected&(UINT64_C(1)<<state.processor_id)))state.processor_id=static_cast<std::uint32_t>(std::countr_zero(selected));}
                result=1;
            }
        }
    } else if(symbol=="GetActiveProcessorGroupCount" || symbol=="GetMaximumProcessorGroupCount")result=1;
    else if(symbol=="GetActiveProcessorCount" || symbol=="GetMaximumProcessorCount"){
        const auto group=static_cast<std::uint16_t>(arg(0));if(group==0 || group==65535)result=runtime_.processor_count;else last_error_=87;
    } else if(symbol=="GetCurrentProcessorNumberEx") {
        memory.store(GuestAddress{arg(0)},32,state.processor_id<<16);
    } else if(symbol=="GetCurrentProcessorNumber")result=state.processor_id;
    else if(symbol=="GetPhysicallyInstalledSystemMemory"){
        if(!arg(0))last_error_=87;else if(!runtime_.network.external_io)last_error_=50;
        else {const auto snapshot=native_system_performance();if(snapshot.error)last_error_=snapshot.error;
            else {memory.store(GuestAddress{arg(0)},64,snapshot.pages[3]*snapshot.pages[9]/1024);result=1;}}
    }
    else if(symbol=="GetNumaHighestNodeNumber") {
        if(!arg(0))last_error_=87;
        else {memory.store(GuestAddress{arg(0)},32,0);result=1;}
    }
    else if(symbol=="GetNumaNodeProcessorMask" || symbol=="GetNumaNodeProcessorMaskEx" ||
            symbol=="GetNumaAvailableMemoryNode" || symbol=="GetNumaAvailableMemoryNodeEx") {
        const auto node=symbol.ends_with("Ex")?static_cast<std::uint16_t>(arg(0)):static_cast<std::uint8_t>(arg(0));
        if(node || !arg(1))last_error_=87;
        else {
            if(symbol=="GetNumaNodeProcessorMaskEx") {
                memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(x86_?12:16));
                memory.store(GuestAddress{arg(1)},x86_?32:64,processor_mask);
                result=1;
            } else if(symbol=="GetNumaNodeProcessorMask"){memory.store(GuestAddress{arg(1)},64,processor_mask);result=1;}
            else if(!runtime_.network.external_io)last_error_=50;
            else {const auto snapshot=native_system_performance();if(snapshot.error)last_error_=snapshot.error;
                else {memory.store(GuestAddress{arg(1)},64,snapshot.pages[4]*snapshot.pages[9]);result=1;}}
        }
    }
    else if(symbol=="GetSystemCpuSetInformation") {
        const auto process=arg(3);const bool self=process==(x86_?UINT32_MAX:UINT64_MAX);
        if(!arg(2) || static_cast<std::uint32_t>(arg(4)))last_error_=87;
        else if(process && !self)last_error_=6;
        else {
            const auto size=runtime_.processor_count*32U;memory.store(GuestAddress{arg(2)},32,size);
            if(!arg(0) || static_cast<std::uint32_t>(arg(1))<size)last_error_=122;
            else {
                memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(size));
                for(unsigned cpu=0;cpu<runtime_.processor_count;++cpu){const auto entry=arg(0)+cpu*32;
                    memory.store(GuestAddress{entry},32,32);memory.store(GuestAddress{entry+8},32,0x100+cpu);
                    memory.store(GuestAddress{entry+14},8,cpu);memory.store(GuestAddress{entry+15},8,cpu);
                }result=1;
            }
        }
    }
    else if (symbol=="GetLogicalProcessorInformationEx" || symbol=="GetLogicalProcessorInformation") {
        const bool legacy=symbol=="GetLogicalProcessorInformation";
        const auto relation=legacy?0xffffU:static_cast<std::uint32_t>(arg(0));
        const auto output=arg(legacy?0:1),length=arg(legacy?1:2);
        if(!length || (relation>7 && relation!=0xffff))last_error_=87;
        else {
            const auto bytes=processor_relationships(relation,runtime_.processor_count,x86_,legacy);
            const auto capacity=memory.load(GuestAddress{length},32);
            memory.check(GuestAddress{length},4,Access::Write);
            if(!output || capacity<bytes.size()) {memory.store(GuestAddress{length},32,bytes.size());last_error_=122;}
            else {
                memory.check(GuestAddress{output},bytes.size(),Access::Write);
                memory.write(GuestAddress{output},bytes);memory.store(GuestAddress{length},32,bytes.size());result=1;
            }
        }
    } else if(symbol=="DsRoleGetPrimaryDomainInformation"){
        if(!arg(2))result=87;
        else {
            memory.store(GuestAddress{arg(2)},x86_?32:64,0);
            auto server=arg(0)?environment_key(read_utf16(memory,GuestAddress{arg(0)})):std::u16string{};
            if(server.starts_with(u"\\\\"))server.erase(0,2);
            const auto level=static_cast<std::uint32_t>(arg(1));
            if(!server.empty() && server!=virtual_domain.computer)result=53;
            else if(level<1 || level>3)result=87;
            else {
                const auto structure=level==1?(x86_?36U:48U):level==2?8U:4U;
                const auto size=structure+(level==1?2U:0U);const auto buffer=allocate_heap(memory,size);
                if(!buffer)result=8;
                else {
                    memory.write(GuestAddress{buffer},std::vector<std::uint8_t>(size));
                    if(level==1){
                        memory.store(GuestAddress{buffer},32,virtual_domain.role);
                        memory.store(GuestAddress{buffer+4},32,virtual_domain.flags);
                        memory.store(GuestAddress{buffer+8},x86_?32:64,buffer+structure);
                    }else memory.store(GuestAddress{buffer},32,level==2?virtual_domain.upgrade:virtual_domain.operation);
                    memory.store(GuestAddress{arg(2)},x86_?32:64,buffer);
                }
            }
        }
    } else if(symbol=="DsRoleFreeMemory"){
        if(arg(0))(void)free_heap(memory,arg(0),state);
    } else if(symbol=="NetGetAadJoinInformation"){
        // 虛擬系統未加入 Azure AD／Entra ID：依契約回傳 S_OK 並把輸出設為 NULL。
        if(!arg(1))result=0x80070057U;
        else {memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);memory.store(GuestAddress{arg(1)},x86_?32:64,0);result=0;}
    } else if(symbol=="NetFreeAadJoinInformation"){
        if(arg(0) && allocations_.contains(arg(0)))(void)free_heap(memory,arg(0),state);
    } else if(symbol=="NetGetJoinInformation"){
        auto server=arg(0)?environment_key(read_utf16(memory,GuestAddress{arg(0)})):std::u16string{};if(server.starts_with(u"\\\\"))server.erase(0,2);
        if(!arg(1) || !arg(2))result=87;
        else if(!server.empty() && server!=virtual_domain.computer)result=53;
        else {memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);memory.check(GuestAddress{arg(2)},4,Access::Write);
            memory.store(GuestAddress{arg(1)},x86_?32:64,0);memory.store(GuestAddress{arg(2)},32,virtual_domain.join_status);} // 與 GetComputerNameEx 的獨立、未加入網域狀態一致。
    } else if(symbol=="NetUserGetInfo"){
        // 與 GetUserName／GetUserNameEx／IsUserAnAdmin 共用同一個未提升、本機虛擬帳號。
        // 目前沒有網域帳號資料庫；不從宿主洩漏使用者或管理權限。
        const auto level=static_cast<std::uint32_t>(arg(2));const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
        if(!arg(3) || !arg(1))result=87;
        else {
            memory.check(GuestAddress{arg(3)},unit,Access::Write);memory.store(GuestAddress{arg(3)},bits,0);
            auto server=arg(0)?environment_key(read_utf16(memory,GuestAddress{arg(0)})):std::u16string{};if(server.starts_with(u"\\\\"))server.erase(0,2);
            const auto user=environment_key(read_utf16(memory,GuestAddress{arg(1)}));
            if(!server.empty() && server!=virtual_domain.computer)result=53;
            else if(user!=u"USER")result=2221;
            else if(level!=0 && level!=1 && level!=10)result=(level==2 || level==3 || level==4 || level==11 || level==20 || level==23 || level==24)?50:124;
            else {
                const unsigned structure=level==0?unit:level==10?unit*4:x86_?32:56;
                auto bytes=std::vector<std::uint8_t>(structure);std::vector<std::pair<unsigned,unsigned>> pointers;
                const auto put=[&](unsigned at,std::uint64_t value,unsigned size){for(unsigned j=0;j<size;++j)bytes[at+j]=static_cast<std::uint8_t>(value>>(j*8));};
                const auto field_text=[&](unsigned at,std::u16string_view value){const auto offset=static_cast<unsigned>(bytes.size());const auto string=utf16_bytes(value);bytes.insert(bytes.end(),string.begin(),string.end());pointers.emplace_back(at,offset);};
                field_text(0,u"user");
                if(level==1){put(unit*2+4,1,4);field_text(x86_?16:24,u"");field_text(x86_?20:32,u"");put(x86_?24:40,0x200,4);field_text(x86_?28:48,u"");}
                else if(level==10){field_text(unit,u"");field_text(unit*2,u"");field_text(unit*3,u"");}
                const auto buffer=allocate_heap(memory,bytes.size());if(!buffer)result=8;
                else {try{for(const auto& [field,offset]:pointers)put(field,buffer+offset,unit);memory.write(GuestAddress{buffer},bytes);memory.store(GuestAddress{arg(3)},bits,buffer);result=0;}catch(...){free_heap(memory,buffer,state);throw;}}
            }
        }
    } else if(symbol=="NetApiBufferFree")result=!arg(0) || free_heap(memory,arg(0),state)?0:87;
    else if (symbol.starts_with("GetComputerName")) {
        const bool extended=symbol.starts_with("GetComputerNameEx");
        const auto type=extended?static_cast<std::uint32_t>(arg(0)):0U;
        const auto buffer=arg(extended?1:0),size=arg(extended?2:1);
        if(type>7 || !size)last_error_=87;
        else {
            // 客體目前是獨立的虛擬電腦，沒有設定 DNS 網域或叢集名稱。
            const auto bytes=encode(type==2 || type==6?u"":u"X64ARM");
            const auto required=bytes.size()/(wide?2U:1U);
            const auto capacity=memory.load(GuestAddress{size},32);
            memory.check(GuestAddress{size},4,Access::Write);
            if(!buffer && capacity)last_error_=87;
            else if(capacity<required) {memory.store(GuestAddress{size},32,required);last_error_=extended?234:111;}
            else {memory.write(GuestAddress{buffer},bytes);memory.store(GuestAddress{size},32,required-1);result=1;}
        }
    } else if (symbol.starts_with("SetCurrentDirectory") || symbol=="_chdir" || symbol=="_wchdir") {
        const bool crt=symbol.front()=='_';std::uint32_t directory_error=0;
        if(!arg(0))directory_error=87;
        else try {
            const auto input=text(0);
            if(input.empty())directory_error=123;
            else {
                const auto directory=normalized_windows_path(input,runtime_.current_directory);
                const auto error=filesystem_.set_current_directory(utf16_to_utf8(directory));
                if(error)directory_error=error;else {runtime_.current_directory=directory;result=1;}
            }
        } catch(const std::invalid_argument&) {directory_error=123;}
        if(crt){
            result=directory_error?UINT32_MAX:0;
            if(directory_error)memory.store(GuestAddress{runtime_.errno_cell},32,directory_error==87?22:directory_error==5?13:2);
            if(!arg(0)){
                if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}
                return 0xc0000409U;
            }
        }else if(directory_error)last_error_=directory_error;
    } else if(symbol=="_dupenv_s" || symbol=="_wdupenv_s" || symbol=="getenv_s" || symbol=="_wgetenv_s"){
        const bool duplicate=symbol.find("dupenv")!=symbol.npos;const auto bits=static_cast<std::uint8_t>(x86_?32:64);
        const auto count=arg(duplicate?1:0),destination=duplicate?0:arg(1);const auto name_index=duplicate?2U:3U;
        if(count)memory.store(GuestAddress{count},bits,0);
        if(duplicate && arg(0))memory.store(GuestAddress{arg(0)},bits,0);
        if(!duplicate && destination && arg(2))memory.store(GuestAddress{destination},wide?16:8,0);
        if(!arg(0) || !arg(name_index) || (!duplicate && !destination && arg(2))){
            result=22;memory.store(GuestAddress{runtime_.errno_cell},32,result);
            if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}
        }else if(const auto value=crt_environment_value(name_index)){
            std::vector<std::uint8_t> bytes;
            if(wide)bytes=utf16_bytes(read_utf16(memory,GuestAddress{value},32767));
            else {const auto string=read_ansi(memory,value,32767U*4U);bytes.assign(string.begin(),string.end());bytes.push_back(0);}
            const auto required=bytes.size()/(wide?2U:1U);
            if(duplicate){
                const auto pointer=allocate_heap(memory,bytes.size());
                if(!pointer)result=12;
                else {memory.write(GuestAddress{pointer},bytes);memory.store(GuestAddress{arg(0)},bits,pointer);if(count)memory.store(GuestAddress{count},bits,required);}
            }else{
                memory.store(GuestAddress{count},bits,required);
                if(destination || arg(2)){if(arg(2)<required)result=34;else memory.write(GuestAddress{destination},bytes);}
            }
            if(result)memory.store(GuestAddress{runtime_.errno_cell},32,result);
        }
    } else if (symbol=="getenv" || symbol=="_wgetenv") {
        result=crt_environment_value(0);
    } else if (symbol.starts_with("GetEnvironmentVariable")) {
        const auto found=runtime_.environment.find(environment_key(text(0)));
        if (found==runtime_.environment.end()) last_error_=203;
        else {
            const auto bytes=encode(found->second.second);const auto count=bytes.size()/(wide?2U:1U);result=count;
            if (static_cast<std::uint32_t>(arg(2))>=count) {memory.write(GuestAddress{arg(1)},bytes);--result;}
        }
    } else if (symbol.starts_with("SetEnvironmentVariable") || symbol.find("putenv")!=std::string_view::npos) {
        const bool crt=symbol.front()=='_',secure=symbol.ends_with("_s");auto name=text(0);std::u16string value;
        bool remove=false,valid=true;
        if (crt && !secure) {
            const auto equal=name.find(u'=');if (equal==std::u16string::npos) valid=false;
            else {value=name.substr(equal+1);name.resize(equal);remove=value.empty();}
        } else {remove=!arg(1);if (!remove) value=text(1);if (crt) {valid=arg(1)!=0;remove=value.empty();}}
        const bool drive_variable=symbol.starts_with("SetEnvironmentVariable") && name.size()==3 && name[0]==u'=' && ((name[1]>=u'A' && name[1]<=u'Z') || (name[1]>=u'a' && name[1]<=u'z')) && name[2]==u':';
        valid=valid && !name.empty() && (name.find(u'=')==std::u16string::npos || drive_variable) && name.size()+value.size()+2<=32767;
        if (!valid) {if (crt) {memory.store(GuestAddress{runtime_.errno_cell},32,22);result=secure?22:UINT32_MAX;}else last_error_=87;}
        else {
            const auto key=environment_key(name);
            if (remove) runtime_.environment.erase(key);else runtime_.environment[key]={name,value};
            if (crt) {
                if (remove) runtime_.crt_environment.erase(key);else runtime_.crt_environment[key]={name,value};
                rebuild_crt_environment(memory,state);
            } else result=1;
        }
    } else if (symbol.starts_with("GetEnvironmentStrings")) {
        std::vector<std::uint8_t> bytes;
        for (const auto& [key,entry] : runtime_.environment) {(void)key;const auto encoded=encode(entry.first+u"="+entry.second);bytes.insert(bytes.end(),encoded.begin(),encoded.end());}
        const auto terminator=wide?2U:1U;if (bytes.empty()) bytes.resize(terminator);bytes.resize(bytes.size()+terminator);
        result=allocate_heap(memory,bytes.size());if (!result) last_error_=8;
        else {memory.write(GuestAddress{result},bytes);runtime_.environment_blocks.emplace(result,wide);}
    } else if (symbol.starts_with("FreeEnvironmentStrings")) {
        const auto found=runtime_.environment_blocks.find(arg(0));
        if (found==runtime_.environment_blocks.end() || found->second!=wide) last_error_=87;
        else {free_heap(memory,arg(0),state);runtime_.environment_blocks.erase(found);result=1;}
    } else if (symbol.starts_with("ExpandEnvironmentStrings")) {
        const auto source=text(0);std::u16string expanded;
        for (std::size_t at=0;at<source.size();) {
            if (source[at]!=u'%') {expanded.push_back(source[at++]);continue;}
            const auto end=source.find(u'%',at+1);if (end==std::u16string::npos) {expanded.append(source.substr(at));break;}
            const auto found=runtime_.environment.find(environment_key(source.substr(at+1,end-at-1)));
            expanded.append(found==runtime_.environment.end()?source.substr(at,end-at+1):found->second.second);at=end+1;
            if (expanded.size()>32767) throw HostCallFault("展開的環境變數字串超過上限");
        }
        const auto bytes=encode(expanded);result=bytes.size()/(wide?2U:1U);
        if (static_cast<std::uint32_t>(arg(2))>=result) memory.write(GuestAddress{arg(1)},bytes);
    }
    arg.return_to(state,result,symbol=="GetTraceLoggerHandle");return std::nullopt;
}
}
