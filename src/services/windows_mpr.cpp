#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/windows_default_environment.hpp"
#include "mpr_state.hpp"
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
Resource share_resource(const Path& path){Resource value;value.remote=path.remote();return value;}
std::uint32_t resource_type(const Resource& value,std::uint32_t filter){return !filter || value.type==filter || (value.usage&2U);}
}
std::span<const WindowsServiceDescriptor> mpr_service_descriptors(){
    static constexpr WindowsServiceDescriptor values[]{
        {"mpr.dll","WNetOpenEnumA",5},{"mpr.dll","WNetOpenEnumW",5},{"mpr.dll","WNetEnumResourceA",4},{"mpr.dll","WNetEnumResourceW",4},{"mpr.dll","WNetCloseEnum",1},
        {"mpr.dll","WNetGetResourceParentA",3},{"mpr.dll","WNetGetResourceParentW",3},{"mpr.dll","WNetGetResourceInformationA",4},{"mpr.dll","WNetGetResourceInformationW",4},
        {"mpr.dll","WNetAddConnection2A",4},{"mpr.dll","WNetAddConnection2W",4},{"mpr.dll","WNetCancelConnection2A",3},{"mpr.dll","WNetCancelConnection2W",3},
        {"mpr.dll","WNetGetConnectionA",3},{"mpr.dll","WNetGetConnectionW",3},{"mpr.dll","WNetGetUserA",3},{"mpr.dll","WNetGetUserW",3},
        {"mpr.dll","WNetGetUniversalNameA",4},{"mpr.dll","WNetGetUniversalNameW",4}
    };return values;
}
void WindowsServices::set_network_resource_backend(std::shared_ptr<Backend> backend){if(!mpr_)mpr_=std::make_shared<MprState>();mpr_->backend=std::move(backend);}
std::optional<std::uint32_t> WindowsServices::invoke_mpr(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto& symbol=service.symbol;const bool wide=symbol.ends_with('W');const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint32_t code)->std::optional<std::uint32_t>{arg.return_to(state,code);return {};};
    try{
        refresh_file_security_context();if(!mpr_)mpr_=std::make_shared<MprState>();if(!mpr_->backend)mpr_->backend=make_native_backend();const auto scope=filesystem_.network_scope();
        const auto text=[&](std::uint64_t pointer){return !pointer?std::u16string{}:wide?read_utf16(memory,GuestAddress{pointer},32767):utf8_to_utf16_lossy(read_ansi(memory,pointer,131068));};
        const auto encode=[&](std::u16string_view value){if(wide)return utf16_bytes(value);const auto s=utf16_to_utf8(value);std::vector<std::uint8_t> bytes(s.begin(),s.end());bytes.push_back(0);return bytes;};
        const auto input=[&](std::uint64_t pointer){require(pointer);memory.check(GuestAddress{pointer},16+4*unit,Access::Read);Resource value;value.type=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+4},32));
            const auto field=[&](unsigned index){return memory.load(GuestAddress{pointer+16+index*unit},bits);};if(field(0))value.local=text(field(0));if(field(1))value.remote=text(field(1));if(field(3))value.provider=text(field(3));require(value.provider.empty() || canonical(value.provider)==canonical(std::u16string(provider_name)),1204);return value;
        };
        const auto packed=[&](const Resource& row){PackedResource value;value.value=row;value.strings.resize(4);if(row.local)value.strings[0]=encode(*row.local);if(row.remote)value.strings[1]=encode(*row.remote);if(row.comment)value.strings[2]=encode(*row.comment);value.strings[3]=encode(row.provider);return value;};
        const auto credentials=[&](std::u16string_view server){const auto sessions=mpr_->credentials.find(scope);if(sessions!=mpr_->credentials.end()){const auto found=sessions->second.find(canonical(std::u16string(server)));if(found!=sessions->second.end())return found->second;}Credentials value;value.user=guest_default_environment(true).at(u"USERNAME").second;return value;};
        const auto external=[&]{if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許原生網路資源 I/O");};
        const auto available_share=[&](const Path& path){for(const auto& row:filesystem_.network_files()->connections(scope))if(canonical(row->remote)==canonical(path.remote()))return true;external();const auto shares=mpr_->backend->shares(path.server,credentials(path.server));if(shares.error)throw Error{shares.error};return std::any_of(shares.values.begin(),shares.values.end(),[&](const auto& share){return !share.printer && canonical(share.name)==canonical(path.share);});};
        const auto sid=token_user_sid_string(effective_token_id());const auto profile_key=sid==token_user_sid_string(1)?std::u16string(1,1)+u"\\network":std::u16string(1,3)+u"\\"+utf8_to_utf16(sid)+u"\\network";
        const auto profile=[&](char drive,const Resource* resource,std::u16string_view user){RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_);if(transaction.error)return transaction.error;const auto key=canonical(profile_key+u"\\"+std::u16string(1,static_cast<char16_t>(drive)));auto old=runtime_.registry;
            if(resource){auto& values=runtime_.registry.keys[key];values[u"remotepath"]={1,utf16_bytes(*resource->remote),u"RemotePath"};values[u"providername"]={1,utf16_bytes(provider_name),u"ProviderName"};values[u"username"]={1,utf16_bytes(user),u"UserName"};runtime_.registry.display_names[key]=profile_key+u"\\"+std::u16string(1,static_cast<char16_t>(drive));runtime_.registry.modified_times[key]=runtime_.system_time_state().filetime;}
            else{runtime_.registry.keys.erase(key);runtime_.registry.display_names.erase(key);runtime_.registry.modified_times.erase(key);}const auto error=transaction.commit();if(error)runtime_.registry=std::move(old);return error;
        };
        if(symbol=="WNetCloseEnum"){const auto found=mpr_->enumerations.find(arg(0));if(found==mpr_->enumerations.end() || found->second.scope!=scope)return done(6);mpr_->enumerations.erase(found);return done(0);}
        if(symbol.starts_with("WNetOpenEnum")){
            const auto requested=static_cast<std::uint32_t>(arg(0)),type=static_cast<std::uint32_t>(arg(1)),usage=static_cast<std::uint32_t>(arg(2));require((requested==1 || requested==2 || requested==3 || requested==5) && type<=2 && !(usage&~0x1fU) && arg(4));memory.check(GuestAddress{arg(4)},unit,Access::Write);std::vector<Resource> values;
            if(requested==1){for(const auto& row:filesystem_.network_files()->connections(scope)){auto value=share_resource(*parse_path(row->remote));value.scope=1;if(row->drive)value.local=std::u16string(1,static_cast<char16_t>(row->drive))+u":";values.push_back(std::move(value));}}
            else if(requested==3){RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return done(transaction.error);const auto prefix=canonical(profile_key)+u"\\";for(const auto& [key,data]:runtime_.registry.keys)if(key.starts_with(prefix) && key.size()==prefix.size()+1){const auto remote=data.find(u"remotepath");if(remote==data.end() || remote->second.type!=1)continue;const auto& bytes=remote->second.bytes;std::u16string name;for(std::size_t n=0;n+1<bytes.size();n+=2){const auto c=static_cast<char16_t>(bytes[n]|static_cast<unsigned>(bytes[n+1])<<8);if(!c)break;name+=c;}const auto parsed=parse_path(name);if(!parsed)continue;auto value=share_resource(*parsed);value.scope=3;value.local=std::u16string(1,static_cast<char16_t>(key.back()>=u'a' && key.back()<=u'z'?key.back()-32:key.back()))+u":";values.push_back(std::move(value));}}
            else if(!arg(3) && requested==2)values.push_back(root_resource());
            else{
                const auto parent=arg(3)?input(arg(3)):root_resource();if(!parent.remote || canonical(*parent.remote)==canonical(std::u16string(provider_name))){external();auto servers=mpr_->backend->servers();if(servers.error)return done(servers.error);std::set<std::u16string> seen;for(auto& server:servers.values)if(seen.insert(canonical(server.name)).second){auto value=server_resource(server.name);value.comment=server.comment;values.push_back(std::move(value));}}
                else{const auto path=parse_path(*parent.remote);require(path.has_value(),67);require(path->share.empty(),1207);external();auto shares=mpr_->backend->shares(path->server,credentials(path->server));if(shares.error)return done(shares.error);for(auto& share:shares.values)if(!share.hidden){auto value=share_resource(Path{path->server,share.name,{}});value.type=share.printer?2U:1U;value.display=share.printer?5U:3U;value.comment=share.comment;values.push_back(std::move(value));}}
            }
            std::erase_if(values,[&](const auto& value){return !resource_type(value,type) || (usage && !(value.usage&usage));});require(mpr_->enumerations.size()<256,8);const auto handle=mpr_->next_enumeration++;mpr_->enumerations.emplace(handle,MprState::Enumeration{scope,std::move(values),0});memory.store(GuestAddress{arg(4)},bits,handle);return done(0);
        }
        if(symbol.starts_with("WNetEnumResource")){
            const auto found=mpr_->enumerations.find(arg(0));require(found!=mpr_->enumerations.end() && found->second.scope==scope,6);require(arg(1) && arg(2) && arg(3));const auto maximum=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));const auto capacity=memory.load(GuestAddress{arg(3)},32);require(maximum);auto& enumeration=found->second;
            if(enumeration.position>=enumeration.values.size()){memory.store(GuestAddress{arg(1)},32,0);return done(259);}std::vector<PackedResource> rows;
            for(std::size_t n=enumeration.position;n<enumeration.values.size() && rows.size()<maximum;++n){rows.push_back(packed(enumeration.values[n]));if(packed_size(rows,unit)>capacity){rows.pop_back();break;}}
            if(rows.empty()){const std::array first{packed(enumeration.values[enumeration.position])};memory.store(GuestAddress{arg(1)},32,0);memory.store(GuestAddress{arg(3)},32,packed_size(first,unit));return done(234);}const auto bytes=pack(rows,unit,arg(2));memory.write(GuestAddress{arg(2)},bytes);memory.store(GuestAddress{arg(1)},32,rows.size());enumeration.position+=rows.size();return done(0);
        }
        if(symbol.starts_with("WNetGetResourceParent") || symbol.starts_with("WNetGetResourceInformation")){
            auto value=input(arg(0));require(value.remote && !value.remote->empty(),67);require(arg(1) && arg(2));const bool parent=symbol.starts_with("WNetGetResourceParent");Resource output;output.scope=0;std::u16string system;
            if(canonical(*value.remote)==canonical(std::u16string(provider_name))){output=root_resource();output.scope=0;if(parent)output.remote.reset();}
            else{const auto path=parse_path(*value.remote);require(path.has_value(),67);require(value.type==0 || value.type==1,66);
                if(parent)output=path->share.empty()?root_resource():server_resource(path->server);
                else if(path->share.empty())output=server_resource(path->server);
                else{require(available_share(*path),67);output=share_resource(*path);system=path->system;}output.scope=0;
            }
            const std::array rows{packed(output)};const auto extra=parent?std::vector<std::uint8_t>{}:encode(system);const auto required=packed_size(rows,unit)+extra.size();const auto capacity=memory.load(GuestAddress{arg(2)},32);if(capacity<required){memory.store(GuestAddress{arg(2)},32,required);return done(234);}auto bytes=pack(rows,unit,arg(1));if(!parent && arg(3))memory.store(GuestAddress{arg(3)},bits,arg(1)+bytes.size());bytes.insert(bytes.end(),extra.begin(),extra.end());memory.write(GuestAddress{arg(1)},bytes);return done(0);
        }
        if(symbol.starts_with("WNetAddConnection2")){
            auto value=input(arg(0));const auto flags=static_cast<std::uint32_t>(arg(3));require(!(flags&~0x3b9fU) && value.type<=2);require(value.remote && !value.remote->empty(),67);const auto path=parse_path(*value.remote);require(path && !path->share.empty() && path->system.empty(),67);require(value.type!=2,66);char drive=0;
            if(value.local && !value.local->empty()){require(value.local->size()==2 && (*value.local)[1]==u':',1200);auto c=(*value.local)[0];if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);require(c>=u'A' && c<=u'Z',1200);drive=static_cast<char>(c);require(filesystem_.information(std::string(1,drive)+":/").error!=0,85);}
            if((flags&0x80U) && !drive){require(value.type!=0);for(char c='Z';c>='C';--c)if(filesystem_.information(std::string(1,c)+":/").error){drive=c;break;}require(drive,85);}
            auto auth=credentials(path->server);if(arg(2))auth.user=text(arg(2));if(arg(1))auth.password=text(arg(1));auth.guest=auth.user.empty();external();const auto existing=filesystem_.network_files()->connections(scope);
            for(const auto& row:existing){const auto remote=parse_path(row->remote);if(remote && canonical(remote->server)==canonical(path->server))require(canonical(row->user)==canonical(auth.user),1219);}
            const auto directory=filesystem_.network_mount_directory(std::to_string(mpr_->next_enumeration++));auto connection=mpr_->backend->connect(*path,auth,directory,!filesystem_.writable());if(connection.error)return done(connection.error);require(connection.mount!=nullptr,1208);auto row=std::make_shared<NetworkFileBinding>();row->remote=path->remote();row->drive=drive;row->user=auth.user;row->readonly=!filesystem_.writable();row->mount=connection.mount;
            const auto added=filesystem_.network_files()->add(scope,row);if(added){(void)connection.mount->disconnect(false);return done(added);}if((flags&1U) && !(flags&4U) && drive){const auto error=profile(drive,&value,auth.user);if(error){(void)filesystem_.network_files()->remove(scope,row->remote,true);return done(error);}}mpr_->credentials[scope][canonical(path->server)]=std::move(auth);return done(0);
        }
        if(symbol.starts_with("WNetCancelConnection2")){require(arg(0) && !(arg(1)&~1U));const auto name=text(arg(0));const auto values=filesystem_.network_files()->connections(scope);char drive=0;for(const auto& row:values)if(canonical(row->remote)==canonical(name) || (row->drive && canonical(name)==canonical(std::u16string(1,static_cast<char16_t>(row->drive))+u":")))drive=row->drive;const auto error=filesystem_.network_files()->remove(scope,name,arg(2)!=0);if(error)return done(error);return done((arg(1)&1U) && drive?profile(drive,nullptr,{}):0);}
        if(symbol.starts_with("WNetGetConnection") || symbol.starts_with("WNetGetUser")){
            require(arg(1) && arg(2));const auto name=text(arg(0));const auto values=filesystem_.network_files()->connections(scope);const auto found=std::find_if(values.begin(),values.end(),[&](const auto& row){return canonical(row->remote)==canonical(name) || (row->drive && canonical(name)==canonical(std::u16string(1,static_cast<char16_t>(row->drive))+u":"));});
            const bool user=symbol.starts_with("WNetGetUser");if(found==values.end() && !(user && name.empty()))return done(2250);const auto value=found==values.end()?credentials({}).user:user?(*found)->user:(*found)->remote;const auto bytes=encode(value);const auto count=bytes.size()/(wide?2U:1U);if(memory.load(GuestAddress{arg(2)},32)<count){memory.store(GuestAddress{arg(2)},32,count);return done(234);}memory.write(GuestAddress{arg(1)},bytes);return done(0);
        }
        if(symbol.starts_with("WNetGetUniversalName")){
            require(arg(0) && arg(2) && arg(3) && (arg(1)==1 || arg(1)==2));const auto location=filesystem_.network_files()->locate(scope,text(arg(0)));require(location.matched && !location.error && location.binding,2250);const auto remote=location.binding->remote,remaining=location.suffix.empty()?std::u16string{}:u"\\"+location.suffix;std::vector<std::vector<std::uint8_t>> strings{encode(remote+remaining)};if(arg(1)==2){strings.push_back(encode(remote));strings.push_back(encode(remaining));}std::size_t required=strings.size()*unit;for(const auto& bytes:strings)required+=bytes.size();if(memory.load(GuestAddress{arg(3)},32)<required){memory.store(GuestAddress{arg(3)},32,required);return done(234);}std::vector<std::uint8_t> output(required);std::size_t tail=strings.size()*unit;for(std::size_t n=0;n<strings.size();++n){const auto pointer=arg(2)+tail;for(unsigned b=0;b<unit;++b)output[n*unit+b]=static_cast<std::uint8_t>(pointer>>(b*8));std::copy(strings[n].begin(),strings[n].end(),output.begin()+static_cast<std::ptrdiff_t>(tail));tail+=strings[n].size();}memory.write(GuestAddress{arg(2)},output);return done(0);
        }
        throw HostCallFault("MPR 入口缺少對應實作");
    }catch(const Error& error){return done(error.code);}catch(const SecurityDescriptorError& error){return done(error.code);}catch(const std::bad_alloc&){return done(8);}catch(const std::length_error&){return done(534);}catch(const std::invalid_argument&){return done(87);}
}
}
