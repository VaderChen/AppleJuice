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
        {"advapi32.dll","SetServiceStatus",2},{"advapi32.dll","NotifyServiceStatusChangeW",3,0,0,2},{"advapi32.dll","QueryServiceObjectSecurity",5},{"advapi32.dll","SetServiceObjectSecurity",3},
        {"advapi32.dll","EnumServiceGroupA",9},{"advapi32.dll","EnumServiceGroupW",9},
        {"advapi32.dll","EnumServicesStatusA",8},{"advapi32.dll","EnumServicesStatusW",8},
        {"advapi32.dll","EnumServicesStatusExA",10},{"advapi32.dll","EnumServicesStatusExW",10},
        // 目錄查詢：顯示名稱／鍵名互查、相依服務（遞移，依停止順序）、相容用的資料庫鎖定。
        {"advapi32.dll","GetServiceDisplayNameA",4},{"advapi32.dll","GetServiceDisplayNameW",4},{"advapi32.dll","GetServiceKeyNameA",4},{"advapi32.dll","GetServiceKeyNameW",4},
        {"advapi32.dll","EnumDependentServicesA",6,0,0,0x2},{"advapi32.dll","EnumDependentServicesW",6,0,0,0x2},
        {"advapi32.dll","LockServiceDatabase",1},{"advapi32.dll","UnlockServiceDatabase",1},
        {"advapi32.dll","QueryServiceLockStatusA",4,0,0,0x4},{"advapi32.dll","QueryServiceLockStatusW",4,0,0,0x4},
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
// ENUM_SERVICE_STATUS(_PROCESS)：名稱與顯示名稱指標、SERVICE_STATUS（Ex 為 SERVICE_STATUS_PROCESS），字串接在陣列之後。
struct EncodedServices {std::vector<std::uint8_t> bytes;std::vector<std::pair<std::size_t,std::uint64_t>> pointers;};
std::vector<std::uint8_t> service_text(std::u16string_view value,bool wide){
    if(wide)return utf16_bytes(value);const auto text=utf16_to_utf8(value);std::vector<std::uint8_t> part(text.begin(),text.end());part.push_back(0);return part;
}
std::uint64_t service_entry_size(const ServiceCatalogEntry& entry,bool ex,bool wide,bool x86){
    return (ex?(x86?44U:56U):(x86?36U:48U))+service_text(entry.name,wide).size()+service_text(entry.display,wide).size();
}
EncodedServices encode_services(std::span<const ServiceCatalogEntry* const> entries,bool ex,bool wide,bool x86){
    const unsigned unit=x86?4U:8U,entry_size=ex?(x86?44U:56U):(x86?36U:48U),status_words=ex?9U:7U;
    EncodedServices out;out.bytes.resize(entries.size()*entry_size);
    const auto put=[&](std::size_t at,std::uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)out.bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    for(std::size_t n=0;n<entries.size();++n){const auto& item=*entries[n];const auto at=n*entry_size;
        for(unsigned field=0;field<2;++field){const auto part=service_text(field?item.display:item.name,wide);out.pointers.emplace_back(at+field*unit,out.bytes.size());out.bytes.insert(out.bytes.end(),part.begin(),part.end());}
        for(unsigned i=0;i<status_words;++i)put(at+2*unit+i*4,item.words[i],4);}
    return out;
}
void write_services(GuestMemory& memory,std::uint64_t buffer,EncodedServices& encoded,bool x86){
    if(encoded.bytes.empty())return;const unsigned unit=x86?4U:8U;
    memory.check(GuestAddress{buffer},encoded.bytes.size(),Access::Write);
    for(const auto& [at,offset]:encoded.pointers)for(unsigned i=0;i<unit;++i)encoded.bytes[at+i]=static_cast<std::uint8_t>((buffer+offset)>>(i*8));
    memory.write(GuestAddress{buffer},encoded.bytes);
}
std::uint32_t mapped_access(std::uint32_t access,bool manager) {
    const auto read=manager?0x20015U:0x2008dU,write=manager?0x20022U:0x20002U,execute=manager?0x20009U:0x20170U,all=manager?0xf003fU:0xf01ffU;
    auto result=access&0x0fffffffU;if(access&0x80000000U)result|=read;if(access&0x40000000U)result|=write;if(access&0x20000000U)result|=execute;if(access&0x10000000U)result|=all;
    if(result&0x02000000U)result=(result&~0x02000000U)|read;return result;
}
}
std::vector<ServiceCatalogEntry> WindowsServices::service_catalog_entries(){
    // 原生唯讀服務目錄與客體 SCM 服務合併，依名稱（不分大小寫）排序；客體服務的 DependOnService 不含 SC_GROUP_IDENTIFIER 群組項目。
    std::vector<ServiceCatalogEntry> entries;
    if(const auto catalog=native_service_catalog())for(const auto& job:catalog->jobs)entries.push_back({job.label,job.label,{},service_status_words(job),{}});
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
    if(!transaction.error)for(const auto& [key,values]:runtime_.registry.keys){
        if(!key.starts_with(guest_service::root+u"\\") || key.find(u'\\',guest_service::root.size()+1)!=key.npos || !guest_service::installed(values))continue;
        try{
            ServiceCatalogEntry entry;entry.words=guest_service::status(values);entry.name=guest_service::text(values,u"name",key.substr(guest_service::root.size()+1));
            entry.display=guest_service::text(values,u"displayname",entry.name);entry.group=guest_service::text(values,u"group");
            const auto depends=guest_service::bytes(values,u"dependonservice");std::u16string item;
            for(std::size_t i=0;i+1<depends.size();i+=2){const auto c=static_cast<char16_t>(depends[i]|(unsigned{depends[i+1]}<<8));
                if(c){item.push_back(c);continue;}if(!item.empty() && item.front()!=u'+')entry.dependencies.push_back(item);item.clear();}
            entries.push_back(std::move(entry));
        }catch(const SecurityDescriptorError&){}
    }
    std::sort(entries.begin(),entries.end(),[](const ServiceCatalogEntry& a,const ServiceCatalogEntry& b){return folded(a.name)<folded(b.name);});
    return entries;
}
std::optional<std::uint32_t> WindowsServices::invoke_service_catalog(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');std::uint64_t result=0;
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    const auto text=[&](std::uint64_t pointer){return wide?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer,32767));};
    const auto find=[&](std::uint64_t handle){auto found=service_control_handles_.find(handle);return found!=service_control_handles_.end() && found->second.owner==process_id()?found:service_control_handles_.end();};
    try {
        if(name.starts_with("GetServiceDisplayName") || name.starts_with("GetServiceKeyName")){
            // (hSCManager, 查詢名稱, 輸出, lpcchBuffer)：成功時 *lpcchBuffer 為不含 NUL 的長度；空間不足回報需要的長度（A 版為位元組）。
            const bool display=name.starts_with("GetServiceDisplayName");const auto manager=find(arg(0));
            if(manager==service_control_handles_.end() || !manager->second.manager)fail(6);
            else if(!arg(1) || !arg(3))fail(87);
            else if(!runtime_.network.external_io)fail(50);
            else {
                const auto wanted=folded(text(arg(1)));const auto entries=service_catalog_entries();
                const auto entry=std::find_if(entries.begin(),entries.end(),[&](const auto& item){return folded(display?item.name:item.display)==wanted;});
                if(entry==entries.end())fail(1060);
                else {
                    const auto value=service_text(display?entry->display:entry->name,wide);const auto length=static_cast<std::uint32_t>(value.size()/(wide?2U:1U)-1);
                    const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));memory.check(GuestAddress{arg(3)},4,Access::Write);
                    if(!arg(2) || capacity<=length){memory.store(GuestAddress{arg(3)},32,length);fail(122);}
                    else {memory.write(GuestAddress{arg(2)},value);memory.store(GuestAddress{arg(3)},32,length);result=1;}
                }
            }
        }else if(name.starts_with("EnumDependentServices")){
            // 直接與間接相依於此服務者，依停止順序（較深的相依者在前）；dwServiceState 為 ACTIVE／INACTIVE／ALL。
            const auto handle=find(arg(0));const auto wanted=static_cast<std::uint32_t>(arg(1)),capacity=static_cast<std::uint32_t>(arg(3));
            if(handle==service_control_handles_.end() || handle->second.manager || handle->second.status_handle)fail(6);
            else if(!(handle->second.access&8U))fail(5);
            else if(wanted<1 || wanted>3 || !arg(4) || !arg(5) || (capacity && !arg(2)))fail(87);
            else {
                const auto entries=service_catalog_entries();std::vector<const ServiceCatalogEntry*> order;std::vector<std::u16string> visiting;
                const auto visit=[&](auto& self,const std::u16string& target)->void{
                    visiting.push_back(folded(target));
                    for(const auto& item:entries){
                        if(std::none_of(item.dependencies.begin(),item.dependencies.end(),[&](const auto& d){return folded(d)==folded(target);}))continue;
                        if(std::find(order.begin(),order.end(),&item)!=order.end() || std::find(visiting.begin(),visiting.end(),folded(item.name))!=visiting.end())continue;
                        self(self,item.name);order.push_back(&item);
                    }
                    visiting.pop_back();
                };
                visit(visit,handle->second.label);
                std::erase_if(order,[&](const auto* item){return !(wanted==3 || (wanted==1)==(item->words[1]!=1));});
                std::uint64_t needed=0;for(const auto* item:order)needed+=service_entry_size(*item,false,wide,x86_);
                memory.check(GuestAddress{arg(4)},4,Access::Write);memory.check(GuestAddress{arg(5)},4,Access::Write);
                if(needed>capacity){memory.store(GuestAddress{arg(4)},32,needed);memory.store(GuestAddress{arg(5)},32,0);fail(234);}
                else {auto encoded=encode_services(order,false,wide,x86_);write_services(memory,arg(2),encoded,x86_);
                    memory.store(GuestAddress{arg(4)},32,0);memory.store(GuestAddress{arg(5)},32,order.size());result=1;}
            }
        }else if(name=="LockServiceDatabase"){
            const auto manager=find(arg(0));
            if(manager==service_control_handles_.end() || !manager->second.manager)fail(6);
            else if(!(manager->second.access&8U))fail(5);
            else if(service_database_lock_)fail(1055); // ERROR_SERVICE_DATABASE_LOCKED
            else {
                initialize_tokens();const auto account=security::resolve_account(runtime_.token_objects.at(effective_token_id()).user.sid,runtime_.token_objects.at(1).user.sid);
                const auto owner=account?utf8_to_utf16((account->domain.empty()?std::string(".")+"\\":std::string(account->domain)+"\\")+std::string(account->name)):std::u16string{};
                service_database_lock_=ServiceDatabaseLock{runtime_.allocate_wait_handle(),runtime_.clock_ticks,owner};result=service_database_lock_->handle;
            }
        }else if(name=="UnlockServiceDatabase"){
            if(!service_database_lock_ || service_database_lock_->handle!=arg(0))fail(1071); // ERROR_INVALID_SERVICE_LOCK
            else {service_database_lock_.reset();result=1;}
        }else if(name.starts_with("QueryServiceLockStatus")){
            // QUERY_SERVICE_LOCK_STATUS {fIsLocked, lpLockOwner, dwLockDuration（秒）}，持有者字串接在結構之後。
            const auto manager=find(arg(0));const auto capacity=static_cast<std::uint32_t>(arg(2));
            if(manager==service_control_handles_.end() || !manager->second.manager)fail(6);
            else if(!(manager->second.access&0x10U))fail(5);
            else if(!arg(3) || (capacity && !arg(1)))fail(87);
            else {
                const unsigned unit=x86_?4U:8U,header=x86_?12U:24U;const auto owner=service_text(service_database_lock_?service_database_lock_->owner:u"",wide);
                const auto needed=static_cast<std::uint32_t>(header+owner.size());memory.check(GuestAddress{arg(3)},4,Access::Write);
                if(capacity<needed){memory.store(GuestAddress{arg(3)},32,needed);fail(122);}
                else {
                    std::vector<std::uint8_t> bytes(header);const auto put=[&](unsigned at,std::uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                    const auto seconds=service_database_lock_ && runtime_.clock_ticks>service_database_lock_->since?(runtime_.clock_ticks-service_database_lock_->since)/10000000U:0U;
                    put(0,service_database_lock_?1:0,4);put(unit,arg(1)+header,unit);put(unit*2,seconds,4);bytes.insert(bytes.end(),owner.begin(),owner.end());
                    memory.write(GuestAddress{arg(1)},bytes);memory.store(GuestAddress{arg(3)},32,needed);result=1;
                }
            }
        }
    }catch(const std::bad_alloc&){fail(8);}
    arg.return_to(state,result);return std::nullopt;
}
// NotifyServiceStatusChange 的狀態檢查：服務 handle 依目前狀態（1～7 對應遮罩 1<<(狀態−1)）與刪除標記（DELETE_PENDING 0x200）；
// SCM handle 比對登錄時的客體服務名稱快照（CREATED 0x80／DELETED 0x100，名稱清單為 MULTI_SZ，刪除者前綴「/」）。
// 符合時寫入 SERVICE_NOTIFY_2W（dwNotificationStatus、SERVICE_STATUS_PROCESS、dwNotificationTriggered、pszServiceNames），再以 APC 交給登錄的執行緒；一次性。
bool WindowsServices::deliver_service_notification(GuestMemory& memory,ServiceNotification& notification){
    const auto handle=service_control_handles_.find(notification.handle);if(handle==service_control_handles_.end())return true;
    const unsigned unit=x86_?4:8;const std::uint64_t status_at=notification.buffer+(x86_?12U:24U),words_at=status_at+4,triggered_at=words_at+36,names_at=x86_?56U:72U;
    std::array<std::uint32_t,9> words{};std::uint32_t triggered=0,error=0;std::vector<std::u16string> created,deleted;
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return false;
    if(handle->second.manager){
        std::vector<std::u16string> current;
        for(const auto& [key,values]:runtime_.registry.keys)if(key.starts_with(guest_service::root+u"\\") && key.find(u'\\',guest_service::root.size()+1)==key.npos && guest_service::installed(values))
            current.push_back(guest_service::text(values,u"name",key.substr(guest_service::root.size()+1)));
        const auto folded=[](std::u16string text){for(auto& c:text)c=unicode_case_unit(c,false);return text;};
        const auto contains=[&](const std::vector<std::u16string>& list,const std::u16string& name){return std::any_of(list.begin(),list.end(),[&](const auto& item){return folded(item)==folded(name);});};
        if(notification.mask&0x80U)for(const auto& name:current)if(!contains(notification.names,name))created.push_back(name);
        if(notification.mask&0x100U)for(const auto& name:notification.names)if(!contains(current,name))deleted.push_back(name);
        if(created.empty() && deleted.empty())return false;triggered=(created.empty()?0U:0x80U)|(deleted.empty()?0U:0x100U);
    }else if(handle->second.guest){
        const auto found=runtime_.registry.keys.find(guest_service::path(handle->second.label));
        if(found==runtime_.registry.keys.end() || guest_service::number(found->second,u"deleteflag")){if(!(notification.mask&0x200U))return false;triggered=0x200U;error=0;
            if(found!=runtime_.registry.keys.end())words=guest_service::status(found->second);}
        else {words=guest_service::status(found->second);const auto bit=words[1]>=1 && words[1]<=7?1U<<(words[1]-1):0U;if(!(notification.mask&bit))return false;triggered=bit;}
    }else {
        const auto status=native_service_status(handle->second.label,handle->second.system);if(!status)return false;
        words=service_status_words(*status);const auto bit=words[1]>=1 && words[1]<=7?1U<<(words[1]-1):0U;if(!(notification.mask&bit))return false;triggered=bit;
    }
    memory.store(GuestAddress{status_at},32,error);for(unsigned i=0;i<9;++i)memory.store(GuestAddress{words_at+4*i},32,words[i]);memory.store(GuestAddress{triggered_at},32,triggered);
    std::uint64_t names=0;
    if(!created.empty() || !deleted.empty()){
        std::vector<std::uint8_t> bytes;const auto add=[&](const std::u16string& text){const auto encoded=utf16_bytes(text);bytes.insert(bytes.end(),encoded.begin(),encoded.end());};
        for(const auto& name:created)add(name);for(const auto& name:deleted)add(u"/"+name);bytes.push_back(0);bytes.push_back(0);
        names=allocate_heap(memory,bytes.size());if(names)memory.write(GuestAddress{names},bytes);
    }
    memory.store(GuestAddress{notification.buffer+names_at},static_cast<std::uint8_t>(unit*8),names);
    if(const auto thread=threads_.items.find(notification.thread);thread!=threads_.items.end() && thread->second.status!=ThreadStatus::Exited && !thread->second.terminating && thread->second.apcs.size()<1024)
        thread->second.apcs.push_back({notification.callback,{notification.buffer}});
    return true;
}
void WindowsServices::poll_service_notifications(GuestMemory& memory){
    if(runtime_.clock_ticks<service_notification_poll_)return;service_notification_poll_=runtime_.clock_ticks+200000;
    // 狀態讀取失敗（登錄內容損毀等）時丟棄該筆通知，不中斷排程輪詢。
    for(auto it=service_notifications_.begin();it!=service_notifications_.end();){
        bool done=true;try{done=deliver_service_notification(memory,*it);}catch(const SecurityDescriptorError&){}catch(const MemoryFault&){}
        if(done)it=service_notifications_.erase(it);else ++it;}
}
std::optional<std::uint32_t> WindowsServices::invoke_service_control(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(service.symbol=="NotifyServiceStatusChangeW"){
        // (hService, dwNotifyMask, pNotifyBuffer)：回傳 Win32 錯誤碼。SERVICE_NOTIFY 的 dwVersion 須為 SERVICE_NOTIFY_STATUS_CHANGE（2）。
        const WindowsCall call(state,memory,service);const auto finish=[&](std::uint32_t error){call.return_to(state,error);return std::optional<std::uint32_t>{};};
        const auto handle=service_control_handles_.find(call(0));if(handle==service_control_handles_.end() || handle->second.owner!=process_id())return finish(6);
        const auto mask=static_cast<std::uint32_t>(call(1));const auto buffer=call(2);const bool manager=handle->second.manager;
        if(!buffer || !mask || (manager?(mask&~0x180U):(mask&~0x27fU)))return finish(87);
        if(!manager && !(handle->second.access&4))return finish(5);if(manager && !(handle->second.access&1))return finish(5);
        try{
            memory.check(GuestAddress{buffer},x86_?60:80,Access::Write);if(memory.load(GuestAddress{buffer},32)!=2)return finish(87);
            const auto callback=memory.load(GuestAddress{buffer+(x86_?4U:8U)},x86_?32:64);if(!callback)return finish(87);
            ServiceNotification notification;notification.handle=call(0);notification.buffer=buffer;notification.callback=callback;notification.thread=threads_.current;notification.mask=mask;
            {RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return finish(transaction.error);
                if(manager){for(const auto& [key,values]:runtime_.registry.keys)if(key.starts_with(guest_service::root+u"\\") && key.find(u'\\',guest_service::root.size()+1)==key.npos && guest_service::installed(values))
                    notification.names.push_back(guest_service::text(values,u"name",key.substr(guest_service::root.size()+1)));}
                else if(handle->second.guest){const auto found=runtime_.registry.keys.find(guest_service::path(handle->second.label));
                    if(found==runtime_.registry.keys.end() || guest_service::number(found->second,u"deleteflag"))return finish(1072);}}
            std::erase_if(service_notifications_,[&](const auto& item){return item.handle==call(0);});
            if(!deliver_service_notification(memory,notification)){if(service_notifications_.size()>=1024)return finish(8);service_notifications_.push_back(std::move(notification));}
        }catch(const MemoryFault&){return finish(998);}catch(const SecurityDescriptorError& error){return finish(error.code);}
        return finish(0);
    }
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');std::uint64_t result=0;
    if(name.starts_with("GetServiceDisplayName") || name.starts_with("GetServiceKeyName") || name.starts_with("EnumDependentServices") ||
       name=="LockServiceDatabase" || name=="UnlockServiceDatabase" || name.starts_with("QueryServiceLockStatus"))return invoke_service_catalog(state,memory,service);
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
        }else if(name.starts_with("EnumServicesStatus") || name.starts_with("EnumServiceGroup")) {
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
                std::optional<std::u16string> group;
                if(ex || name.starts_with("EnumServiceGroup")){
                    const auto group_name=arg(base+8);
                    if(group_name)group=folded(text(group_name));
                }
                std::vector<ServiceCatalogEntry> entries;
                for(auto& item:service_catalog_entries())
                    if((item.words[0]&type) && (wanted==3 || (wanted==1)==(item.words[1]!=1)) && (!group || *group==folded(item.group)))entries.push_back(std::move(item));
                const std::uint64_t start=resume?memory.load(GuestAddress{resume},32):0;
                std::size_t index=static_cast<std::size_t>(std::min<std::uint64_t>(start,entries.size()));std::uint64_t used=0;std::vector<const ServiceCatalogEntry*> fitted;
                for(;index<entries.size();++index){const auto size=service_entry_size(entries[index],ex,wide,x86_);if(used+size>capacity)break;used+=size;fitted.push_back(&entries[index]);}
                std::uint64_t remaining=0;for(auto i=index;i<entries.size();++i)remaining+=service_entry_size(entries[i],ex,wide,x86_);
                auto encoded=encode_services(fitted,ex,wide,x86_);
                memory.check(GuestAddress{needed},4,Access::Write);memory.check(GuestAddress{returned},4,Access::Write);
                write_services(memory,buffer,encoded,x86_);
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
