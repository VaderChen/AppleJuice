#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <limits>

namespace x64arm {
namespace {
using Guid=std::array<std::uint8_t,16>;
constexpr Guid unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Guid item_iid{0x1e,0x6d,0x82,0x43,0x18,0xe7,0xee,0x42,0xbc,0x55,0xa1,0xe2,0x61,0xc3,0x7b,0xfe};
constexpr Guid enum_iid{0x33,0x90,0x62,0x70,0x63,0xe3,0x28,0x4a,0xa5,0x67,0x0d,0xb7,0x80,0x06,0xe6,0xd7};
constexpr Guid stream_iid{12,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Guid sequential_iid{0x30,0x3a,0x73,0x0c,0x1c,0x2a,0xce,0x11,0xad,0xe5,0,0xaa,0,0x44,0x77,0x3d};
constexpr Guid stream_handler{0xab,0xb3,0xeb,0x1c,0x10,0x7c,0x9a,0x49,0xa4,0x17,0x92,0xca,0x16,0xc4,0xcb,0x83};
constexpr Guid enum_handler{0x19,0x05,0xf6,0x94,0x50,0x28,0x24,0x49,0xaa,0x5a,0xd1,0x5e,0x84,0x86,0x80,0x39};
constexpr Guid storage_enum_handler{0xe3,0xa4,0x21,0x46,0xd6,0xf0,0x73,0x47,0x8a,0x9c,0x46,0xe7,0x7b,0x17,0x48,0x40};
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_interface=0x80004002U,no_memory=0x8007000eU;
std::uint32_t hr(std::uint32_t error){return error?0x80070000U|(error&65535U):0;}
Guid guid(const GuestMemory& memory,std::uint64_t at){Guid value{};memory.read(GuestAddress{at},value);return value;}
std::u16string canonical(std::u16string path,std::u16string_view current){
    if(path.empty() || path.size()>32767)throw std::invalid_argument("Shell 路徑為空或過長");
    std::replace(path.begin(),path.end(),u'/',u'\\');
    if(path.starts_with(u"\\\\"))throw std::invalid_argument("Shell UNC／裝置 namespace 尚未映射");
    std::u16string root=current.size()>=3?std::u16string(current.substr(0,3)):u"C:\\";
    if(path.size()>1 && path[1]==u':'){
        auto drive=path[0];if(drive>=u'a' && drive<=u'z')drive-=32;
        if(drive<u'A' || drive>u'Z')throw std::invalid_argument("Shell 磁碟代號不合法");
        root={drive,u':',u'\\'};
        if(path.size()==2 || path[2]!=u'\\')throw std::invalid_argument("Shell 磁碟相對路徑尚未映射");
        path.erase(0,2);
    }else if(path.front()!=u'\\'){path=std::u16string(current)+u"\\"+path;root=path.substr(0,3);path.erase(0,2);}
    std::vector<std::u16string> parts;
    for(std::size_t at=0;at<path.size();){auto end=path.find(u'\\',at);if(end==path.npos)end=path.size();auto part=path.substr(at,end-at);at=end+1;
        if(part==u".."){if(!parts.empty())parts.pop_back();}
        else if(!part.empty() && part!=u"."){if(part.find_first_of(u"\"<>|:*?\0",0,8)!=part.npos)throw std::invalid_argument("Shell 檔案名稱不合法");parts.push_back(std::move(part));}
    }
    for(const auto& part:parts){if(root.back()!=u'\\')root+=u'\\';root+=part;}return root;
}
std::u16string leaf(const std::u16string& path){return path.size()==3?path:path.substr(path.find_last_of(u'\\')+1);}
std::u16string display(const std::u16string& path,std::uint32_t form){
    switch(form){
    case 0:case 0x80018001:case 0x80031001:case 0x8007c001:case 0x80080001:case 0x80094001:return leaf(path);
    case 0x80028000:case 0x8004c000:case 0x80058000:return path;
    case 0x80068000:{
        auto bytes=utf16_to_utf8(path);std::string result="file:///";constexpr char hex[]="0123456789ABCDEF";
        for(const auto value:bytes){const auto c=static_cast<unsigned char>(value);if(c=='\\')result+='/';else if((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c==':' || c=='-' || c=='_' || c=='.' || c=='~')result+=static_cast<char>(c);else {result+='%';result+=hex[c>>4];result+=hex[c&15];}}
        return utf8_to_utf16_lossy(result);
    }
    default:throw std::invalid_argument("Shell 顯示名稱格式不合法");
    }
}
std::int32_t compare(std::u16string a,std::u16string b,std::uint32_t hint){
    for(auto& c:a)c=unicode_case_unit(c,false);for(auto& c:b)c=unicode_case_unit(c,false);
    if(a==b)return 0;
    if(!(hint&0x90000000U)){const auto first=leaf(a),second=leaf(b);if(first!=second)return first<second?-1:1;}
    return a<b?-1:1;
}
std::uint64_t method(const GuestMemory& memory,std::uint64_t object,unsigned slot){
    const std::uint8_t bits=memory.execution_mode()==ExecutionMode::X86?32:64;
    if(!object)throw HostCallFault("Shell COM 物件為空");const auto table=memory.load(GuestAddress{object},bits);
    if(!table || table>UINT64_MAX-slot*(bits/8))throw HostCallFault("Shell COM vtable 不合法");
    const auto target=memory.load(GuestAddress{table+slot*(bits/8)},bits);if(!target)throw HostCallFault("Shell COM 方法為空");return target;
}
}
std::span<const WindowsServiceDescriptor> shell_item_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shell32.dll","SHCreateItemFromParsingName",4},{"shell32.dll","SHCreateItemFromIDList",3},
        {"shell32.dll","SHGetNameFromIDList",3},
        {"internal-shell-item","QueryInterface",3},{"internal-shell-item","AddRef",1},{"internal-shell-item","Release",1},
        {"internal-shell-item","BindToHandler",5},{"internal-shell-item","GetParent",2},{"internal-shell-item","GetDisplayName",3},
        {"internal-shell-item","GetAttributes",3},{"internal-shell-item","Compare",4},
        {"internal-shell-item-enum","QueryInterface",3},{"internal-shell-item-enum","AddRef",1},{"internal-shell-item-enum","Release",1},
        {"internal-shell-item-enum","Next",4},{"internal-shell-item-enum","Skip",2},{"internal-shell-item-enum","Reset",1},{"internal-shell-item-enum","Clone",2},
        {"shlwapi.dll","SHCreateStreamOnFileA",3},{"shlwapi.dll","SHCreateStreamOnFileW",3},{"shlwapi.dll","SHCreateStreamOnFileEx",6}
    };return table;
}
std::uint64_t WindowsServices::create_shell_item(GuestMemory& memory,std::u16string path){
    if(gui_.shell_items.size()>=8192)throw std::bad_alloc();const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(!gui_.shell_item_vtable){const auto table=runtime_allocate(memory,{},8*unit);unsigned i=0;
        for(const auto& entry:shell_item_service_descriptors())if(entry.family=="internal-shell-item")memory.store(GuestAddress{table+unit*i++},bits,resolve(entry.family,entry.symbol).value);gui_.shell_item_vtable=table;}
    const auto object=allocate_heap(memory,unit);if(!object)throw std::bad_alloc();
    try{memory.store(GuestAddress{object},bits,gui_.shell_item_vtable);gui_.shell_items.emplace(object,GuiShellItem{std::move(path)});}
    catch(...){(void)free_heap(memory,object,GuestState{});throw;}return object;
}
std::uint64_t WindowsServices::create_shell_enumerator(GuestMemory& memory,std::vector<std::u16string> paths,std::size_t cursor){
    if(gui_.shell_item_enumerators.size()>=1024 || paths.size()>100000)throw std::bad_alloc();const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(!gui_.shell_item_enum_vtable){const auto table=runtime_allocate(memory,{},7*unit);unsigned i=0;
        for(const auto& entry:shell_item_service_descriptors())if(entry.family=="internal-shell-item-enum")memory.store(GuestAddress{table+unit*i++},bits,resolve(entry.family,entry.symbol).value);gui_.shell_item_enum_vtable=table;}
    const auto object=allocate_heap(memory,unit);if(!object)throw std::bad_alloc();
    try{memory.store(GuestAddress{object},bits,gui_.shell_item_enum_vtable);gui_.shell_item_enumerators.emplace(object,GuiShellItemEnumerator{std::move(paths),cursor});}
    catch(...){(void)free_heap(memory,object,GuestState{});throw;}return object;
}
void WindowsServices::discard_shell_item(GuestState& state,GuestMemory& memory,std::uint64_t id){
    const auto found=gui_.shell_item_operations.find(id);if(found==gui_.shell_item_operations.end())return;
    const auto op=found->second;gui_.shell_item_operations.erase(found);
    if(!op.binding){const auto name=memory.load(GuestAddress{op.scratch},x86_?32:64);if(name && allocations_.contains(name)){gui_.task_allocations.erase(name);(void)free_heap(memory,name,state);}}
    (void)free_heap(memory,op.scratch,state);
    const auto item=gui_.shell_items.find(op.item);if(item!=gui_.shell_items.end() && !--item->second.references){gui_.shell_items.erase(item);(void)free_heap(memory,op.item,state);}
}
std::optional<std::uint32_t> WindowsServices::resume_shell_item(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint32_t status){
    const auto op=gui_.shell_item_operations.at(id);const auto path=gui_.shell_items.at(op.item).path;
    try{
        if(!(status&0x80000000U)){
            if(op.binding){std::uint64_t stream{};memory.check(GuestAddress{op.output},x86_?4:8,Access::Write);
                status=open_shell_file_stream(memory,path,static_cast<std::uint32_t>(memory.load(GuestAddress{op.scratch+8},32)),0,false,stream);
                if(!status)memory.store(GuestAddress{op.output},x86_?32:64,stream);
            }else {const auto name=memory.load(GuestAddress{op.scratch},x86_?32:64);if(!name)status=pointer_error;
                else {const auto other=canonical(read_utf16(memory,GuestAddress{name},32767),runtime_.current_directory);const auto order=compare(path,other,op.hint);memory.store(GuestAddress{op.output},32,static_cast<std::uint32_t>(order));status=order?1:0;}}
        }
    }catch(const std::bad_alloc&){status=no_memory;}catch(const std::invalid_argument&){status=invalid;}
    catch(...){discard_shell_item(state,memory,id);throw;}
    discard_shell_item(state,memory,id);complete_call(state,memory,status);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_shell_item(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    std::uint64_t failure_output{};
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto output=[&](std::uint64_t at){if(!at)return false;memory.store(GuestAddress{at},bits,0);return true;};
    const auto text_output=[&](std::uint64_t at,const std::u16string& text){const auto bytes=utf16_bytes(text);const auto value=allocate_heap(memory,bytes.size());if(!value)throw std::bad_alloc();
        try{memory.write(GuestAddress{value},bytes);gui_.task_allocations.insert(value);memory.store(GuestAddress{at},bits,value);}catch(...){gui_.task_allocations.erase(value);(void)free_heap(memory,value,state);throw;}return finish(0);};
    const auto begin=[&](std::uint64_t item,bool binding,std::uint64_t out,std::uint32_t hint,std::uint64_t target,std::uint64_t other){
        if(gui_.shell_item_operations.size()>=1024)throw std::bad_alloc();const auto scratch=allocate_heap(memory,32);if(!scratch)throw std::bad_alloc();
        try{memory.write(GuestAddress{scratch},std::array<std::uint8_t,32>{});if(binding)memory.store(GuestAddress{scratch},32,16);
            gui_.shell_item_operations.emplace(scratch,GuiShellItemOperation{binding,item,scratch,out,hint});++gui_.shell_items.at(item).references;
            start_callbacks(state,memory,{{target,binding?std::vector<std::uint64_t>{other,scratch}:std::vector<std::uint64_t>{other,0x80028000U,scratch}}},Continuation::ShellItemOperation,scratch);
        }catch(...){if(gui_.shell_item_operations.contains(scratch))discard_shell_item(state,memory,scratch);else (void)free_heap(memory,scratch,state);throw;}return std::optional<std::uint32_t>{};
    };
    try{
        if(symbol.starts_with("SHCreateStreamOnFile")){
            const bool ex=symbol=="SHCreateStreamOnFileEx";const auto out=arg(ex?5:2);if(!out)return finish(pointer_error);memory.check(GuestAddress{out},unit,Access::Write);failure_output=out;
            if(!arg(0) || (ex && arg(4))){output(out);return finish(invalid);}
            const auto path=canonical(symbol.ends_with('A')?utf8_to_utf16_lossy(read_ansi(memory,arg(0),32767)):read_utf16(memory,GuestAddress{arg(0)},32767),runtime_.current_directory);
            output(out);std::uint64_t stream{};const auto status=open_shell_file_stream(memory,path,d(1),ex?d(2):0,ex && d(3),stream);if(!status)memory.store(GuestAddress{out},bits,stream);return finish(status);
        }
        if(service.family=="shell32.dll"){
            const bool parsing=symbol=="SHCreateItemFromParsingName",name=symbol=="SHGetNameFromIDList";const auto out=arg(parsing?3:2);
            if(!out)return finish(pointer_error);memory.check(GuestAddress{out},unit,Access::Write);failure_output=out;Guid iid{};if(!name){if(!arg(parsing?2:1)){output(out);return finish(pointer_error);}iid=guid(memory,arg(parsing?2:1));}
            std::u16string path;if(parsing){if(!arg(0)){output(out);return finish(invalid);}path=canonical(read_utf16(memory,GuestAddress{arg(0)},32767),runtime_.current_directory);}
            else {const auto pidl=gui_.shell_pidls.find(arg(0));if(pidl==gui_.shell_pidls.end() || !allocations_.contains(arg(0))){output(out);return finish(invalid);}path=pidl->second;}
            output(out);if(name)return text_output(out,display(path,d(1)));
            if(iid!=unknown && iid!=item_iid)return finish(no_interface);
            if(parsing && arg(1))return finish(0x80004001U); // 自訂 namespace bind 資料尚未實作，不能假裝已採用。
            const auto info=filesystem_.information(utf16_to_utf8(path));if(info.error)return finish(hr(info.error));
            const auto object=create_shell_item(memory,std::move(path));memory.store(GuestAddress{out},bits,object);return finish(0);
        }
        const bool enumeration=service.family=="internal-shell-item-enum";
        if(enumeration){
            const auto found=gui_.shell_item_enumerators.find(arg(0));if(found==gui_.shell_item_enumerators.end())return finish(invalid);auto& object=found->second;
            if(symbol=="QueryInterface"){if(!arg(2))return finish(pointer_error);const auto iid=arg(1)?guid(memory,arg(1)):Guid{};output(arg(2));if(!arg(1))return finish(pointer_error);if(iid!=unknown && iid!=enum_iid)return finish(no_interface);++object.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);}
            if(symbol=="AddRef")return finish(++object.references);
            if(symbol=="Release"){if(--object.references)return finish(object.references);gui_.shell_item_enumerators.erase(found);(void)free_heap(memory,arg(0),state);return finish(0);}
            if(symbol=="Reset"){object.cursor=0;return finish(0);}
            if(symbol=="Clone"){if(!output(arg(1)))return finish(pointer_error);const auto clone=create_shell_enumerator(memory,object.paths,object.cursor);memory.store(GuestAddress{arg(1)},bits,clone);return finish(0);}
            const auto count=d(1);const auto available=object.paths.size()-object.cursor;const auto take=std::min<std::size_t>(count,available);
            if(symbol=="Skip"){object.cursor+=take;return finish(take==count?0:1);}
            if(symbol=="Next"){
                if(arg(3))memory.store(GuestAddress{arg(3)},32,0);if(!arg(2) || (!arg(3) && count!=1))return finish(pointer_error);
                if(count>8192)return finish(no_memory);memory.check(GuestAddress{arg(2)},static_cast<std::size_t>(count)*unit,Access::Write);
                std::vector<std::uint64_t> items;items.reserve(take);
                try{for(std::size_t i=0;i<take;++i)items.push_back(create_shell_item(memory,object.paths[object.cursor+i]));}
                catch(...){for(const auto value:items){gui_.shell_items.erase(value);(void)free_heap(memory,value,state);}throw;}
                for(std::size_t i=0;i<take;++i)memory.store(GuestAddress{arg(2)+i*unit},bits,items[i]);object.cursor+=take;
                if(arg(3))memory.store(GuestAddress{arg(3)},32,take);return finish(take || !count?0:1);
            }
            return finish(invalid);
        }
        const auto found=gui_.shell_items.find(arg(0));if(found==gui_.shell_items.end())return finish(invalid);auto& object=found->second;
        if(symbol=="QueryInterface"){if(!arg(2))return finish(pointer_error);const auto iid=arg(1)?guid(memory,arg(1)):Guid{};output(arg(2));if(!arg(1))return finish(pointer_error);if(iid!=unknown && iid!=item_iid)return finish(no_interface);++object.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);}
        if(symbol=="AddRef")return finish(++object.references);
        if(symbol=="Release"){if(--object.references)return finish(object.references);gui_.shell_items.erase(found);(void)free_heap(memory,arg(0),state);return finish(0);}
        if(symbol=="GetDisplayName"){if(!output(arg(2)))return finish(pointer_error);return text_output(arg(2),display(object.path,d(1)));}
        if(symbol=="GetParent"){
            if(!output(arg(1)))return finish(pointer_error);if(object.path.size()==3)return finish(hr(2));const auto slash=object.path.find_last_of(u'\\');
            const auto parent=object.path.substr(0,slash<3?3:slash);const auto info=filesystem_.information(utf16_to_utf8(parent));if(info.error)return finish(hr(info.error));
            const auto value=create_shell_item(memory,parent);memory.store(GuestAddress{arg(1)},bits,value);return finish(0);
        }
        if(symbol=="Compare"){
            if(!arg(3))return finish(pointer_error);memory.store(GuestAddress{arg(3)},32,0);if(!arg(1) || (d(2)&~0xb0000000U))return finish(invalid);
            const auto other=gui_.shell_items.find(arg(1));if(other==gui_.shell_items.end())return begin(arg(0),false,arg(3),d(2),method(memory,arg(1),5),arg(1));
            const auto order=compare(object.path,other->second.path,d(2));memory.store(GuestAddress{arg(3)},32,static_cast<std::uint32_t>(order));return finish(order?1:0);
        }
        if(symbol=="GetAttributes"){
            if(!arg(2))return finish(pointer_error);memory.store(GuestAddress{arg(2)},32,0);const auto info=filesystem_.information(utf16_to_utf8(object.path));if(info.error)return finish(hr(info.error));
            const bool folder=(info.metadata.attributes&16)!=0;std::uint32_t attributes=0x40000001U|(folder?0x30800000U:0x00400000U);
            if((info.metadata.attributes&1) || !filesystem_.writable())attributes|=0x40000U;if(info.metadata.attributes&2)attributes|=0x80000U;
            if(filesystem_.writable() && !(info.metadata.attributes&1) && object.path.size()>3)attributes|=0x32U;
            if(folder && (d(1)&0x80000000U)){const auto listing=filesystem_.list(utf16_to_utf8(object.path));if(listing.error)return finish(hr(listing.error));if(std::any_of(listing.entries.begin(),listing.entries.end(),[](const auto& entry){return (entry.metadata.attributes&16)!=0;}))attributes|=0x80000000U;}
            const auto masked=attributes&d(1);memory.store(GuestAddress{arg(2)},32,masked);return finish(masked==d(1)?0:1);
        }
        if(symbol=="BindToHandler"){
            if(!arg(4))return finish(pointer_error);if(!arg(2) || !arg(3)){output(arg(4));return finish(pointer_error);}const auto handler=guid(memory,arg(2)),iid=guid(memory,arg(3));output(arg(4));
            if(handler==stream_handler){if(iid!=unknown && iid!=stream_iid && iid!=sequential_iid)return finish(no_interface);
                if(arg(1))return begin(arg(0),true,arg(4),0,method(memory,arg(1),7),arg(1));
                std::uint64_t stream{};const auto status=open_shell_file_stream(memory,object.path,0,0,false,stream);if(!status)memory.store(GuestAddress{arg(4)},bits,stream);return finish(status);
            }
            if(handler==enum_handler || handler==storage_enum_handler){if(iid!=unknown && iid!=enum_iid)return finish(no_interface);if(arg(1))return finish(0x80004001U);
                const auto listing=filesystem_.list(utf16_to_utf8(object.path));if(listing.error)return finish(hr(listing.error));std::vector<std::u16string> paths;paths.reserve(listing.entries.size());
                for(const auto& entry:listing.entries){auto path=object.path;if(path.back()!=u'\\')path+=u'\\';path+=utf8_to_utf16_lossy(entry.name);paths.push_back(std::move(path));}
                const auto value=create_shell_enumerator(memory,std::move(paths));memory.store(GuestAddress{arg(4)},bits,value);return finish(0);
            }
            return finish(0x800401e5U); // MK_E_NOOBJECT：本 namespace 沒有此 handler。
        }
        return finish(invalid);
    }catch(const std::bad_alloc&){return finish(no_memory);}catch(const std::length_error&){return finish(no_memory);}catch(const std::invalid_argument&){if(failure_output)output(failure_output);return finish(invalid);}
}
}
