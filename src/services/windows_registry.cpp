#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <chrono>
#include "x64arm/registry_store.hpp"
#include "processor_registry.hpp"
#include "x64arm/network_registry.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "registry_hive.hpp"

namespace x64arm {
void WindowsServices::set_registry_file(std::filesystem::path path) {
    registry_file_=std::move(path);
    if(registry_file_.empty() || !runtime_.network.external_io) return;
    // 初始化與其他程序的讀寫共用 RegistryTransaction 的鎖及原子提交。
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_);
    if(transaction.error) return; // API 呼叫仍以同一後端回報實際 I/O 錯誤。
    if(network_registry_present(runtime_.registry)) return;
    const auto before=runtime_.registry;
    try {
        if(initialize_network_registry(runtime_.registry,*runtime_.network.configuration))
            if(transaction.commit()) runtime_.registry=before;
    } catch(...) {runtime_.registry=before;throw;}
}
std::span<const WindowsServiceDescriptor> registry_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"advapi32.dll","RegDisablePredefinedCache",0},{"advapi32.dll","RegDisablePredefinedCacheEx",0},{"advapi32.dll","RegOpenCurrentUser",2,0,0,0x1},
        {"advapi32.dll","RegGetKeySecurity",4,0,0,0x2},{"advapi32.dll","RegSetKeySecurity",3,0,0,0x2},
        {"advapi32.dll","RegOpenKeyExA",5,0,0,0xc},{"advapi32.dll","RegOpenKeyExW",5,0,0,0xc},
        {"advapi32.dll","RegOpenKeyA",3},{"advapi32.dll","RegOpenKeyW",3},
        {"advapi32.dll","RegCreateKeyExA",9,0,0,0x34},{"advapi32.dll","RegCreateKeyExW",9,0,0,0x34},
        {"advapi32.dll","RegCreateKeyA",3},{"advapi32.dll","RegCreateKeyW",3},
        {"advapi32.dll","RegGetValueA",7,0,0,0x8},{"advapi32.dll","RegGetValueW",7,0,0,0x8},{"advapi32.dll","RegQueryValueA",4},{"advapi32.dll","RegQueryValueW",4},
        {"advapi32.dll","RegQueryInfoKeyA",12},{"advapi32.dll","RegQueryInfoKeyW",12},{"advapi32.dll","RegQueryValueExA",6},{"advapi32.dll","RegQueryValueExW",6},
        {"advapi32.dll","RegSetValueExA",6,0,0,0x2c},{"advapi32.dll","RegSetValueExW",6,0,0,0x2c},
        {"advapi32.dll","RegSetKeyValueA",6,0,0,0x28},{"advapi32.dll","RegSetKeyValueW",6,0,0,0x28},
        {"advapi32.dll","RegDeleteKeyValueA",3},{"advapi32.dll","RegDeleteKeyValueW",3},
        {"advapi32.dll","RegDeleteValueA",2},{"advapi32.dll","RegDeleteValueW",2},
        {"advapi32.dll","RegEnumValueA",8,0,0,0x2},{"advapi32.dll","RegEnumValueW",8,0,0,0x2},
        {"advapi32.dll","RegEnumKeyA",4,0,0,0xa},{"advapi32.dll","RegEnumKeyW",4,0,0,0xa},
        {"advapi32.dll","RegEnumKeyExA",8,0,0,0x2},{"advapi32.dll","RegEnumKeyExW",8,0,0,0x2},
        {"advapi32.dll","RegDeleteKeyA",2},{"advapi32.dll","RegDeleteKeyW",2},
        {"advapi32.dll","RegDeleteKeyExA",4,0,0,0xc},{"advapi32.dll","RegDeleteKeyExW",4,0,0,0xc},
        {"advapi32.dll","RegDeleteTreeA",2},{"advapi32.dll","RegDeleteTreeW",2},
        {"shlwapi.dll","SHGetValueA",6},{"shlwapi.dll","SHGetValueW",6},
        {"shlwapi.dll","SHDeleteKeyA",2},{"shlwapi.dll","SHDeleteKeyW",2},
        {"advapi32.dll","RegNotifyChangeKeyValue",5,0,0,0x16},{"advapi32.dll","RegCloseKey",1},
        {"advapi32.dll","RegFlushKey",1},
        {"advapi32.dll","RegSaveKeyA",3},{"advapi32.dll","RegSaveKeyW",3},
        {"advapi32.dll","RegSaveKeyExA",4,0,0,8},{"advapi32.dll","RegSaveKeyExW",4,0,0,8},
        {"advapi32.dll","RegLoadKeyA",3},{"advapi32.dll","RegLoadKeyW",3},
        {"advapi32.dll","RegUnLoadKeyA",2},{"advapi32.dll","RegUnLoadKeyW",2},
        {"advapi32.dll","RegConnectRegistryA",3},{"advapi32.dll","RegConnectRegistryW",3},
        {"advapi32.dll","RegDisableReflectionKey",1},{"advapi32.dll","RegEnableReflectionKey",1},{"advapi32.dll","RegQueryReflectionKey",2},
        {"ntdll.dll","NtOpenKey",3,0,0,0x2},{"ntdll.dll","NtOpenKeyEx",4,0,0,0xa},{"ntdll.dll","NtCreateKey",7,0,0,0x2a},
        {"ntdll.dll","NtQueryKey",5,0,0,0xa},{"ntdll.dll","NtEnumerateKey",6,0,0,0x16},
        {"ntdll.dll","NtQueryValueKey",6,0,0,0x14},{"ntdll.dll","NtEnumerateValueKey",6,0,0,0x16},{"ntdll.dll","NtSetValueKey",6,0,0,0x2c},
        {"ntdll.dll","NtDeleteValueKey",2},{"ntdll.dll","NtDeleteKey",1},{"ntdll.dll","NtFlushKey",1},{"ntdll.dll","NtClose",1}
    };return table;
}
#include "windows_registry_hive.inc"
void WindowsServices::poll_registry_notifications(bool reload,std::u16string_view mutation,std::uint32_t filter) {
    if(registry_notifications_.empty())return;
    auto& registry=runtime_.registry;
    bool compare_snapshots=!reload || registry_file_.empty();
    if(reload && !registry_file_.empty()) {
        const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if(now>=registry_poll_time_+100){RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return;registry_poll_time_=now;compare_snapshots=true;}
    }
    for(auto it=registry_notifications_.begin();it!=registry_notifications_.end();) {
        const auto& watch=it->second;
        const auto thread=threads_.items.find(watch.thread);
        const bool root=watch.key>=0x80000000U && watch.key<=0x80000006U;
        bool changed=(!root && !registry.handles.contains(watch.key)) || !registry.keys.contains(watch.path) ||
            (!(watch.filter&0x10000000U) && (thread==threads_.items.end() || thread->second.status==ThreadStatus::Exited));
        // 背景排程每個時間片都會輪詢；只有比對變更路徑或快照時才需要組出子鍵前綴。
        if(!changed && ((filter&watch.filter) || compare_snapshots)) {
            const auto prefix=watch.path+u"\\";
            const auto within=[&](std::u16string_view path){return path==watch.path || (watch.subtree && path.starts_with(prefix));};
            if((filter&watch.filter) && within(mutation))changed=true;
            const auto relevant=[&](std::u16string_view path){return within(path) || ((watch.filter&1) && path.starts_with(prefix) && path.find(u'\\',prefix.size())==std::u16string_view::npos);};
            // API 內的交易／變更立即比對；宿主登錄檔尚未重載時不重掃相同快照。
            // 關閉 handle 與執行緒退出仍在每次輪詢檢查。
            if(!changed && compare_snapshots) {
                for(const auto& [path,values]:registry.keys)if(relevant(path)) {
                    const auto before=watch.snapshot.find(path);
                    if(((watch.filter&1) && before==watch.snapshot.end()) || ((watch.filter&4) && within(path) && before!=watch.snapshot.end() && values!=before->second)){changed=true;break;}
                }
                if(watch.filter&1)for(const auto& [path,values]:watch.snapshot){(void)values;if(!registry.keys.contains(path)){changed=true;break;}}
                // 描述元可由繼承規則惰性建立；其他程序提交後重載會清空本地快取。
                // 比對有效描述元，不能將「尚未放入快取」誤認為安全性變更。
                if(watch.filter&8)for(const auto& [path,descriptor]:watch.security_snapshot)
                    if(!registry.keys.contains(path) || registry_security_descriptor(path)!=descriptor){changed=true;break;}
            }
        }
        if(!changed){++it;continue;}
        if(watch.event){const auto event=runtime_.wait_objects.find(watch.event);if(event!=runtime_.wait_objects.end())event->second.set_signal(true);}
        it=registry_notifications_.erase(it);
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_registry(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(service.symbol.starts_with("RegSaveKey") || service.symbol.starts_with("RegLoadKey") || service.symbol.starts_with("RegUnLoadKey") || service.symbol.starts_with("RegConnectRegistry"))return invoke_registry_hive(state,memory,service);
    if(service.symbol=="RegGetKeySecurity" || service.symbol=="RegSetKeySecurity")return invoke_registry_security(state,memory,service);
    if(service.family=="ntdll.dll")return invoke_nt_registry(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;auto& registry=runtime_.registry;
    const bool set_key_value=symbol.starts_with("RegSetKeyValue"),delete_key_value=symbol.starts_with("RegDeleteKeyValue");
    const bool key_value=set_key_value || delete_key_value;
    try {
    const bool get_value=symbol.starts_with("RegGetValue"),legacy_query=symbol=="RegQueryValueA" || symbol=="RegQueryValueW";
    const auto query_flags=get_value?static_cast<std::uint32_t>(arg(3)):0U;
    const auto zero_capacity=get_value && (query_flags&0x20000000U) && arg(5) && arg(6)?memory.load(GuestAddress{arg(6)},32):0;
    const auto zero_failure=[&](std::uint32_t result){if(result && zero_capacity){if(zero_capacity>memory.limit())throw HostCallFault("登錄值輸出超過記憶體上限");memory.write(GuestAddress{arg(5)},std::vector<std::uint8_t>(static_cast<std::size_t>(zero_capacity)));}};
    if(symbol=="RegDisablePredefinedCache" || symbol=="RegDisablePredefinedCacheEx"){
        runtime_.registry_user_cache_disabled=true;runtime_.registry_predefined_cache.erase(0x80000001U);
        if(symbol.ends_with("Ex")){runtime_.registry_cache_disabled=true;runtime_.registry_predefined_cache.clear();}
        arg.return_to(state,0);return std::nullopt;
    }
    if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
    if(symbol=="RegNotifyChangeKeyValue" && !arg(4)){
        auto& thread=threads_.items.at(threads_.current);
        if(thread.wait.kind==ThreadWaitKind::RegistryNotify && thread.wait.signaled){thread.wait={};arg.return_to(state,0);return std::nullopt;}
    }
    const bool readonly=symbol.starts_with("RegOpen") || symbol.starts_with("RegQuery") || symbol.starts_with("RegGet") ||
        symbol.starts_with("RegEnum") || symbol=="RegCloseKey" || symbol=="RegNotifyChangeKeyValue";
    auto transaction=std::make_unique<RegistryTransaction>(registry,registry_file_,&registry_store_cache_,readonly);
    if(transaction->error){zero_failure(transaction->error);arg.return_to(state,transaction->error);return std::nullopt;}
    refresh_processor_registry(registry,memory.processor_count(),guest_processor_information(memory));
    initialize_registry_views(registry);
    poll_registry_notifications(false);
    std::vector<std::pair<std::u16string,std::uint32_t>> mutations;
    bool changed=false;
    auto before_transaction=(!key_value && registry_file_.empty()) || readonly?RegistryState{}:registry;
    struct KeyValueRollback {
        RegistryState& target;RegistryState& before;bool& changed;bool enabled;bool accepted{};
        ~KeyValueRollback(){if(enabled && changed && !accepted)target=std::move(before);}
    } rollback{registry,before_transaction,changed,key_value};
    const auto done=[&](std::uint32_t result)->std::optional<std::uint32_t>{
        if(!result && changed){result=commit_registry_transaction(*transaction);if(result && !registry_file_.empty())registry=before_transaction;}
        if(!result){for(const auto& [path,filter]:mutations)poll_registry_notifications(false,path,filter);poll_registry_notifications(false);}
        rollback.accepted=!result;zero_failure(result);arg.return_to(state,result);return std::nullopt;
    };
    if(get_value){
        if((query_flags&~0x3003ffffU) || (query_flags&0x30000U)==0x30000U || ((query_flags&0xffffU)!=0xffffU && (query_flags&0xff80U)))return done(87);
        if((query_flags&0xffffU)!=0xffffU && (query_flags&4U) && !(query_flags&0x10000000U))return done(87);
    }
    initialize_tokens();
    const auto user_hive=[&](std::uint64_t id){auto sid=utf8_to_utf16(token_user_sid_string(id));if(!sid.empty())sid[0]=u's';return std::u16string(1,3)+u"\\"+sid;};
    const auto primary_hive=user_hive(1);
    // 保留既有 HKCU 的儲存路徑，HKEY_USERS\SID 透過同一解析器對應，避免搬動使用者資料。
    const auto canonical=[&](const std::u16string& path){return path==primary_hive || path.starts_with(primary_hive+u"\\")?std::u16string(1,1)+path.substr(primary_hive.size()):path;};
    registry.keys.try_emplace(std::u16string(1,1));registry.keys.try_emplace(std::u16string(1,3));registry.keys.try_emplace(primary_hive);
    registry.display_names[primary_hive]=utf8_to_utf16(token_user_sid_string(1));
    const auto current_user_path=[&]{const auto path=user_hive(effective_token_id());if(path==primary_hive)return std::u16string(1,1);if(registry.keys.contains(path))return path;const auto fallback=std::u16string(1,3)+u"\\.default";registry.keys.try_emplace(fallback);return fallback;};
    if(symbol=="RegOpenCurrentUser"){
        const auto access=static_cast<std::uint32_t>(arg(0));
        if(!arg(1) || (access&0x300U)==0x300U)return done(87);
        if(access&~0xf30f033fU)return done(50);
        if(registry.handles.size()>=4096)return done(8);
        memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);const auto token=registry.next_handle++;
        const auto path=current_user_path();const auto granted=registry_security_access(path,access);
        registry.handles.emplace(token,RegistryHandle{path,granted,registry_view(access,x86_),path});memory.store(GuestAddress{arg(1)},x86_?32:64,token);return done(0);
    }
    const auto root=static_cast<std::uint32_t>(arg(0));
    const bool root_encoding=arg(0)==root || arg(0)==(UINT64_C(0xffffffff00000000)|root);
    const bool predefined=root_encoding && root>=0x80000000U && root<=0x80000006U && root!=0x80000004U;
    RegistryHandle handle;
    if(predefined) {
        const bool cache=!runtime_.registry_cache_disabled && !(root==0x80000001U && runtime_.registry_user_cache_disabled);
        const auto found=runtime_.registry_predefined_cache.find(root);
        handle.path=cache && found!=runtime_.registry_predefined_cache.end()?found->second:root==0x80000001U?current_user_path():std::u16string(1,static_cast<char16_t>(root&255));
        if(cache)runtime_.registry_predefined_cache[root]=handle.path;handle.access=0xf003f;handle.view=registry_view(0,x86_);handle.logical_path=handle.path;registry.keys.try_emplace(handle.path);
    }
    else {
        const auto found=registry.handles.find(arg(0));if(found==registry.handles.end())return done(6);handle=found->second;
    }
    if(symbol=="RegCloseKey") {if(!predefined){registry.handles.erase(arg(0));runtime_.handle_flags.erase(arg(0));poll_registry_notifications(false);}return done(0);}
    if(!registry.keys.contains(handle.path))return done(1018);
    // Windows 7 起使用共享鍵及 WOW64 重導向，不再對任何鍵進行 reflection。
    // 仍驗證 handle／存活鍵及輸出記憶體；啟用或停用不改變共享鍵語義。
    if(symbol=="RegDisableReflectionKey" || symbol=="RegEnableReflectionKey" || symbol=="RegQueryReflectionKey"){
        if(symbol=="RegQueryReflectionKey"){if(!arg(1))return done(87);memory.store(GuestAddress{arg(1)},32,1);}
        return done(0);
    }
    const auto modified=[&](const std::u16string& path){changed=true;registry.modified_times[path]=runtime_.system_filetime();};
    const bool wide=symbol.ends_with("W");
    const auto name=[&](std::uint64_t pointer) {
        auto value=pointer?(wide?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16(read_ansi(memory,pointer,32768))):std::u16string{};
        for(auto& c:value)c=unicode_case_unit(c,false);
        return value;
    };
    // 相對開啟及刪除檢查目標鍵；不可先要求 hive 根具有 MAXIMUM_ALLOWED。
    // 僅直接操作 predefined 鍵時，才解析目前身分對該鍵持有的能力。
    const bool relative_open=symbol.starts_with("RegOpenKey") || symbol.starts_with("RegCreateKey") || (symbol.starts_with("RegDeleteKey") && !delete_key_value) || (set_key_value && !name(arg(1)).empty());
    const bool relative_query=(symbol.starts_with("SHGetValue") || get_value || legacy_query || delete_key_value) && !name(arg(1)).empty();
    if(predefined && !relative_open && !relative_query)handle.access=registry_security_access(handle.path,0x02000000U);
    if(symbol=="RegFlushKey"){if(!(handle.access&1U))return done(5);if(const auto error=flush_registry_hives())return done(error);return done(transaction->flush());}
    if(key_value && !predefined && !(handle.access&2))return done(5);
    if(set_key_value){const auto size=static_cast<std::uint32_t>(arg(5));if(size>output_limit)return done(8);if(size && !arg(4))return done(87);if(size)memory.check(GuestAddress{arg(4)},size,Access::Read);}
    if(symbol.starts_with("RegQueryInfoKey")){
        if(!(handle.access&1))return done(5);
        if(arg(3) || (arg(1) && !arg(2)))return done(87);
        const auto capacity=arg(2)?memory.load(GuestAddress{arg(2)},32):0;
        const auto class_item=registry.classes.find(handle.path);const auto class_name=class_item==registry.classes.end()?std::u16string{}:class_item->second;
        const auto class_text=wide?std::string{}:utf16_to_utf8(class_name);const auto class_length=wide?class_name.size():class_text.size();
        std::uint64_t subkeys=0,maxkey=0,maxclass=0,maxname=0,maxdata=0;
        for(const auto& path:registry_subkeys(registry,handle)){
            ++subkeys;const auto display=registry.display_names.contains(path)?registry.display_names.at(path):path.substr(path.find_last_of(u'\\')+1);maxkey=std::max<std::uint64_t>(maxkey,wide?display.size():utf16_to_utf8(display).size());
            if(const auto item=registry.classes.find(path);item!=registry.classes.end())maxclass=std::max<std::uint64_t>(maxclass,wide?item->second.size():utf16_to_utf8(item->second).size());
        }
        const auto& values=registry.keys.at(handle.path);
        for(const auto& [entry,value]:values){maxname=std::max<std::uint64_t>(maxname,wide?entry.size():utf16_to_utf8(entry).size());std::uint64_t size=value.bytes.size();if(!wide && (value.type==1 || value.type==2 || value.type==7) && !(size%2)){std::u16string text;for(std::size_t i=0;i<value.bytes.size();i+=2)text.push_back(static_cast<char16_t>(value.bytes[i]|(static_cast<unsigned>(value.bytes[i+1])<<8)));size=utf16_to_utf8(text).size();}maxdata=std::max(maxdata,size);}
        for(unsigned i:{2U,4U,5U,6U,7U,8U,9U,10U})if(arg(i))memory.check(GuestAddress{arg(i)},4,Access::Write);
        if(arg(11))memory.check(GuestAddress{arg(11)},8,Access::Write);
        if(arg(1) && capacity)memory.check(GuestAddress{arg(1)},wide?2:1,Access::Write);
        const std::array<std::pair<unsigned,std::uint64_t>,8> outputs{{{2,class_length},{4,subkeys},{5,maxkey},{6,maxclass},{7,values.size()},{8,maxname},{9,maxdata},{10,registry_security_descriptor(handle.path).size()}}};
        for(const auto& [index,value]:outputs)if(arg(index))memory.store(GuestAddress{arg(index)},32,value);
        if(arg(11))memory.store(GuestAddress{arg(11)},64,registry.modified_times.contains(handle.path)?registry.modified_times.at(handle.path):runtime_.filetime_base);
        if(arg(1)){if(capacity<=class_length)return done(234);if(wide)memory.write(GuestAddress{arg(1)},utf16_bytes(class_name));else{std::vector<std::uint8_t> bytes(class_text.begin(),class_text.end());bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);}}
        return done(0);
    }
    if(symbol.starts_with("RegEnumValue")){
        if(!(handle.access&1))return done(5);
        if(!arg(2) || !arg(3) || arg(4) || (arg(6) && !arg(7)))return done(87);
        const auto& values=registry.keys.at(handle.path);const auto index=static_cast<std::uint32_t>(arg(1));if(index>=values.size())return done(259);
        const auto item=std::next(values.begin(),index);const auto& value=item->second;const auto& display=value.display_name.empty()?item->first:value.display_name;
        const auto text=utf16_to_utf8(display);const auto length=wide?display.size():text.size();const auto capacity=memory.load(GuestAddress{arg(3)},32),data_capacity=arg(7)?memory.load(GuestAddress{arg(7)},32):0;
        auto data=value.bytes;
        if(!wide && (value.type==1 || value.type==2 || value.type==7)){
            if(data.size()%2)return done(13);std::u16string chars;for(std::size_t i=0;i<data.size();i+=2)chars.push_back(static_cast<char16_t>(data[i]|(static_cast<unsigned>(data[i+1])<<8)));
            const auto converted=utf16_to_utf8(chars);data.assign(converted.begin(),converted.end());
        }
        memory.check(GuestAddress{arg(3)},4,Access::Write);if(arg(5))memory.check(GuestAddress{arg(5)},4,Access::Write);if(arg(7))memory.check(GuestAddress{arg(7)},4,Access::Write);
        const bool name_fits=capacity>length,data_fits=!arg(6) || data_capacity>=data.size();
        if(name_fits)memory.check(GuestAddress{arg(2)},(length+1)*(wide?2:1),Access::Write);
        if(name_fits && data_fits && arg(6))memory.check(GuestAddress{arg(6)},data.size(),Access::Write);
        if(arg(5))memory.store(GuestAddress{arg(5)},32,value.type);if(arg(7))memory.store(GuestAddress{arg(7)},32,data.size());
        if(!name_fits)return done(234);
        if(wide)memory.write(GuestAddress{arg(2)},utf16_bytes(display));else{std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);memory.write(GuestAddress{arg(2)},bytes);}
        memory.store(GuestAddress{arg(3)},32,length);if(!data_fits)return done(234);if(arg(6))memory.write(GuestAddress{arg(6)},data);return done(0);
    }
    if(symbol.starts_with("RegEnumKey")){
        const bool extended=symbol.starts_with("RegEnumKeyEx");
        if(extended && (!arg(3) || arg(4) || (arg(5) && !arg(6))))return done(87);
        if(!(handle.access&8))return done(5);std::vector<std::u16string> children;const auto physical_children=registry_subkeys(registry,handle);
        for(const auto& path:physical_children)children.push_back(registry.display_names.contains(path)?registry.display_names.at(path):path.substr(path.find_last_of(u'\\')+1));
        const auto index=static_cast<std::uint32_t>(arg(1));if(index>=children.size())return done(259);
        const auto text=utf16_to_utf8(children[index]);const auto size=wide?children[index].size():text.size();const auto capacity=extended?memory.load(GuestAddress{arg(3)},32):arg(3);if(capacity<=size)return done(234);
        if(extended && arg(5) && !memory.load(GuestAddress{arg(6)},32))return done(234);
        if(!arg(2))return done(87);
        memory.check(GuestAddress{arg(2)},(size+1)*(wide?2:1),Access::Write);
        if(extended){memory.check(GuestAddress{arg(3)},4,Access::Write);if(arg(6))memory.check(GuestAddress{arg(6)},4,Access::Write);if(arg(7))memory.check(GuestAddress{arg(7)},8,Access::Write);if(arg(5))memory.check(GuestAddress{arg(5)},wide?2:1,Access::Write);}
        if(wide)memory.write(GuestAddress{arg(2)},utf16_bytes(children[index]));else {std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);memory.write(GuestAddress{arg(2)},bytes);}
        if(extended){memory.store(GuestAddress{arg(3)},32,size);if(arg(6))memory.store(GuestAddress{arg(6)},32,0);if(arg(5))memory.store(GuestAddress{arg(5)},wide?16:8,0);
            if(arg(7)){const auto& path=physical_children[index];memory.store(GuestAddress{arg(7)},64,registry.modified_times.contains(path)?registry.modified_times.at(path):runtime_.filetime_base);}}
        return done(0);
    }
    if(symbol=="RegNotifyChangeKeyValue") {
        const auto filter=static_cast<std::uint32_t>(arg(2));
        if(!(handle.access&0x10))return done(5);
        if(!(filter&15) || (filter&~0x1000000fU))return done(87);
        if(arg(4)) {
            const auto event=runtime_.wait_objects.find(arg(3));
            if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event)return done(6);
            if(!(event->second.access&2))return done(5);
        }
        if(registry_notifications_.size()>=128)return done(8);
        RegistryNotification watch;watch.key=predefined?root:arg(0);watch.event=arg(4)?arg(3):0;watch.thread=threads_.current;
        watch.path=handle.path;watch.filter=filter;watch.subtree=arg(1)!=0;
        const auto prefix=handle.path+u"\\";
        for(const auto& [path,values]:registry.keys)if(path==handle.path || (path.starts_with(prefix) && (watch.subtree || ((filter&1) && path.find(u'\\',prefix.size())==std::u16string::npos))))watch.snapshot.emplace(path,values);
        if(filter&8U)for(const auto& [path,values]:watch.snapshot){(void)values;watch.security_snapshot.emplace(path,registry_security_descriptor(path));}
        const auto token=next_registry_notification_++;registry_notifications_.emplace(token,std::move(watch));
        if(arg(4))return done(0);
        auto& thread=threads_.items.at(threads_.current);thread.wait={};thread.wait.kind=ThreadWaitKind::RegistryNotify;thread.wait.handles={token};thread.status=ThreadStatus::Waiting;
        transaction.reset();schedule(state,memory);return std::nullopt;
    }
    const bool shell_query=symbol.starts_with("SHGetValue");
    if(shell_query || get_value || legacy_query || delete_key_value){
        const auto subkey=name(arg(1));
        if(!subkey.empty()){
            const auto logical=canonical(registry_logical_path(handle)+u"\\"+subkey);
            const auto view=delete_key_value?handle.view:get_value?registry_view(query_flags>>8,x86_):registry_view(0,x86_);
            handle.path=registry_view_path(logical,view);handle.view=view;handle.logical_path=logical;
            if(registry.keys.contains(handle.path))handle.access=registry_security_access(handle.path,delete_key_value?2U:1U);
        }
        if(!registry.keys.contains(handle.path))return done(2);
    }
    const auto key_name=legacy_query?std::u16string{}:name(arg(shell_query || get_value || delete_key_value?2:1));
    if(symbol.starts_with("RegDeleteTree") || symbol.starts_with("SHDeleteKey")){
        constexpr std::uint32_t required=0x10009U; // DELETE、KEY_ENUMERATE_SUB_KEYS、KEY_QUERY_VALUE。
        if((handle.access&required)!=required)return done(5);
        const bool child=!key_name.empty();
        const auto path=child?registry_view_path(canonical(registry_logical_path(handle)+u"\\"+key_name),handle.view):handle.path;
        const auto target=registry.keys.find(path);if(target==registry.keys.end())return done(2);
        const auto prefix=path+u"\\";
        for(const auto& [mount,info]:registry.hives){(void)info;if((child && mount==path) || mount.starts_with(prefix))return done(5);}
        // 清除值也需要 KEY_SET_VALUE；先檢查整棵樹，不能刪到一半才發現權限不足。
        if(!target->second.empty() && !(handle.access&2))return done(5);
        for(auto item=registry.keys.lower_bound(prefix);item!=registry.keys.end() && item->first.starts_with(prefix);++item)
            if(!item->second.empty() && !(handle.access&2))return done(5);
        if(child)(void)registry_security_access(path,required|(!target->second.empty()?2U:0U));
        for(auto item=registry.keys.lower_bound(prefix);item!=registry.keys.end() && item->first.starts_with(prefix);++item)(void)registry_security_access(item->first,required|(!item->second.empty()?2U:0U));
        mutations.emplace_back(handle.path,child?1U:5U);modified(handle.path);
        const auto remove_values=[&](auto& values){for(const auto& [value_name,value]:values){(void)value_name;registry.bytes-=value.bytes.size();--registry.values;}};
        auto item=registry.keys.lower_bound(prefix);
        while(item!=registry.keys.end() && item->first.starts_with(prefix)){
            remove_values(item->second);registry.security_descriptors.erase(item->first);registry.display_names.erase(item->first);registry.modified_times.erase(item->first);registry.classes.erase(item->first);registry.volatile_keys.erase(item->first);item=registry.keys.erase(item);
        }
        remove_values(target->second);
        if(child){registry.security_descriptors.erase(path);registry.display_names.erase(path);registry.modified_times.erase(path);registry.classes.erase(path);registry.volatile_keys.erase(path);registry.keys.erase(target);}else target->second.clear();
        return done(0);
    }
    if(symbol.starts_with("RegDeleteKey") && !delete_key_value){
        const auto flags=symbol.starts_with("RegDeleteKeyEx")?static_cast<std::uint32_t>(arg(2)):0U;
        if(symbol.starts_with("RegDeleteKeyEx") && (arg(3) || (flags&~0x300U) || flags==0x300))return done(87);
        const auto path=registry_view_path(canonical(registry_logical_path(handle)+u"\\"+key_name),registry_view(flags,x86_));const auto found=registry.keys.find(path);if(found==registry.keys.end())return done(2);
        if(registry.hives.contains(path))return done(5);
        (void)registry_security_access(path,0x10000U);
        for(const auto& [p,v]:registry.keys){(void)v;if(p.starts_with(path+u"\\"))return done(5);}
        for(const auto& [n,v]:found->second){(void)n;registry.bytes-=v.bytes.size();--registry.values;}registry.keys.erase(found);registry.security_descriptors.erase(path);registry.display_names.erase(path);registry.modified_times.erase(path);registry.classes.erase(path);registry.volatile_keys.erase(path);modified(handle.path);mutations.emplace_back(handle.path,symbol.starts_with("RegDeleteKey")?1U:4U);return done(0);
    }
    if(symbol.starts_with("RegOpenKey") || symbol.starts_with("RegCreateKey") || (set_key_value && !key_name.empty())) {
        const bool create=symbol.starts_with("RegCreate") || set_key_value;
        const bool legacy=symbol=="RegOpenKeyA" || symbol=="RegOpenKeyW" || symbol=="RegCreateKeyA" || symbol=="RegCreateKeyW";
        const auto access=set_key_value?2U:legacy?0x02000000U:static_cast<std::uint32_t>(arg(create?5:3));
        const auto output=set_key_value?0:arg(legacy?2:create?7:4),disposition=create && !legacy && !set_key_value?arg(8):0;
        std::u16string requested_class;
        if(create && !legacy && !set_key_value && arg(3)){requested_class=wide?read_utf16(memory,GuestAddress{arg(3)},32768):utf8_to_utf16(read_ansi(memory,arg(3),32768));if(requested_class.size()>32767)return done(87);}
        if((!legacy && !set_key_value && static_cast<std::uint32_t>(arg(2))) || (!output && !set_key_value) || (access&0x300)==0x300)return done(87);
        if((access&~0xf30f033fU) || (create && !legacy && !set_key_value && static_cast<std::uint32_t>(arg(4))>1))return done(50);
        if(output)memory.check(GuestAddress{output},x86_?4:8,Access::Write);if(disposition)memory.check(GuestAddress{disposition},4,Access::Write);
        bool inherit=false;security::SecurityDescriptor explicit_descriptor;
        if(create && !legacy && !set_key_value && arg(6)){const auto sa=arg(6);const auto size=x86_?12U:24U;memory.check(GuestAddress{sa},size,Access::Read);if(memory.load(GuestAddress{sa},32)!=size)return done(87);inherit=memory.load(GuestAddress{sa+(x86_?8:16)},32)!=0;const auto sd=memory.load(GuestAddress{sa+(x86_?4:8)},x86_?32:64);if(sd)explicit_descriptor=security::read_descriptor(memory,sd,x86_,true);}
        if(create && legacy && predefined && key_name.empty()){(void)registry_security_access(handle.path,access);memory.store(GuestAddress{output},x86_?32:64,arg(0));return done(0);}
        if(key_name.empty() && (predefined || legacy) && !create) {(void)registry_security_access(handle.path,access);memory.store(GuestAddress{output},x86_?32:64,arg(0));return done(0);}
        std::vector<std::u16string> paths;auto logical=registry_logical_path(handle);std::size_t start=0;
        while(start<key_name.size()) {
            const auto end=key_name.find(u'\\',start),count=(end==std::u16string::npos?key_name.size():end)-start;
            if(!count || count>255 || paths.size()>=128)return done(87);
            logical+=u'\\';logical.append(key_name,start,count);logical=canonical(logical);paths.push_back(logical);
            if(end==std::u16string::npos)break;start=end+1;
        }
        const auto view=set_key_value?handle.view:registry_view(access,x86_);const auto path=registry_view_path(logical,view);
        paths.clear();for(std::size_t pos=2;pos<=path.size();){const auto end=path.find(u'\\',pos),stop=end==path.npos?path.size():end;paths.push_back(path.substr(0,stop));if(end==path.npos)break;pos=end+1;}
        const bool existed=registry.keys.contains(path);
        const bool volatile_key=create && !legacy && !set_key_value && arg(4)!=0;
        if(create && !existed && !volatile_key)for(const auto& parent:registry.volatile_keys)if(path.starts_with(parent+u"\\"))return done(1021);
        if(!existed && !create)return done(2);
        if((!set_key_value && registry.handles.size()>=4096) || (create && registry.keys.size()+paths.size()>4096))return done(8);
        std::map<std::u16string,std::vector<std::uint8_t>> created_descriptors;
        if(create && !existed)for(const auto& created:paths)if(!registry.keys.contains(created)){
            const auto parent=created.substr(0,created.find_last_of(u'\\'));
            const auto& parent_descriptor=created_descriptors.contains(parent)?created_descriptors.at(parent):registry_security_descriptor(parent);
            (void)object_security_access(parent_descriptor,4U,{0x20019U,0x20006U,0x20019U,0xf003fU});
            created_descriptors.emplace(created,registry_creation_descriptor(parent_descriptor,created==path?explicit_descriptor:security::SecurityDescriptor{}));
        }
        const auto granted=created_descriptors.contains(path)?object_security_access(created_descriptors.at(path),access&~0x300U,{0x20019U,0x20006U,0x20019U,0xf003fU}):registry_security_access(path,access);
        if(create){const auto requested=arg(1)?(wide?read_utf16(memory,GuestAddress{arg(1)}):utf8_to_utf16(read_ansi(memory,arg(1)))):std::u16string{};const auto original=registry_view_path(registry_logical_path(handle)+(requested.empty()?std::u16string{}:u"\\"+requested),view);for(const auto& parent:paths){const auto inserted=registry.keys.try_emplace(parent).second;if(inserted)mutations.emplace_back(parent.substr(0,parent.find_last_of(u'\\')),1);const auto pos=parent.find_last_of(u'\\')+1;registry.display_names.try_emplace(parent,original.substr(pos,parent.size()-pos));}}
        if(create && !existed){for(const auto& created:paths)if(!registry.modified_times.contains(created))modified(created);modified(handle.path);}
        if(create && !existed){if(volatile_key)for(const auto& created:paths)if(created_descriptors.contains(created))registry.volatile_keys.insert(created);
            if(!requested_class.empty())registry.classes[path]=std::move(requested_class);}
        for(auto& [created,descriptor]:created_descriptors)registry.security_descriptors[created]=std::move(descriptor);
        if(set_key_value){handle=RegistryHandle{path,granted,view,logical};}
        else {
        const auto token=registry.next_handle++;
        registry.handles.emplace(token,RegistryHandle{path,granted,view,logical});if(inherit)runtime_.handle_flags[token]=1;
        memory.store(GuestAddress{output},x86_?32:64,token);if(disposition)memory.store(GuestAddress{disposition},32,existed?2:1);return done(0);
        }
    }
    auto& values=registry.keys.at(handle.path);
    const auto value_name=set_key_value?name(arg(2)):key_name;
    if(symbol.starts_with("RegDeleteValue") || delete_key_value) {
        if(!(handle.access&2))return done(5);
        const auto found=values.find(value_name);if(found==values.end())return done(2);
        registry.bytes-=found->second.bytes.size();--registry.values;values.erase(found);modified(handle.path);mutations.emplace_back(handle.path,4U);return done(0);
    }
    if(!shell_query && !get_value && !legacy_query && !key_value && arg(2))return done(87);
    if(symbol.starts_with("RegSetValueEx") || set_key_value) {
        if(!(handle.access&2))return done(5);
        const auto type=static_cast<std::uint32_t>(arg(3)),size=static_cast<std::uint32_t>(arg(5));
        if(size>output_limit)return done(8);if(size && !arg(4))return done(87);
        std::vector<std::uint8_t> data(size);if(size)memory.read(GuestAddress{arg(4)},data);
        if(!wide && (type==1 || type==2 || type==7)) {
            const auto converted=utf8_to_utf16(std::string(data.begin(),data.end()));data.clear();
            for(const auto c:converted){data.push_back(static_cast<std::uint8_t>(c));data.push_back(static_cast<std::uint8_t>(c>>8));}
        }
        const auto found=values.find(value_name);const auto previous=found==values.end()?0:found->second.bytes.size();
        if(registry.bytes-previous+data.size()>output_limit || (found==values.end() && registry.values>=4096))return done(8);
        registry.bytes=registry.bytes-previous+data.size();if(found==values.end())++registry.values;
        const auto pointer=arg(set_key_value?2:1);const auto original=pointer?(wide?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16(read_ansi(memory,pointer))):std::u16string{};
        const auto display=found==values.end()?(original==value_name?std::u16string{}:original):found->second.display_name;
        values[value_name]={type,std::move(data),display};modified(handle.path);mutations.emplace_back(handle.path,4U);return done(0);
    }
    if(!(handle.access&1))return done(5);
    const auto type_out=legacy_query?0:arg(get_value?4:3),data_out=arg(legacy_query?2:get_value?5:4),size_out=arg(legacy_query?3:get_value?6:5);
    if(data_out && !size_out)return done(87);
    const auto found=values.find(key_name);if(found==values.end() && !legacy_query)return done(2);
    auto data=found==values.end()?std::vector<std::uint8_t>{0,0}:found->second.bytes;
    auto type=found==values.end()?1U:found->second.type;
    if(legacy_query && type!=1)return done(13);
    if(get_value && (type==1 || type==2 || type==7)){
        if(data.size()%2)return done(13);
        std::u16string text;for(std::size_t i=0;i<data.size();i+=2)text.push_back(static_cast<char16_t>(data[i]|(static_cast<unsigned>(data[i+1])<<8)));
        if(type==2 && !(query_flags&0x10000000U)){
            std::u16string expanded;
            for(std::size_t i=0;i<text.size() && text[i];){
                if(text[i]!=u'%'){expanded+=text[i++];continue;}
                const auto end=text.find(u'%',i+1);if(end==text.npos){expanded+=text.substr(i);break;}
                auto key=text.substr(i+1,end-i-1);for(auto& c:key)if(c>=u'a' && c<=u'z')c-=32;
                const auto env=runtime_.environment.find(key);expanded+=env==runtime_.environment.end()?text.substr(i,end-i+1):env->second.second;i=end+1;
                if(expanded.size()>output_limit/2)return done(8);
            }
            text=std::move(expanded);type=1;
        }
        if(text.empty() || text.back())text.push_back(0);
        if(type==7 && (text.size()<2 || text[text.size()-2]))text.push_back(0);
        data=utf16_bytes(text);data.resize(text.size()*2);
    }
    if(get_value){
        const auto accepted=query_flags&0xffffU,bit=type<=4?1U<<type:type==7?32U:type==11?64U:0U;
        if(accepted && accepted!=0xffffU && !(accepted&bit))return done(1630);
        if((accepted==0x18U && data.size()!=4) || (accepted==0x48U && data.size()!=8))return done(13);
    }
    if(!wide && (type==1 || type==2 || type==7)) {
        if(data.size()%2)return done(13);std::u16string text;
        for(std::size_t i=0;i<data.size();i+=2)text.push_back(static_cast<char16_t>(data[i]|(static_cast<unsigned>(data[i+1])<<8)));
        const auto converted=utf16_to_utf8(text);data.assign(converted.begin(),converted.end());
    }
    const auto capacity=size_out?memory.load(GuestAddress{size_out},32):0;
    if(type_out)memory.check(GuestAddress{type_out},4,Access::Write);if(size_out)memory.check(GuestAddress{size_out},4,Access::Write);
    if(data_out && capacity>=data.size())memory.check(GuestAddress{data_out},data.size(),Access::Write);
    if(type_out)memory.store(GuestAddress{type_out},32,type);if(size_out)memory.store(GuestAddress{size_out},32,data.size());
    if(data_out){if(capacity<data.size())return done(234);memory.write(GuestAddress{data_out},data);}
    return done(0);
    }catch(const SecurityDescriptorError& error){arg.return_to(state,error.code);return std::nullopt;}
    catch(const MemoryFault&){arg.return_to(state,998);return std::nullopt;}
    catch(const std::bad_alloc&){arg.return_to(state,8);return std::nullopt;}
}
}
