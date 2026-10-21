#include "x64arm/windows_services.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/unicode.hpp"
#include "type_library_guest.hpp"
#include "x64arm/pe_loader.hpp"
#include <algorithm>
#include <charconv>
#include <set>
#include <functional>

namespace x64arm {
bool TypeLibraryView::operator==(const TypeLibraryView& other)const{
    return path==other.path && library==other.library && index==other.index && references==other.references && modules==other.modules &&
        (data && other.data?*data==*other.data:!data && !other.data);
}
namespace {
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,nomemory=0x8007000eU,
    nointerface=0x80004002U,notimpl=0x80004001U,cantload=0x80029c4aU,notregistered=0x8002801dU,
    registry_error=0x8002801cU,element_missing=0x8002802bU,badindex=0x8002000bU,unknown_name=0x80020006U;
constexpr WindowsGuid unknown_iid{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr WindowsGuid library_iid{2,4,2,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr WindowsGuid compiler_iid{3,4,2,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr WindowsGuid info_iid{1,4,2,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
[[noreturn]] void failure(std::uint32_t code){throw typelib::Error(code,"型別庫操作未成功");}
std::u16string folded(std::u16string value){for(auto& c:value)c=unicode_case_unit(c,false);return value;}
std::u16string wide(std::string_view value){return utf8_to_utf16_lossy(value);}
bool matches(std::string_view source,std::u16string_view query){return folded(wide(source))==folded(std::u16string(query));}
std::u16string hex(std::uint32_t value){char buffer[9]{};const auto result=std::to_chars(buffer,buffer+8,value,16);return wide(std::string_view(buffer,static_cast<std::size_t>(result.ptr-buffer)));}
std::optional<unsigned> unhex(std::u16string_view value){
    if(value.empty() || value.size()>8)return {};std::string bytes;for(const auto c:value){if(c>127)return {};bytes+=static_cast<char>(c);}
    unsigned result{};const auto parsed=std::from_chars(bytes.data(),bytes.data()+bytes.size(),result,16);
    if(parsed.ec!=std::errc{} || parsed.ptr!=bytes.data()+bytes.size())return {};return result;
}
struct LibraryPath {std::u16string file;std::optional<std::uint16_t> resource;};
LibraryPath library_path(std::u16string_view path){
    LibraryPath result{std::u16string(path),{}};const auto slash=path.find_last_of(u"\\/");
    if(slash==path.npos || slash+1==path.size())return result;
    const auto tail=path.substr(slash+1);std::uint32_t value{};
    if(!std::all_of(tail.begin(),tail.end(),[](char16_t c){return c>=u'0' && c<=u'9';}))return result;
    for(const auto c:tail){value=value*10+static_cast<unsigned>(c-u'0');if(value>0xffff)failure(cantload);}
    result.file.resize(slash);result.resource=static_cast<std::uint16_t>(value);return result;
}
std::string library_directory(std::string_view path){
    const auto file=utf16_to_utf8(library_path(wide(path)).file);const auto slash=file.find_last_of("/\\");
    return slash==file.npos?std::string{}:file.substr(0,slash+1);
}
std::u16string registry_root(const WindowsGuid& guid){return std::u16string(1,0)+u"\\typelib\\"+folded(windows_guid_text(guid));}
const typelib::TypeInfo* by_reference(const typelib::Library& lib,std::uint32_t ref){
    const auto found=std::find_if(lib.types.begin(),lib.types.end(),[&](const auto& t){return t.reference==ref;});return found==lib.types.end()?nullptr:&*found;
}
struct ReferencedType {const typelib::Library* library{};const typelib::TypeInfo* info{};};
using ResolveReference=std::function<ReferencedType(const typelib::Library&,std::uint32_t)>;
struct ReferenceLeases {
    std::vector<std::uint64_t> owners;
    std::function<void(std::uint64_t)> release;
    ~ReferenceLeases(){for(auto i=owners.rbegin();i!=owners.rend();++i)release(*i);}
};
struct Member {const typelib::Library* library{};const typelib::TypeInfo* type{};const typelib::Function* function{};const typelib::Variable* variable{};};
Member member(const typelib::Library& lib,const typelib::TypeInfo& type,std::int32_t id,const ResolveReference& resolve,std::optional<std::u16string_view> name={},unsigned depth=0){
    if(depth>64)failure(typelib::invalid_data);
    for(const auto& f:type.functions)if(name?matches(f.doc.name,*name):f.id==id)return {&lib,&type,&f,{}};
    for(const auto& v:type.variables)if(name?matches(v.doc.name,*name):v.id==id)return {&lib,&type,{},&v};
    if(type.kind==3 || type.kind==4)for(const auto& impl:type.implementations){
        auto base=ReferencedType{&lib,by_reference(lib,impl.reference)};if(!base.info)base=resolve(lib,impl.reference);
        const auto found=member(*base.library,*base.info,id,resolve,name,depth+1);if(found.type)return found;
    }
    return {};
}
struct Binding {Member target;bool mismatch{};};
Binding bind_member(const typelib::Library& lib,const typelib::TypeInfo& type,std::u16string_view name,std::uint16_t flags,const ResolveReference& resolve,unsigned depth=0){
    if(depth>64)failure(typelib::invalid_data);bool mismatch=false;
    for(const auto& f:type.functions)if(matches(f.doc.name,name)){
        if(!flags || (flags&f.invocation))return {{&lib,&type,&f,{}},false};mismatch=true;
    }
    for(const auto& v:type.variables)if(matches(v.doc.name,name)){
        if(!flags || (flags&2))return {{&lib,&type,{},&v},false};mismatch=true;
    }
    if(mismatch)return {{},true};
    if(type.kind==3 || type.kind==4 || type.kind==5)for(const auto& impl:type.implementations){
        if(type.kind==5 && (!(impl.flags&1) || (impl.flags&2)))continue;
        auto base=ReferencedType{&lib,by_reference(lib,impl.reference)};if(!base.info)base=resolve(lib,impl.reference);
        const auto result=bind_member(*base.library,*base.info,name,flags,resolve,depth+1);if(result.target.type || result.mismatch)return result;
    }
    return {};
}
}
std::span<const WindowsServiceDescriptor> type_library_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"internal-type-compiler","QueryInterface",3},{"internal-type-compiler","AddRef",1},{"internal-type-compiler","Release",1},
        {"internal-type-compiler","Bind",7},{"internal-type-compiler","BindType",5},
        {"oleaut32.dll","LoadTypeLib",2},{"oleaut32.dll","LoadRegTypeLib",5},{"oleaut32.dll","LoadTypeLibEx",3},
        {"oleaut32.dll","UnRegisterTypeLib",5},
        {"internal-type-library","QueryInterface",3},{"internal-type-library","AddRef",1},{"internal-type-library","Release",1},
        {"internal-type-library","GetTypeInfoCount",1},{"internal-type-library","GetTypeInfo",3},{"internal-type-library","GetTypeInfoType",3},
        {"internal-type-library","GetTypeInfoOfGuid",3},{"internal-type-library","GetLibAttr",2},{"internal-type-library","GetTypeComp",2},
        {"internal-type-library","GetDocumentation",6},{"internal-type-library","IsName",4},{"internal-type-library","FindName",6},{"internal-type-library","ReleaseTLibAttr",2},
        {"internal-type-info","QueryInterface",3},{"internal-type-info","AddRef",1},{"internal-type-info","Release",1},
        {"internal-type-info","GetTypeAttr",2},{"internal-type-info","GetTypeComp",2},{"internal-type-info","GetFuncDesc",3},{"internal-type-info","GetVarDesc",3},
        {"internal-type-info","GetNames",5},{"internal-type-info","GetRefTypeOfImplType",3},{"internal-type-info","GetImplTypeFlags",3},
        {"internal-type-info","GetIDsOfNames",4},{"internal-type-info","Invoke",8},{"internal-type-info","GetDocumentation",6},
        {"internal-type-info","GetDllEntry",6},{"internal-type-info","GetRefTypeInfo",3},{"internal-type-info","AddressOfMember",4},
        {"internal-type-info","CreateInstance",4},{"internal-type-info","GetMops",3},{"internal-type-info","GetContainingTypeLib",3},
        {"internal-type-info","ReleaseTypeAttr",2},{"internal-type-info","ReleaseFuncDesc",2},{"internal-type-info","ReleaseVarDesc",2}
    };return table;
}
std::uint64_t WindowsServices::load_type_library(GuestState& state,GuestMemory& memory,std::u16string_view requested){
    auto& bridge=runtime_.type_libraries;if(requested.empty())failure(invalid);
    const auto target=library_path(requested);
    std::vector<std::u16string> candidates{target.file};
    if(target.file.find_first_of(u"\\/:")==target.file.npos){
        if(!runtime_.application_directory.empty())candidates.push_back(runtime_.application_directory+u"\\"+target.file);
        candidates.push_back(u"C:\\Windows\\System32\\"+target.file);
    }
    OpenHostFile source;std::string requested_key;
    for(const auto& candidate:candidates){
        source=filesystem_.open_reader(utf16_to_utf8(candidate),runtime_.application_source,utf16_to_utf8(runtime_.application_directory));
        requested_key=source.key+(target.resource?"\\"+std::to_string(*target.resource):std::string{});
        if(const auto cached=bridge.loaded_paths.find(requested_key);cached!=bridge.loaded_paths.end()){
            auto& view=bridge.views.at(cached->second);if(view.references==UINT32_MAX)failure(nomemory);++view.references;return cached->second;
        }
        if(!source.error)break;
    }
    if(source.error || !source.file || source.metadata.size>256U*1024U*1024U)failure(cantload);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(source.metadata.size));const auto read=source.file->read(bytes,0);
    if(read.error || read.bytes!=bytes.size())failure(typelib::invalid_data);
    std::string key=source.key;
    if(bytes.size()>=2 && bytes[0]=='M' && bytes[1]=='Z'){
        std::optional<PeResource> resource;
        try{resource=read_pe_resource(bytes,u"TYPELIB",target.resource);}catch(const std::invalid_argument&){failure(typelib::invalid_data);}
        if(!resource)failure(cantload);
        if(resource->id)key+="\\"+std::to_string(*resource->id);
        bytes=std::move(resource->bytes);
    }else if(target.resource)failure(cantload);
    for(auto& [pointer,view]:bridge.views)if(view.index==-1 && view.path==key){if(view.references==UINT32_MAX)failure(nomemory);bridge.loaded_paths.emplace(requested_key,pointer);++view.references;return pointer;}
    auto data=std::make_shared<typelib::Library>(typelib::read(bytes));
    // 描述中的 instance offset 及 vtable offset 目前沿用磁碟 ABI；禁止靜默提供錯誤跨架構資料。
    if(data->system!=(x86_?1:3))failure(typelib::unsupported_format);
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto table=[&](std::string_view family,std::uint64_t& target,unsigned count){
        if(target)return;if(runtime_.arena_next+count*unit>1024U*1024U)failure(nomemory);
        const auto at=runtime_allocate(memory,{},count*unit);unsigned index{};
        for(const auto& d:type_library_service_descriptors())if(d.family==family)memory.store(GuestAddress{at+unit*index++},bits,resolve(d.family,d.symbol).value);
        target=at;
    };
    table("internal-type-library",bridge.library_table,13);table("internal-type-info",bridge.info_table,22);table("internal-type-compiler",bridge.compiler_table,5);
    if(bridge.views.size()>=4096)failure(nomemory);const auto object=allocate_heap(memory,unit);if(!object)failure(nomemory);
    try{auto paths=bridge.loaded_paths;paths.emplace(requested_key,object);paths.emplace(key,object);
        memory.store(GuestAddress{object},bits,bridge.library_table);bridge.views.emplace(object,TypeLibraryView{std::move(data),key,object,-1,1});bridge.loaded_paths.swap(paths);}
    catch(...){(void)free_heap(memory,object,state);throw;}
    return object;
}
namespace {
void dispatch_function(typelib::Function& function){
            function.kind=4;function.vtable_offset=0;
            if(function.result.vartype==25){function.result={};function.result.vartype=24;}
            bool retval=false;
            for(const auto& parameter:function.parameters)if(parameter.flags&8U){
                if(retval || parameter.type.vartype!=26 || !parameter.type.element)failure(typelib::invalid_data);
                function.result=*parameter.type.element;retval=true;
            }
            std::erase_if(function.parameters,[&](const auto& parameter){
                if(!(parameter.flags&12U))return false;
                if((parameter.flags&16U) && function.optional_parameters>0)--function.optional_parameters;return true;
            });
}
}

std::uint64_t WindowsServices::type_library_info(GuestMemory& memory,std::uint64_t library,std::uint32_t index,bool native){
    auto& bridge=runtime_.type_libraries;auto& parent=bridge.views.at(library);
    if(index>=parent.data->types.size())failure(badindex);
    const auto& type=parent.data->types[index];if(native && (type.kind!=4 || !(type.flags&0x40U)))failure(invalid);
    const auto kind=native?3:type.kind;
    for(auto& [pointer,view]:bridge.views)if(view.library==library && view.index==static_cast<std::int32_t>(index) && view.data->types[index].kind==kind){
        if(view.references==UINT32_MAX)failure(nomemory);++view.references;return pointer;
    }
    if(bridge.views.size()>=4096 || parent.references==UINT32_MAX)failure(nomemory);
    auto data=parent.data;if(type.kind==4 && (type.flags&0x40U)){
        auto copy=std::make_shared<typelib::Library>(*data);
        for(auto& selected:copy->types){
        if(selected.kind!=4 || !(selected.flags&0x40U))continue;
        if(native)selected.kind=3;
        else for(auto& function:selected.functions){
            dispatch_function(function);
        }
        }
        data=std::move(copy);
    }
    const auto object=allocate_heap(memory,x86_?4:8);if(!object)failure(nomemory);
    try{memory.store(GuestAddress{object},x86_?32:64,bridge.info_table);bridge.views.emplace(object,TypeLibraryView{std::move(data),parent.path,library,static_cast<std::int32_t>(index),1});++parent.references;}
    catch(...){GuestState unused;(void)free_heap(memory,object,unused);throw;}return object;
}
std::uint64_t WindowsServices::reference_type_library(GuestState& state,GuestMemory& memory,std::uint64_t owner,std::uint32_t ref){
    auto& bridge=runtime_.type_libraries;const auto& view=bridge.views.at(owner);const auto& lib=*view.data;
    if(ref==UINT32_MAX && view.index>=0){const auto& type=lib.types.at(static_cast<std::size_t>(view.index));if((type.flags&0x40U) && (type.kind==3 || type.kind==4))return type_library_info(memory,view.library,static_cast<std::uint32_t>(view.index),type.kind==4);}
    const bool native=view.index>=0 && lib.types.at(static_cast<std::size_t>(view.index)).kind==3 && (lib.types.at(static_cast<std::size_t>(view.index)).flags&0x40U);
    if(const auto* value=by_reference(lib,ref))return type_library_info(memory,view.library,static_cast<std::uint32_t>(value-lib.types.data()),native && (value->flags&0x40U));
    const auto found=std::find_if(lib.external_references.begin(),lib.external_references.end(),[&](const auto& r){return r.reference==ref;});if(found==lib.external_references.end())failure(element_missing);
    const auto& dependency=lib.imports.at(found->import_index);std::u16string path;
    try{path=registered_type_library(dependency.guid,dependency.major,dependency.minor,dependency.lcid);}catch(const typelib::Error& e){if(e.code!=notregistered)throw;path=wide(dependency.file.find(':')!=dependency.file.npos || dependency.file.starts_with('/') || dependency.file.starts_with('\\')?dependency.file:library_directory(view.path)+dependency.file);}
    const auto library=load_type_library(state,memory,path);std::uint64_t result{};
    try{
        const auto& data=*bridge.views.at(library).data;if(data.guid!=dependency.guid)failure(cantload);auto index=found->type_index;
        if(found->guid){const auto t=std::find_if(data.types.begin(),data.types.end(),[&](const auto& value){return value.guid==*found->guid;});if(t==data.types.end())failure(element_missing);index=static_cast<std::uint32_t>(t-data.types.begin());}
        result=type_library_info(memory,library,index,native && index<data.types.size() && (data.types[index].flags&0x40U));
    }catch(...){(void)release_type_library(state,memory,library);throw;}(void)release_type_library(state,memory,library);return result;
}
std::uint32_t WindowsServices::release_type_library(GuestState& state,GuestMemory& memory,std::uint64_t object){
    auto& bridge=runtime_.type_libraries;auto found=bridge.views.find(object);if(found==bridge.views.end())return 0;
    if(found->second.references==1)bridge.pending_unloads.reserve(bridge.pending_unloads.size()+found->second.modules.size());
    const auto remaining=--found->second.references;if(remaining)return remaining;
    const auto library=found->second.library;const bool info=found->second.index>=0;
    for(auto it=bridge.allocations.begin();it!=bridge.allocations.end();)if(it->second.owner==object){
        for(const auto string:it->second.strings)free_bstr(state,memory,string);(void)free_heap(memory,it->first,state);it=bridge.allocations.erase(it);
    }else ++it;
    bridge.pending_unloads.insert(bridge.pending_unloads.end(),found->second.modules.begin(),found->second.modules.end());
    std::erase_if(bridge.loaded_paths,[&](const auto& entry){return entry.second==object;});
    bridge.views.erase(found);(void)free_heap(memory,object,state);
    if(info)(void)release_type_library(state,memory,library);return 0;
}
void WindowsServices::register_type_library(std::uint64_t pointer){
    const auto& view=runtime_.type_libraries.views.at(pointer);const auto& lib=*view.data;
    if(!registry_file_.empty() && !runtime_.network.external_io)failure(registry_error);
    RegistryTransaction transaction(runtime_.registry,registry_file_);if(transaction.error)failure(registry_error);
    auto next=runtime_.registry;const auto version=registry_root(lib.guid)+u"\\"+hex(lib.major)+u"."+hex(lib.minor);
    const auto set=[&](std::u16string key,std::u16string value,std::u16string name=u""){
        key=registry_view_path(std::move(key),registry_view(0,x86_));
        auto bytes=utf16_bytes(value);
        for(std::size_t i=0;i<=key.size();++i)if(i==key.size() || key[i]==u'\\'){
            const auto parent=key.substr(0,i);if(next.keys.size()>=4096 && !next.keys.contains(parent))failure(nomemory);
            next.keys.try_emplace(parent);next.modified_times[parent]=runtime_.system_filetime();
        }
        auto& values=next.keys[key];const auto old=values.find(name);const auto previous=old==values.end()?0:old->second.bytes.size();
        if(next.bytes-previous+bytes.size()>output_limit || (old==values.end() && next.values>=4096))failure(nomemory);
        next.bytes=next.bytes-previous+bytes.size();if(old==values.end())++next.values;values[name]={1,std::move(bytes),{}};
    };
    auto registered_path=wide(view.path);std::replace(registered_path.begin(),registered_path.end(),u'/',u'\\');
    set(version,wide(lib.doc.name));set(version+u"\\"+hex(lib.lcid)+(x86_?u"\\win32":u"\\win64"),registered_path);
    set(version+u"\\flags",hex(lib.flags&~8U));
    auto help=lib.help_file;
    if(help.empty() || (help.find(':')==help.npos && !help.starts_with('/') && !help.starts_with('\\')))help=library_directory(view.path)+help;
    auto directory=wide(library_directory(help));std::replace(directory.begin(),directory.end(),u'/',u'\\');
    set(version+u"\\helpdir",directory);
    bool interfaces=false;
    for(const auto& type:lib.types){
        if(type.kind!=4 && !(type.kind==3 && (type.flags&0x100U)))continue;
        if(std::none_of(type.guid.begin(),type.guid.end(),[](auto byte){return byte!=0;}))continue;
        const auto key=std::u16string(1,0)+u"\\interface\\"+folded(windows_guid_text(type.guid));
        const auto proxy=next.keys.find(registry_view_path(key+u"\\proxystubclsid32",registry_view(0,x86_)));
        // 已安裝的自訂 proxy 由原提供者管理；不以 Automation proxy 覆寫。
        if(type.kind==3 && proxy!=next.keys.end() && proxy->second.contains(u""))continue;
        set(key,wide(type.doc.name));set(key+u"\\typelib",windows_guid_text(lib.guid));set(key+u"\\typelib",hex(lib.major)+u"."+hex(lib.minor),u"version");
        if(proxy==next.keys.end() || !proxy->second.contains(u""))set(key+u"\\proxystubclsid32",u"{00020424-0000-0000-C000-000000000046}");
        interfaces=true;
    }
    const auto previous=runtime_.registry;runtime_.registry=std::move(next);
    if(transaction.commit()){runtime_.registry=previous;failure(registry_error);}
    poll_registry_notifications(false,registry_root(lib.guid),5);
    if(interfaces)poll_registry_notifications(false,registry_view_path(std::u16string(1,0)+u"\\interface",registry_view(0,x86_)),5);
}
std::u16string WindowsServices::registered_type_library(const WindowsGuid& guid,std::uint16_t major,std::uint16_t minor,std::uint32_t lcid){
    if(!registry_file_.empty() && !runtime_.network.external_io)failure(registry_error);
    RegistryTransaction transaction(runtime_.registry,registry_file_);if(transaction.error)failure(registry_error);
    const auto prefix=registry_root(guid)+u"\\";std::optional<unsigned> chosen;
    for(const auto& [path,values]:runtime_.registry.keys){
        (void)values;if(!path.starts_with(prefix))continue;const auto part=std::u16string_view(path).substr(prefix.size());
        if(part.find(u'\\')!=part.npos)continue;const auto dot=part.find(u'.');if(dot==part.npos)continue;
        const auto ma=unhex(part.substr(0,dot)),mi=unhex(part.substr(dot+1));if(!ma || !mi || *ma!=major || *mi<minor || *mi>0xffff)continue;
        if(*mi==minor){chosen=minor;break;}if(!chosen || *mi>*chosen)chosen=*mi;
    }
    if(!chosen)failure(notregistered);const auto version=prefix+hex(major)+u"."+hex(*chosen);
    for(const auto locale:{lcid,lcid&0x3ffU,0U})for(const auto platform:{x86_?u"win32":u"win64",x86_?u"win64":u"win32"}){
        const auto key=runtime_.registry.keys.find(version+u"\\"+hex(locale)+u"\\"+platform);if(key==runtime_.registry.keys.end())continue;
        const auto value=key->second.find(u"");if(value==key->second.end() || value->second.type!=1 || value->second.bytes.size()%2)continue;
        std::u16string result;for(std::size_t at=0;at<value->second.bytes.size();at+=2){const auto c=static_cast<char16_t>(value->second.bytes[at]|unsigned(value->second.bytes[at+1])<<8);if(!c)break;result+=c;}
        if(!result.empty())return result;
    }
    failure(notregistered);
}
std::optional<std::uint32_t> WindowsServices::invoke_type_library(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;auto& bridge=runtime_.type_libraries;
    if(name=="UnRegisterTypeLib"){
        // 移除 TypeLib\{libid}\{major.minor}\{lcid}\{win16|win32|win64}；該版本沒有其他 LCID 時一併移除版本鍵，
        // 該 LIBID 沒有其他版本時移除 GUID 鍵。刪除經 RegDeleteTreeW／RegDeleteKeyW，沿用登錄日誌與 ACL。
        const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        try{
            if(!arg(0))return finish(0x80070057U);
            const auto syskind=static_cast<std::uint32_t>(arg(4));if(syskind>3 || syskind==2)return finish(0x80070057U);
            std::array<std::uint8_t,16> id{};memory.read(GuestAddress{arg(0)},id);
            constexpr std::array order{3U,2U,1U,0U,5U,4U,7U,6U,8U,9U,10U,11U,12U,13U,14U,15U};constexpr std::u16string_view hex=u"0123456789abcdef";
            std::u16string guid=u"{";for(unsigned n=0;n<16;++n){if(n==4 || n==6 || n==8 || n==10)guid+=u'-';guid+=hex[id[order[n]]>>4];guid+=hex[id[order[n]]&15];}guid+=u'}';
            const auto number=[&](std::uint32_t value){std::u16string text;do{text.insert(text.begin(),hex[value&15]);value>>=4;}while(value);return text;};
            const auto version=number(static_cast<std::uint16_t>(arg(1)))+u'.'+number(static_cast<std::uint16_t>(arg(2)));
            const auto lcid=number(static_cast<std::uint32_t>(arg(3)));const std::u16string sys=syskind==0?u"win16":syskind==1?u"win32":u"win64";
            const auto relative=u"typelib\\"+guid;
            bool found=false,lcid_shared=false,version_shared=false,guid_shared=false;unsigned hive=0;
            {
                if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
                RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return finish(0x8002801cU);
                const auto children=[&](const std::u16string& path){std::vector<std::u16string> names;const auto prefix=path+u"\\";
                    for(auto item=runtime_.registry.keys.lower_bound(prefix);item!=runtime_.registry.keys.end() && item->first.starts_with(prefix);++item){const auto rest=item->first.substr(prefix.size());if(rest.find(u'\\')==rest.npos)names.push_back(rest);}
                    return names;};
                for(const unsigned root_hive:{1U,0U,2U}){
                    const auto root=root_hive?std::u16string(1,static_cast<char16_t>(root_hive))+u"\\software\\classes":std::u16string(1,0);
                    const auto base=registry_view_path(root+u"\\"+relative,registry_view(0,x86_));
                    if(!runtime_.registry.keys.contains(base+u"\\"+version+u"\\"+lcid+u"\\"+sys))continue;
                    found=true;hive=root_hive;
                    for(const auto& child:children(base+u"\\"+version+u"\\"+lcid))if(child!=sys)lcid_shared=true;
                    for(const auto& child:children(base+u"\\"+version))if(child!=lcid && child!=u"flags" && child!=u"helpdir")version_shared=true;
                    for(const auto& child:children(base))if(child!=version)guid_shared=true;
                    break;
                }
            }
            if(!found)return finish(0x8002801cU); // TYPE_E_REGISTRYACCESS
            // 在登錄所在的根（HKCU／HKLM 的 Software\\Classes 或 HKCR）刪除，不經 HKCR 合併檢視轉向。
            const std::u16string prefix=hive?u"Software\\Classes\\TypeLib\\":u"TypeLib\\";
            std::vector<std::u16string> trees{prefix+guid+u"\\"+version+u"\\"+lcid+u"\\"+sys};
            if(!lcid_shared)trees.push_back(prefix+guid+u"\\"+version+u"\\"+lcid);
            if(!lcid_shared && !version_shared)trees.push_back(prefix+guid+u"\\"+version);
            if(!lcid_shared && !version_shared && !guid_shared)trees.push_back(prefix+guid);
            std::vector<std::uint8_t> bytes;std::vector<std::uint64_t> offsets;
            for(const auto& path:trees){offsets.push_back(bytes.size());const auto data=utf16_bytes(path);bytes.insert(bytes.end(),data.begin(),data.end());}
            const auto scratch=allocate_heap(memory,bytes.size());if(!scratch)return finish(0x8007000eU);memory.write(GuestAddress{scratch},bytes);
            std::vector<GuestCallback> callbacks;const auto classes_root=(x86_?UINT64_C(0x80000000):UINT64_C(0xffffffff80000000))+hive;
            for(const auto offset:offsets)callbacks.push_back({resolve("advapi32.dll","RegDeleteTreeW").value,{classes_root,scratch+offset}});
            start_callbacks(state,memory,std::move(callbacks),Continuation::ReturnZeroFreeHeap,scratch);return std::nullopt;
        }catch(const MemoryFault&){return finish(0x80070057U);}catch(const std::bad_alloc&){return finish(0x8007000eU);}
    }
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){return finish_type_library_call(state,memory,value);};
    const auto output=[&](std::uint64_t address,std::size_t length){if(!address)failure(pointer_error);memory.check(GuestAddress{address},length,Access::Write);};
    const auto put=[&](std::uint64_t address,std::uint64_t value){memory.store(GuestAddress{address},bits,value);};
    const auto text=[&](std::u16string_view value){auto data=utf16_bytes(value);data.resize(value.size()*2);const auto p=allocate_bstr(memory,data.size(),data);if(!p)failure(nomemory);return p;};
    const auto describe=[&](std::uint64_t owner,std::uint8_t kind,const typelib::Description& description){
        if(bridge.allocations.size()>=4096)failure(nomemory);
        const auto base=allocate_heap(memory,description.bytes.size());if(!base)failure(nomemory);TypeLibraryAllocation allocation{owner,kind,{}};
        try{allocation.strings.reserve(description.strings.size());memory.write(GuestAddress{base},description.bytes);
            for(const auto at:description.pointers){const auto relative=memory.load(GuestAddress{base+at},bits);put(base+at,base+relative);}
            for(const auto& [at,value]:description.strings){const auto string=text(value);allocation.strings.push_back(string);put(base+at,string);}
            bridge.allocations.emplace(base,allocation);
        }catch(...){for(const auto string:allocation.strings)free_bstr(state,memory,string);bridge.allocations.erase(base);(void)free_heap(memory,base,state);throw;}
        return base;
    };
    const auto compiler=[&](std::uint64_t owner){
        for(auto& [address,value]:bridge.compilers)if(value.owner==owner){if(value.references==UINT32_MAX)failure(nomemory);++value.references;return address;}
        auto& view=bridge.views.at(owner);if(bridge.compilers.size()>=4096 || view.references==UINT32_MAX)failure(nomemory);
        const auto address=allocate_heap(memory,unit);if(!address)failure(nomemory);
        try{put(address,bridge.compiler_table);bridge.compilers.emplace(address,TypeLibraryBridge::Compiler{owner,1});++view.references;}
        catch(...){(void)free_heap(memory,address,state);throw;}return address;
    };
    ReferenceLeases dependencies{{},[&](std::uint64_t owner){(void)release_type_library(state,memory,owner);}};
    const auto owner_for=[&](const typelib::Library& data){
        const auto owner=service.family=="internal-type-compiler"?bridge.compilers.at(arg(0)).owner:arg(0);
        if(bridge.views.at(owner).data.get()==&data)return owner;
        const auto found=std::find_if(dependencies.owners.begin(),dependencies.owners.end(),[&](auto value){return bridge.views.at(value).data.get()==&data;});
        if(found==dependencies.owners.end())failure(typelib::invalid_data);return *found;
    };
    const ResolveReference resolve_reference=[&](const typelib::Library& data,std::uint32_t ref){
        const auto target=reference_type_library(state,memory,owner_for(data),ref);
        try{dependencies.owners.push_back(target);}catch(...){(void)release_type_library(state,memory,target);throw;}
        const auto& view=bridge.views.at(target);return ReferencedType{view.data.get(),&view.data->types.at(static_cast<std::size_t>(view.index))};
    };
    try{
        if(service.family=="internal-type-compiler"){
            const auto found=bridge.compilers.find(arg(0));if(found==bridge.compilers.end())return done(invalid);auto& object=found->second;
            if(name=="AddRef"){if(object.references==UINT32_MAX)return done(nomemory);return done(++object.references);}
            if(name=="Release"){
                const auto remaining=--object.references;if(remaining)return done(remaining);const auto owner=object.owner;
                bridge.compilers.erase(found);(void)free_heap(memory,arg(0),state);(void)release_type_library(state,memory,owner);return done(0);
            }
            if(name=="QueryInterface"){
                output(arg(2),unit);put(arg(2),0);if(!arg(1))return done(pointer_error);WindowsGuid iid{};memory.read(GuestAddress{arg(1)},iid);
                if(iid!=unknown_iid && iid!=compiler_iid)return done(nointerface);if(object.references==UINT32_MAX)return done(nomemory);
                ++object.references;put(arg(2),arg(0));return done(0);
            }
            const auto& view=bridge.views.at(object.owner);const auto& lib=*view.data;
            if(name=="BindType"){
                output(arg(3),unit);output(arg(4),unit);put(arg(3),0);put(arg(4),0);if(!arg(1))return done(invalid);
                const auto query=read_utf16(memory,GuestAddress{arg(1)});if(view.index>=0)return done(0);
                for(std::size_t i=0;i<lib.types.size();++i)if(matches(lib.types[i].doc.name,query)){
                    const auto info=type_library_info(memory,view.library,static_cast<std::uint32_t>(i));std::uint64_t comp{};
                    try{comp=compiler(info);}catch(...){(void)release_type_library(state,memory,info);throw;}
                    put(arg(3),info);put(arg(4),comp);return done(0);
                }
                return done(0);
            }
            if(name!="Bind")return done(notimpl);
            output(arg(4),unit);output(arg(5),4);output(arg(6),unit);put(arg(4),0);memory.store(GuestAddress{arg(5)},32,0);put(arg(6),0);
            if(!arg(1))return done(invalid);const auto flags=static_cast<std::uint16_t>(arg(3));if(flags&~15U)return done(invalid);
            const auto query=read_utf16(memory,GuestAddress{arg(1)});Binding binding;
            if(view.index>=0)binding=bind_member(lib,lib.types[static_cast<std::size_t>(view.index)],query,flags,resolve_reference);
            else{
                for(std::size_t i=0;i<lib.types.size();++i){const auto& type=lib.types[i];
                    if((type.kind==0 || type.kind==2 || type.kind==5) && matches(type.doc.name,query)){
                        const auto info=type_library_info(memory,view.library,static_cast<std::uint32_t>(i));std::uint64_t comp{};
                        try{comp=compiler(info);}catch(...){(void)release_type_library(state,memory,info);throw;}
                        (void)release_type_library(state,memory,info);put(arg(6),comp);memory.store(GuestAddress{arg(5)},32,3);return done(0);
                    }
                }
                for(const auto& type:lib.types)if(type.kind==0 || type.kind==2 || (type.kind==5 && (type.flags&1))){
                    const auto match=bind_member(lib,type,query,flags,resolve_reference);binding.mismatch|=match.mismatch;
                    if(match.target.type){if(type.kind==5)return done(notimpl);binding.target=match.target;break;}
                }
            }
            if(!binding.target.type)return done(binding.mismatch?0x80028ca0U:0);
            const auto& member=binding.target;const auto& source=bridge.views.at(owner_for(*member.library));const auto info=type_library_info(memory,source.library,static_cast<std::uint32_t>(member.type-member.library->types.data()),member.type->kind==3 && (member.type->flags&0x40U));std::uint64_t description{};
            try{
                const auto* function=member.function;
                if(function){const auto& bound=bridge.views.at(info);const auto& functions=bound.data->types.at(static_cast<std::size_t>(bound.index)).functions;
                    const auto selected_function=std::find_if(functions.begin(),functions.end(),[&](const auto& candidate){return candidate.id==function->id && candidate.invocation==function->invocation;});
                    if(selected_function==functions.end())failure(typelib::invalid_data);function=&*selected_function;
                }
                description=describe(info,function?2:3,function?typelib::function_description(*function,unit):typelib::variable_description(*member.variable,unit));
            }
            catch(...){(void)release_type_library(state,memory,info);throw;}
            put(arg(4),info);memory.store(GuestAddress{arg(5)},32,member.function?1:2);put(arg(6),description);return done(0);
        }
        if(service.family=="oleaut32.dll"){
            const bool registered=name=="LoadRegTypeLib";const auto destination=arg(registered?4:name=="LoadTypeLibEx"?2:1);
            if(!arg(0) || !destination)return done(invalid);output(destination,unit);put(destination,0);
            std::u16string path;std::uint32_t kind=name=="LoadTypeLibEx"?static_cast<std::uint32_t>(arg(1)):0;if(!registered && kind>2)return done(invalid);
            if(registered){WindowsGuid guid{};memory.read(GuestAddress{arg(0)},guid);path=registered_type_library(guid,static_cast<std::uint16_t>(arg(1)),static_cast<std::uint16_t>(arg(2)),static_cast<std::uint32_t>(arg(3)));}
            else path=read_utf16(memory,GuestAddress{arg(0)});
            const auto object=load_type_library(state,memory,path);
            try{if(!registered && (kind==1 || (kind==0 && path.find_first_of(u"\\/:")==path.npos)))register_type_library(object);put(destination,object);}
            catch(...){(void)release_type_library(state,memory,object);throw;}return done(0);
        }
        const auto it=bridge.views.find(arg(0));if(it==bridge.views.end())return done(invalid);
        auto& view=it->second;const auto& lib=*view.data;const auto* info=view.index<0?nullptr:&lib.types[static_cast<std::size_t>(view.index)];
        if((service.family=="internal-type-info")!=bool(info))return done(invalid);
        if(name=="AddRef"){if(view.references==UINT32_MAX)return done(nomemory);return done(++view.references);}
        if(name=="Release")return done(release_type_library(state,memory,arg(0)));
        if(name=="QueryInterface"){
            output(arg(2),unit);put(arg(2),0);if(!arg(1))return done(pointer_error);WindowsGuid iid{};memory.read(GuestAddress{arg(1)},iid);
            if(iid!=unknown_iid && iid!=(info?info_iid:library_iid))return done(nointerface);
            if(view.references==UINT32_MAX)return done(nomemory);++view.references;put(arg(2),arg(0));return done(0);
        }
        if((name.starts_with("Release") && name.ends_with("Attr")) || name=="ReleaseFuncDesc" || name=="ReleaseVarDesc"){
            const auto kind=static_cast<std::uint8_t>(name=="ReleaseTLibAttr"?0:name=="ReleaseTypeAttr"?1:name=="ReleaseFuncDesc"?2:3);
            const auto found=bridge.allocations.find(arg(1));if(found==bridge.allocations.end() || found->second.owner!=arg(0) || found->second.kind!=kind)return done(0);
            for(const auto string:found->second.strings)free_bstr(state,memory,string);(void)free_heap(memory,arg(1),state);bridge.allocations.erase(found);return done(0);
        }
        if(name=="GetLibAttr" || name=="GetTypeAttr" || name=="GetFuncDesc" || name=="GetVarDesc"){
            const bool indexed=name=="GetFuncDesc" || name=="GetVarDesc";const auto destination=arg(indexed?2:1);output(destination,unit);put(destination,0);
            const auto index=static_cast<std::uint32_t>(arg(1));typelib::Description description;std::uint8_t kind{};
            const auto original_data=view.data;std::shared_ptr<const typelib::Library> mapped_data;
            std::vector<Member> inherited;
            const bool dual=info && info->kind==4 && (info->flags&0x40U) && (name=="GetTypeAttr" || name=="GetFuncDesc");
            if(dual){
                const auto collect=[&](const auto& self,const typelib::Library& source,const typelib::TypeInfo& type,unsigned depth)->void{
                    if(depth>64)failure(typelib::invalid_data);
                    for(const auto& base:type.implementations){auto target=ReferencedType{&source,by_reference(source,base.reference)};if(!target.info)target=resolve_reference(source,base.reference);self(self,*target.library,*target.info,depth+1);}
                    for(const auto& function:type.functions){if(inherited.size()>=65535)failure(typelib::invalid_data);inherited.push_back({&source,&type,&function,nullptr});}
                };
                collect(collect,lib,*info,0);
            }
            if(name=="GetLibAttr")description=typelib::library_description(lib);
            else if(name=="GetTypeAttr"){kind=1;description=typelib::type_description(lib,static_cast<std::size_t>(view.index),unit);
                if(dual){const unsigned at=unit==4?44:48;description.bytes[at]=static_cast<std::uint8_t>(inherited.size());description.bytes[at+1]=static_cast<std::uint8_t>(inherited.size()>>8);}
            }
            else if(name=="GetFuncDesc"){
                if(index>=(dual?inherited.size():info->functions.size()))return done(badindex);kind=2;
                if(dual){
                    auto function=*inherited[index].function;
                    if(inherited[index].library!=&lib){
                        auto mapped=std::make_shared<typelib::Library>(lib);
                        const auto remap=[&](const auto& self,typelib::Type& type,const typelib::Library& source,unsigned depth)->void{
                            if(depth>64)failure(typelib::invalid_data);
                            if(type.element){auto element=std::make_shared<typelib::Type>(*type.element);self(self,*element,source,depth+1);type.element=std::move(element);}
                            if(type.vartype!=29)return;
                            auto target=ReferencedType{&source,by_reference(source,type.reference)};if(!target.info)target=resolve_reference(source,type.reference);
                            const auto& data=*target.library;
                            if(data.guid==mapped->guid && data.major==mapped->major && data.minor==mapped->minor && by_reference(*mapped,target.info->reference)){type.reference=target.info->reference;return;}
                            auto imported=std::find_if(mapped->imports.begin(),mapped->imports.end(),[&](const auto& entry){return entry.guid==data.guid && entry.major==data.major && entry.minor==data.minor && entry.lcid==data.lcid;});
                            std::uint32_t import_index{};
                            if(imported==mapped->imports.end()){
                                if(mapped->imports.size()>=4096)failure(nomemory);import_index=static_cast<std::uint32_t>(mapped->imports.size());
                                typelib::Import entry;entry.guid=data.guid;entry.lcid=data.lcid;entry.major=data.major;entry.minor=data.minor;entry.file=bridge.views.at(owner_for(data)).path;mapped->imports.push_back(std::move(entry));
                            }else import_index=static_cast<std::uint32_t>(imported-mapped->imports.begin());
                            const auto target_index=static_cast<std::uint32_t>(target.info-data.types.data());
                            const bool has_guid=std::any_of(target.info->guid.begin(),target.info->guid.end(),[](auto byte){return byte!=0;});
                            const auto prior=std::find_if(mapped->external_references.begin(),mapped->external_references.end(),[&](const auto& entry){return entry.import_index==import_index && (has_guid?entry.guid==target.info->guid:!entry.guid && entry.type_index==target_index);});
                            if(prior!=mapped->external_references.end()){type.reference=prior->reference;return;}
                            if(mapped->external_references.size()>=65535)failure(nomemory);
                            std::uint32_t ref=0x80000001U;
                            while(by_reference(*mapped,ref) || std::any_of(mapped->external_references.begin(),mapped->external_references.end(),[&](const auto& entry){return entry.reference==ref;})){if(ref>=0xffffffedU)failure(nomemory);ref+=4;}
                            typelib::ExternalReference entry;entry.reference=ref;entry.import_index=import_index;entry.type_index=target_index;entry.kind=static_cast<std::uint8_t>(target.info->kind);if(has_guid)entry.guid=target.info->guid;mapped->external_references.push_back(entry);type.reference=ref;
                        };
                        remap(remap,function.result,*inherited[index].library,0);for(auto& parameter:function.parameters)remap(remap,parameter.type,*inherited[index].library,0);
                        mapped_data=std::move(mapped);
                    }
                    dispatch_function(function);description=typelib::function_description(function,unit);
                }else description=typelib::function_description(info->functions[index],unit);
            }
            else{if(index>=info->variables.size())return done(badindex);kind=3;description=typelib::variable_description(info->variables[index],unit);}
            put(destination,describe(arg(0),kind,description));if(mapped_data)view.data=std::move(mapped_data);
            return done(0);
        }
        if(name=="GetTypeInfoCount")return done(lib.types.size());
        if(name=="GetTypeInfo" || name=="GetTypeInfoType" || name=="GetTypeInfoOfGuid"){
            const bool type=name=="GetTypeInfoType";output(arg(2),type?4:unit);memory.store(GuestAddress{arg(2)},type?32:bits,0);
            auto index=static_cast<std::uint32_t>(arg(1));if(name=="GetTypeInfoOfGuid"){
                if(!arg(1))return done(pointer_error);WindowsGuid guid{};memory.read(GuestAddress{arg(1)},guid);
                const auto found=std::find_if(lib.types.begin(),lib.types.end(),[&](const auto& item){return item.guid==guid;});if(found==lib.types.end())return done(element_missing);index=static_cast<std::uint32_t>(found-lib.types.begin());
            }
            if(index>=lib.types.size())return done(badindex);
            if(type)memory.store(GuestAddress{arg(2)},32,lib.types[index].kind);else put(arg(2),type_library_info(memory,view.library,index));return done(0);
        }
        if(name=="GetContainingTypeLib"){
            output(arg(1),unit);output(arg(2),4);auto& parent=bridge.views.at(view.library);if(parent.references==UINT32_MAX)return done(nomemory);
            ++parent.references;put(arg(1),view.library);memory.store(GuestAddress{arg(2)},32,static_cast<std::uint32_t>(view.index));return done(0);
        }
        if(name=="GetRefTypeOfImplType" || name=="GetImplTypeFlags"){
            output(arg(2),4);const auto index=static_cast<std::uint32_t>(arg(1));
            if(name=="GetRefTypeOfImplType" && index==UINT32_MAX && (info->flags&0x40U) && (info->kind==3 || info->kind==4)){memory.store(GuestAddress{arg(2)},32,UINT32_MAX);return done(0);}
            if(index>=info->implementations.size())return done(badindex);
            const auto& value=info->implementations[index];memory.store(GuestAddress{arg(2)},32,name=="GetImplTypeFlags"?value.flags:value.reference);return done(0);
        }
        if(name=="GetRefTypeInfo"){
            output(arg(2),unit);put(arg(2),0);put(arg(2),reference_type_library(state,memory,arg(0),static_cast<std::uint32_t>(arg(1))));return done(0);
        }
        if(name=="GetDocumentation" || name=="GetDllEntry"){
            const bool entry=name=="GetDllEntry";const auto id=static_cast<std::int32_t>(arg(1));const typelib::Documentation* doc{};const typelib::Function* function{};const auto* document_library=&lib;std::string_view document_path=view.path;
            if(entry){for(const auto& f:info->functions)if(f.id==id && (f.invocation&arg(2))){function=&f;break;}if(!function)return done(element_missing);}
            else if(!info){if(id==-1)doc=&lib.doc;else if(id>=0 && static_cast<std::size_t>(id)<lib.types.size())doc=&lib.types[static_cast<std::size_t>(id)].doc;else return done(badindex);}
            else if(id==-1)doc=&info->doc;else{const auto found=member(lib,*info,id,resolve_reference);if(!found.type)return done(element_missing);doc=found.function?&found.function->doc:&found.variable->doc;document_library=found.library;document_path=bridge.views.at(owner_for(*found.library)).path;}
            std::vector<std::pair<std::uint64_t,std::u16string>> requested;
            if(entry && info->kind!=2)return done(0x800288bdU);
            if(entry){if(arg(3))requested.emplace_back(arg(3),wide(info->dll));if(arg(4)){output(arg(4),unit);if(!function->entry_ordinal)requested.emplace_back(arg(4),wide(function->entry_name.value_or(function->doc.name)));}if(arg(5))output(arg(5),2);}
            else{
                if(arg(2))requested.emplace_back(arg(2),wide(doc->name));if(arg(3))requested.emplace_back(arg(3),wide(doc->help));
                if(arg(5)){auto help=document_library->help_file;const auto directory=library_directory(document_path);if(!help.empty() && help.find(':')==help.npos && !help.starts_with('/') && !help.starts_with('\\') && !directory.empty())help=directory+help;auto path=wide(help);std::replace(path.begin(),path.end(),u'/',u'\\');requested.emplace_back(arg(5),std::move(path));}
                if(arg(4))output(arg(4),4);
            }
            for(const auto& [address,value]:requested){(void)value;output(address,unit);}
            std::vector<std::uint64_t> allocated;allocated.reserve(requested.size());try{for(const auto& [address,value]:requested){(void)address;allocated.push_back(text(value));}}catch(...){for(const auto p:allocated)free_bstr(state,memory,p);throw;}
            for(std::size_t i=0;i<requested.size();++i)put(requested[i].first,allocated[i]);
            if(entry){if(arg(4) && function->entry_ordinal)put(arg(4),0);if(arg(5))memory.store(GuestAddress{arg(5)},16,function->entry_ordinal.value_or(0));}
            else if(arg(4))memory.store(GuestAddress{arg(4)},32,doc->context);return done(0);
        }
        if(name=="GetNames"){
            output(arg(4),4);memory.store(GuestAddress{arg(4)},32,0);const auto max=static_cast<std::uint32_t>(arg(3));if(!max)return done(0);
            const auto found=member(lib,*info,static_cast<std::int32_t>(arg(1)),resolve_reference);if(!found.type)return done(element_missing);
            std::vector<std::string> names{found.function?found.function->doc.name:found.variable->doc.name};if(found.function)for(const auto& p:found.function->parameters)names.push_back(p.name);
            names.resize(std::min<std::size_t>(max,names.size()));output(arg(2),names.size()*unit);std::vector<std::uint64_t> strings;strings.reserve(names.size());
            try{for(const auto& value:names)strings.push_back(text(wide(value)));}catch(...){for(const auto p:strings)free_bstr(state,memory,p);throw;}
            for(std::size_t i=0;i<strings.size();++i)put(arg(2)+unit*i,strings[i]);memory.store(GuestAddress{arg(4)},32,strings.size());return done(0);
        }
        if(name=="GetIDsOfNames"){
            const auto count=static_cast<std::uint32_t>(arg(2));if(!count || count>4096)return done(invalid);output(arg(3),count*4);memory.check(GuestAddress{arg(1)},count*unit,Access::Read);
            std::vector<std::u16string> names;for(unsigned i=0;i<count;++i)names.push_back(read_utf16(memory,GuestAddress{memory.load(GuestAddress{arg(1)+i*unit},bits)}));
            for(unsigned i=0;i<count;++i)memory.store(GuestAddress{arg(3)+i*4},32,UINT32_MAX);
            const auto found=member(lib,*info,0,resolve_reference,names[0]);if(!found.type)return done(unknown_name);
            memory.store(GuestAddress{arg(3)},32,static_cast<std::uint32_t>(found.function?found.function->id:found.variable->id));bool missing=false;
            for(unsigned i=1;i<count;++i){bool matched=false;if(found.function)for(std::size_t j=0;j<found.function->parameters.size();++j)if(matches(found.function->parameters[j].name,names[i])){memory.store(GuestAddress{arg(3)+i*4},32,j);matched=true;break;}missing|=!matched;}
            return done(missing?unknown_name:0);
        }
        if(name=="IsName" || name=="FindName"){
            const auto query=read_utf16(memory,GuestAddress{arg(1)});std::string canonical;std::vector<std::pair<std::uint32_t,std::int32_t>> found;
            for(std::size_t i=0;i<lib.types.size();++i){const auto& t=lib.types[i];std::set<std::int32_t> ids;
                const auto match=[&](const std::string& n,std::int32_t id){if(matches(n,query) && ids.insert(id).second){if(canonical.empty())canonical=n;found.emplace_back(static_cast<std::uint32_t>(i),id);}};
                match(t.doc.name,-1);for(const auto& f:t.functions)match(f.doc.name,f.id);for(const auto& v:t.variables)match(v.doc.name,v.id);
            }
            const bool library_name=matches(lib.doc.name,query);if(library_name && canonical.empty())canonical=lib.doc.name;
            const auto converted=wide(canonical);const bool correct_case=!canonical.empty() && converted.size()<=query.size();
            if(correct_case)output(arg(1),(converted.size()+1)*2);
            if(name=="IsName"){output(arg(3),4);memory.store(GuestAddress{arg(3)},32,library_name || !found.empty());}
            else{
                output(arg(5),2);const auto capacity=memory.load(GuestAddress{arg(5)},16);const auto count=std::min<std::size_t>(capacity,found.size());
                if(count){output(arg(3),count*unit);output(arg(4),count*4);}std::vector<std::uint64_t> objects;objects.reserve(count);
                try{for(std::size_t i=0;i<count;++i)objects.push_back(type_library_info(memory,view.library,found[i].first));}
                catch(...){for(const auto p:objects)(void)release_type_library(state,memory,p);throw;}
                for(std::size_t i=0;i<count;++i){put(arg(3)+i*unit,objects[i]);memory.store(GuestAddress{arg(4)+i*4},32,static_cast<std::uint32_t>(found[i].second));}memory.store(GuestAddress{arg(5)},16,count);
            }
            if(correct_case)memory.write(GuestAddress{arg(1)},utf16_bytes(converted));
            return done(0);
        }
        if(name=="GetMops"){output(arg(2),unit);put(arg(2),0);return done(0);}
        // 未完成的呼叫契約保持明確失敗。
        if(name=="GetTypeComp"){output(arg(1),unit);put(arg(1),0);put(arg(1),compiler(arg(0)));return done(0);}
        if(name=="Invoke")return invoke_type_library_member(state,memory,service);
        if(name=="CreateInstance"){
            output(arg(3),unit);put(arg(3),0);if(info->kind!=5)return done(0x800288bdU);
            if(!arg(2))return done(invalid);memory.check(GuestAddress{arg(2)},16,Access::Read);
            if(info->flags&1)return done(notimpl); // Application Object 仍需共用 GetActiveObject／ROT。
            const auto scratch=allocate_heap(memory,16);if(!scratch)return done(nomemory);
            try{memory.write(GuestAddress{scratch},info->guid);
                start_callbacks(state,memory,{{resolve("ole32.dll","CoCreateInstance").value,{scratch,arg(1),5,arg(2),arg(3)}}},Continuation::ReturnCallbackFreeHeap,scratch);
            }catch(...){(void)free_heap(memory,scratch,state);throw;}return std::nullopt;
        }
        if(name=="AddressOfMember"){
            output(arg(3),unit);put(arg(3),0);const auto id=static_cast<std::int32_t>(arg(1));const auto invocation=static_cast<std::uint32_t>(arg(2));
            const auto found=member(lib,*info,id,resolve_reference);if(!found.type)return done(element_missing);
            if(found.type->kind!=2 || found.type->dll.empty())return done(0x800288bdU);
            if(found.function){
                const typelib::Function* function=nullptr;
                for(const auto& item:found.type->functions)if(item.id==id && (item.invocation==1 || (item.invocation&invocation))){function=&item;break;}
                if(!function)return done(element_missing);
                return start_type_library_address(state,memory,arg(0),arg(3),wide(found.type->dll),function->entry_name.value_or(function->doc.name),function->entry_ordinal);
            }
            if(found.variable->kind!=1)return done(0x800288bdU);
            return start_type_library_address(state,memory,arg(0),arg(3),wide(found.type->dll),found.variable->doc.name,{});
        }
        return done(notimpl);
    }catch(const typelib::Error& e){return done(e.code);}
    catch(const std::bad_alloc&){return done(nomemory);}
    catch(const MemoryFault&){return done(pointer_error);}
    catch(const HostCallFault&){return done(pointer_error);}
}
}
