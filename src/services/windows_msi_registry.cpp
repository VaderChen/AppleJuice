#include "windows_msi_state.hpp"
#include <algorithm>

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
}
