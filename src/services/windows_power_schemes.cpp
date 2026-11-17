// powrprof 電源方案（新舊兩代介面共用同一份沙箱方案儲存；windows_power.cpp 把方案查詢入口轉到這裡）：
// - 方案清單：舊式索引 0 為 macOS 原生方案（power::native_scheme，三項逾時預設取自宿主電源偏好）；WritePwrScheme 以新索引建立的方案排在其後（最多 64 個）。
// - 儲存在沙箱登錄 HKLM\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes：ActivePowerScheme（REG_SZ）、GlobalPowerPolicy（REG_BINARY 192），
//   各方案子機碼 <GUID> 存 FriendlyName／Description／LegacyIndex／LegacyPolicy（144）／ProcessorPolicy（156），<群組>\<設定> 存 ACSettingIndex／DCSettingIndex。
//   方案與設定只影響客體讀到的值，不改動 macOS 電源偏好；宿主值只在沒有寫入覆寫時採用。
// - 寫入前以登錄 ACL 檢查 KEY_SET_VALUE|KEY_CREATE_SUB_KEY，CanUserWritePwrScheme 回報同一結果（不足為 ERROR_ACCESS_DENIED）。
// - 舊式 POWER_POLICY 的 Video／Idle／Spindown 逾時與三項設定的 AC／DC 值互相對應；其他欄位保存寫入值，沒有寫入時依宿主能力推導，無法得知的欄位為 0。
//   寫入的結構 Revision 必須為 1（否則 ERROR_REVISION_MISMATCH）；不能刪除使用中的方案與原生方案（ERROR_ACCESS_DENIED）。
// - IsPwr*Allowed 依 GetPwrCapabilities 的 S1～S5 與 hiberfile 欄位；GetPwrDiskSpindownRange 為 macOS 以分鐘計的磁碟睡眠範圍。
// - IsAdminOverrideActive：沙箱沒有系統管理員電源原則，回 FALSE。
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_power_values.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <random>

