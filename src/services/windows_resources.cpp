#include "gui_window_menu.hpp"
#include "windows_input_layout.hpp"
#include "windows_display_identity.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_keyboard.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/windows_icon.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/file_security.hpp"
#include <algorithm>
#include <bit>
#include <functional>
#include <variant>

namespace x64arm {
std::span<const WindowsServiceDescriptor> resource_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","BeginUpdateResourceA",2},{"kernel32.dll","BeginUpdateResourceW",2},
        {"kernel32.dll","UpdateResourceA",6},{"kernel32.dll","UpdateResourceW",6},
        {"kernel32.dll","EndUpdateResourceA",2},{"kernel32.dll","EndUpdateResourceW",2},
        {"kernel32.dll","FreeResource",1},
        {"kernel32.dll","EnumResourceTypesA",3},{"kernel32.dll","EnumResourceTypesW",3},
        {"kernel32.dll","EnumResourceNamesA",4},{"kernel32.dll","EnumResourceNamesW",4},
        {"kernel32.dll","EnumResourceLanguagesA",5},{"kernel32.dll","EnumResourceLanguagesW",5},
        {"kernel32.dll","EnumResourceTypesExA",5},{"kernel32.dll","EnumResourceTypesExW",5},{"kernel32.dll","EnumResourceNamesExA",6},{"kernel32.dll","EnumResourceNamesExW",6},
        {"kernel32.dll","EnumResourceLanguagesExA",7},{"kernel32.dll","EnumResourceLanguagesExW",7},
        {"user32.dll","LoadStringA",4},{"user32.dll","LoadStringW",4},
        {"user32.dll","LoadBitmapA",2},{"user32.dll","LoadBitmapW",2},
        {"user32.dll","SetThreadDpiAwarenessContext",1},{"user32.dll","GetThreadDpiAwarenessContext",0},
        {"user32.dll","SetProcessDpiAwarenessContext",1},{"user32.dll","IsValidDpiAwarenessContext",1},
        {"user32.dll","SetProcessDpiAwarenessInternal",1},{"user32.dll","GetProcessDpiAwarenessInternal",2},
        {"user32.dll","AreDpiAwarenessContextsEqual",2},{"user32.dll","GetAwarenessFromDpiAwarenessContext",1},
        {"user32.dll","GetWindowDpiAwarenessContext",1},{"user32.dll","GetDpiFromDpiAwarenessContext",1},
        {"user32.dll","EnableNonClientDpiScaling",1},{"user32.dll","GetSystemMetricsForDpi",2},
        {"user32.dll","GetDpiForWindow",1},{"user32.dll","GetDpiForSystem",0},
        {"shcore.dll","SetProcessDpiAwareness",1},{"shcore.dll","GetProcessDpiAwareness",2},{"shcore.dll","GetDpiForMonitor",4,0,0,0x2},{"user32.dll","GetDpiForMonitorInternal",4,0,0,0x2},
        {"shcore.dll","GetScaleFactorForMonitor",2},{"shcore.dll","GetScaleFactorForDevice",1,0,0,0x1},{"shcore.dll","RegisterScaleChangeEvent",2},{"shcore.dll","UnregisterScaleChangeEvent",1},
        {"shcore.dll","RegisterScaleChangeNotifications",4,0,0,0x5},{"shcore.dll","RevokeScaleChangeNotifications",2,0,0,0x3},
        {"user32.dll","SetProcessDPIAware",0},{"user32.dll","IsProcessDPIAware",0},{"user32.dll","GetDialogBaseUnits",0},
        {"user32.dll","CallWindowProcA",5},{"user32.dll","CallWindowProcW",5},{"user32.dll","GetKeyState",1},
        {"user32.dll","MapVirtualKeyA",2},{"user32.dll","MapVirtualKeyW",2},{"user32.dll","MapVirtualKeyExA",3},{"user32.dll","MapVirtualKeyExW",3},
        {"user32.dll","GetClassInfoA",3},{"user32.dll","GetClassInfoW",3},{"user32.dll","GetClassInfoExA",3},{"user32.dll","GetClassInfoExW",3},
        {"user32.dll","GetWindowPlacement",2},{"user32.dll","SetWindowPlacement",2},
        {"user32.dll","GetInternalWindowPos",3},{"user32.dll","SetInternalWindowPos",4,0,0,0x2},
        {"win32u.dll","NtUserWindowFromPoint",2,0,0,0x3},{"win32u.dll","NtUserChildWindowFromPointEx",4,0,0,0xe},
        {"win32u.dll","NtUserRealChildWindowFromPoint",3,0,0,0x6},
        {"user32.dll","WindowFromPoint",1,1},{"user32.dll","WindowFromPhysicalPoint",1,1},{"user32.dll","ChildWindowFromPoint",2,2},{"user32.dll","ChildWindowFromPointEx",3,2},{"user32.dll","RealChildWindowFromPoint",2,2},
        {"user32.dll","LoadMenuA",2},{"user32.dll","LoadMenuW",2},
        {"user32.dll","LoadMenuIndirectA",1},{"user32.dll","LoadMenuIndirectW",1},
        {"user32.dll","LoadAcceleratorsA",2},{"user32.dll","LoadAcceleratorsW",2},
        {"user32.dll","CreateAcceleratorTableA",2},{"user32.dll","CreateAcceleratorTableW",2},
        {"user32.dll","CopyAcceleratorTableA",3},{"user32.dll","CopyAcceleratorTableW",3},
        {"user32.dll","DestroyAcceleratorTable",1},{"user32.dll","TranslateAcceleratorA",3},{"user32.dll","TranslateAcceleratorW",3},
        {"user32.dll","GetMenu",1},{"user32.dll","SetMenu",2},{"user32.dll","DrawMenuBar",1},
        {"user32.dll","MonitorFromWindow",2},{"user32.dll","MonitorFromPoint",2,1},{"user32.dll","MonitorFromRect",2},
        {"user32.dll","ChangeWindowMessageFilter",2},{"user32.dll","ChangeWindowMessageFilterEx",4},{"user32.dll","RegisterDeviceNotificationA",3},{"user32.dll","RegisterDeviceNotificationW",3},{"user32.dll","UnregisterDeviceNotification",1},{"user32.dll","EnumDisplayDevicesA",4},{"user32.dll","EnumDisplayDevicesW",4},{"user32.dll","ChangeDisplaySettingsA",2},{"user32.dll","ChangeDisplaySettingsW",2},{"user32.dll","ChangeDisplaySettingsExA",5},{"user32.dll","ChangeDisplaySettingsExW",5},{"user32.dll","EnumDisplaySettingsA",3},{"user32.dll","EnumDisplaySettingsW",3},{"user32.dll","EnumDisplaySettingsExA",4},{"user32.dll","EnumDisplaySettingsExW",4},{"user32.dll","EnumDisplayMonitors",4},{"user32.dll","GetMonitorInfoA",2},{"user32.dll","GetMonitorInfoW",2},
        {"win32u.dll","NtUserEnumDisplayDevices",4},{"win32u.dll","NtUserEnumDisplaySettings",4},{"win32u.dll","NtUserChangeDisplaySettings",5},
        {"user32.dll","SetDisplayConfig",5},{"win32u.dll","NtUserSetDisplayConfig",5},
        {"win32u.dll","NtUserDisplayConfigGetDeviceInfo",1},{"win32u.dll","NtUserDisplayConfigSetDeviceInfo",1},
        {"user32.dll","GetDisplayConfigBufferSizes",3},{"user32.dll","QueryDisplayConfig",6},{"user32.dll","DisplayConfigGetDeviceInfo",1},{"user32.dll","DisplayConfigSetDeviceInfo",1},
        {"shlwapi.dll","#537",2},
        {"user32.dll","SystemParametersInfoA",4},{"user32.dll","SystemParametersInfoW",4}
    };return table;
}
namespace {
// 已轉小寫的 "\\.\displayN" → N（1 起算）；格式不符回 0。
std::uint64_t display_device_index(std::u16string_view name){
    constexpr std::u16string_view prefix=u"\\\\.\\display";
    if(!name.starts_with(prefix) || name.size()==prefix.size() || name.size()>prefix.size()+3)return 0;
    std::uint64_t value=0;
    for(const auto c:name.substr(prefix.size())){if(c<u'0' || c>u'9')return 0;value=value*10+(c-u'0');}
    return value;
}
// 與現有 GDI 單一虛擬桌面一致，並非宿主螢幕 EDID／硬體時序。
constexpr std::uint64_t display_adapter_luid=UINT64_C(0x58464d0000000001);
constexpr unsigned display_width=1440,display_height=900,display_refresh=60;
void display_put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint64_t value,unsigned count=4){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));}
using DisplayMode=std::array<std::uint32_t,4>;
void display_signal(std::vector<std::uint8_t>& bytes,std::size_t at,const DisplayMode& mode){
    const auto [width,height,depth,refresh]=mode;
    (void)depth;
    display_put(bytes,at,std::uint64_t{width}*height*refresh,8);
    display_put(bytes,at+8,height*refresh);display_put(bytes,at+12,1);
    display_put(bytes,at+16,refresh);display_put(bytes,at+20,1);
    display_put(bytes,at+24,width);display_put(bytes,at+28,height);
    display_put(bytes,at+32,width);display_put(bytes,at+36,height);display_put(bytes,at+44,1);
}
std::uint32_t display_config_call(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol,const DisplayMode& active,const DisplayMode& preferred){
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
            // DisplayConfig 尚未接上監視器共用身分（display_identity）：monitorDevicePath 留空、edidIdsValid＝0，名稱由虛擬顯示器指定。
            display_put(bytes,20,2);display_put(bytes,24,UINT32_MAX);
            const auto name=utf16_bytes(u"x64arm Virtual Monitor");std::copy(name.begin(),name.end(),bytes.begin()+36);
        }else if(type==3){display_put(bytes,20,preferred[0]);display_put(bytes,24,preferred[1]);display_signal(bytes,32,preferred);}
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
    display_put(path,40,1);display_put(path,44,1);display_put(path,48,active[3]);display_put(path,52,1);
    display_put(path,56,1);display_put(path,60,1);display_put(path,64,1);display_put(path,68,1);
    display_put(mode,0,1);display_put(mode,8,display_adapter_luid,8);display_put(mode,16,active[0]);display_put(mode,20,active[1]);display_put(mode,24,active[2]==16?2:4);
    display_put(mode,64,2);display_put(mode,72,display_adapter_luid,8);display_signal(mode,80,active);
    memory.write(GuestAddress{arg(2)},path);memory.write(GuestAddress{arg(4)},mode);
    memory.store(GuestAddress{arg(1)},32,1);memory.store(GuestAddress{arg(3)},32,2);if(arg(5))memory.store(GuestAddress{arg(5)},32,1);return 0;
}
// 單一路徑虛擬桌面的 CCD 設定；先完整解析，成功後才由呼叫端發布。
std::uint32_t select_display_config(GuestMemory& memory,const WindowsCall& arg,const DisplayMode& saved,DisplayMode& mode){
    const auto flags=static_cast<std::uint32_t>(arg(4)),topology=flags&0x3fU;
    const bool apply=flags&0x80U,supplied=topology==0x20;
    if((flags&~0x2bfffU) || apply==bool(flags&0x40U) ||
       ((flags&0x200U) && (!supplied || !apply)))return 87;
    if(!supplied){
        if(arg(0) || arg(1) || arg(2) || arg(3) || !topology || topology>15)return 87;
        if(!(topology&1U))return 1610;
        mode=saved;return 0;
    }
    const auto path=arg(1),modes=arg(3);
    const auto count=static_cast<std::uint32_t>(arg(2));
    if(!arg(0) || !path || (count&&!modes) || (!count&&modes))return 87;
    if(arg(0)!=1)return 1610;
    memory.check(GuestAddress{path},72,Access::Read);
    const auto field=[&](std::uint64_t address){return static_cast<std::uint32_t>(memory.load(GuestAddress{address},32));};
    if(memory.load(GuestAddress{path},64)!=display_adapter_luid || field(path+8) ||
       memory.load(GuestAddress{path+20},64)!=display_adapter_luid || field(path+28) || field(path+68)!=1)return 1610;
    if(field(path+40)!=1 || field(path+44)!=1 || field(path+56)!=1)return 50;
    if(field(path+52) && field(path+48)!=std::uint64_t{mode[3]}*field(path+52))return 1610;
    for(unsigned part=0;part<2;++part){
        const auto index=field(path+(part?32:12));
        if(index==UINT32_MAX)continue;
        if(index>=count)return 87;
        const auto record=modes+std::uint64_t{index}*64;
        memory.check(GuestAddress{record},64,Access::Read);
        if(field(record)!=part+1 || field(record+4) || memory.load(GuestAddress{record+8},64)!=display_adapter_luid)return 87;
        if(!part){
            const auto format=field(record+24);
            if((format!=2 && format!=4) || field(record+28) || field(record+32))return 1610;
            mode[0]=field(record+16);mode[1]=field(record+20);mode[2]=format==2?16:32;
            if(mode[0]<320 || mode[1]<200 || mode[0]>16384 || mode[1]>16384)return 1610;
        }else{
            if(field(record+40)!=mode[0] || field(record+44)!=mode[1] ||
               !field(record+36) || field(record+32)!=std::uint64_t{mode[3]}*field(record+36))return 1610;
        }
    }
    return 0;
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
// 判斷視窗所在螢幕的矩形：最小化視窗依還原位置，其他依外框的螢幕座標（MonitorFromWindow 與 Shell hook 共用）。
std::array<std::int32_t,4> WindowsServices::gui_window_screen_rect(const GuiWindow& window){
    if(window.show_command==2 && window.normal_rect){const auto& n=*window.normal_rect;return {n[0],n[1],n[0]+n[2],n[1]+n[3]};}
    const auto origin=gui_client_origin(gui_,window.parent);const auto outer=gui_outer_size(window);
    const auto x=static_cast<std::int32_t>(window.x+origin[0]),y=static_cast<std::int32_t>(window.y+origin[1]);
    return {x,y,x+static_cast<std::int32_t>(outer[0]),y+static_cast<std::int32_t>(outer[1])};
}
std::uint64_t WindowsServices::effective_dpi_context() const {
    const auto thread=threads_.items.find(threads_.current);
    return thread!=threads_.items.end() && thread->second.dpi_context?thread->second.dpi_context:gui_.dpi_context;
}
#include "windows_resource_update.inc"
std::optional<std::uint32_t> WindowsServices::invoke_resource(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol.starts_with("BeginUpdateResource") || service.symbol.starts_with("UpdateResource") || service.symbol.starts_with("EndUpdateResource"))return invoke_resource_update(state,memory,service);
    const WindowsCall arg(state,memory,service);
    auto symbol=service.symbol;
    const bool native_devices=symbol=="NtUserEnumDisplayDevices";
    const bool native_modes=symbol=="NtUserEnumDisplaySettings";
    const bool native_change=symbol=="NtUserChangeDisplaySettings";
    std::u16string native_device;
    if(native_devices || native_modes || native_change){
        if(arg(0)){
            try{
                const auto length=memory.load(GuestAddress{arg(0)},16);
                if((length&1) || length>memory.load(GuestAddress{arg(0)+2},16)){
                    arg.return_to(state,native_devices?0xc000000dU:native_change?0xfffffffbU:0U);return std::nullopt;
                }
                if(length){
                    const auto buffer=memory.load(GuestAddress{arg(0)+(x86_?4U:8U)},x86_?32:64);
                    memory.check(GuestAddress{buffer},length,Access::Read);
                    for(unsigned offset=0;offset<length;offset+=2)
                        native_device.push_back(static_cast<char16_t>(memory.load(GuestAddress{buffer+offset},16)));
                }
            }catch(const MemoryFault&){
                arg.return_to(state,native_devices?0xc0000005U:native_change?0xfffffffbU:0U);return std::nullopt;
            }
        }
        symbol=native_devices?"EnumDisplayDevicesW":native_modes?"EnumDisplaySettingsExW":"ChangeDisplaySettingsExW";
    }
    const bool wide=symbol.ends_with('W');
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
    const auto publish_display_mode=[&](const DisplayMode& previous,const DisplayMode& selected){
        gui_.display_mode=selected;
        gui_area_cached_={};
        if(previous==selected)return;
        ++gui_.display_uniqueness;
        for(const auto& [window,item]:gui_.windows)if(!item.parent && !item.destroying)
            enqueue_gui_message({window,selected[2],selected[0]|(std::uint64_t{selected[1]}<<16),0x7e});
    };
    if(symbol.starts_with("EnumResource")){
        const bool types=symbol.starts_with("EnumResourceTypes"),names=symbol.starts_with("EnumResourceNames");
        const unsigned function=types?1:names?2:3;
        const auto* image=resource_image(arg(0)?arg(0):runtime_.image.base);
        // Ex 版本：RESOURCE_ENUM_LN（1）列舉模組本身，RESOURCE_ENUM_MUI（2／4）列舉 MUI 檔；沒有 MUI 檔時只剩 LN 的內容，
        // 0 等同 LN|MUI。LangId 只用來挑選 MUI 檔。
        const bool extended=symbol.find("Ex")!=symbol.npos;const auto enum_flags=extended?d(function+2):0U;
        if(!image)fail(126);
        else if(!arg(function) || (!types && !arg(1)) || (!types && !names && !arg(2)) || (enum_flags&~0xfU))fail(87);
        else if(enum_flags && !(enum_flags&1U))fail(types?1812U:names?1813U:1815U);
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
    }else if(symbol=="GetScaleFactorForMonitor" || symbol=="GetScaleFactorForDevice"){
        // 虛擬桌面固定 96 DPI，每台螢幕都是 SCALE_100_PERCENT；未知裝置類型為 DEVICE_SCALE_FACTOR_INVALID。
        if(symbol=="GetScaleFactorForDevice")result=d(0)<=1?100U:0U;
        else if(!arg(0) || arg(0)>gui_monitors().size() || !arg(1))result=0x80070057U;
        else {memory.store(GuestAddress{arg(1)},32,100);result=0;}
    }else if(symbol=="RegisterScaleChangeEvent" || symbol=="UnregisterScaleChangeEvent" || symbol=="RegisterScaleChangeNotifications" || symbol=="RevokeScaleChangeNotifications"){
        // 登錄縮放變更的事件或視窗訊息；虛擬桌面的縮放固定為 100%，登錄有效但不會觸發。
        result=0x80070057U;
        if(symbol=="RegisterScaleChangeEvent"){
            const auto event=runtime_.wait_objects.find(arg(0));
            if(event!=runtime_.wait_objects.end() && event->second.kind==WaitObject::Kind::Event && arg(1)){
                const auto cookie=++next_scale_change_cookie_;scale_change_events_[cookie]=arg(0);memory.store(GuestAddress{arg(1)},x86_?32:64,cookie);result=0;}
        }else if(symbol=="UnregisterScaleChangeEvent"){if(scale_change_events_.erase(arg(0)))result=0;}
        else if(symbol=="RegisterScaleChangeNotifications"){
            if(d(0)<=1 && gui_.windows.contains(arg(1)) && arg(3)){
                const auto cookie=static_cast<std::uint32_t>(++next_scale_change_cookie_);scale_change_windows_[cookie]={d(0),arg(1),d(2)};memory.store(GuestAddress{arg(3)},32,cookie);result=0;}
        }else if(const auto found=scale_change_windows_.find(d(1));found!=scale_change_windows_.end() && found->second.device==d(0)){scale_change_windows_.erase(found);result=0;}
    }else if(symbol=="GetDpiForMonitor" || symbol=="GetDpiForMonitorInternal"){
        // 虛擬桌面以 96 DPI 呈現；所有已列舉的 HMONITOR 都屬於同一座標模型。
        std::uint32_t error=0;
        if(d(1)>2)error=160; // ERROR_BAD_ARGUMENTS
        else if(!arg(2) || !arg(3))error=487; // ERROR_INVALID_ADDRESS
        else if(!arg(0) || arg(0)>gui_monitors().size())error=1461;
        else try{
            memory.check(GuestAddress{arg(2)},4,Access::Write);
            memory.check(GuestAddress{arg(3)},4,Access::Write);
            memory.store(GuestAddress{arg(2)},32,96);
            memory.store(GuestAddress{arg(3)},32,96);
        }catch(const MemoryFault&){error=998;}
        if(symbol=="GetDpiForMonitor")result=error?0x80070057U:0;
        else if(error)fail(error);
        else result=1;
    }else if(symbol=="SetThreadDpiAwarenessContext" || symbol=="GetThreadDpiAwarenessContext" || symbol=="SetProcessDpiAwarenessContext" || symbol=="SetProcessDPIAware" || symbol=="SetProcessDpiAwareness" || symbol=="GetProcessDpiAwareness" || symbol=="SetProcessDpiAwarenessInternal" || symbol=="GetProcessDpiAwarenessInternal" || symbol=="IsProcessDPIAware" || symbol=="IsValidDpiAwarenessContext" || symbol=="AreDpiAwarenessContextsEqual" || symbol=="GetAwarenessFromDpiAwarenessContext" || symbol=="GetWindowDpiAwarenessContext" || symbol=="GetDpiFromDpiAwarenessContext" || symbol=="GetDpiForWindow" || symbol=="GetDpiForSystem" || symbol=="GetSystemMetricsForDpi" || symbol=="EnableNonClientDpiScaling"){
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
        else if(symbol=="GetSystemMetricsForDpi"){if(!d(1))fail(87);else result=system_metric(memory,d(0),d(1));}
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
        else if(symbol=="GetProcessDpiAwareness" || symbol=="GetProcessDpiAwarenessInternal"){
            // USER32 內部入口使用 BOOL／LastError；SHCore 使用 HRESULT，共用程序狀態。
            const bool internal=symbol=="GetProcessDpiAwarenessInternal";
            const bool process=arg(0)==0 || current_process_handle(arg(0));
            std::uint32_t error=arg(1) && process?0:87;
            if(arg(1) && (process || internal))try{memory.store(GuestAddress{arg(1)},32,process?awareness(gui_.dpi_context):UINT32_MAX);}
            catch(const MemoryFault&){error=87;}
            if(error){if(internal)fail(error);else result=0x80070000U|error;}
            else result=internal?1:0;
        }else {
            const bool hresult=symbol=="SetProcessDpiAwareness",enumerated=hresult || symbol=="SetProcessDpiAwarenessInternal";
            const auto value=symbol=="SetProcessDPIAware"?UINT64_MAX-1:enumerated?UINT64_MAX-d(0):context;
            const auto error=!valid(value) || (enumerated && d(0)>2)?87U:(gui_.dpi_context_set && !(symbol=="SetProcessDPIAware" && value==gui_.dpi_context)) || (!gui_.windows.empty() && symbol!="SetProcessDPIAware")?5U:0U;
            if(error){if(hresult)result=0x80070000U|error;else fail(error);}
            else{gui_.dpi_context=value;gui_.dpi_context_set=true;gui_.dpi_aware=awareness(value)!=0;result=hresult?0:1;}
        }
    }
    else if(symbol=="GetKeyState"){
        const auto vk=d(0);
        const auto byte=vk<256?threads_.input_queue(threads_.current).keys[vk]:0U;
        result=((byte&0x80U)?0x8000U:0U)|(byte&1U);
    }
    else if(symbol.starts_with("GetClassInfo")){
        return invoke_gui_support(state,memory,service);
    }else if(symbol=="GetWindowPlacement" || symbol=="SetWindowPlacement" ||
             symbol=="GetInternalWindowPos" || symbol=="SetInternalWindowPos"){
        using Point=std::array<std::int32_t,2>;
        using Rect=std::array<std::int32_t,4>;
        const bool internal=symbol.ends_with("InternalWindowPos"), get=symbol.starts_with("Get");
        const auto hwnd=arg(0), buffer=arg(1);
        const auto found=gui_.windows.find(hwnd);
        const bool desktop=hwnd==gui_.desktop && hwnd!=0;
        const auto remote=get && !desktop && found==gui_.windows.end()?shared_gui_window(hwnd):std::nullopt;
        const auto work_for=[&](const Rect& rect){
            const auto index=gui_monitor_from_rect(rect,2);
            const auto& monitors=gui_monitors();
            return index && index<=monitors.size() && !monitors[index-1].primary ? monitors[index-1].work : gui_work_area();
        };
        const auto write=[&](std::uint64_t address,const auto& values){
            for(unsigned i=0;i<values.size();++i)memory.store(GuestAddress{address+i*4},32,static_cast<std::uint32_t>(values[i]));
        };
        const auto read=[&](std::uint64_t address,auto& values){
            for(unsigned i=0;i<values.size();++i)values[i]=static_cast<std::int32_t>(memory.load(GuestAddress{address+i*4},32));
        };
        if(!internal && !buffer)result=0;
        else if(!internal && !get && memory.load(GuestAddress{buffer},32)!=44)fail(87);
        else if((!desktop && !remote && found==gui_.windows.end()) || (desktop && !get))fail(1400);
        else {
            const auto style=desktop?0:remote?remote->style:found->second.style;
            const auto ex_style=desktop?0:remote?remote->ex_style:found->second.ex_style;
            const bool workspace=!internal && !desktop && !(style&0x40000000U) && !(ex_style&0x80U);
            const Point empty{-1,-1};
            Rect rect{};
            Point minimum=empty, maximum=empty;
            std::uint32_t show=1, flags=0;
            if(get){
                if(desktop)rect=gui_screen_area();
                else if(remote){
                    show=(style&0x20000000U)?2:(style&0x01000000U)?3:1;
                    rect={remote->x,remote->y,static_cast<int>(static_cast<std::int64_t>(remote->x)+remote->outer_width),
                          static_cast<int>(static_cast<std::int64_t>(remote->y)+remote->outer_height)};
                }
                else {
                    const auto& w=found->second;
                    show=w.show_command;
                    const auto insets=gui_frame_insets(w);
                    rect={w.x,w.y,static_cast<std::int32_t>(w.width),static_cast<std::int32_t>(w.height)};
                    if(show!=1 && w.normal_rect)rect=*w.normal_rect;
                    rect[2]=static_cast<int>(static_cast<std::int64_t>(rect[2])+rect[0]+insets.left+insets.right);
                    rect[3]=static_cast<int>(static_cast<std::int64_t>(rect[3])+rect[1]+insets.top+insets.bottom);
                    minimum=show==2?Point{w.x,w.y}:w.min_position;
                    maximum=w.max_position;
                    if(show==3){
                        maximum={w.x,w.y};
                        const auto work=work_for({w.x,w.y,w.x+1,w.y+1});
                        if(!(w.style&0x40000000U) && w.x<=work[0] && w.y<=work[1] &&
                           w.x+static_cast<std::int64_t>(w.width)+insets.left+insets.right>=work[2] &&
                           w.y+static_cast<std::int64_t>(w.height)+insets.top+insets.bottom>=work[3])maximum=empty;
                    }else if(!(w.style&0x40000000U))maximum=empty;
                    flags=w.restore_maximized?2:0;
                }
                if(workspace){
                    const auto work=work_for(rect);
                    for(unsigned i=0;i<2;++i){rect[i]-=work[i];rect[i+2]-=work[i];}
                    if(minimum!=empty){minimum[0]-=work[0];minimum[1]-=work[1];}
                    if(maximum!=empty && show!=3){maximum[0]-=work[0];maximum[1]-=work[1];}
                }
                if(internal){
                    if(buffer)write(buffer,rect);
                    if(arg(2))write(arg(2),minimum);
                    result=show;
                }else{
                    write(buffer,std::array<std::uint32_t,3>{44,flags,show});
                    write(buffer+12,minimum);write(buffer+20,maximum);write(buffer+28,rect);result=1;
                }
            }else{
                auto& w=found->second;
                const auto rect_ptr=internal?arg(2):buffer+28;
                flags=internal?0:static_cast<std::uint32_t>(memory.load(GuestAddress{buffer+4},32));
                show=internal?d(1):static_cast<std::uint32_t>(memory.load(GuestAddress{buffer+8},32));
                const auto min_ptr=internal?arg(3):(flags&1?buffer+12:0);
                if(rect_ptr)read(rect_ptr,rect);
                if(min_ptr)read(min_ptr,minimum);
                if(!internal)read(buffer+20,maximum);
                const auto width=static_cast<std::int64_t>(rect[2])-rect[0],height=static_cast<std::int64_t>(rect[3])-rect[1];
                if(show>11 || (rect_ptr && (width<1 || width>8192 || height<1 || height>8192)))fail(87);
                else {
                    std::vector<GuestCallback> calls;
                    const auto origin=workspace?work_for(rect):Rect{};
                    const auto fit=[](std::int64_t pos,std::int64_t size,int low,int high){
                        return static_cast<std::int32_t>(pos+size<=low?low:pos>=high?high-size:pos);
                    };
                    const auto fit_point=[&](Point point){
                        if(point!=empty){point[0]+=origin[0];point[1]+=origin[1];}
                        const auto work=work_for({point[0],point[1],point[0],point[1]});
                        return Point{fit(point[0],1,work[0],work[2]),fit(point[1],1,work[1],work[3])};
                    };
                    if(min_ptr)w.min_position=fit_point(minimum);
                    if(!internal)w.max_position=fit_point(maximum);
                    if(rect_ptr){
                        for(unsigned i=0;i<2;++i){rect[i]+=origin[i];rect[i+2]+=origin[i];}
                        const auto work=work_for(rect);
                        const auto x=fit(rect[0],width,work[0],work[2]),y=fit(rect[1],height,work[1],work[3]);
                        const auto size=gui_client_size(static_cast<unsigned>(width),static_cast<unsigned>(height),gui_frame_insets(w));
                        w.normal_rect=Rect{x,y,static_cast<int>(size[0]),static_cast<int>(size[1])};
                        if(w.show_command==1)calls.push_back({resolve("user32.dll","SetWindowPos").value,
                            {hwnd,0,static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y),
                             static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),0x14}});
                    }
                    if(w.show_command==2 && min_ptr){w.x=w.min_position[0];w.y=w.min_position[1];}
                    if(w.show_command==3 && !internal && (w.style&0x40000000U)){w.x=w.max_position[0];w.y=w.max_position[1];}
                    const bool minimizing=show==2 || show==6 || show==7 || show==11;
                    w.placement_restore_maximized=minimizing && (flags&2);
                    calls.push_back({resolve("user32.dll","ShowWindow").value,{hwnd,show}});
                    start_callbacks(state,memory,std::move(calls),Continuation::Return,1);
                    return std::nullopt;
                }
            }
        }
    }else if(symbol.ends_with("WindowFromPoint") || symbol.ends_with("WindowFromPointEx") || symbol=="WindowFromPhysicalPoint"){
        const bool child=symbol.find("ChildWindow")!=std::string_view::npos;
        const bool real=symbol.ends_with("RealChildWindowFromPoint");
        const bool split=symbol.starts_with("NtUser");
        const unsigned first=child?1U:0U;
        const auto point=arg(first);
        const auto x=static_cast<std::int32_t>(point);
        const auto y=static_cast<std::int32_t>(split?arg(first+1):point>>32);
        const auto flags=symbol.ends_with("PointEx")?d(split?3:2):real?5U:0U;
        const auto parent_handle=child?arg(0):0;
        const bool desktop=child && parent_handle==gui_.desktop;
        if(child && !desktop && !gui_.windows.contains(parent_handle))fail(1400);
        else if(flags>7)fail(87);
        else if(!child)result=gui_window_from_point(gui_,x,y,gui_.input_desktop);
        else {
            bool inside;
            if(desktop){
                const auto area=gui_screen_area();
                inside=x>=area[0] && y>=area[1] && x<area[2] && y<area[3];
            }else{
                const auto& parent=gui_.windows.at(parent_handle);
                inside=x>=0 && y>=0 && static_cast<std::uint32_t>(x)<parent.width && static_cast<std::uint32_t>(y)<parent.height;
            }
            if(inside)result=parent_handle;
            if(inside)for(const auto handle:gui_.z_order){const auto item=gui_.windows.find(handle);if(item==gui_.windows.end())continue;const auto& window=item->second;
                if(window.parent!=(desktop?0:parent_handle) || (desktop && window.desktop_object!=gui_.input_desktop))continue;
                if((flags&1) && !window.visible)continue;if((flags&2) && !window.enabled)continue;if((flags&4) && (window.ex_style&32))continue;
                const std::int64_t left=window.x,top=window.y;
                const auto outer=gui_outer_size(window);if(x>=left && x<left+outer[0] && y>=top && y<top+outer[1]){
                    if(real){
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
        const auto load_bitmap=[&]()->std::optional<GuiBitmap>{
            if(image && (d(2)!=0 || (d(5)&~0xa050U))){fail(50);return {};}
            if(image && (d(5)&0x10U)){
                if(arg(0) || arg(1)<=65535){fail(87);return {};}
                const auto path=wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)})):read_ansi(memory,arg(1));
                const auto file=filesystem_.open(path);
                if(file.error){fail(file.error);return {};}
                try{return decode_bitmap_file(*file.bytes);}
                catch(const std::invalid_argument&){fail(193);return {};}
            }
            const auto reader=resource(arg(1),2);
            if(!reader)return {};
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
            return bitmap;
        };
        if(auto loaded=load_bitmap()){
            auto& bitmap=*loaded;
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
        else if(window->second.enabled){const auto msg=gui_read_message(memory,arg(2));
            for(const auto& entry:table->second){const auto flags=entry[0];const bool virt=flags&1;
                if(msg.wparam!=entry[1] || (virt?(msg.message!=0x100 && msg.message!=0x104):(msg.message!=0x102 && msg.message!=0x106)))continue;
                const auto& keys=threads_.input_queue(threads_.current).keys;
                if(virt && (((flags&4)!=0)!=((keys[16]&128)!=0) || ((flags&8)!=0)!=((keys[17]&128)!=0) || ((flags&16)!=0)!=((keys[18]&128)!=0)))continue;
                result=1;const auto procedure=window->second.procedure;
                if(procedure){start_callbacks(state,memory,{{procedure,{arg(0),0x111,0x10000U|entry[2],0}}},Continuation::Return,1);return std::nullopt;}break;
            }
        }
    }else if(symbol=="GetMenu" || symbol=="SetMenu" || symbol=="DrawMenuBar"){
        const auto handle=arg(0);
        const auto window=gui_.windows.find(handle);
        if(symbol=="GetMenu"){
            result=nested_service(state,memory,"user32.dll",x86_?"GetWindowLongW":"GetWindowLongPtrW",{handle,static_cast<std::uint32_t>(-12)});
        }else if(window==gui_.windows.end())fail(1400);
        else if(gui_uses_control_id(window->second))result=symbol=="DrawMenuBar";
        else if(symbol=="SetMenu" && arg(1) && !gui_.menus.contains(arg(1)))fail(1401);
        else {
            std::vector<GuestCallback> calls;
            if(symbol=="SetMenu"){
                if(gui_.capture==handle && nested_service(state,memory,"user32.dll","GetCapture",{})==handle)
                    calls.push_back({resolve("user32.dll","ReleaseCapture").value,{}});
                window->second.menu=arg(1);
                publish_gui_window(handle);
            }
            present_gui_menu(memory,handle);
            // SWP_FRAMECHANGED 走既有視窗定位流程，保留 WM_NCCALCSIZE 與外框尺寸。
            calls.push_back({resolve("user32.dll","SetWindowPos").value,{handle,0,0,0,0,0,0x37}});
            start_callbacks(state,memory,std::move(calls),Continuation::ReturnCallback);
            return std::nullopt;
        }
    }else if(symbol=="ChangeWindowMessageFilter" || symbol=="ChangeWindowMessageFilterEx"){
        const bool extended=symbol.ends_with("Ex");const auto message=d(extended?1:0),action=d(extended?2:1);
        if(extended && !gui_.windows.contains(arg(0)))fail(1400);
        else if(message>65535 || action>2 || (!extended && !action))fail(87);
        else if(extended && arg(3) && memory.load(GuestAddress{arg(3)},32)!=8)fail(87);
        else fail(50); // 尚無跨 integrity 的 UIPI 訊息來源／過濾後端。
    }else if(symbol.starts_with("RegisterDeviceNotification") || symbol=="UnregisterDeviceNotification"){
        return invoke_device_notification(state,memory,service);
    }else if(symbol.find("DisplayConfig")!=symbol.npos){
        const bool native=symbol.starts_with("NtUser");
        const auto operation=native?symbol.substr(6):symbol;
        const auto host=native_gui_?native_gui_->current_display_mode():std::optional<GuiDisplayMode>{};
        const DisplayMode original{host?host->width:display_width,host?host->height:display_height,32,
            host && host->refresh_denominator?(host->refresh_numerator+host->refresh_denominator/2)/host->refresh_denominator:display_refresh};
        const auto saved=gui_.display_registry_mode.value_or(original);
        const auto active=gui_.display_mode.value_or(original);
        try{
            if(!interactive_station())result=5;
            else if(operation=="SetDisplayConfig"){
                auto selected=active;
                result=(job_ui_restrictions()&0x10U)?5:select_display_config(memory,arg,saved,selected);
                if(!result && (d(4)&0x80U)){
                    if(d(4)&0x200U)gui_.display_registry_mode=selected;
                    publish_display_mode(active,selected);
                }
            }else{
                const auto& selected=operation=="QueryDisplayConfig" && (d(0)&7)==4?saved:active;
                result=display_config_call(memory,arg,operation,selected,original);
            }
            if(native && operation!="SetDisplayConfig" && result){
                if(operation=="DisplayConfigGetDeviceInfo"){
                    if(!arg(0) || memory.load(GuestAddress{arg(0)+4},32)<20)result=0xc0000001U;
                    else if(result==87)result=0xc0000001U;
                    else result=result==5?0xc0000022U:0xc000000dU;
                }else result=result==5?0xc0000022U:result==50?0xc00000bbU:result==1610?0xc0000001U:0xc000000dU;
            }
        }catch(const MemoryFault&){result=native && operation!="SetDisplayConfig"?0xc0000005U:998U;}
    }
    else if(symbol.starts_with("EnumDisplayDevices")){
        auto device=native_devices || native_modes?native_device:arg(0)?(wide?read_utf16(memory,GuestAddress{arg(0)}):utf8_to_utf16(read_ansi(memory,arg(0)))):std::u16string{};
        for(auto& c:device)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
        const unsigned stride=wide?2:1,size=wide?840:424;
        // lpDevice 為 NULL 時依 iDevNum 列舉各顯示器的轉接輸出 \\.\DISPLAYn；指定裝置時回報其唯一的監視器。
        const auto& monitors=gui_monitors();const auto adapter=device.empty()?0:display_device_index(device);
        const auto capacity=arg(2)?memory.load(GuestAddress{arg(2)},32):0;
        if(!capacity || (d(3)&~1U)){if(!native_devices)fail(87);}
        else if(device.empty()?d(1)>=monitors.size():(!adapter || adapter>monitors.size() || d(1)))result=0;
        else if(!native_devices && capacity!=size)fail(87);
        else {const auto index=device.empty()?std::uint64_t{d(1)}+1:adapter;const auto name=u"\\\\.\\DISPLAY"+utf8_to_utf16(std::to_string(index));
            if(!native_devices){memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(size));memory.store(GuestAddress{arg(2)},32,size);}
            if(capacity>=4+32*stride)(void)copy_text(arg(2)+4,32,device.empty()?name:name+u"\\Monitor0");
            if(capacity>=4+160*stride)(void)copy_text(arg(2)+4+32*stride,128,device.empty()?u"x64arm Virtual Display":display_identity::monitor_name);
            // DISPLAY_DEVICE_ATTACHED_TO_DESKTOP＝1、PRIMARY_DEVICE＝4；監視器為 ACTIVE｜ATTACHED＝3。
            if(capacity>=8+160*stride)memory.store(GuestAddress{arg(2)+4+160*stride},32,device.empty()?(monitors[index-1].primary?5U:1U):3U);
            // 監視器身分與 SetupAPI／CM 的 devnode 共用。EDD_GET_DEVICE_INTERFACE_NAME：DeviceID 改放裝置介面路徑，可接 SetupAPI。
            const auto monitor_number=static_cast<std::uint32_t>(index);const bool interface_name=(d(3)&1U)!=0;
            if(capacity>=8+288*stride)(void)copy_text(arg(2)+8+160*stride,128,device.empty()?std::u16string(interface_name?display_identity::adapter_interface_path:display_identity::instance):
                interface_name?display_identity::monitor_interface_path(monitor_number):display_identity::monitor_device_id(monitor_number));
            if(capacity>=size)(void)copy_text(arg(2)+8+288*stride,128,device.empty()?std::u16string(u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\")+std::u16string(display_identity::class_name)+u"\\0000":display_identity::monitor_device_key(monitor_number));result=1;}
        if(native_devices)result=result?0U:0xc0000001U;
    }else if(symbol=="ChangeDisplaySettingsA" || symbol=="ChangeDisplaySettingsW" || symbol=="ChangeDisplaySettingsExA" || symbol=="ChangeDisplaySettingsExW"){
        const bool extended=symbol.starts_with("ChangeDisplaySettingsEx");const auto mode=arg(extended?1:0);const auto flags=d(extended?3:1);
        const auto native=native_gui_?native_gui_->current_display_mode():std::optional<GuiDisplayMode>{};
        const std::array<std::uint32_t,4> original{native?native->width:1440U,native?native->height:900U,32U,native?(native->refresh_numerator+native->refresh_denominator/2)/native->refresh_denominator:60U};
        auto selected=mode?gui_.display_mode.value_or(original):gui_.display_registry_mode.value_or(original);
        auto device=native_change?native_device:extended && arg(0)?(wide?read_utf16(memory,GuestAddress{arg(0)}):utf8_to_utf16(read_ansi(memory,arg(0)))):std::u16string{};
        for(auto& c:device)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
        if(job_ui_restrictions()&0x10U){last_error_=5;result=static_cast<std::uint32_t>(-1);}
        else if(extended && (arg(2) || arg(4) || (!device.empty() && device!=u"\\\\.\\display1")))result=static_cast<std::uint32_t>(-5);
        else if((flags&~0x7000031fU) || ((flags&0x10000008U) && !(flags&1U)) || (flags&0x300U)==0x300U)result=static_cast<std::uint32_t>(-4);
        else {
            if(mode){const unsigned prefix=wide?64:32,pixels=wide?168:104,total=pixels+20;
                if(memory.load(GuestAddress{mode+prefix+4},16)<total)result=static_cast<std::uint32_t>(-5);
                else {const auto fields=memory.load(GuestAddress{mode+prefix+8},32);
                    for(const auto [flag,index,offset]:std::array<std::array<unsigned,3>,4>{{{0x80000,0,4},{0x100000,1,8},{0x40000,2,0},{0x400000,3,16}}})
                        if(fields&flag)selected[index]=static_cast<std::uint32_t>(memory.load(GuestAddress{mode+pixels+offset},32));
                    if(selected[3]<=1)selected[3]=original[3];
                    if(selected[0]<320 || selected[1]<200 || selected[0]>16384 || selected[1]>16384 || (selected[2]!=16 && selected[2]!=32) || (selected[3]!=60 && selected[3]!=original[3]))result=static_cast<std::uint32_t>(-2);
                    if(((fields&0x200000U) && memory.load(GuestAddress{mode+pixels+12},32)) || ((fields&0x20U) && memory.load(GuestAddress{mode+prefix+12},64)))result=static_cast<std::uint32_t>(-2);
                    if(fields&~0x7c00a0U)result=static_cast<std::uint32_t>(-5);
                    if((fields&0x80U) && memory.load(GuestAddress{mode+prefix+20},32))result=static_cast<std::uint32_t>(-2);
                }
            }
            if(!result && !(flags&2U)){
                if(flags&1U)gui_.display_registry_mode=selected;
                if(!(flags&0x10000000U)){
                    publish_display_mode(gui_.display_mode.value_or(original),selected);
                }
            }
        }
    }else if(symbol.starts_with("EnumDisplaySettings")){
        const auto native=native_gui_?native_gui_->current_display_mode():std::optional<GuiDisplayMode>{};
        auto device=native_devices || native_modes?native_device:arg(0)?(wide?read_utf16(memory,GuestAddress{arg(0)}):utf8_to_utf16(read_ansi(memory,arg(0)))):std::u16string{};
        for(auto& c:device)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
        const auto& monitors=gui_monitors();const auto index=device.empty()?1:display_device_index(device);
        const unsigned name_bytes=wide?64:32,total=wide?220:156,pixels=wide?168:104;
        if(!arg(2) || (symbol.starts_with("EnumDisplaySettingsEx") && (d(3)&~3U)))fail(87);
        else if(!index || index>monitors.size())fail(1168);
        else {
            // 輸入的 dmSize 不檢查下限（Steel Nomad 以 dmSize=0 查詢，在 Windows 上可用）：不足時寫回到 dmICMMethod
            // 之前的固定部分（W 188、A 124 位元組）並設定 dmSize；呼叫端提供的較大結構照舊寫到其 dmSize（最多完整 DEVMODE）。
            const auto requested=static_cast<unsigned>(memory.load(GuestAddress{arg(2)+name_bytes+4},16));
            const auto supplied=native_modes?188U:requested>=total-8?std::min<unsigned>(total,requested):(wide?188U:124U);
            const auto& monitor=monitors[index-1];
            std::array<std::uint32_t,4> original{monitor.primary && native?native->width:static_cast<std::uint32_t>(monitor.rect[2]-monitor.rect[0]),monitor.primary && native?native->height:static_cast<std::uint32_t>(monitor.rect[3]-monitor.rect[1]),32U,monitor.primary && native?(native->refresh_numerator+native->refresh_denominator/2)/native->refresh_denominator:monitor.refresh};
            auto selected=original;std::vector<std::array<std::uint32_t,4>> modes{original};
            const auto add=[&](std::array<std::uint32_t,4> candidate){if(std::find(modes.begin(),modes.end(),candidate)==modes.end())modes.push_back(candidate);};
            if(monitor.primary){
                if(native_gui_)for(const auto& entry:native_gui_->display_modes())for(const auto depth:{32U,16U})add({entry.width,entry.height,depth,(entry.refresh_numerator+entry.refresh_denominator/2)/entry.refresh_denominator});
                // 常見桌面解析度由沙盒的視窗／交換鏈實際承載，不切換宿主顯示器。
                for(const auto size:std::array<std::array<unsigned,2>,18>{{{640,480},{800,600},{1024,768},{1152,864},{1280,720},{1280,800},{1280,960},{1280,1024},{1366,768},{1440,900},{1600,900},{1600,1200},{1680,1050},{1920,1080},{1920,1200},{2560,1440},{2560,1600},{3840,2160}}})
                    if(size[0]<=original[0] && size[1]<=original[1])for(const auto depth:{32U,16U})add({size[0],size[1],depth,60});
                if(d(1)==UINT32_MAX)selected=gui_.display_mode.value_or(original);
                else if(d(1)==UINT32_MAX-1)selected=gui_.display_registry_mode.value_or(original);
            }
            if(d(1)<UINT32_MAX-1){if(d(1)>=modes.size()){fail(259);arg.return_to(state,result);return std::nullopt;}selected=modes[d(1)];}
            memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(supplied));(void)copy_text(arg(2),32,u"\\\\.\\DISPLAY"+utf8_to_utf16(std::to_string(index)));
            memory.store(GuestAddress{arg(2)+name_bytes},16,0x401);memory.store(GuestAddress{arg(2)+name_bytes+2},16,1);memory.store(GuestAddress{arg(2)+name_bytes+4},16,supplied);
            memory.store(GuestAddress{arg(2)+name_bytes+8},32,0x007c0020);
            memory.store(GuestAddress{arg(2)+name_bytes+12},32,static_cast<std::uint32_t>(monitor.rect[0]));memory.store(GuestAddress{arg(2)+name_bytes+16},32,static_cast<std::uint32_t>(monitor.rect[1]));
            memory.store(GuestAddress{arg(2)+pixels-2},16,96);memory.store(GuestAddress{arg(2)+pixels},32,selected[2]);
            memory.store(GuestAddress{arg(2)+pixels+4},32,selected[0]);memory.store(GuestAddress{arg(2)+pixels+8},32,selected[1]);memory.store(GuestAddress{arg(2)+pixels+16},32,selected[3]);result=1;
        }
    }else if(symbol=="EnumDisplayMonitors"){
        const auto dc=arg(0);
        std::array<std::int32_t,2> origin{};
        std::array<std::int32_t,4> limit{INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX};
        const auto intersect=[&](const std::array<std::int32_t,4>& r){
            limit={std::max(limit[0],r[0]),std::max(limit[1],r[1]),std::min(limit[2],r[2]),std::min(limit[3],r[3])};
        };
        const auto read_rect=[&](std::uint64_t ptr){
            std::array<std::int32_t,4> r{};
            for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(memory.load(GuestAddress{ptr+i*4},32));
            return r;
        };
        if(!arg(2))fail(87);
        else {
            if(dc){
                GuestScratch scratch(*this,memory,state);
                const auto ptr=scratch.allocate(16);
                if(!nested_service(state,memory,"gdi32.dll","GetDCOrgEx",{dc,ptr})){
                    arg.return_to(state,0);return std::nullopt;
                }
                for(unsigned i=0;i<2;++i)origin[i]=static_cast<std::int32_t>(memory.load(GuestAddress{ptr+i*4},32));
                if(!nested_service(state,memory,"gdi32.dll","GetClipBox",{dc,ptr})){
                    arg.return_to(state,0);return std::nullopt;
                }
                limit=read_rect(ptr);
            }
            memory.check(GuestAddress{arg(2)},1,Access::Execute);
            if(arg(1))intersect(read_rect(arg(1)));
            std::vector<std::pair<std::uint64_t,std::array<std::int32_t,4>>> visible;
            const auto& monitors=gui_monitors();
            for(std::size_t i=0;i<monitors.size();++i){
                // 在 DC 座標求交集；先擴為 64 位元，負座標及 LONG 邊界不會溢位。
                std::array<std::int64_t,4> rect{};
                for(unsigned j=0;j<4;++j){
                    const auto coord=static_cast<std::int64_t>(monitors[i].rect[j])-origin[j%2];
                    rect[j]=j<2?std::max<std::int64_t>(coord,limit[j]):std::min<std::int64_t>(coord,limit[j]);
                }
                if(rect[0]<rect[2] && rect[1]<rect[3])visible.emplace_back(i+1,std::array<std::int32_t,4>{
                    static_cast<int>(rect[0]),static_cast<int>(rect[1]),static_cast<int>(rect[2]),static_cast<int>(rect[3])});
            }
            if(visible.empty())result=1;
            else {const auto rects=allocate_heap(memory,16*visible.size());
                if(!rects){fail(8);arg.return_to(state,0);return std::nullopt;}
                std::vector<GuestCallback> calls;
                for(std::size_t j=0;j<visible.size();++j){
                    for(unsigned i=0;i<4;++i)memory.store(GuestAddress{rects+j*16+i*4},32,static_cast<std::uint32_t>(visible[j].second[i]));
                    calls.push_back({arg(2),{visible[j].first,dc,rects+j*16,arg(3)}});
                }
                try{start_callbacks(state,memory,std::move(calls),Continuation::MonitorEnumeration,rects);}
                catch(...){(void)free_heap(memory,rects,state);throw;}return std::nullopt;}
        }
    }else if(symbol.starts_with("MonitorFrom")){
        const auto flags=d(1);
        if(flags>2)fail(87);
        else if(symbol=="MonitorFromWindow"){
            // 最小化視窗依還原位置判斷；其他程序的視窗查共用視窗表，找不到時依旗標回主螢幕或 NULL。
            std::optional<std::array<std::int32_t,4>> rect;
            if(arg(0)==gui_.desktop)rect=gui_screen_area();
            else if(const auto found=gui_.windows.find(arg(0));found!=gui_.windows.end())rect=gui_window_screen_rect(found->second);
            else if(const auto shared=shared_gui_window(arg(0))){
                rect=std::array<std::int32_t,4>{shared->x,shared->y,shared->x+static_cast<std::int32_t>(shared->outer_width),shared->y+static_cast<std::int32_t>(shared->outer_height)};
            }
            result=rect?gui_monitor_from_rect(*rect,flags):flags?1:0;
        }
        else if(symbol=="MonitorFromPoint"){const auto x=static_cast<std::int32_t>(arg(0)),y=static_cast<std::int32_t>(arg(0)>>32);result=gui_monitor_from_rect({x,y,x,y},flags);}
        else {std::array<std::int32_t,4> rect{};for(unsigned i=0;i<4;++i)rect[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(0)+4*i},32));result=gui_monitor_from_rect(rect,flags);}
    }else if(symbol.starts_with("GetMonitorInfo")){
        const auto& monitors=gui_monitors();
        const auto size=memory.load(GuestAddress{arg(1)},32);if(!arg(0) || arg(0)>monitors.size())fail(1461);else if(size!=40 && size!=(wide?104U:72U))fail(87);
        else {memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(size),Access::Write);const auto& monitor=monitors[arg(0)-1];
            // 主螢幕工作區沿用 AppBar 調整後的結果；其他螢幕使用原生 visibleFrame。
            const auto work=monitor.primary?gui_work_area():monitor.work;
            for(unsigned i=0;i<4;++i){memory.store(GuestAddress{arg(1)+4+i*4},32,static_cast<std::uint32_t>(monitor.rect[i]));memory.store(GuestAddress{arg(1)+20+i*4},32,static_cast<std::uint32_t>(work[i]));}
            memory.store(GuestAddress{arg(1)+36},32,monitor.primary?1U:0U);
            if(size>40)(void)copy_text(arg(1)+40,32,u"\\\\.\\DISPLAY"+utf8_to_utf16(std::to_string(arg(0))));result=1;}
    }else if(service.family=="shlwapi.dll" && symbol=="#537"){
        const auto action=d(0);
        const auto data=arg(1);
        const auto spi=[&](std::uint32_t size,std::uint64_t parameter){
            return nested_service(state,memory,"user32.dll","SystemParametersInfoW",{action,size,parameter,0});
        };
        if(!data)fail(87);
        else if(action==0x48 || action==0x49){
            GuestScratch scratch(*this,memory,state);
            const auto info=scratch.allocate(8);
            memory.store(GuestAddress{info},32,8);
            if(action==0x49)memory.store(GuestAddress{info+4},32,memory.load(GuestAddress{data},32)!=0);
            result=spi(8,info);
            if(result && action==0x48)memory.store(GuestAddress{data},32,memory.load(GuestAddress{info+4},32));
        }else if(action==0x25 || action==0x4b)result=spi(memory.load(GuestAddress{data},32)!=0,0);
        else if(action==0x26 || action==0x4a || (action>=0x1000 && !(action&1U)))result=spi(0,data);
        else if(action>=0x1000)result=spi(0,memory.load(GuestAddress{data},32));
        else fail(87);
    }else if(symbol.starts_with("SystemParametersInfo")){
        // 與 GetSystemMetrics、AppBar 共用桌面／工作區；未支援的 action 回傳明確錯誤。
        const auto font=[&](std::uint64_t pointer){memory.write(GuestAddress{pointer},std::vector<std::uint8_t>(wide?92U:60U));memory.store(GuestAddress{pointer},32,static_cast<std::uint32_t>(-12));memory.store(GuestAddress{pointer+16},32,400);memory.store(GuestAddress{pointer+23},8,1);memory.store(GuestAddress{pointer+24},8,4);memory.store(GuestAddress{pointer+26},8,5);(void)copy_text(pointer+28,32,runtime_.network.external_io?native_ui_font_family():u"Segoe UI");};
        if(d(0)==41){const auto size=memory.load(GuestAddress{arg(2)},32);const unsigned old_size=wide?500U:340U,lf=wide?92U:60U;
            if(size!=old_size && size!=old_size+4)fail(87);else {memory.write(GuestAddress{arg(2)+4},std::vector<std::uint8_t>(static_cast<std::size_t>(size-4)));
                for(unsigned offset:{4U,8U,12U,16U,20U})memory.store(GuestAddress{arg(2)+offset},32,offset==4?1:offset==20?24:16);
                font(arg(2)+24);memory.store(GuestAddress{arg(2)+24+lf},32,16);memory.store(GuestAddress{arg(2)+28+lf},32,20);font(arg(2)+32+lf);
                memory.store(GuestAddress{arg(2)+32+lf*2},32,16);memory.store(GuestAddress{arg(2)+36+lf*2},32,20);
                font(arg(2)+40+lf*2);font(arg(2)+40+lf*3);font(arg(2)+40+lf*4);result=1;}
        }else if(d(0)==0x2b || d(0)==0x2c){
            if(!arg(2) || memory.load(GuestAddress{arg(2)},32)!=20)fail(87);
            else if(d(0)==0x2b){
                memory.check(GuestAddress{arg(2)},20,Access::Write);
                for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(2)+4+i*4},32,gui_.minimized_metrics[i]);
                result=1;
            }else if(job_ui_restrictions()&8U)fail(5);
            else if(d(3))fail(d(3)&~3U?87:50);
            else{
                auto settings=gui_.minimized_metrics;
                for(unsigned i=0;i<4;++i){
                    const auto raw=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+4+i*4},32));
                    settings[i]=i==3?raw&15U:raw<=INT32_MAX?raw:0U;
                }
                gui_.minimized_metrics=settings;result=1;
            }
        }else if(d(0)==0x2d){
            const unsigned size=wide?108U:76U;
            if(!arg(2) || memory.load(GuestAddress{arg(2)},32)!=size)fail(87);
            else{
                memory.check(GuestAddress{arg(2)},size,Access::Write);
                memory.store(GuestAddress{arg(2)+4},32,system_metric(memory,38));
                memory.store(GuestAddress{arg(2)+8},32,system_metric(memory,39));
                memory.store(GuestAddress{arg(2)+12},32,1);
                font(arg(2)+16);result=1;
            }
        }else if(d(0)==0x25 || d(0)==0x26 || d(0)==0x48 || d(0)==0x49){
            const bool animation=d(0)>=0x48;
            const bool set=d(0)==0x25 || d(0)==0x49;
            auto& setting=animation?gui_.minimize_animation:gui_.drag_full_windows;
            if((animation || !set) && !arg(2))fail(87);
            else if(animation && memory.load(GuestAddress{arg(2)},32)!=8)fail(87);
            else if(set && (job_ui_restrictions()&8U))fail(5);
            else if(set && d(3))fail(d(3)&~3U?87:50);
            else if(set){setting=animation?memory.load(GuestAddress{arg(2)+4},32)!=0:d(1)!=0;result=1;}
            else {memory.store(GuestAddress{arg(2)+(animation?4:0)},32,setting);result=1;}
        }else if(d(0)==0x20){ // SPI_SETDOUBLECLICKTIME
            if(job_ui_restrictions()&8U)fail(5);
            else if(d(3))fail(d(3)&~3U?87:50);
            else {set_gui_timing(memory,0,d(1));result=1;}
        }else if(d(0)==0x21 || d(0)==0x57){
            // SPI_SETMOUSEBUTTONSWAP 與 SwapMouseButton 共用左右鍵對調狀態；SPI_SETCURSORS 還原被 SetSystemCursor 取代的系統游標。
            if(d(0)==0x21)(void)exchange_mouse_buttons(memory,d(1)!=0);else {for(const auto& [id,icon]:gui_.system_cursors)release_cursor_frames(0xd000U+id);gui_.system_cursors.clear();present_gui_cursor();}
            result=1;
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
        else if((d(0)>=0x1000 && d(0)<=0x1027 && (d(0)&~1U)!=0x1010) ||
                (d(0)>=0x103e && d(0)<=0x1043) || (d(0)>=0x104a && d(0)<=0x1051)){
            const auto bit=UINT64_C(1)<<((d(0)-0x1000)/2);
            if(!(d(0)&1U)){
                if(!arg(2))fail(87);else {memory.store(GuestAddress{arg(2)},32,(gui_.ui_preferences&bit)!=0);result=1;}
            }else if(job_ui_restrictions()&8U)fail(5);
            else if(d(3))fail(d(3)&~3U?87:50);
            else {if(arg(2))gui_.ui_preferences|=bit;else gui_.ui_preferences&=~bit;result=1;}
        }
        else fail(87);
    }else throw HostCallFault("未實作的 GUI resource API");
    arg.return_to(state,result);return std::nullopt;
}
}
