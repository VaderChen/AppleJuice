#include "windows_input_layout.hpp"
#include "windows_display_identity.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_keyboard.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <functional>
#include <variant>

namespace x64arm {
std::span<const WindowsServiceDescriptor> resource_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","FreeResource",1},
        {"kernel32.dll","EnumResourceTypesA",3},{"kernel32.dll","EnumResourceTypesW",3},
        {"kernel32.dll","EnumResourceNamesA",4},{"kernel32.dll","EnumResourceNamesW",4},
        {"kernel32.dll","EnumResourceLanguagesA",5},{"kernel32.dll","EnumResourceLanguagesW",5},
        {"user32.dll","LoadStringA",4},{"user32.dll","LoadStringW",4},
        {"user32.dll","LoadBitmapA",2},{"user32.dll","LoadBitmapW",2},
        {"user32.dll","SetThreadDpiAwarenessContext",1},{"user32.dll","GetThreadDpiAwarenessContext",0},
        {"user32.dll","SetProcessDpiAwarenessContext",1},{"user32.dll","IsValidDpiAwarenessContext",1},
        {"user32.dll","AreDpiAwarenessContextsEqual",2},{"user32.dll","GetAwarenessFromDpiAwarenessContext",1},
        {"user32.dll","GetWindowDpiAwarenessContext",1},{"user32.dll","GetDpiFromDpiAwarenessContext",1},
        {"user32.dll","EnableNonClientDpiScaling",1},{"user32.dll","GetSystemMetricsForDpi",2},
        {"user32.dll","GetDpiForWindow",1},{"user32.dll","GetDpiForSystem",0},
        {"shcore.dll","SetProcessDpiAwareness",1},{"shcore.dll","GetProcessDpiAwareness",2},{"shcore.dll","GetDpiForMonitor",4},
        {"user32.dll","SetProcessDPIAware",0},{"user32.dll","IsProcessDPIAware",0},{"user32.dll","GetDialogBaseUnits",0},
        {"user32.dll","CallWindowProcA",5},{"user32.dll","CallWindowProcW",5},{"user32.dll","GetKeyState",1},
        {"user32.dll","MapVirtualKeyA",2},{"user32.dll","MapVirtualKeyW",2},{"user32.dll","MapVirtualKeyExA",3},{"user32.dll","MapVirtualKeyExW",3},
        {"user32.dll","GetClassInfoA",3},{"user32.dll","GetClassInfoW",3},{"user32.dll","GetClassInfoExA",3},{"user32.dll","GetClassInfoExW",3},
        {"user32.dll","GetWindowPlacement",2},{"user32.dll","SetWindowPlacement",2},
        {"user32.dll","WindowFromPoint",1,1},{"user32.dll","ChildWindowFromPoint",2,2},{"user32.dll","ChildWindowFromPointEx",3,2},{"user32.dll","RealChildWindowFromPoint",2,2},
        {"user32.dll","LoadMenuA",2},{"user32.dll","LoadMenuW",2},
        {"user32.dll","LoadMenuIndirectA",1},{"user32.dll","LoadMenuIndirectW",1},
        {"user32.dll","LoadAcceleratorsA",2},{"user32.dll","LoadAcceleratorsW",2},
        {"user32.dll","CreateAcceleratorTableA",2},{"user32.dll","CreateAcceleratorTableW",2},
        {"user32.dll","CopyAcceleratorTableA",3},{"user32.dll","CopyAcceleratorTableW",3},
        {"user32.dll","DestroyAcceleratorTable",1},{"user32.dll","TranslateAcceleratorA",3},{"user32.dll","TranslateAcceleratorW",3},
        {"user32.dll","GetMenu",1},{"user32.dll","SetMenu",2},{"user32.dll","DrawMenuBar",1},
        {"user32.dll","MonitorFromWindow",2},{"user32.dll","MonitorFromPoint",2,1},{"user32.dll","MonitorFromRect",2},
        {"user32.dll","ChangeWindowMessageFilter",2},{"user32.dll","ChangeWindowMessageFilterEx",4},{"user32.dll","RegisterDeviceNotificationA",3},{"user32.dll","RegisterDeviceNotificationW",3},{"user32.dll","UnregisterDeviceNotification",1},{"user32.dll","EnumDisplayDevicesA",4},{"user32.dll","EnumDisplayDevicesW",4},{"user32.dll","ChangeDisplaySettingsA",2},{"user32.dll","ChangeDisplaySettingsW",2},{"user32.dll","ChangeDisplaySettingsExA",5},{"user32.dll","ChangeDisplaySettingsExW",5},{"user32.dll","EnumDisplaySettingsA",3},{"user32.dll","EnumDisplaySettingsW",3},{"user32.dll","EnumDisplaySettingsExA",4},{"user32.dll","EnumDisplaySettingsExW",4},{"user32.dll","EnumDisplayMonitors",4},{"user32.dll","GetMonitorInfoA",2},{"user32.dll","GetMonitorInfoW",2},
        {"user32.dll","GetDisplayConfigBufferSizes",3},{"user32.dll","QueryDisplayConfig",6},{"user32.dll","DisplayConfigGetDeviceInfo",1},{"user32.dll","DisplayConfigSetDeviceInfo",1},
        {"user32.dll","SystemParametersInfoA",4},{"user32.dll","SystemParametersInfoW",4}
    };return table;
}
namespace {
// 與現有 GDI 單一虛擬桌面一致，並非宿主螢幕 EDID／硬體時序。
constexpr std::uint64_t display_adapter_luid=UINT64_C(0x58464d0000000001);
constexpr unsigned display_width=1440,display_height=900,display_refresh=60;
void display_put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint64_t value,unsigned count=4){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));}
void display_signal(std::vector<std::uint8_t>& bytes,std::size_t at){
    display_put(bytes,at,std::uint64_t{display_width}*display_height*display_refresh,8);
    display_put(bytes,at+8,display_height*display_refresh);display_put(bytes,at+12,1);
    display_put(bytes,at+16,display_refresh);display_put(bytes,at+20,1);
    display_put(bytes,at+24,display_width);display_put(bytes,at+28,display_height);
    display_put(bytes,at+32,display_width);display_put(bytes,at+36,display_height);display_put(bytes,at+44,1);
}
std::uint32_t display_config_call(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    if(symbol=="DisplayConfigSetDeviceInfo"){
        // 虛擬顯示器只有一個 SDR 目標：可接受開機持續性與「關閉」類請求；需要 HDR／WCG／虛擬解析度的請求回報不支援。
        if(!arg(0))return 87;
        const auto type=memory.load(GuestAddress{arg(0)},32),size=memory.load(GuestAddress{arg(0)+4},32);
        if(size<20)return 87;
        const auto adapter=memory.load(GuestAddress{arg(0)+8},64),id=memory.load(GuestAddress{arg(0)+16},32);
        if(adapter!=display_adapter_luid || id)return 87;
        if(type!=5 && type!=8 && type!=10 && type!=13 && type!=16 && type!=17)return 87;
        if(size<24)return 87;
        const auto value=memory.load(GuestAddress{arg(0)+20},32);
        if(type==5)return 0;                   // SET_TARGET_PERSISTENCE：單一內建目標永遠存在。
        if(type==8)return (value&1U)?0U:50U;   // SET_SUPPORT_VIRTUAL_RESOLUTION：只提供停用狀態。
        if(type==13)return 50;                 // SET_MONITOR_SPECIALIZATION。
        return (value&1U)?50U:0U;              // SET_ADVANCED_COLOR_STATE／HDR_STATE／WCG_STATE。
    }
    if(symbol=="DisplayConfigGetDeviceInfo"){
        if(!arg(0))return 87;
        const auto type=memory.load(GuestAddress{arg(0)},32),size=memory.load(GuestAddress{arg(0)+4},32);
        if(size<20)return 87;
        const auto adapter=memory.load(GuestAddress{arg(0)+8},64),id=memory.load(GuestAddress{arg(0)+16},32);
        if(adapter!=display_adapter_luid || id)return 87;
        const unsigned required=type==1?84:type==2?420:type==3?80:type==6 || type==7?24:type==9?32:0;
        if(!required)return 50; // 尚無 PnP 裝置路徑、HDR／SDR 白點或 monitor specialization。
        if(size<required)return 122;
        memory.check(GuestAddress{arg(0)},required,Access::Write);std::vector<std::uint8_t> bytes(required);
        memory.read(GuestAddress{arg(0)},std::span(bytes).first(20));
        if(type==1){const auto name=utf16_bytes(u"\\\\.\\DISPLAY1");std::copy(name.begin(),name.end(),bytes.begin()+20);}
        else if(type==2){
            // 沒有 EDID／可開啟的 Windows PnP 路徑；名稱由虛擬顯示器指定。
            display_put(bytes,20,2);display_put(bytes,24,UINT32_MAX);
            const auto name=utf16_bytes(u"x64arm Virtual Monitor");std::copy(name.begin(),name.end(),bytes.begin()+36);
        }else if(type==3){display_put(bytes,20,display_width);display_put(bytes,24,display_height);display_signal(bytes,32);}
        else if(type==6)display_put(bytes,20,UINT32_MAX);
        else if(type==7)display_put(bytes,20,1); // 不提供額外虛擬解析度。
        else if(type==9){display_put(bytes,24,0);display_put(bytes,28,8);} // SDR、RGB、每色 8 bit。
        memory.write(GuestAddress{arg(0)},bytes);return 0;
    }
    const auto flags=static_cast<std::uint32_t>(arg(0)),base=flags&7U;
    if((base!=1 && base!=2 && base!=4) || (flags&~0x77U) || ((flags&0x20U) && base!=2))return 87;
    if(symbol=="GetDisplayConfigBufferSizes"){
        if(!arg(1) || !arg(2))return 87;
        memory.check(GuestAddress{arg(1)},4,Access::Write);memory.check(GuestAddress{arg(2)},4,Access::Write);
        memory.store(GuestAddress{arg(1)},32,1);memory.store(GuestAddress{arg(2)},32,2);return 0;
    }
    if(!arg(1) || !arg(2) || !arg(3) || !arg(4) || (base==4)!=bool(arg(5)))return 87;
    const auto paths=memory.load(GuestAddress{arg(1)},32),modes=memory.load(GuestAddress{arg(3)},32);
    if(paths<1 || modes<2)return 122;
    memory.check(GuestAddress{arg(1)},4,Access::Write);memory.check(GuestAddress{arg(3)},4,Access::Write);
    memory.check(GuestAddress{arg(2)},72,Access::Write);memory.check(GuestAddress{arg(4)},128,Access::Write);
    if(arg(5))memory.check(GuestAddress{arg(5)},4,Access::Write);
    std::vector<std::uint8_t> path(72),mode(128);
    display_put(path,0,display_adapter_luid,8);display_put(path,16,1); // source 0、mode 0、IN_USE。
    display_put(path,20,display_adapter_luid,8);display_put(path,32,1);display_put(path,36,UINT32_MAX);
    display_put(path,40,1);display_put(path,44,1);display_put(path,48,display_refresh);display_put(path,52,1);
    display_put(path,56,1);display_put(path,60,1);display_put(path,64,1);display_put(path,68,1);
    display_put(mode,0,1);display_put(mode,8,display_adapter_luid,8);display_put(mode,16,display_width);display_put(mode,20,display_height);display_put(mode,24,4);
    display_put(mode,64,2);display_put(mode,72,display_adapter_luid,8);display_signal(mode,80);
    memory.write(GuestAddress{arg(2)},path);memory.write(GuestAddress{arg(4)},mode);
    memory.store(GuestAddress{arg(1)},32,1);memory.store(GuestAddress{arg(3)},32,2);if(arg(5))memory.store(GuestAddress{arg(5)},32,1);return 0;
}
struct ResourceReader {
    const GuestMemory& memory;std::uint64_t begin,end,at;
    std::uint64_t number(unsigned bytes){
        if(at<begin || at>end || bytes>end-at)throw HostCallFault("GUI 資源模板超出範圍");
        const auto value=memory.load(GuestAddress{at},static_cast<std::uint8_t>(bytes*8));at+=bytes;return value;
    }
    void align(unsigned bytes){at=(at+bytes-1)&~std::uint64_t{bytes-1};if(at>end)throw HostCallFault("GUI 資源模板對齊超出範圍");}
    std::u16string string(){std::u16string text;for(unsigned i=0;i<32768;++i){const auto c=number(2);if(!c)return text;text+=static_cast<char16_t>(c);}throw HostCallFault("GUI 資源字串超過上限");}
};
struct ResourceDirectoryError {std::uint32_t code;};
using ResourceKey=std::variant<std::uint16_t,std::u16string>;
ResourceKey resource_key(const GuestMemory& memory,std::uint64_t pointer,bool wide){
    if(pointer<=UINT16_MAX)return static_cast<std::uint16_t>(pointer);
    std::u16string text;
    if(wide)text=read_utf16(memory,GuestAddress{pointer});
    else {
        std::string bytes;
        for(std::size_t i=0;;++i){const auto value=memory.load(GuestAddress{pointer+i},8);if(!value)break;bytes.push_back(static_cast<char>(value));}
        text=utf8_to_utf16_lossy(bytes);
    }
    if(text.size()>1 && text.front()==u'#'){
        unsigned number=0;
        for(std::size_t i=1;i<text.size();++i){
            if(text[i]<u'0' || text[i]>u'9' || number>(UINT16_MAX-(text[i]-u'0'))/10U)throw ResourceDirectoryError{87};
            number=number*10U+static_cast<unsigned>(text[i]-u'0');
        }
        return static_cast<std::uint16_t>(number);
    }
    return text;
}
bool resource_key_equal(const ResourceKey& left,const ResourceKey& right){
    if(left.index()!=right.index())return false;
    if(const auto* number=std::get_if<std::uint16_t>(&left))return *number==std::get<std::uint16_t>(right);
    const auto& a=std::get<std::u16string>(left);const auto& b=std::get<std::u16string>(right);
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)if(unicode_case_unit(a[i],true)!=unicode_case_unit(b[i],true))return false;
    return true;
}
// 列舉直接讀取映射後的 PE 資源樹；名稱與 ID 共用表示法，回呼使用獨立可寫副本。
struct ResourceDirectory {
    struct Entry {ResourceKey key;std::uint32_t offset;bool directory;};
    const GuestMemory& memory;const RuntimeImage& image;std::uint64_t root{};std::uint32_t extent{};
    ResourceDirectory(const GuestMemory& source,const RuntimeImage& module):memory(source),image(module){
        const auto header=[&](std::uint64_t offset,unsigned bytes){
            if(offset>image.size || bytes>image.size-offset)throw ResourceDirectoryError{193};
            return memory.load(GuestAddress{image.base+offset},static_cast<std::uint8_t>(bytes*8));
        };
        if(header(0,2)!=0x5a4d)throw ResourceDirectoryError{193};
        const auto nt=header(60,4);if(header(nt,4)!=0x4550)throw ResourceDirectoryError{193};
        const auto optional=nt+24,magic=header(optional,2);
        if(magic!=0x10b && magic!=0x20b)throw ResourceDirectoryError{193};
        const unsigned directory=magic==0x10b?112:128;
        if(header(optional+(magic==0x10b?92U:108U),4)<3)throw ResourceDirectoryError{1812};
        const auto rva=header(optional+directory,4);extent=static_cast<std::uint32_t>(header(optional+directory+4,4));
        if(!rva || !extent)throw ResourceDirectoryError{1812};
        if(rva>image.size || extent>image.size-rva || extent<16)throw ResourceDirectoryError{13};
        root=image.base+rva;check(0,16);
    }
    void check(std::uint64_t offset,std::uint64_t size)const{
        if(offset>extent || size>extent-offset)throw ResourceDirectoryError{13};
        memory.check(GuestAddress{root+offset},static_cast<std::size_t>(size),Access::Read);
    }
    std::uint64_t number(std::uint64_t offset,unsigned bytes)const{
        check(offset,bytes);return memory.load(GuestAddress{root+offset},static_cast<std::uint8_t>(bytes*8));
    }
    std::vector<Entry> entries(std::uint32_t offset,bool children)const{
        check(offset,16);const auto named=number(offset+12,2),total=named+number(offset+14,2);
        check(std::uint64_t{offset}+16,total*8);std::vector<Entry> items;items.reserve(static_cast<std::size_t>(total));
        for(std::uint64_t i=0;i<total;++i){
            const auto at=std::uint64_t{offset}+16+i*8,key=number(at,4),next=number(at+4,4);
            if(bool(key&0x80000000U)!=(i<named) || bool(next&0x80000000U)!=children)throw ResourceDirectoryError{13};
            ResourceKey value;
            if(key&0x80000000U){
                const auto string_offset=key&0x7fffffffU,count=number(string_offset,2);check(string_offset+2,count*2);
                std::u16string text;text.reserve(static_cast<std::size_t>(count));
                for(std::uint64_t character=0;character<count;++character)text.push_back(static_cast<char16_t>(number(string_offset+2+character*2,2)));
                value=std::move(text);
            }else {if(key>UINT16_MAX)throw ResourceDirectoryError{13};value=static_cast<std::uint16_t>(key);}
            const auto child=static_cast<std::uint32_t>(next&0x7fffffffU);check(child,16);
            if(!children){
                if(std::holds_alternative<std::u16string>(value))throw ResourceDirectoryError{13};
                const auto data=number(child,4),length=number(child+4,4);
                if(data>image.size || length>image.size-data)throw ResourceDirectoryError{13};
            }
            items.push_back({std::move(value),child,children});
        }
        return items;
    }
    std::uint32_t lookup(std::uint32_t offset,const ResourceKey& key,std::uint32_t missing)const{
        const auto items=entries(offset,true);
        for(const auto& item:items)if(resource_key_equal(item.key,key))return item.offset;
        throw ResourceDirectoryError{missing};
    }
};
}
std::uint64_t WindowsServices::load_gui_menu(GuestMemory& memory,std::uint64_t begin,std::uint64_t size){
    if(begin>UINT64_MAX-size)throw HostCallFault("MENU 模板範圍溢位");
    ResourceReader reader{memory,begin,begin+size,begin};
    std::vector<std::uint64_t> created;std::size_t items=0;
    try{
        const auto version=reader.number(2),offset=reader.number(2);if(version>1)throw HostCallFault("MENU 模板版本尚未支援");
        reader.at+=offset;if(reader.at>reader.end)throw HostCallFault("MENU 模板標頭越界");
        std::function<std::uint64_t(unsigned)> parse=[&](unsigned depth)->std::uint64_t{
            if(depth>=64 || gui_.menus.size()>=8192)throw HostCallFault("MENU 模板階層過深");
            const auto handle=gui_.next_object++;created.push_back(handle);auto& entries=gui_.menus[handle];
            for(;;){if(++items>65536)throw HostCallFault("MENU 模板項目過多");
                std::uint32_t flags;std::uint64_t id;bool popup,end;
                if(version){reader.align(4);flags=static_cast<std::uint32_t>(reader.number(4));flags|=static_cast<std::uint32_t>(reader.number(4));id=reader.number(4);const auto bits=reader.number(2);popup=(bits&1)!=0;end=(bits&128)!=0;}
                else {const auto bits=reader.number(2);flags=static_cast<std::uint32_t>(bits&~UINT64_C(128));popup=(bits&16)!=0;end=(bits&128)!=0;id=popup?0:reader.number(2);}
                const auto command=version?static_cast<std::uint32_t>(id):UINT32_MAX;auto text=reader.string();if(popup){if(version){reader.align(4);(void)reader.number(4);}id=parse(depth+1);flags|=16;}
                if(!popup && text.empty() && !id)flags|=0x800;
                GuiMenuItem item{id,flags,std::move(text)};item.command=command;entries.push_back(std::move(item));if(end)break;
            }return handle;
        };return parse(0);
    }catch(...){for(const auto handle:created)gui_.menus.erase(handle);throw;}
}
std::uint64_t WindowsServices::effective_dpi_context() const {
    const auto thread=threads_.items.find(threads_.current);
    return thread!=threads_.items.end() && thread->second.dpi_context?thread->second.dpi_context:gui_.dpi_context;
}
std::optional<std::uint32_t> WindowsServices::invoke_resource(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const bool wide=symbol.ends_with('W');
    // 現代 Win32 的資源與模組同壽命；官方規格明訂此過時 API 永遠回傳 FALSE。
    if(symbol=="FreeResource"){arg.return_to(state,0);return std::nullopt;}
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    std::uint64_t result=0;const auto fail=[&](std::uint32_t code){last_error_=code;result=0;};
    const auto resource=[&](std::uint64_t name,std::uint64_t type)->std::optional<ResourceReader>{
        const auto entry=gui_resource(memory,arg(0),name,type,wide);if(!entry){last_error_=1814;return {};}
        const auto begin=resource_image(entry,true)->base+memory.load(GuestAddress{entry},32),length=memory.load(GuestAddress{entry+4},32);
        return ResourceReader{memory,begin,begin+length,begin};
    };
    const auto copy_text=[&](std::uint64_t pointer,std::uint32_t capacity,std::u16string_view text){
        if(!capacity)return UINT64_C(0);
        if(wide){const auto count=std::min<std::size_t>(capacity-1,text.size());memory.write(GuestAddress{pointer},utf16_bytes(text.substr(0,count)));return static_cast<std::uint64_t>(count);}
        const auto ansi=utf16_to_utf8(text);const auto count=std::min<std::size_t>(capacity-1,ansi.size());std::vector<std::uint8_t> bytes(ansi.begin(),ansi.begin()+static_cast<std::ptrdiff_t>(count));bytes.push_back(0);memory.write(GuestAddress{pointer},bytes);return static_cast<std::uint64_t>(count);
    };
    if(symbol.starts_with("EnumResource")){
        const bool types=symbol.starts_with("EnumResourceTypes"),names=symbol.starts_with("EnumResourceNames");
        const unsigned function=types?1:names?2:3;
        const auto* image=resource_image(arg(0)?arg(0):runtime_.image.base);
        if(!image)fail(126);
        else if(!arg(function) || (!types && !arg(1)) || (!types && !names && !arg(2)))fail(87);
        else try{
            const ResourceDirectory tree(memory,*image);
            const auto type=types?ResourceKey{std::uint16_t{0}}:resource_key(memory,arg(1),wide);
            const auto name=types || names?ResourceKey{std::uint16_t{0}}:resource_key(memory,arg(2),wide);
            const auto type_directory=types?0:tree.lookup(0,type,1813);
            const auto target=types || names?type_directory:tree.lookup(type_directory,name,1814);
            const auto entries=tree.entries(target,types || names);
            if(entries.empty())throw ResourceDirectoryError{types?1812U:names?1813U:1815U};
            std::vector<std::uint8_t> strings;
            const auto encode=[&](const ResourceKey& key)->std::pair<std::uint64_t,bool>{
                if(const auto* id=std::get_if<std::uint16_t>(&key))return {*id,false};
                const auto at=strings.size();const auto& text=std::get<std::u16string>(key);
                if(wide){const auto bytes=utf16_bytes(text);strings.insert(strings.end(),bytes.begin(),bytes.end());}
                else {const auto bytes=utf16_to_utf8(text);strings.insert(strings.end(),bytes.begin(),bytes.end());strings.push_back(0);}
                return {at,true};
            };
            const auto encoded_type=types?std::pair<std::uint64_t,bool>{0,false}:encode(type);
            const auto encoded_name=types || names?std::pair<std::uint64_t,bool>{0,false}:encode(name);
            std::vector<std::pair<std::uint64_t,bool>> keys;keys.reserve(entries.size());
            for(const auto& entry:entries)keys.push_back(encode(entry.key));
            const auto scratch=strings.empty()?0:allocate_heap(memory,strings.size());
            if(!strings.empty() && !scratch)throw ResourceDirectoryError{8};
            try{
                if(scratch)memory.write(GuestAddress{scratch},strings);
                const auto pointer=[&](const auto& item){return item.second?scratch+item.first:item.first;};
                std::vector<GuestCallback> callbacks;callbacks.reserve(entries.size());
                for(const auto& key:keys){
                    std::vector<std::uint64_t> parameters{arg(0)};
                    if(!types)parameters.push_back(pointer(encoded_type));
                    if(!types && !names)parameters.push_back(pointer(encoded_name));
                    parameters.push_back(pointer(key));parameters.push_back(arg(function+1));
                    callbacks.push_back({arg(function),std::move(parameters)});
                }
                start_callbacks(state,memory,std::move(callbacks),Continuation::ResourceEnumeration,scratch);return std::nullopt;
            }catch(...){if(scratch)(void)free_heap(memory,scratch,state);throw;}
        }catch(const ResourceDirectoryError& error){fail(error.code);}catch(const MemoryFault&){fail(998);}
    }else if(symbol.starts_with("CallWindowProc")){
        if(!arg(0))fail(87);else {
            auto procedure=arg(0);bool encoding=wide;const auto reference=gui_procedure_references_.find(procedure);
            if(reference!=gui_procedure_references_.end()){procedure=reference->second.procedure;encoding=reference->second.wide;}
            else if(const auto window=gui_.windows.find(arg(1));window!=gui_.windows.end()){
                if(window->second.procedure==procedure)encoding=window->second.unicode;
                else if(const auto stock=stock_gui_class(procedure);!stock.empty())encoding=procedure==stock_gui_proc(stock,true);
                else for(const auto& [name,record]:gui_.classes){(void)name;if(record.atom==window->second.class_atom && record.procedure==procedure){encoding=record.unicode;break;}}
            }
            if(begin_gui_text_thunk(state,memory,{arg(1),arg(3),arg(4),static_cast<std::uint32_t>(arg(2))},wide,Continuation::ReturnCallback,0,procedure,encoding))return std::nullopt;
            start_callbacks(state,memory,{{procedure,{arg(1),arg(2),arg(3),arg(4)}}},Continuation::ReturnCallback);return std::nullopt;
        }
    }else if(symbol.starts_with("MapVirtualKey")){
        if(symbol.starts_with("MapVirtualKeyEx") && !windows_input_layout_supported(arg(2)))fail(87);else result=windows_map_virtual_key(d(0),d(1));
    }else if(symbol=="GetDpiForMonitor"){
        // 與 monitor／GDI／DPI context 共用同一個 96 DPI 虛擬桌面。
        result=0x80070057U;
        if(arg(0)==1 && d(1)<=2 && arg(2) && arg(3))try{memory.check(GuestAddress{arg(2)},4,Access::Write);memory.check(GuestAddress{arg(3)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,96);memory.store(GuestAddress{arg(3)},32,96);result=0;}catch(const MemoryFault&){}
    }else if(symbol=="SetThreadDpiAwarenessContext" || symbol=="GetThreadDpiAwarenessContext" || symbol=="SetProcessDpiAwarenessContext" || symbol=="SetProcessDPIAware" || symbol=="SetProcessDpiAwareness" || symbol=="GetProcessDpiAwareness" || symbol=="IsProcessDPIAware" || symbol=="IsValidDpiAwarenessContext" || symbol=="AreDpiAwarenessContextsEqual" || symbol=="GetAwarenessFromDpiAwarenessContext" || symbol=="GetWindowDpiAwarenessContext" || symbol=="GetDpiFromDpiAwarenessContext" || symbol=="GetDpiForWindow" || symbol=="GetDpiForSystem" || symbol=="GetSystemMetricsForDpi" || symbol=="EnableNonClientDpiScaling"){
        const auto canonical=[&](std::uint64_t value){return x86_?static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value))):value;};
        const auto valid=[](std::uint64_t context){return context>=UINT64_MAX-4;};
        const auto awareness=[](std::uint64_t context){return context==UINT64_MAX || context==UINT64_MAX-4?0U:context==UINT64_MAX-1?1U:2U;};
        const auto context=canonical(service.arguments?arg(0):0);
        if(symbol=="IsValidDpiAwarenessContext")result=valid(context);
        else if(symbol=="AreDpiAwarenessContextsEqual")result=valid(context) && valid(canonical(arg(1))) && context==canonical(arg(1));
        else if(symbol=="GetAwarenessFromDpiAwarenessContext")result=valid(context)?awareness(context):UINT32_MAX;
        else if(symbol=="GetDpiFromDpiAwarenessContext")result=valid(context) && awareness(context)!=2?96:0;
        else if(symbol=="GetThreadDpiAwarenessContext")result=effective_dpi_context();
        else if(symbol=="SetThreadDpiAwarenessContext"){if(!valid(context))fail(87);else{result=effective_dpi_context();threads_.items.at(threads_.current).dpi_context=context;}}
        else if(symbol=="GetWindowDpiAwarenessContext" || symbol=="GetDpiForWindow"){const auto window=gui_.windows.find(arg(0));if(window==gui_.windows.end())fail(1400);else result=symbol=="GetDpiForWindow"?96:window->second.dpi_context;}
        else if(symbol=="GetDpiForSystem")result=96;
        else if(symbol=="GetSystemMetricsForDpi"){if(!d(1))fail(87);else result=system_metric(d(0),d(1));}
        else if(symbol=="EnableNonClientDpiScaling"){
            const auto w=gui_.windows.find(arg(0));
            bool nccreate=false;
            if(!runtime_.callbacks.empty()){
                const auto& frame=runtime_.callbacks.back();
                if(frame.index<frame.callbacks.size()){
                    const auto& params=frame.callbacks[frame.index].arguments;
                    nccreate=params.size()>=2 && params[0]==arg(0) && params[1]==0x81;
                }
            }
            if(w==gui_.windows.end())fail(1400);
            else if(w->second.thread!=threads_.current || (w->second.style&0x40000000U) || !nccreate ||
                    (w->second.dpi_context!=UINT64_MAX-2 && w->second.dpi_context!=UINT64_MAX-3))fail(87);
            else {w->second.nonclient_dpi_scaling=true;result=1;}
        }
        else if(symbol=="IsProcessDPIAware")result=awareness(gui_.dpi_context)!=0;
        else if(symbol=="GetProcessDpiAwareness"){
            if(!arg(1) || (arg(0) && !current_process_handle(arg(0))))result=0x80070057U;
            else{memory.store(GuestAddress{arg(1)},32,awareness(gui_.dpi_context));result=0;}
        }else {
            const bool hresult=symbol=="SetProcessDpiAwareness";const auto value=symbol=="SetProcessDPIAware"?UINT64_MAX-1:hresult?UINT64_MAX-d(0):context;
            const auto error=!valid(value) || (hresult && d(0)>2)?87U:(gui_.dpi_context_set && !(symbol=="SetProcessDPIAware" && value==gui_.dpi_context)) || (!gui_.windows.empty() && symbol!="SetProcessDPIAware")?5U:0U;
            if(error){if(hresult)result=0x80070000U|error;else fail(error);}
            else{gui_.dpi_context=value;gui_.dpi_context_set=true;gui_.dpi_aware=awareness(value)!=0;result=hresult?0:1;}
        }
    }
    else if(symbol=="GetKeyState")result=d(0)<256?((gui_.keyboard[d(0)]&128)?0x8000U:0U)|(gui_.keyboard[d(0)]&1):0;
    else if(symbol.starts_with("GetClassInfo")){
        return invoke_gui_support(state,memory,service);
    }else if(symbol=="GetWindowPlacement" || symbol=="SetWindowPlacement"){
        const auto found=gui_.windows.find(arg(0));if(found==gui_.windows.end())fail(1400);else if(memory.load(GuestAddress{arg(1)},32)!=44)fail(87);
        else {auto& window=found->second;if(symbol=="GetWindowPlacement"){
                memory.write(GuestAddress{arg(1)},std::array<std::uint8_t,44>{});memory.store(GuestAddress{arg(1)},32,44);memory.store(GuestAddress{arg(1)+8},32,window.show_command);
                memory.store(GuestAddress{arg(1)+12},64,UINT64_MAX);memory.store(GuestAddress{arg(1)+20},64,UINT64_MAX);
                const auto r=(window.show_command==1?std::optional<std::array<std::int32_t,4>>{}:window.normal_rect).value_or(std::array<std::int32_t,4>{window.x,window.y,static_cast<std::int32_t>(window.width),static_cast<std::int32_t>(window.height)});const auto insets=gui_frame_insets(window);
                memory.store(GuestAddress{arg(1)+4},32,window.restore_maximized?2:0);
                memory.store(GuestAddress{arg(1)+28},32,static_cast<std::uint32_t>(r[0]));memory.store(GuestAddress{arg(1)+32},32,static_cast<std::uint32_t>(r[1]));
                memory.store(GuestAddress{arg(1)+36},32,static_cast<std::uint32_t>(r[0]+r[2])+insets.left+insets.right);memory.store(GuestAddress{arg(1)+40},32,static_cast<std::uint32_t>(r[1]+r[3])+insets.top+insets.bottom);result=1;
            }else {const auto x=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+28},32)),y=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+32},32));const auto right=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+36},32)),bottom=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+40},32));
                const auto width=static_cast<std::int64_t>(right)-x,height=static_cast<std::int64_t>(bottom)-y;
                const auto show=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+8},32));
                if(width<1 || width>8192 || height<1 || height>8192 || show>11)fail(87);
                else {const auto client=gui_client_size(static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),gui_frame_insets(window));window.x=x;window.y=y;window.width=client[0];window.height=client[1];window.normal_rect=std::array<std::int32_t,4>{x,y,static_cast<std::int32_t>(client[0]),static_cast<std::int32_t>(client[1])};window.show_command=1;window.restore_maximized=(memory.load(GuestAddress{arg(1)+4},32)&2)!=0;window.visible=show!=0;window.style=(window.style&~0x10000000U)|(window.visible?0x10000000U:0U);
                    start_callbacks(state,memory,{{resolve("user32.dll","ShowWindow").value,{arg(0),show}}},Continuation::Return,1);return std::nullopt;}
            }
        }
    }else if(symbol=="WindowFromPoint" || symbol=="ChildWindowFromPoint" || symbol=="ChildWindowFromPointEx" || symbol=="RealChildWindowFromPoint"){
        const bool child=symbol!="WindowFromPoint";const auto flags=symbol=="ChildWindowFromPointEx"?d(2):0;
        const auto point=arg(child?1:0);const auto x=static_cast<std::int32_t>(point),y=static_cast<std::int32_t>(point>>32);
        if(child && !gui_.windows.contains(arg(0)))fail(1400);
        else if(flags>7)fail(87);
        else if(!child)result=gui_window_from_point(gui_,x,y,gui_.input_desktop);
        else {
            const auto& parent=gui_.windows.at(arg(0));const bool inside=x>=0 && y>=0 && static_cast<std::uint32_t>(x)<parent.width && static_cast<std::uint32_t>(y)<parent.height;if(inside)result=arg(0);
            if(inside)for(const auto handle:gui_.z_order){const auto item=gui_.windows.find(handle);if(item==gui_.windows.end())continue;const auto& window=item->second;
                if(window.parent!=arg(0))continue;
                if((flags&1) && !window.visible)continue;if((flags&2) && !window.enabled)continue;if((flags&4) && (window.ex_style&32))continue;
                const std::int64_t left=window.x,top=window.y;
                const auto outer=gui_outer_size(window);if(x>=left && x<left+outer[0] && y>=top && y<top+outer[1]){
                    if(symbol=="RealChildWindowFromPoint"){
                        if(!window.visible)continue;
                        // BS_GROUPBOX 的內部是 HTTRANSPARENT；STATIC 仍回傳本身。
                        if(gui_control_class(window)==u"button" && (window.style&15U)==7U){
                            const auto metrics=fallback_font_metrics(gui_font(gui_,window.font));
                            if(x>left+2 && x<left+outer[0]-2 && y>top+metrics.height && y<top+outer[1]-2)continue;
                        }
                    }
                    result=handle;break;
                }
            }
        }
    }else if(symbol=="GetDialogBaseUnits")result=(16U<<16)|8U;
    else if(symbol.starts_with("LoadBitmap") || symbol.starts_with("LoadImage")){
        const bool image=symbol.starts_with("LoadImage");
        if(image && (d(2)!=0 || (d(5)&~0xa040U)))fail(50);
        else if(auto reader=resource(arg(1),2)){
            const auto size=reader->end-reader->begin;if(size>16U*1024U*1024U)throw HostCallFault("DIB 資源超過上限");
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.read(GuestAddress{reader->begin},bytes);
            GuiBitmap bitmap;
            if(native_gui_){
                const auto cached=bitmap_templates_.find(reader->begin);
                if(cached!=bitmap_templates_.end() && cached->second.first==bytes)bitmap=cached->second.second;
                else {bitmap=decode_dib_bitmap(bytes);bitmap.resource_id=next_bitmap_template_++;
                    std::size_t cache_bytes=0;for(const auto& [key,item]:bitmap_templates_){(void)key;cache_bytes+=item.first.size()+item.second.pixels.size()*4;}
                    if(bitmap_templates_.size()>=32 || cache_bytes+bytes.size()+bitmap.pixels.size()*4>64*1024*1024)bitmap_templates_.clear();
                    bitmap_templates_[reader->begin]={std::move(bytes),bitmap};
                }
            }else bitmap=decode_dib_bitmap(bytes);
            const auto width=image && d(3)?d(3):bitmap.width,height=image && d(4)?d(4):bitmap.height;
            if(!width || !height || width>4096 || height>4096 || static_cast<std::uint64_t>(width)*height>4*1024*1024)throw HostCallFault("DIB 輸出尺寸超過上限");
            if(width!=bitmap.width || height!=bitmap.height){GuiBitmap scaled{width,height,std::vector<std::uint32_t>(static_cast<std::size_t>(width)*height)};
                for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x)scaled.pixels[static_cast<std::size_t>(y)*width+x]=bitmap.pixels[static_cast<std::size_t>(static_cast<std::uint64_t>(y)*bitmap.height/height)*bitmap.width+static_cast<std::size_t>(static_cast<std::uint64_t>(x)*bitmap.width/width)];bitmap=std::move(scaled);
            }
            result=gui_.next_object++;gui_.bitmaps.emplace(result,std::move(bitmap));
        }
    }else if(symbol.starts_with("LoadString")){
        if(d(3)>INT32_MAX || (!wide && !d(3)))fail(87);
        else if(auto reader=resource((d(1)>>4)+1,6)){
            for(unsigned i=0;i<16;++i){const auto length=reader->number(2);if(length>(reader->end-reader->at)/2)throw HostCallFault("STRINGTABLE 長度超出資源");
                if(i==(d(1)&15)){if(wide && !d(3)){memory.store(GuestAddress{arg(2)},x86_?32:64,reader->at);result=length;}
                    else {std::u16string text;for(std::uint64_t n=0;n<length;++n)text+=static_cast<char16_t>(reader->number(2));result=copy_text(arg(2),d(3),text);}break;}
                reader->at+=length*2;
            }
        }else if(d(3))memory.store(GuestAddress{arg(2)},wide?16:8,0);
    }else if(symbol.starts_with("LoadMenu")){
        auto reader=symbol.starts_with("LoadMenuIndirect")?std::optional{ResourceReader{memory,arg(0),arg(0)+1024*1024,arg(0)}}:resource(arg(1),4);
        if(reader){
            result=load_gui_menu(memory,reader->begin,reader->end-reader->begin);
        }
    }else if(symbol.starts_with("LoadAccelerators") || symbol.starts_with("CreateAcceleratorTable")){
        std::vector<std::array<std::uint16_t,3>> entries;
        if(symbol.starts_with("Load")){
            if(auto reader=resource(arg(1),9)){if((reader->end-reader->begin)%8)throw HostCallFault("ACCEL 資源長度不合法");
                while(reader->at<reader->end){const auto flags=reader->number(2),key=reader->number(2),command=reader->number(2);(void)reader->number(2);
                    entries.push_back({static_cast<std::uint16_t>(flags&127),static_cast<std::uint16_t>(key),static_cast<std::uint16_t>(command)});if(flags&128)break;}
            }
        }else if(d(1)>0 && d(1)<=32768){
            for(std::uint64_t i=0;i<d(1);++i)entries.push_back({static_cast<std::uint16_t>(memory.load(GuestAddress{arg(0)+i*6},8)),static_cast<std::uint16_t>(memory.load(GuestAddress{arg(0)+i*6+2},16)),static_cast<std::uint16_t>(memory.load(GuestAddress{arg(0)+i*6+4},16))});
        }else fail(87);
        if(!entries.empty()){if(gui_.accelerators.size()>=4096)fail(8);else {result=gui_.next_object++;gui_.accelerators.emplace(result,std::move(entries));}}
    }else if(symbol=="DestroyAcceleratorTable") {result=gui_.accelerators.erase(arg(0));if(!result)last_error_=1403;}
    else if(symbol.starts_with("CopyAcceleratorTable")){
        const auto found=gui_.accelerators.find(arg(0));if(found==gui_.accelerators.end())fail(1403);
        else if(!arg(1))result=found->second.size();else if(d(2)>INT32_MAX)fail(87);
        else {result=std::min<std::size_t>(found->second.size(),d(2));for(std::uint64_t i=0;i<result;++i){const auto& entry=found->second[static_cast<std::size_t>(i)];memory.store(GuestAddress{arg(1)+i*6},8,entry[0]);memory.store(GuestAddress{arg(1)+i*6+2},16,entry[1]);memory.store(GuestAddress{arg(1)+i*6+4},16,entry[2]);}}
    }else if(symbol.starts_with("TranslateAccelerator")){
        const auto table=gui_.accelerators.find(arg(1));const auto window=gui_.windows.find(arg(0));
        if(table==gui_.accelerators.end())fail(1403);else if(window==gui_.windows.end())fail(1400);
        else if(window->second.enabled){const auto message=memory.load(GuestAddress{arg(2)+8},32),key=memory.load(GuestAddress{arg(2)+16},64);
            for(const auto& entry:table->second){const auto flags=entry[0];const bool virt=flags&1;
                if(key!=entry[1] || (virt?(message!=0x100 && message!=0x104):(message!=0x102 && message!=0x106)))continue;
                if(virt && (((flags&4)!=0)!=((gui_.keyboard[16]&128)!=0) || ((flags&8)!=0)!=((gui_.keyboard[17]&128)!=0) || ((flags&16)!=0)!=((gui_.keyboard[18]&128)!=0)))continue;
                result=1;const auto procedure=window->second.procedure;
                if(procedure){start_callbacks(state,memory,{{procedure,{arg(0),0x111,0x10000U|entry[2],0}}},Continuation::Return,1);return std::nullopt;}break;
            }
        }
    }else if(symbol=="GetMenu" || symbol=="SetMenu" || symbol=="DrawMenuBar"){
        const auto window=gui_.windows.find(arg(0));if(window==gui_.windows.end())fail(1400);
        else if(symbol=="GetMenu")result=window->second.menu;
        else if(symbol=="SetMenu" && arg(1) && !gui_.menus.contains(arg(1)))fail(1401);
        else {if(symbol=="SetMenu"){auto& w=window->second;const auto outer=gui_outer_size(w);w.menu=arg(1);gui_refresh_frame(w);const auto client=gui_client_size(outer[0],outer[1],gui_frame_insets(w));w.width=client[0];w.height=client[1];present_gui(arg(0));}present_gui_menu(memory,arg(0));result=1;}
    }else if(symbol=="ChangeWindowMessageFilter" || symbol=="ChangeWindowMessageFilterEx"){
        const bool extended=symbol.ends_with("Ex");const auto message=d(extended?1:0),action=d(extended?2:1);
        if(extended && !gui_.windows.contains(arg(0)))fail(1400);
        else if(message>65535 || action>2 || (!extended && !action))fail(87);
        else if(extended && arg(3) && memory.load(GuestAddress{arg(3)},32)!=8)fail(87);
        else fail(50); // 尚無跨 integrity 的 UIPI 訊息來源／過濾後端。
    }else if(symbol.starts_with("RegisterDeviceNotification") || symbol=="UnregisterDeviceNotification"){
        return invoke_device_notification(state,memory,service);
    }else if(symbol=="GetDisplayConfigBufferSizes" || symbol=="QueryDisplayConfig" || symbol=="DisplayConfigGetDeviceInfo" || symbol=="DisplayConfigSetDeviceInfo"){
        result=interactive_station()?display_config_call(memory,arg,symbol):5;
    }
    else if(symbol.starts_with("EnumDisplayDevices")){
        auto device=arg(0)?(wide?read_utf16(memory,GuestAddress{arg(0)}):utf8_to_utf16(read_ansi(memory,arg(0)))):std::u16string{};
        for(auto& c:device)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
        const unsigned stride=wide?2:1,size=wide?840:424;
        if(!arg(2) || (d(3)&~1U))fail(87);
        else if(d(3)&1U)fail(50); // 沒有可開啟的 PnP 裝置介面路徑。
        else if(d(1) || (!device.empty() && device!=u"\\\\.\\display1"))result=0;
        else if(memory.load(GuestAddress{arg(2)},32)!=size)fail(87);
        else {memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(size));memory.store(GuestAddress{arg(2)},32,size);
            (void)copy_text(arg(2)+4,32,device.empty()?u"\\\\.\\DISPLAY1":u"\\\\.\\DISPLAY1\\Monitor0");
            (void)copy_text(arg(2)+4+32*stride,128,device.empty()?u"x64arm Virtual Display":u"x64arm Virtual Monitor");
            memory.store(GuestAddress{arg(2)+4+160*stride},32,device.empty()?5:3);
            (void)copy_text(arg(2)+8+160*stride,128,device.empty()?display_identity::instance:u"X64ARM\\MONITOR");
            if(device.empty())(void)copy_text(arg(2)+8+288*stride,128,std::u16string(u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\")+std::u16string(display_identity::class_name)+u"\\0000");result=1;}
    }else if(symbol=="ChangeDisplaySettingsA" || symbol=="ChangeDisplaySettingsW" || symbol=="ChangeDisplaySettingsExA" || symbol=="ChangeDisplaySettingsExW"){
        // User32 桌面模式查詢與 DXGI 共用原生目前模式；本入口只接受現有模式，實際 DXGI 切換由交換鏈持有。
        const auto current_mode=native_gui_?native_gui_->current_display_mode():std::optional<GuiDisplayMode>{};
        const auto mode_width=current_mode?current_mode->width:1440U,mode_height=current_mode?current_mode->height:900U;
        const auto mode_rate=current_mode?(current_mode->refresh_numerator+current_mode->refresh_denominator/2)/current_mode->refresh_denominator:60U;
        if(job_ui_restrictions()&0x10U){last_error_=5;arg.return_to(state,static_cast<std::uint32_t>(-1));return std::nullopt;}
        const bool extended=symbol.starts_with("ChangeDisplaySettingsEx");
        const auto mode=arg(extended?1:0);const auto flags=d(extended?3:1);std::u16string device;
        if(extended && arg(0))device=wide?read_utf16(memory,GuestAddress{arg(0)}):utf8_to_utf16(read_ansi(memory,arg(0)));
        for(auto& c:device)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
        result=0;
        if(extended && (arg(2) || arg(4) || (!device.empty() && device!=u"\\\\.\\display1")))result=static_cast<std::uint32_t>(-5);
        else if(flags&~0x7000031fU)result=static_cast<std::uint32_t>(-4);
        else if(((flags&0x10000008U) && !(flags&1U)) || (flags&0x300U)==0x300U)result=static_cast<std::uint32_t>(-4);
        else if(mode){const unsigned prefix=wide?64:32,pixels=wide?168:104,total=wide?220:156;
            if(memory.load(GuestAddress{mode+prefix+4},16)<total)result=static_cast<std::uint32_t>(-5);
            else {const auto fields=memory.load(GuestAddress{mode+prefix+8},32);
                if((fields&0x40000U) && memory.load(GuestAddress{mode+pixels},32)!=32)result=static_cast<std::uint32_t>(-2);
                if((fields&0x80000U) && memory.load(GuestAddress{mode+pixels+4},32)!=mode_width)result=static_cast<std::uint32_t>(-2);
                if((fields&0x100000U) && memory.load(GuestAddress{mode+pixels+8},32)!=mode_height)result=static_cast<std::uint32_t>(-2);
                if((fields&0x400000U) && memory.load(GuestAddress{mode+pixels+16},32)>1 && memory.load(GuestAddress{mode+pixels+16},32)!=mode_rate)result=static_cast<std::uint32_t>(-2);
                if((fields&0x200000U) && memory.load(GuestAddress{mode+pixels+12},32))result=static_cast<std::uint32_t>(-2);
                if(fields&~0x7c0020U)result=static_cast<std::uint32_t>(-5);
                if((fields&0x20U) && memory.load(GuestAddress{mode+prefix+12},64))result=static_cast<std::uint32_t>(-2);
            }
        }
    }else if(symbol.starts_with("EnumDisplaySettings")){
        const auto current_mode=native_gui_?native_gui_->current_display_mode():std::optional<GuiDisplayMode>{};
        const auto mode_width=current_mode?current_mode->width:1440U,mode_height=current_mode?current_mode->height:900U;
        const auto mode_rate=current_mode?(current_mode->refresh_numerator+current_mode->refresh_denominator/2)/current_mode->refresh_denominator:60U;
        auto device=arg(0)?(wide?read_utf16(memory,GuestAddress{arg(0)}):utf8_to_utf16(read_ansi(memory,arg(0)))):std::u16string{};
        for(auto& c:device)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
        const unsigned name_bytes=wide?64:32,total=wide?220:156,pixels=wide?168:104;
        if(!arg(2) || (symbol.starts_with("EnumDisplaySettingsEx") && (d(3)&~3U)))fail(87);
        else if(!device.empty() && device!=u"\\\\.\\display1")fail(1168);
        else if(d(1)!=0 && d(1)!=UINT32_MAX && d(1)!=UINT32_MAX-1)fail(259);
        else if(memory.load(GuestAddress{arg(2)+name_bytes+4},16)!=total)fail(87);
        else {memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(total));(void)copy_text(arg(2),32,u"\\\\.\\DISPLAY1");
            memory.store(GuestAddress{arg(2)+name_bytes},16,0x401);memory.store(GuestAddress{arg(2)+name_bytes+2},16,1);memory.store(GuestAddress{arg(2)+name_bytes+4},16,total);
            memory.store(GuestAddress{arg(2)+name_bytes+8},32,0x007c0020);
            memory.store(GuestAddress{arg(2)+pixels-2},16,96);memory.store(GuestAddress{arg(2)+pixels},32,32);
            memory.store(GuestAddress{arg(2)+pixels+4},32,mode_width);memory.store(GuestAddress{arg(2)+pixels+8},32,mode_height);memory.store(GuestAddress{arg(2)+pixels+16},32,mode_rate);result=1;}
    }else if(symbol=="EnumDisplayMonitors"){
        if(!arg(2))fail(87);
        else if(arg(0))fail(50); // HDC 畫圖枚舉尚未提供，不能回報未套用 clipping 的成功結果。
        else {
            memory.check(GuestAddress{arg(2)},1,Access::Execute);bool visible=true;const auto screen=gui_screen_area();
            if(arg(1)){std::array<std::int32_t,4> rect{};for(unsigned i=0;i<4;++i)rect[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+4*i},32));visible=rect[0]<rect[2] && rect[1]<rect[3] && rect[0]<screen[2] && rect[1]<screen[3] && rect[2]>screen[0] && rect[3]>screen[1];}
            if(!visible)result=1;
            else {const auto rect=allocate_heap(memory,16);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{rect+i*4},32,static_cast<std::uint32_t>(screen[i]));
                try{start_callbacks(state,memory,{{arg(2),{1,0,rect,arg(3)}}},Continuation::ReturnCallbackFreeHeap,rect);}
                catch(...){(void)free_heap(memory,rect,state);throw;}return std::nullopt;}
        }
    }else if(symbol.starts_with("MonitorFrom")){
        const auto screen=gui_screen_area();if(d(1)>2)fail(87);else if(symbol=="MonitorFromWindow"){result=gui_.windows.contains(arg(0)) || d(1)?1:0;}
        else if(symbol=="MonitorFromPoint"){const auto x=static_cast<std::int32_t>(arg(0)),y=static_cast<std::int32_t>(arg(0)>>32);result=(x>=screen[0] && y>=screen[1] && x<screen[2] && y<screen[3]) || d(1)?1:0;}
        else {const auto x=static_cast<std::int32_t>(memory.load(GuestAddress{arg(0)},32)),y=static_cast<std::int32_t>(memory.load(GuestAddress{arg(0)+4},32)),right=static_cast<std::int32_t>(memory.load(GuestAddress{arg(0)+8},32)),bottom=static_cast<std::int32_t>(memory.load(GuestAddress{arg(0)+12},32));result=(x<screen[2] && y<screen[3] && right>screen[0] && bottom>screen[1]) || d(1)?1:0;}
    }else if(symbol.starts_with("GetMonitorInfo")){
        const auto size=memory.load(GuestAddress{arg(1)},32);if(arg(0)!=1)fail(1461);else if(size!=40 && size!=(wide?104U:72U))fail(87);
        else {memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(size),Access::Write);for(unsigned offset:{4U,20U}){const auto area=offset==4?gui_screen_area():gui_work_area();for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(1)+offset+i*4},32,static_cast<std::uint32_t>(area[i]));}memory.store(GuestAddress{arg(1)+36},32,1);if(size>40)(void)copy_text(arg(1)+40,32,u"\\\\.\\DISPLAY1");result=1;}
    }else if(symbol.starts_with("SystemParametersInfo")){
        // 與 GetSystemMetrics、AppBar 共用桌面／工作區；未支援的 action 回傳明確錯誤。
        const auto font=[&](std::uint64_t pointer){memory.write(GuestAddress{pointer},std::vector<std::uint8_t>(wide?92U:60U));memory.store(GuestAddress{pointer},32,static_cast<std::uint32_t>(-12));memory.store(GuestAddress{pointer+16},32,400);memory.store(GuestAddress{pointer+23},8,1);memory.store(GuestAddress{pointer+24},8,4);memory.store(GuestAddress{pointer+26},8,5);(void)copy_text(pointer+28,32,runtime_.network.external_io?native_ui_font_family():u"Segoe UI");};
        if(d(0)==41){const auto size=memory.load(GuestAddress{arg(2)},32);const unsigned old_size=wide?500U:340U,lf=wide?92U:60U;
            if(size!=old_size && size!=old_size+4)fail(87);else {memory.write(GuestAddress{arg(2)+4},std::vector<std::uint8_t>(static_cast<std::size_t>(size-4)));
                for(unsigned offset:{4U,8U,12U,16U,20U})memory.store(GuestAddress{arg(2)+offset},32,offset==4?1:offset==20?24:16);
                font(arg(2)+24);memory.store(GuestAddress{arg(2)+24+lf},32,16);memory.store(GuestAddress{arg(2)+28+lf},32,20);font(arg(2)+32+lf);
                memory.store(GuestAddress{arg(2)+32+lf*2},32,16);memory.store(GuestAddress{arg(2)+36+lf*2},32,20);
                font(arg(2)+40+lf*2);font(arg(2)+40+lf*3);font(arg(2)+40+lf*4);result=1;}
        }else if(d(0)==0x4a || d(0)==0x200a || d(0)==0x200c || d(0)==0x2012 || d(0)==0x1048){
            if(!arg(2))fail(87);else {
                const auto value=d(0)==0x4a?static_cast<std::uint32_t>(gui_.font_smoothing):d(0)==0x200a?gui_.font_smoothing_type:d(0)==0x200c?gui_.font_smoothing_contrast:d(0)==0x2012?gui_.font_smoothing_orientation:static_cast<std::uint32_t>(gui_.font_smoothing && gui_.font_smoothing_type==2);
                memory.store(GuestAddress{arg(2)},32,value);result=1;
            }
        }else if(d(0)==0x4b || d(0)==0x200b || d(0)==0x200d || d(0)==0x2013 || d(0)==0x1049){
            // uiParam 是 smoothing 開關；其餘 action 的 pvParam 是值，不是客體指標。
            const auto value=d(0)==0x4b?arg(1):arg(2);
            if(d(3)&~3U)fail(87);else if(d(3))fail(50);else if(job_ui_restrictions()&8U)fail(5);
            else if(value>UINT32_MAX || ((d(0)==0x4b || d(0)==0x1049) && value>1) || (d(0)==0x200b && value!=1 && value!=2) || (d(0)==0x200d && (value<1000 || value>2200)) || (d(0)==0x2013 && value>1))fail(87);
            else {if(d(0)==0x4b)gui_.font_smoothing=value!=0;else if(d(0)==0x200b)gui_.font_smoothing_type=static_cast<std::uint32_t>(value);else if(d(0)==0x200d)gui_.font_smoothing_contrast=static_cast<std::uint32_t>(value);else if(d(0)==0x2013)gui_.font_smoothing_orientation=static_cast<std::uint32_t>(value);else gui_.font_smoothing_type=value?2:1;result=1;}
        }else if(d(0)==0x4c || d(0)==0x4d){
            // SPI_SETDRAGWIDTH／HEIGHT 與 SM_CXDRAG／CYDRAG 共用有號門檻。
            auto& value=d(0)==0x4c?gui_.mouse_drag_width:gui_.mouse_drag_height;
            if(d(3)&~3U)fail(87);else if(d(3))fail(50);else {value=std::bit_cast<std::int32_t>(d(1));result=1;}
        }else if(d(0)>=0x62 && d(0)<=0x67){
            auto& value=d(0)<=0x63?gui_.mouse_hover_width:d(0)<=0x65?gui_.mouse_hover_height:gui_.mouse_hover_time;
            if(d(0)&1U){if(job_ui_restrictions()&8U)fail(5);else if(d(1)>0x7fffffffU)fail(87);else {value=d(1);result=1;}}
            else if(!arg(2))fail(87);else {memory.store(GuestAddress{arg(2)},32,value);result=1;}
        }else if(d(0)==31){font(arg(2));result=1;}
        else if(d(0)==48){memory.check(GuestAddress{arg(2)},16,Access::Write);const auto area=gui_work_area();for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(2)+i*4},32,static_cast<std::uint32_t>(area[i]));result=1;}
        else if(d(0)==0x1022 || d(0)==0x1026){memory.store(GuestAddress{arg(2)},32,0);result=1;}
        else if(d(0)==0x26){memory.store(GuestAddress{arg(2)},32,1);result=1;}
        else fail(87);
    }else throw HostCallFault("未實作的 GUI resource API");
    arg.return_to(state,result);return std::nullopt;
}
}