namespace x64arm {
namespace {
using power::Guid;
constexpr std::u16string_view store_display=u"\x02\\SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes";
constexpr std::size_t policy_size=144,global_size=192,processor_size=156,user_scheme_limit=64;
// POWER_POLICY 內三項逾時的 AC 欄位位移（DC 在其後 4 bytes）：顯示器 VideoTimeout、睡眠 IdleTimeout、磁碟 SpindownTimeout。
constexpr std::array<unsigned,3> timeout_offset{56,28,64};
std::u16string folded(std::u16string_view text){std::u16string out(text);for(auto& c:out)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return out;}
std::u16string guid_text(const Guid& id){
    constexpr char16_t digits[]=u"0123456789abcdef";constexpr std::array<unsigned,16> order{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    std::u16string out;for(unsigned i=0;i<16;++i){if(i==4 || i==6 || i==8 || i==10)out.push_back(u'-');out.push_back(digits[id[order[i]]>>4]);out.push_back(digits[id[order[i]]&15]);}
    return out;
}
std::optional<Guid> parse_guid(std::u16string_view text){
    if(text.size()==38 && text.front()==u'{' && text.back()==u'}')text=text.substr(1,36);
    if(text.size()!=36)return {};
    constexpr std::array<unsigned,16> order{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};Guid out{};std::size_t at=0;
    const auto nibble=[](char16_t c)->int{return c>=u'0' && c<=u'9'?c-u'0':c>=u'a' && c<=u'f'?c-u'a'+10:c>=u'A' && c<=u'F'?c-u'A'+10:-1;};
    for(unsigned i=0;i<16;++i){
        if(i==4 || i==6 || i==8 || i==10){if(text[at]!=u'-')return {};++at;}
        const auto high=nibble(text[at]),low=nibble(text[at+1]);if(high<0 || low<0)return {};
        out[order[i]]=static_cast<std::uint8_t>(high*16+low);at+=2;
    }
    return out;
}
std::uint32_t le32(std::span<const std::uint8_t> bytes,std::size_t at){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t{bytes[at+i]}<<(8*i);return v;}
void put32(std::span<std::uint8_t> bytes,std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(v>>(8*i));}
struct Scheme {
    Guid id{};std::uint32_t index{};
    std::optional<std::u16string> name,description;
    std::array<std::optional<std::uint32_t>,6> values{}; // 設定 i 的 AC 在 i*2、DC 在 i*2+1
    std::vector<std::uint8_t> legacy,processor;
};
struct Store {Guid active{power::native_scheme};std::vector<Scheme> schemes;std::vector<std::uint8_t> global;};
Store load_store(const RegistryState& registry){
    Store store;Scheme native;native.id=power::native_scheme;store.schemes.push_back(std::move(native));
    const auto base=folded(store_display);
    const auto value=[&](const std::u16string& key,std::u16string_view name)->const RegistryValue*{
        const auto k=registry.keys.find(key);if(k==registry.keys.end())return nullptr;const auto v=k->second.find(std::u16string(name));return v==k->second.end()?nullptr:&v->second;};
    const auto text=[](const RegistryValue* v)->std::optional<std::u16string>{
        if(!v || (v->type!=1 && v->type!=2) || v->bytes.size()%2)return {};std::u16string out;
        for(std::size_t i=0;i+1<v->bytes.size();i+=2){const auto c=static_cast<char16_t>(v->bytes[i]|(v->bytes[i+1]<<8));if(!c)break;out.push_back(c);}return out;};
    const auto dword=[](const RegistryValue* v)->std::optional<std::uint32_t>{if(!v || v->type!=4 || v->bytes.size()!=4)return {};return le32(v->bytes,0);};
    const auto blob=[](const RegistryValue* v,std::size_t size){return v && v->type==3 && v->bytes.size()==size?v->bytes:std::vector<std::uint8_t>{};};
    if(const auto active=text(value(base,u"activepowerscheme")))if(const auto id=parse_guid(*active))store.active=*id;
    store.global=blob(value(base,u"globalpowerpolicy"),global_size);
    const auto prefix=base+u"\\";
    for(auto it=registry.keys.lower_bound(prefix);it!=registry.keys.end() && it->first.starts_with(prefix);++it){
        const auto rest=std::u16string_view(it->first).substr(prefix.size());if(rest.find(u'\\')!=rest.npos)continue;
        const auto id=parse_guid(rest);if(!id)continue;
        Scheme scheme;scheme.id=*id;
        if(*id!=power::native_scheme){
            const auto index=dword(value(it->first,u"legacyindex"));if(!index || !*index)continue;scheme.index=*index;
            if(std::any_of(store.schemes.begin(),store.schemes.end(),[&](const auto& s){return s.index==scheme.index;}))continue;
        }
        scheme.name=text(value(it->first,u"friendlyname"));scheme.description=text(value(it->first,u"description"));
        scheme.legacy=blob(value(it->first,u"legacypolicy"),policy_size);scheme.processor=blob(value(it->first,u"processorpolicy"),processor_size);
        for(std::size_t i=0;i<power::settings.size();++i){
            const auto key=it->first+u"\\"+guid_text(power::settings[i].group)+u"\\"+guid_text(power::settings[i].value);
            scheme.values[i*2]=dword(value(key,u"acsettingindex"));scheme.values[i*2+1]=dword(value(key,u"dcsettingindex"));
        }
        if(*id==power::native_scheme)store.schemes.front()=std::move(scheme);else store.schemes.push_back(std::move(scheme));
    }
    std::sort(store.schemes.begin()+1,store.schemes.end(),[](const auto& a,const auto& b){return a.index<b.index;});
    if(std::none_of(store.schemes.begin(),store.schemes.end(),[&](const auto& s){return s.id==store.active;}))store.active=power::native_scheme;
    return store;
}
const Scheme* find_scheme(const Store& store,const Guid& id){for(const auto& s:store.schemes)if(s.id==id)return &s;return nullptr;}
const Scheme* find_index(const Store& store,std::uint32_t index){for(const auto& s:store.schemes)if(s.index==index)return &s;return nullptr;}
std::size_t setting_position(const Guid& group,const Guid& setting){
    std::size_t i=0;while(i<power::settings.size() && (power::settings[i].group!=group || power::settings[i].value!=setting))++i;return i;
}
// 登錄變更（呼叫者持有可寫交易）：機碼與顯示名稱、值數量與位元組統計和其他直接寫入登錄的服務一致。
void ensure_key(RegistryState& registry,const std::u16string& display,std::uint64_t now){
    const auto canonical=folded(display);
    for(std::size_t at=0;;){
        const auto next=canonical.find(u'\\',at);const auto prefix=canonical.substr(0,next);
        if(registry.keys.try_emplace(prefix).second){registry.display_names[prefix]=display.substr(0,next);registry.modified_times[prefix]=now;}
        if(next==canonical.npos)break;at=next+1;
    }
}
void put_value(RegistryState& registry,const std::u16string& display,std::u16string_view name,std::uint32_t type,std::vector<std::uint8_t> bytes,std::uint64_t now){
    ensure_key(registry,display,now);const auto key=folded(display);auto& values=registry.keys[key];const auto folded_name=folded(name);
    if(const auto old=values.find(folded_name);old!=values.end())registry.bytes-=old->second.bytes.size();else ++registry.values;
    registry.bytes+=bytes.size();values[folded_name]={type,std::move(bytes),std::u16string(name)};registry.modified_times[key]=now;
}
std::vector<std::uint8_t> dword_bytes(std::uint32_t v){std::vector<std::uint8_t> out(4);put32(out,0,v);return out;}
void erase_tree(RegistryState& registry,const std::u16string& canonical){
    for(auto it=registry.keys.lower_bound(canonical);it!=registry.keys.end() && it->first.starts_with(canonical);){
        if(it->first.size()!=canonical.size() && it->first[canonical.size()]!=u'\\'){++it;continue;}
        for(const auto& [name,value]:it->second){(void)name;registry.bytes-=value.bytes.size();--registry.values;}
        registry.modified_times.erase(it->first);registry.display_names.erase(it->first);registry.security_descriptors.erase(it->first);it=registry.keys.erase(it);
    }
}
std::u16string scheme_display(const Guid& id){return std::u16string(store_display)+u"\\"+guid_text(id);}
void put_values(RegistryState& registry,const Guid& id,const std::array<std::optional<std::uint32_t>,6>& values,std::uint64_t now){
    for(std::size_t i=0;i<power::settings.size();++i)for(unsigned dc=0;dc<2;++dc)if(const auto v=values[i*2+dc])
        put_value(registry,scheme_display(id)+u"\\"+guid_text(power::settings[i].group)+u"\\"+guid_text(power::settings[i].value),dc?u"DCSettingIndex":u"ACSettingIndex",4,dword_bytes(*v),now);
}
std::array<std::optional<std::uint32_t>,6> policy_values(std::span<const std::uint8_t> policy){
    std::array<std::optional<std::uint32_t>,6> out{};for(std::size_t i=0;i<3;++i){out[i*2]=le32(policy,timeout_offset[i]);out[i*2+1]=le32(policy,timeout_offset[i]+4);}return out;
}
// 能力結構：PowerButtonPresent 0、SleepButtonPresent 1、LidPresent 2、SystemS1～S5 3～7、HiberFilePresent 8。
std::uint32_t deepest_sleep(const std::optional<std::array<std::uint8_t,76>>& caps){if(!caps)return 0;for(unsigned s=3;s>=1;--s)if((*caps)[2+s])return s+1;return 0;}
std::uint32_t lightest_sleep(const std::optional<std::array<std::uint8_t,76>>& caps){if(!caps)return 0;for(unsigned s=1;s<=3;++s)if((*caps)[2+s])return s+1;return 0;}
std::vector<std::uint8_t> default_policy(const std::optional<std::array<std::uint8_t,76>>& caps){
    std::vector<std::uint8_t> p(policy_size);put32(p,0,1);put32(p,80,1); // USER／MACHINE_POWER_POLICY.Revision
    p[38]=p[39]=3; // ThrottlePolicy：PO_THROTTLE_ADAPTIVE（macOS 依負載動態調整時脈）
    put32(p,40,deepest_sleep(caps));put32(p,44,deepest_sleep(caps));
    p[74]=p[75]=100;p[76]=p[77]=100; // FanThrottleTolerance／ForcedThrottle：不強制降速
    for(const unsigned at:{84U,88U,92U,96U})put32(p,at,lightest_sleep(caps));
    return p;
}
std::vector<std::uint8_t> default_global(const std::optional<std::array<std::uint8_t,76>>& caps){
    std::vector<std::uint8_t> g(global_size);put32(g,0,1);put32(g,176,1);
    if(caps && (*caps)[2]){put32(g,52,2);put32(g,64,2);put32(g,180,1);put32(g,184,1);} // 闔上螢幕睡眠（PowerActionSleep）、掀開回到 PowerSystemWorking
    return g;
}
std::vector<std::uint8_t> default_processor(){
    std::vector<std::uint8_t> p(processor_size);put32(p,0,1);
    for(const unsigned at:{4U,80U}){put32(p,at,1);p[at+4]=3;} // PROCESSOR_POWER_POLICY：Revision 1、DynamicThrottle 為 PO_THROTTLE_ADAPTIVE、無 C-state 原則
    return p;
}
Guid random_guid(){
    std::random_device device;Guid id{};for(unsigned i=0;i<16;i+=4){const auto v=device();for(unsigned b=0;b<4;++b)id[i+b]=static_cast<std::uint8_t>(v>>(8*b));}
    id[7]=static_cast<std::uint8_t>((id[7]&0x0f)|0x40);id[8]=static_cast<std::uint8_t>((id[8]&0x3f)|0x80);return id;
}
}
std::span<const WindowsServiceDescriptor> power_scheme_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"powrprof.dll","CanUserWritePwrScheme",0},{"powrprof.dll","IsAdminOverrideActive",1},
        {"powrprof.dll","IsPwrHibernateAllowed",0},{"powrprof.dll","IsPwrShutdownAllowed",0},{"powrprof.dll","IsPwrSuspendAllowed",0},
        {"powrprof.dll","GetPwrDiskSpindownRange",2},{"powrprof.dll","GetCurrentPowerPolicies",2},
        {"powrprof.dll","ReadGlobalPwrPolicy",1},{"powrprof.dll","WriteGlobalPwrPolicy",1},
        {"powrprof.dll","ReadPwrScheme",2,0,0,0x1},{"powrprof.dll","WritePwrScheme",4},{"powrprof.dll","DeletePwrScheme",1,0,0,0x1},
        {"powrprof.dll","SetActivePwrScheme",3,0,0,0x1},
        {"powrprof.dll","ReadProcessorPwrScheme",2,0,0,0x1},{"powrprof.dll","WriteProcessorPwrScheme",2,0,0,0x1},
        {"powrprof.dll","PowerSetActiveScheme",2},{"powrprof.dll","PowerWriteACValueIndex",5,0,0,0x10},{"powrprof.dll","PowerWriteDCValueIndex",5,0,0,0x10}};
    return table;
}
bool power_scheme_query_symbol(std::string_view symbol){
    return symbol=="EnumPwrSchemes" || symbol=="GetActivePwrScheme" || symbol=="PowerEnumerate" || symbol=="PowerGetActiveScheme" ||
        symbol=="PowerReadFriendlyName" || symbol=="PowerReadDescription" || symbol=="PowerReadACValue" || symbol=="PowerReadDCValue" ||
        symbol=="PowerReadACValueIndex" || symbol=="PowerReadDCValueIndex";
}
std::optional<std::uint32_t> WindowsServices::invoke_power_schemes(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    const auto read_store=[this](Store& out)->std::uint32_t{
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return transaction.error;
        out=load_store(runtime_.registry);return 0;
    };
    std::optional<NativePowerPreferences> host;bool host_loaded=false;
    // 設定值：寫入覆寫優先；原生方案沒有覆寫時查宿主（比較模式不讀宿主）。
    const auto effective=[&](const Scheme& scheme,std::size_t setting,bool dc)->power::Value{
        if(const auto v=scheme.values[setting*2+(dc?1:0)])return {0,*v};
        if(scheme.id!=power::native_scheme)return {2,0};
        if(!runtime_.network.external_io)return {50,0};
        if(!host_loaded){host=native_power_preferences();host_loaded=true;}
        if(!host)return {50,0};
        return power::timeout_value(*host,dc,setting);
    };
    std::optional<std::array<std::uint8_t,76>> caps;bool caps_loaded=false;
    const auto capabilities=[&]()->const std::optional<std::array<std::uint8_t,76>>&{
        if(!caps_loaded){caps_loaded=true;if(runtime_.network.external_io)caps=native_power_capabilities();}return caps;};
    const auto compose=[&](const Scheme& scheme){
        auto policy=scheme.legacy.empty()?default_policy(capabilities()):scheme.legacy;
        for(std::size_t i=0;i<3;++i)for(unsigned dc=0;dc<2;++dc){
            const auto v=effective(scheme,i,dc);const auto at=timeout_offset[i]+dc*4;
            if(!v.status)put32(policy,at,v.seconds);else if(scheme.legacy.empty())put32(policy,at,0);
        }
        // 預設閒置動作：有睡眠逾時才睡眠（PowerActionSleep），否則 PowerActionNone。
        if(scheme.legacy.empty()){put32(policy,4,le32(policy,28)?2:0);put32(policy,16,le32(policy,32)?2:0);}
        return policy;
    };
    const auto scheme_name=[](const Scheme& s){return s.name?*s.name:s.id==power::native_scheme?std::u16string(u"macOS 原生電源設定"):std::u16string{};};
    const auto scheme_description=[](const Scheme& s){return s.description?*s.description:s.id==power::native_scheme?std::u16string(u"由 macOS 系統電源偏好設定提供的電源方案"):std::u16string{};};
    if(name=="EnumPwrSchemes"){
        // PWRSCHEMESENUMPROC(uiIndex, dwName, sName, dwDesc, sDesc, pp, lParam)；回呼回傳 FALSE 時停止列舉。
        Store store;const auto error=read_store(store);
        struct Row {std::uint32_t index{};std::vector<std::uint8_t> name,description,policy;};
        std::vector<Row> rows;if(!error)for(const auto& s:store.schemes)rows.push_back({s.index,utf16_bytes(scheme_name(s)),utf16_bytes(scheme_description(s)),compose(s)});
        return nested_continuation(state,memory,"EnumPwrSchemes",[this,service,error,rows=std::move(rows)](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto finish=[&](bool ok,std::uint32_t code=0){if(!ok)last_error_=code;call.return_to(s,ok?1:0);return std::optional<std::uint32_t>{};};
            if(!call(0))co_return finish(false,87);if(error)co_return finish(false,error);
            for(const auto& row:rows){
                const auto size=policy_size+row.name.size()+row.description.size();
                const auto block=allocate_heap(m,size);if(!block)co_return finish(false,8);
                struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::uint64_t block;~Release(){(void)self.free_heap(m,block,s);}} release{*this,m,s,block};
                const auto name_at=block+policy_size,description_at=name_at+row.name.size();
                m.write(GuestAddress{block},row.policy);m.write(GuestAddress{name_at},row.name);m.write(GuestAddress{description_at},row.description);
                const auto keep=co_await service_callback::call_value(call(0),{row.index,row.name.size(),name_at,row.description.size(),description_at,block,call(1)});
                if(!(keep&0xff))break;
            }
            co_return finish(true);
        });
    }
    const WindowsCall arg(state,memory,service);const std::uint8_t bits=x86_?32:64;
    const bool modern=name.starts_with("Power");
    const auto status=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto boolean=[&](bool ok,std::uint32_t error=0){if(!ok)last_error_=error;arg.return_to(state,ok?1:0);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){return modern?status(error):boolean(false,error);};
    const auto load_guid=[&](std::uint64_t at){Guid v{};memory.read(GuestAddress{at},v);return v;};
    const auto read_bytes=[&](std::uint64_t at,std::size_t size){std::vector<std::uint8_t> out(size);memory.check(GuestAddress{at},size,Access::Read);for(std::size_t i=0;i<size;++i)out[i]=static_cast<std::uint8_t>(memory.load(GuestAddress{at+i},8));return out;};
    const auto writable=[&]{
        auto probe=folded(store_display);
        while(!runtime_.registry.keys.contains(probe)){const auto slash=probe.find_last_of(u'\\');if(slash==probe.npos)return true;probe.resize(slash);}
        return (registry_security_access(probe,6)&6U)==6U;
    };
    // 修改：可寫交易內重新載入方案、檢查 ACL，失敗或超出登錄上限時還原。
    const auto mutate=[&](const std::function<std::uint32_t(const Store&,RegistryState&,std::uint64_t)>& change)->std::uint32_t{
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_);if(transaction.error)return transaction.error;
        if(!writable()){transaction.unchanged();return 5;}
        auto before=runtime_.registry;const auto store=load_store(runtime_.registry);
        auto error=change(store,runtime_.registry,runtime_.system_filetime());
        if(!error && (runtime_.registry.keys.size()>4096 || runtime_.registry.values>4096 || runtime_.registry.bytes>16*1024*1024))error=8;
        if(error){runtime_.registry=std::move(before);transaction.unchanged();return error;}
        if(const auto committed=transaction.commit()){runtime_.registry=std::move(before);return committed;}
        poll_registry_notifications(true,folded(store_display),5);return 0;
    };
    const auto revision=[](std::span<const std::uint8_t> bytes,std::initializer_list<std::size_t> at){return std::all_of(at.begin(),at.end(),[&](auto o){return le32(bytes,o)==1;});};
    std::uint64_t allocation=0;
    try{
        if(name=="IsAdminOverrideActive")return boolean(false,last_error_);
        if(name=="IsPwrHibernateAllowed" || name=="IsPwrShutdownAllowed" || name=="IsPwrSuspendAllowed"){
            const auto& value=capabilities();if(!value)return boolean(false,50);
            const bool allowed=name=="IsPwrSuspendAllowed"?((*value)[3] || (*value)[4] || (*value)[5]):name=="IsPwrHibernateAllowed"?((*value)[6] && (*value)[8]):(*value)[7]!=0;
            return boolean(allowed,last_error_);
        }
        if(name=="GetPwrDiskSpindownRange"){
            // macOS 磁碟睡眠以分鐘設定：最短 1 分鐘，最長為 32 位元秒數可表示的最大整分鐘。
            if(!arg(0) || !arg(1))return boolean(false,87);
            memory.store(GuestAddress{arg(0)},32,(UINT32_MAX/60U)*60U);memory.store(GuestAddress{arg(1)},32,60);return boolean(true);
        }
        if(name=="CanUserWritePwrScheme"){
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return boolean(false,transaction.error);
            return writable()?boolean(true):boolean(false,5);
        }
        if(name=="GetActivePwrScheme"){
            if(!arg(0))return boolean(false,87);Store store;if(const auto e=read_store(store))return boolean(false,e);
            memory.store(GuestAddress{arg(0)},32,find_scheme(store,store.active)->index);return boolean(true);
        }
        if(name=="ReadPwrScheme" || name=="ReadProcessorPwrScheme" || name=="ReadGlobalPwrPolicy" || name=="GetCurrentPowerPolicies"){
            const bool global=name=="ReadGlobalPwrPolicy",current=name=="GetCurrentPowerPolicies";
            if(!arg(global?0:1) || (current && !arg(0)))return boolean(false,87);
            Store store;if(const auto e=read_store(store))return boolean(false,e);
            const auto* scheme=global?nullptr:current?find_scheme(store,store.active):find_index(store,static_cast<std::uint32_t>(arg(0)));
            if(!global && !scheme)return boolean(false,2);
            if(global || current){const auto g=store.global.empty()?default_global(capabilities()):store.global;memory.check(GuestAddress{arg(0)},global_size,Access::Write);memory.write(GuestAddress{arg(0)},g);}
            if(name=="ReadProcessorPwrScheme"){memory.write(GuestAddress{arg(1)},scheme->processor.empty()?default_processor():scheme->processor);return boolean(true);}
            if(!global){const auto policy=compose(*scheme);memory.check(GuestAddress{arg(1)},policy_size,Access::Write);memory.write(GuestAddress{arg(1)},policy);}
            return boolean(true);
        }
        if(name=="WriteGlobalPwrPolicy"){
            if(!arg(0))return boolean(false,87);const auto g=read_bytes(arg(0),global_size);if(!revision(g,{0,176}))return boolean(false,1306);
            const auto e=mutate([&](const Store&,RegistryState& r,std::uint64_t now){put_value(r,std::u16string(store_display),u"GlobalPowerPolicy",3,g,now);return 0U;});
            return e?boolean(false,e):boolean(true);
        }
        if(name=="WriteProcessorPwrScheme"){
            if(!arg(1))return boolean(false,87);const auto p=read_bytes(arg(1),processor_size);if(!revision(p,{0}))return boolean(false,1306);
            const auto index=static_cast<std::uint32_t>(arg(0));
            const auto e=mutate([&](const Store& store,RegistryState& r,std::uint64_t now){
                const auto* scheme=find_index(store,index);if(!scheme)return 2U;put_value(r,scheme_display(scheme->id),u"ProcessorPolicy",3,p,now);return 0U;});
            return e?boolean(false,e):boolean(true);
        }
        if(name=="WritePwrScheme"){
            // (puiID, lpszSchemeName, lpszDescription 可省略, lpScheme)：同索引方案取代，否則以該索引建立新方案；不改變使用中的方案。
            if(!arg(0) || !arg(1) || !arg(3))return boolean(false,87);
            const auto index=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},32));
            const auto title=read_utf16(memory,GuestAddress{arg(1)},32767),description=arg(2)?read_utf16(memory,GuestAddress{arg(2)},32767):std::u16string{};
            const auto p=read_bytes(arg(3),policy_size);if(!revision(p,{0,80}))return boolean(false,1306);
            const auto e=mutate([&](const Store& store,RegistryState& r,std::uint64_t now){
                const auto* existing=find_index(store,index);
                if(!existing && store.schemes.size()>user_scheme_limit)return 8U;
                const auto id=existing?existing->id:random_guid();const auto key=scheme_display(id);
                if(!existing)put_value(r,key,u"LegacyIndex",4,dword_bytes(index),now);
                put_value(r,key,u"FriendlyName",1,utf16_bytes(title),now);put_value(r,key,u"Description",1,utf16_bytes(description),now);
                put_value(r,key,u"LegacyPolicy",3,p,now);put_values(r,id,policy_values(p),now);return 0U;
            });
            return e?boolean(false,e):boolean(true);
        }
        if(name=="DeletePwrScheme"){
            const auto index=static_cast<std::uint32_t>(arg(0));
            const auto e=mutate([&](const Store& store,RegistryState& r,std::uint64_t){
                const auto* scheme=find_index(store,index);if(!scheme)return 2U;
                if(scheme->id==store.active || scheme->id==power::native_scheme)return 5U;
                erase_tree(r,folded(scheme_display(scheme->id)));return 0U;
            });
            return e?boolean(false,e):boolean(true);
        }
        if(name=="SetActivePwrScheme"){
            // (uiID, pGlobalPowerPolicy 可省略, pPowerPolicy 可省略)：提供的結構取代全域原則與方案原則後啟用該方案。
            const auto index=static_cast<std::uint32_t>(arg(0));
            std::vector<std::uint8_t> g,p;
            if(arg(1)){g=read_bytes(arg(1),global_size);if(!revision(g,{0,176}))return boolean(false,1306);}
            if(arg(2)){p=read_bytes(arg(2),policy_size);if(!revision(p,{0,80}))return boolean(false,1306);}
            const auto e=mutate([&](const Store& store,RegistryState& r,std::uint64_t now){
                const auto* scheme=find_index(store,index);if(!scheme)return 2U;
                if(!g.empty())put_value(r,std::u16string(store_display),u"GlobalPowerPolicy",3,g,now);
                if(!p.empty()){put_value(r,scheme_display(scheme->id),u"LegacyPolicy",3,p,now);put_values(r,scheme->id,policy_values(p),now);}
                put_value(r,std::u16string(store_display),u"ActivePowerScheme",1,utf16_bytes(guid_text(scheme->id)),now);return 0U;
            });
            return e?boolean(false,e):boolean(true);
        }
        if(name=="PowerSetActiveScheme"){
            // (UserRootPowerKey 保留須為 NULL, SchemeGuid)
            if(arg(0) || !arg(1))return status(87);const auto id=load_guid(arg(1));
            return status(mutate([&](const Store& store,RegistryState& r,std::uint64_t now){
                if(!find_scheme(store,id))return 2U;put_value(r,std::u16string(store_display),u"ActivePowerScheme",1,utf16_bytes(guid_text(id)),now);return 0U;}));
        }
        if(name=="PowerWriteACValueIndex" || name=="PowerWriteDCValueIndex"){
            // (RootPowerKey 須為 NULL, SchemeGuid, SubGroupOfPowerSettingsGuid, PowerSettingGuid, ValueIndex)
            if(arg(0) || !arg(1) || !arg(3))return status(87);
            const auto id=load_guid(arg(1)),group=arg(2)?load_guid(arg(2)):Guid{},setting=load_guid(arg(3));
            const auto position=setting_position(group,setting);const bool dc=name=="PowerWriteDCValueIndex";const auto value=static_cast<std::uint32_t>(arg(4));
            return status(mutate([&](const Store& store,RegistryState& r,std::uint64_t now){
                if(!find_scheme(store,id) || position==power::settings.size())return 2U;
                std::array<std::optional<std::uint32_t>,6> values{};values[position*2+(dc?1:0)]=value;put_values(r,id,values,now);return 0U;}));
        }
        // 以下為新式查詢（自 windows_power.cpp 轉入）；SchemeGuid 為 NULL 時使用目前啟用的方案。
        if(name=="PowerEnumerate"){
            // (RootPowerKey, SchemeGuid, SubGroupOfPowerSettingsGuid, AccessFlags, Index, Buffer, BufferSize)
            if(arg(0) || !arg(6))return status(87);
            Store store;if(const auto e=read_store(store))return status(e);
            const auto access=static_cast<std::uint32_t>(arg(3)),index=static_cast<std::uint32_t>(arg(4));std::vector<Guid> items;
            if(access==16){for(const auto& s:store.schemes)items.push_back(s.id);}
            else if(access==17 || access==18){
                if(!arg(1) || !find_scheme(store,load_guid(arg(1))))return status(2);
                if(access==17){for(const auto& setting:power::settings)if(std::find(items.begin(),items.end(),setting.group)==items.end())items.push_back(setting.group);}
                else {if(!arg(2))return status(87);const auto group=load_guid(arg(2));for(const auto& setting:power::settings)if(setting.group==group)items.push_back(setting.value);if(items.empty())return status(2);}
            }else return status(87);
            if(index>=items.size())return status(259);
            const auto capacity=memory.load(GuestAddress{arg(6)},32);memory.store(GuestAddress{arg(6)},32,16);
            if(!arg(5))return status(0);if(capacity<16)return status(234);memory.write(GuestAddress{arg(5)},items[index]);return status(0);
        }
        if(name=="PowerReadFriendlyName" || name=="PowerReadDescription"){
            // (RootPowerKey, SchemeGuid, SubGroupOfPowerSettingsGuid, PowerSettingGuid, Buffer, BufferSize)
            if(arg(0) || !arg(5))return status(87);
            Store store;if(const auto e=read_store(store))return status(e);
            const auto* scheme=find_scheme(store,arg(1)?load_guid(arg(1)):store.active);if(!scheme)return status(2);
            const bool friendly=name=="PowerReadFriendlyName";
            std::u16string text=friendly?scheme_name(*scheme):scheme_description(*scheme);
            if(arg(2)){
                const auto group=load_guid(arg(2));std::size_t i=0;while(i<power::settings.size() && power::settings[i].group!=group)++i;
                if(i==power::settings.size())return status(2);
                static constexpr std::u16string_view group_names[]{u"顯示器",u"睡眠",u"磁碟"},setting_names[]{u"關閉顯示器時間",u"系統睡眠時間",u"磁碟睡眠時間"};
                static constexpr std::u16string_view group_texts[]{u"顯示器的電源管理設定",u"系統睡眠的電源管理設定",u"磁碟的電源管理設定"};
                static constexpr std::u16string_view setting_texts[]{u"閒置多久後關閉顯示器（macOS 顯示器睡眠）",u"閒置多久後讓系統睡眠（macOS 系統睡眠）",u"閒置多久後讓磁碟睡眠（macOS 磁碟睡眠）"};
                if(arg(3) && load_guid(arg(3))!=power::settings[i].value)return status(2);
                text=std::u16string(arg(3)?(friendly?setting_names[i]:setting_texts[i]):(friendly?group_names[i]:group_texts[i]));
            }else if(arg(3))return status(87);
            const auto bytes=utf16_bytes(text);const auto capacity=memory.load(GuestAddress{arg(5)},32);
            memory.store(GuestAddress{arg(5)},32,bytes.size());if(!arg(4))return status(0);if(capacity<bytes.size())return status(234);
            memory.write(GuestAddress{arg(4)},bytes);return status(0);
        }
        if(name=="PowerGetActiveScheme"){
            // (UserRootPowerKey, ActivePolicyGuid**)：GUID 以 LocalAlloc 語義配置，呼叫者以 LocalFree 釋放。
            if(arg(0) || !arg(1))return status(87);
            memory.store(GuestAddress{arg(1)},bits,0);
            Store store;if(const auto e=read_store(store))return status(e);
            if(store.active==power::native_scheme && (!runtime_.network.external_io || !native_power_preferences()))return status(50);
            if(gui_.global_memory.size()>=65536)return status(8);
            allocation=allocate_heap(memory,16);if(!allocation)return status(8);
            gui_.global_memory.emplace(allocation,GuiGlobalMemory{allocation,16,0,0,false});
            memory.write(GuestAddress{allocation},store.active);
            memory.store(GuestAddress{arg(1)},bits,allocation);return status(0);
        }
        if(name.starts_with("PowerRead")){
            // Index：(RootPowerKey, SchemeGuid, SubGroup, PowerSetting, ValueIndex*)；Value：(RootPowerKey, SchemeGuid, SubGroup, PowerSetting, Type*, Buffer, BufferSize*)
            const bool index=name.ends_with("Index"),dc=name.starts_with("PowerReadDC");
            if(arg(0))return status(87);
            if(!arg(3) || (index && !arg(4)) || (!index && arg(5) && !arg(6)))return status(87);
            Store store;if(const auto e=read_store(store))return status(e);
            const auto* scheme=find_scheme(store,arg(1)?load_guid(arg(1)):store.active);if(!scheme)return status(2);
            const auto position=setting_position(arg(2)?load_guid(arg(2)):Guid{},load_guid(arg(3)));
            if(position==power::settings.size())return status(2);
            const auto value=effective(*scheme,position,dc);if(value.status)return status(value.status);
            if(index){memory.store(GuestAddress{arg(4)},32,value.seconds);return status(0);}
            const auto capacity=arg(6)?memory.load(GuestAddress{arg(6)},32):0;
            if(arg(4))memory.check(GuestAddress{arg(4)},4,Access::Write);
            if(arg(6))memory.check(GuestAddress{arg(6)},4,Access::Write);
            if(arg(5) && capacity>=4)memory.check(GuestAddress{arg(5)},4,Access::Write);
            if(arg(4))memory.store(GuestAddress{arg(4)},32,4); // REG_DWORD
            if(arg(6))memory.store(GuestAddress{arg(6)},32,4);
            if(!arg(5))return status(0);if(capacity<4)return status(234);
            memory.store(GuestAddress{arg(5)},32,value.seconds);return status(0);
        }
    }catch(const MemoryFault&){if(allocation){gui_.global_memory.erase(allocation);(void)free_heap(memory,allocation,state);}return fail(998);}
    catch(const std::bad_alloc&){if(allocation){gui_.global_memory.erase(allocation);(void)free_heap(memory,allocation,state);}return fail(8);}
    throw HostCallFault("未知的電源方案服務");
}
}
