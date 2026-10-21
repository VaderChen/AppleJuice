#include "x64arm/windows_services.hpp"
#include "x64arm/windows_service_control.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include "guest_service_store.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
std::span<const WindowsServiceDescriptor> service_control_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"advapi32.dll","OpenSCManagerA",3},{"advapi32.dll","OpenSCManagerW",3},
        {"advapi32.dll","OpenServiceA",3},{"advapi32.dll","OpenServiceW",3},
        {"advapi32.dll","CloseServiceHandle",1},{"advapi32.dll","QueryServiceStatus",2},
        {"advapi32.dll","QueryServiceStatusEx",5},{"advapi32.dll","ControlService",3},
        {"advapi32.dll","QueryServiceConfigA",4},{"advapi32.dll","QueryServiceConfigW",4},
        {"advapi32.dll","StartServiceA",3},{"advapi32.dll","StartServiceW",3},
        {"advapi32.dll","CreateServiceA",13},{"advapi32.dll","CreateServiceW",13},
        {"advapi32.dll","DeleteService",1},{"advapi32.dll","ChangeServiceConfigA",11},{"advapi32.dll","ChangeServiceConfigW",11},
        {"advapi32.dll","ChangeServiceConfig2A",3},{"advapi32.dll","ChangeServiceConfig2W",3},
        {"advapi32.dll","QueryServiceConfig2A",5},{"advapi32.dll","QueryServiceConfig2W",5},
        {"advapi32.dll","StartServiceCtrlDispatcherA",1},{"advapi32.dll","StartServiceCtrlDispatcherW",1},
        {"advapi32.dll","RegisterServiceCtrlHandlerA",2},{"advapi32.dll","RegisterServiceCtrlHandlerW",2},
        {"advapi32.dll","RegisterServiceCtrlHandlerExA",3},{"advapi32.dll","RegisterServiceCtrlHandlerExW",3},
        {"advapi32.dll","SetServiceStatus",2},{"advapi32.dll","QueryServiceObjectSecurity",5},{"advapi32.dll","SetServiceObjectSecurity",3},
        {"advapi32.dll","EnumServicesStatusA",8},{"advapi32.dll","EnumServicesStatusW",8},
        {"advapi32.dll","EnumServicesStatusExA",10},{"advapi32.dll","EnumServicesStatusExW",10},
        {"internal-service","ServiceMainDispatch",1}
    };return table;
}
std::array<std::uint32_t,9> service_status_words(const NativeServiceStatus& service) {
    const bool running=service.pid!=0;
    // 原生唯讀服務不宣告可停止／暫停。先前 incarnation 的退出碼不套用於正在執行的服務。
    return {0x10,running?4U:1U,0,!running && service.exit_status?1066U:0U,
        running?0U:std::bit_cast<std::uint32_t>(service.exit_status),0,0,service.pid,0};
}
namespace {
std::u16string folded(std::u16string_view text){std::u16string value(text);for(auto& c:value)c=unicode_case_unit(c,true);return value;}
std::uint32_t mapped_access(std::uint32_t access,bool manager) {
    const auto read=manager?0x20015U:0x2008dU,write=manager?0x20022U:0x20002U,execute=manager?0x20009U:0x20170U,all=manager?0xf003fU:0xf01ffU;
    auto result=access&0x0fffffffU;if(access&0x80000000U)result|=read;if(access&0x40000000U)result|=write;if(access&0x20000000U)result|=execute;if(access&0x10000000U)result|=all;
    if(result&0x02000000U)result=(result&~0x02000000U)|read;return result;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_service_control(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');std::uint64_t result=0;
    const auto existing=service_control_handles_.find(arg(0));
    bool guest=(existing!=service_control_handles_.end() && existing->second.guest) ||
        name.starts_with("CreateService") || name=="DeleteService" || name.starts_with("ChangeServiceConfig") || name.starts_with("QueryServiceConfig2") ||
        name.starts_with("StartServiceCtrlDispatcher") || name.starts_with("RegisterServiceCtrlHandler") || name=="SetServiceStatus" ||
        name=="QueryServiceObjectSecurity" || name=="SetServiceObjectSecurity" || name=="ServiceMainDispatch";
    if(name.starts_with("OpenService") && arg(1) && runtime_.network.external_io){
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
        if(!transaction.error){const auto label=wide?read_utf16(memory,GuestAddress{arg(1)}):utf8_to_utf16_lossy(read_ansi(memory,arg(1),32767));const auto found=runtime_.registry.keys.find(guest_service::path(label));guest=found!=runtime_.registry.keys.end() && guest_service::installed(found->second);}
    }
    if(guest)return invoke_guest_service(state,memory,service);
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    const auto text=[&](std::uint64_t pointer){return wide?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer,32767));};
    const auto find=[&](std::uint64_t handle){auto found=service_control_handles_.find(handle);return found!=service_control_handles_.end() && found->second.owner==process_id()?found:service_control_handles_.end();};
    const auto readable_token=[&](){initialize_tokens();const auto id=effective_token_id();const auto& token=runtime_.token_objects.at(id);return (token.type!=2 || token.level!=0) && token_user_sid_string(id)!="S-1-5-7";};
    try {
        if(name=="CloseServiceHandle") {
            const auto found=find(arg(0));if(found==service_control_handles_.end())fail(6);else {service_control_handles_.erase(found);result=1;}
        }else if(name.starts_with("OpenSCManager")) {
            auto machine=arg(0)?folded(text(arg(0))):std::u16string{};if(machine.starts_with(u"\\\\"))machine.erase(0,2);
            const auto database=arg(1)?folded(text(arg(1))):u"SERVICESACTIVE";
            const auto access=mapped_access(static_cast<std::uint32_t>(arg(2)),true)|1U;
            if(!machine.empty() && machine!=u"." && machine!=u"LOCALHOST" && machine!=u"X64ARM")fail(1722);
            else if(database!=u"SERVICESACTIVE")fail(1065);
            else if(!readable_token())fail(5);
            else if(access&~((filesystem_.writable() && runtime_.token_objects.at(effective_token_id()).integrity>=0x3000U &&
                std::any_of(runtime_.token_objects.at(effective_token_id()).groups.begin(),runtime_.token_objects.at(effective_token_id()).groups.end(),[](const auto& group){return group.sid==security::account_sid(*security::find_account("Administrators")) && (group.attributes&0x14U)==4U;}))?0xf003fU:0x20015U))fail(5);
            else if(!runtime_.network.external_io || !native_service_catalog())fail(50);
            else if(service_control_handles_.size()>=8192)fail(8);
            else {const auto handle=runtime_.allocate_wait_handle();service_control_handles_.emplace(handle,ServiceControlHandle{{},access,process_id(),true,false});result=handle;}
        }else if(name.starts_with("OpenService")) {
            const auto manager=find(arg(0));
            if(manager==service_control_handles_.end() || !manager->second.manager)fail(6);
            else if(!(manager->second.access&1) || !readable_token())fail(5);
            else if(!arg(1))fail(123);
            else {
                const auto requested=text(arg(1));auto lookup=folded(requested);std::optional<bool> domain;
                if(lookup.starts_with(u"SYSTEM:")){domain=true;lookup.erase(0,7);}else if(lookup.starts_with(u"USER:")){domain=false;lookup.erase(0,5);}
                const auto access=mapped_access(static_cast<std::uint32_t>(arg(2)),false);
                if(requested.empty() || requested.size()>256 || lookup.empty() || requested.find_first_of(u"/\\")!=requested.npos)fail(123);
                else if(!runtime_.network.external_io)fail(50);
                else if(const auto catalog=native_service_catalog()) {
                    const auto job=std::find_if(catalog->jobs.begin(),catalog->jobs.end(),[&](const auto& entry){return (!domain || *domain==entry.system) && folded(entry.label)==lookup;});
                    if(domain && !catalog->domains[*domain?1:0])fail(5);
                    else if(job==catalog->jobs.end())fail(1060);
                    else if(access&~0x2008dU)fail(5);
                    else if(service_control_handles_.size()>=8192)fail(8);
                    else {const auto handle=runtime_.allocate_wait_handle();service_control_handles_.emplace(handle,ServiceControlHandle{job->label,access,process_id(),false,job->system});result=handle;}
                }else fail(50);
            }
        }else if(name.starts_with("StartService")) {
            const auto handle=find(arg(0));const auto count=static_cast<std::uint32_t>(arg(1));
            if(handle==service_control_handles_.end() || handle->second.manager)fail(6);
            else if(!(handle->second.access&0x10U))fail(5); // 唯讀原生服務目錄不授予 SERVICE_START。
            else if(count>1024 || (count && !arg(2)))fail(87);
            else fail(5);
        }else if(name=="ControlService") {
            const auto handle=find(arg(0));const auto control=static_cast<std::uint32_t>(arg(1));
            const auto access=control==1?0x20U:control==4?0x80U:(control==2 || control==3 || (control>=6 && control<=10))?0x40U:(control>=128 && control<=255)?0x100U:0U;
            if(handle==service_control_handles_.end() || handle->second.manager)fail(6);
            else if(!access)fail(87);
            else if(!(handle->second.access&access))fail(5);
            else if(!arg(2))fail(87);
            else if(!runtime_.network.external_io)fail(50);
            else if(const auto status=native_service_status(handle->second.label,handle->second.system)){
                const auto words=service_status_words(*status);memory.check(GuestAddress{arg(2)},28,Access::Write);for(unsigned i=0;i<7;++i)memory.store(GuestAddress{arg(2)+i*4},32,words[i]);
                if(!status->pid)fail(1062);else if(control!=4)fail(1052);else result=1;
            }else fail(1060);
        }else if(name.starts_with("QueryServiceConfig")) {
            const auto handle=find(arg(0));const auto capacity=static_cast<std::uint32_t>(arg(2));
            if(handle==service_control_handles_.end() || handle->second.manager)fail(6);
            else if(!(handle->second.access&1))fail(5);
            else if(!arg(3) || capacity>8192)fail(87);
            else if(!runtime_.network.external_io)fail(50);
            else if(const auto config=native_service_status(handle->second.label,handle->second.system)){
                if(!config->configured)fail(13);
                else {
                    const unsigned unit=x86_?4U:8U,header=x86_?36U:64U;std::vector<std::uint8_t> bytes(header);std::vector<std::pair<unsigned,std::uint64_t>> pointers;
                    const auto put=[&](unsigned at,std::uint64_t value,unsigned count=4){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                    const auto add=[&](unsigned at,std::u16string_view value,bool multisz=false){const auto offset=bytes.size();std::vector<std::uint8_t> part;
                        if(wide)part=utf16_bytes(value);else {const auto text=utf16_to_utf8(value);part.assign(text.begin(),text.end());part.push_back(0);}
                        if(multisz)part.resize(part.size()+(wide?2U:1U));bytes.insert(bytes.end(),part.begin(),part.end());pointers.emplace_back(at,offset);
                    };
                    put(0,0x10);put(4,config->automatic?2:3);put(8,1);
                    add(x86_?12U:16U,config->command);add(x86_?16U:24U,u"");add(x86_?24U:40U,u"",true);add(x86_?28U:48U,config->account);add(x86_?32U:56U,config->label);
                    memory.check(GuestAddress{arg(3)},4,Access::Write);
                    if(bytes.size()>8192)fail(13);
                    else if(!arg(1) || capacity<bytes.size()){memory.store(GuestAddress{arg(3)},32,bytes.size());fail(122);}
                    else {memory.check(GuestAddress{arg(1)},bytes.size(),Access::Write);for(const auto& [at,offset]:pointers)put(at,arg(1)+offset,unit);memory.write(GuestAddress{arg(1)},bytes);result=1;}
                }
            }else fail(1060);
        }else if(name.starts_with("EnumServicesStatus")) {
            // 原生唯讀服務目錄與客體 SCM 服務合併，依名稱（不分大小寫）排序；resume handle 為下一筆索引。
            const bool ex=name.starts_with("EnumServicesStatusEx");const unsigned base=ex?1U:0U;
            const auto manager=find(arg(0));const auto type=static_cast<std::uint32_t>(arg(base+1)),wanted=static_cast<std::uint32_t>(arg(base+2));
            const auto buffer=arg(base+3);const auto capacity=static_cast<std::uint32_t>(arg(base+4));
            const auto needed=arg(base+5),returned=arg(base+6),resume=arg(base+7);
            if(manager==service_control_handles_.end() || !manager->second.manager)fail(6);
            else if(!(manager->second.access&4))fail(5);
            else if(ex && static_cast<std::uint32_t>(arg(1))!=0)fail(124);
            else if(!needed || !returned || !type || (type&~0x3fU) || wanted<1 || wanted>3 || capacity>0x1000000 || (capacity && !buffer))fail(87);
            else {
                struct Entry {std::u16string name,display;std::array<std::uint32_t,9> words;};std::vector<Entry> entries;
                std::optional<std::u16string> group;if(ex && arg(9))group=text(arg(9));
                const auto accept=[&](const std::array<std::uint32_t,9>& words,std::u16string_view member){
                    return (words[0]&type) && (wanted==3 || (wanted==1)==(words[1]!=1)) && (!group || folded(*group)==folded(member));};
                if(const auto catalog=native_service_catalog())for(const auto& job:catalog->jobs){
                    const auto words=service_status_words(job);if(accept(words,u""))entries.push_back({job.label,job.label,words});}
                RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
                if(!transaction.error)for(const auto& [key,values]:runtime_.registry.keys){
                    if(!key.starts_with(guest_service::root+u"\\") || key.find(u'\\',guest_service::root.size()+1)!=key.npos || !guest_service::installed(values))continue;
                    try{const auto words=guest_service::status(values);const auto label=guest_service::text(values,u"name",key.substr(guest_service::root.size()+1));
                        if(accept(words,guest_service::text(values,u"group")))entries.push_back({label,guest_service::text(values,u"displayname",label),words});}
                    catch(const SecurityDescriptorError&){}
                }
                std::sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b){return folded(a.name)<folded(b.name);});
                const unsigned unit=x86_?4U:8U,entry_size=ex?(x86_?44U:56U):(x86_?36U:48U),status_words=ex?9U:7U;
                const auto encode=[&](std::u16string_view value){std::vector<std::uint8_t> part;if(wide)part=utf16_bytes(value);else {const auto s=utf16_to_utf8(value);part.assign(s.begin(),s.end());part.push_back(0);}return part;};
                const std::uint64_t start=resume?memory.load(GuestAddress{resume},32):0;
                std::size_t index=static_cast<std::size_t>(std::min<std::uint64_t>(start,entries.size()));std::uint64_t used=0;std::vector<std::size_t> fitted;
                for(;index<entries.size();++index){const auto size=entry_size+encode(entries[index].name).size()+encode(entries[index].display).size();if(used+size>capacity)break;used+=size;fitted.push_back(index);}
                std::uint64_t remaining=0;for(auto i=index;i<entries.size();++i)remaining+=entry_size+encode(entries[i].name).size()+encode(entries[i].display).size();
                std::vector<std::uint8_t> bytes(fitted.size()*entry_size);std::vector<std::pair<std::size_t,std::uint64_t>> pointers;
                const auto put=[&](std::size_t at,std::uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                for(std::size_t n=0;n<fitted.size();++n){const auto& item=entries[fitted[n]];const auto at=n*entry_size;
                    for(unsigned field=0;field<2;++field){const auto part=encode(field?item.display:item.name);pointers.emplace_back(at+field*unit,bytes.size());bytes.insert(bytes.end(),part.begin(),part.end());}
                    for(unsigned i=0;i<status_words;++i)put(at+2*unit+i*4,item.words[i],4);}
                memory.check(GuestAddress{needed},4,Access::Write);memory.check(GuestAddress{returned},4,Access::Write);
                if(!bytes.empty()){memory.check(GuestAddress{buffer},bytes.size(),Access::Write);for(const auto& [at,offset]:pointers)put(at,buffer+offset,unit);memory.write(GuestAddress{buffer},bytes);}
                memory.store(GuestAddress{returned},32,fitted.size());memory.store(GuestAddress{needed},32,remaining);
                if(resume)memory.store(GuestAddress{resume},32,index<entries.size()?index:0);
                if(index<entries.size())fail(234);else result=1;
            }
        }else if(name=="QueryServiceStatus" || name=="QueryServiceStatusEx") {
            const bool ex=name.ends_with("Ex");const auto handle=find(arg(0));const auto output=arg(ex?2:1);
            if(handle==service_control_handles_.end() || handle->second.manager)fail(6);
            else if(!(handle->second.access&4))fail(5);
            else if(ex && static_cast<std::uint32_t>(arg(1))!=0)fail(124);
            else if(ex && (!arg(4) || static_cast<std::uint32_t>(arg(3))>8192))fail(87);
            else if(ex && static_cast<std::uint32_t>(arg(3))<36){memory.store(GuestAddress{arg(4)},32,36);fail(122);}
            else if(!output)fail(87);
            else if(!runtime_.network.external_io)fail(50);
            else {
                const auto status=native_service_status(handle->second.label,handle->second.system);
                if(!status){const auto catalog=native_service_catalog();fail(!catalog?50U:!catalog->domains[handle->second.system?1:0]?5U:1060U);}
                else {
                    const auto words=service_status_words(*status);const auto count=ex?9U:7U;
                    memory.check(GuestAddress{output},count*4,Access::Write);if(ex)memory.check(GuestAddress{arg(4)},4,Access::Write);
                    for(unsigned i=0;i<count;++i)memory.store(GuestAddress{output+i*4},32,words[i]);if(ex)memory.store(GuestAddress{arg(4)},32,36);result=1;
                }
            }
        }else return {};
    }catch(const std::bad_alloc&){fail(8);}
    arg.return_to(state,result);return {};
}
#include "windows_guest_service.inc"
}
