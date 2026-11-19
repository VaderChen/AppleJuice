#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <functional>

// advapi32／shlwapi 登錄補充：RegCopyTree、RegLoadMUIString 與 SHLoadIndirectString。
// 全部以既有的公開登錄與資源操作在宿主端組合（nested_service），共用登錄交易、日誌、通知與存取檢查。
namespace x64arm {
std::span<const WindowsServiceDescriptor> advapi32_extra_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"advapi32.dll","RegCopyTreeW",3},{"advapi32.dll","RegCopyTreeA",3},
        {"advapi32.dll","RegLoadMUIStringW",7,0,0,0x28},{"advapi32.dll","RegLoadMUIStringA",7,0,0,0x28},
        {"internal-shell-resource","SHLoadRegUIStringA",4,0,0,0x8},{"internal-shell-resource","SHLoadRegUIStringW",4,0,0,0x8},
        {"shlwapi.dll","SHLoadIndirectString",4,0,0,0x4},
        {"internal-shell-mime","RegisterMIMETypeForExtensionA",2},
        {"internal-shell-mime","RegisterMIMETypeForExtensionW",2},
        {"internal-shell-mime","UnregisterMIMETypeForExtensionA",1},
        {"internal-shell-mime","UnregisterMIMETypeForExtensionW",1},
        {"internal-shell-mime","RegisterExtensionForMIMETypeA",2},
        {"internal-shell-mime","RegisterExtensionForMIMETypeW",2},
        {"internal-shell-mime","UnregisterExtensionForMIMETypeA",1},
        {"internal-shell-mime","UnregisterExtensionForMIMETypeW",1},
        {"internal-shell-mime","GetMIMETypeSubKeyA",3,0,0,4},
        {"internal-shell-mime","GetMIMETypeSubKeyW",3,0,0,4},
        {"internal-shell-mime","MIME_GetExtensionA",3,0,0,4},
        {"internal-shell-mime","MIME_GetExtensionW",3,0,0,4},
        // shlwapi 的 SH* 登錄輔助：以 advapi32 的對應操作組合。
        {"shlwapi.dll","SHQueryInfoKeyA",5},{"shlwapi.dll","SHQueryInfoKeyW",5},{"shlwapi.dll","SHEnumKeyExA",4,0,0,0x2},{"shlwapi.dll","SHEnumKeyExW",4,0,0,0x2},
        {"shlwapi.dll","SHEnumValueA",7,0,0,0x2},{"shlwapi.dll","SHEnumValueW",7,0,0,0x2},{"shlwapi.dll","SHQueryValueExA",6},{"shlwapi.dll","SHQueryValueExW",6},
        {"shlwapi.dll","SHDeleteEmptyKeyA",2},{"shlwapi.dll","SHDeleteEmptyKeyW",2},{"shlwapi.dll","SHDeleteOrphanKeyA",2},{"shlwapi.dll","SHDeleteOrphanKeyW",2},
        {"shlwapi.dll","SHCopyKeyA",4},{"shlwapi.dll","SHCopyKeyW",4},{"shlwapi.dll","SHRegDuplicateHKey",1},
        // 使用者／系統（US）登錄：HUSKEY 同時持有 HKCU 與 HKLM 的機碼，查詢先 HKCU 後 HKLM，寫入依 SHREGSET 旗標。
        {"shlwapi.dll","SHRegOpenUSKeyA",5},{"shlwapi.dll","SHRegOpenUSKeyW",5},{"shlwapi.dll","SHRegCreateUSKeyA",5},{"shlwapi.dll","SHRegCreateUSKeyW",5},{"shlwapi.dll","SHRegQueryUSValueA",8},{"shlwapi.dll","SHRegQueryUSValueW",8},{"shlwapi.dll","SHRegWriteUSValueA",6},{"shlwapi.dll","SHRegWriteUSValueW",6},{"shlwapi.dll","SHRegSetUSValueA",6},{"shlwapi.dll","SHRegSetUSValueW",6},{"shlwapi.dll","SHRegDeleteUSValueA",3},{"shlwapi.dll","SHRegDeleteUSValueW",3},{"shlwapi.dll","SHRegDeleteEmptyUSKeyA",3},{"shlwapi.dll","SHRegDeleteEmptyUSKeyW",3},{"shlwapi.dll","SHRegEnumUSKeyA",5},{"shlwapi.dll","SHRegEnumUSKeyW",5},{"shlwapi.dll","SHRegEnumUSValueA",8},{"shlwapi.dll","SHRegEnumUSValueW",8},{"shlwapi.dll","SHRegQueryInfoUSKeyA",6},{"shlwapi.dll","SHRegQueryInfoUSKeyW",6},{"shlwapi.dll","SHRegGetBoolUSValueA",4},{"shlwapi.dll","SHRegGetBoolUSValueW",4},{"shlwapi.dll","SHRegGetPathA",5},{"shlwapi.dll","SHRegGetPathW",5},{"shlwapi.dll","SHRegSetPathA",5},{"shlwapi.dll","SHRegSetPathW",5},{"shlwapi.dll","SHRegCloseUSKey",1},{"shlwapi.dll","PathUnExpandEnvStringsA",3},{"shlwapi.dll","PathUnExpandEnvStringsW",3},{"shlwapi.dll","SHRegGetIntW",3},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_advapi32_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto saved_error=last_error_;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){last_error_=saved_error;arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto host=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,family,symbol,args);};
    const auto reg=[&](std::string_view symbol,std::initializer_list<std::uint64_t> args){return static_cast<std::uint32_t>(host("advapi32.dll",symbol,args));};
    GuestScratch temporary(*this,memory,state);
    const auto allocate=[&](std::size_t size)->std::uint64_t{const auto address=temporary.allocate(size);if(!address)throw std::bad_alloc();return address;};
    const auto wide_copy=[&](const std::u16string& text)->std::uint64_t{const auto address=temporary.text(text);if(!address)throw std::bad_alloc();return address;};
    // "@[路徑],-資源識別碼[;註解]"：以資料檔方式載入模組並讀出字串資源。
    const auto indirect=[&](std::u16string text,std::u16string& result)->std::uint32_t{
        if(text.empty() || text.front()!=u'@'){result=std::move(text);return 0;}
        text.erase(0,1);if(const auto comment=text.find(u';');comment!=text.npos)text.resize(comment);
        const auto comma=text.rfind(u',');if(comma==text.npos || comma+2>=text.size() || text[comma+1]!=u'-')return 87;
        std::uint32_t id=0;for(std::size_t i=comma+2;i<text.size();++i){if(text[i]<u'0' || text[i]>u'9')return 87;id=id*10+static_cast<std::uint32_t>(text[i]-u'0');if(id>65535)return 87;}
        auto path=text.substr(0,comma);
        const auto source=wide_copy(path),expanded=allocate(32768*2);
        const auto chars=static_cast<std::uint32_t>(host("kernel32.dll","ExpandEnvironmentStringsW",{source,expanded,32768}));if(!chars || chars>32768)return 87;
        auto module=host("kernel32.dll","LoadLibraryExW",{expanded,0,0x22});if(x86_)module&=UINT32_MAX;if(!module)return last_error_?last_error_:2;
        const auto output=allocate(65536);
        const auto length=static_cast<std::uint32_t>(host("user32.dll","LoadStringW",{module,id,output,32767}));const auto error=last_error_;
        (void)host("kernel32.dll","FreeLibrary",{module});
        if(!length)return error?error:1814; // ERROR_RESOURCE_NAME_NOT_FOUND
        result=read_utf16_counted(memory,GuestAddress{output},length);return 0;
    };
    // 完整路徑開頭的資料夾換回環境變數（%ALLUSERSPROFILE%、%APPDATA%、%USERPROFILE%、%ProgramFiles%、%SystemRoot%、%SystemDrive%、%COMPUTERNAME%），取最長的相符者；沒有可換的回 nullopt。
    const auto unexpand=[&](const std::u16string& path)->std::optional<std::u16string>{
        std::optional<std::u16string> best;std::size_t best_length=0;
        for(const auto variable:{u"ALLUSERSPROFILE",u"APPDATA",u"USERPROFILE",u"ProgramFiles",u"SystemRoot",u"SystemDrive",u"COMPUTERNAME"}){
            const auto name_text=wide_copy(variable),value=allocate(1040);const auto length=static_cast<std::uint32_t>(host("kernel32.dll","GetEnvironmentVariableW",{name_text,value,520}));
            if(!length || length>=520)continue;auto folder=read_utf16(memory,GuestAddress{value},520);while(folder.size()>1 && folder.back()==u'\\')folder.pop_back();
            if(folder.size()<=best_length || path.size()<folder.size())continue;
            bool same=true;for(std::size_t i=0;i<folder.size() && same;++i)same=unicode_case_unit(path[i],false)==unicode_case_unit(folder[i],false);
            if(!same || (path.size()>folder.size() && path[folder.size()]!=u'\\'))continue;
            best=u"%"+std::u16string(variable)+u"%"+path.substr(folder.size());best_length=folder.size();
        }
        return best;};
    try{
        if(service.family=="internal-shell-mime"){
            const auto mime=[&]<typename Char>(Char){
                using Text=std::basic_string<Char>;
                constexpr bool wide=sizeof(Char)==2;
                const auto encoded=[](std::string_view ascii){return Text(ascii.begin(),ascii.end());};
                const auto read=[&](std::uint64_t address)->Text{
                    if constexpr(wide)return read_utf16(memory,GuestAddress{address});
                    else return read_ansi(memory,address,32768);
                };
                const auto write=[&](std::uint64_t address,const Text& text){
                    memory.write(GuestAddress{address},std::span(reinterpret_cast<const std::uint8_t*>(text.c_str()),(text.size()+1)*sizeof(Char)));
                };
                const auto copy=[&](const Text& text){
                    const auto at=allocate((text.size()+1)*sizeof(Char));write(at,text);return at;
                };
                const auto shell=[&](std::string symbol,std::initializer_list<std::uint64_t> args){
                    return host("shlwapi.dll",symbol+(wide?'W':'A'),args);
                };
                constexpr std::uint64_t classes=0x80000000U;
                if(name.starts_with("RegisterMIMETypeForExtension") || name.starts_with("UnregisterMIMETypeForExtension")){
                    const auto label=copy(encoded("Content Type"));
                    if(name.starts_with("Unregister"))return shell("SHDeleteValue",{classes,arg(0),label})==0;
                    if(!arg(1))return false;
                    return shell("SHSetValue",{classes,arg(0),label,1,arg(1),(read(arg(1)).size()+1)*sizeof(Char)})==0;
                }
                const bool lookup=name.starts_with("MIME_GetExtension");
                const bool registration=name.starts_with("RegisterExtension");
                const auto type=arg(registration?1:0);
                const auto capacity=service.arguments==3?static_cast<std::uint32_t>(arg(2)):0U;
                if(lookup){
                    if(!arg(1) || static_cast<std::int32_t>(capacity)<=0)return false;
                    memory.store(GuestAddress{arg(1)},sizeof(Char)*8,0);
                    if(capacity<=2)return false;
                }
                if(!type)return false;
                const Text path=encoded("MIME\\Database\\Content Type\\")+read(type);
                if(name.starts_with("GetMIMETypeSubKey")){
                    if(!arg(1) || path.size()>=capacity)return false;
                    write(arg(1),path);return true;
                }
                if(path.size()>=260)return false;
                const auto key=copy(path),label=copy(encoded("Extension"));
                if(registration){
                    if(!arg(0))return false;
                    return shell("SHSetValue",{classes,key,label,1,arg(0),(read(arg(0)).size()+1)*sizeof(Char)})==0;
                }
                if(!lookup){
                    if(shell("SHDeleteValue",{classes,key,label}))return false;
                    (void)shell("SHDeleteEmptyKey",{classes,key});
                    return true;
                }
                const auto size=allocate(4),kind=allocate(4);
                memory.store(GuestAddress{size},32,0);
                if(shell("SHGetValue",{classes,key,label,kind,0,size}) || memory.load(GuestAddress{kind},32)!=1)return false;
                const auto bytes=memory.load(GuestAddress{size},32);
                if(!bytes || bytes%sizeof(Char) || bytes/sizeof(Char)>capacity-1)return false;
                const auto value=allocate(bytes+sizeof(Char));
                memory.store(GuestAddress{value+bytes},sizeof(Char)*8,0);
                if(shell("SHGetValue",{classes,key,label,kind,value,size}) || memory.load(GuestAddress{kind},32)!=1)return false;
                auto extension=read(value);
                if(extension.empty())return false;
                if(extension.front()!=Char('.'))extension.insert(extension.begin(),Char('.'));
                if(extension.size()>=capacity)return false;
                write(arg(1),extension);return true;
            };
            return done(name.ends_with('W')?mime(char16_t{}):mime(char{}));
        }
        if(name=="PathUnExpandEnvStringsA" || name=="PathUnExpandEnvStringsW"){
            // (path, buffer, cch)：沒有可換的或緩衝不足回 FALSE。
            const bool wide=name.ends_with('W');if(!arg(0) || !arg(1))return done(0);
            const auto path=wide?read_utf16(memory,GuestAddress{arg(0)},32767):utf8_to_utf16_lossy(read_ansi(memory,arg(0),32767));const auto result=unexpand(path);if(!result)return done(0);
            if(wide){if(result->size()+1>static_cast<std::uint32_t>(arg(2)))return done(0);for(std::size_t i=0;i<result->size();++i)memory.store(GuestAddress{arg(1)+i*2},16,(*result)[i]);memory.store(GuestAddress{arg(1)+result->size()*2},16,0);return done(1);}
            const auto narrow=utf16_to_utf8_lossy(*result);if(narrow.size()+1>static_cast<std::uint32_t>(arg(2)))return done(0);
            memory.write(GuestAddress{arg(1)},std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(narrow.c_str()),narrow.size()+1));return done(1);
        }
        if(name.starts_with("SHReg") && (name.find("US")!=name.npos || name.starts_with("SHRegGetPath") || name.starts_with("SHRegSetPath") || name=="SHRegGetIntW")){
            const bool wide=name.back()!='A';const char suffix=wide?'W':'A';
            const auto call_reg=[&](std::string_view base,std::initializer_list<std::uint64_t> args){return reg(std::string(base)+suffix,args);};
            const auto key_of=[&](std::uint64_t handle)->ShellUsKey*{const auto found=shell_us_keys_.find(handle);return found==shell_us_keys_.end()?nullptr:&found->second;};
            const auto open=[&](std::uint64_t root,std::uint64_t path,std::uint32_t access,bool create)->std::uint64_t{
                if(!root)return 0;const auto slot=allocate(8);memory.store(GuestAddress{slot},bits,0);
                const auto error=create?call_reg("RegCreateKeyEx",{root,path,0,0,0,access,0,slot,0}):call_reg("RegOpenKeyEx",{root,path,0,access,slot});
                return error?0:memory.load(GuestAddress{slot},bits);};
            const auto close_us=[&](std::uint64_t handle){const auto found=shell_us_keys_.find(handle);if(found==shell_us_keys_.end())return false;
                if(found->second.user)(void)reg("RegCloseKey",{found->second.user});if(found->second.machine)(void)reg("RegCloseKey",{found->second.machine});
                shell_us_keys_.erase(found);(void)free_heap(memory,handle,state);return true;};
            const auto new_handle=[&](ShellUsKey key)->std::uint64_t{const auto handle=allocate_heap(memory,8);if(!handle){if(key.user)(void)reg("RegCloseKey",{key.user});if(key.machine)(void)reg("RegCloseKey",{key.machine});return 0;}shell_us_keys_[handle]=key;return handle;};
            // 相對 HUSKEY 時以其兩個機碼為根，否則以 HKCU／HKLM 為根。
            const auto roots=[&](std::uint64_t relative)->std::optional<std::pair<std::uint64_t,std::uint64_t>>{
                if(!relative)return std::pair<std::uint64_t,std::uint64_t>{x86_?0x80000001ULL:0xffffffff80000001ULL,x86_?0x80000002ULL:0xffffffff80000002ULL};
                const auto* key=key_of(relative);if(!key)return std::nullopt;return std::pair{key->user,key->machine};};
            if(name.starts_with("SHRegOpenUSKey") || name.starts_with("SHRegCreateUSKey")){
                // Open(path, sam, relative, out, ignoreHKCU)：至少一邊開啟即成功。Create(path, sam, relative, out, flags)：依 SHREGSET_HKCU／FORCE_HKCU（1／2）與 HKLM／FORCE_HKLM（4／8）建立。
                const bool create=name.starts_with("SHRegCreateUSKey");if(!arg(3))return done(87);memory.store(GuestAddress{arg(3)},bits,0);
                const auto base=roots(arg(2));if(!base)return done(6);const auto access=static_cast<std::uint32_t>(arg(1));const auto flags=static_cast<std::uint32_t>(arg(4));
                ShellUsKey key;
                if(create){if(flags&3U)key.user=open(base->first,arg(0),access,true);if(flags&12U)key.machine=open(base->second,arg(0),access,true);}
                else {if(!flags)key.user=open(base->first,arg(0),access,false);key.machine=open(base->second,arg(0),access,false);}
                if(!key.user && !key.machine)return done(2);const auto handle=new_handle(key);if(!handle)return done(8);memory.store(GuestAddress{arg(3)},bits,handle);return done(0);
            }
            if(name=="SHRegCloseUSKey")return done(close_us(arg(0))?0U:6U);
            if(name.starts_with("SHRegQueryUSValue")){
                // (key, value, type*, data, size*, ignoreHKCU, default, defaultSize)：先 HKCU 後 HKLM，都沒有時用預設資料。
                const auto* key=key_of(arg(0));if(!key)return done(6);const auto capacity=arg(4)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)},32)):0U;
                std::uint32_t error=2;
                for(const auto hive:{static_cast<std::uint32_t>(arg(5))?0ULL:key->user,key->machine}){if(!hive)continue;if(arg(4))memory.store(GuestAddress{arg(4)},32,capacity);
                    error=call_reg("RegQueryValueEx",{hive,arg(1),0,arg(2),arg(3),arg(4)});if(error!=2)break;}
                if(error==2 && arg(6) && arg(7) && arg(4)){
                    const auto count=std::min(capacity,static_cast<std::uint32_t>(arg(7)));std::vector<std::uint8_t> bytes(count);memory.read(GuestAddress{arg(6)},bytes);
                    if(count && arg(3))memory.write(GuestAddress{arg(3)},bytes);memory.store(GuestAddress{arg(4)},32,count);error=0;}
                return done(error);
            }
            const auto write_value=[&](const ShellUsKey& key,std::uint64_t value,std::uint32_t type,std::uint64_t data,std::uint32_t size,std::uint32_t flags)->std::uint32_t{
                // SHREGSET_HKCU（1）／HKLM（4）只在值不存在時寫入，FORCE（2／8）一律寫入。
                std::uint32_t error=0;bool wrote=false;
                for(const auto& [hive,soft,force]:{std::tuple{key.user,1U,2U},std::tuple{key.machine,4U,8U}}){
                    if(!(flags&(soft|force)))continue;if(!hive){error=6;continue;}
                    if(!(flags&force) && call_reg("RegQueryValueEx",{hive,value,0,0,0,0})==0){wrote=true;continue;}
                    if((error=call_reg("RegSetValueEx",{hive,value,0,type,data,size})))return error;wrote=true;}
                return wrote?0:error?error:87;};
            if(name.starts_with("SHRegWriteUSValue")){const auto* key=key_of(arg(0));if(!key)return done(6);return done(write_value(*key,arg(1),static_cast<std::uint32_t>(arg(2)),arg(3),static_cast<std::uint32_t>(arg(4)),static_cast<std::uint32_t>(arg(5))));}
            if(name.starts_with("SHRegSetUSValue")){
                // (subkey, value, type, data, size, flags)：依旗標建立機碼後寫入。
                const auto flags=static_cast<std::uint32_t>(arg(5));const auto base=*roots(0);ShellUsKey key;
                if(flags&3U)key.user=open(base.first,arg(0),0xf003fU,true);if(flags&12U)key.machine=open(base.second,arg(0),0xf003fU,true);
                if(!key.user && !key.machine)return done(flags?5U:87U);const auto error=write_value(key,arg(1),static_cast<std::uint32_t>(arg(2)),arg(3),static_cast<std::uint32_t>(arg(4)),flags);
                if(key.user)(void)reg("RegCloseKey",{key.user});if(key.machine)(void)reg("RegCloseKey",{key.machine});return done(error);
            }
            // SHREGDEL／SHREGENUM 旗標：DEFAULT 0（HKCU，沒有時 HKLM）、HKCU 1、HKLM 0x10、BOTH 0x11。
            const auto hives=[&](const ShellUsKey& key,std::uint32_t flags){std::vector<std::uint64_t> out;
                if(flags==0 || (flags&1U))if(key.user)out.push_back(key.user);if(flags==0 || (flags&0x10U))if(key.machine)out.push_back(key.machine);return out;};
            if(name.starts_with("SHRegDeleteUSValue") || name.starts_with("SHRegDeleteEmptyUSKey")){
                const auto* key=key_of(arg(0));if(!key)return done(6);const auto flags=static_cast<std::uint32_t>(arg(2));const bool value=name.starts_with("SHRegDeleteUSValue");
                std::uint32_t error=2;
                for(const auto hive:hives(*key,flags)){
                    std::uint32_t result=0;
                    if(value)result=call_reg("RegDeleteValue",{hive,arg(1)});
                    else {const auto child=open(hive,arg(1),0x20019U,false);if(!child){result=2;}
                        else {const auto info=allocate(8);(void)call_reg("RegQueryInfoKey",{child,0,0,0,info,0,0,info+4,0,0,0,0});(void)reg("RegCloseKey",{child});
                            result=memory.load(GuestAddress{info},32) || memory.load(GuestAddress{info+4},32)?1020U:call_reg("RegDeleteKey",{hive,arg(1)});}}
                    if(result==0){error=0;if(flags==0)break;}else if(error!=0 && result!=2)error=result;
                }
                return done(error);
            }
            if(name.starts_with("SHRegEnumUSKey") || name.starts_with("SHRegEnumUSValue") || name.starts_with("SHRegQueryInfoUSKey")){
                const auto* key=key_of(arg(0));if(!key)return done(6);const bool info=name.starts_with("SHRegQueryInfoUSKey");
                const auto flags=static_cast<std::uint32_t>(info?arg(5):name.starts_with("SHRegEnumUSKey")?arg(4):arg(7));if(flags==0x11U)return done(120);
                const auto chosen=hives(*key,flags);if(chosen.empty())return done(6);const auto hive=chosen.front();
                if(info)return done(call_reg("RegQueryInfoKey",{hive,0,0,0,arg(1),arg(2),0,arg(3),arg(4),0,0,0}));
                if(name.starts_with("SHRegEnumUSKey"))return done(call_reg("RegEnumKeyEx",{hive,arg(1),arg(2),arg(3),0,0,0,0}));
                return done(call_reg("RegEnumValue",{hive,arg(1),arg(2),arg(3),0,arg(4),arg(5),arg(6)}));
            }
            if(name.starts_with("SHRegGetBoolUSValue")){
                // 字串 "yes"／"true"／"1" 為真、"no"／"false"／"0" 為假；DWORD 非零為真；讀不到或無法判斷時用預設值。
                const auto data=allocate(64),size=allocate(8),type=size+4;memory.store(GuestAddress{size},32,62);memory.fill(GuestAddress{data},0,64);
                const auto error=static_cast<std::uint32_t>(host("shlwapi.dll",wide?"SHRegGetUSValueW":"SHRegGetUSValueA",{arg(0),arg(1),type,data,size,arg(2),0,0}));
                const bool fallback=static_cast<std::uint32_t>(arg(3))!=0;if(error)return done(fallback);
                const auto kind=static_cast<std::uint32_t>(memory.load(GuestAddress{type},32));
                if(kind==4 || kind==3)return done(memory.load(GuestAddress{data},32)!=0);
                if(kind!=1 && kind!=2)return done(fallback);
                auto text=wide?read_utf16(memory,GuestAddress{data},31):utf8_to_utf16_lossy(read_ansi(memory,data,62));for(auto& c:text)c=unicode_case_unit(c,false);
                if(text==u"yes" || text==u"true" || text==u"1")return done(1);if(text==u"no" || text==u"false" || text==u"0")return done(0);return done(fallback);
            }
            if(name=="SHRegGetIntW"){
                // 沿用 Shell 查值的環境變數展開，以及本引擎的 DWORD 與帶號十進位支援。
                constexpr unsigned capacity=32*sizeof(char16_t);
                const auto data=allocate(capacity+2),size=allocate(8),type=size+4;
                memory.fill(GuestAddress{data},0,capacity+2);
                memory.store(GuestAddress{size},32,capacity);
                if(host("shlwapi.dll","SHQueryValueExW",{arg(0),arg(1),0,type,data,size}))return done(arg(2));
                const auto kind=memory.load(GuestAddress{type},32);
                if(kind==4 && memory.load(GuestAddress{size},32)==4)return done(memory.load(GuestAddress{data},32));
                if(kind!=1)return done(arg(2));
                const auto text=read_utf16(memory,GuestAddress{data},33);
                const bool negative=!text.empty() && text.front()==u'-';
                std::size_t i=negative || (!text.empty() && text.front()==u'+') ? 1 : 0;
                if(i==text.size() || text[i]<u'0' || text[i]>u'9')return done(arg(2));
                const std::uint64_t limit=negative?0x80000000ULL:0x7fffffffULL;
                std::uint64_t value=0;
                for(;i<text.size() && text[i]>=u'0' && text[i]<=u'9';++i)
                    value=std::min(limit,value*10+text[i]-u'0');
                return done(static_cast<std::uint32_t>(negative?-value:value));
            }
            if(name.starts_with("SHRegGetPath")){
                // (hkey, subkey, value, path[MAX_PATH], flags)：開啟子機碼後經 SHQueryValueEx 讀入（REG_EXPAND_SZ 展開成 REG_SZ）。
                if(!arg(3))return done(87);const auto key=arg(1)?open(arg(0),arg(1),0x20019U,false):arg(0);if(!key)return done(2);
                const auto size=allocate(8);memory.store(GuestAddress{size},32,wide?520U:260U);
                const auto error=static_cast<std::uint32_t>(host("shlwapi.dll",wide?"SHQueryValueExW":"SHQueryValueExA",{key,arg(2),0,0,arg(3),size}));
                if(arg(1))(void)reg("RegCloseKey",{key});return done(error);
            }
            if(name.starts_with("SHRegSetPath")){
                // (hkey, subkey, value, path, flags)：可還原成環境變數形式時存成 REG_EXPAND_SZ，否則 REG_SZ。
                if(!arg(3))return done(87);const auto text=wide?read_utf16(memory,GuestAddress{arg(3)},260):utf8_to_utf16_lossy(read_ansi(memory,arg(3),260));
                std::uint32_t type=1;auto stored=text;if(const auto shorter=unexpand(text)){stored=*shorter;type=2;}
                const auto bytes=wide_copy(stored);
                if(wide)return done(reg("RegSetKeyValueW",{arg(0),arg(1),arg(2),type,bytes,(stored.size()+1)*2}));
                const auto narrow=utf16_to_utf8_lossy(stored);const auto ansi=allocate(narrow.size()+1);memory.write(GuestAddress{ansi},std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(narrow.c_str()),narrow.size()+1));
                return done(reg("RegSetKeyValueA",{arg(0),arg(1),arg(2),type,ansi,narrow.size()+1}));
            }
        }
        if(name.starts_with("SHLoadRegUIString")){
            const bool wide=name.ends_with('W');
            const auto capacity=static_cast<std::uint32_t>(arg(3));
            if(!arg(2) || !capacity || (wide && capacity>UINT32_MAX/2))return done(0x80004005U);
            const auto bytes=allocate(4);
            memory.store(GuestAddress{bytes},32,capacity*(wide?2U:1U));
            if(reg(wide?"RegQueryValueExW":"RegQueryValueExA",{arg(0),arg(1),0,0,arg(2),bytes}))
                return done(0x80004005U);
            const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{bytes},32));
            std::u16string source;
            if(wide)source=read_utf16_counted(memory,GuestAddress{arg(2)},count/2);
            else{
                std::string raw(count,'\0');
                memory.read(GuestAddress{arg(2)},std::span(reinterpret_cast<std::uint8_t*>(raw.data()),raw.size()));
                source=utf8_to_utf16_lossy(raw);
            }
            if(const auto end=source.find(u'\0');end!=source.npos)source.resize(end);
            std::u16string value;
            if(const auto error=indirect(std::move(source),value))return done(0x80070000U|(error&0xffffU));
            if(wide){
                if(value.size()>=capacity)value.resize(capacity-1);
                memory.write(GuestAddress{arg(2)},utf16_bytes(value));
            }else{
                auto text=utf16_to_utf8(value);
                if(text.size()>=capacity){
                    std::size_t end=capacity-1;
                    while(end && (static_cast<unsigned char>(text[end])&0xc0)==0x80)--end;
                    text.resize(end);
                }
                memory.write(GuestAddress{arg(2)},std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(text.c_str()),text.size()+1));
            }
            return done(0);
        }
        if(name.starts_with("SH") && name!="SHLoadIndirectString"){
            // A／W 版直接交給 advapi32 的同類 A／W 函式，字串不另轉碼。
            const char suffix=name.back()=='A'?'A':'W';
            const auto call_reg=[&](std::string_view base,std::initializer_list<std::uint64_t> args){const std::string symbol=std::string(base)+suffix;return reg(symbol,args);};
            if(name.starts_with("SHQueryInfoKey"))return done(call_reg("RegQueryInfoKey",{arg(0),0,0,0,arg(1),arg(2),0,arg(3),arg(4),0,0,0}));
            if(name.starts_with("SHEnumKeyEx"))return done(call_reg("RegEnumKeyEx",{arg(0),arg(1),arg(2),arg(3),0,0,0,0}));
            if(name.starts_with("SHEnumValue"))return done(call_reg("RegEnumValue",{arg(0),arg(1),arg(2),arg(3),0,arg(4),arg(5),arg(6)}));
            if(name.starts_with("SHQueryValueEx")){
                // REG_EXPAND_SZ 會展開成 REG_SZ；緩衝區不足時回 ERROR_MORE_DATA 並寫回展開後需要的位元組數。
                const auto type_slot=allocate(16);const auto size_slot=type_slot+8;
                const auto capacity=arg(5)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(5)},32)):0U;
                memory.store(GuestAddress{size_slot},32,0);
                auto error=call_reg("RegQueryValueEx",{arg(0),arg(1),0,type_slot,0,size_slot});if(error)return done(error);
                const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{type_slot},32));const auto raw_size=static_cast<std::uint32_t>(memory.load(GuestAddress{size_slot},32));
                if(type!=2){
                    error=call_reg("RegQueryValueEx",{arg(0),arg(1),0,arg(3),arg(4),arg(5)});return done(error);
                }
                const auto raw=allocate(raw_size+4);memory.store(GuestAddress{size_slot},32,raw_size);
                if((error=call_reg("RegQueryValueEx",{arg(0),arg(1),0,0,raw,size_slot})))return done(error);
                const auto expanded=allocate(65536);const bool wide_text=suffix=='W';
                const auto chars=static_cast<std::uint32_t>(host("kernel32.dll",wide_text?"ExpandEnvironmentStringsW":"ExpandEnvironmentStringsA",{raw,expanded,wide_text?32768U:65535U}));
                if(!chars)return done(last_error_?last_error_:13);
                const auto needed=chars*(wide_text?2U:1U);
                if(arg(3))memory.store(GuestAddress{arg(3)},32,1);
                if(arg(5))memory.store(GuestAddress{arg(5)},32,needed);
                if(!arg(4))return done(0);
                if(capacity<needed)return done(234);
                std::vector<std::uint8_t> bytes(needed);memory.read(GuestAddress{expanded},bytes);memory.write(GuestAddress{arg(4)},bytes);return done(0);
            }
            if(name.starts_with("SHDeleteEmptyKey") || name.starts_with("SHDeleteOrphanKey")){
                // EmptyKey：沒有子機碼也沒有值才刪除；OrphanKey：沒有子機碼且沒有預設值才刪除。不符合時回 ERROR_KEY_HAS_CHILDREN。
                const auto slot=allocate(32);
                if(const auto error=call_reg("RegOpenKeyEx",{arg(0),arg(1),0,0x20019U,slot}))return done(error);
                const auto key=memory.load(GuestAddress{slot},bits);
                (void)call_reg("RegQueryInfoKey",{key,0,0,0,slot+8,0,0,slot+12,0,0,0,0});
                const auto subkeys=memory.load(GuestAddress{slot+8},32),values=memory.load(GuestAddress{slot+12},32);
                bool removable=!subkeys && !values;
                if(name.starts_with("SHDeleteOrphanKey")){memory.store(GuestAddress{slot+16},32,0);removable=!subkeys && call_reg("RegQueryValueEx",{key,0,0,0,0,slot+16})!=0;}
                (void)reg("RegCloseKey",{key});
                if(!removable)return done(1020);
                return done(call_reg("RegDeleteKey",{arg(0),arg(1)}));
            }
            if(name.starts_with("SHCopyKey"))return done(call_reg("RegCopyTree",{arg(0),arg(1),arg(2)}));
            if(name=="SHRegDuplicateHKey"){
                // 以 MAXIMUM_ALLOWED 重新開啟同一機碼，回傳新的 HKEY（失敗回 NULL）。
                const auto slot=allocate(16);if(reg("RegOpenKeyExW",{arg(0),0,0,0x02000000U,slot}))return done(0);
                return done(memory.load(GuestAddress{slot},bits));
            }
        }
        if(name=="RegCopyTreeW" || name=="RegCopyTreeA"){
            // (Source, SubKey 可為 NULL, Destination)：遞迴複製值與子機碼到目的機碼；目的已有的同名值會被覆寫。
            std::uint64_t subkey=0;
            if(arg(1)){const auto text=name.ends_with('W')?read_utf16(memory,GuestAddress{arg(1)},32767):utf8_to_utf16_lossy(read_ansi(memory,arg(1),65534));subkey=wide_copy(text);}
            const auto handles=allocate(4*16);
            std::uint64_t source=arg(0);bool opened=false;
            if(subkey){if(const auto error=reg("RegOpenKeyExW",{arg(0),subkey,0,0x20019U,handles}))return done(error);source=memory.load(GuestAddress{handles},bits);opened=true;}
            const auto name_buffer=allocate(16384*2+2),counter=allocate(16);
            std::uint64_t data_buffer=allocate(4096);std::uint32_t data_capacity=4096;
            std::function<std::uint32_t(std::uint64_t,std::uint64_t,unsigned)> copy=[&](std::uint64_t from,std::uint64_t to,unsigned depth)->std::uint32_t{
                if(depth>512)return 1010; // ERROR_BADKEY：異常深的樹
                for(std::uint32_t index=0;;++index){
                    memory.store(GuestAddress{counter},32,16384);memory.store(GuestAddress{counter+4},32,data_capacity);
                    auto error=reg("RegEnumValueW",{from,index,name_buffer,counter,0,counter+8,data_buffer,counter+4});
                    if(error==234){const auto needed=static_cast<std::uint32_t>(memory.load(GuestAddress{counter+4},32));data_capacity=std::max(needed,data_capacity*2);data_buffer=allocate(data_capacity);--index;continue;}
                    if(error==259)break;if(error)return error;
                    error=reg("RegSetValueExW",{to,name_buffer,0,memory.load(GuestAddress{counter+8},32),data_buffer,memory.load(GuestAddress{counter+4},32)});if(error)return error;
                }
                for(std::uint32_t index=0;;++index){
                    memory.store(GuestAddress{counter},32,16384);
                    auto error=reg("RegEnumKeyExW",{from,index,name_buffer,counter,0,0,0,0});
                    if(error==259)break;if(error)return error;
                    const auto child_name=wide_copy(read_utf16(memory,GuestAddress{name_buffer},16384));
                    const auto slot=allocate(16);
                    if((error=reg("RegOpenKeyExW",{from,child_name,0,0x20019U,slot})))return error;
                    const auto child=memory.load(GuestAddress{slot},bits);
                    error=reg("RegCreateKeyExW",{to,child_name,0,0,0,0xf003fU,0,slot+8,0});
                    if(error){(void)reg("RegCloseKey",{child});return error;}
                    const auto target=memory.load(GuestAddress{slot+8},bits);
                    error=copy(child,target,depth+1);(void)reg("RegCloseKey",{child});(void)reg("RegCloseKey",{target});if(error)return error;
                }
                return 0;
            };
            const auto error=copy(source,arg(2),0);if(opened)(void)reg("RegCloseKey",{source});
            return done(error);
        }
        if(name=="RegLoadMUIStringW" || name=="RegLoadMUIStringA"){
            // (Key, Value, OutBuf, OutBufBytes, Data*, Flags, Directory)：REG_SZ／REG_EXPAND_SZ；"@檔案,-識別碼" 由模組字串資源取得。
            // Flags 的 REG_MUI_STRING_TRUNCATE（1）允許截斷；否則緩衝區不足回 ERROR_MORE_DATA 並寫回需要的位元組數。ANSI 版與 Windows 相同不支援。
            if(name.ends_with('A'))return done(120); // ERROR_CALL_NOT_IMPLEMENTED（Windows 的 ANSI 版即如此）
            const auto flags=static_cast<std::uint32_t>(arg(5));const auto capacity=static_cast<std::uint32_t>(arg(3));
            if(flags&~1U || (flags&1U && arg(4)) || (!arg(2) && capacity) || capacity%2)return done(87);
            const auto slot=allocate(16);memory.store(GuestAddress{slot},32,0);
            auto error=reg("RegQueryValueExW",{arg(0),arg(1),0,slot+8,0,slot});if(error)return done(error);
            const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{slot+8},32));if(type!=1 && type!=2)return done(13); // ERROR_INVALID_DATA
            const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{slot},32));const auto raw=allocate(size+4);
            if((error=reg("RegQueryValueExW",{arg(0),arg(1),0,slot+8,raw,slot})))return done(error);
            auto text=read_utf16(memory,GuestAddress{raw},size/2);
            if(type==2){const auto source=wide_copy(text),expanded=allocate(65536);
                const auto chars=static_cast<std::uint32_t>(host("kernel32.dll","ExpandEnvironmentStringsW",{source,expanded,32768}));if(chars && chars<=32768)text=read_utf16_counted(memory,GuestAddress{expanded},chars-1);}
            std::u16string result;if((error=indirect(text,result)))return done(error);
            const auto needed=static_cast<std::uint32_t>(result.size()*2+2);
            if(arg(4))memory.store(GuestAddress{arg(4)},32,needed);
            if(capacity<needed){
                if(!(flags&1U) || !capacity)return done(234);
                result.resize(capacity/2-1);
            }
            memory.write(GuestAddress{arg(2)},utf16_bytes(result));return done(0);
        }
        if(name=="SHLoadIndirectString"){
            // (Source, OutBuf, OutBufChars, Reserved)：非 "@" 開頭的字串原樣複製；回 HRESULT。
            if(!arg(0) || !arg(1) || !static_cast<std::uint32_t>(arg(2)))return done(0x80070057U);
            std::u16string result;
            if(const auto error=indirect(read_utf16(memory,GuestAddress{arg(0)},32767),result))return done(0x80070000U|(error&0xffffU));
            const auto capacity=static_cast<std::uint32_t>(arg(2));if(result.size()>=capacity)result.resize(capacity-1);
            memory.write(GuestAddress{arg(1)},utf16_bytes(result));return done(0);
        }
    }catch(const MemoryFault&){return done(name.starts_with("SHLoad")?0x800703e6U:998U);}
    catch(const std::bad_alloc&){return done(name.starts_with("SHLoad")?0x8007000eU:8U);}
    (void)unit;
    throw HostCallFault("未知的 advapi32 補充服務");
}
}
