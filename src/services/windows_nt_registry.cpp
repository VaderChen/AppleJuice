#include "x64arm/windows_services.hpp"
#include "x64arm/registry_store.hpp"
#include "processor_registry.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid_handle=0xc0000008U,invalid_parameter=0xc000000dU,denied=0xc0000022U;
constexpr std::uint32_t missing=0xc0000034U,deleted=0xc000017cU,unsupported=0xc00000bbU,no_memory=0xc0000017U;
struct NativeRegistryError {std::uint32_t status;};
std::uint32_t native_io_error(std::uint32_t code){return !code?0U:code==2 || code==3?missing:code==5?denied:code==6?invalid_handle:code==8?no_memory:code==13?0xc000003eU:code==50?unsupported:code==87?invalid_parameter:code==1314?0xc0000061U:code==1336?0xc0000077U:code==1337?0xc0000078U:code==1338?0xc0000079U:0xc0000001U;}
std::u16string folded(std::u16string text){for(auto& c:text)c=unicode_case_unit(c,false);return text;}
void put(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint64_t value,unsigned size=4){for(unsigned i=0;i<size;++i)bytes.at(offset+i)=static_cast<std::uint8_t>(value>>(i*8));}
std::vector<std::uint8_t> text_bytes(std::u16string_view text){auto bytes=utf16_bytes(text);bytes.resize(text.size()*2);return bytes;}
}
std::optional<std::uint32_t> WindowsServices::invoke_nt_registry(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    RegistryIoScope io([&](const auto& work){await_registry_io(state,memory,work);});
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;auto& registry=runtime_.registry;
    const auto finish=[&](std::uint32_t status){arg.return_to(state,status);return std::optional<std::uint32_t>{};};
    if(symbol=="NtClose"){
        if(const auto flags=runtime_.handle_flags.find(arg(0));flags!=runtime_.handle_flags.end() && (flags->second&2U))return finish(0xc0000235U);
        if(registry.handles.erase(arg(0))){runtime_.handle_flags.erase(arg(0));poll_registry_notifications(false);return finish(0);}
        start_callbacks(state,memory,{{resolve("kernel32.dll","CloseHandle").value,{arg(0)}}},Continuation::NtCloseResult);return std::nullopt;
    }
    if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
    const bool readonly=symbol.starts_with("NtOpen") || symbol.starts_with("NtQuery") || symbol.starts_with("NtEnumerate") || symbol=="NtFlushKey";
    RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_,readonly);if(transaction.error)return finish(native_io_error(transaction.error));
    refresh_processor_registry(registry,memory.processor_count(),guest_processor_information(memory));
    initialize_registry_views(registry);
    poll_registry_notifications(false);bool changed=false;std::vector<std::pair<std::u16string,std::uint32_t>> mutations;
    const auto original=registry_file_.empty() || readonly?RegistryState{}:registry;
    const auto done=[&](std::uint32_t status){
        if(!status && changed){status=native_io_error(commit_registry_transaction(transaction));if(status && !registry_file_.empty())registry=original;}
        if(!status){for(const auto& [path,filter]:mutations)poll_registry_notifications(false,path,filter);poll_registry_notifications(false);}
        return finish(status);
    };
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto ptr=[&](std::uint64_t address){return memory.load(GuestAddress{address},bits);};
    const auto string=[&](std::uint64_t address){
        if(!address)throw NativeRegistryError{invalid_parameter};
        const auto length=memory.load(GuestAddress{address},16),maximum=memory.load(GuestAddress{address+2},16),buffer=ptr(address+unit);
        if((length&1U) || length>maximum || (length && !buffer))throw NativeRegistryError{invalid_parameter};
        std::u16string value;value.reserve(static_cast<std::size_t>(length/2));
        for(std::uint64_t i=0;i<length;i+=2){const auto c=static_cast<char16_t>(memory.load(GuestAddress{buffer+i},16));if(!c)throw NativeRegistryError{0xc0000033U};value.push_back(c);}return value;
    };
    const auto modified=[&](const std::u16string& path,std::uint32_t filter){changed=true;registry.modified_times[path]=runtime_.system_filetime();mutations.emplace_back(path,filter);};
    try {
        initialize_tokens();const auto user_sid=utf8_to_utf16(token_user_sid_string(1));const auto user_hive=std::u16string(1,3)+u"\\"+folded(user_sid);
        registry.keys.try_emplace(std::u16string(1,1));registry.keys.try_emplace(std::u16string(1,2));registry.keys.try_emplace(std::u16string(1,3));
        registry.keys.try_emplace(user_hive);registry.display_names[user_hive]=user_sid;
        const auto canonical=[&](const std::u16string& path){return path==user_hive || path.starts_with(user_hive+u"\\")?std::u16string(1,1)+path.substr(user_hive.size()):path;};
        const auto display_path=[&](const std::u16string& path){
            std::u16string result=path[0]==1?u"\\REGISTRY\\USER\\"+user_sid:path[0]==2?u"\\REGISTRY\\MACHINE":path[0]==3?u"\\REGISTRY\\USER":u"";
            if(result.empty())throw NativeRegistryError{unsupported};
            for(std::size_t start=2;start<path.size();){const auto end=path.find(u'\\',start),stop=end==path.npos?path.size():end;
                const auto key=path.substr(0,stop);const auto named=registry.display_names.find(key);result+=u'\\';result+=named==registry.display_names.end()?path.substr(start,stop-start):named->second;
                if(end==path.npos)break;start=end+1;
            }return result;
        };
        const auto write_information=[&](const std::vector<std::uint8_t>& bytes,std::size_t header,unsigned output,unsigned length,unsigned returned){
            if(!arg(returned))throw NativeRegistryError{invalid_parameter};memory.check(GuestAddress{arg(returned)},4,Access::Write);
            const auto capacity=static_cast<std::uint32_t>(arg(length));const auto count=std::min<std::size_t>(capacity,bytes.size());
            if(capacity>=header && count)memory.check(GuestAddress{arg(output)},count,Access::Write);
            memory.store(GuestAddress{arg(returned)},32,bytes.size());
            if(capacity<header)return UINT32_C(0xc0000023);
            if(count)memory.write(GuestAddress{arg(output)},std::span(bytes).first(count));
            return capacity<bytes.size()?UINT32_C(0x80000005):0U;
        };
        if(symbol=="NtOpenKey" || symbol=="NtOpenKeyEx" || symbol=="NtCreateKey"){
            const bool create=symbol=="NtCreateKey";const auto attributes=arg(2);const auto access=static_cast<std::uint32_t>(arg(1));
            if(!arg(0) || !attributes)return done(invalid_parameter);
            const auto size=memory.load(GuestAddress{attributes},32);if(size!=(x86_?24U:48U))return done(invalid_parameter);
            memory.check(GuestAddress{attributes},size,Access::Read);const auto root=ptr(attributes+unit),name_at=ptr(attributes+2*unit),flags=memory.load(GuestAddress{attributes+3*unit},32);
            if(flags&~0x42U)return done(unsupported);
            if((access&0x300U)==0x300U)return done(invalid_parameter);
            if(access&~0xf30f033fU)return done(unsupported);
            const auto options=create?static_cast<std::uint32_t>(arg(5)):symbol=="NtOpenKeyEx"?static_cast<std::uint32_t>(arg(3)):0;
            if(options && !(create && options==1)){if(!create && (options&~0xcU))return done(0xc00000f2U);return done(unsupported);}
            const auto class_name=create && arg(4)?string(arg(4)):std::u16string{};
            security::SecurityDescriptor explicit_descriptor;
            if(create && ptr(attributes+4*unit))explicit_descriptor=security::read_descriptor(memory,ptr(attributes+4*unit),x86_,true);
            const auto requested=string(name_at);auto lower=folded(requested);std::u16string path,relative;
            if(root){const auto found=registry.handles.find(root);if(found==registry.handles.end())return done(invalid_handle);if(!registry.keys.contains(found->second.path))return done(deleted);
                if(lower.starts_with(u"\\"))return done(0xc000003bU);path=registry_logical_path(found->second);relative=requested;
            }else {
                constexpr std::u16string_view machine=u"\\registry\\machine",users=u"\\registry\\user";
                const auto prefix=lower==machine || lower.starts_with(std::u16string(machine)+u"\\")?machine:lower==users || lower.starts_with(std::u16string(users)+u"\\")?users:std::u16string_view{};
                if(prefix.empty())return done(0xc000003bU);path=std::u16string(1,static_cast<char16_t>(prefix==machine?2:3));relative=requested.substr(prefix.size());if(!relative.empty())relative.erase(0,1);
            }
            std::vector<std::pair<std::u16string,std::u16string>> parents;
            for(std::size_t start=0;start<relative.size();){const auto end=relative.find(u'\\',start),stop=end==relative.npos?relative.size():end;auto component=relative.substr(start,stop-start);
                if(component.empty() || component.size()>255 || parents.size()>=128)return done(0xc0000033U);path=canonical(path+u"\\"+folded(component));parents.emplace_back(path,std::move(component));
                if(end==relative.npos)break;start=end+1;if(start==relative.size())return done(0xc0000033U);
            }
            const auto logical=path;const auto view=registry_view(access,x86_);path=registry_view_path(path,view);
            initialize_registry_views(registry);
            const bool exists=registry.keys.contains(path);if(!exists && !create)return done(missing);
            if(create && !exists){
                if(!options)for(const auto& item:registry.volatile_keys)if(path.starts_with(item+u"\\"))return done(0xc0000181U);
                // NtCreateKey 只建立最後一層；缺少中間節點不能沿用 RegCreateKey 的遞迴語義。
                const auto parent=path.substr(0,path.find_last_of(u'\\'));
                if(!registry.keys.contains(parent))return done(0xc000003aU);
                if(root && !(registry.handles.at(root).access&4U))return done(denied);
                (void)registry_security_access(parent,4U);
            }
            if(registry.handles.size()>=4096 || (create && registry.keys.size()+parents.size()>4096))return done(no_memory);
            memory.check(GuestAddress{arg(0)},unit,Access::Write);if(create && arg(6))memory.check(GuestAddress{arg(6)},4,Access::Write);
            std::vector<std::uint8_t> descriptor;
            if(create && !exists)descriptor=registry_creation_descriptor(registry_security_descriptor(path.substr(0,path.find_last_of(u'\\'))),std::move(explicit_descriptor));
            const auto granted=descriptor.empty()?registry_security_access(path,access):object_security_access(descriptor,access&~0x300U,{0x20019U,0x20006U,0x20019U,0xf003fU});
            if(create && !exists){registry.keys.try_emplace(path);registry.security_descriptors[path]=std::move(descriptor);registry.display_names[path]=parents.back().second;if(options)registry.volatile_keys.insert(path);if(!class_name.empty())registry.classes[path]=class_name;modified(path,1);modified(path.substr(0,path.find_last_of(u'\\')),1);}
            const auto handle=registry.next_handle++;registry.handles.emplace(handle,RegistryHandle{path,granted,view,logical});if(flags&2U)runtime_.handle_flags[handle]=1;
            memory.store(GuestAddress{arg(0)},bits,handle);if(create && arg(6))memory.store(GuestAddress{arg(6)},32,exists?2U:1U);return done(0);
        }
        const auto found=registry.handles.find(arg(0));if(found==registry.handles.end())return done(invalid_handle);
        const auto handle=found->second;if(!registry.keys.contains(handle.path))return done(deleted);
        auto& values=registry.keys.at(handle.path);
        if(symbol=="NtFlushKey"){if(!(handle.access&1U))return done(denied);if(const auto error=flush_registry_hives())return done(native_io_error(error));return done(native_io_error(transaction.flush()));}
        if(symbol=="NtDeleteKey"){
            if(registry.hives.contains(handle.path))return done(denied);
            if(!(handle.access&0x10000U) || handle.path.size()==1)return done(denied);const auto prefix=handle.path+u"\\";
            if(std::any_of(registry.keys.begin(),registry.keys.end(),[&](const auto& entry){return entry.first.starts_with(prefix);}))return done(denied);
            for(const auto& [name,value]:values){(void)name;registry.bytes-=value.bytes.size();--registry.values;}
            registry.keys.erase(handle.path);registry.security_descriptors.erase(handle.path);registry.display_names.erase(handle.path);registry.modified_times.erase(handle.path);registry.classes.erase(handle.path);registry.volatile_keys.erase(handle.path);modified(handle.path.substr(0,handle.path.find_last_of(u'\\')),1);return done(0);
        }
        if(symbol=="NtSetValueKey" || symbol=="NtDeleteValueKey"){
            if(!(handle.access&2U))return done(denied);const auto original_name=string(arg(1)),name=folded(original_name);const auto value=values.find(name);
            if(symbol=="NtDeleteValueKey"){
                if(value==values.end())return done(missing);registry.bytes-=value->second.bytes.size();--registry.values;values.erase(value);modified(handle.path,4);return done(0);
            }
            const auto size=static_cast<std::uint32_t>(arg(5));if(size>output_limit || (size && !arg(4)))return done(size>output_limit?no_memory:invalid_parameter);
            const auto previous=value==values.end()?0:value->second.bytes.size();if(registry.bytes-previous+size>output_limit || (value==values.end() && registry.values>=4096))return done(no_memory);
            std::vector<std::uint8_t> bytes(size);if(size)memory.read(GuestAddress{arg(4)},bytes);
            const auto display=value==values.end()?(original_name==name?std::u16string{}:original_name):value->second.display_name;
            values[name]={static_cast<std::uint32_t>(arg(3)),std::move(bytes),display};registry.bytes=registry.bytes-previous+size;if(value==values.end())++registry.values;modified(handle.path,4);return done(0);
        }
        if(symbol=="NtQueryValueKey" || symbol=="NtEnumerateValueKey"){
            if(!(handle.access&1U))return done(denied);const bool enumerate=symbol=="NtEnumerateValueKey";const auto info=static_cast<std::uint32_t>(arg(2));
            if(info>4)return done(invalid_parameter);if((info==3 || info==4) && (arg(3)&7U))return done(0x80000002U);
            auto value=values.end();if(enumerate){const auto index=static_cast<std::uint32_t>(arg(1));if(index>=values.size())return done(0x8000001aU);value=std::next(values.begin(),index);}
            else value=values.find(folded(string(arg(1))));if(value==values.end())return done(missing);
            const auto& data=value->second;const auto name=text_bytes(data.display_name.empty()?value->first:data.display_name);std::vector<std::uint8_t> bytes;std::size_t header=0;
            if(info==0){header=12;bytes.resize(header+name.size());put(bytes,4,data.type);put(bytes,8,name.size());std::copy(name.begin(),name.end(),bytes.begin()+12);}
            else if(info==2 || info==4){header=info==4?8U:12U;bytes.resize(header+data.bytes.size());put(bytes,info==4?0U:4U,data.type);put(bytes,info==4?4U:8U,data.bytes.size());std::copy(data.bytes.begin(),data.bytes.end(),bytes.begin()+static_cast<std::ptrdiff_t>(header));}
            else {header=20;const auto alignment=info==3?8U:4U;const auto offset=(header+name.size()+alignment-1)&~static_cast<std::size_t>(alignment-1);bytes.resize(offset+data.bytes.size());put(bytes,4,data.type);put(bytes,8,offset);put(bytes,12,data.bytes.size());put(bytes,16,name.size());std::copy(name.begin(),name.end(),bytes.begin()+20);std::copy(data.bytes.begin(),data.bytes.end(),bytes.begin()+static_cast<std::ptrdiff_t>(offset));}
            return done(write_information(bytes,header,3,4,5));
        }
        if(symbol=="NtQueryKey" || symbol=="NtEnumerateKey"){
            const bool enumerate=symbol=="NtEnumerateKey";const auto info=static_cast<std::uint32_t>(arg(enumerate?2:1));if(info>3 || (enumerate && info==3))return done(invalid_parameter);
            if(!(handle.access&(enumerate?8U:1U)) && (enumerate || info!=3))return done(denied);
            auto path=handle.path;const auto children=[&](const std::u16string& parent){auto selected=handle;selected.path=parent;if(parent!=handle.path)selected.logical_path=registry_logical_path(handle)+u"\\"+parent.substr(parent.find_last_of(u'\\')+1);return registry_subkeys(registry,selected);};
            if(enumerate){const auto list=children(path);const auto index=static_cast<std::uint32_t>(arg(1));if(index>=list.size())return done(0x8000001aU);path=list[index];}
            const auto named=registry.display_names.find(path);const auto full=display_path(path);const auto name=info==3?full:named==registry.display_names.end()?full.substr(full.find_last_of(u'\\')+1):named->second;
            const auto class_name=registry.classes.contains(path)?text_bytes(registry.classes.at(path)):std::vector<std::uint8_t>{};
            const auto encoded=text_bytes(name);std::vector<std::uint8_t> bytes;std::size_t header=info==3?4U:info==0?16U:info==1?24U:44U;
            const auto class_offset=info==2?header:(header+encoded.size()+3)&~std::size_t(3);
            bytes.resize((info==1 || info==2) && !class_name.empty()?class_offset+class_name.size():header+(info==2?0:encoded.size()));
            if(info==3)put(bytes,0,encoded.size());
            else {put(bytes,0,registry.modified_times.contains(path)?registry.modified_times.at(path):runtime_.filetime_base,8);
                if(info==0)put(bytes,12,encoded.size());else {put(bytes,12,class_name.empty()?UINT32_MAX:class_offset);put(bytes,16,class_name.size());if(info==1)put(bytes,20,encoded.size());}
            }
            if(info==2){const auto list=children(path);std::size_t maximum_name=0,maximum_value_name=0,maximum_data=0;
                for(const auto& child:list){const auto display=registry.display_names.find(child);maximum_name=std::max(maximum_name,(display==registry.display_names.end()?child.substr(child.find_last_of(u'\\')+1):display->second).size()*2);}
                std::size_t maximum_class=0;for(const auto& child:list)if(const auto item=registry.classes.find(child);item!=registry.classes.end())maximum_class=std::max(maximum_class,item->second.size()*2);put(bytes,28,maximum_class);
                const auto& entries=registry.keys.at(path);for(const auto& [key,value]:entries){maximum_value_name=std::max(maximum_value_name,(value.display_name.empty()?key:value.display_name).size()*2);maximum_data=std::max(maximum_data,value.bytes.size());}
                put(bytes,20,list.size());put(bytes,24,maximum_name);put(bytes,32,entries.size());put(bytes,36,maximum_value_name);put(bytes,40,maximum_data);
            }else std::copy(encoded.begin(),encoded.end(),bytes.begin()+static_cast<std::ptrdiff_t>(header));
            if((info==1 || info==2) && !class_name.empty())std::copy(class_name.begin(),class_name.end(),bytes.begin()+static_cast<std::ptrdiff_t>(class_offset));
            return done(write_information(bytes,header,enumerate?3:2,enumerate?4:3,enumerate?5:4));
        }
        return done(unsupported);
    }catch(const NativeRegistryError& error){return done(error.status);}
    catch(const SecurityDescriptorError& error){return done(native_io_error(error.code));}
    catch(const MemoryFault&){return done(0xc0000005U);}
    catch(const std::bad_alloc&){return done(no_memory);}
}
}
