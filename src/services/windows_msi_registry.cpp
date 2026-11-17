#include "service_callback_task.hpp"
#include "windows_msi_state.hpp"
#include <algorithm>
#include <array>
#include <memory>
#include <openssl/evp.h>

namespace x64arm {
namespace {
std::optional<std::u16string> packed_guid(std::u16string_view input){
    if(input.size()!=38 || input.front()!=u'{' || input.back()!=u'}' || input[9]!=u'-' || input[14]!=u'-' || input[19]!=u'-' || input[24]!=u'-')return {};
    std::u16string digits;for(std::size_t i=1;i<37;++i){if(i==9 || i==14 || i==19 || i==24)continue;auto c=input[i];if(c>=u'a' && c<=u'f')c-=32;if(!((c>=u'0' && c<=u'9') || (c>=u'A' && c<=u'F')))return {};digits.push_back(c);}
    std::u16string out;for(const auto range:{std::pair{0U,8U},std::pair{8U,12U},std::pair{12U,16U}})for(auto i=range.second;i-->range.first;)out+=digits[i];
    for(std::size_t i=16;i<32;i+=2){out+=digits[i+1];out+=digits[i];}return out;
}
std::optional<std::u16string> unpacked_guid(std::u16string_view input){
    if(input.size()!=32)return {};std::u16string raw=u"{";std::u16string digits;
    for(const auto range:{std::pair{0U,8U},std::pair{8U,12U},std::pair{12U,16U}})for(auto i=range.second;i-->range.first;)digits+=input[i];
    for(std::size_t i=16;i<32;i+=2){digits+=input[i+1];digits+=input[i];}
    for(std::size_t i=0;i<32;++i){if(i==8 || i==12 || i==16 || i==20)raw+=u'-';raw+=digits[i];}raw+=u'}';return packed_guid(raw)?std::optional{raw}:std::nullopt;
}
struct Property {std::u16string_view name,value;bool installed;};
constexpr Property properties[]{
    {u"HelpLink",u"HelpLink",true},{u"HelpTelephone",u"HelpTelephone",true},{u"InstallDate",u"InstallDate",true},
    {u"InstalledLanguage",u"Language",true},{u"InstalledProductName",u"DisplayName",true},{u"InstallLocation",u"InstallLocation",true},
    {u"InstallSource",u"InstallSource",true},{u"LocalPackage",u"LocalPackage",true},{u"Publisher",u"Publisher",true},
    {u"URLInfoAbout",u"URLInfoAbout",true},{u"URLUpdateInfo",u"URLUpdateInfo",true},{u"VersionMinor",u"VersionMinor",true},
    {u"VersionMajor",u"VersionMajor",true},{u"VersionString",u"DisplayVersion",true},{u"ProductID",u"ProductID",true},
    {u"RegCompany",u"RegCompany",true},{u"RegOwner",u"RegOwner",true},
    {u"InstanceType",u"InstanceType",false},{u"Transforms",u"Transforms",false},{u"Language",u"Language",false},
    {u"ProductName",u"ProductName",false},{u"AssignmentType",u"Assignment",false},{u"PackageCode",u"PackageCode",false},
    {u"Version",u"Version",false},{u"ProductIcon",u"ProductIcon",false},{u"PackageName",u"PackageName",false},{u"AuthorizedLUAApp",u"AuthorizedLUAApp",false}
};
}
std::optional<std::uint32_t> WindowsServices::begin_msi_registry(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall args(state,memory,service);const bool component=service.symbol.starts_with("MsiGetComponentPath"),wide=service.symbol.ends_with('W');
    const auto invalid=[&](){args.return_to(state,component?static_cast<std::uint32_t>(-2):87U);return std::optional<std::uint32_t>{};};
    try{
        if(!args(0) || !args(1) || (args(2) && !args(3)))return invalid();
        const auto product=packed_guid(msi_api::input(memory,args(0),wide));if(!product)return invalid();
        MsiState::Operation op;op.kind=component?MsiState::Kind::Component:MsiState::Kind::Product;op.phase=MsiState::Phase::RegistryStart;
        op.product=*product;op.output=args(2);op.size=args(3);op.wide=wide;op.saved_error=last_error_;op.owner=threads_.current;
        const auto sid=utf8_to_utf16(token_user_sid_string(effective_token_id()));const std::u16string root=u"Software\\Microsoft\\Windows\\CurrentVersion\\Installer\\";
        if(component){
            const auto id=packed_guid(msi_api::input(memory,args(1),wide));if(!id)return invalid();op.property=*product;
            op.locations={{0x80000002U,root+u"UserData\\"+sid+u"\\Components\\"+*id,true,false},{0x80000002U,root+u"UserData\\S-1-5-18\\Components\\"+*id,true,true}};
        }else {
            op.property=msi_api::input(memory,args(1),wide);
            for(const auto& property:properties)if(op.property==property.name){op.path=property.value;op.known_property=true;op.installed_property=property.installed;break;}
            op.locations={{0x80000002U,root+u"UserData\\"+sid+u"\\Products\\"+*product+u"\\InstallProperties",true,false},
                          {0x80000002U,root+u"UserData\\S-1-5-18\\Products\\"+*product+u"\\InstallProperties",true,true},
                          {0x80000002U,root+u"Managed\\"+sid+u"\\Installer\\Products\\"+*product,false,false},
                          {0x80000001U,u"Software\\Microsoft\\Installer\\Products\\"+*product,false,false},
                          {0x80000002U,u"Software\\Classes\\Installer\\Products\\"+*product,false,true}};
            if(op.property==u"PackageName")for(std::size_t i=2;i<5;++i){auto location=op.locations[i];location.path+=u"\\SourceList";op.locations.push_back(std::move(location));}
        }
        if(op.size){memory.check(GuestAddress{op.size},4,Access::Read);memory.check(GuestAddress{op.size},4,Access::Write);}
        auto& bridge=msi_bridge();const auto token=bridge.next_operation++;bridge.operations.emplace(token,std::move(op));return resume_msi_registry(state,memory,token,0);
    }catch(const MemoryFault&){return invalid();}catch(const std::invalid_argument&){return invalid();}
}
std::optional<std::uint32_t> WindowsServices::resume_msi_registry(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result){
    auto& bridge=msi_bridge();auto& op=bridge.operations.at(token);using Phase=MsiState::Phase;const bool component=op.kind==MsiState::Kind::Component;
    const auto callback=[&](Phase phase,std::string_view family,std::string_view symbol,std::vector<std::uint64_t> arguments){
        op.phase=phase;start_callbacks(state,memory,{{resolve(family,symbol).value,std::move(arguments)}},Continuation::MsiOperation,token);return std::optional<std::uint32_t>{};
    };
    const auto finish=[&](std::uint32_t status,bool copy){
        if(copy){const auto written=msi_api::output(memory,op.value,op.wide,op.output,op.size);if(written)status=component?static_cast<std::uint32_t>(written==234?-3:-2):written;}
        const auto saved=op.saved_error;discard_msi(state,memory,token);last_error_=saved;complete_call(state,memory,status);return std::optional<std::uint32_t>{};
    };
    const auto close=[&](std::uint32_t next){op.mode=next;const auto handle=op.registry_key;op.registry_key=0;return callback(Phase::RegistryClose,"advapi32.dll","RegCloseKey",{handle});};
    const auto next=[&](){
        if(op.location>=op.locations.size()){
            if(component)return finish(static_cast<std::uint32_t>(-1),false);
            if(!op.found_product)return finish(1605,false);if(!op.known_property || (op.installed_property && !op.found_installed))return finish(1608,false);
            op.value=op.property==u"InstanceType" || op.property==u"AuthorizedLUAApp"?u"0":u"";return finish(0,true);
        }
        const auto& location=op.locations[op.location];memory.write(GuestAddress{op.scratch+128},utf16_bytes(location.path));memory.store(GuestAddress{op.scratch},x86_?32:64,0);
        return callback(Phase::RegistryOpen,"advapi32.dll","RegOpenKeyExW",{location.root,op.scratch+128,0,0x20119,op.scratch});
    };
    const auto component_path=[&](){
        if(op.value.empty()){op.success=static_cast<std::uint32_t>(-7);return finish(op.success,true);}
        // Installer 的 registry key path 使用 00..03／20..23 根目錄編號。
        if(op.value.size()>=4 && op.value[2]==u':' && op.value[3]==u'\\' && op.value[0]>=u'0' && op.value[0]<=u'2' && op.value[1]>=u'0' && op.value[1]<=u'9'){
            const auto root=static_cast<unsigned>((op.value[0]-u'0')*10+op.value[1]-u'0');if(root>3 && (root<20 || root>23))return finish(static_cast<std::uint32_t>(-6),false);
            auto path=op.value.substr(4);const bool key=!path.empty() && path.back()==u'\\';op.component_source=key;
            if(key)path.pop_back();else {const auto at=path.find_last_of(u'\\');op.destination=at==path.npos?path:path.substr(at+1);path=at==path.npos?u"":path.substr(0,at);}
            memory.write(GuestAddress{op.scratch+128},utf16_bytes(path));memory.store(GuestAddress{op.scratch},x86_?32:64,0);
            return callback(Phase::ComponentKey,"advapi32.dll","RegOpenKeyExW",{0x80000000U+(root%20),op.scratch+128,0,0x20019U+(root>=20?0x100U:0x200U),op.scratch});
        }
        memory.write(GuestAddress{op.scratch+128},utf16_bytes(op.value));return callback(Phase::ComponentAttributes,"kernel32.dll","GetFileAttributesW",{op.scratch+128});
    };
    try{
        if(op.phase==Phase::RegistryStart){
            op.scratch=allocate_heap(memory,131200);if(!op.scratch)throw std::bad_alloc();op.buffer=allocate_heap(memory,op.buffer_size);if(!op.buffer)throw std::bad_alloc();return next();
        }
        if(op.phase==Phase::RegistryOpen){
            if(result){if(result!=2 && result!=3)return finish(component?static_cast<std::uint32_t>(-6):static_cast<std::uint32_t>(result),false);++op.location;return next();}
            op.registry_key=memory.load(GuestAddress{op.scratch},x86_?32:64);op.found_product=true;op.found_installed|=op.locations[op.location].installed;
            if(!component){
                if(!op.known_property || op.installed_property!=op.locations[op.location].installed)return close(0);
                if(op.property==u"AssignmentType"){op.value=op.locations[op.location].machine?u"1":u"0";return close(1);}
            }
            memory.write(GuestAddress{op.scratch+128},utf16_bytes(component?op.property:op.path));memory.store(GuestAddress{op.scratch+16},32,0);
            return callback(Phase::RegistrySize,"advapi32.dll","RegQueryValueExW",{op.registry_key,op.scratch+128,0,op.scratch+8,0,op.scratch+16});
        }
        if(op.phase==Phase::RegistrySize){
            if(result){if(result==2)return close(0);return finish(component?static_cast<std::uint32_t>(-6):static_cast<std::uint32_t>(result),false);}
            const auto count=memory.load(GuestAddress{op.scratch+16},32);if(count>1048576)return finish(component?static_cast<std::uint32_t>(-6):1610U,false);
            if(count>op.buffer_size){const auto replacement=allocate_heap(memory,static_cast<std::size_t>(count));if(!replacement)throw std::bad_alloc();(void)free_heap(memory,op.buffer,state);op.buffer=replacement;op.buffer_size=static_cast<std::uint32_t>(count);}
            memory.store(GuestAddress{op.scratch+16},32,op.buffer_size);return callback(Phase::RegistryRead,"advapi32.dll","RegQueryValueExW",{op.registry_key,op.scratch+128,0,op.scratch+8,op.buffer,op.scratch+16});
        }
        if(op.phase==Phase::RegistryRead){
            if(result==2)return close(0);if(result)return finish(component?static_cast<std::uint32_t>(-6):static_cast<std::uint32_t>(result),false);
            const auto count=memory.load(GuestAddress{op.scratch+16},32),type=memory.load(GuestAddress{op.scratch+8},32);op.value.clear();
            if((type==1 || type==2) && !(count&1) && count<=op.buffer_size){
                for(std::uint64_t i=0;i<count;i+=2){const auto c=static_cast<char16_t>(memory.load(GuestAddress{op.buffer+i},16));if(!c)break;op.value+=c;}
            }else if(!component && type==4 && count==4){const auto value=std::to_string(static_cast<std::uint32_t>(memory.load(GuestAddress{op.buffer},32)));op.value={value.begin(),value.end()};}
            else return finish(component?static_cast<std::uint32_t>(-6):1610U,false);
            if(op.value.size()>65535)return finish(component?static_cast<std::uint32_t>(-6):1610U,false);
            if(!component && op.property==u"PackageCode" && !op.value.empty()){const auto guid=unpacked_guid(op.value);if(!guid)return finish(1610,false);op.value=*guid;}
            return close(component?2U:1U);
        }
        if(op.phase==Phase::RegistryClose){if(op.mode==0){++op.location;return next();}if(op.mode==2)return component_path();return finish(op.mode==3?op.success:0,true);}
        if(op.phase==Phase::ComponentAttributes)return finish(static_cast<std::uint32_t>(result)==UINT32_MAX?2U:3U,true);
        if(op.phase==Phase::ComponentKey){
            if(result)return finish(2,true);op.registry_key=memory.load(GuestAddress{op.scratch},x86_?32:64);
            if(op.component_source){op.success=3;return close(3);}
            memory.write(GuestAddress{op.scratch+128},utf16_bytes(op.destination));memory.store(GuestAddress{op.scratch+16},32,0);
            return callback(Phase::ComponentValue,"advapi32.dll","RegQueryValueExW",{op.registry_key,op.scratch+128,0,0,0,op.scratch+16});
        }
        if(op.phase==Phase::ComponentValue){op.success=result?2U:3U;return close(3);}
        return finish(component?static_cast<std::uint32_t>(-6):1627U,false);
    }catch(const MemoryFault&){return finish(component?static_cast<std::uint32_t>(-2):87U,false);}catch(const std::bad_alloc&){return finish(component?static_cast<std::uint32_t>(-6):8U,false);}catch(const std::invalid_argument&){return finish(component?static_cast<std::uint32_t>(-6):1610U,false);}
}
// 安裝產品查詢與檔案工具：MsiEnumProducts、MsiQueryProductState、MsiEnumRelatedProducts 讀取與 MsiGetProductInfo 相同的 Installer 登錄
// （壓縮 GUID 鍵：受管理的使用者、未受管理的使用者 HKCU\Software\Microsoft\Installer、整台電腦 HKLM\Software\Classes\Installer；
// 安裝狀態看 UserData\<SID>\Products\<p>\InstallProperties）。MsiGetFileVersion 沿用 version.dll，MsiGetFileHash 為檔案內容的 MD5。
std::optional<std::uint32_t> WindowsServices::invoke_msi_query(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall args(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const auto saved_error=last_error_;
    const auto done=[&](std::uint64_t result){last_error_=saved_error;args.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto host=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> values){return static_cast<std::uint32_t>(nested_service(state,memory,family,symbol,values));};
    GuestScratch scratch(*this,memory,state);
    constexpr std::uint32_t machine=0x80000002U,user=0x80000001U;
    const auto sid=utf8_to_utf16(token_user_sid_string(effective_token_id()));
    const std::u16string installer=u"Software\\Microsoft\\Windows\\CurrentVersion\\Installer\\";
    // 開啟（64 位元檢視、唯讀）並列出子機碼或值名稱；機碼不存在時為空。
    const auto names=[&](std::uint32_t root,const std::u16string& path,bool values)->std::optional<std::vector<std::u16string>>{
        const auto text=scratch.text(path),key=scratch.allocate(8),buffer=scratch.allocate(520),length=scratch.allocate(4);if(!text || !key || !buffer || !length)throw std::bad_alloc();
        if(host("advapi32.dll","RegOpenKeyExW",{root,text,0,0x20119,key}))return std::nullopt;
        const auto handle=memory.load(GuestAddress{key},x86_?32:64);std::vector<std::u16string> result;
        for(std::uint32_t index=0;index<65536;++index){
            memory.store(GuestAddress{length},32,260);
            const auto status=values?host("advapi32.dll","RegEnumValueW",{handle,index,buffer,length,0,0,0,0}):host("advapi32.dll","RegEnumKeyExW",{handle,index,buffer,length,0,0,0,0});
            if(status)break;result.push_back(read_utf16(memory,GuestAddress{buffer},260));
        }
        (void)host("advapi32.dll","RegCloseKey",{handle});return result;
    };
    const auto exists=[&](std::uint32_t root,const std::u16string& path){return names(root,path,false).has_value();};
    // 已公告產品的三個位置，依 MsiEnumProducts 的順序：受管理的使用者、未受管理的使用者、整台電腦。
    const std::array<std::pair<std::uint32_t,std::u16string>,3> advertised{{{machine,installer+u"Managed\\"+sid+u"\\Installer\\Products"},
        {user,u"Software\\Microsoft\\Installer\\Products"},{machine,u"Software\\Classes\\Installer\\Products"}}};
    const auto product_state=[&](const std::u16string& packed)->std::int32_t{
        bool found=false;for(const auto& [root,path]:advertised)if(exists(root,path+u"\\"+packed)){found=true;break;}
        if(found){
            for(const auto& owner:{sid,std::u16string{u"S-1-5-18"}})if(exists(machine,installer+u"UserData\\"+owner+u"\\Products\\"+packed+u"\\InstallProperties"))return 5; // INSTALLSTATE_DEFAULT
            return 1; // INSTALLSTATE_ADVERTISED
        }
        // 只安裝給其他使用者：INSTALLSTATE_ABSENT。
        if(const auto users=names(machine,installer+u"UserData",false))for(const auto& owner:*users)if(owner!=sid && owner!=u"S-1-5-18" && exists(machine,installer+u"UserData\\"+owner+u"\\Products\\"+packed))return 2;
        return -1; // INSTALLSTATE_UNKNOWN
    };
    const auto write_guid=[&](const std::u16string& packed,std::uint64_t out)->bool{
        const auto guid=unpacked_guid(packed);if(!guid)return false;
        if(wide)memory.write(GuestAddress{out},utf16_bytes(*guid));else{auto narrow=utf16_to_utf8(*guid);std::vector<std::uint8_t> bytes(narrow.begin(),narrow.end());bytes.push_back(0);memory.write(GuestAddress{out},bytes);}
        return true;
    };
    try{
        if(name.starts_with("MsiEnumProducts")){
            // (Index, ProductBuf[39])：依序列出各位置的產品，重複者只列一次；結束回 ERROR_NO_MORE_ITEMS。
            if(!args(1))return done(87);const auto wanted=static_cast<std::uint32_t>(args(0));
            std::vector<std::u16string> products;
            for(const auto& [root,path]:advertised)if(const auto list=names(root,path,false))for(const auto& packed:*list)
                if(unpacked_guid(packed) && std::find(products.begin(),products.end(),packed)==products.end())products.push_back(packed);
            if(wanted>=products.size())return done(259);return done(write_guid(products[wanted],args(1))?0U:1610U);
        }
        if(name.starts_with("MsiQueryProductState")){
            const auto packed=packed_guid(msi_api::input(memory,args(0),wide));if(!args(0) || !packed)return done(static_cast<std::uint32_t>(-2)); // INSTALLSTATE_INVALIDARG
            return done(static_cast<std::uint32_t>(product_state(*packed)));
        }
        if(name.starts_with("MsiEnumRelatedProducts")){
            // (UpgradeCode, Reserved, Index, ProductBuf[39])：UpgradeCodes\<壓縮升級碼> 的值名稱為壓縮產品碼，只列出已公告或已安裝的產品。
            const auto upgrade=args(0)?packed_guid(msi_api::input(memory,args(0),wide)):std::nullopt;
            if(!upgrade || static_cast<std::uint32_t>(args(1)) || !args(3))return done(87);
            const std::array<std::pair<std::uint32_t,std::u16string>,3> roots{{{machine,installer+u"Managed\\"+sid+u"\\Installer\\UpgradeCodes\\"+*upgrade},
                {user,u"Software\\Microsoft\\Installer\\UpgradeCodes\\"+*upgrade},{machine,u"Software\\Classes\\Installer\\UpgradeCodes\\"+*upgrade}}};
            std::vector<std::u16string> products;
            for(const auto& [root,path]:roots)if(const auto list=names(root,path,true))for(const auto& packed:*list)
                if(unpacked_guid(packed) && std::find(products.begin(),products.end(),packed)==products.end() && product_state(packed)!=-1)products.push_back(packed);
            const auto wanted=static_cast<std::uint32_t>(args(2));if(wanted>=products.size())return done(259);
            return done(write_guid(products[wanted],args(3))?0U:1610U);
        }
        // 元件：UserData\<SID>\Components\<壓縮元件碼> 與整台電腦的 S-1-5-18，值名稱為使用此元件的壓縮產品碼（全 0 為系統共用標記），值內容為 KeyPath。
        const std::array<std::u16string,2> owners{sid,std::u16string{u"S-1-5-18"}};
        const auto clients=[&](const std::u16string& component){
            std::vector<std::u16string> products;
            for(const auto& owner:owners)if(const auto list=names(machine,installer+u"UserData\\"+owner+u"\\Components\\"+component,true))for(const auto& packed:*list)
                if(packed!=u"00000000000000000000000000000000" && unpacked_guid(packed) && std::find(products.begin(),products.end(),packed)==products.end())products.push_back(packed);
            return products;
        };
        if(name.starts_with("MsiEnumComponents")){
            // (Index, ComponentBuf[39])：列出目前使用者與整台電腦已安裝的元件。
            if(!args(1))return done(87);std::vector<std::u16string> components;
            for(const auto& owner:owners)if(const auto list=names(machine,installer+u"UserData\\"+owner+u"\\Components",false))for(const auto& packed:*list)
                if(unpacked_guid(packed) && std::find(components.begin(),components.end(),packed)==components.end())components.push_back(packed);
            const auto wanted=static_cast<std::uint32_t>(args(0));if(wanted>=components.size())return done(259);
            return done(write_guid(components[wanted],args(1))?0U:1610U);
        }
        if(name.starts_with("MsiEnumClients") || name.starts_with("MsiGetProductCode") || name.starts_with("MsiLocateComponent")){
            const auto component=args(0)?packed_guid(msi_api::input(memory,args(0),wide)):std::nullopt;
            const bool locate=name.starts_with("MsiLocateComponent");
            if(!component)return done(locate?static_cast<std::uint32_t>(-2):87U);
            const auto products=clients(*component);
            if(name.starts_with("MsiEnumClients")){
                // (Component, Index, ProductBuf[39])
                if(!args(2))return done(87);const auto wanted=static_cast<std::uint32_t>(args(1));
                if(products.empty())return done(wanted?259U:1607U); // ERROR_UNKNOWN_COMPONENT
                if(wanted>=products.size())return done(259);return done(write_guid(products[wanted],args(2))?0U:1610U);
            }
            // MsiGetProductCode／MsiLocateComponent：取第一個仍為已安裝或已公告的使用產品。
            std::optional<std::u16string> owner_product;for(const auto& packed:products)if(const auto state_value=product_state(packed);state_value==5 || state_value==1){owner_product=packed;break;}
            if(!locate){if(!args(1))return done(87);if(!owner_product)return done(1607);return done(write_guid(*owner_product,args(1))?0U:1610U);}
            // (Component, PathBuf, pcchBuf)：以找到的產品交給 MsiGetComponentPath，回傳值與輸出緩衝語義相同。
            if(args(1) && !args(2))return done(static_cast<std::uint32_t>(-2));
            if(!owner_product)return done(static_cast<std::uint32_t>(-1));
            const auto product_text=*unpacked_guid(*owner_product);
            auto product_bytes=wide?utf16_bytes(product_text):std::vector<std::uint8_t>{};
            if(!wide){const auto narrow=utf16_to_utf8(product_text);product_bytes.assign(narrow.begin(),narrow.end());product_bytes.push_back(0);}
            const auto target=resolve("msi.dll",wide?"MsiGetComponentPathW":"MsiGetComponentPathA").value;
            const auto component_at=args(0),buffer=args(1),count=args(2);
            return nested_continuation(state,memory,"MsiLocateComponent",
                [this,service,target,component_at,buffer,count,product_bytes,saved_error](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                    const WindowsCall inner(s,m,service);const auto product=allocate_heap(m,product_bytes.size());
                    if(!product){inner.return_to(s,static_cast<std::uint32_t>(-1));co_return std::optional<std::uint32_t>{};}
                    m.write(GuestAddress{product},product_bytes);
                    const auto result=co_await service_callback::call(target,{product,component_at,buffer,count});
                    (void)free_heap(m,product,s);last_error_=saved_error;inner.return_to(s,result);co_return std::optional<std::uint32_t>{};
                });
        }
        if(name.starts_with("MsiGetUserInfo")){
            // (Product, UserNameBuf, pcch, OrgNameBuf, pcch, SerialBuf, pcch)：InstallProperties 的 RegOwner、RegCompany、ProductID。
            // USERINFOSTATE：PRESENT 1、ABSENT 0（使用者名稱不存在）、MOREDATA -3、INVALIDARG -2、UNKNOWN -1（產品未安裝）。
            const auto product=args(0)?packed_guid(msi_api::input(memory,args(0),wide)):std::nullopt;
            if(!product || (args(1) && !args(2)) || (args(3) && !args(4)) || (args(5) && !args(6)))return done(static_cast<std::uint32_t>(-2));
            std::optional<std::u16string> install_key;
            for(const auto& owner:owners){const auto key=installer+u"UserData\\"+owner+u"\\Products\\"+*product+u"\\InstallProperties";if(exists(machine,key)){install_key=key;break;}}
            if(!install_key)return done(static_cast<std::uint32_t>(-1));
            const auto value=[&](std::u16string_view field)->std::optional<std::u16string>{
                const auto key=scratch.text(*install_key),label=scratch.text(std::u16string(field)),data=scratch.allocate(2048),size=scratch.allocate(4);
                if(!key || !label || !data || !size)throw std::bad_alloc();memory.store(GuestAddress{size},32,2048);
                if(host("advapi32.dll","RegGetValueW",{machine,key,label,0x10002,0,data,size}))return std::nullopt;return read_utf16(memory,GuestAddress{data},1023);};
            const auto owner=value(u"RegOwner");if(!owner)return done(0);
            bool more=false;
            const auto emit=[&](const std::optional<std::u16string>& text,unsigned at){if(!args(at+1))return;if(msi_api::output(memory,text.value_or(u""),wide,args(at),args(at+1))==234)more=true;};
            emit(owner,1);emit(value(u"RegCompany"),3);emit(value(u"ProductID"),5);
            return done(more?static_cast<std::uint32_t>(-3):1U);
        }
        const auto path=msi_api::input(memory,args(0),wide);
        if(name.starts_with("MsiGetFileVersion")){
            // (FilePath, VersionBuf, pcchVersionBuf, LangBuf, pcchLangBuf)：版本為 a.b.c.d，語言為 Translation 表中的十進位語言識別碼（多個以逗號分隔）。
            if(!args(0) || (args(1) && !args(2)) || (args(3) && !args(4)))return done(87);
            const auto text=scratch.text(path),handle=scratch.allocate(4);if(!text || !handle)return done(8);
            const auto size=host("version.dll","GetFileVersionInfoSizeW",{text,handle});
            if(!size){const auto error=last_error_;return done(error==2 || error==3?error:1006U);} // ERROR_FILE_INVALID：沒有版本資源
            const auto block=scratch.allocate(size),root=scratch.text(u"\\"),translation=scratch.text(u"\\VarFileInfo\\Translation"),value=scratch.allocate(8),length=scratch.allocate(4);
            if(!block || !root || !translation || !value || !length)return done(8);
            if(!host("version.dll","GetFileVersionInfoW",{text,0,size,block}))return done(1006);
            std::uint32_t status=0;
            if(args(2)){
                if(!host("version.dll","VerQueryValueW",{block,root,value,length}))return done(1006);
                const auto info=memory.load(GuestAddress{value},x86_?32:64);const auto ms=static_cast<std::uint32_t>(memory.load(GuestAddress{info+8},32)),ls=static_cast<std::uint32_t>(memory.load(GuestAddress{info+12},32));
                const auto version=utf8_to_utf16(std::to_string(ms>>16)+"."+std::to_string(ms&0xffffU)+"."+std::to_string(ls>>16)+"."+std::to_string(ls&0xffffU));
                status=msi_api::output(memory,version,wide,args(1),args(2));
            }
            if(args(4)){
                std::u16string languages;
                if(host("version.dll","VerQueryValueW",{block,translation,value,length})){
                    const auto table=memory.load(GuestAddress{value},x86_?32:64);const auto bytes=static_cast<std::uint32_t>(memory.load(GuestAddress{length},32));
                    for(std::uint32_t i=0;i+4<=bytes;i+=4){if(!languages.empty())languages+=u",";languages+=utf8_to_utf16(std::to_string(memory.load(GuestAddress{table+i},16)));}
                }
                if(const auto written=msi_api::output(memory,languages,wide,args(3),args(4)))status=written;
            }
            return done(status);
        }
        if(name.starts_with("MsiGetFileHash")){
            // (FilePath, Options, MSIFILEHASHINFO*)：dwFileHashInfoSize 必須為 20；檔案內容的 MD5 依位元組順序分成四個 little-endian DWORD。
            if(!args(0) || static_cast<std::uint32_t>(args(1)) || !args(2) || memory.load(GuestAddress{args(2)},32)!=20)return done(87);
            const auto text=scratch.text(path);if(!text)return done(8);
            const auto invalid_handle=x86_?UINT64_C(0xffffffff):UINT64_MAX;
            const auto file=nested_service(state,memory,"kernel32.dll","CreateFileW",{text,0x80000000U,3,0,3,0x80,0});
            if(file==invalid_handle || !file)return done(last_error_?last_error_:2U);
            const auto close=[&](std::uint64_t handle){const auto error=last_error_;(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{handle});last_error_=error;};
            std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);
            if(!context || EVP_DigestInit_ex(context.get(),EVP_md5(),nullptr)!=1){close(file);return done(8);}
            const auto buffer=scratch.allocate(65536),count=scratch.allocate(4);if(!buffer || !count){close(file);return done(8);}
            std::vector<std::uint8_t> chunk(65536);
            for(;;){
                memory.store(GuestAddress{count},32,0);
                if(!host("kernel32.dll","ReadFile",{file,buffer,65536,count,0})){const auto error=last_error_;close(file);return done(error?error:30U);}
                const auto got=static_cast<std::uint32_t>(memory.load(GuestAddress{count},32));if(!got)break;
                memory.read(GuestAddress{buffer},std::span(chunk).first(got));EVP_DigestUpdate(context.get(),chunk.data(),got);
            }
            close(file);std::array<unsigned char,16> digest{};unsigned size=0;EVP_DigestFinal_ex(context.get(),digest.data(),&size);
            memory.write(GuestAddress{args(2)+4},digest);return done(0);
        }
        throw HostCallFault("未知的 MSI 查詢服務");
    }catch(const MemoryFault&){return done(name.starts_with("MsiQueryProductState")?static_cast<std::uint32_t>(-2):87U);}
    catch(const std::bad_alloc&){return done(8);}
}
}
