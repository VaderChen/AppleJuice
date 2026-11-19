#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/security_descriptor.hpp"
#include "mpr_state.hpp"
#include "service_callback_task.hpp"
#include "x64arm/network_provider_error.hpp"
#include <system_error>
#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace x64arm {
namespace {
using namespace network_resources;
struct Error {std::uint32_t code;};
void require(bool value,std::uint32_t code=87){if(!value)throw Error{code};}
Resource root_resource(){Resource value;value.type=0;value.display=6;value.usage=2;value.remote=std::u16string(provider_name);return value;}
Resource server_resource(std::u16string server){Resource value;value.type=0;value.display=2;value.usage=2;value.remote=u"\\\\"+server;return value;}
Resource share_resource(const Path& path,bool printer=false){
    Resource value;value.remote=path.remote();
    if(printer){value.type=2;value.display=5;}
    return value;
}
struct SavedDrive {
    Resource resource;
    std::u16string user;
    std::uint32_t error{};
};
std::vector<SavedDrive> saved_drives(const RegistryState& registry,std::u16string prefix,char16_t filter){
    std::vector<SavedDrive> result;
    prefix=canonical(prefix)+u"\\";
    for(auto it=registry.keys.lower_bound(prefix);it!=registry.keys.end() && it->first.starts_with(prefix);++it){
        const auto& key=it->first;
        if(key.size()!=prefix.size()+1 || key.back()<u'a' || key.back()>u'z' || (filter && filter!=key.back()))continue;
        SavedDrive drive;
        const auto field=[&](std::u16string_view name){
            std::u16string text;
            const auto value=it->second.find(std::u16string(name));
            if(value==it->second.end())return text;
            const auto& bytes=value->second.bytes;
            if(value->second.type!=1 || bytes.size()%2){drive.error=1206;return text;}
            for(std::size_t i=0;i<bytes.size();i+=2){
                const auto c=static_cast<char16_t>(bytes[i]|(bytes[i+1]<<8));
                if(!c)break;
                text+=c;
            }
            return text;
        };
        drive.resource.scope=3;
        drive.resource.local=std::u16string(1,static_cast<char16_t>(key.back()-32))+u":";
        drive.resource.remote=field(u"remotepath");
        const auto provider=field(u"providername");
        if(!provider.empty())drive.resource.provider=provider;
        drive.user=field(u"username");
        const auto path=parse_path(*drive.resource.remote);
        if(!path || path->share.empty() || !path->system.empty())drive.error=1206;
        result.push_back(std::move(drive));
    }
    return result;
}
std::uint32_t resource_type(const Resource& value,std::uint32_t filter){return !filter || value.type==filter || (value.usage&2U);}
}
std::span<const WindowsServiceDescriptor> mpr_service_descriptors(){
    static constexpr WindowsServiceDescriptor values[]{
        {"mpr.dll","#22",1,0,0,1},{"mpr.dll","#23",2,0,0,2},{"mpr.dll","#24",1},
        {"mpr.dll","WNetOpenEnumA",5},{"mpr.dll","WNetOpenEnumW",5},{"mpr.dll","WNetEnumResourceA",4},{"mpr.dll","WNetEnumResourceW",4},{"mpr.dll","WNetCloseEnum",1},
        {"mpr.dll","WNetGetResourceParentA",3},{"mpr.dll","WNetGetResourceParentW",3},{"mpr.dll","WNetGetResourceInformationA",4},{"mpr.dll","WNetGetResourceInformationW",4},
        {"shell32.dll","SHValidateUNC",3,0,0,4},
        {"mpr.dll","WNetGetLastErrorA",5},{"mpr.dll","WNetGetLastErrorW",5},
        {"mpr.dll","WNetSetLastErrorA",3},{"mpr.dll","WNetSetLastErrorW",3},{"mpr.dll","NPSSetExtendedErrorA",2},
        {"mpr.dll","WNetGetProviderNameA",3},{"mpr.dll","WNetGetProviderNameW",3},
        {"mpr.dll","WNetGetNetworkInformationA",2},{"mpr.dll","WNetGetNetworkInformationW",2},
        {"mpr.dll","WNetClearConnections",1},{"mpr.dll","WNetRestoreConnection",2},
        {"mpr.dll","WNetRestoreConnectionA",2},{"mpr.dll","WNetRestoreConnectionW",2},
        {"mpr.dll","WNetUseConnectionA",8},{"mpr.dll","WNetUseConnectionW",8},
        {"mpr.dll","WNetAddConnectionA",3},{"mpr.dll","WNetAddConnectionW",3},
        {"mpr.dll","WNetCancelConnectionA",2},{"mpr.dll","WNetCancelConnectionW",2},
        {"mpr.dll","WNetAddConnection3A",5},{"mpr.dll","WNetAddConnection3W",5},
        {"mpr.dll","WNetAddConnection2A",4},{"mpr.dll","WNetAddConnection2W",4},{"mpr.dll","WNetCancelConnection2A",3},{"mpr.dll","WNetCancelConnection2W",3},
        {"mpr.dll","WNetGetConnectionA",3},{"mpr.dll","WNetGetConnectionW",3},{"mpr.dll","WNetGetUserA",3},{"mpr.dll","WNetGetUserW",3},
        {"mpr.dll","WNetGetUniversalNameA",4},{"mpr.dll","WNetGetUniversalNameW",4}
    };return values;
}
void WindowsServices::set_network_resource_backend(std::shared_ptr<Backend> backend){if(!mpr_)mpr_=std::make_shared<MprState>();mpr_->backend=std::move(backend);}
std::optional<std::uint32_t> WindowsServices::invoke_mpr(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto& symbol=service.symbol;const bool validate=symbol=="SHValidateUNC",wide=validate || symbol.ends_with('W');const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(symbol=="#22" || symbol=="#23" || symbol=="#24"){
        // MPR 匯出使用 stdcall，內部配置器沿用客體 CRT 的呼叫慣例、堆積及配置失敗處理。
        return nested_continuation(state,memory,"MPR heap",[this,service](GuestState& cpu,GuestMemory& mem)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(cpu,mem,service);
            const auto first=call(0);
            std::uint64_t result;
            if(service.symbol=="#22")
                result=co_await service_callback::call_value(resolve("msvcrt.dll","calloc").value,{1,static_cast<std::uint32_t>(first)});
            else if(service.symbol=="#23")
                result=co_await service_callback::call_value(resolve("msvcrt.dll","_recalloc").value,{first,1,static_cast<std::uint32_t>(call(1))});
            else{
                if(first)(void)co_await service_callback::call(resolve("msvcrt.dll","free").value,{first});
                result=first!=0;
            }
            call.return_to(cpu,result);
            co_return std::nullopt;
        });
    }
    const bool provider_name_query=symbol.starts_with("WNetGetProviderName");
    const bool provider_info_query=symbol.starts_with("WNetGetNetworkInformation");
    native_provider_error=0;
    const auto done=[&](std::uint32_t code)->std::optional<std::uint32_t>{
        if(code)last_error_=code;
        if(code==1208 && mpr_){
            const int native=native_provider_error;
            const auto message=native?std::error_code(native,std::system_category()).message():"Invalid network provider response";
            mpr_->errors[threads_.current]={native?static_cast<std::uint32_t>(native):code,
                                             utf8_to_utf16_lossy(message),std::u16string(provider_name)};
        }
        arg.return_to(state,validate?static_cast<unsigned>(!code):code);
        return {};
    };
    try{
        refresh_file_security_context();if(!mpr_)mpr_=std::make_shared<MprState>();if(!mpr_->backend)mpr_->backend=make_native_backend();const auto scope=filesystem_.network_scope();
        const auto text=[&](std::uint64_t pointer){return !pointer?std::u16string{}:wide?read_utf16(memory,GuestAddress{pointer},32767):utf8_to_utf16_lossy(read_ansi(memory,pointer,131068));};
        const auto encode=[&](std::u16string_view value){if(wide)return utf16_bytes(value);const auto s=utf16_to_utf8(value);std::vector<std::uint8_t> bytes(s.begin(),s.end());bytes.push_back(0);return bytes;};
        if(symbol.starts_with("WNetSetLastError") || symbol=="NPSSetExtendedErrorA"){
            const auto provider=symbol=="NPSSetExtendedErrorA"?std::u16string(provider_name):text(arg(2));
            mpr_->errors[threads_.current]={static_cast<std::uint32_t>(arg(0)),text(arg(1)),provider};
            return done(0);
        }
        if(symbol.starts_with("WNetGetLastError")){
            require(arg(0) && arg(1) && arg(3) && static_cast<std::uint32_t>(arg(2)) && static_cast<std::uint32_t>(arg(4)),487);
            const auto found=mpr_->errors.find(threads_.current);
            const MprState::ExtendedError empty;
            const auto& error=found==mpr_->errors.end()?empty:found->second;
            const auto bounded=[&](const std::u16string& value,std::uint32_t capacity){
                auto bytes=encode(value);
                const unsigned width=wide?2U:1U;
                if(bytes.size()/width>capacity){
                    auto keep=static_cast<std::size_t>(capacity-1)*width;
                    if(wide && keep && value[keep/2-1]>=0xd800 && value[keep/2-1]<=0xdbff)keep-=2;
                    if(!wide)while(keep && (bytes[keep]&0xc0)==0x80)--keep;
                    bytes.resize(keep);
                    bytes.resize(keep+width,0);
                }
                return bytes;
            };
            const auto description=bounded(error.description,static_cast<std::uint32_t>(arg(2)));
            const auto provider=bounded(error.provider,static_cast<std::uint32_t>(arg(4)));
            try{
                memory.check(GuestAddress{arg(0)},4,Access::Write);
                memory.check(GuestAddress{arg(1)},description.size(),Access::Write);
                memory.check(GuestAddress{arg(3)},provider.size(),Access::Write);
                memory.store(GuestAddress{arg(0)},32,error.code);
                memory.write(GuestAddress{arg(1)},description);
                memory.write(GuestAddress{arg(3)},provider);
            }catch(const MemoryFault&){return done(487);}
            return done(0);
        }
        if(provider_name_query){
            require(arg(1) && arg(2),487);
            const auto provider=mpr_->backend->provider();
            require(provider && (arg(0)&0xffff0000U)==provider->network_type,1222);
            const auto bytes=encode(provider->name);
            const auto count=bytes.size()/(wide?2U:1U);
            if(memory.load(GuestAddress{arg(2)},32)<count){
                memory.store(GuestAddress{arg(2)},32,count);
                return done(234);
            }
            memory.write(GuestAddress{arg(1)},bytes);
            return done(0);
        }
        if(provider_info_query){
            require(arg(0) && arg(1),487);
            const GuestAddress out{arg(1)};
            const unsigned size=x86_?32U:40U;
            require(memory.load(out,32)>=size);
            const auto provider=mpr_->backend->provider();
            require(provider.has_value(),1222);
            require(text(arg(0))==provider->name,1204);
            memory.fill(out,0,size);
            memory.store(out,32,size);
            memory.store(GuestAddress{out.value+4},32,provider->version);
            memory.store(GuestAddress{out.value+16+unit},16,provider->network_type>>16);
            memory.store(GuestAddress{out.value+20+unit},32,provider->printer_mask);
            memory.store(GuestAddress{out.value+24+unit},32,provider->drive_mask);
            return done(0);
        }
        const auto input=[&](std::uint64_t pointer,bool read_local=false){require(pointer);memory.check(GuestAddress{pointer},16+4*unit,Access::Read);Resource value;value.type=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+4},32));
            const auto field=[&](unsigned index){return memory.load(GuestAddress{pointer+16+index*unit},bits);};if(read_local && field(0))value.local=text(field(0));if(field(1))value.remote=text(field(1));if(field(3))value.provider=text(field(3));require(value.provider.empty() || canonical(value.provider)==canonical(std::u16string(provider_name)),1204);return value;
        };
        const auto packed=[&](const Resource& row){PackedResource value;value.value=row;value.strings.resize(4);if(row.local)value.strings[0]=encode(*row.local);if(row.remote)value.strings[1]=encode(*row.remote);if(row.comment)value.strings[2]=encode(*row.comment);value.strings[3]=encode(row.provider);return value;};
        const auto credentials=[&](std::u16string_view server){
            const auto sessions=mpr_->credentials.find(scope);
            if(sessions!=mpr_->credentials.end()){
                const auto found=sessions->second.find(canonical(std::u16string(server)));
                if(found!=sessions->second.end())return found->second;
            }
            const auto account=security::resolve_account(runtime_.token_objects.at(effective_token_id()).user.sid,
                                                         runtime_.token_objects.at(1).user.sid);
            Credentials value;
            if(account)value.user=utf8_to_utf16(account->name);
            value.guest=value.user.empty();
            return value;
        };
        const auto external=[&]{if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許原生網路資源 I/O");};
        const auto resolve_share=[&](const Path& path){
            const auto remote=canonical(path.remote());
            for(const auto& row:filesystem_.network_files()->connections(scope)){
                if(canonical(row->remote)==remote){auto result=share_resource(path);result.remote=row->remote;return result;}
            }
            external();
            const auto shares=mpr_->backend->shares(path.server,credentials(path.server));
            if(shares.error)throw Error{shares.error};
            for(const auto& share:shares.values){
                if(canonical(share.name)==canonical(path.share))return share_resource(Path{path.server,share.name,{}},share.printer);
            }
            throw Error{67};
        };
        const auto sid=token_user_sid_string(effective_token_id());const auto profile_key=sid==token_user_sid_string(1)?std::u16string(1,1)+u"\\network":std::u16string(1,3)+u"\\"+utf8_to_utf16(sid)+u"\\network";
        const auto profile=[&](char drive,const Resource* resource,std::u16string_view user,bool active=true){RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_);if(transaction.error)return transaction.error;const auto key=canonical(profile_key+u"\\"+std::u16string(1,static_cast<char16_t>(drive)));auto old=runtime_.registry;
            if(resource){auto& values=runtime_.registry.keys[key];values[u"remotepath"]={1,utf16_bytes(*resource->remote),u"RemotePath"};values[u"providername"]={1,utf16_bytes(provider_name),u"ProviderName"};values[u"username"]={1,utf16_bytes(user),u"UserName"};runtime_.registry.display_names[key]=profile_key+u"\\"+std::u16string(1,static_cast<char16_t>(drive));runtime_.registry.modified_times[key]=runtime_.system_time_state().filetime;}
            else{if(!runtime_.registry.keys.contains(key))return active?0U:2250U;runtime_.registry.keys.erase(key);runtime_.registry.display_names.erase(key);runtime_.registry.modified_times.erase(key);}const auto error=transaction.commit();if(error)runtime_.registry=std::move(old);return error;
        };
        if(symbol.starts_with("WNetRestoreConnection")){
            return nested_continuation(state,memory,"WNetRestoreConnection",[this,service,profile_key,wide,unit,bits](GuestState& cpu,GuestMemory& mem)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(cpu,mem,service);
                const auto finish=[&](std::uint32_t code){
                    if(code)last_error_=code;
                    call.return_to(cpu,code);return std::optional<std::uint32_t>{};
                };
                char16_t filter=0;
                if(call(1)){
                    const auto device=wide?read_utf16(mem,GuestAddress{call(1)},32767):utf8_to_utf16_lossy(read_ansi(mem,call(1),131068));
                    if(device.size()!=2 || device[1]!=u':')co_return finish(1200);
                    filter=canonical(device)[0];
                    if(filter<u'a' || filter>u'z')co_return finish(1200);
                }
                std::vector<SavedDrive> plan;
                {
                    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
                    if(transaction.error)co_return finish(1205);
                    plan=saved_drives(runtime_.registry,profile_key,filter);
                }
                if(filter && plan.empty())co_return finish(2250);
                const auto owner=call(0);
                std::uint32_t first_error=0;
                for(const auto& drive:plan){
                    if(drive.error){if(!first_error)first_error=drive.error;continue;}
                    GuestScratch scratch(*this,mem,cpu);
                    const auto nr=scratch.allocate(16+4*unit),local=scratch.text(*drive.resource.local),
                               remote=scratch.text(*drive.resource.remote),provider=scratch.text(drive.resource.provider);
                    auto user=drive.user.empty()?0:scratch.text(drive.user);
                    if(!nr || !local || !remote || !provider || (!drive.user.empty() && !user))co_return finish(8);
                    mem.store(GuestAddress{nr+4},32,1);
                    mem.store(GuestAddress{nr+16},bits,local);
                    mem.store(GuestAddress{nr+16+unit},bits,remote);
                    mem.store(GuestAddress{nr+16+3*unit},bits,provider);
                    const auto add=[&](std::uint64_t password){
                        return static_cast<std::uint32_t>(nested_service(cpu,mem,"mpr.dll","WNetAddConnection2W",{nr,password,user,0}));
                    };
                    auto error=add(0);
                    if(native_gui_ && (error==5 || error==86 || error==1326)){
                        constexpr unsigned capacity=514;
                        const auto info=scratch.allocate(5*unit),password=scratch.allocate(2*capacity);
                        user=scratch.allocate(2*capacity);
                        if(!info || !password || !user)co_return finish(8);
                        mem.store(GuestAddress{info},32,5*unit);
                        mem.store(GuestAddress{info+unit},bits,owner);
                        if(drive.user.size()<capacity)mem.write(GuestAddress{user},utf16_bytes(drive.user));
                        const auto prompt=resolve("credui.dll","CredUIPromptForCredentialsW").value;
                        error=static_cast<std::uint32_t>(co_await service_callback::call(prompt,{info,remote,0,error,user,capacity,password,capacity,0,0x40002}));
                        if(!error)error=add(password);
                        mem.fill(GuestAddress{password},0,2*capacity);
                    }
                    if(error && !first_error)first_error=error;
                    if(error==1223)break;
                }
                co_return finish(first_error);
            });
        }
        if(symbol=="WNetCloseEnum"){const auto found=mpr_->enumerations.find(arg(0));if(found==mpr_->enumerations.end() || found->second.scope!=scope)return done(6);mpr_->enumerations.erase(found);return done(0);}
        if(symbol.starts_with("WNetOpenEnum")){
            const auto requested=static_cast<std::uint32_t>(arg(0)),type=static_cast<std::uint32_t>(arg(1)),usage=static_cast<std::uint32_t>(arg(2));require((requested==1 || requested==2 || requested==3 || requested==5) && type<=2 && arg(4));
            require(requested==2?!(usage&~0x1fU):!arg(3));
            const auto usage_filter=requested==2?usage&3U:0U;
            if(requested==2 && (usage&16U)){
                const auto parent=arg(3)?input(arg(3)):root_resource();
                const auto path=parent.remote?parse_path(*parent.remote):std::nullopt;
                require(!credentials(path?path->server:std::u16string{}).guest,1244);
            }
            memory.check(GuestAddress{arg(4)},unit,Access::Write);std::vector<Resource> values;
            if(requested==1){for(const auto& row:filesystem_.network_files()->connections(scope)){auto value=share_resource(*parse_path(row->remote));value.scope=1;if(row->drive)value.local=std::u16string(1,static_cast<char16_t>(row->drive))+u":";values.push_back(std::move(value));}}
            else if(requested==3){
                RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
                if(transaction.error)return done(transaction.error);
                for(auto& row:saved_drives(runtime_.registry,profile_key,0))if(!row.error)values.push_back(std::move(row.resource));
            }
            else if(!arg(3) && requested==2)values.push_back(root_resource());
            else{
                const auto parent=arg(3)?input(arg(3)):root_resource();if(!parent.remote || canonical(*parent.remote)==canonical(std::u16string(provider_name))){external();auto servers=mpr_->backend->servers();if(servers.error)return done(servers.error);std::set<std::u16string> seen;for(auto& server:servers.values)if(seen.insert(canonical(server.name)).second){auto value=server_resource(server.name);value.comment=server.comment;values.push_back(std::move(value));}}
                else{const auto path=parse_path(*parent.remote);require(path.has_value(),67);require(path->share.empty(),1207);external();auto shares=mpr_->backend->shares(path->server,credentials(path->server));if(shares.error)return done(shares.error);for(auto& share:shares.values)if(!share.hidden){auto value=share_resource(Path{path->server,share.name,{}},share.printer);value.comment=share.comment;values.push_back(std::move(value));}}
            }
            std::erase_if(values,[&](const auto& value){return !resource_type(value,type) || (usage_filter && !(value.usage&usage_filter));});require(mpr_->enumerations.size()<256,8);const auto handle=mpr_->next_enumeration++;mpr_->enumerations.emplace(handle,MprState::Enumeration{scope,std::move(values),0});memory.store(GuestAddress{arg(4)},bits,handle);return done(0);
        }
        if(symbol.starts_with("WNetEnumResource")){
            const auto found=mpr_->enumerations.find(arg(0));require(found!=mpr_->enumerations.end() && found->second.scope==scope,6);require(arg(1) && arg(2) && arg(3));const auto maximum=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));const auto capacity=memory.load(GuestAddress{arg(3)},32);require(maximum);auto& enumeration=found->second;
            if(enumeration.position>=enumeration.values.size()){memory.store(GuestAddress{arg(1)},32,0);return done(259);}std::vector<PackedResource> rows;
            for(std::size_t n=enumeration.position;n<enumeration.values.size() && rows.size()<maximum;++n){rows.push_back(packed(enumeration.values[n]));if(packed_size(rows,unit)>capacity){rows.pop_back();break;}}
            if(rows.empty()){const std::array first{packed(enumeration.values[enumeration.position])};memory.store(GuestAddress{arg(1)},32,0);memory.store(GuestAddress{arg(3)},32,packed_size(first,unit));return done(234);}const auto bytes=pack(rows,unit,arg(2));memory.write(GuestAddress{arg(2)},bytes);memory.store(GuestAddress{arg(1)},32,rows.size());enumeration.position+=rows.size();return done(0);
        }
        if(symbol.starts_with("WNetGetResourceParent") || symbol.starts_with("WNetGetResourceInformation")){
            auto value=input(arg(0));require(value.remote && !value.remote->empty(),67);require(arg(1) && arg(2));const bool parent=symbol.starts_with("WNetGetResourceParent");Resource output;std::u16string system;
            if(canonical(*value.remote)==canonical(std::u16string(provider_name))){output=root_resource();output.scope=0;if(parent)output.remote.reset();}
            else{const auto path=parse_path(*value.remote);require(path.has_value(),67);require(value.type<=2,66);
                if(parent)output=path->share.empty()?root_resource():server_resource(path->server);
                else if(path->share.empty())output=server_resource(path->server);
                else{output=resolve_share(*path);require(!value.type || value.type==output.type,66);system=path->system;}
            }
            output.scope=0;
            const std::array rows{packed(output)};
            auto bytes=pack(rows,unit,arg(1));
            const auto system_pointer=arg(1)+bytes.size();
            if(!parent && arg(3)){const auto extra=encode(system);bytes.insert(bytes.end(),extra.begin(),extra.end());}
            if(memory.load(GuestAddress{arg(2)},32)<bytes.size()){
                memory.store(GuestAddress{arg(2)},32,bytes.size());
                return done(234);
            }
            memory.check(GuestAddress{arg(1)},bytes.size(),Access::Write);
            if(!parent && arg(3))memory.check(GuestAddress{arg(3)},unit,Access::Write);
            memory.write(GuestAddress{arg(1)},bytes);
            if(!parent && arg(3))memory.store(GuestAddress{arg(3)},bits,system_pointer);
            return done(0);
        }
        const auto connect=[&](const Resource& value,std::uint64_t password,std::uint64_t user,std::uint32_t flags,char& drive)->std::uint32_t{
            require(!(flags&~0x3b9fU) && value.type<=2);require(value.remote && !value.remote->empty(),67);const auto path=parse_path(*value.remote);require(path && !path->share.empty() && path->system.empty(),67);require(value.type!=2,66);drive=0;
            if(value.local && !value.local->empty()){require(value.local->size()==2 && (*value.local)[1]==u':',1200);auto c=(*value.local)[0];if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);require(c>=u'A' && c<=u'Z',1200);drive=static_cast<char>(c);require(filesystem_.information(std::string(1,drive)+":/").error!=0,85);}
            if((flags&0x80U) && !drive){require(value.type!=0);for(char c='Z';c>='C';--c)if(filesystem_.information(std::string(1,c)+":/").error){drive=c;break;}require(drive,85);}
            auto auth=credentials(path->server);if(user){auto requested=text(user);if(canonical(requested)!=canonical(auth.user))auth.password.clear();auth.user=std::move(requested);}if(password)auth.password=text(password);auth.guest=auth.user.empty();external();const auto existing=filesystem_.network_files()->connections(scope);
            for(const auto& row:existing){const auto remote=parse_path(row->remote);if(remote && canonical(remote->server)==canonical(path->server))require(canonical(row->user)==canonical(auth.user),1219);}
            const auto directory=filesystem_.network_mount_directory(std::to_string(mpr_->next_enumeration++));auto connection=mpr_->backend->connect(*path,auth,directory,!filesystem_.writable());if(connection.error)return connection.error;require(connection.mount!=nullptr,1208);auto row=std::make_shared<NetworkFileBinding>();row->remote=path->remote();row->drive=drive;row->user=auth.user;row->readonly=!filesystem_.writable();row->mount=connection.mount;
            const auto added=filesystem_.network_files()->add(scope,row);if(added){(void)connection.mount->disconnect(false);return added;}if((flags&1U) && !(flags&4U) && drive){const auto error=profile(drive,&value,auth.user);if(error){(void)filesystem_.network_files()->remove(scope,std::u16string(1,static_cast<char16_t>(drive))+u":",true);return error;}}mpr_->credentials[scope][canonical(path->server)]=std::move(auth);return 0;
        };
        if(validate){
            const auto flags=static_cast<std::uint32_t>(arg(2));require(arg(1) && !(flags&~15U));
            const auto path=parse_path(text(arg(1)));require(path && !path->share.empty(),67);
            if(flags&4U){
                require(!(flags&1U) && path->system.empty(),66);
                external();const auto shares=mpr_->backend->shares(path->server,credentials(path->server));
                if(shares.error)return done(shares.error);
                const auto found=std::any_of(shares.values.begin(),shares.values.end(),[&](const auto& item){
                    return item.printer && canonical(item.name)==canonical(path->share);
                });
                return done(found?0U:67U);
            }
            const auto remote=path->remote();auto resource=share_resource(*path);
            const bool map_drive=(flags&1U)!=0;
            const bool remember=(flags&8U)!=0;
            char drive=0;bool reuse=false;
            for(const auto& row:filesystem_.network_files()->connections(scope)){
                if(canonical(row->remote)!=canonical(remote) || (map_drive && !row->drive))continue;
                drive=row->drive;reuse=true;
                if(remember && drive){const auto error=profile(drive,&resource,row->user);if(error)return done(error);}
                break;
            }
            if(!reuse){
                const auto error=connect(resource,0,0,(map_drive?0x80U:0U)|(remember?1U:0U),drive);
                if(error)return done(error);
            }
            if(map_drive){
                const auto local=std::u16string(1,static_cast<char16_t>(drive))+u":"+(path->system.empty()?u"\\":path->system);
                memory.write(GuestAddress{arg(1)},utf16_bytes(local));
            }
            return done(0);
        }
        if(symbol.starts_with("WNetUseConnection")){
            auto value=input(arg(1),true);
            const auto flags=static_cast<std::uint32_t>(arg(4));
            require(value.remote && !value.remote->empty(),67);
            const bool local=(value.local && !value.local->empty()) || (flags&0x80U);
            const auto access=arg(5),size=arg(6),result=arg(7);
            const bool output=access && size && memory.load(GuestAddress{size},32);
            if(output){
                const auto needed=local?3U:encode(*value.remote).size()/(wide?2U:1U);
                if(needed>memory.load(GuestAddress{size},32)){
                    memory.store(GuestAddress{size},32,needed);return done(234);
                }
                memory.check(GuestAddress{access},needed*(wide?2U:1U),Access::Write);
                if(result)memory.check(GuestAddress{result},4,Access::Write);
            }
            char drive=0;
            const auto error=connect(value,arg(2),arg(3),flags,drive);
            if(!error && output){
                const auto name=drive?std::u16string(1,static_cast<char16_t>(drive))+u":":*value.remote;
                memory.write(GuestAddress{access},encode(name));
                if(result)memory.store(GuestAddress{result},32,drive?1:0);
            }
            return done(error);
        }
        if(symbol.starts_with("WNetAddConnection")){
            char drive=0;
            if(symbol=="WNetAddConnectionA" || symbol=="WNetAddConnectionW"){
                Resource resource;resource.remote=text(arg(0));
                if(arg(2))resource.local=text(arg(2));
                // 舊版 WNetAddConnection 將具名裝置寫入使用者的連線設定。
                return done(connect(resource,arg(1),0,1,drive));
            }
            const unsigned first=symbol.starts_with("WNetAddConnection3")?1U:0U;
            return done(connect(input(arg(first),true),arg(first+1),arg(first+2),static_cast<std::uint32_t>(arg(first+3)),drive));
        }
        if(symbol.starts_with("WNetCancelConnection")){
            const bool extended=symbol.starts_with("WNetCancelConnection2");
            const auto flags=extended?static_cast<std::uint32_t>(arg(1)):0U;
            require(arg(0) && !(flags&~1U));
            const auto name=canonical(text(arg(0)));
            const bool drive=name.size()==2 && name[0]>=u'a' && name[0]<=u'z' && name[1]==u':';
            const auto error=filesystem_.network_files()->remove(scope,name,arg(extended?2:1)!=0);
            if((flags&1U) && drive && (!error || error==2250))
                return done(profile(static_cast<char>(name[0]-u'a'+'A'),nullptr,{},!error));
            return done(error);
        }
        const auto network_files=filesystem_.network_files();
        if(symbol=="WNetClearConnections"){
            std::set<std::u16string> names;
            for(const auto& row:network_files->connections(scope))
                names.insert(row->drive?std::u16string(1,static_cast<char16_t>(row->drive))+u":":canonical(row->remote));
            std::uint32_t first_error=0;
            for(const auto& name:names){
                const auto error=network_files->remove(scope,name,true);
                if(error && error!=2250 && !first_error)first_error=error;
            }
            if(network_files->connections(scope).empty())mpr_->credentials.erase(scope);
            return done(first_error);
        }
        const bool universal=symbol.starts_with("WNetGetUniversalName");
        const bool user=symbol.starts_with("WNetGetUser");
        if(universal || user || symbol.starts_with("WNetGetConnection")){
            const auto output=arg(universal?2:1),length=arg(universal?3:2);
            require(length && output);
            if(universal)require(arg(0) && (arg(1)==1 || arg(1)==2));
            const auto name=text(arg(0));
            const bool drive=name.size()>=2 && name[1]==u':' &&
                ((name[0]>=u'A' && name[0]<=u'Z') || (name[0]>=u'a' && name[0]<=u'z'));
            if(!user)require(drive && (universal?(name.size()==2 || name[2]==u'\\' || name[2]==u'/'):name.size()==2),1200);
            std::u16string value;
            if(user && name.empty())value=credentials({}).user;
            else{
                const auto key=canonical(universal?name.substr(0,2):name);
                const auto rows=network_files->connections(scope);
                const auto found=std::find_if(rows.begin(),rows.end(),[&](const auto& row){
                    if(row->drive)return name.size()>=2 && (universal || name.size()==2) &&
                        key==std::u16string(1,static_cast<char16_t>(row->drive+32))+u":";
                    return user && canonical(row->remote)==key;
                });
                if(found==rows.end()){
                    if(drive && !user){
                        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
                        if(transaction.error)return done(transaction.error);
                        const auto saved=runtime_.registry.keys.find(canonical(profile_key+u"\\"+key.substr(0,1)));
                        if(saved!=runtime_.registry.keys.end() && saved->second.contains(u"remotepath"))return done(1201);
                    }
                    return done(2250);
                }
                value=user?(*found)->user:(*found)->remote;
            }
            if(!universal){
                const auto bytes=encode(value);
                const auto count=bytes.size()/(wide?2U:1U);
                if(memory.load(GuestAddress{length},32)<count){memory.store(GuestAddress{length},32,count);return done(234);}
                memory.write(GuestAddress{output},bytes);
                return done(0);
            }
            auto remaining=name.substr(2);
            std::replace(remaining.begin(),remaining.end(),u'/',u'\\');
            std::vector<std::vector<std::uint8_t>> strings{encode(value+remaining)};
            if(arg(1)==2){strings.push_back(encode(value));strings.push_back(encode(remaining));}
            std::size_t required=strings.size()*unit;
            for(const auto& string:strings)required+=string.size();
            if(memory.load(GuestAddress{length},32)<required){memory.store(GuestAddress{length},32,required);return done(234);}
            memory.check(GuestAddress{output},required,Access::Write);
            memory.check(GuestAddress{length},4,Access::Write);
            std::size_t offset=strings.size()*unit;
            for(std::size_t i=0;i<strings.size();++i){
                memory.store(GuestAddress{output+i*unit},bits,output+offset);
                memory.write(GuestAddress{output+offset},strings[i]);
                offset+=strings[i].size();
            }
            memory.store(GuestAddress{length},32,required);
            return done(0);
        }
        throw HostCallFault("MPR 入口缺少對應實作");
    }catch(const Error& error){return done(error.code);}catch(const SecurityDescriptorError& error){return done(error.code);}catch(const std::bad_alloc&){return done(8);}catch(const std::length_error&){return done(534);}catch(const std::invalid_argument&){return done(87);}
}
}
