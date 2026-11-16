#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
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
constexpr Guid folder_iid{0xe6,0x14,0x02,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Guid persist_iid{0x0c,0x01,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},persist_folder_iid{0xea,0x14,0x02,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Guid idlist_enum_iid{0xf2,0x14,0x02,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},extract_icon_iid{0xfa,0x14,0x02,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Guid fs_folder_clsid{0xa0,0x4b,0x36,0xf3,0xb9,0x65,0xce,0x11,0xa9,0xba,0x00,0xaa,0x00,0x4a,0xe8,0x37},desktop_clsid{0x00,0x14,0x02,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Guid array_iid{0x6d,0xa7,0x3e,0xb6,0x85,0x1f,0x6f,0x45,0xa1,0x9c,0x48,0x15,0x9e,0xfa,0x85,0x8b};
constexpr Guid persist_idlist_iid{0xfc,0xac,0x79,0x10,0xbd,0x29,0xd3,0x11,0x8e,0x0d,0x00,0xc0,0x4f,0x68,0x37,0xd5};
constexpr Guid persist_folder2_iid{0xf0,0xd9,0xc3,0x1a,0x5c,0x17,0xd1,0x11,0x95,0xbe,0x00,0x60,0x97,0x97,0xea,0x4f};
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
        {"shell32.dll","SHParseDisplayName",5,0,0,0x8},{"shell32.dll","SHCreateShellItem",4},{"shell32.dll","SHCreateItemWithParent",5},
        {"shell32.dll","SHCreateItemFromRelativeName",5},{"shell32.dll","SHCreateItemInKnownFolder",5,0,0,0x2},{"shell32.dll","SHGetKnownFolderItem",5,0,0,0x2},
        {"shell32.dll","SHCreateShellItemArray",5,0,0,0x4},{"shell32.dll","SHCreateShellItemArrayFromIDLists",3,0,0,0x1},{"shell32.dll","SHCreateShellItemArrayFromShellItem",3},
        {"shell32.dll","SHGetIDListFromObject",2},{"shell32.dll","SHGetItemFromObject",3},{"shell32.dll","ILSaveToStream",2},{"shell32.dll","SHGetFolderPathEx",5,0,0,0x12},
        {"shell32.dll","SHGetDesktopFolder",1},{"shell32.dll","SHBindToParent",4},{"shell32.dll","SHBindToObject",5},{"shell32.dll","SHBindToFolderIDListParent",5},
        {"shell32.dll","SHGetDataFromIDListA",5,0,0,0x14},{"shell32.dll","SHGetDataFromIDListW",5,0,0,0x14},{"shell32.dll","SHMapPIDLToSystemImageListIndex",3},
        {"internal-shell-folder","QueryInterface",3},{"internal-shell-folder","AddRef",1},{"internal-shell-folder","Release",1},
        {"internal-shell-folder","ParseDisplayName",7},{"internal-shell-folder","EnumObjects",4,0,0,0x4},{"internal-shell-folder","BindToObject",5},{"internal-shell-folder","BindToStorage",5},
        {"internal-shell-folder","CompareIDs",4},{"internal-shell-folder","CreateViewObject",4},{"internal-shell-folder","GetAttributesOf",4,0,0,0x2},
        {"internal-shell-folder","GetUIObjectOf",7,0,0,0x4},{"internal-shell-folder","GetDisplayNameOf",4,0,0,0x4},{"internal-shell-folder","SetNameOf",6,0,0,0x10},
        {"internal-shell-persist","QueryInterface",3},{"internal-shell-persist","AddRef",1},{"internal-shell-persist","Release",1},
        {"internal-shell-persist","GetClassID",2},{"internal-shell-persist","Initialize",2},{"internal-shell-persist","GetCurFolder",2},
        {"internal-shell-idlist-enum","QueryInterface",3},{"internal-shell-idlist-enum","AddRef",1},{"internal-shell-idlist-enum","Release",1},
        {"internal-shell-idlist-enum","Next",4,0,0,0x2},{"internal-shell-idlist-enum","Skip",2,0,0,0x2},{"internal-shell-idlist-enum","Reset",1},{"internal-shell-idlist-enum","Clone",2},
        {"internal-shell-item-array","QueryInterface",3},{"internal-shell-item-array","AddRef",1},{"internal-shell-item-array","Release",1},
        {"internal-shell-item-array","BindToHandler",5},{"internal-shell-item-array","GetPropertyStore",4,0,0,0x2},{"internal-shell-item-array","GetPropertyDescriptionList",4},
        {"internal-shell-item-array","GetAttributes",4,0,0,0x6},{"internal-shell-item-array","GetCount",2},{"internal-shell-item-array","GetItemAt",3,0,0,0x2},{"internal-shell-item-array","EnumItems",2},
        {"internal-shell-item","QueryInterface",3},{"internal-shell-item","AddRef",1},{"internal-shell-item","Release",1},
        {"internal-shell-item","BindToHandler",5},{"internal-shell-item","GetParent",2},{"internal-shell-item","GetDisplayName",3},
        {"internal-shell-item","GetAttributes",3},{"internal-shell-item","Compare",4},
        {"internal-shell-item-enum","QueryInterface",3},{"internal-shell-item-enum","AddRef",1},{"internal-shell-item-enum","Release",1},
        {"internal-shell-item-enum","Next",4},{"internal-shell-item-enum","Skip",2},{"internal-shell-item-enum","Reset",1},{"internal-shell-item-enum","Clone",2},
        {"shlwapi.dll","SHCreateStreamOnFileA",3},{"shlwapi.dll","SHCreateStreamOnFileW",3},{"shlwapi.dll","SHCreateStreamOnFileEx",6},
        // shcore 是這組串流函式的實作模組（api-ms-win-shcore-stream），shlwapi 版本轉送至此；共用同一處理。
        {"shcore.dll","SHCreateStreamOnFileA",3},{"shcore.dll","SHCreateStreamOnFileW",3},{"shcore.dll","SHCreateStreamOnFileEx",6}
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
std::uint64_t WindowsServices::create_shell_folder(GuestMemory& memory,std::u16string path){
    if(gui_.shell_folders.size()>=4096)throw std::bad_alloc();const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto table=[&](std::uint64_t& cache,std::string_view family,unsigned count){if(cache)return;const auto address=runtime_allocate(memory,{},count*unit);unsigned i=0;
        for(const auto& entry:shell_item_service_descriptors())if(entry.family==family)memory.store(GuestAddress{address+unit*i++},bits,resolve(entry.family,entry.symbol).value);cache=address;};
    table(gui_.shell_folder_vtable,"internal-shell-folder",13);table(gui_.shell_persist_vtable,"internal-shell-persist",6);
    const auto object=allocate_heap(memory,2*unit);if(!object)throw std::bad_alloc();
    try{memory.store(GuestAddress{object},bits,gui_.shell_folder_vtable);memory.store(GuestAddress{object+unit},bits,gui_.shell_persist_vtable);gui_.shell_folders.emplace(object,GuiState::ShellFolder{std::move(path)});}
    catch(...){(void)free_heap(memory,object,GuestState{});throw;}return object;
}
std::uint64_t WindowsServices::create_shell_item_array(GuestMemory& memory,std::vector<std::u16string> paths){
    if(gui_.shell_item_arrays.size()>=1024 || paths.size()>100000)throw std::bad_alloc();const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(!gui_.shell_item_array_vtable){const auto table=runtime_allocate(memory,{},10*unit);unsigned i=0;
        for(const auto& entry:shell_item_service_descriptors())if(entry.family=="internal-shell-item-array")memory.store(GuestAddress{table+unit*i++},bits,resolve(entry.family,entry.symbol).value);gui_.shell_item_array_vtable=table;}
    const auto object=allocate_heap(memory,unit);if(!object)throw std::bad_alloc();
    try{memory.store(GuestAddress{object},bits,gui_.shell_item_array_vtable);gui_.shell_item_arrays.emplace(object,GuiShellItemArray{std::move(paths)});}
    catch(...){(void)free_heap(memory,object,GuestState{});throw;}return object;
}
// SFGAO：CANCOPY｜檔案系統、資料夾（FOLDER｜FILESYSANCESTOR｜BROWSABLE）或 STREAM、唯讀、隱藏、可改名／刪除／移動，HASSUBFOLDER 需要時才列舉。
std::uint32_t WindowsServices::shell_path_attributes(const std::u16string& path,std::uint32_t requested,std::uint32_t& masked){
    masked=0;const auto info=filesystem_.information(utf16_to_utf8(path));if(info.error)return hr(info.error);
    const bool folder=(info.metadata.attributes&16)!=0;std::uint32_t attributes=0x40000001U|(folder?0x30800000U:0x00400000U);
    if((info.metadata.attributes&1) || !filesystem_.writable())attributes|=0x40000U;if(info.metadata.attributes&2)attributes|=0x80000U;
    if(filesystem_.writable() && !(info.metadata.attributes&1) && path.size()>3)attributes|=0x32U;
    if(folder && (requested&0x80000000U)){const auto listing=filesystem_.list(utf16_to_utf8(path));if(listing.error)return hr(listing.error);if(std::any_of(listing.entries.begin(),listing.entries.end(),[](const auto& entry){return (entry.metadata.attributes&16)!=0;}))attributes|=0x80000000U;}
    masked=attributes&requested;return 0;
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
        // 項目輸出：介面須為 IUnknown／IShellItem，路徑必須存在。
        const auto item_out=[&](std::u16string path,std::uint64_t riid,std::uint64_t out)->std::optional<std::uint32_t>{
            if(!out)return finish(pointer_error);output(out);if(!riid)return finish(pointer_error);const auto iid=guid(memory,riid);if(iid!=unknown && iid!=item_iid)return finish(no_interface);
            const auto info=filesystem_.information(utf16_to_utf8(path));if(info.error)return finish(hr(info.error));
            memory.store(GuestAddress{out},bits,create_shell_item(memory,std::move(path)));return finish(0);};
        const auto array_out=[&](std::vector<std::u16string> paths,std::uint64_t riid,std::uint64_t out)->std::optional<std::uint32_t>{
            if(!out)return finish(pointer_error);output(out);if(riid){const auto iid=guid(memory,riid);if(iid!=unknown && iid!=array_iid)return finish(no_interface);}
            memory.store(GuestAddress{out},bits,create_shell_item_array(memory,std::move(paths)));return finish(0);};
        if(symbol=="SHParseDisplayName"){
            // (名稱, IBindCtx, PIDL*, sfgaoIn, sfgaoOut*)：檔案系統路徑轉成絕對 PIDL，並回報要求的屬性。
            if(!arg(2))return finish(invalid);output(arg(2));if(arg(4))memory.store(GuestAddress{arg(4)},32,0);if(!arg(0))return finish(invalid);
            const auto path=canonical(read_utf16(memory,GuestAddress{arg(0)},32767),runtime_.current_directory);std::uint32_t masked=0;
            if(const auto status=shell_path_attributes(path,d(3),masked))return finish(status);
            const auto pidl=shell_make_pidl(memory,path);if(!pidl)return finish(no_memory);memory.store(GuestAddress{arg(2)},bits,pidl);if(arg(4))memory.store(GuestAddress{arg(4)},32,masked);return finish(0);
        }
        if(symbol=="SHCreateItemInKnownFolder" || symbol=="SHGetKnownFolderItem"){
            // 已知資料夾（可加上相對名稱）的 IShellItem。
            const bool relative=symbol=="SHCreateItemInKnownFolder";const auto out=arg(4);if(!out)return finish(pointer_error);output(out);if(!arg(0))return finish(invalid);
            GuestScratch temporary(*this,memory,state);const auto slot=temporary.allocate(8);if(!slot)return finish(no_memory);memory.store(GuestAddress{slot},64,0);
            const auto status=static_cast<std::uint32_t>(nested_service(state,memory,"shell32.dll","SHGetKnownFolderPath",{arg(0),d(1),relative?0U:arg(2),slot}));
            const auto pointer=memory.load(GuestAddress{slot},bits);auto path=pointer?read_utf16(memory,GuestAddress{pointer},32767):std::u16string{};
            if(pointer)(void)nested_service(state,memory,"ole32.dll","CoTaskMemFree",{pointer});if(status)return finish(status);
            if(relative && arg(2)){const auto name=read_utf16(memory,GuestAddress{arg(2)},32767);if(!name.empty())path=canonical(path+u"\\"+name,runtime_.current_directory);}
            return item_out(std::move(path),arg(3),out);
        }
        if(symbol=="SHCreateShellItemArrayFromIDLists"){
            const auto count=d(0);if(!arg(2))return finish(pointer_error);output(arg(2));if(!count || !arg(1))return finish(invalid);std::vector<std::u16string> paths;
            for(std::uint32_t i=0;i<count;++i){const auto path=shell_pidl_path(memory,memory.load(GuestAddress{arg(1)+i*unit},bits));if(!path)return finish(invalid);paths.push_back(*path);}
            return array_out(std::move(paths),0,arg(2));
        }
        if(symbol=="SHGetFolderPathEx"){
            // (KNOWNFOLDERID, flags, token, 緩衝區, cch)：同 SHGetKnownFolderPath，複製到呼叫端緩衝區。
            if(!arg(3) || !d(4))return finish(invalid);GuestScratch temporary(*this,memory,state);const auto slot=temporary.allocate(8);if(!slot)return finish(no_memory);memory.store(GuestAddress{slot},64,0);
            const auto status=static_cast<std::uint32_t>(nested_service(state,memory,"shell32.dll","SHGetKnownFolderPath",{arg(0),d(1),arg(2),slot}));
            const auto pointer=memory.load(GuestAddress{slot},bits);const auto path=pointer?read_utf16(memory,GuestAddress{pointer},32767):std::u16string{};
            if(pointer)(void)nested_service(state,memory,"ole32.dll","CoTaskMemFree",{pointer});if(status)return finish(status);
            if(path.size()>=d(4))return finish(0x8007007aU);memory.write(GuestAddress{arg(3)},utf16_bytes(path));return finish(0);
        }
        if(symbol=="SHCreateShellItem" || symbol=="SHCreateItemWithParent" || symbol=="SHCreateItemFromRelativeName" || symbol=="SHCreateShellItemArray" ||
           symbol=="SHCreateShellItemArrayFromShellItem" || symbol=="SHGetIDListFromObject" || symbol=="SHGetItemFromObject" || symbol=="ILSaveToStream"){
            // 需要從物件取得路徑者：本引擎的 IShellItem 直接取得；其他物件依序經 IPersistIDList::GetIDList、IPersistFolder2::GetCurFolder、IShellItem::GetDisplayName(FILESYSPATH)。
            return nested_continuation(state,memory,"ShellItemObject",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);const auto name=service.symbol;const unsigned size=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(size*8);
                const auto finish_call=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
                const auto method_at=[&](std::uint64_t object,unsigned index){return m.load(GuestAddress{m.load(GuestAddress{object},width)+index*size},width);};
                const auto clear=[&](std::uint64_t at){if(at)m.store(GuestAddress{at},width,0);};
                GuestScratch temporary(*this,m,s);const auto slot=temporary.allocate(16);if(!slot)co_return finish_call(no_memory);
                std::optional<std::u16string> path;
                // 參數配置：來源物件與輸出位置。
                std::uint64_t object=0,out=0,riid=0;
                if(name=="SHCreateShellItem"){out=call(3);if(!call(0))object=call(1);}
                else if(name=="SHCreateItemWithParent"){out=call(4);riid=call(3);if(!call(0))object=call(1);}
                else if(name=="SHCreateItemFromRelativeName"){out=call(4);riid=call(3);object=call(0);}
                else if(name=="SHCreateShellItemArray"){out=call(4);if(!call(0))object=call(1);}
                else if(name=="SHCreateShellItemArrayFromShellItem"){out=call(2);riid=call(1);object=call(0);}
                else if(name=="SHGetIDListFromObject"){out=call(1);object=call(0);}
                else if(name=="SHGetItemFromObject"){out=call(2);riid=call(1);object=call(0);}
                if(name!="ILSaveToStream"){if(!out)co_return finish_call(pointer_error);clear(out);}
                if(object){
                    if(const auto own=gui_.shell_items.find(object);own!=gui_.shell_items.end())path=own->second.path;
                    else {
                        const auto attempt=[&](const Guid& iid){const auto at=temporary.allocate(16);if(at)m.write(GuestAddress{at},iid);return at;};
                        for(const auto& [iid,index]:{std::pair{persist_idlist_iid,5U},std::pair{persist_folder2_iid,5U}}){
                            const auto requested=attempt(iid);if(!requested)break;m.store(GuestAddress{slot},width,0);
                            if(static_cast<std::uint32_t>(co_await service_callback::call(method_at(object,0),{object,requested,slot}))&0x80000000U)continue;
                            const auto persist=m.load(GuestAddress{slot},width);m.store(GuestAddress{slot},width,0);
                            const auto status=static_cast<std::uint32_t>(co_await service_callback::call(method_at(persist,index),{persist,slot}));
                            (void)co_await service_callback::call(method_at(persist,2),{persist});
                            if(!(status&0x80000000U)){const auto pidl=m.load(GuestAddress{slot},width);path=shell_pidl_path(m,pidl);
                                if(pidl)(void)co_await service_callback::call(resolve("ole32.dll","CoTaskMemFree").value,{pidl});}
                            if(path)break;}
                        if(!path){const auto requested=attempt(item_iid);m.store(GuestAddress{slot},width,0);
                            if(requested && !(static_cast<std::uint32_t>(co_await service_callback::call(method_at(object,0),{object,requested,slot}))&0x80000000U)){
                                const auto item=m.load(GuestAddress{slot},width);m.store(GuestAddress{slot},width,0);
                                const auto status=static_cast<std::uint32_t>(co_await service_callback::call(method_at(item,5),{item,0x80058000U,slot})); // SIGDN_FILESYSPATH
                                (void)co_await service_callback::call(method_at(item,2),{item});
                                if(!(status&0x80000000U)){const auto text=m.load(GuestAddress{slot},width);if(text){path=read_utf16(m,GuestAddress{text},32767);(void)co_await service_callback::call(resolve("ole32.dll","CoTaskMemFree").value,{text});}}}}
                    }
                    if(!path)co_return finish_call(name=="SHGetIDListFromObject" || name=="SHGetItemFromObject"?no_interface:invalid);
                }
                try{
                    if(name=="SHGetIDListFromObject"){if(!object)co_return finish_call(invalid);const auto pidl=shell_make_pidl(m,*path);if(!pidl)co_return finish_call(no_memory);m.store(GuestAddress{out},width,pidl);co_return finish_call(0);}
                    const auto make_item=[&](std::u16string target,std::uint64_t interface_id)->std::uint32_t{
                        if(interface_id){Guid iid{};m.read(GuestAddress{interface_id},iid);if(iid!=unknown && iid!=item_iid)return no_interface;}
                        const auto info=filesystem_.information(utf16_to_utf8(target));if(info.error)return hr(info.error);m.store(GuestAddress{out},width,create_shell_item(m,std::move(target)));return 0;};
                    if(name=="SHGetItemFromObject"){if(!object)co_return finish_call(invalid);co_return finish_call(make_item(*path,riid));}
                    if(name=="SHCreateItemFromRelativeName"){if(!call(1))co_return finish_call(invalid);co_return finish_call(make_item(canonical(*path+u"\\"+read_utf16(m,GuestAddress{call(1)},32767),runtime_.current_directory),riid));}
                    if(name=="SHCreateShellItemArrayFromShellItem"){if(!object)co_return finish_call(invalid);Guid iid{};if(riid)m.read(GuestAddress{riid},iid);if(riid && iid!=unknown && iid!=array_iid)co_return finish_call(no_interface);
                        m.store(GuestAddress{out},width,create_shell_item_array(m,{*path}));co_return finish_call(0);}
                    if(name=="ILSaveToStream"){
                        // WORD 大小（含結尾的 IDList 位元組數）後接 IDList 本身。
                        if(!call(0))co_return finish_call(invalid);std::vector<std::uint8_t> bytes;
                        if(call(1)){for(std::uint64_t at=call(1);;){const auto cb=static_cast<std::uint16_t>(m.load(GuestAddress{at},16));if(!cb)break;const auto chunk=at;at+=cb;
                            for(std::uint64_t i=0;i<cb;++i)bytes.push_back(static_cast<std::uint8_t>(m.load(GuestAddress{chunk+i},8)));if(bytes.size()>65000)co_return finish_call(invalid);}}
                        bytes.push_back(0);bytes.push_back(0);std::vector<std::uint8_t> data{static_cast<std::uint8_t>(bytes.size()&0xffU),static_cast<std::uint8_t>(bytes.size()>>8)};data.insert(data.end(),bytes.begin(),bytes.end());
                        const auto buffer=temporary.allocate(data.size());if(!buffer)co_return finish_call(no_memory);m.write(GuestAddress{buffer},data);m.store(GuestAddress{slot},32,0);
                        const auto status=static_cast<std::uint32_t>(co_await service_callback::call(method_at(call(0),4),{call(0),buffer,data.size(),slot}));
                        if(status&0x80000000U)co_return finish_call(status);co_return finish_call(m.load(GuestAddress{slot},32)==data.size()?0U:0x80004005U);
                    }
                    // SHCreateShellItem／SHCreateItemWithParent／SHCreateShellItemArray：父路徑來自 PIDL 或資料夾物件，子 PIDL 相對於父路徑。
                    std::u16string parent;if(object)parent=*path;else if(call(0)){const auto decoded=shell_pidl_path(m,call(0));if(!decoded)co_return finish_call(invalid);parent=*decoded;}
                    if(name=="SHCreateShellItemArray"){const auto count=static_cast<std::uint32_t>(call(2));if(!count || !call(3) || parent.empty())co_return finish_call(invalid);std::vector<std::u16string> paths;
                        for(std::uint32_t i=0;i<count;++i){const auto child=shell_pidl_path(m,m.load(GuestAddress{call(3)+i*size},width),parent);if(!child)co_return finish_call(invalid);paths.push_back(*child);}
                        m.store(GuestAddress{out},width,create_shell_item_array(m,std::move(paths)));co_return finish_call(0);}
                    const auto child_pidl=call(name=="SHCreateShellItem"?2:2);if(!child_pidl)co_return finish_call(invalid);
                    const auto target=parent.empty()?shell_pidl_path(m,child_pidl):shell_pidl_path(m,child_pidl,parent);if(!target)co_return finish_call(invalid);
                    co_return finish_call(make_item(*target,name=="SHCreateItemWithParent"?riid:0));
                }catch(const std::invalid_argument&){co_return finish_call(invalid);}catch(const std::bad_alloc&){co_return finish_call(no_memory);}
            });
        }
        // ── IShellFolder（桌面＝空路徑，子項目為磁碟）、IPersistFolder2 tear-off（物件＋一個指標位移）、IEnumIDList 與繫結函式 ──
        const auto child_path=[&](const std::u16string& folder,std::uint64_t pidl)->std::optional<std::u16string>{return folder.empty()?shell_pidl_path(memory,pidl):shell_pidl_path(memory,pidl,folder);};
        const auto child_pidl=[&](const std::u16string& folder,const std::u16string& path)->std::uint64_t{return shell_encode_pidl(memory,folder,path,folder.empty());};
        const auto is_folder=[&](const std::u16string& path){const auto info=filesystem_.information(utf16_to_utf8(path));return !info.error && (info.metadata.attributes&16U);};
        // 依介面輸出資料夾物件：IShellFolder／IUnknown 為物件本身，IPersistFolder2／IPersistFolder／IPersist 為 tear-off。
        const auto folder_out=[&](std::u16string path,std::uint64_t riid,std::uint64_t out)->std::uint32_t{
            if(!out)return pointer_error;output(out);if(!riid)return invalid;const auto iid=guid(memory,riid);
            const bool folder=iid==unknown || iid==folder_iid,persist=iid==persist_folder2_iid || iid==persist_folder_iid || iid==persist_iid;if(!folder && !persist)return no_interface;
            const auto object=create_shell_folder(memory,std::move(path));memory.store(GuestAddress{out},bits,persist?object+unit:object);return 0;};
        // BindToObject／BindToStorage：資料夾 → 資料夾物件；檔案 → IStream；IShellItem 直接建立項目。
        const auto bind_path=[&](const std::u16string& path,std::uint64_t riid,std::uint64_t out,bool storage)->std::uint32_t{
            if(!out)return pointer_error;output(out);if(!riid)return invalid;const auto info=filesystem_.information(utf16_to_utf8(path));if(info.error)return hr(info.error);
            const auto iid=guid(memory,riid);if(iid==item_iid){memory.store(GuestAddress{out},bits,create_shell_item(memory,path));return 0;}
            if(iid==stream_iid || iid==sequential_iid){if(info.metadata.attributes&16U)return 0x80070005U;std::uint64_t stream{};const auto status=open_shell_file_stream(memory,path,0,0,false,stream);if(!status)memory.store(GuestAddress{out},bits,stream);return status;}
            if(storage)return no_interface;if(!(info.metadata.attributes&16U))return invalid;return folder_out(path,riid,out);};
        const auto folder_of=[&](std::uint64_t object)->std::optional<std::u16string>{if(!object)return std::u16string{};const auto found=gui_.shell_folders.find(object);if(found==gui_.shell_folders.end())return std::nullopt;return found->second.path;};
        const auto parent_of=[](const std::u16string& path){if(path.size()<=3)return std::u16string{};const auto slash=path.find_last_of(u'\\');return path.substr(0,slash<3?3:slash);};
        const auto last_item=[&](std::uint64_t pidl)->std::uint64_t{std::uint64_t at=pidl,last=pidl;for(unsigned guard=0;guard<4096;++guard){const auto cb=memory.load(GuestAddress{at},16);if(!cb)break;last=at;at+=cb;}return last;};
        if(symbol=="SHGetDesktopFolder"){if(!arg(0))return finish(pointer_error);output(arg(0));memory.store(GuestAddress{arg(0)},bits,create_shell_folder(memory,u""));return finish(0);}
        if(symbol=="SHBindToParent" || symbol=="SHBindToFolderIDListParent"){
            // 繫結到 PIDL 最後一段的父資料夾，ppidlLast 指向原 PIDL 中的最後一個項目。
            const bool root=symbol=="SHBindToFolderIDListParent";const auto pidl=arg(root?1:0),riid=arg(root?2:1),out=arg(root?3:2),last=arg(root?4:3);
            if(last)output(last);if(!out)return finish(pointer_error);output(out);if(!pidl || !memory.load(GuestAddress{pidl},16))return finish(invalid);
            const auto base=root?folder_of(arg(0)):std::optional<std::u16string>{std::u16string{}};if(!base)return finish(invalid);
            const auto path=child_path(*base,pidl);if(!path)return finish(invalid);const auto status=folder_out(parent_of(*path),riid,out);
            if(!status && last)memory.store(GuestAddress{last},bits,last_item(pidl));return finish(status);
        }
        if(symbol=="SHBindToObject"){
            // (psf, pidl, pbc, riid, ppv)：psf 為 NULL 時以桌面為根；其他實作的資料夾直接轉到其 BindToObject（參數順序相同）。空 PIDL 回傳資料夾本身。
            const auto base=folder_of(arg(0));
            if(!base){state.rip=GuestAddress{method(memory,arg(0),5)};return std::nullopt;}
            if(!arg(4))return finish(pointer_error);output(arg(4));
            if(!arg(1) || !memory.load(GuestAddress{arg(1)},16))return finish(folder_out(*base,arg(3),arg(4)));
            const auto path=child_path(*base,arg(1));if(!path)return finish(invalid);return finish(bind_path(*path,arg(3),arg(4),false));
        }
        if(symbol.starts_with("SHGetDataFromIDList") || symbol=="SHMapPIDLToSystemImageListIndex"){
            const auto base=folder_of(arg(0));if(!base || !arg(1))return finish(invalid);const auto path=child_path(*base,arg(1));if(!path)return finish(invalid);
            const auto info=filesystem_.information(utf16_to_utf8(*path));if(info.error)return finish(hr(info.error));const bool folder=(info.metadata.attributes&16U)!=0;
            if(symbol=="SHMapPIDLToSystemImageListIndex"){
                // 與 SHGetFileInfo(SHGFI_SYSICONINDEX) 相同的系統影像索引：檔案 0、資料夾 1、磁碟 2。
                const std::uint32_t index=path->size()==3?2U:folder?1U:0U;if(arg(2))memory.store(GuestAddress{arg(2)},32,index);return finish(index);
            }
            // SHGDFIL_FINDDATA 1（WIN32_FIND_DATA）、NETRESOURCE 2（不是網路項目）、DESCRIPTIONID 3（SHDESCRIPTIONID）。
            const bool wide=symbol.ends_with('W');const auto format=d(2);const auto size=static_cast<std::int32_t>(d(4));if(!arg(3))return finish(invalid);
            if(format==3){if(size<20)return finish(invalid);std::uint32_t id=folder?3U:2U;
                if(path->size()==3){GuestScratch temporary(*this,memory,state);const auto text=temporary.text(*path);const auto type=text?static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll","GetDriveTypeW",{text})):0U;
                    id=type==2?7U:type==4?9U:type==5?10U:type==6?11U:type==3?8U:12U;}
                memory.store(GuestAddress{arg(3)},32,id);memory.write(GuestAddress{arg(3)+4},Guid{});return finish(0);}
            if(format!=1)return finish(invalid);if(size<(wide?592:320))return finish(invalid);
            GuestScratch temporary(*this,memory,state);auto query=*path;if(query.size()==3)query+=u"*";const auto text=temporary.text(query),data=temporary.allocate(592);if(!text || !data)return finish(no_memory);
            auto handle=nested_service(state,memory,"kernel32.dll","FindFirstFileW",{text,data});if(x86_)handle&=UINT32_MAX;if(handle==(x86_?UINT32_MAX:UINT64_MAX))return finish(hr(last_error_?last_error_:2));
            (void)nested_service(state,memory,"kernel32.dll","FindClose",{handle});
            if(path->size()==3){memory.store(GuestAddress{data},32,info.metadata.attributes);memory.write(GuestAddress{data+44},utf16_bytes(*path));}
            if(wide){std::vector<std::uint8_t> bytes(592);memory.read(GuestAddress{data},bytes);memory.write(GuestAddress{arg(3)},bytes);return finish(0);}
            std::vector<std::uint8_t> head(44);memory.read(GuestAddress{data},head);memory.write(GuestAddress{arg(3)},head);
            const auto name=utf16_to_utf8_lossy(read_utf16(memory,GuestAddress{data+44},259)),alternate=utf16_to_utf8_lossy(read_utf16(memory,GuestAddress{data+44+520},13));
            std::vector<std::uint8_t> names(276);std::copy_n(name.begin(),std::min<std::size_t>(name.size(),259),names.begin());std::copy_n(alternate.begin(),std::min<std::size_t>(alternate.size(),13),names.begin()+260);
            memory.write(GuestAddress{arg(3)+44},names);return finish(0);
        }
        if(service.family=="internal-shell-persist"){
            const auto object=arg(0)-unit;const auto found=gui_.shell_folders.find(object);if(found==gui_.shell_folders.end())return finish(invalid);auto& folder=found->second;
            if(symbol=="QueryInterface"){if(!arg(2))return finish(pointer_error);output(arg(2));if(!arg(1))return finish(pointer_error);const auto iid=guid(memory,arg(1));
                if(iid==unknown || iid==folder_iid){++folder.references;memory.store(GuestAddress{arg(2)},bits,object);return finish(0);}
                if(iid==persist_folder2_iid || iid==persist_folder_iid || iid==persist_iid){++folder.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);}return finish(no_interface);}
            if(symbol=="AddRef")return finish(++folder.references);
            if(symbol=="Release"){if(--folder.references)return finish(folder.references);gui_.shell_folders.erase(found);(void)free_heap(memory,object,state);return finish(0);}
            if(symbol=="GetClassID"){if(!arg(1))return finish(pointer_error);memory.write(GuestAddress{arg(1)},folder.path.empty()?desktop_clsid:fs_folder_clsid);return finish(0);}
            if(symbol=="Initialize"){if(!arg(1) || !memory.load(GuestAddress{arg(1)},16)){folder.path.clear();return finish(0);}const auto path=shell_pidl_path(memory,arg(1));if(!path)return finish(invalid);folder.path=*path;return finish(0);}
            if(symbol=="GetCurFolder"){if(!output(arg(1)))return finish(pointer_error);std::uint64_t pidl=0;
                if(folder.path.empty()){pidl=allocate_heap(memory,2);if(pidl)memory.store(GuestAddress{pidl},16,0);}else pidl=shell_make_pidl(memory,folder.path);
                if(!pidl)return finish(no_memory);memory.store(GuestAddress{arg(1)},bits,pidl);return finish(0);}
            return finish(invalid);
        }
        if(service.family=="internal-shell-idlist-enum"){
            const auto found=gui_.shell_idlist_enums.find(arg(0));if(found==gui_.shell_idlist_enums.end())return finish(invalid);auto& object=found->second;
            if(symbol=="QueryInterface"){if(!arg(2))return finish(pointer_error);output(arg(2));if(!arg(1))return finish(pointer_error);const auto iid=guid(memory,arg(1));if(iid!=unknown && iid!=idlist_enum_iid)return finish(no_interface);++object.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);}
            if(symbol=="AddRef")return finish(++object.references);
            if(symbol=="Release"){if(--object.references)return finish(object.references);gui_.shell_idlist_enums.erase(found);(void)free_heap(memory,arg(0),state);return finish(0);}
            if(symbol=="Reset"){object.cursor=0;return finish(0);}
            const auto count=d(1);const auto available=object.names.size()-object.cursor;const auto take=std::min<std::size_t>(count,available);
            if(symbol=="Skip"){object.cursor+=take;return finish(take==count?0:1);}
            if(symbol=="Clone"){if(!output(arg(1)))return finish(pointer_error);auto copy=object;copy.references=1;const auto clone=allocate_heap(memory,unit);if(!clone)return finish(no_memory);
                memory.store(GuestAddress{clone},bits,gui_.shell_idlist_enum_vtable);gui_.shell_idlist_enums.emplace(clone,std::move(copy));memory.store(GuestAddress{arg(1)},bits,clone);return finish(0);}
            if(symbol=="Next"){
                if(arg(3))memory.store(GuestAddress{arg(3)},32,0);if(!arg(2) || (!arg(3) && count!=1))return finish(pointer_error);if(count>8192)return finish(no_memory);
                for(std::size_t i=0;i<take;++i){const auto& name=object.names[object.cursor+i];const auto path=object.folder.empty()?name:object.folder+(object.folder.back()==u'\\'?u"":u"\\")+name;
                    const auto pidl=child_pidl(object.folder,path);if(!pidl)return finish(no_memory);memory.store(GuestAddress{arg(2)+i*unit},bits,pidl);}
                object.cursor+=take;if(arg(3))memory.store(GuestAddress{arg(3)},32,take);return finish(take==count?0:1);
            }
            return finish(invalid);
        }
        if(service.family=="internal-shell-folder"){
            const auto found=gui_.shell_folders.find(arg(0));if(found==gui_.shell_folders.end())return finish(invalid);auto& folder=found->second;const auto base=folder.path;
            if(symbol=="QueryInterface"){if(!arg(2))return finish(pointer_error);output(arg(2));if(!arg(1))return finish(pointer_error);const auto iid=guid(memory,arg(1));
                if(iid==unknown || iid==folder_iid){++folder.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);}
                if(iid==persist_folder2_iid || iid==persist_folder_iid || iid==persist_iid){++folder.references;memory.store(GuestAddress{arg(2)},bits,arg(0)+unit);return finish(0);}return finish(no_interface);}
            if(symbol=="AddRef")return finish(++folder.references);
            if(symbol=="Release"){if(--folder.references)return finish(folder.references);gui_.shell_folders.erase(found);(void)free_heap(memory,arg(0),state);return finish(0);}
            if(symbol=="ParseDisplayName"){
                // (hwnd, pbc, 名稱, pchEaten, ppidl, pdwAttributes)：桌面接受完整路徑；其他資料夾接受相對名稱，回傳相對 PIDL。
                if(!arg(5))return finish(invalid);output(arg(5));if(!arg(3))return finish(invalid);const auto text=read_utf16(memory,GuestAddress{arg(3)},32767);
                const bool absolute=text.size()>=2 && text[1]==u':';if(!base.empty() && absolute)return finish(invalid);if(base.empty() && !absolute)return finish(invalid);
                const auto path=canonical(base.empty()?text:base+u"\\"+text,runtime_.current_directory);std::uint32_t masked=0;
                const auto requested=arg(6)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(6)},32)):0U;if(const auto status=shell_path_attributes(path,requested,masked))return finish(status);
                const auto pidl=child_pidl(base,path);if(!pidl)return finish(invalid);memory.store(GuestAddress{arg(5)},bits,pidl);
                if(arg(4))memory.store(GuestAddress{arg(4)},32,text.size());if(arg(6))memory.store(GuestAddress{arg(6)},32,masked);return finish(0);
            }
            if(symbol=="EnumObjects"){
                // SHCONTF_FOLDERS 0x20、NONFOLDERS 0x40、INCLUDEHIDDEN 0x80；桌面的子項目為存在的磁碟根目錄。
                if(!output(arg(3)))return finish(pointer_error);const auto flags=d(2);std::vector<std::u16string> names;
                if(base.empty()){if(flags&0x20U){const auto mask=static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll","GetLogicalDrives",{}));
                    for(unsigned i=0;i<26;++i)if(mask&(1U<<i))names.push_back(std::u16string{static_cast<char16_t>(u'A'+i),u':',u'\\'});}}
                else {const auto listing=filesystem_.list(utf16_to_utf8(base));if(listing.error)return finish(hr(listing.error));
                    for(const auto& entry:listing.entries){const bool directory=(entry.metadata.attributes&16U)!=0;if(directory?!(flags&0x20U):!(flags&0x40U))continue;
                        if((entry.metadata.attributes&2U) && !(flags&0x80U))continue;names.push_back(utf8_to_utf16_lossy(entry.name));}}
                if(gui_.shell_idlist_enums.size()>=1024)return finish(no_memory);
                if(!gui_.shell_idlist_enum_vtable){const auto table=runtime_allocate(memory,{},7*unit);unsigned i=0;
                    for(const auto& entry:shell_item_service_descriptors())if(entry.family=="internal-shell-idlist-enum")memory.store(GuestAddress{table+unit*i++},bits,resolve(entry.family,entry.symbol).value);gui_.shell_idlist_enum_vtable=table;}
                const auto object=allocate_heap(memory,unit);if(!object)return finish(no_memory);memory.store(GuestAddress{object},bits,gui_.shell_idlist_enum_vtable);
                gui_.shell_idlist_enums.emplace(object,GuiState::ShellIdListEnum{base,std::move(names)});memory.store(GuestAddress{arg(3)},bits,object);return finish(0);
            }
            if(symbol=="BindToObject" || symbol=="BindToStorage"){if(!arg(4))return finish(pointer_error);output(arg(4));if(!arg(1))return finish(invalid);const auto path=child_path(base,arg(1));if(!path)return finish(invalid);
                return finish(bind_path(*path,arg(3),arg(4),symbol=="BindToStorage"));}
            if(symbol=="CompareIDs"){
                // 依名稱（不分大小寫）逐段比較，同一層資料夾排在檔案前；結果放在 HRESULT 的 code 欄位。
                const auto first=child_path(base,arg(2)),second=child_path(base,arg(3));if(!first || !second)return finish(invalid);
                const auto split=[&](const std::u16string& path){std::vector<std::u16string> parts;const auto start=base.empty()?0:base.size()+1;std::size_t at=start;
                    if(base.empty()){parts.push_back(path.substr(0,3));at=3;}for(;at<path.size();){auto end=path.find(u'\\',at);if(end==path.npos)end=path.size();if(end>at)parts.push_back(path.substr(at,end-at));at=end+1;}return parts;};
                const auto a=split(*first),b=split(*second);std::int32_t order=0;std::u16string prefix=base;
                for(std::size_t i=0;i<std::min(a.size(),b.size()) && !order;++i){
                    auto x=a[i],y=b[i];for(auto& c:x)c=unicode_case_unit(c,false);for(auto& c:y)c=unicode_case_unit(c,false);
                    if(x!=y){const auto join=[&](const std::u16string& name){return prefix.empty()?name:prefix+(prefix.back()==u'\\'?u"":u"\\")+name;};
                        const bool da=is_folder(join(a[i])),db=is_folder(join(b[i]));order=da!=db?(da?-1:1):(x<y?-1:1);}
                    prefix=prefix.empty()?a[i]:prefix+(prefix.back()==u'\\'?u"":u"\\")+a[i];}
                if(!order && a.size()!=b.size())order=a.size()<b.size()?-1:1;return finish(static_cast<std::uint16_t>(static_cast<std::int16_t>(order)));
            }
            if(symbol=="CreateViewObject"){output(arg(3));return finish(no_interface);} // 沒有資料夾檢視（IShellView）後端
            if(symbol=="GetAttributesOf"){
                if(!arg(3))return finish(invalid);const auto requested=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));std::uint32_t combined=requested;
                if(!d(1)){if(!base.empty()){std::uint32_t masked=0;if(const auto status=shell_path_attributes(base,requested,masked))return finish(status);combined=masked;}
                    else combined=requested&0x20000000U;} // 桌面：SFGAO_FOLDER
                for(std::uint32_t i=0;i<d(1);++i){const auto path=child_path(base,memory.load(GuestAddress{arg(2)+i*unit},bits));if(!path)return finish(invalid);
                    std::uint32_t masked=0;if(const auto status=shell_path_attributes(*path,requested,masked))return finish(status);combined&=masked;}
                memory.store(GuestAddress{arg(3)},32,combined);return finish(0);
            }
            if(symbol=="GetUIObjectOf"){
                // 單一子項目的 IExtractIconW 交給 SHCreateFileExtractIconW；其他 UI 物件（IContextMenu、IDataObject 等）尚無後端。
                if(!arg(6))return finish(pointer_error);output(arg(6));if(!arg(4))return finish(invalid);if(guid(memory,arg(4))!=extract_icon_iid || d(2)!=1)return finish(no_interface);
                const auto path=child_path(base,memory.load(GuestAddress{arg(3)},bits));if(!path)return finish(invalid);const auto info=filesystem_.information(utf16_to_utf8(*path));
                GuestScratch temporary(*this,memory,state);const auto text=temporary.text(*path);if(!text)return finish(no_memory);
                return finish(nested_service(state,memory,"shell32.dll","SHCreateFileExtractIconW",{text,info.error?0U:info.metadata.attributes,arg(4),arg(6)})&UINT32_MAX);
            }
            if(symbol=="GetDisplayNameOf"){
                // SHGDN_FORPARSING（0x8000）且非 INFOLDER（1）為完整路徑，否則為名稱；以 STRRET_WSTR 回傳。
                if(!arg(3))return finish(invalid);const auto path=child_path(base,arg(1));if(!path)return finish(invalid);const auto flags=d(2);
                const auto text=(flags&0x8000U) && !(flags&1U)?*path:leaf(*path);const auto bytes=utf16_bytes(text);const auto value=allocate_heap(memory,bytes.size());if(!value)return finish(no_memory);
                memory.write(GuestAddress{value},bytes);gui_.task_allocations.insert(value);memory.store(GuestAddress{arg(3)},32,0);memory.store(GuestAddress{arg(3)+unit},bits,value);return finish(0);
            }
            if(symbol=="SetNameOf"){
                // (hwnd, pidl, 新名稱, SHGDN, ppidlOut)：在同一資料夾內改名。
                if(arg(5))output(arg(5));if(!arg(2) || !arg(3))return finish(invalid);const auto path=child_path(base,arg(2));if(!path)return finish(invalid);
                const auto name=read_utf16(memory,GuestAddress{arg(3)},32767);if(name.empty() || name.find_first_of(u"\\/:*?\"<>|")!=name.npos)return finish(invalid);
                const auto target=parent_of(*path)+u"\\"+name;GuestScratch temporary(*this,memory,state);const auto from=temporary.text(*path),to=temporary.text(target);if(!from || !to)return finish(no_memory);
                if(!(nested_service(state,memory,"kernel32.dll","MoveFileW",{from,to})&UINT32_MAX))return finish(hr(last_error_?last_error_:5));
                if(arg(5)){const auto pidl=child_pidl(base,target);if(pidl)memory.store(GuestAddress{arg(5)},bits,pidl);}return finish(0);
            }
            return finish(invalid);
        }
        if(service.family=="internal-shell-item-array"){
            const auto found=gui_.shell_item_arrays.find(arg(0));if(found==gui_.shell_item_arrays.end())return finish(invalid);auto& object=found->second;
            if(symbol=="QueryInterface"){if(!arg(2))return finish(pointer_error);const auto iid=arg(1)?guid(memory,arg(1)):Guid{};output(arg(2));if(!arg(1))return finish(pointer_error);if(iid!=unknown && iid!=array_iid)return finish(no_interface);++object.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);}
            if(symbol=="AddRef")return finish(++object.references);
            if(symbol=="Release"){if(--object.references)return finish(object.references);gui_.shell_item_arrays.erase(found);(void)free_heap(memory,arg(0),state);return finish(0);}
            if(symbol=="GetCount"){if(!arg(1))return finish(pointer_error);memory.store(GuestAddress{arg(1)},32,object.paths.size());return finish(0);}
            if(symbol=="GetItemAt"){if(!output(arg(2)))return finish(pointer_error);if(d(1)>=object.paths.size())return finish(invalid);memory.store(GuestAddress{arg(2)},bits,create_shell_item(memory,object.paths[d(1)]));return finish(0);}
            if(symbol=="EnumItems"){if(!output(arg(1)))return finish(pointer_error);memory.store(GuestAddress{arg(1)},bits,create_shell_enumerator(memory,object.paths,0));return finish(0);}
            if(symbol=="BindToHandler"){
                // BHID_EnumItems → IEnumShellItems；其他處理常式沒有對應物件（MK_E_NOOBJECT）。
                if(!output(arg(4)))return finish(pointer_error);if(!arg(2) || !arg(3))return finish(pointer_error);
                if(guid(memory,arg(2))!=enum_handler)return finish(0x800401e5U);const auto iid=guid(memory,arg(3));if(iid!=unknown && iid!=enum_iid)return finish(no_interface);
                memory.store(GuestAddress{arg(4)},bits,create_shell_enumerator(memory,object.paths,0));return finish(0);
            }
            if(symbol=="GetAttributes"){
                // SIATTRIBFLAGS：AND 1、OR 2、APPCOMPAT 3（同 AND）；全部符合 mask 時回 S_OK，否則 S_FALSE。
                if(!arg(3))return finish(pointer_error);memory.store(GuestAddress{arg(3)},32,0);const auto mode=d(1)&3U;if(!mode)return finish(invalid);
                std::uint32_t combined=mode==2?0U:d(2);
                for(const auto& path:object.paths){std::uint32_t masked=0;if(const auto status=shell_path_attributes(path,d(2),masked))return finish(status);combined=mode==2?combined|masked:combined&masked;}
                memory.store(GuestAddress{arg(3)},32,combined);return finish(combined==d(2)?0:1);
            }
            if(symbol=="GetPropertyStore" || symbol=="GetPropertyDescriptionList"){output(arg(3));return finish(0x80004001U);} // 檔案項目的屬性系統尚無共同後端
            return finish(invalid);
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
                if(arg(3))memory.store(GuestAddress{arg(3)},32,take);return finish(take==count?0:1); // 少於要求數量時回 S_FALSE
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
            if(!arg(2))return finish(pointer_error);memory.store(GuestAddress{arg(2)},32,0);std::uint32_t masked=0;
            if(const auto status=shell_path_attributes(object.path,d(1),masked))return finish(status);memory.store(GuestAddress{arg(2)},32,masked);return finish(masked==d(1)?0:1);
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
