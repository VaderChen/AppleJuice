#include "x64arm/windows_services.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/windows_icon.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/windows_system_paths.hpp"
#include "automation_value.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <functional>
#include <set>

namespace x64arm {
std::span<const WindowsServiceDescriptor> shell_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"shell32.dll","SetCurrentProcessExplicitAppUserModelID",1},{"shell32.dll","GetCurrentProcessExplicitAppUserModelID",1},
        {"shell32.dll","ShellExecuteA",6},{"shell32.dll","ShellExecuteW",6},
        {"shell32.dll","ShellExecuteExA",1},{"shell32.dll","ShellExecuteExW",1},
        {"shell32.dll","FindExecutableA",3},{"shell32.dll","FindExecutableW",3},
        {"shell32.dll","SHAppBarMessage",2},{"shell32.dll","SHOpenFolderAndSelectItems",4},{"shell32.dll","SHOpenWithDialog",2},
        {"shell32.dll","SHChangeNotify",4},{"shell32.dll","SHGetPropertyStoreForWindow",3},
        {"ole32.dll","PropVariantClear",1},{"ole32.dll","PropVariantCopy",2},{"propsys.dll","InitPropVariantFromCLSID",2},{"propsys.dll","PSCreateMemoryPropertyStore",2},{"propsys.dll","VariantCompare",2},{"propsys.dll","PropVariantToStringAlloc",2},{"propsys.dll","PropVariantToString",3},
        {"internal-shell","IPropertyStore.QueryInterface",3},{"internal-shell","IPropertyStore.AddRef",1},{"internal-shell","IPropertyStore.Release",1},
        {"internal-shell","IPropertyStore.GetCount",2},{"internal-shell","IPropertyStore.GetAt",3},{"internal-shell","IPropertyStore.GetValue",3},{"internal-shell","IPropertyStore.SetValue",3},{"internal-shell","IPropertyStore.Commit",1},
        {"shell32.dll","SHGetKnownFolderPath",4},{"shell32.dll","SHGetKnownFolderIDList",4},
        {"shell32.dll","SHGetFolderPathA",5},{"shell32.dll","SHGetFolderPathW",5},
        {"shell32.dll","SHFileOperationA",1},{"shell32.dll","SHFileOperationW",1},
        {"shell32.dll","SHCloneSpecialIDList",3},{"shell32.dll","SHGetSpecialFolderLocation",3},{"shell32.dll","SHGetFileInfoA",5},{"shell32.dll","SHGetFileInfoW",5},
        {"shell32.dll","SHGetSpecialFolderPathA",4},{"shell32.dll","SHGetSpecialFolderPathW",4},
        {"shell32.dll","SHBrowseForFolderA",1},{"shell32.dll","SHBrowseForFolderW",1},
        {"shell32.dll","SHGetPathFromIDListA",2},{"shell32.dll","SHGetPathFromIDListW",2},
        {"shell32.dll","ILCreateFromPathA",1},{"shell32.dll","ILCreateFromPathW",1},
        {"shell32.dll","SHILCreateFromPath",3},{"shell32.dll","ILFree",1},
        {"shell32.dll","SHGetMalloc",1},{"ole32.dll","CoGetMalloc",2},
        {"internal-shell","IMalloc.QueryInterface",3},{"internal-shell","IMalloc.AddRef",1},{"internal-shell","IMalloc.Release",1},
        {"internal-shell","IMalloc.Alloc",2},{"internal-shell","IMalloc.Realloc",3},{"internal-shell","IMalloc.Free",2},
        {"internal-shell","IMalloc.GetSize",2},{"internal-shell","IMalloc.DidAlloc",2},{"internal-shell","IMalloc.HeapMinimize",1},
        {"ole32.dll","CoTaskMemRealloc",2},{"ole32.dll","CoTaskMemAlloc",1},{"ole32.dll","CoTaskMemFree",1}
        ,{"shell32.dll","SHGetDesktopFolder",1}
    };return table;
}
namespace {
constexpr std::uint32_t invalid_argument=0x80070057U,not_implemented=0x80004001U;
constexpr std::uint32_t browse_selection_changed=0x10005;
std::uint32_t hresult(std::uint32_t error){return error?0x80070000U|(error&0xffffU):0;}
std::u16string normalized(std::u16string path){
    std::replace(path.begin(),path.end(),u'/',u'\\');
    if(path.size()<3 || path[1]!=u':' || path[2]!=u'\\')return {};
    if(path[0]>=u'a' && path[0]<=u'z')path[0]-=32;
    if(path[0]<u'A' || path[0]>u'Z')return {};
    std::u16string result=path.substr(0,3);
    for(std::size_t at=3;at<path.size();){
        auto end=path.find(u'\\',at);if(end==path.npos)end=path.size();const auto part=path.substr(at,end-at);
        if(part==u".."){const auto slash=result.find_last_of(u'\\');if(result.size()>3)result.resize(slash<3?3:slash);}
        else if(!part.empty() && part!=u"."){if(part.find_first_of(u"\"<>|:*?")!=part.npos)return {};if(result.size()>3)result+=u'\\';result+=part;}
        at=end+1;
    }
    return result;
}
std::u16string parent_path(const std::u16string& path){const auto pos=path.find_last_of(u'\\');return pos<3?path.substr(0,3):path.substr(0,pos);}
std::u16string leaf_name(const std::u16string& path){return path.size()<=3?path:path.substr(path.find_last_of(u'\\')+1);}
bool within(std::u16string path,std::u16string root){
    const auto fold=[](std::u16string& value){for(auto& c:value)if(c>=u'A' && c<=u'Z')c+=32;};fold(path);fold(root);
    return path==root || (path.starts_with(root) && (root.back()==u'\\' || path[root.size()]==u'\\'));
}
std::u16string known_folder(std::uint32_t id){
    switch(id){
        case 0:case 0x10:return u"C:\\Users\\User\\Desktop";
        case 2:return u"C:\\Users\\User\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs";
        case 5:return u"C:\\Users\\User\\Documents";
        case 6:return u"C:\\Users\\User\\Favorites";
        case 0xd:return u"C:\\Users\\User\\Music";
        case 0xe:return u"C:\\Users\\User\\Videos";
        case 0x27:return u"C:\\Users\\User\\Pictures";
        case 0x100:return u"C:\\Users\\User\\Downloads";
        case 0x101:return u"C:\\Users\\User\\AppData\\LocalLow";
        case 0x102:return u"C:\\Users\\User\\Saved Games";
        case 0x103:return u"C:\\Users\\Public";
        case 0x104:return u"C:\\Users";
        case 7:return u"C:\\Users\\User\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
        case 0xb:return u"C:\\Users\\User\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu";
        case 0x16:return u"C:\\ProgramData\\Microsoft\\Windows\\Start Menu";
        case 0x17:return u"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs";
        case 0x18:return u"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
        case 0x19:return u"C:\\Users\\Public\\Desktop";
        case 0x1a:return u"C:\\Users\\User\\AppData\\Roaming";
        case 0x1c:return u"C:\\Users\\User\\AppData\\Local";
        case 0x23:return u"C:\\ProgramData";
        case 0x24:return windows_directory;
        case 0x25:return windows_system_directory;
        case 0x26:return u"C:\\Program Files";
        case 0x28:return u"C:\\Users\\User";
        case 0x29:return windows_wow64_directory;
        case 0x2a:return u"C:\\Program Files (x86)";
        case 0x2b:return u"C:\\Program Files\\Common Files";
        case 0x2c:return u"C:\\Program Files (x86)\\Common Files";
        case 0x2e:return u"C:\\Users\\Public\\Documents";
        default:return {};
    }
}
std::u16string known_folder_guid(const GuestMemory& memory,std::uint64_t address){
    // 識別值來自 Microsoft KNOWNFOLDERID；路徑共用本沙箱既有 CSIDL 模型。
    static const std::map<std::array<std::uint8_t,16>,std::uint32_t> ids{
        {{0x3a,0xcc,0xbf,0xb4,0x2c,0xdb,0x4c,0x42,0xb0,0x29,0x7f,0xe9,0x9a,0x87,0xc6,0x41},16}, // Desktop
        {{0xd0,0x9a,0xd3,0xfd,0x8f,0x23,0xaf,0x46,0xad,0xb4,0x6c,0x85,0x48,0x03,0x69,0xc7},5}, // Documents
        {{0x90,0xe2,0x4d,0x37,0x3f,0x12,0x65,0x45,0x91,0x64,0x39,0xc4,0x92,0x5e,0x46,0x7b},256}, // Downloads
        {{0x85,0x27,0xb3,0xf1,0xba,0x6f,0xcf,0x4f,0x9d,0x55,0x7b,0x8e,0x7f,0x15,0x70,0x91},28}, // LocalAppData
        {{0xdb,0x85,0xb6,0x3e,0xf9,0x65,0xf6,0x4c,0xa0,0x3a,0xe3,0xef,0x65,0x72,0x9f,0x3d},26}, // RoamingAppData
        {{0x8f,0x85,0x6c,0x5e,0x22,0x0e,0x60,0x47,0x9a,0xfe,0xea,0x33,0x17,0xb6,0x71,0x73},40}, // Profile
        {{0x82,0x5d,0xab,0x62,0xc1,0xfd,0xc3,0x4d,0xa9,0xdd,0x07,0x0d,0x1d,0x49,0x5d,0x97},35}, // ProgramData
        {{0x77,0x4e,0xc1,0x1a,0xe7,0x02,0x5d,0x4e,0xb7,0x44,0x2e,0xb1,0xae,0x51,0x98,0xb7},37}, // System
        {{0xb0,0x31,0x52,0xd6,0xf1,0xb2,0x57,0x48,0xa4,0xce,0xa8,0xe7,0xc6,0xea,0x7d,0x27},41}, // SystemX86
        {{0x04,0xf4,0x8b,0xf3,0x43,0x1d,0xf2,0x42,0x93,0x05,0x67,0xde,0x0b,0x28,0xfc,0x23},36}, // Windows
        {{0xb6,0x63,0x5e,0x90,0xbf,0xc1,0x4e,0x49,0xb2,0x9c,0x65,0xb7,0x32,0xd3,0xd2,0x1a},38}, // ProgramFiles
        {{0xef,0x40,0x5a,0x7c,0xfb,0xa0,0xfc,0x4b,0x87,0x4a,0xc0,0xf2,0xe0,0xb9,0xfa,0x8e},42}, // ProgramFilesX86
        {{0x05,0xed,0xf1,0xf7,0x6d,0x9f,0xa2,0x47,0xaa,0xae,0x29,0xd3,0x17,0xc6,0xf0,0x66},43}, // ProgramFilesCommon
        {{0x24,0x4d,0x97,0xde,0xc6,0xd9,0x3e,0x4d,0xbf,0x91,0xf4,0x45,0x51,0x20,0xb9,0x17},44}, // ProgramFilesCommonX86
        {{0xc3,0x53,0x5b,0x62,0x48,0xab,0xc1,0x4e,0xba,0x1f,0xa1,0xef,0x41,0x46,0xfc,0x19},11}, // StartMenu
        {{0x77,0x5d,0x7f,0xa7,0x2b,0x2e,0xc3,0x44,0xa6,0xa2,0xab,0xa6,0x01,0x05,0x4a,0x51},2}, // Programs
        {{0xbb,0x20,0x7d,0xb9,0x6a,0xf4,0x97,0x4c,0xba,0x10,0x5e,0x36,0x08,0x43,0x08,0x54},7}, // Startup
        {{0x19,0x57,0x11,0xa4,0x2e,0xd6,0x1d,0x49,0xaa,0x7c,0xe7,0x4b,0x8b,0xe3,0xb0,0x67},22}, // CommonStartMenu
        {{0x4e,0xd4,0x39,0x01,0xfe,0x6a,0xf2,0x49,0x86,0x90,0x3d,0xaf,0xca,0xe6,0xff,0xb8},23}, // CommonPrograms
        {{0x35,0xea,0xa5,0x82,0xcd,0xd9,0xc5,0x47,0x96,0x29,0xe1,0x5d,0x2f,0x71,0x4e,0x6e},24}, // CommonStartup
        {{0x0d,0x34,0xaa,0xc4,0x0f,0xf2,0x63,0x48,0xaf,0xef,0xf8,0x7e,0xf2,0xe6,0xba,0x25},25}, // PublicDesktop
        {{0xaf,0x24,0x48,0xed,0xe4,0xdc,0xa8,0x45,0x81,0xe2,0xfc,0x79,0x65,0x08,0x36,0x34},46}, // PublicDocuments
        {{0x71,0xd5,0xd8,0x4b,0x19,0x6d,0xd3,0x48,0xbe,0x97,0x42,0x22,0x20,0x08,0x0e,0x43},13}, // Music
        {{0x30,0x81,0xe2,0x33,0x1e,0x4e,0x76,0x46,0x83,0x5a,0x98,0x39,0x5c,0x3b,0xc3,0xbb},39}, // Pictures
        {{0x1d,0x9b,0x98,0x18,0xb5,0x99,0x5b,0x45,0x84,0x1c,0xab,0x7c,0x74,0xe4,0xdd,0xfc},14}, // Videos
        {{0x61,0xf7,0x77,0x17,0xad,0x68,0x8a,0x4d,0x87,0xbd,0x30,0xb7,0x59,0xfa,0x33,0xdd},6}, // Favorites
        {{0xa4,0xa1,0x20,0xa5,0x80,0x17,0xf6,0x4f,0xbd,0x18,0x16,0x73,0x43,0xc5,0xaf,0x16},257}, // LocalAppDataLow
        {{0xff,0x32,0x5c,0x4c,0x9d,0xbb,0xb0,0x43,0xb5,0xb4,0x2d,0x72,0xe5,0x4e,0xaa,0xa4},258}, // SavedGames
        {{0xa2,0x76,0xdf,0xdf,0x2a,0xc8,0x63,0x4d,0x90,0x6a,0x56,0x44,0xac,0x45,0x73,0x85},259}, // Public
        {{0x72,0xd2,0x62,0x07,0x0a,0xc5,0xb0,0x4b,0xa3,0x82,0x69,0x7d,0xcd,0x72,0x9b,0x80},260}, // UserProfiles
    };
    std::array<std::uint8_t,16> guid{};memory.read(GuestAddress{address},guid);const auto found=ids.find(guid);return found==ids.end()?std::u16string{}:known_folder(found->second);
}
void write_text(GuestMemory& memory,std::uint64_t address,const std::u16string& text,bool wide){
    if(wide)memory.write(GuestAddress{address},utf16_bytes(text));
    else {const auto bytes=utf16_to_utf8(text);memory.write(GuestAddress{address},{reinterpret_cast<const std::uint8_t*>(bytes.c_str()),bytes.size()+1});}
}

}
std::uint64_t WindowsServices::shell_make_pidl(GuestMemory& memory,std::u16string path){
    if(path.empty() || path.size()>=260 || gui_.shell_pidls.size()>=8192)return 0;
    const auto text=utf16_bytes(path);const auto address=allocate_heap(memory,text.size()+4);if(!address)return 0;
    // PIDL 是虛擬 Shell 的 opaque SHITEMID；仍有合法 cb 與 0 結尾，配置由 CoTaskMemFree/ILFree 釋放。
    memory.store(GuestAddress{address},16,text.size()+2);memory.write(GuestAddress{address+2},text);memory.store(GuestAddress{address+2+text.size()},16,0);
    try{gui_.shell_pidls.emplace(address,std::move(path));}catch(...){(void)free_heap(memory,address,GuestState{});throw;}return address;
}
void WindowsServices::shell_browse_refresh(std::uint64_t handle){
    auto& data=gui_.shell_browsers.at(handle);auto& list=gui_.windows.at(gui_child(handle,412));list.items.clear();list.selection=-1;
    gui_.windows.at(gui_child(handle,410)).title=data.directory;present_gui(gui_child(handle,410));
    const auto snapshot=filesystem_.list(utf16_to_utf8(data.directory));
    gui_.windows.at(gui_child(handle,499)).title=snapshot.error?u"無法存取目錄，請選擇已映射的 C:\\ 路徑。":data.status;
    if(data.directory!=data.root)list.items.push_back({gui_.next_item++,0,1,u"..",0});
    for(const auto& entry:snapshot.entries)if(entry.metadata.attributes&16)list.items.push_back({gui_.next_item++,0,1,utf8_to_utf16_lossy(entry.name),0});
    present_gui(gui_child(handle,412));present_gui(gui_child(handle,499));
}
std::optional<std::uint64_t> WindowsServices::shell_browse_control(GuestMemory& memory,const GuiMessage& message,bool){
    const auto found=gui_.shell_browsers.find(message.window);if(found==gui_.shell_browsers.end())return std::nullopt;
    auto& data=found->second;const auto msg=message.message;
    if(msg==0x466 || msg==0x467 || msg==0x46a){ // BFFM_SETSELECTIONA/W, BFFM_SETEXPANDED
        std::u16string path;
        if(message.wparam)path=message.lparam?(msg==0x466?utf8_to_utf16_lossy(read_ansi(memory,message.lparam)):read_utf16(memory,GuestAddress{message.lparam})):u"";
        else if(const auto pidl=gui_.shell_pidls.find(message.lparam);pidl!=gui_.shell_pidls.end() && allocations_.contains(pidl->first))path=pidl->second;
        path=normalized(path);if(path.empty() || !within(path,data.root))return 0;
        const auto info=filesystem_.information(utf16_to_utf8(path));if(info.error || !(info.metadata.attributes&16))return 0;
        const bool changed=data.directory!=path;data.directory=path;shell_browse_refresh(message.window);
        if(changed && data.callback){if(std::none_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& item){return item.window==message.window && item.message==browse_selection_changed;}))enqueue_gui_message({message.window,0,0,browse_selection_changed});}
        return 1;
    }
    if(msg==0x464 || msg==0x468){
        data.status=message.lparam?(msg==0x464?utf8_to_utf16_lossy(read_ansi(memory,message.lparam)):read_utf16(memory,GuestAddress{message.lparam})):u"";
        gui_.windows.at(gui_child(message.window,499)).title=data.status;present_gui(gui_child(message.window,499));return 0;
    }
    if(msg==0x465){gui_set_window_enabled(gui_.windows.at(gui_child(message.window,1)),message.lparam!=0);present_gui(gui_child(message.window,1));return 0;}
    if(msg==0x469){gui_.windows.at(gui_child(message.window,1)).title=message.lparam?read_utf16(memory,GuestAddress{message.lparam}):u"";present_gui(gui_child(message.window,1));return 0;}
    return std::nullopt;
}
std::uint32_t WindowsServices::finish_shell_delete(GuestMemory& memory,const GuiShellDelete& operation,bool confirmed){
    const auto finish=[&](std::uint32_t error){memory.store(GuestAddress{operation.aborted_address},32,error?1:0);return error;};
    if(!confirmed)return finish(1223);
    if(!filesystem_.writable())return finish(5);
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許 Shell 刪除宿主檔案");
    struct Entry {std::string path,key;bool directory;};
    std::vector<Entry> roots,tree;std::set<std::string> seen;
    // 先驗證整批目標與子目錄，再進行刪除；所有 I/O 沿用映射及 NOFOLLOW 邊界。
    std::function<std::uint32_t(const std::string&,unsigned)> inspect=[&](const std::string& path,unsigned depth)->std::uint32_t{
        if(depth>128 || tree.size()>=100000)return 8;
        const auto info=filesystem_.information(path);if(info.error)return info.error;
        if(info.key.ends_with(":/") || (info.metadata.attributes&1U))return 5;
        if(std::any_of(files_.begin(),files_.end(),[&](const auto& file){return file.second.key==info.key && !(file.second.sharing&4U);}))return 32;
        if(!seen.insert(info.key).second)return 0;
        const bool directory=(info.metadata.attributes&0x10U)!=0;
        if(directory){const auto entries=filesystem_.list(path);if(entries.error)return entries.error;
            for(const auto& child:entries.entries)if(const auto error=inspect(path+"/"+child.name,depth+1))return error;
        }
        tree.push_back({path,info.key,directory});return 0;
    };
    for(auto path:operation.paths){
        std::replace(path.begin(),path.end(),'\\','/');while(path.size()>3 && path.back()=='/')path.pop_back();
        const auto info=filesystem_.information(path);if(info.error)return finish(info.error);
        if(std::none_of(roots.begin(),roots.end(),[&](const auto& item){return item.key==info.key;}))roots.push_back({path,info.key,(info.metadata.attributes&0x10U)!=0});
    }
    std::sort(roots.begin(),roots.end(),[](const auto& a,const auto& b){return a.key<b.key;});
    std::vector<Entry> selected;
    for(const auto& entry:roots)if(std::none_of(selected.begin(),selected.end(),[&](const auto& parent){return parent.directory && entry.key.starts_with(parent.key+"/");}))selected.push_back(entry);
    for(const auto& entry:selected)if(const auto error=inspect(entry.path,0))return finish(error);
    for(const auto& entry:selected){
        const bool recycle=(operation.flags&0x40U) && entry.path.size()>2 && entry.path[1]==':' && entry.path[2]=='/';
        if(recycle){
            const auto bin=entry.path.substr(0,3)+"$Recycle.Bin";
            auto folded=entry.key.substr(3);if(folded=="$recycle.bin" || folded.starts_with("$recycle.bin/"))return finish(5);
            auto error=filesystem_.create_directory(bin);if(error && error!=183)return finish(error);
            std::string group;
            for(unsigned attempt=0;attempt<128;++attempt){
                group=bin+"/"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+"-"+std::to_string(attempt);
                error=filesystem_.create_directory(group);if(error!=183)break;
            }
            if(error)return finish(error);
            const auto record=group+"/original-path.txt";
            {auto output=filesystem_.open_host(record,0x40000000U,1,0);
                error=output.error;
                if(!error){const auto text=entry.path+"\n";const auto written=output.file->write({reinterpret_cast<const std::uint8_t*>(text.data()),text.size()},0);error=written.error?written.error:written.bytes!=text.size()?112U:output.file->flush();}
            }
            if(!error)error=filesystem_.rename(entry.path,group+"/item",false);
            if(error){(void)filesystem_.remove(record,false);(void)filesystem_.remove(group,true);return finish(error);}
        }else{
            for(const auto& child:tree)if(child.key==entry.key || (entry.directory && child.key.starts_with(entry.key+"/")))
                if(const auto error=filesystem_.remove(child.path,child.directory))return finish(error);
        }
    }
    return finish(0);
}
std::optional<std::uint32_t> WindowsServices::invoke_shell(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol=="SHAppBarMessage" || service.symbol=="SHOpenFolderAndSelectItems" || service.symbol=="SHOpenWithDialog")return invoke_shell_desktop(state,memory,service);
    if(service.symbol=="SHChangeNotify")return invoke_shell_notification(state,memory,service);
    if(service.symbol.starts_with("ShellExecute") || service.symbol.starts_with("FindExecutable"))return invoke_shell_execute(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const bool wide=symbol.ends_with('W') || symbol=="SHILCreateFromPath";
    const auto read=[&](std::uint64_t p){return p?(wide?read_utf16(memory,GuestAddress{p}):utf8_to_utf16_lossy(read_ansi(memory,p))):std::u16string{};};
    std::uint64_t result=0;
    if(symbol=="SHGetMalloc" || symbol=="CoGetMalloc" || symbol.starts_with("IMalloc.")){
        const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
        const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        if(symbol=="SHGetMalloc" || symbol=="CoGetMalloc"){
            const auto output=arg(symbol=="SHGetMalloc"?0:1);if(!output)return finish(0x80070057U);
            memory.store(GuestAddress{output},bits,0);if(symbol=="CoGetMalloc" && arg(0)!=1)return finish(0x80070057U);
            if(!gui_.task_allocator){
                constexpr std::array names{"QueryInterface","AddRef","Release","Alloc","Realloc","Free","GetSize","DidAlloc","HeapMinimize"};
                const auto block=runtime_allocate(memory,{},(names.size()+1)*unit);
                for(unsigned i=0;i<names.size();++i)memory.store(GuestAddress{block+(i+1)*unit},bits,resolve("internal-shell",std::string("IMalloc.")+names[i]).value);
                memory.store(GuestAddress{block},bits,block+unit);gui_.task_allocator=block;
            }
            ++gui_.task_allocator_references;memory.store(GuestAddress{output},bits,gui_.task_allocator);return finish(0);
        }
        if(!arg(0) || arg(0)!=gui_.task_allocator)throw HostCallFault("無效的 IMalloc 介面");
        if(symbol=="IMalloc.QueryInterface"){
            if(!arg(2))return finish(0x80004003U);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return finish(0x80004003U);
            std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{arg(1)},iid);
            constexpr std::array<std::uint8_t,16> unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},allocator{2,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
            if(iid!=unknown && iid!=allocator)return finish(0x80004002U);
            ++gui_.task_allocator_references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);
        }
        if(symbol=="IMalloc.AddRef")return finish(++gui_.task_allocator_references);
        if(symbol=="IMalloc.Release"){if(gui_.task_allocator_references)--gui_.task_allocator_references;return finish(gui_.task_allocator_references);}
        if(symbol=="IMalloc.Alloc"){const auto pointer=allocate_heap(memory,arg(1));if(pointer)gui_.task_allocations.insert(pointer);return finish(pointer);}
        if(symbol=="IMalloc.Realloc"){
            const auto old=arg(1);std::uint64_t pointer=0;
            if(!old)pointer=allocate_heap(memory,arg(2));
            else if(!arg(2)){if(free_heap(memory,old,state)){gui_.task_allocations.erase(old);gui_.shell_pidls.erase(old);}return finish(0);}
            else pointer=reallocate_heap(memory,old,arg(2),state);
            if(pointer){gui_.task_allocations.erase(old);gui_.task_allocations.insert(pointer);gui_.shell_pidls.erase(old);}return finish(pointer);
        }
        if(symbol=="IMalloc.Free"){if(free_heap(memory,arg(1),state)){gui_.task_allocations.erase(arg(1));gui_.shell_pidls.erase(arg(1));}return finish(0);}
        if(symbol=="IMalloc.GetSize"){const auto found=allocations_.find(arg(1));return finish(found==allocations_.end()?(x86_?UINT32_MAX:UINT64_MAX):found->second);}
        if(symbol=="IMalloc.DidAlloc")return finish(!arg(1)?UINT32_MAX:gui_.task_allocations.contains(arg(1)) && allocations_.contains(arg(1))?1:allocations_.contains(arg(1))?UINT32_MAX:0);
        // free_heap 已逐次取消空頁映射並合併相鄰空洞，不需延後到 HeapMinimize 才回收。
        if(symbol=="IMalloc.HeapMinimize")return finish(0);
    }
    if(symbol=="SetCurrentProcessExplicitAppUserModelID" || symbol=="GetCurrentProcessExplicitAppUserModelID"){
        try {
            if(!arg(0))result=0x80070057U;
            else if(symbol.starts_with("Set")){
                std::u16string value;bool terminated=false;
                for(unsigned i=0;i<=128;++i){const auto c=memory.load(GuestAddress{arg(0)+i*2},16);if(!c){terminated=true;break;}value+=static_cast<char16_t>(c);}
                if(!terminated || value.empty() || value.find(u' ')!=value.npos)result=0x80070057U;
                else gui_.explicit_app_user_model_id=std::move(value);
            }else {
                memory.store(GuestAddress{arg(0)},x86_?32:64,0);
                if(gui_.explicit_app_user_model_id.empty())result=0x80004005U;
                else{const auto bytes=utf16_bytes(gui_.explicit_app_user_model_id);const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)result=0x8007000eU;else{memory.write(GuestAddress{pointer},bytes);memory.store(GuestAddress{arg(0)},x86_?32:64,pointer);}}
            }
        }catch(const MemoryFault&){result=0x80070057U;}catch(const std::bad_alloc&){result=0x8007000eU;}
        arg.return_to(state,result);return std::nullopt;
    }
    if(symbol.starts_with("PropVariantToString") || symbol=="InitPropVariantFromCLSID" || symbol=="SHGetPropertyStoreForWindow" || symbol=="PSCreateMemoryPropertyStore" || symbol=="VariantCompare" || symbol.starts_with("IPropertyStore.") || symbol=="PropVariantClear" || symbol=="PropVariantCopy"){
        const auto bits=static_cast<std::uint8_t>(x86_?32:64);const unsigned unit=x86_?4:8,size=x86_?16:24;
        constexpr std::uint32_t pointer_error=0x80004003U,no_interface=0x80004002U;
        constexpr std::array<std::uint8_t,16> iid{0xeb,0x8e,0x6d,0x88,0xf2,0x8c,0x46,0x44,0x8d,0x02,0xcd,0xba,0x1d,0xbd,0xcf,0x99},unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
        const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto scalar_size=[](unsigned type)->unsigned{switch(type){case 0:case 1:return 0;case 16:case 17:return 1;case 2:case 11:case 18:return 2;case 3:case 4:case 10:case 19:case 22:case 23:return 4;case 5:case 6:case 7:case 20:case 21:case 64:return 8;case 14:return 16;default:return 99;}};
        const auto indirect=[](unsigned type){return type==8 || type==30 || type==31 || type==65 || type==72 || type==0x1011;};
        const auto read_payload=[&](std::uint16_t type,std::uint64_t payload){
            GuiStoredVariant v;v.type=type;
            const auto n=scalar_size(v.type);
            if(n!=99){if(n)memory.read(GuestAddress{payload-(v.type==14?8U:0U)},std::span{v.scalar}.first(n));return v;}
            if(!indirect(v.type))throw std::invalid_argument("尚未支援的 PROPVARIANT 型別");
            const bool counted=v.type==65 || v.type==0x1011;const auto pointer=memory.load(GuestAddress{payload+(counted?unit:0)},bits);
            std::size_t length=0;
            if(counted)length=memory.load(GuestAddress{payload},32);
            else if(pointer){if(v.type==8)length=memory.load(GuestAddress{pointer-4},32);else if(v.type==72)length=16;else if(v.type==31){v.payload=utf16_bytes(read_utf16(memory,GuestAddress{pointer}));return v;}else {const auto text=read_ansi(memory,pointer);v.payload.assign(text.begin(),text.end());v.payload.push_back(0);return v;}}
            if(length>16*1024*1024 || (length && !pointer))throw std::invalid_argument("PROPVARIANT 資料大小不合法");
            v.payload.resize(length);if(length)memory.read(GuestAddress{pointer},v.payload);return v;
        };
        const auto read_variant=[&](std::uint64_t at){
            memory.check(GuestAddress{at},size,Access::Read);
            return read_payload(static_cast<std::uint16_t>(memory.load(GuestAddress{at},16)),at+8);
        };
        const auto string_value=[&](const GuiStoredVariant& v)->std::u16string{
            if(!v.type)return {};
            if(v.type==8 || v.type==31){
                if(v.payload.size()%2)throw std::invalid_argument("寬字串位元組數不合法");
                std::u16string text;for(std::size_t i=0;i<v.payload.size();i+=2)text+=static_cast<char16_t>(v.payload[i]|(static_cast<unsigned>(v.payload[i+1])<<8));
                if(v.type==31 && !text.empty())text.pop_back();return text;
            }
            if(v.type==30){std::string text(v.payload.begin(),v.payload.end());if(!text.empty())text.pop_back();return utf8_to_utf16_lossy(text);}
            if(v.type==72){if(v.payload.size()!=16)throw std::invalid_argument("CLSID 長度不合法");WindowsGuid guid{};std::copy(v.payload.begin(),v.payload.end(),guid.begin());return windows_guid_text(guid);}
            automation::Value value{};const auto n=scalar_size(v.type);
            if(n==99 || v.type==1 || v.type==10 || v.type==64)throw std::invalid_argument("無法轉換為字串的 PROPVARIANT");
            if(n)std::copy_n(v.scalar.begin(),n,value.begin()+(v.type==14?0:8));automation::put(value,0,2,v.type);
            if(v.type==11)return automation::bool_string(automation::get(value,8,2)!=0,0x409,false);
            if(v.type==7){auto text=automation::format_date(std::bit_cast<double>(automation::get(value,8,8)),0x409,0);if(!text)throw std::invalid_argument("DATE 不合法");return *text;}
            return utf8_to_utf16(automation::number_string(value));
        };
        const auto clear_variant=[&](std::uint64_t at)->std::uint32_t{
            memory.check(GuestAddress{at},size,Access::Write);const auto type=static_cast<unsigned>(memory.load(GuestAddress{at},16));
            if(scalar_size(type)==99){
                if(!indirect(type))return not_implemented;
                const auto pointer=memory.load(GuestAddress{at+8+((type==65 || type==0x1011)?unit:0)},bits);
                if(pointer){const auto base=type==8?pointer-8:pointer;
                    if(!allocations_.contains(base) || (type==8 && memory.load(GuestAddress{base},32)!=0x52545342U))return invalid_argument;
                    if(!free_heap(memory,base,state))return invalid_argument;
                }
            }
            memory.write(GuestAddress{at},std::vector<std::uint8_t>(size));return 0;
        };
        const auto write_variant=[&](std::uint64_t at,const GuiStoredVariant& v){
            memory.check(GuestAddress{at},size,Access::Write);std::vector<std::uint8_t> data(size);std::uint64_t pointer=0;
            const auto n=scalar_size(v.type);
            if(n!=99){if(n)std::copy_n(v.scalar.begin(),n,data.begin()+(v.type==14?0:8));}
            else if(!v.payload.empty() || v.type==8){
                const auto bytes=v.payload.size()+(v.type==8?10:0);const auto base=allocate_heap(memory,bytes);if(!base)throw std::bad_alloc{};pointer=base+(v.type==8?8:0);
                if(v.type==8){memory.store(GuestAddress{base},32,0x52545342U);memory.store(GuestAddress{base+4},32,v.payload.size());memory.store(GuestAddress{pointer+v.payload.size()},16,0);}
                if(!v.payload.empty())memory.write(GuestAddress{pointer},v.payload);
            }
            data[0]=v.type&255;data[1]=v.type>>8;
            if(n==99){const bool counted=v.type==65 || v.type==0x1011;const auto offset=8+(counted?unit:0);for(unsigned i=0;i<unit;++i)data[offset+i]=static_cast<std::uint8_t>(pointer>>(8*i));if(counted)for(unsigned i=0;i<4;++i)data[8+i]=static_cast<std::uint8_t>(v.payload.size()>>(8*i));}
            memory.write(GuestAddress{at},data);
        };
        try{
            if(symbol=="PropVariantToStringAlloc" || symbol=="PropVariantToString"){
                const bool allocated=symbol=="PropVariantToStringAlloc";
                if(!arg(1))return finish(pointer_error);
                if(allocated)memory.store(GuestAddress{arg(1)},bits,0);
                if(!arg(0))return finish(pointer_error);
                memory.check(GuestAddress{arg(0)},size,Access::Read);
                const auto type=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(0)},16));std::u16string text;
                if(type&0x1000U){
                    const auto element=static_cast<std::uint16_t>(type&~0x1000U);auto stride=scalar_size(element);
                    if(element==8 || element==30 || element==31)stride=unit;
                    if(!stride || stride==99 || element==14 || element==64)return finish(automation::mismatch);
                    const auto count=memory.load(GuestAddress{arg(0)+8},32),pointer=memory.load(GuestAddress{arg(0)+8+unit},bits);
                    if(count>1048576 || (count && !pointer))return finish(invalid_argument);
                    memory.check(GuestAddress{pointer},count*stride,Access::Read);
                    for(std::uint64_t i=0;i<count;++i){if(i)text+=u"; ";text+=string_value(read_payload(element,pointer+i*stride));if(text.size()>16*1024*1024)return finish(0x8007000eU);}
                }else text=string_value(read_variant(arg(0)));
                const auto bytes=utf16_bytes(text);
                if(allocated){const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)return finish(0x8007000eU);memory.write(GuestAddress{pointer},bytes);gui_.task_allocations.insert(pointer);memory.store(GuestAddress{arg(1)},bits,pointer);}
                else {const auto capacity=static_cast<std::uint32_t>(arg(2));if(capacity<bytes.size()/2){if(capacity)memory.store(GuestAddress{arg(1)},16,0);return finish(0x8007007aU);}memory.write(GuestAddress{arg(1)},bytes);}
                return finish(0);
            }
            if(symbol=="InitPropVariantFromCLSID"){
                if(!arg(0) || !arg(1))return finish(pointer_error);
                GuiStoredVariant value;value.type=72;value.payload.resize(16);memory.read(GuestAddress{arg(0)},value.payload);
                memory.check(GuestAddress{arg(1)},size,Access::Write);memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(size));write_variant(arg(1),value);return finish(0);
            }
            if(symbol=="PropVariantClear" || symbol=="PropVariantCopy"){
                if(!arg(0) || (symbol=="PropVariantCopy" && !arg(1)))return finish(pointer_error);
                if(symbol=="PropVariantClear")return finish(clear_variant(arg(0)));
                if(arg(0)==arg(1))return finish(0);const auto value=read_variant(arg(1));const auto error=clear_variant(arg(0));if(error)return finish(error);write_variant(arg(0),value);return finish(0);
            }
            if(symbol=="VariantCompare"){
                // PropVariantCompareEx 的預設規則：空值最小；數值依數值比較；字串不分大小寫；不同類別依 VARTYPE 排序。
                const auto left=read_variant(arg(0)),right=read_variant(arg(1));
                const auto empty=[](const GuiStoredVariant& v){return v.type==0 || v.type==1;};
                const auto numeric=[&](const GuiStoredVariant& v)->std::optional<double>{
                    const auto raw=[&](unsigned n){std::uint64_t value=0;for(unsigned i=0;i<n;++i)value|=std::uint64_t{v.scalar[i]}<<(i*8);return value;};
                    switch(v.type){
                    case 16:return static_cast<double>(static_cast<std::int8_t>(raw(1)));case 17:return static_cast<double>(raw(1));
                    case 2:return static_cast<double>(static_cast<std::int16_t>(raw(2)));case 18:return static_cast<double>(raw(2));
                    case 11:return raw(2)?-1.0:0.0;
                    case 3:case 22:return static_cast<double>(static_cast<std::int32_t>(raw(4)));case 19:case 23:return static_cast<double>(raw(4));
                    case 20:return static_cast<double>(static_cast<std::int64_t>(raw(8)));case 21:return static_cast<double>(raw(8));
                    case 4:return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw(4))));
                    case 5:case 7:return std::bit_cast<double>(raw(8));
                    default:return std::nullopt;
                    }
                };
                const auto text=[&](const GuiStoredVariant& v)->std::optional<std::u16string>{if(v.type!=8 && v.type!=30 && v.type!=31)return std::nullopt;auto value=string_value(v);for(auto& c:value)c=unicode_case_unit(c,false);return value;};
                std::int32_t order=0;
                if(empty(left) || empty(right))order=empty(left)==empty(right)?0:empty(left)?-1:1;
                else if(const auto a=numeric(left),b=numeric(right);a && b)order=*a<*b?-1:*a>*b?1:0;
                else if(const auto x=text(left),y=text(right);x && y)order=*x<*y?-1:*x>*y?1:0;
                else if(left.type!=right.type)order=left.type<right.type?-1:1;
                else order=left.payload<right.payload?-1:left.payload>right.payload?1:0;
                return finish(static_cast<std::uint32_t>(order));
            }
            if(symbol=="PSCreateMemoryPropertyStore"){
                if(!arg(1))return finish(pointer_error);memory.store(GuestAddress{arg(1)},bits,0);if(!arg(0))return finish(pointer_error);
                std::array<std::uint8_t,16> requested{};memory.read(GuestAddress{arg(0)},requested);
                if(requested!=iid && requested!=unknown)return finish(no_interface);
                if(gui_.shell_property_stores.size()>=65536)return finish(0x8007000eU);
                if(!gui_.shell_property_vtable){
                    constexpr std::array names{"QueryInterface","AddRef","Release","GetCount","GetAt","GetValue","SetValue","Commit"};
                    const auto table=runtime_allocate(memory,{},names.size()*unit);for(unsigned i=0;i<names.size();++i)memory.store(GuestAddress{table+i*unit},bits,resolve("internal-shell",std::string("IPropertyStore.")+names[i]).value);gui_.shell_property_vtable=table;
                }
                const auto object=allocate_heap(memory,unit);if(!object)return finish(0x8007000eU);memory.store(GuestAddress{object},bits,gui_.shell_property_vtable);
                GuiPropertyStore store;store.memory=true;gui_.shell_property_stores.emplace(object,std::move(store));memory.store(GuestAddress{arg(1)},bits,object);return finish(0);
            }
            if(symbol=="SHGetPropertyStoreForWindow"){
                if(!arg(2))return finish(pointer_error);memory.store(GuestAddress{arg(2)},bits,0);
                if(!gui_.windows.contains(arg(0)))return finish(0x80070578U);
                if(!arg(1))return finish(pointer_error);std::array<std::uint8_t,16> requested{};memory.read(GuestAddress{arg(1)},requested);
                if(requested!=iid && requested!=unknown)return finish(no_interface);
                if(gui_.shell_property_stores.size()>=65536)return finish(0x8007000eU);
                if(!gui_.shell_property_vtable){
                    constexpr std::array names{"QueryInterface","AddRef","Release","GetCount","GetAt","GetValue","SetValue","Commit"};
                    const auto table=runtime_allocate(memory,{},names.size()*unit);for(unsigned i=0;i<names.size();++i)memory.store(GuestAddress{table+i*unit},bits,resolve("internal-shell",std::string("IPropertyStore.")+names[i]).value);gui_.shell_property_vtable=table;
                }
                const auto object=allocate_heap(memory,unit);if(!object)return finish(0x8007000eU);memory.store(GuestAddress{object},bits,gui_.shell_property_vtable);
                GuiPropertyStore store;store.window=arg(0);gui_.shell_property_stores.emplace(object,std::move(store));memory.store(GuestAddress{arg(2)},bits,object);return finish(0);
            }
            const auto found=gui_.shell_property_stores.find(arg(0));if(found==gui_.shell_property_stores.end())throw HostCallFault("無效的視窗屬性 COM 介面");auto& object=found->second;
            if(symbol=="IPropertyStore.QueryInterface"){
                if(!arg(2))return finish(pointer_error);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return finish(pointer_error);
                std::array<std::uint8_t,16> requested{};memory.read(GuestAddress{arg(1)},requested);if(requested!=iid && requested!=unknown)return finish(no_interface);
                ++object.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);
            }
            if(symbol=="IPropertyStore.AddRef")return finish(++object.references);
            if(symbol=="IPropertyStore.Release"){const auto refs=--object.references;if(!refs){gui_.shell_property_stores.erase(found);(void)free_heap(memory,arg(0),state);}return finish(refs);}
            const auto window=object.memory?gui_.windows.end():gui_.windows.find(object.window);if(!object.memory && window==gui_.windows.end())return finish(0x80070578U);
            auto& properties=object.memory?object.properties:window->second.shell_properties;
            if(symbol=="IPropertyStore.Commit")return finish(0); // 視窗屬性在 SetValue 立即更新。
            if(!arg(1) && symbol!="IPropertyStore.GetAt")return finish(pointer_error);
            if(symbol=="IPropertyStore.GetCount"){memory.store(GuestAddress{arg(1)},32,properties.size());return finish(0);}
            if(!arg(2))return finish(pointer_error);
            if(symbol=="IPropertyStore.GetAt"){if(arg(1)>=properties.size())return finish(invalid_argument);auto it=properties.begin();std::advance(it,arg(1));memory.write(GuestAddress{arg(2)},it->first);return finish(0);}
            std::array<std::uint8_t,20> key{};memory.read(GuestAddress{arg(1)},key);
            if(symbol=="IPropertyStore.GetValue"){const auto value=properties.find(key);write_variant(arg(2),value==properties.end()?GuiStoredVariant{}:value->second);return finish(0);}
            const auto value=read_variant(arg(2));if(!value.type)properties.erase(key);else {if(properties.size()>=4096 && !properties.contains(key))return finish(0x8007000eU);properties[key]=value;}return finish(0);
        }catch(const std::bad_alloc&){return finish(0x8007000eU);}
        catch(const std::invalid_argument&){return finish(symbol.starts_with("PropVariantToString")?automation::mismatch:not_implemented);}
        catch(const std::range_error&){if(symbol.starts_with("PropVariantToString"))return finish(automation::overflow);throw;}
        catch(const MemoryFault&){if(symbol.starts_with("PropVariantToString"))return finish(pointer_error);throw;}
    }
    if(symbol.starts_with("SHFileOperation")){
        const auto p=arg(0);const unsigned pointer_bits=x86_?32U:64U;
        const auto fail=[&](std::uint32_t error){arg.return_to(state,error);};
        if(!p){fail(87);return std::nullopt;}
        memory.check(GuestAddress{p},x86_?32:56,Access::Read);
        GuiShellDelete operation;operation.aborted_address=p+(x86_?20:36);
        memory.store(GuestAddress{operation.aborted_address},32,1);
        memory.store(GuestAddress{p+(x86_?24:40)},static_cast<std::uint8_t>(pointer_bits),0);
        const auto function=memory.load(GuestAddress{p+(x86_?4:8)},32);
        operation.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{p+(x86_?16:32)},16));
        if(function!=3 || (operation.flags&~0x6d54U)){fail(50);return std::nullopt;}
        auto cursor=memory.load(GuestAddress{p+(x86_?8:16)},static_cast<std::uint8_t>(pointer_bits));
        if(!cursor){fail(87);return std::nullopt;}
        std::u16string text;std::string ansi;bool terminated=false;
        for(unsigned count=0;count<65536;++count){
            const auto c=memory.load(GuestAddress{cursor},wide?16:8);cursor+=wide?2:1;
            if(!c){
                if(text.empty() && ansi.empty()){terminated=true;break;}
                auto path=wide?utf16_to_utf8(text):ansi;
                if(path.find_first_of("*?")!=std::string::npos || path.size()>32767){fail(50);return std::nullopt;}
                operation.paths.push_back(std::move(path));text.clear();ansi.clear();
                if(operation.paths.size()>4096){fail(8);return std::nullopt;}
            }else if(wide)text.push_back(static_cast<char16_t>(c));else ansi.push_back(static_cast<char>(c));
        }
        if(!terminated || operation.paths.empty()){fail(87);return std::nullopt;}
        if(operation.flags&0x10U || !native_gui_){result=finish_shell_delete(memory,operation,(operation.flags&0x10U)!=0);}
        else{
            const auto owner=memory.load(GuestAddress{p},static_cast<std::uint8_t>(pointer_bits));
            GuiWindow dialog;dialog.dpi_context=effective_dpi_context();dialog.thread=threads_.current;dialog.title=operation.flags&0x40U?u"移至回收區":u"永久刪除";dialog.class_name=u"#32770";dialog.dialog=true;dialog.modal=true;dialog.builtin=true;dialog.owner=owner;dialog.width=500;dialog.height=190;dialog.style=0x10000000U;dialog.visible=true;dialog.extra.resize(40);dialog.default_id=2;
            dialog.procedure=resolve("user32.dll","DefDlgProcW").value;dialog.shell_delete=operation;
            const auto handle=allocate_gui_window();std::vector<GuiWindowCreation> plans{{handle,std::move(dialog),true}};
            const auto add=[&](GuiWindow child){child.thread=threads_.current;child.parent=handle;child.style|=0x50000000U;child.visible=true;child.builtin=true;child.control_class=child.class_name;child.procedure=stock_gui_proc(child.class_name,true);plans.push_back({allocate_gui_window(),std::move(child),true});};
            GuiWindow label;label.dpi_context=effective_dpi_context();label.class_name=u"static";label.x=20;label.y=20;label.width=460;label.height=110;
            label.title=u"確定要刪除 "+utf8_to_utf16_lossy(std::to_string(operation.paths.size()))+u" 個項目？\n";
            label.title+=operation.flags&0x40U?u"檔案會保留在所在磁碟的 $Recycle.Bin 回收區。":u"這會永久刪除檔案及所選資料夾的內容。";add(label);
            for(unsigned id:{1U,2U}){GuiWindow button;button.dpi_context=effective_dpi_context();button.class_name=u"button";button.id=id;button.title=id==1?u"刪除":u"取消";button.x=id==1?290:390;button.y=145;button.width=90;button.height=28;button.style=id==2?1:0;add(button);}
            return start_gui_creation(state,memory,std::move(plans));
        }
    }else if(symbol=="SHGetDesktopFolder"){
        if(!arg(0))result=0x80004003U;else {memory.store(GuestAddress{arg(0)},64,0);result=not_implemented;}
    }else if(symbol=="CoTaskMemAlloc"){result=allocate_heap(memory,arg(0));if(result)gui_.task_allocations.insert(result);}
    else if(symbol=="CoTaskMemRealloc"){
        if(!arg(0))result=allocate_heap(memory,arg(1));
        else if(!allocations_.contains(arg(0)))last_error_=6;
        else if(!arg(1)){(void)free_heap(memory,arg(0),state);result=0;}
        else result=reallocate_heap(memory,arg(0),arg(1),state);
        if(result || !arg(1))gui_.task_allocations.erase(arg(0));if(result)gui_.task_allocations.insert(result);
    }
    else if(symbol=="CoTaskMemFree" || symbol=="ILFree"){
        if(arg(0)){gui_.shell_pidls.erase(arg(0));if(!free_heap(memory,arg(0),state))last_error_=6;else gui_.task_allocations.erase(arg(0));}
    }else if(symbol=="SHGetSpecialFolderLocation" || symbol=="SHCloneSpecialIDList"){
        const bool clone=symbol=="SHCloneSpecialIDList";
        const auto flags=static_cast<std::uint32_t>(arg(1))|(clone && arg(2)?0x8000U:0U);const auto path=known_folder(flags&0xff);
        if((!clone && !arg(2)) || (flags&~0xc8ffU))result=invalid_argument;
        else {
            if(!clone)memory.store(GuestAddress{arg(2)},x86_?32:64,0);std::uint32_t error=0;
            if(path.empty())result=not_implemented;
            else {
                if(flags&0x8000){
                    for(std::size_t at=3;at<=path.size();++at)if(at==path.size() || path[at]==u'\\'){
                        const auto prefix=utf16_to_utf8(path.substr(0,at));const auto info=filesystem_.information(prefix);
                        if(info.error){error=filesystem_.create_directory(prefix);if(error && error!=183)break;error=0;}
                        else if(!(info.metadata.attributes&16)){error=183;break;}
                    }
                }else if(!(flags&0x4000)){const auto info=filesystem_.information(utf16_to_utf8(path));error=info.error;if(!error && !(info.metadata.attributes&16))error=3;}
                if(error)result=hresult(error);
                else {const auto pidl=shell_make_pidl(memory,path);if(pidl){if(clone){arg.return_to(state,pidl);return std::nullopt;}memory.store(GuestAddress{arg(2)},x86_?32:64,pidl);}else result=0x8007000eU;}
            }
        }
        if(clone)result=0; // 指標 API 的失敗值不能回傳 HRESULT。
    }else if(symbol=="SHGetFileInfoA" || symbol=="SHGetFileInfoW"){
        const auto flags=static_cast<std::uint32_t>(arg(4));const auto output=arg(2);const auto unit=x86_?4U:8U;const auto size=wide?(x86_?692U:696U):(x86_?352U:360U);
        constexpr std::uint32_t supported=1|2|4|8|16|32|64|0x100|0x200|0x400|0x800|0x4000|0x8000|0x20000;
        std::uint32_t error=0;
        if(flags&~supported)error=50;
        else if(!arg(0) || !output || static_cast<std::uint32_t>(arg(3))<size ||
            ((flags&16) && (flags&(8|0x800))) || ((flags&0x20000) && (flags&0x100)) ||
            ((flags&(4|32|64|0x8000)) && !(flags&0x100)) || ((flags&3) && !(flags&(0x100|0x4000))))error=87;
        else if(!gui_.com_initializations.contains(threads_.current) || gui_.com_initializations.at(threads_.current)<16)error=0x800401f0U;
        if(error){last_error_=error;arg.return_to(state,0);return std::nullopt;}
        memory.check(GuestAddress{output},size,Access::Write);
        const auto mask=flags&0x20000?static_cast<std::uint32_t>(memory.load(GuestAddress{output+unit+4},32)):UINT32_MAX;
        std::u16string path;
        if(flags&8){const auto found=gui_.shell_pidls.find(arg(0));if(found==gui_.shell_pidls.end() || !allocations_.contains(arg(0)))error=87;else path=found->second;}
        else path=read(arg(0));
        if(path.empty() && !(flags&16))error=87;else if(path.size()>=260)error=206;
        std::uint32_t attributes=static_cast<std::uint32_t>(arg(1));
        if(!error && !(flags&16)){const auto info=filesystem_.information(utf16_to_utf8(path));error=info.error;attributes=info.metadata.attributes;}
        if(error){last_error_=error;arg.return_to(state,0);return std::nullopt;}
        std::replace(path.begin(),path.end(),u'/',u'\\');while(path.size()>3 && path.back()==u'\\')path.pop_back();
        const bool drive=path.size()==3 && path[1]==u':' && path[2]==u'\\',folder=(attributes&16)!=0;
        const std::uint32_t index=drive?2U:folder?1U:0U;const auto width=(flags&1)?16U:32U;
        const auto text=[&](std::uint64_t destination,std::u16string value,std::size_t capacity){
            if(wide){if(value.size()>=capacity){value.resize(capacity-1);if(!value.empty() && value.back()>=0xd800 && value.back()<=0xdbff)value.pop_back();}memory.write(GuestAddress{destination},utf16_bytes(value));}
            else {auto bytes=utf16_to_utf8(value);if(bytes.size()>=capacity){auto at=capacity-1;while(at && (static_cast<unsigned char>(bytes[at])&0xc0)==0x80)--at;bytes.resize(at);}memory.write(GuestAddress{destination},{reinterpret_cast<const std::uint8_t*>(bytes.c_str()),bytes.size()+1});}
        };
        if(flags&0x200)text(output+unit+8,leaf_name(path),260);
        if(flags&0x400){
            std::u16string type=drive?u"本機磁碟":folder?u"資料夾":u"檔案";
            if(!folder && !drive){const auto leaf=leaf_name(path);const auto dot=leaf.find_last_of(u'.');if(dot!=leaf.npos && dot+1<leaf.size()){type=leaf.substr(dot+1);for(auto& c:type)if(c>=u'a' && c<=u'z')c-=32;type+=u" 檔案";}}
            text(output+unit+8+(wide?520:260),type,80);
        }
        if(flags&0x800){const auto shell_attributes=0x40000000U|(folder?0x20000000U:0U)|((attributes&1)?0x40000U:0U)|((attributes&2)?0x80000U:0U);memory.store(GuestAddress{output+unit+4},32,shell_attributes&mask);}
        if(flags&(0x100|0x4000))memory.store(GuestAddress{output+unit},32,index);
        if(flags&0x100){auto icon=native_stock_icon(drive?8U:folder?3U:0U,width);if(!icon)icon=GuiIcon{{},width,width,0x10001U+index};icon->system=0x10001U+index;const auto handle=store_gui_icon(std::move(*icon));memory.store(GuestAddress{output},x86_?32:64,handle);}
        result=1;
        if(flags&0x4000){
            auto& handle=(flags&1)?gui_.shell_small_image_list:gui_.shell_large_image_list;
            if(!gui_.image_lists.contains(handle)){
                handle=shell_system_icons(width);if(!handle){handle=gui_.next_object++;GuiImageList images;images.width=static_cast<std::int32_t>(width);images.height=images.width;images.flags=0x21;
                for(std::uint32_t i=0;i<3;++i)images.images.push_back(GuiIcon{{},width,width,0x10001U+i});gui_.image_lists.emplace(handle,std::move(images));}
            }
            result=handle;
        }
    }else if(symbol.starts_with("SHGetPathFromIDList")){
        const auto found=gui_.shell_pidls.find(arg(0));
        if(!arg(1)){last_error_=87;}
        else if(found==gui_.shell_pidls.end() || !allocations_.contains(arg(0))){last_error_=87;memory.store(GuestAddress{arg(1)},wide?16:8,0);}
        else {const auto& path=found->second;if((wide?path.size():utf16_to_utf8(path).size())>=260)last_error_=206;else {write_text(memory,arg(1),path,wide);result=1;}}
    }else if(symbol.starts_with("ILCreateFromPath") || symbol=="SHILCreateFromPath"){
        const bool hr=symbol=="SHILCreateFromPath";auto path=normalized(read(arg(0)));
        if(hr && !arg(1))result=invalid_argument;
        else {
            if(hr)memory.store(GuestAddress{arg(1)},64,0);
            const auto info=path.empty()?FileInformation{{},{},123}:filesystem_.information(utf16_to_utf8(path));
            if(info.error){last_error_=info.error;result=hr?hresult(info.error):0;}
            else {const auto pidl=shell_make_pidl(memory,path);if(hr){if(pidl){memory.store(GuestAddress{arg(1)},64,pidl);if(arg(2)){const auto requested=memory.load(GuestAddress{arg(2)},32);const auto flags=0x40000000U|((info.metadata.attributes&16)?0x20000000U:0U)|((info.metadata.attributes&1)?0x40000U:0U);memory.store(GuestAddress{arg(2)},32,requested&flags);}result=0;}else result=0x8007000eU;}else result=pidl;}
        }
    }else if(symbol=="SHGetKnownFolderPath" || symbol=="SHGetKnownFolderIDList"){
        const bool pidl=symbol=="SHGetKnownFolderIDList";const auto flags=static_cast<std::uint32_t>(arg(1));std::uint32_t error=0;
        if(!arg(3))result=invalid_argument;
        else{
            memory.store(GuestAddress{arg(3)},x86_?32:64,0);
            if(!arg(0) || (flags&~0x1d700U) || ((flags&0x200) && !(flags&0x400)))error=87;
            else if(arg(2))error=(arg(2)==UINT64_MAX || arg(2)==UINT32_MAX)?5U:50U;
            const auto path=error?std::u16string{}:known_folder_guid(memory,arg(0));if(!error && path.empty())error=87;
            if(!error && (flags&0x8000)){
                for(std::size_t at=3;at<=path.size();++at)if(at==path.size() || path[at]==u'\\'){
                    const auto prefix=utf16_to_utf8(path.substr(0,at));const auto info=filesystem_.information(prefix);
                    if(info.error){error=filesystem_.create_directory(prefix);if(error && error!=183)break;error=0;}else if(!(info.metadata.attributes&16)){error=183;break;}
                }
            }else if(!error && !(flags&0x4000)){
                const auto info=filesystem_.information(utf16_to_utf8(path));error=info.error;if(!error && !(info.metadata.attributes&16))error=3;
            }
            if(!error){
                const auto bytes=utf16_bytes(path);const auto pointer=pidl?shell_make_pidl(memory,path):allocate_heap(memory,bytes.size());if(!pointer)error=8;
                else{if(!pidl)memory.write(GuestAddress{pointer},bytes);memory.store(GuestAddress{arg(3)},x86_?32:64,pointer);}
            }
            result=hresult(error);
        }
    }else if(symbol.starts_with("SHGetFolderPath") || symbol.starts_with("SHGetSpecialFolderPath")){
        const bool special=symbol.starts_with("SHGetSpecial");const auto flags=static_cast<std::uint32_t>(arg(special?2:1));const auto output=arg(special?1:4);
        const bool create=(flags&0x8000) || (special && arg(3));const auto path=known_folder(flags&0xff);
        std::uint32_t error=0;
        if(!output || (flags&~0xc8ffU) || (!special && (arg(2) || arg(3)>1)))error=87;
        else if(path.empty())error=50;
        else {
            memory.store(GuestAddress{output},wide?16:8,0);
            if(create){
                for(std::size_t at=3;at<=path.size();++at)if(at==path.size() || path[at]==u'\\'){
                    const auto prefix=utf16_to_utf8(path.substr(0,at));const auto info=filesystem_.information(prefix);
                    if(info.error){error=filesystem_.create_directory(prefix);if(error && error!=183)break;error=0;}
                    else if(!(info.metadata.attributes&16)){error=183;break;}
                }
            }else if((special || arg(3)==0) && !(flags&0x4000)){
                const auto info=filesystem_.information(utf16_to_utf8(path));error=info.error;if(!error && !(info.metadata.attributes&16))error=3;
            }
            if(!error)write_text(memory,output,path,wide);
        }
        if(error)last_error_=error;result=special?(error?0:1):(error==50?not_implemented:hresult(error));
    }else if(symbol.starts_with("SHBrowseForFolder")){
        const auto p=arg(0);if(!p){last_error_=87;arg.return_to(state,0);return std::nullopt;}
        memory.check(GuestAddress{p},64,Access::Read);const auto load=[&](unsigned offset,unsigned width=64){return memory.load(GuestAddress{p+offset},static_cast<std::uint8_t>(width));};
        GuiShellBrowse data;data.request=p;data.wide=wide;data.callback=load(40);data.callback_data=load(48);data.flags=static_cast<std::uint32_t>(load(32,32));data.root=u"C:\\";data.directory=data.root;
        if(load(8)){const auto root=gui_.shell_pidls.find(load(8));if(root==gui_.shell_pidls.end() || !allocations_.contains(root->first)){last_error_=87;arg.return_to(state,0);return std::nullopt;}data.root=data.directory=root->second;}
        const auto root_info=filesystem_.information(utf16_to_utf8(data.root));
        if(root_info.error || !(root_info.metadata.attributes&16)){last_error_=root_info.error?root_info.error:267;arg.return_to(state,0);return std::nullopt;}
        // 虛擬 Shell 目前提供檔案系統資料夾；印表機、電腦、URL與檔案選擇不可偽裝成資料夾。
        if(data.flags&~0x75dU){last_error_=50;arg.return_to(state,0);return std::nullopt;}
        if(load(0) && !gui_.windows.contains(load(0))){last_error_=1400;arg.return_to(state,0);return std::nullopt;}
        if(load(16))memory.check(GuestAddress{load(16)},wide?520:260,Access::Write);memory.check(GuestAddress{p+56},4,Access::Write);
        if(data.callback && !handles(GuestAddress{data.callback}))memory.check(GuestAddress{data.callback},1,Access::Execute);
        if(!native_gui_){arg.return_to(state,0);return std::nullopt;}
        GuiWindow window;window.dpi_context=effective_dpi_context();window.thread=threads_.current;window.title=read(load(24));if(window.title.empty())window.title=u"選擇資料夾";
        window.class_name=u"#32770";window.builtin=true;window.dialog=true;window.modal=true;window.owner=load(0);window.width=620;window.height=420;
        const auto handle=allocate_gui_window();gui_.windows[handle]=window;gui_.shell_browsers[handle]=data;native_gui_->create(handle,window);
        const auto add=[&](std::uint64_t id,std::u16string name,std::u16string text,int x,int y,unsigned width,unsigned height,std::uint32_t style=0){GuiWindow w;w.dpi_context=effective_dpi_context();w.thread=threads_.current;w.parent=handle;w.id=id;w.class_name=std::move(name);w.title=std::move(text);w.x=x;w.y=y;w.width=width;w.height=height;w.style=style;w.visible=true;w.builtin=true;const auto h=allocate_gui_window();gui_.windows[h]=w;native_gui_->create(h,w);};
        add(410,u"edit",data.directory,20,20,470,26);add(414,u"button",u"前往",510,20,90,28);
        add(417,u"button",u"上一層",20,56,100,28);add(416,u"button",u"根目錄",130,56,100,28);
        add(412,u"listbox",{},20,95,580,230,1);add(499,u"static",{},20,333,580,30);
        add(1,u"button",u"選擇此資料夾",345,377,155,28,1);add(2,u"button",u"取消",510,377,90,28);
        shell_browse_refresh(handle);std::vector<GuestCallback> callbacks;
        if(data.callback){callbacks.push_back({data.callback,{handle,1,0,data.callback_data}});if(std::none_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& item){return item.window==handle && item.message==browse_selection_changed;}))enqueue_gui_message({handle,0,0,browse_selection_changed});}
        start_callbacks(state,memory,std::move(callbacks),Continuation::GuiModalInit,handle);return std::nullopt;
    }else throw HostCallFault("未實作的 shell service");
    arg.return_to(state,result);return std::nullopt;
}
bool WindowsServices::shell_dialog_message(GuestState& state,GuestMemory& memory,const GuiMessage& message){
    const auto found=gui_.shell_browsers.find(message.window);if(found==gui_.shell_browsers.end() || (message.message!=0x111 && message.message!=0x10 && message.message!=browse_selection_changed))return false;
    auto& data=found->second;auto& window=gui_.windows.at(message.window);const auto id=message.message==0x10?2U:static_cast<unsigned>(message.wparam&0xffff),notice=static_cast<unsigned>(message.wparam>>16);
    std::vector<GuestCallback> callbacks;
    const auto status=[&](std::u16string text){data.status=std::move(text);gui_.windows.at(gui_child(message.window,499)).title=data.status;present_gui(gui_child(message.window,499));};
    auto path=data.directory;bool changed=message.message==browse_selection_changed;
    if(id==2){window.ended=true;window.dialog_result=0;}
    else if(id==410 && notice==0x300){} // 編輯路徑須按「前往」或「選擇」。
    else if(id==414 || id==1 || id==417 || id==416 || (id==412 && (notice==1 || notice==2))){
        if(id==414 || id==1)path=normalized(gui_.windows.at(gui_child(message.window,410)).title);
        else if(id==417)path=parent_path(data.directory);
        else if(id==416)path=data.root;
        else {const auto& list=gui_.windows.at(gui_child(message.window,412));if(list.selection>=0 && static_cast<std::size_t>(list.selection)<list.items.size()){const auto name=list.items[static_cast<std::size_t>(list.selection)].text;path=name==u".."?parent_path(data.directory):data.directory+(data.directory.back()==u'\\'?u"":u"\\")+name;}else path.clear();}
        const auto info=path.empty()?FileInformation{{},{},123}:filesystem_.information(utf16_to_utf8(path));
        if(path.empty() || !within(path,data.root) || path.size()>=260 || (!data.wide && utf16_to_utf8(path).size()>=260) || info.error || !(info.metadata.attributes&16))status(u"請選擇根目錄內存在的資料夾。 ");
        else if(id==1 && gui_.windows.at(gui_child(message.window,1)).enabled){
            const auto pidl=shell_make_pidl(memory,path);if(!pidl)status(u"無法配置選取結果。 ");
            else {const auto display=memory.load(GuestAddress{data.request+16},64);if(display)write_text(memory,display,leaf_name(path),data.wide);memory.store(GuestAddress{data.request+56},32,0);window.ended=true;window.dialog_result=pidl;}
        }else {
            data.directory=path;data.status.clear();shell_browse_refresh(message.window);changed=true;
        }
    }
    if(changed && data.callback){
        if(data.selection_pidl){(void)free_heap(memory,data.selection_pidl,state);gui_.shell_pidls.erase(data.selection_pidl);}
        data.selection_pidl=shell_make_pidl(memory,data.directory);
        if(data.selection_pidl)callbacks.push_back({data.callback,{message.window,2,data.selection_pidl,data.callback_data}});
    }
    if(window.ended && data.selection_pidl){(void)free_heap(memory,data.selection_pidl,state);gui_.shell_pidls.erase(data.selection_pidl);data.selection_pidl=0;}
    start_callbacks(state,memory,std::move(callbacks),Continuation::GuiModalDispatch,message.window);return true;
}
}
