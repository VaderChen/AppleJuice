#include "gui_timing_layout.h"
#include "mapi_property_tag.h"
#include "x64arm/security_descriptor.hpp"
#include "shell_dispatch_layout.h"
#include "shell_window_interfaces.h"
#include "windows_path_character.h"
#include "urlmon_uri_parse.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>

// shlwapi 雜項家族：HLS 色彩（ColorRGBToHLS／ColorHLSToRGB／ColorAdjustLuma）、GetMenuPosFromID、GetAcceptLanguages、
// 跨程序共用記憶體區塊（SHAllocShared／SHLockShared／SHUnlockShared／SHFreeShared）、WhichPlatform、SHIsLowMemoryMachine、IsInternetESCEnabled。
// COM 輔助：IStream_Read／Write／Reset／Size、IUnknown_AtomicRelease／Set／GetSite／SetSite／GetWindow、ConnectToConnectionPoint、
// 執行緒參照（SHSetThreadRef／SHGetThreadRef／SHReleaseThreadRef／SHCreateThreadRef）、Explorer 程序參照、SHIsChildOrSelf、SHSendMessageBroadcast。
// 全部在宿主端計算或組合既有 kernel32／user32／advapi32 服務；呼叫客體 COM 方法或傳送訊息者經 nested_continuation 續行。
// 字串一律以 UTF-16 處理，A 版依 ACP（UTF-8）轉換。
namespace x64arm {
namespace {
// HLS 範圍 0～240（HLSMAX），RGB 0～255；色相未定義（灰階）時為 HLSMAX*2/3。
constexpr int hls_max=240,rgb_max=255,undefined_hue=hls_max*2/3;
struct Hls {int hue{},luminance{},saturation{};};
Hls rgb_to_hls(std::uint32_t color){
    const int r=static_cast<int>(color&0xffU),g=static_cast<int>((color>>8)&0xffU),b=static_cast<int>((color>>16)&0xffU);
    const int high=std::max({r,g,b}),low=std::min({r,g,b});Hls out;
    out.luminance=((high+low)*hls_max+rgb_max)/(2*rgb_max);
    if(high==low){out.hue=undefined_hue;return out;}
    const int range=high-low,sum=high+low;
    out.saturation=out.luminance<=hls_max/2?(range*hls_max+sum/2)/sum:(range*hls_max+(2*rgb_max-sum)/2)/(2*rgb_max-sum);
    const auto delta=[&](int c){return ((high-c)*(hls_max/6)+range/2)/range;};
    const int dr=delta(r),dg=delta(g),db=delta(b);
    out.hue=r==high?db-dg:g==high?hls_max/3+dr-db:2*hls_max/3+dg-dr;
    if(out.hue<0)out.hue+=hls_max;if(out.hue>hls_max)out.hue-=hls_max;return out;
}
int hue_to_rgb(int n1,int n2,int hue){
    if(hue<0)hue+=hls_max;if(hue>hls_max)hue-=hls_max;
    if(hue<hls_max/6)return n1+((n2-n1)*hue+hls_max/12)/(hls_max/6);
    if(hue<hls_max/2)return n2;
    if(hue<hls_max*2/3)return n1+((n2-n1)*(hls_max*2/3-hue)+hls_max/12)/(hls_max/6);
    return n1;
}
using Iid=std::array<std::uint8_t,16>;
constexpr Iid iid_unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Iid iid_object_with_site{0xa3,0x01,0x48,0xfc,0xa9,0x2b,0xcf,0x11,0xa2,0x29,0x00,0xaa,0x00,0x3d,0x73,0x52};
constexpr Iid iid_security_manager{0xee,0xc9,0xea,0x79,0xf9,0xba,0xce,0x11,0x8c,0x82,0x00,0xaa,0x00,0x4b,0xa9,0x0b};
constexpr Iid iid_command_target{0xcb,0xbc,0x22,0xb7,0x68,0x4e,0x1b,0x10,0xa2,0xbc,0,0xaa,0,0x40,0x47,0x70};
constexpr Iid iid_service_provider{0xc1,0x40,0x51,0x6d,0x36,0x74,0xce,0x11,0x80,0x34,0,0xaa,0,0x60,9,0xfa};
constexpr Iid iid_control_site{0x89,0xb2,0x96,0xb1,0xb4,0xba,0x1a,0x10,0xb6,0x9c,0x00,0xaa,0x00,0x34,0x1d,0x07};
constexpr Iid iid_input_object{0xaa,0x4f,0x28,0x68,0x48,0x6a,0xd0,0x11,0x8c,0x78,0x00,0xc0,0x4f,0xd9,0x18,0xb4};
constexpr Iid iid_input_site{0x92,0x83,0xdb,0xf1,0x31,0x73,0xd0,0x11,0x8c,0x99,0x00,0xa0,0xc9,0x2d,0xbf,0xe8};
constexpr Iid iid_point_container{0x84,0xb2,0x96,0xb1,0xb4,0xba,0x1a,0x10,0xb6,0x9c,0x00,0xaa,0x00,0x34,0x1d,0x07};
// 客體串列以 DWORD 對齊；未對齊的區塊包在保留簽章的外層，串流只含內層。
struct ShellDataBlock {
    std::uint64_t address, payload;
    std::uint32_t span, length, signature;
};

ShellDataBlock shell_data_block(const GuestMemory& memory,std::uint64_t address){
    const auto load=[&](std::uint64_t at){return static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));};
    const auto span=address?load(address):0U;
    if(!span)return {address,address,0,0,0};
    if(span<8 || (span&3))throw std::invalid_argument("資料區塊長度錯誤");
    memory.check(GuestAddress{address},span,Access::Read);
    const auto outer=load(address+4);
    if(outer!=UINT32_MAX)return {address,address,span,span,outer};
    if(span<16)throw std::invalid_argument("資料區塊容器過短");
    const auto length=load(address+8);
    if(length<8 || length>span-8)throw std::invalid_argument("資料區塊超出容器");
    return {address,address+8,span,length,load(address+12)};
}
std::uint32_t hls_to_rgb(int hue,int luminance,int saturation){
    const auto pack=[](int r,int g,int b){const auto clamp=[](int v){return static_cast<std::uint32_t>(std::clamp(v,0,rgb_max));};return clamp(r)|(clamp(g)<<8)|(clamp(b)<<16);};
    if(!saturation){const int gray=luminance*rgb_max/hls_max;return pack(gray,gray,gray);}
    const int high=luminance<=hls_max/2?(luminance*(hls_max+saturation)+hls_max/2)/hls_max:luminance+saturation-(luminance*saturation+hls_max/2)/hls_max;
    const int low=2*luminance-high;
    const auto channel=[&](int h){return (hue_to_rgb(low,high,h)*rgb_max+hls_max/2)/hls_max;};
    return pack(channel(hue+hls_max/3),channel(hue),channel(hue-hls_max/3));
}
}
std::span<const WindowsServiceDescriptor> shlwapi_misc_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shlwapi.dll","#491",3,0,0,5},{"shlwapi.dll","#516",6,0,0,1},
        {"shlwapi.dll","#517",6,0,0,0x29},{"shlwapi.dll","#518",3,0,0,1},{"shlwapi.dll","#519",6,0,0,1},
        {"shlwapi.dll","#160",2,0,0,2},{"shlwapi.dll","#161",2,0,0,2},
        {"shlwapi.dll","#356",3},{"shlwapi.dll","#475",2,0,0,2},
        {"shlwapi.dll","#17",2},{"shlwapi.dll","#18",2},{"shlwapi.dll","#19",1},
        {"shlwapi.dll","#20",2},{"shlwapi.dll","#21",2,0,0,2},{"shlwapi.dll","#22",2,0,0,2},
        {"shlwapi.dll","#236",1},{"shlwapi.dll","#343",5},{"shlwapi.dll","#344",5},
        {"shlwapi.dll","#178",5},{"internal-message-propagation","EnumProc",2},{"shlwapi.dll","#194",2},{"shlwapi.dll","#355",2,0,0,2},
        {"shlwapi.dll","#163",5},{"shlwapi.dll","#164",6},{"shlwapi.dll","#484",7},
        {"shlwapi.dll","#188",3},{"shlwapi.dll","#189",2},{"shlwapi.dll","#478",2},
        {"shlwapi.dll","#479",3},{"shlwapi.dll","#481",1},{"shlwapi.dll","#509",3},
        {"shlwapi.dll","#542",0},
        {"shlwapi.dll","#35",3,0,0,0x2},
        {"shlwapi.dll","ColorRGBToHLS",4,0,0,0x1},{"shlwapi.dll","ColorHLSToRGB",3,0,0,0x7},{"shlwapi.dll","ColorAdjustLuma",3,0,0,0x7},
        {"internal-shell-persist","IUnknown_GetClassID",2},
        {"internal-shell-owner","IUnknown_SetOwner",2},
        {"internal-shell-identity","SHIsSameObject",2},
        {"internal-shell-slashes","PathSkipLeadingSlashesA",1},
        {"internal-shell-slashes","FixSlashesAndColonA",1},{"internal-shell-slashes","FixSlashesAndColonW",1},
        {"internal-shell-fdsa","FDSA_Initialize",5,0,0,0x13},{"internal-shell-fdsa","FDSA_Destroy",1},
        {"internal-shell-fdsa","FDSA_InsertItem",3,0,0,0x2},{"internal-shell-fdsa","FDSA_DeleteItem",2,0,0,0x2},
        {"internal-shell-dispatch","SHPackDispParamsV",4,0,0,4},
        {"internal-shell-dispatch","SHPackDispParams",3,0,0,4,true},
        {"internal-shell-map","SHSearchMapInt",4,0,0,0xc},
        {"shlwapi.dll","#222",1},{"shlwapi.dll","#223",1},{"shlwapi.dll","#224",1},
        {"shlwapi.dll","#422",2,0,0,2},{"shlwapi.dll","#423",2,0,0,2},{"shlwapi.dll","#424",1},
        {"shlwapi.dll","SHGlobalCounterGetValue",1,0,0,1},{"shlwapi.dll","SHGlobalCounterIncrement",1,0,0,1},
        {"shlwapi.dll","SHGlobalCounterDecrement",1,0,0,1},{"internal-shell-counter","SHGlobalCounterSetValue",2,0,0,3},
        {"shlwapi.dll","#193",0},{"shlwapi.dll","#197",3,0,0,0x4},
        {"shlwapi.dll","#177",2},{"shlwapi.dll","#180",1},
        {"shlwapi.dll","#181",3,0,0,0x6},{"shlwapi.dll","#182",3,0,0,0x6},{"shlwapi.dll","#192",2,0,0,0x2},
        {"shlwapi.dll","GetMenuPosFromID",2,0,0,0x2},{"shlwapi.dll","GetAcceptLanguagesA",2},{"shlwapi.dll","GetAcceptLanguagesW",2},
        {"internal-shell-shared","SHMapHandle",5,0,0,0x1e},{"internal-shell-shared","SHLockSharedEx",3,0,0,0x6},
        {"shlwapi.dll","SHAllocShared",3,0,0,0x6},{"shlwapi.dll","SHLockShared",2,0,0,0x2},{"shlwapi.dll","SHUnlockShared",1},{"shlwapi.dll","SHFreeShared",2,0,0,0x2},
        {"shlwapi.dll","WhichPlatform",0},{"shlwapi.dll","SHIsLowMemoryMachine",1,0,0,0x1},{"shlwapi.dll","IsInternetESCEnabled",0},
        {"shlwapi.dll","#166",1},{"shlwapi.dll","IStream_Read",3,0,0,0x4},{"shlwapi.dll","IStream_Write",3,0,0,0x4},{"shlwapi.dll","IStream_Reset",1},{"shlwapi.dll","IStream_Size",2},
        {"shlwapi.dll","#267",4},{"shlwapi.dll","#268",2},
        {"internal-mapi-table","HrAddColumns",4},{"internal-mapi-table","HrAddColumnsEx",5},
        {"internal-mapi-table","HrQueryAllRows",6,0,0,16},
        {"internal-mapi-one","HrGetOneProp",3,0,0,2},{"internal-mapi-one","HrSetOneProp",2},{"internal-mapi-one","FPropExists",2,0,0,2},
        {"internal-mapi-reference","UlAddRef",1},{"internal-mapi-reference","UlRelease",1},
        {"shlwapi.dll","IUnknown_AtomicRelease",1},{"shlwapi.dll","IUnknown_Set",2},{"shlwapi.dll","IUnknown_GetSite",3},{"shlwapi.dll","IUnknown_SetSite",2},{"shlwapi.dll","IUnknown_GetWindow",2},
        {"shlwapi.dll","ConnectToConnectionPoint",6,0,0,0x4},
        {"shlwapi.dll","SHSetThreadRef",1},{"shlwapi.dll","SHGetThreadRef",1},{"shlwapi.dll","SHReleaseThreadRef",0},{"shlwapi.dll","SHCreateThreadRef",2},
        {"shell32.dll","SHSetInstanceExplorer",1},{"shell32.dll","SHGetInstanceExplorer",1},{"shlwapi.dll","_SHGetInstanceExplorer",1},
        {"shlwapi.dll","SHIsChildOrSelf",2},{"shlwapi.dll","SHSendMessageBroadcastA",3,0,0,0x1},{"shlwapi.dll","SHSendMessageBroadcastW",3,0,0,0x1},
        {"internal-thread-ref","QueryInterface",3},{"internal-thread-ref","AddRef",1},{"internal-thread-ref","Release",1},
        {"shlwapi.dll","SHCreateThread",4,0,0,0x4},{"internal-shell-thread","Start",1},
        {"shlwapi.dll","SHFormatDateTimeA",4,0,0,0x8},{"shlwapi.dll","SHFormatDateTimeW",4,0,0,0x8},
        {"shlwapi.dll","SHMessageBoxCheckA",6,0,0,0x18},{"shlwapi.dll","SHMessageBoxCheckW",6,0,0,0x18},
        {"shlwapi.dll","ShellMessageBoxA",5,0,0,0x10,true},{"shlwapi.dll","ShellMessageBoxW",5,0,0,0x10,true},
        {"shlwapi.dll","SHCreateShellPalette",1},{"shlwapi.dll","SHGetInverseCMAP",2,0,0,0x2},{"shlwapi.dll","SHSkipJunction",2},{"shlwapi.dll","UrlFixupW",3,0,0,0x4},
        {"internal-shell-dialog","SHDialogBox",5},{"internal-shell-dialog","DialogProc",4},
        {"internal-worker-window","SHDefWindowProc",4},
        {"internal-worker-window","SHCreateWorkerWindowA",6},{"internal-worker-window","SHCreateWorkerWindowW",6},
        {"internal-shell-window","SHSetWindowBits",4},{"internal-shell-window","SHSetParentHwnd",2},
        {"internal-shell-window","SHRegisterClassA",1},{"internal-shell-window","SHRegisterClassW",1},
        {"internal-shell-window","SHUnregisterClassesA",3},{"internal-shell-window","SHUnregisterClassesW",3},
        {"internal-dialog-font","SHSetDefaultDialogFont",2,0,0,0x2},
        {"internal-dialog-font","SHRemoveDefaultDialogFont",1},
        {"shlwapi.dll","#429",1},{"shlwapi.dll","#430",2,0,0,2},{"shlwapi.dll","#431",1},
        {"shlwapi.dll","#405",6,0,0,0x24},{"shlwapi.dll","#406",6,0,0,0x24},{"shlwapi.dll","MLFreeLibrary",1},
        {"shlwapi.dll","MLLoadLibraryA",3,0,0,0x4},{"shlwapi.dll","MLLoadLibraryW",3,0,0,0x4},
        // 雜湊與路徑字元（windows_shlwapi_hash.inc）。
        {"shlwapi.dll","HashData",4,0,0,0xa},{"shlwapi.dll","UrlHashA",3,0,0,0x4},{"shlwapi.dll","UrlHashW",3,0,0,0x4},
        {"shlwapi.dll","PathIsValidCharA",2,0,0,0x3},{"shlwapi.dll","PathIsValidCharW",2,0,0,0x3},{"shlwapi.dll","SHTruncateString",2,0,0,0x2},
        {"shlwapi.dll","SHOpenRegStreamA",4,0,0,0x8},{"shlwapi.dll","SHOpenRegStreamW",4,0,0,0x8},{"shlwapi.dll","SHOpenRegStream2A",4,0,0,0x8},{"shlwapi.dll","SHOpenRegStream2W",4,0,0,0x8},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_shlwapi_misc(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto handle=[&](std::uint64_t value){return x86_?value&UINT32_MAX:value;};
    const auto host=[&](std::string_view dll,std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,dll,symbol,args);};
    const auto pointer_bits=static_cast<std::uint8_t>(x86_?32:64);
    if(service.family=="internal-mapi-reference"){
        const auto object=arg(0);
        if(!object)return done(0);
        // 包裝函式與 COM 方法均只有 this，引數及呼叫端返回位址可直接沿用。
        const auto table=memory.load(GuestAddress{object},pointer_bits);
        const unsigned slot=name=="UlAddRef"?1:2;
        state.rip=GuestAddress{memory.load(GuestAddress{table+slot*(pointer_bits/8)},pointer_bits)};
        return std::nullopt;
    }
    if(name=="#491"){
        static constexpr std::u16string_view bases[]{
            u"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer",u"Software\\Microsoft\\Windows\\Shell",
            u"Software\\Microsoft\\Windows\\ShellNoRoam",u"Software\\Classes"};
        static constexpr std::u16string_view groups[]{
            u"",u"LocalizedResourceName",u"Handlers",u"Associations",u"Volatile",u"MUICache",u"FileExts"};
        const auto flags=u32(0)==0x1ffff?0x21U:u32(0);
        const auto base=(flags>>4)&0xff,group=(flags>>12)&0xff;
        if(base>=std::size(bases) || group>=std::size(groups))return done(0);
        std::u16string path{bases[base]};
        if(group){path+=u'\\';path+=groups[group];}
        if(arg(1)){path+=u'\\';path+=read_utf16(memory,GuestAddress{arg(1)},32768);}
        GuestScratch scratch(*this,memory,state);
        const auto text=scratch.text(path),slot=scratch.allocate(8);
        if(!text || !slot)return done(0);
        const auto root=handle((flags&15U)==2?UINT64_C(0xffffffff80000002):UINT64_C(0xffffffff80000001));
        const auto status=u32(2)?host("advapi32.dll","RegCreateKeyExW",{root,text,0,0,0,0x02000000,0,slot,0}):
            host("advapi32.dll","RegOpenKeyExW",{root,text,0,0x02000000,slot});
        return done(status?0:memory.load(GuestAddress{slot},pointer_bits));
    }
    if(name=="#516" || name=="#517" || name=="#518" || name=="#519"){
        const auto key=handle(host("shlwapi.dll","#491",{u32(0),arg(1),name=="#517"}));
        if(!key)return done(0x80070002U);
        const auto close=[&]{(void)host("advapi32.dll","RegCloseKey",{key});};
        std::uint64_t error=0;
        try{
            if(name=="#517")error=host("advapi32.dll","RegSetValueExW",{key,arg(2),0,u32(3),arg(4),u32(5)});
            else if(name=="#518")error=host("advapi32.dll","RegDeleteValueW",{key,arg(2)});
            else if(name=="#516")error=host("shlwapi.dll","SHQueryValueExW",{key,arg(2),0,arg(3),arg(4),arg(5)});
            else{
                GuestScratch scratch(*this,memory,state);
                const auto size=scratch.allocate(4);
                const auto query=[&](std::uint64_t data){
                    return host("shlwapi.dll","SHQueryValueExW",{key,arg(2),0,arg(3),data,size});
                };
                error=size?query(0):14;
                if(!error){
                    memory.check(GuestAddress{arg(4)},pointer_bits/8,Access::Write);
                    const auto count=memory.load(GuestAddress{size},32);
                    const auto data=count>UINT32_MAX-2?0:handle(host("kernel32.dll","LocalAlloc",{0,count+2}));
                    memory.store(GuestAddress{arg(4)},pointer_bits,data);
                    if(!data)error=14;
                    else{
                        memory.store(GuestAddress{size},32,count+2);
                        error=query(data);
                        if(arg(5))memory.store(GuestAddress{arg(5)},32,memory.load(GuestAddress{size},32));
                    }
                }
            }
        }catch(...){close();throw;}
        close();
        return done(error?0x80070000U|(error&0xffffU):0U);
    }
    if(name=="#160" || name=="#161"){
        const auto dest=arg(0);
        if(!dest)return done(0);
        const auto capacity=u32(1);
        const bool wide=name=="#161";
        if(wide)memory.store(GuestAddress{dest},16,0);

        GuestScratch scratch(*this,memory,state);
        const auto machine=x86_?UINT64_C(0x80000002):UINT64_C(0xffffffff80000002);
        const auto nt=scratch.text(u"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion");
        const auto legacy=scratch.text(u"SOFTWARE\\Microsoft\\Windows\\CurrentVersion");
        const auto ie=scratch.text(u"SOFTWARE\\Microsoft\\Internet Explorer");
        const auto registration=scratch.text(u"SOFTWARE\\Microsoft\\Internet Explorer\\Registration");
        const auto slot=scratch.allocate(8),size=scratch.allocate(4),buffer=scratch.allocate(2050);
        if(!nt || !legacy || !ie || !registration || !slot || !size || !buffer)return done(0);
        const auto open=[&](std::uint64_t path){
            return host("advapi32.dll","RegOpenKeyExW",{machine,path,0,0x20019,slot})==0;
        };
        if(!open(nt) && !open(legacy))return done(0);
        const auto current=memory.load(GuestAddress{slot},pointer_bits);
        const auto field=[&](std::uint64_t root,std::uint64_t path,std::u16string_view label,
                             unsigned bytes)->std::optional<std::u16string>{
            const auto value=scratch.text(label);
            if(!value)return {};
            memory.store(GuestAddress{size},32,bytes);
            if(host("shlwapi.dll","SHGetValueW",{root,path,value,0,buffer,size}))return {};
            const auto length=memory.load(GuestAddress{size},32)/2;
            memory.store(GuestAddress{buffer+2*length},16,0);
            return read_utf16(memory,GuestAddress{buffer},length+1);
        };
        std::u16string result;
        if(auto version=field(machine,ie,u"Version",30)){
            result=*version;
            result+=field(machine,ie,u"CustomizedVersion",30-static_cast<unsigned>(version->size())).value_or(u"");
        }
        const auto append=[&](std::uint64_t root,std::uint64_t path,std::u16string_view label,unsigned bytes){
            result+=u'~';
            if(auto value=field(root,path,label,bytes))result+=*value;
        };
        append(current,0,u"RegisteredOwner",256);
        append(current,0,u"RegisteredOrganization",256);
        result+=u"~0";
        append(machine,registration,u"ProductId",256);
        append(machine,legacy,u"IEAKUpdateUrl",2048);
        append(current,0,u"IEAKHelpString",256);
        (void)host("advapi32.dll","RegCloseKey",{current});
        if(capacity){
            if(result.size()>=capacity)result.resize(capacity-1);
            if(wide)memory.write(GuestAddress{dest},utf16_bytes(result));
            else{
                const auto source=scratch.text(result);
                if(!source)return done(0);
                (void)host("kernel32.dll","WideCharToMultiByte",{0,0,source,UINT32_MAX,dest,capacity,0,0});
            }
        }
        return done(1);
    }
    if(service.family=="internal-shell-dispatch"){
        const auto params=arg(0),args=arg(1);
        const unsigned unit=pointer_bits/8;
        const auto count=u32(2);
        const bool indirect=name=="SHPackDispParamsV";
        auto cursor=indirect?arg(3):0;
        unsigned index=3;
        const auto next=[&](unsigned bits){
            if(!indirect)return arg(index++);
            const auto value=memory.load(GuestAddress{cursor},static_cast<std::uint8_t>(bits));
            cursor+=unit;
            return value;
        };
        memory.store(GuestAddress{params},pointer_bits,args);
        memory.store(GuestAddress{params+unit},pointer_bits,0);
        memory.store(GuestAddress{params+2*unit},32,count);
        memory.store(GuestAddress{params+2*unit+4},32,0);
        for(auto remaining=count;remaining;--remaining){
            const auto item=args+std::uint64_t{remaining-1}*(8+2*unit);
            const auto type=shell_dispatch_type(static_cast<std::uint16_t>(next(32)));
            memory.store(GuestAddress{item},16,type);
            const auto width=shell_dispatch_width(type,pointer_bits);
            memory.store(GuestAddress{item+8},static_cast<std::uint8_t>(width),next(std::max(width,32U)));
        }
        return done(0);
    }
    if(name=="#429")return done(runtime_.multilingual_modules.contains(handle(arg(0))));
    if(name=="#430"){
        const auto module=handle(arg(0));
        if(!module)return done(0x8007000eU);
        try{runtime_.multilingual_modules.emplace(module,static_cast<std::uint16_t>(arg(1)));}
        catch(const std::bad_alloc&){return done(0x8007000eU);}
        return done(0);
    }
    if(name=="#431"){
        const auto found=runtime_.multilingual_modules.find(handle(arg(0)));
        if(found!=runtime_.multilingual_modules.end())runtime_.multilingual_modules.erase(found);
        return done(0);
    }
#include "windows_shell_security.inc"
#include "windows_shlwapi_counter.inc"
#include "windows_shlwapi_fdsa.inc"
#include "windows_shlwapi_hash.inc"
    if(name=="#193"){ // SHGetCurColorRes
        const auto dc=handle(host("user32.dll","GetDC",{0}));
        const auto depth=static_cast<std::uint32_t>(host("gdi32.dll","GetDeviceCaps",{dc,12}));
        const auto planes=static_cast<std::uint32_t>(host("gdi32.dll","GetDeviceCaps",{dc,14}));
        (void)host("user32.dll","ReleaseDC",{0,dc});
        return done(depth*planes);
    }
    if(name=="#197"){ // SHFillRectClr 透過 ETO_OPAQUE 套用 DC 的座標與裁切。
        const auto dc=arg(0), rect=arg(1);
        const auto previous=host("gdi32.dll","SetBkColor",{dc,u32(2)});
        (void)host("gdi32.dll","ExtTextOutA",{dc,0,0,2,rect,0,0,0});
        (void)host("gdi32.dll","SetBkColor",{dc,previous});
        return done(0);
    }
    if(name=="#19")return done(arg(0)?host("kernel32.dll","LocalFree",{arg(0)}):0);
    if(name=="#20" || name=="#21" || name=="#22"){
        const bool add=name=="#20",find=name=="#22";
        if(!arg(0) || (add && !arg(1)))return done(0);
        const auto list=find?arg(0):memory.load(GuestAddress{arg(0)},pointer_bits);
        if(!find)memory.check(GuestAddress{arg(0)},pointer_bits/8,Access::Write);
        auto cursor=list;
        ShellDataBlock removed{};
        for(;;){
            const auto block=shell_data_block(memory,cursor);
            if(!block.span)break;
            if(!add && !removed.span){
                const auto match=block.payload!=cursor && u32(1)==UINT32_MAX?cursor:block.signature==u32(1)?block.payload:0;
                if(match){
                    if(find)return done(match);
                    removed=block;
                }
            }
            cursor+=block.span;
        }
        const auto used=cursor-list;
        if(add){
            const auto item=arg(1),length=memory.load(GuestAddress{item},32);
            if(length<8 || memory.load(GuestAddress{item+4},32)==UINT32_MAX)return done(0);
            const auto span=(length&3)?((length+3)&~UINT64_C(3))+8:length;
            if(used+span+4>UINT32_MAX || used+span+4>memory.limit())return done(0);
            memory.check(GuestAddress{item},length,Access::Read);
            const auto dest=handle(list?host("kernel32.dll","LocalReAlloc",{list,used+span+4,0x42}):
                                        host("kernel32.dll","LocalAlloc",{0x40,span+4}));
            if(!dest)return done(0);
            // 允許把 SHFindDataBlock 交回的區塊再附加一次；重配後須重定位來源。
            const auto source=item>=list && item-list<used?dest+(item-list):item;
            const auto start=dest+used;
            if(span!=length){
                memory.store(GuestAddress{start},32,span);
                memory.store(GuestAddress{start+4},32,UINT32_MAX);
            }
            memory.move(GuestAddress{start+(span!=length?8:0)},GuestAddress{source},length);
            memory.store(GuestAddress{start+span},32,0);
            memory.store(GuestAddress{arg(0)},pointer_bits,dest);
            return done(1);
        }
        if(!removed.span)return done(0);
        const auto next=removed.address+removed.span;
        memory.move(GuestAddress{removed.address},GuestAddress{next},cursor-next+4);
        if(used==removed.span){
            (void)host("kernel32.dll","LocalFree",{list});
            memory.store(GuestAddress{arg(0)},pointer_bits,0);
        }else{
            const auto resized=handle(host("kernel32.dll","LocalReAlloc",{list,used-removed.span+4,0x42}));
            if(resized)memory.store(GuestAddress{arg(0)},pointer_bits,resized);
        }
        return done(1);
    }
    if(name=="#35"){ // GetStringType3ExW
        return done(host("kernel32.dll","GetStringTypeW",{4,arg(0),u32(1),arg(2)}));
    }
    if(name=="ColorRGBToHLS"){
        const auto value=rgb_to_hls(u32(0));
        if(arg(1))memory.store(GuestAddress{arg(1)},16,static_cast<std::uint16_t>(value.hue));
        if(arg(2))memory.store(GuestAddress{arg(2)},16,static_cast<std::uint16_t>(value.luminance));
        if(arg(3))memory.store(GuestAddress{arg(3)},16,static_cast<std::uint16_t>(value.saturation));
        return done(0);
    }
    if(name=="ColorHLSToRGB")return done(hls_to_rgb(static_cast<std::uint16_t>(arg(0)),static_cast<std::uint16_t>(arg(1)),static_cast<std::uint16_t>(arg(2))));
    if(name=="ColorAdjustLuma"){
        // n 為 -1000～1000；fScale 時依目前亮度到端點的距離按千分比調整，否則直接加到亮度（HLS 單位）。結果夾在 0～240。
        const auto n=static_cast<std::int32_t>(u32(1));if(!n)return done(u32(0));auto value=rgb_to_hls(u32(0));
        if(u32(2))value.luminance+=n>0?(hls_max-value.luminance)*n/1000:value.luminance*n/1000;else value.luminance+=n;
        value.luminance=std::clamp(value.luminance,0,hls_max);return done(hls_to_rgb(value.hue,value.luminance,value.saturation));
    }
    if(name=="PathSkipLeadingSlashesA"){
        const auto path=arg(0);
        if(path && memory.load(GuestAddress{path},8)=='/' &&
           memory.load(GuestAddress{handle(path+1)},8)=='/')return done(handle(path+2));
        return done(path);
    }
    if(name=="FixSlashesAndColonA" || name=="FixSlashesAndColonW"){
        const unsigned stride=name.ends_with('W')?2U:1U;
        const auto bits=static_cast<std::uint8_t>(stride*8);
        for(auto p=arg(0);;p=handle(p+stride)){
            const auto unit=memory.load(GuestAddress{p},bits);
            if(!unit)break;
            if(unit=='/')memory.store(GuestAddress{p},bits,'\\');
        }
        return done(0);
    }
    if(name=="SHSearchMapInt"){
        const auto keys=arg(0), values=arg(1);
        const auto count=static_cast<std::int32_t>(u32(2));
        const auto key=u32(3);
        if(keys && values){
            for(std::int32_t i=0;i<count;++i){
                const auto offset=static_cast<std::uint64_t>(i)*4;
                if(memory.load(GuestAddress{handle(keys+offset)},32)==key)
                    return done(memory.load(GuestAddress{handle(values+offset)},32));
            }
        }
        return done(UINT32_MAX);
    }
    if(name=="#177"){ // SHLoadMenuPopup
        const auto menu=handle(host("user32.dll","LoadMenuW",{arg(0),arg(1)}));
        if(!menu)return done(0);
        if(host("user32.dll","GetSubMenu",{menu,0}))
            (void)host("user32.dll","RemoveMenu",{menu,0,0x400});
        (void)host("user32.dll","DestroyMenu",{menu});
        return done(1);
    }
    if(name=="#180"){ // SHRemoveAllSubMenus 保留已分離的子選單。
        auto count=static_cast<std::int32_t>(host("user32.dll","GetMenuItemCount",{arg(0)}));
        while(count>0){
            const auto index=static_cast<std::uint32_t>(--count);
            if(host("user32.dll","GetSubMenu",{arg(0),index}))
                (void)host("user32.dll","RemoveMenu",{arg(0),index,0x400});
        }
        return done(static_cast<std::uint32_t>(count)-1);
    }
    if(name=="#181")return done(host("user32.dll","EnableMenuItem",{arg(0),u32(1),u32(2)?0U:1U}));
    if(name=="#182")return done(host("user32.dll","CheckMenuItem",{arg(0),u32(1),u32(2)?8U:0U}));
    if(name=="GetMenuPosFromID" || name=="#192"){
        const bool by_position=name=="GetMenuPosFromID";
        const auto count=by_position?static_cast<std::int32_t>(host("user32.dll","GetMenuItemCount",{arg(0)})):1;
        const auto missing=by_position?UINT32_MAX:0U;
        if(count<=0)return done(missing);
        GuestScratch scratch(*this,memory,state);
        const unsigned size=x86_?48U:80U;
        const auto info=scratch.allocate(size);
        if(!info)return done(missing);
        memory.store(GuestAddress{info},32,size);
        memory.store(GuestAddress{info+4},32,by_position?2U:4U);
        for(std::int32_t i=0;i<count;++i){
            const auto index=static_cast<std::uint32_t>(i);
            if(!host("user32.dll","GetMenuItemInfoW",{arg(0),by_position?index:u32(1),by_position,info}))continue;
            if(!by_position)return done(memory.load(GuestAddress{info+(x86_?20U:24U)},pointer_bits));
            if(memory.load(GuestAddress{info+16},32)==u32(1))return done(index);
        }
        return done(missing);
    }
    if(name.starts_with("GetAcceptLanguages")){
        // (buffer, cch* 輸入輸出)：HKCU\Software\Microsoft\Internet Explorer\International 的 AcceptLanguage，沒有時為使用者預設地區名稱。
        // 成功時 *cch 為不含 NUL 的長度；空間不足回 E_NOT_SUFFICIENT_BUFFER 並寫回所需長度（不含 NUL）。
        if(!arg(0) || !arg(1))return done(0x80070057U);const bool wide=name.ends_with('W');const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
        GuestScratch temporary(*this,memory,state);const auto key=temporary.text(u"Software\\Microsoft\\Internet Explorer\\International"),value=temporary.text(u"AcceptLanguage");
        const auto data=temporary.allocate(1026),size=temporary.allocate(4);if(!key || !value || !data || !size)return done(0x8007000eU);
        memory.store(GuestAddress{size},32,1024);const std::uint64_t current_user=x86_?0x80000001ULL:0xffffffff80000001ULL;
        std::u16string languages;
        if(!(host("advapi32.dll","RegGetValueW",{current_user,key,value,2,0,data,size})&UINT32_MAX))languages=read_utf16(memory,GuestAddress{data},512);
        if(languages.empty() && (host("kernel32.dll","GetUserDefaultLocaleName",{data,85})&UINT32_MAX))languages=read_utf16(memory,GuestAddress{data},85);
        if(languages.empty())languages=u"en-US";
        std::vector<std::uint8_t> bytes;std::size_t length=languages.size();
        if(wide)bytes=utf16_bytes(languages);else {const auto text=utf16_to_utf8_lossy(languages);length=text.size();bytes.assign(text.begin(),text.end());bytes.push_back(0);}
        if(length>=capacity){memory.store(GuestAddress{arg(1)},32,length);return done(0x8007007aU);}
        memory.write(GuestAddress{arg(0)},bytes);memory.store(GuestAddress{arg(1)},32,length);return done(0);
    }
    if(name=="SHUnlockShared"){
        return done(arg(0)>=4 && host("kernel32.dll","UnmapViewOfFile",{arg(0)-4}));
    }
    if(name=="SHAllocShared" || name=="SHLockShared" || name=="SHFreeShared" || service.family=="internal-shell-shared"){
        const auto self=handle(host("kernel32.dll","GetCurrentProcess",{}));
        const auto current=static_cast<std::uint32_t>(host("kernel32.dll","GetCurrentProcessId",{}));
        GuestScratch temporary(*this,memory,state);
        const auto slot=temporary.allocate(8);
        if(!slot)return done(0);
        const auto close=[&](std::uint64_t h){return host("kernel32.dll","CloseHandle",{h});};
        const auto open=[&](std::uint32_t pid){return pid==current?self:handle(host("kernel32.dll","OpenProcess",{0x40,0,pid}));};
        const auto duplicate=[&](std::uint64_t h,std::uint32_t from,std::uint32_t to,std::uint32_t access,std::uint32_t options){
            if(!h)return UINT64_C(0);
            const auto target=open(to);
            if(!target)return UINT64_C(0);
            const auto source=open(from);
            std::uint64_t copy=0;
            if(source){
                if(host("kernel32.dll","DuplicateHandle",{source,h,target,slot,access,0,options|2U}))
                    copy=handle(memory.load(GuestAddress{slot},pointer_bits));
                if(source!=self)(void)close(source);
            }
            if(target!=self)(void)close(target);
            return copy;
        };
        if(name=="SHMapHandle")return done(duplicate(arg(0),u32(1),u32(2),u32(3),u32(4)));
        if(name=="SHAllocShared"){
            const auto size=u32(1);
            if(size>UINT32_MAX-4)return done(0);
            if(arg(0) && size)memory.check(GuestAddress{arg(0)},size,Access::Read);
            const auto mapping=handle(host("kernel32.dll","CreateFileMappingW",{self,0,4,0,std::uint64_t{size}+4,0}));
            if(!mapping)return done(0);
            std::uint64_t result=0;
            const auto view=handle(host("kernel32.dll","MapViewOfFile",{mapping,2,0,0,0}));
            if(view){
                try{
                    memory.store(GuestAddress{view},32,size);
                    if(arg(0)){
                        std::array<std::uint8_t,4096> buffer;
                        for(std::size_t offset=0;offset<size;){
                            auto chunk=std::span(buffer).first(std::min(buffer.size(),size-offset));
                            memory.read(GuestAddress{arg(0)+offset},chunk);
                            memory.write(GuestAddress{view+4+offset},chunk);
                            offset+=chunk.size();
                        }
                    }
                }catch(...){
                    (void)host("kernel32.dll","UnmapViewOfFile",{view});(void)close(mapping);throw;
                }
                (void)host("kernel32.dll","UnmapViewOfFile",{view});
                result=duplicate(mapping,current,u32(2),0,0);
            }
            (void)close(mapping);
            return done(result);
        }
        if(name=="SHFreeShared"){
            if(!arg(0))return done(1);
            const auto local=duplicate(arg(0),u32(1),current,0,1);
            return done(local && close(local));
        }
        const auto local=duplicate(arg(0),u32(1),current,0,0);
        if(!local)return done(0);
        const bool write=name=="SHLockShared" || u32(2)!=0;
        const auto view=handle(host("kernel32.dll","MapViewOfFile",{local,write?6U:4U,0,0,0}));
        (void)close(local);
        return done(view?view+4:0);
    }
    if(name=="WhichPlatform" && shell_browser_platform_)return done(shell_browser_platform_);
    if(name=="SHIsLowMemoryMachine"){
        // ILMM_IE4（0）：總實體記憶體少於 64 MiB 視為低記憶體。
        GuestScratch temporary(*this,memory,state);const auto status=temporary.allocate(64);if(!status)return done(0);memory.store(GuestAddress{status},32,64);
        if(!(host("kernel32.dll","GlobalMemoryStatusEx",{status})&UINT32_MAX))return done(0);
        return done(memory.load(GuestAddress{status+8},64)<(std::uint64_t{64}<<20)?1U:0U);
    }
    if(name=="IsInternetESCEnabled"){
        // Internet Explorer 增強式安全性設定：ZoneMap 的 IEHarden（DWORD）非零即啟用。
        GuestScratch temporary(*this,memory,state);const auto key=temporary.text(u"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\ZoneMap"),value=temporary.text(u"IEHarden");
        const auto data=temporary.allocate(4),size=temporary.allocate(4);if(!key || !value || !data || !size)return done(0);memory.store(GuestAddress{size},32,4);
        const std::uint64_t current_user=x86_?0x80000001ULL:0xffffffff80000001ULL;
        if(host("advapi32.dll","RegGetValueW",{current_user,key,value,0x10,0,data,size})&UINT32_MAX)return done(0); // RRF_RT_REG_DWORD
        return done(memory.load(GuestAddress{data},32)?1U:0U);
    }
    const unsigned unit=x86_?4U:8U;
    if(service.family=="internal-thread-ref"){
        // SHCreateThreadRef 的 IUnknown：[vtable][LONG* pcRef]；參照數存在呼叫端提供的 LONG，歸零時釋放物件。
        if(!shell_thread_ref_objects_.contains(arg(0)))return done(0x80010108U); // RPC_E_DISCONNECTED
        const auto counter=memory.load(GuestAddress{arg(0)+unit},pointer_bits);
        if(name=="QueryInterface"){
            if(!arg(2))return done(0x80004003U);memory.store(GuestAddress{arg(2)},pointer_bits,0);if(!arg(1))return done(0x80004003U);
            Iid requested{};memory.read(GuestAddress{arg(1)},requested);if(requested!=iid_unknown)return done(0x80004002U);
            (void)memory.atomic_rmw(GuestAddress{counter},32,GuestMemory::AtomicRmw::Add,1,0);memory.store(GuestAddress{arg(2)},pointer_bits,arg(0));return done(0);
        }
        if(name=="AddRef")return done((memory.atomic_rmw(GuestAddress{counter},32,GuestMemory::AtomicRmw::Add,1,0)+1)&UINT32_MAX);
        const auto remaining=(memory.atomic_rmw(GuestAddress{counter},32,GuestMemory::AtomicRmw::Add,UINT32_MAX,0)-1)&UINT32_MAX;
        if(!remaining){const auto event=memory.load(GuestAddress{arg(0)+2*unit},pointer_bits);shell_thread_ref_objects_.erase(arg(0));(void)free_heap(memory,arg(0),state);
            if(event)(void)host("kernel32.dll","SetEvent",{event});} // CTF_REF_COUNTED 的執行緒等待參照歸零
        return done(remaining);
    }
    if(name=="SHSetThreadRef"){if(arg(0))shell_thread_refs_[threads_.current]=arg(0);else shell_thread_refs_.erase(threads_.current);return done(0);}
    if(name=="SHSetInstanceExplorer"){
        // 常駐映像與慢路徑皆以此位址作為程序唯一的參照槽。
        if(!shell_instance_explorer_)shell_instance_explorer_=runtime_allocate(memory,{},pointer_bits/8);
        memory.store(GuestAddress{shell_instance_explorer_},pointer_bits,arg(0));
        return done(0);
    }
    if(name=="SHCreateThreadRef"){
        if(!arg(1))return done(0x80070057U);memory.store(GuestAddress{arg(1)},pointer_bits,0);if(!arg(0))return done(0x80070057U);
        if(!shell_thread_ref_vtable_){const auto table=runtime_allocate(memory,{},3*unit);if(!table)return done(0x8007000eU);unsigned i=0;
            for(const auto symbol:{"QueryInterface","AddRef","Release"})memory.store(GuestAddress{table+unit*i++},pointer_bits,resolve("internal-thread-ref",symbol).value);
            shell_thread_ref_vtable_=table;}
        const auto object=allocate_heap(memory,3*unit);if(!object)return done(0x8007000eU);
        memory.store(GuestAddress{object},pointer_bits,shell_thread_ref_vtable_);memory.store(GuestAddress{object+unit},pointer_bits,arg(0));memory.store(GuestAddress{object+2*unit},pointer_bits,0);memory.store(GuestAddress{arg(0)},32,1);
        shell_thread_ref_objects_.insert(object);memory.store(GuestAddress{arg(1)},pointer_bits,object);return done(0);
    }
    const bool wide=name.ends_with('W');
    const auto text_at=[&](std::uint64_t address)->std::u16string{if(!address)return {};return wide?read_utf16(memory,GuestAddress{address},32768):utf8_to_utf16_lossy(read_ansi(memory,address,32768));};
    const auto write_text=[&](std::uint64_t address,std::size_t capacity,const std::u16string& value)->std::size_t{
        // 至多寫入 capacity-1 個單位並補 NUL；A 版為 UTF-8 且不切斷字元。回傳寫入的單位數（不含 NUL）。
        if(!capacity)return 0;if(wide){const auto count=std::min(value.size(),capacity-1);auto bytes=utf16_bytes(value.substr(0,count));memory.write(GuestAddress{address},bytes);return count;}
        auto bytes=utf16_to_utf8_lossy(value);auto count=std::min(bytes.size(),capacity-1);while(count<bytes.size() && count && (static_cast<unsigned char>(bytes[count])&0xc0U)==0x80U)--count;
        std::vector<std::uint8_t> data(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(count));data.push_back(0);memory.write(GuestAddress{address},data);return count;};
    if(name=="SHCreateShellPalette"){
        // 半色調調色盤；HDC 為 NULL 時以螢幕 DC 建立。
        if(arg(0))return done(handle(host("gdi32.dll","CreateHalftonePalette",{arg(0)})));
        const auto dc=handle(host("user32.dll","GetDC",{0}));const auto palette=dc?handle(host("gdi32.dll","CreateHalftonePalette",{dc})):0;if(dc)(void)host("user32.dll","ReleaseDC",{0,dc});return done(palette);
    }
    if(name=="SHGetInverseCMAP"){
        // 15 位元色彩（索引＝R5<<10 | G5<<5 | B5）到半色調調色盤最接近項目的反查表；cb 為指標大小時回傳常駐表的位址，否則複製至多 32768 位元組。
        if(!arg(0) || !u32(1))return done(0x80070057U);
        if(!shell_inverse_cmap_){
            const auto dc=handle(host("user32.dll","GetDC",{0}));const auto palette=dc?handle(host("gdi32.dll","CreateHalftonePalette",{dc})):0;if(dc)(void)host("user32.dll","ReleaseDC",{0,dc});if(!palette)return done(0x80004005U);
            GuestScratch temporary(*this,memory,state);const auto entries=temporary.allocate(1024);if(!entries){(void)host("gdi32.dll","DeleteObject",{palette});return done(0x8007000eU);}
            const auto count=static_cast<std::uint32_t>(host("gdi32.dll","GetPaletteEntries",{palette,0,256,entries}));(void)host("gdi32.dll","DeleteObject",{palette});if(!count)return done(0x80004005U);
            std::vector<std::array<int,3>> colors(count);for(std::uint32_t i=0;i<count;++i){const auto c=static_cast<std::uint32_t>(memory.load(GuestAddress{entries+i*4},32));colors[i]={static_cast<int>(c&0xffU),static_cast<int>((c>>8)&0xffU),static_cast<int>((c>>16)&0xffU)};}
            std::vector<std::uint8_t> table(32768);
            for(std::uint32_t index=0;index<32768;++index){
                const int r=static_cast<int>((index>>10)&31U)*255/31,g=static_cast<int>((index>>5)&31U)*255/31,b=static_cast<int>(index&31U)*255/31;int best=INT32_MAX;std::uint8_t chosen=0;
                for(std::uint32_t i=0;i<count;++i){const int dr=colors[i][0]-r,dg=colors[i][1]-g,db=colors[i][2]-b;const int distance=dr*dr+dg*dg+db*db;if(distance<best){best=distance;chosen=static_cast<std::uint8_t>(i);}}
                table[index]=chosen;}
            shell_inverse_cmap_=runtime_allocate(memory,table);if(!shell_inverse_cmap_)return done(0x8007000eU);
        }
        if(u32(1)==unit){memory.store(GuestAddress{arg(0)},pointer_bits,shell_inverse_cmap_);return done(0);}
        std::vector<std::uint8_t> bytes(std::min<std::uint32_t>(u32(1),32768));memory.read(GuestAddress{shell_inverse_cmap_},bytes);memory.write(GuestAddress{arg(0)},bytes);return done(0);
    }
    if(name=="UrlFixupW"){
        if(!arg(0))return done(0x80004005U);
        if(!arg(1) || !u32(2))return done(0x80070057U);
        auto url=text_at(arg(0));
        const auto colon=url.find(u':');
        const uri_model::Scheme* match=nullptr;
        if(colon!=url.npos && colon){
            const auto scheme=uri_model::lowered(std::u16string_view(url).substr(0,colon));
            match=uri_model::find_scheme(scheme);
            if(!match && scheme.size()>=2){
                // 重複鍵、單一錯字與縮寫皆以首字母優先；同分歧義保留原輸入。
                const auto collapse=[](std::u16string text){
                    text.erase(std::unique(text.begin(),text.end()),text.end());
                    return text;
                };
                const auto collapsed=collapse(scheme);
                unsigned best=0;
                for(const auto& candidate:uri_model::schemes){
                    const auto spelling=candidate.name;
                    unsigned score=0;
                    if(collapsed==collapse(std::u16string(spelling)))score=3;
                    else{
                        const auto pos=static_cast<std::size_t>(std::mismatch(scheme.begin(),scheme.end(),spelling.begin(),spelling.end()).first-scheme.begin());
                        const auto a=std::u16string_view(scheme).substr(pos),b=spelling.substr(pos);
                        if((a.size() && a.substr(1)==b) || (b.size() && b.substr(1)==a) ||
                           (a.size() && b.size() && a.substr(1)==b.substr(1)))score=2;
                        else if(scheme.size()>=3 && spelling.starts_with(scheme))score=1;
                    }
                    if(!score)continue;
                    if(scheme.front()==spelling.front())score+=4;
                    if(score>best){match=&candidate;best=score;}
                    else if(score==best)match=nullptr;
                }
                if(match)url.replace(0,colon,match->name);
            }
            if(match && match->hierarchical && match->port){
                const auto start=url.find(u':')+1;
                auto end=start;
                while(end<url.size() && end-start<2 && (url[end]==u'/' || url[end]==u'\\'))++end;
                url.replace(start,end-start,u"//");
            }
        }
        if(url.size()>=u32(2))return done(0x80004003U);
        write_text(arg(1),u32(2),url);
        return done(match?0:1);
    }
    if(name.starts_with("SHFormatDateTime")){
        // (FILETIME* UTC, flags* 可為 NULL＝FDTF_DEFAULT, buffer, cch)：轉成本地時間後依使用者地區組合日期與時間（以空白分隔）；回傳含 NUL 的寫入數。
        // SHORTTIME 1、SHORTDATE 2、LONGDATE 4、LONGTIME 8、RELATIVE 0x10（今天／昨天）、LTRDATE 0x100、RTLDATE 0x200。
        if(!arg(0) || !arg(2) || !u32(3))return done(0);const auto flags=arg(1)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32)):3U;
        if(((flags&2U) && (flags&4U)) || ((flags&1U) && (flags&8U)) || ((flags&0x100U) && (flags&0x200U)))return done(0);
        GuestScratch temporary(*this,memory,state);const auto utc=temporary.allocate(8),local=temporary.allocate(8),fields=temporary.allocate(16),today=temporary.allocate(16),buffer=temporary.allocate(512);
        if(!utc || !local || !fields || !today || !buffer)return done(0);memory.store(GuestAddress{utc},64,memory.load(GuestAddress{arg(0)},64));
        if(!(host("kernel32.dll","FileTimeToLocalFileTime",{utc,local})&UINT32_MAX) || !(host("kernel32.dll","FileTimeToSystemTime",{local,fields})&UINT32_MAX))return done(0);
        std::u16string result;
        if(flags&6U){
            bool relative=false;
            if(flags&0x10U){(void)host("kernel32.dll","GetLocalTime",{today});
                const auto day=[&](std::uint64_t at){return std::array<std::uint64_t,3>{memory.load(GuestAddress{at},16),memory.load(GuestAddress{at+2},16),memory.load(GuestAddress{at+6},16)};};
                const auto now=day(today),when=day(fields);
                if(now==when){result=u"Today";relative=true;}
                else {const auto yesterday=temporary.allocate(8),stamp=temporary.allocate(8),back=temporary.allocate(16);
                    if(yesterday && stamp && back && (host("kernel32.dll","SystemTimeToFileTime",{today,stamp})&UINT32_MAX)){memory.store(GuestAddress{yesterday},64,memory.load(GuestAddress{stamp},64)-UINT64_C(864000000000));
                        if((host("kernel32.dll","FileTimeToSystemTime",{yesterday,back})&UINT32_MAX) && day(back)==when){result=u"Yesterday";relative=true;}}}}
            if(!relative){const auto date_flags=((flags&4U)?2U:1U)|((flags&0x100U)?0x10U:0U)|((flags&0x200U)?0x20U:0U);
                if(!(host("kernel32.dll","GetDateFormatW",{0x400,date_flags,fields,0,buffer,256})&UINT32_MAX))return done(0);result=read_utf16(memory,GuestAddress{buffer},256);}
        }
        if(flags&9U){if(!(host("kernel32.dll","GetTimeFormatW",{0x400,(flags&8U)?0U:2U,fields,0,buffer,256})&UINT32_MAX))return done(0);
            if(!result.empty())result+=u' ';result+=read_utf16(memory,GuestAddress{buffer},256);}
        return done(write_text(arg(2),u32(3),result)+1);
    }
    if(name.starts_with("SHOpenRegStream")){
        const bool legacy=!name.starts_with("SHOpenRegStream2");
        const auto mode=u32(3),access=mode&3U;
        const bool write=access==1 || access==2;
        const auto failure=[&]{return legacy?handle(host("internal-registry-stream","Dummy",{})):0;};
        if(access==3)return done(failure());
        GuestScratch temporary(*this,memory,state);
        const auto value_name=text_at(arg(2));
        const auto subkey=arg(1)?temporary.text(text_at(arg(1))):0;
        const auto value=arg(2)?temporary.text(value_name):0;
        const auto key_slot=temporary.allocate(pointer_bits/8),size=temporary.allocate(4);
        if(!key_slot || !size || (arg(1) && !subkey) || (arg(2) && !value))return done(failure());
        const auto opened=write?host("advapi32.dll","RegCreateKeyExW",{arg(0),subkey,0,0,0,0x2001f,0,key_slot,0}):
                                host("advapi32.dll","RegOpenKeyExW",{arg(0),subkey,0,0x20019,key_slot});
        if(opened&UINT32_MAX)return done(failure());
        const auto key=handle(memory.load(GuestAddress{key_slot},pointer_bits));
        const auto close=[&]{(void)host("advapi32.dll","RegCloseKey",{key});};
        std::uint32_t length=0;
        std::uint64_t data=0;
        if(access!=1 && !(write && (mode&0x1000U))){
            memory.store(GuestAddress{size},32,0);
            auto error=static_cast<std::uint32_t>(host("advapi32.dll","RegQueryValueExW",{key,value,0,0,0,size}));
            if(!error){
                length=static_cast<std::uint32_t>(memory.load(GuestAddress{size},32));
                if(length){
                    data=length<=output_limit?temporary.allocate(length):0;
                    error=data?static_cast<std::uint32_t>(host("advapi32.dll","RegQueryValueExW",{key,value,0,0,data,size})):8;
                    if(!error)length=static_cast<std::uint32_t>(memory.load(GuestAddress{size},32));
                }
            }
            // 機碼存在而值尚未建立時，提供長度為零的資料流。
            if(error==2){length=0;data=0;}
            else if(error){
                close();
                return done(failure());
            }
        }
        const auto stream=handle(host("shlwapi.dll","SHCreateMemStream",{data,length}));
        if(!stream){close();return done(failure());}
        if(!write)close();
        else if(!bind_registry_stream(stream,key,value_name)){
            close();
            (void)host("internal-ole-stream","Release",{stream});
            return done(failure());
        }
        return done(stream);
    }
    if(name=="SHIsChildOrSelf"){
        // 參考實作 的 BOOL 在兩者相同或有子孫關係時為 FALSE，空控制代碼為 TRUE。
        const auto parent=arg(0), child=arg(1);
        if(!parent || !child)return done(1);
        if(parent==child)return done(0);
        return done(!static_cast<std::uint32_t>(host("user32.dll","IsChild",{parent,child})));
    }
    return nested_continuation(state,memory,"ShlwapiCom",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto symbol=service.symbol;const unsigned size=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(size*8);
        const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const auto method=[&](std::uint64_t object,unsigned index){return m.load(GuestAddress{m.load(GuestAddress{object},bits)+index*size},bits);};
        const auto failed=[](std::uint32_t value){return (value&0x80000000U)!=0;};
        const auto status=[](std::uint64_t value){return static_cast<std::uint32_t>(value);};
        if(symbol=="IUnknown_AtomicRelease" || symbol=="IUnknown_Set"){
            const auto dest=call(0);
            const auto replacement=symbol=="IUnknown_Set"?call(1):0;
            const auto previous=dest?m.load(GuestAddress{dest},bits):0;
            if(previous){
                (void)co_await service_callback::call(method(previous,2),{previous});
                m.store(GuestAddress{dest},bits,0);
            }
            if(replacement){
                (void)co_await service_callback::call(method(replacement,1),{replacement});
                m.store(GuestAddress{dest},bits,replacement);
            }
            co_return finish(0);
        }
        // ShellMessageBox 的可變引數起點（第 5 格；x86 為 ESP+4+5*4，x64 為 RSP+8+5*8），供 FormatMessage 當 va_list。
        const auto variadic=s.reg(Gpr::Rsp)+(x86_?24U:48U);(void)variadic;
        GuestScratch temporary(*this,m,s);
        const auto entry=[&](std::string_view dll,std::string_view target){const auto stub=host_service_stub(dll,target);return stub.value?stub.value:resolve(dll,target).value;};
        const auto handle_of=[&](std::uint64_t value){return x86_?value&UINT32_MAX:value;};
        const bool wide=symbol.ends_with('W');
        if(symbol=="HrAddColumns" || symbol=="HrAddColumnsEx"){
            const auto table=call(0),added=call(1),alloc=call(2),release=call(3);
            const auto filter=symbol=="HrAddColumnsEx"?call(4):0;
            if(!table || !added || !alloc || !release)co_return finish(0x80070057U);
            const auto slot=temporary.allocate(size);
            if(!slot)co_return finish(0x8007000eU);
            m.store(GuestAddress{slot},bits,0);
            auto hr=status(co_await service_callback::call(method(table,8),{table,0,slot}));
            if(failed(hr))co_return finish(hr);
            const auto old=m.load(GuestAddress{slot},bits);
            if(!old)co_return finish(0x80004005U);
            const auto capacity=m.load(GuestAddress{old},32);
            if(filter)(void)co_await service_callback::call(filter,{old});
            const auto previous=m.load(GuestAddress{old},32),extra=m.load(GuestAddress{added},32);
            if(previous>capacity)hr=0x80070057U;
            else if(extra+previous>0x3ffffffeU)hr=0x8007000eU;
            else{
                m.store(GuestAddress{slot},bits,0);
                hr=status(co_await service_callback::call(alloc,{(extra+previous+1)*4,slot}));
                if(!failed(hr)){
                    const auto merged=m.load(GuestAddress{slot},bits);
                    if(!merged)hr=0x8007000eU;
                    else{
                        auto count=extra;
                        if(extra)m.move(GuestAddress{merged+4},GuestAddress{added+4},extra*4);
                        for(std::uint64_t i=1;i<=previous;++i){
                            const auto tag=m.load(GuestAddress{old+4*i},32);
                            std::uint64_t j=1;
                            while(j<=extra && m.load(GuestAddress{merged+4*j},32)!=tag)++j;
                            if(j>extra)m.store(GuestAddress{merged+4*(++count)},32,tag);
                        }
                        m.store(GuestAddress{merged},32,count);
                        hr=status(co_await service_callback::call(method(table,7),{table,merged,0}));
                        (void)co_await service_callback::call(release,{merged});
                    }
                }
            }
            (void)co_await service_callback::call(release,{old});
            co_return finish(hr);
        }
        if(symbol=="HrQueryAllRows"){
            const auto table=call(0),columns=call(1),filter=call(2),sort=call(3),out=call(5);
            const auto limit=static_cast<std::int32_t>(call(4));
            if(!out)co_return finish(0x80070057U);
            m.store(GuestAddress{out},bits,0);
            if(!table || limit<0)co_return finish(0x80070057U);
            const auto slot=temporary.allocate(size);
            if(!slot)co_return finish(0x8007000eU);
            std::uint32_t hr=0;
            if(columns)hr=status(co_await service_callback::call(method(table,7),{table,columns,0}));
            if(!failed(hr) && filter)hr=status(co_await service_callback::call(method(table,14),{table,filter,0}));
            if(!failed(hr) && sort)hr=status(co_await service_callback::call(method(table,17),{table,sort,0}));
            if(!failed(hr))hr=status(co_await service_callback::call(method(table,10),{table,0,0,0}));
            const unsigned stride=8+size;
            std::uint64_t total=0;
            std::vector<std::pair<std::uint64_t,std::uint32_t>> pages;
            const auto free_rows=[&](std::uint64_t rows){
                (void)nested_service(s,m,"internal-mapi-buffer","FreeProws",{rows});
            };
            while(!failed(hr)){
                const auto request=limit?std::min<std::uint64_t>(128,static_cast<unsigned>(limit)-total+1):128;
                m.store(GuestAddress{slot},bits,0);
                hr=status(co_await service_callback::call(method(table,19),{table,request,0,slot}));
                if(failed(hr))break;
                const auto rows=m.load(GuestAddress{slot},bits);
                if(!rows){hr=0x80004005U;break;}
                const auto count=status(m.load(GuestAddress{rows},32));
                if(!count){free_rows(rows);break;}
                if((limit && total+count>static_cast<unsigned>(limit)) || total+count>(UINT32_MAX-size)/stride){
                    free_rows(rows);hr=0x80040403U;break;
                }
                try{pages.emplace_back(rows,count);}
                catch(const std::bad_alloc&){free_rows(rows);hr=0x8007000eU;break;}
                total+=count;
            }
            if(!failed(hr)){
                hr=status(nested_service(s,m,"internal-mapi-buffer","MAPIAllocateBuffer",{size+total*stride,slot}));
                if(!failed(hr)){
                    const auto answer=m.load(GuestAddress{slot},bits);
                    m.store(GuestAddress{answer},32,total);
                    auto dest=answer+size;
                    for(const auto& [page,count]:pages){
                        const auto bytes=std::uint64_t(count)*stride;
                        m.move(GuestAddress{dest},GuestAddress{page+size},bytes);
                        for(std::uint64_t i=0;i<count;++i){
                            const auto row=dest+i*stride;
                            const auto props=m.load(GuestAddress{row+8},bits),n=m.load(GuestAddress{row+4},32);
                            for(std::uint64_t j=0;j<n;++j){
                                const auto at=props+j*mapi_property_stride(size);
                                if((m.load(GuestAddress{at},32)&0xffffU)==10 && m.load(GuestAddress{at+8},32)==0x8004010fU)
                                    m.store(GuestAddress{at},32,1);
                            }
                        }
                        // 列中的屬性陣列轉交新列集，只釋放原頁的外層配置。
                        m.store(GuestAddress{page},32,0);
                        dest+=bytes;
                    }
                    m.store(GuestAddress{out},bits,answer);
                }
            }
            for(const auto& page:pages)free_rows(page.first);
            co_return finish(hr);
        }
        if(service.family=="internal-mapi-one"){
            const auto object=call(0),input=call(1);
            if(symbol=="HrSetOneProp")
                co_return finish(status(co_await service_callback::call(method(object,8),{object,1,input,0})));
            const bool get=symbol=="HrGetOneProp";
            if(!get && !object)co_return finish(0);
            const auto scratch=temporary.allocate(get?12:size);
            if(!scratch)co_return finish(get?0x8007000eU:0);
            const auto release=entry("internal-mapi-buffer","MAPIFreeBuffer");
            if(get){
                const auto output=call(2);
                m.store(GuestAddress{scratch},32,1);
                m.store(GuestAddress{scratch+4},32,input);
                const auto hr=status(co_await service_callback::call(method(object,5),{object,scratch,0,scratch+8,output}));
                if(hr!=0x40380U)co_return finish(hr);
                const auto value=m.load(GuestAddress{output},bits);
                (void)co_await service_callback::call(release,{value});
                m.store(GuestAddress{output},bits,0);
                co_return finish(0x8004010fU);
            }
            const auto hr=status(co_await service_callback::call(method(object,6),{object,0,scratch}));
            if(failed(hr))co_return finish(0);
            const auto tags=m.load(GuestAddress{scratch},bits),count=m.load(GuestAddress{tags},32);
            bool found=false;
            for(std::uint64_t i=0;i<count && !found;++i)
                found=mapi_property_matches(status(m.load(GuestAddress{tags+4+i*4},32)),status(input),0);
            (void)co_await service_callback::call(release,{tags});
            co_return finish(found);
        }
        if(symbol=="IUnknown_GetWindow" || symbol=="IUnknown_SetOwner" || symbol=="IUnknown_GetClassID"){
            const auto object=call(0), argument=call(1);
            const bool class_id=symbol=="IUnknown_GetClassID";
            if(!object){
                if(class_id)m.write(GuestAddress{argument},Iid{});
                co_return finish(0x80004005U);
            }
            const auto& query=class_id?class_id_query:symbol=="IUnknown_SetOwner"?owner_query:window_query;
            const auto requested=temporary.allocate(16+size);
            if(!requested)co_return finish(0x8007000eU);
            const auto output=requested+16;
            std::uint32_t hr=0x80004005U;
            for(unsigned i=0;i<query.count;++i){
                const auto& iid=query.iids[i];
                m.write(GuestAddress{requested},iid);
                hr=status(co_await service_callback::call(method(object,0),{object,requested,output}));
                if(query.require_s_ok?hr!=0:failed(hr))continue;
                const auto target=m.load(GuestAddress{output},bits);
                hr=status(co_await service_callback::call(method(target,3),{target,argument}));
                (void)co_await service_callback::call(method(target,2),{target});
                break;
            }
            co_return finish(hr);
        }
        if(symbol=="ConnectToConnectionPoint"){
            const auto sink=call(0), iid=call(1), source=call(3), cookie=call(4), output=call(5);
            const bool connect=static_cast<std::uint32_t>(call(2))!=0;
            if(!source || (connect && !sink))co_return finish(0x80004005U);
            if(output)m.store(GuestAddress{output},bits,0);
            const auto requested=temporary.allocate(16+2*size);
            if(!requested)co_return finish(0x8007000eU);
            m.write(GuestAddress{requested},iid_point_container);
            const auto container_slot=requested+16, point_slot=container_slot+size;
            auto hr=status(co_await service_callback::call(method(source,0),{source,requested,container_slot}));
            if(failed(hr))co_return finish(hr);
            const auto container=m.load(GuestAddress{container_slot},bits);
            hr=status(co_await service_callback::call(method(container,4),{container,iid,point_slot}));
            if(!failed(hr)){
                const auto point=m.load(GuestAddress{point_slot},bits);
                if(connect)hr=status(co_await service_callback::call(method(point,5),{point,sink,cookie}));
                else hr=status(co_await service_callback::call(method(point,6),{point,m.load(GuestAddress{cookie},32)}));
                if(failed(hr))m.store(GuestAddress{cookie},32,0);
                if(output && !failed(hr))m.store(GuestAddress{output},bits,point);
                else (void)co_await service_callback::call(method(point,2),{point});
            }
            (void)co_await service_callback::call(method(container,2),{container});
            co_return finish(hr);
        }
        if(symbol=="SHIsSameObject"){
            const auto left=call(0), right=call(1);
            if(!left || !right)co_return finish(0);
            if(left==right)co_return finish(1);
            const auto buffer=temporary.allocate(16+2*size);
            if(!buffer)co_return finish(0);
            m.write(GuestAddress{buffer},iid_unknown);
            const auto first_slot=buffer+16, second_slot=first_slot+size;
            if(status(co_await service_callback::call(method(left,0),{left,buffer,first_slot}))!=0)
                co_return finish(0);
            const auto second_status=status(co_await service_callback::call(method(right,0),{right,buffer,second_slot}));
            const auto first=m.load(GuestAddress{first_slot},bits);
            const auto second=second_status==0?m.load(GuestAddress{second_slot},bits):0;
            (void)co_await service_callback::call(method(first,2),{first});
            if(second_status!=0)co_return finish(0);
            (void)co_await service_callback::call(method(second,2),{second});
            co_return finish(first==second);
        }
        if(symbol=="#178"){
            const auto parent=call(0);
            if(!parent)co_return finish(0);
            const auto args=temporary.allocate(4*size);
            if(!args){last_error_=8;co_return finish(0);}
            const bool send=static_cast<std::uint32_t>(call(4))!=0;
            const bool unicode=co_await service_callback::call(entry("user32.dll","IsWindowUnicode"),{parent});
            const auto dispatch=send?(unicode?"SendMessageW":"SendMessageA"):(unicode?"PostMessageW":"PostMessageA");
            m.store(GuestAddress{args},bits,entry("user32.dll",dispatch));
            for(unsigned i=1;i<4;++i)m.store(GuestAddress{args+i*size},bits,call(i));
            (void)co_await service_callback::call(entry("user32.dll","EnumChildWindows"),{parent,entry("internal-message-propagation","EnumProc"),args});
            co_return finish(0);
        }
        if(service.family=="internal-message-propagation"){
            const auto data=call(1);
            const auto field=[&](unsigned i){return m.load(GuestAddress{data+i*size},bits);};
            (void)co_await service_callback::call(field(0),{call(0),field(1),field(2),field(3)});
            co_return finish(1);
        }
        if(symbol=="#194"){
            const auto hand=temporary.allocate(size),msg=temporary.allocate(x86_?28:48);
            if(!hand || !msg){last_error_=8;co_return finish(UINT32_MAX);}
            m.store(GuestAddress{hand},bits,call(0));
            const auto timeout=static_cast<std::uint32_t>(call(1));
            const auto tick=entry("kernel32.dll","GetTickCount64");
            const auto start=co_await service_callback::call_value(tick,{});
            auto remaining=timeout;
            for(;;){
                const auto result=co_await service_callback::call(entry("user32.dll","MsgWaitForMultipleObjectsEx"),{1,hand,remaining,0x40,0});
                if(result!=1)co_return finish(result);
                (void)co_await service_callback::call(entry("user32.dll","PeekMessageW"),{msg,0,0,0,0});
                if(timeout!=UINT32_MAX){
                    const auto elapsed=static_cast<std::uint32_t>((co_await service_callback::call_value(tick,{}))-start);
                    if(elapsed>=timeout)co_return finish(258);
                    remaining=timeout-elapsed;
                }
            }
        }
        if(symbol=="#236" || symbol=="#343" || symbol=="#344"){
            const bool load=symbol=="#236";
            if(!call(0) || (!load && !call(4))){last_error_=87;co_return finish(load?0:0x80070057U);}
            WindowsGuid clsid{};
            m.read(GuestAddress{call(0)},clsid);
            const bool user=!load && static_cast<std::uint32_t>(call(2));
            std::u16string path=user?u"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CLSID\\":u"CLSID\\";
            path+=windows_guid_text(clsid);
            if(load)path+=u"\\InProcServer32";
            else if(call(1)){
                path+=u'\\';
                path+=symbol=="#343"?utf8_to_utf16_lossy(read_ansi(m,call(1))):read_utf16(m,GuestAddress{call(1)});
            }
            const auto key=temporary.text(path);
            if(!key){last_error_=8;co_return finish(load?0:0x8007000eU);}
            const auto root=user?0x80000001U:0x80000000U;
            if(!load){
                const auto error=static_cast<std::uint32_t>(static_cast<std::uint32_t>(call(3))?
                    co_await service_callback::call(entry("advapi32.dll","RegCreateKeyW"),{root,key,call(4)}):
                    co_await service_callback::call(entry("advapi32.dll","RegOpenKeyExW"),{root,key,0,0x20019,call(4)}));
                co_return finish(error?0x80070000U|(error&0xffffU):0);
            }
            const auto length=temporary.allocate(4),dll=temporary.allocate(65536);
            if(!length || !dll){last_error_=8;co_return finish(0);}
            m.store(GuestAddress{length},32,65536);
            const auto error=static_cast<std::uint32_t>(co_await service_callback::call(entry("advapi32.dll","RegGetValueW"),{root,key,0,2,0,dll,length}));
            if(error){last_error_=error;co_return finish(0);}
            co_return finish(handle_of(co_await service_callback::call_value(entry("kernel32.dll","LoadLibraryExW"),{dll,0,0})));
        }
        if(service.family=="internal-shell-dialog"){
            if(symbol=="SHDialogBox"){
                const auto data=temporary.allocate(2*size);
                if(!data){last_error_=8;co_return finish(UINT64_MAX);}
                m.store(GuestAddress{data},bits,call(3));
                m.store(GuestAddress{data+size},bits,call(4));
                const auto proc=entry("internal-shell-dialog","DialogProc");
                co_return finish(co_await service_callback::call_value(entry("user32.dll","DialogBoxParamA"),{call(0),call(1),call(2),proc,data}));
            }
            const auto window=call(0),message=call(1),wp=call(2);
            auto lp=call(3);
            std::uint64_t data=0;
            if(message==0x110){
                data=lp;
                (void)co_await service_callback::call(entry("user32.dll","SetWindowLongPtrA"),{window,2*size,data});
            }else{
                data=handle_of(co_await service_callback::call_value(entry("user32.dll","GetWindowLongPtrA"),{window,2*size}));
            }
            if(data){
                const auto proc=m.load(GuestAddress{data},bits),context=m.load(GuestAddress{data+size},bits);
                if(message==0x110)lp=context;
                if(proc){
                    const auto result=co_await service_callback::call_value(proc,{context,window,message,wp,lp});
                    if(result)co_return finish(result);
                }
            }
            if(message==0x110)co_return finish(1);
            const auto id=wp&0xffff;
            if(message==0x111 && id!=9){
                const auto item=handle_of(co_await service_callback::call_value(entry("user32.dll","GetDlgItem"),{window,id}));
                if(item && (0x30U & co_await service_callback::call(entry("user32.dll","SendMessageA"),{item,0x87,0,0}))){
                    (void)co_await service_callback::call(entry("user32.dll","EndDialog"),{window,id});
                    co_return finish(1);
                }
            }
            co_return finish(0);
        }
        if(service.family=="internal-worker-window"){
            if(symbol=="SHDefWindowProc"){
                const auto window=call(0),message=call(1),wp=call(2),lp=call(3);
                const bool unicode=co_await service_callback::call(entry("user32.dll","IsWindowUnicode"),{window});
                co_return finish(co_await service_callback::call_value(entry("user32.dll",unicode?"DefWindowProcW":"DefWindowProcA"),{window,message,wp,lp}));
            }
            const auto proc=call(0),parent=call(1),ex_style=call(2),style=call(3),menu=call(4),extra=call(5);
            const GuiClassAbi abi{x86_,false};
            const auto cls=temporary.allocate(abi.size),library=temporary.text(u"shlwapi.dll");
            const auto name_ptr=wide?temporary.text(u"WorkerW"):temporary.allocate(8);
            if(!cls || !library || !name_ptr){last_error_=8;co_return finish(0);}
            if(!wide){constexpr std::uint8_t name[]="WorkerA";m.write(GuestAddress{name_ptr},name);}
            const auto module=handle_of(co_await service_callback::call_value(entry("kernel32.dll","GetModuleHandleW"),{library}));
            if(!module)co_return finish(0);
            const auto cursor=handle_of(co_await service_callback::call_value(entry("user32.dll",wide?"LoadCursorW":"LoadCursorA"),{0,32512}));
            m.store(GuestAddress{cls+abi.procedure},bits,resolve("user32.dll",wide?"DefWindowProcW":"DefWindowProcA").value);
            m.store(GuestAddress{cls+abi.window_extra},32,size);
            m.store(GuestAddress{cls+abi.instance},bits,module);
            m.store(GuestAddress{cls+abi.cursor},bits,cursor);
            m.store(GuestAddress{cls+abi.background},bits,16);
            m.store(GuestAddress{cls+abi.name},bits,name_ptr);
            (void)co_await service_callback::call(entry("internal-shell-window",wide?"SHRegisterClassW":"SHRegisterClassA"),{cls});
            const auto window=handle_of(co_await service_callback::call_value(entry("user32.dll",wide?"CreateWindowExW":"CreateWindowExA"),
                {ex_style,name_ptr,0,style,0,0,0,0,parent,menu,module,0}));
            if(window){
                (void)co_await service_callback::call(entry("user32.dll","SetWindowLongPtrW"),{window,0,extra});
                if(proc)(void)co_await service_callback::call(entry("user32.dll",wide?"SetWindowLongPtrW":"SetWindowLongPtrA"),{window,static_cast<std::uint32_t>(-4),proc});
            }
            co_return finish(window);
        }
        if(service.family=="internal-shell-window"){
            if(symbol.starts_with("SHRegister") || symbol.starts_with("SHUnregister")){
                const bool registration=symbol.starts_with("SHRegister");
                const auto count=registration?1:static_cast<std::int32_t>(call(2));
                if(count<=0)co_return finish(0);
                const GuiClassAbi abi{x86_,false};
                const auto output=temporary.allocate(abi.size);
                if(!output){last_error_=8;co_return finish(0);}
                const auto input=call(0);
                const auto module=registration?m.load(GuestAddress{input+abi.instance},bits):input;
                const auto get_info=entry("user32.dll",wide?"GetClassInfoW":"GetClassInfoA");
                if(registration){
                    const auto name_ptr=m.load(GuestAddress{input+abi.name},bits);
                    if(co_await service_callback::call(get_info,{module,name_ptr,output}))co_return finish(1);
                    co_return finish(co_await service_callback::call(entry("user32.dll",wide?"RegisterClassW":"RegisterClassA"),{input}));
                }
                const auto remove=entry("user32.dll",wide?"UnregisterClassW":"UnregisterClassA");
                auto list=call(1);
                for(std::int32_t i=0;i<count;++i,list+=size){
                    const auto name_ptr=m.load(GuestAddress{list},bits);
                    if(co_await service_callback::call(get_info,{module,name_ptr,output}))
                        (void)co_await service_callback::call(remove,{name_ptr,module});
                }
                co_return finish(0);
            }
            const bool reparent=symbol=="SHSetParentHwnd";
            const auto window=call(0),parent=reparent?call(1):0;
            if(reparent && handle_of(co_await service_callback::call_value(entry("user32.dll","GetParent"),{window}))==parent)
                co_return finish(0);
            const auto index=reparent?static_cast<std::uint32_t>(-16):static_cast<std::uint32_t>(call(1));
            const auto mask=reparent?0xc0000000U:static_cast<std::uint32_t>(call(2));
            const auto flags=reparent?(parent?0x40000000U:0x80000000U):static_cast<std::uint32_t>(call(3));
            auto previous=static_cast<std::uint32_t>(co_await service_callback::call(entry("user32.dll","GetWindowLongW"),{window,index}));
            const auto updated=(previous&~mask)|(flags&mask);
            if(updated!=previous)
                previous=static_cast<std::uint32_t>(co_await service_callback::call(entry("user32.dll","SetWindowLongW"),{window,index,updated}));
            if(!reparent)co_return finish(previous);
            if(!parent)co_return finish(0);
            co_return finish(handle_of(co_await service_callback::call_value(entry("user32.dll","SetParent"),{window,parent})));
        }
        if(symbol=="#267"){ // SHWeakQueryInterface
            const auto outer=call(0),inner=call(1),iid=call(2),output=call(3);
            m.store(GuestAddress{output},bits,0);
            if(!outer || !inner)co_return finish(0x80004002U);
            const auto hr=status(co_await service_callback::call(method(inner,0),{inner,iid,output}));
            if(!failed(hr))(void)co_await service_callback::call(method(outer,2),{outer});
            co_return finish(hr);
        }
        if(symbol=="#268"){ // SHWeakReleaseInterface
            const auto destination=call(0),source_slot=call(1);
            if(m.load(GuestAddress{source_slot},bits)){
                (void)co_await service_callback::call(method(destination,1),{destination});
                // AddRef 可以重入客體並替換來源；於 Release 前重新讀取。
                const auto source=m.load(GuestAddress{source_slot},bits);
                (void)co_await service_callback::call(method(source,2),{source});
                m.store(GuestAddress{source_slot},bits,0);
            }
            co_return finish(0);
        }
        const auto slot=temporary.allocate(16);if(!slot)co_return finish(0x8007000eU);
        const auto guid=[&](const Iid& iid){const auto at=temporary.allocate(16);if(at)m.write(GuestAddress{at},iid);return at;};
        const auto take=[&]{const auto value=m.load(GuestAddress{slot},bits);m.store(GuestAddress{slot},bits,0);return value;};
        if(symbol=="#355"){
            const auto unknown=call(0),enable=call(1);
            if(!unknown)co_return finish(0x80004005U);
            struct Interface {Iid iid;unsigned method;};
            static constexpr Interface interfaces[]{
                {{0x17,1,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},9}, // IOleInPlaceActiveObject
                {{0x16,1,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},13}, // IOleInPlaceFrame
                {{0xe2,0x14,2,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},9}, // IShellBrowser
                {{0xed,0xc9,0xea,0x79,0xf9,0xba,0xce,0x11,0x8c,0x82,0,0xaa,0,0x4b,0xa9,0x0b},4}, // IInternetSecurityMgrSite
                {{0xc0,0x23,0x3f,0xbd,0x3e,0xd4,0xcf,0x11,0x89,0x3b,0,0xaa,0,0xbd,0xce,0x1a},8} // IDocHostUIHandler
            };
            std::uint32_t hr=0x80004002U;
            for(const auto& iface:interfaces){
                const auto iid=guid(iface.iid);if(!iid)co_return finish(0x8007000eU);
                m.store(GuestAddress{slot},bits,0);
                hr=co_await service_callback::call(method(unknown,0),{unknown,iid,slot});
                if(failed(hr))continue;
                const auto object=take();if(!object)co_return finish(0x80004003U);
                (void)co_await service_callback::call(method(object,iface.method),{object,enable});
                (void)co_await service_callback::call(method(object,2),{object});
                co_return finish(0);
            }
            co_return finish(hr);
        }
        if(symbol=="#17" || symbol=="#18"){
            const auto stream=call(0),list=call(1);
            if(!stream || (symbol=="#18" && !list))co_return finish(0x80070057U);
            const auto count=slot+4;
            if(symbol=="#17"){
                auto cursor=list;
                for(;;){
                    const auto block=shell_data_block(m,cursor);
                    if(!block.span)break;
                    const auto hr=status(co_await service_callback::call(method(stream,4),{stream,block.payload,block.length,count}));
                    if(failed(hr))co_return finish(hr);
                    if(m.load(GuestAddress{count},32)!=block.length)co_return finish(0x80030070U);
                    cursor+=block.span;
                }
                m.store(GuestAddress{slot},32,0);
                co_return finish(status(co_await service_callback::call(method(stream,4),{stream,slot,4,count})));
            }
            const auto previous=m.load(GuestAddress{list},bits);
            if(previous)(void)co_await service_callback::call(entry("kernel32.dll","LocalFree"),{previous});
            m.store(GuestAddress{list},bits,0);
            for(;;){
                auto hr=status(co_await service_callback::call(method(stream,3),{stream,slot,4,count}));
                const auto length=m.load(GuestAddress{slot},32);
                if(failed(hr) || m.load(GuestAddress{count},32)!=4 || !length)co_return finish(hr);
                if(length>UINT16_MAX){
                    // 將超限記錄的長度留給下一位串流讀取者。
                    const auto seek=method(stream,5);
                    const auto query=x86_?co_await service_callback::call(seek,{stream,0,0,1,slot+8}):
                                           co_await service_callback::call(seek,{stream,0,1,slot+8});
                    if(!failed(status(query))){
                        const auto offset=m.load(GuestAddress{slot+8},64)-4;
                        if(x86_)(void)co_await service_callback::call(seek,{stream,status(offset),offset>>32,0,0});
                        else (void)co_await service_callback::call(seek,{stream,offset,0,0});
                    }
                    co_return finish(hr);
                }
                if(length<8)continue;
                GuestScratch scratch(*this,m,s);
                const auto item=scratch.allocate(length);
                if(!item)co_return finish(0x8007000eU);
                m.store(GuestAddress{item},32,length);
                hr=status(co_await service_callback::call(method(stream,3),{stream,item+4,length-4,count}));
                if(failed(hr) || m.load(GuestAddress{count},32)!=length-4)co_return finish(hr);
                if(m.load(GuestAddress{item+4},32)!=UINT32_MAX &&
                   !(co_await service_callback::call(entry("shlwapi.dll","#20"),{list,item})))co_return finish(0x8007000eU);
            }
        }
        if(symbol=="IStream_Read" || symbol=="IStream_Write"){
            // (stream, buffer, cb)：必須處理完全部位元組，否則回 E_FAIL。
            if(!call(0))co_return finish(0x80070057U);m.store(GuestAddress{slot},32,0);const auto bytes=static_cast<std::uint32_t>(call(2));
            const auto result=status(co_await service_callback::call(method(call(0),symbol=="IStream_Read"?3:4),{call(0),call(1),bytes,slot}));
            if(failed(result))co_return finish(result);co_return finish(m.load(GuestAddress{slot},32)==bytes?result:0x80004005U);
        }
        if(symbol=="IStream_Reset"){
            // Seek(0, STREAM_SEEK_SET, NULL)；LARGE_INTEGER 在 x86 佔兩格。
            if(!call(0))co_return finish(0x80070057U);
            if(x86_)co_return finish(status(co_await service_callback::call(method(call(0),5),{call(0),0,0,0,0})));
            co_return finish(status(co_await service_callback::call(method(call(0),5),{call(0),0,0,0})));
        }
        if(symbol=="IStream_Size" || symbol=="#166"){
            const bool check_empty=symbol=="#166";
            const auto stream=call(0);
            if(!stream)co_return finish(check_empty?1:0x80070057U);
            const auto stat=temporary.allocate(96);if(!stat)co_return finish(check_empty?1:0x8007000eU);
            const auto result=status(co_await service_callback::call(method(stream,12),{stream,stat,1}));
            if(!failed(result)){
                const auto length=m.load(GuestAddress{stat+(x86_?8U:16U)},64);
                if(check_empty)co_return finish(length==0);
                if(call(1))m.store(GuestAddress{call(1)},64,length);
            }
            if(!check_empty)co_return finish(result);
            // Stat 失敗時從當前位置試讀 DWORD；依 參考實作 契約，只有讀滿才重設位置。
            const auto read=status(co_await service_callback::call(method(stream,3),{stream,slot+4,4,slot}));
            if(failed(read) || m.load(GuestAddress{slot},32)!=4)co_return finish(1);
            (void)co_await service_callback::call(entry("shlwapi.dll","IStream_Reset"),{stream});
            co_return finish(0);
        }
        if(symbol=="#163" || symbol=="#164" || symbol=="#484"){
            const auto unknown=call(0);
            if(!unknown)co_return finish(0x80004005U);
            const bool service_exec=symbol=="#484",query_status=symbol=="#163";
            const auto command_iid=guid(iid_command_target);
            const auto iid=service_exec?guid(iid_service_provider):command_iid;
            if(!iid || !command_iid)co_return finish(0x8007000eU);
            auto result=status(co_await service_callback::call(method(unknown,0),{unknown,iid,slot}));
            if(failed(result) || (service_exec && result!=0))co_return finish(result);
            auto target=take();
            if(service_exec && target){
                const auto provider=target;
                result=status(co_await service_callback::call(method(provider,3),{provider,call(1),command_iid,slot}));
                (void)co_await service_callback::call(method(provider,2),{provider});
                if(result!=0)co_return finish(result);
                target=take();
            }
            if(target){
                std::vector<std::uint64_t> args{target};
                for(unsigned i=service_exec?2:1;i<service.arguments;++i)args.push_back(call(i));
                result=status(co_await service_callback::call(method(target,query_status?3:4),std::move(args)));
                (void)co_await service_callback::call(method(target,2),{target});
            }
            co_return finish(result);
        }
        // 各輸入介面沿用相同的 QueryInterface／方法呼叫／Release 生命週期。
        struct InputMethod {std::string_view ordinal;const Iid& iid;unsigned index,arguments;};
        static constexpr InputMethod input_methods[]{
            {"#188",iid_control_site,7,2},{"#189",iid_control_site,8,1},
            {"#478",iid_input_object,5,1},{"#479",iid_input_object,3,2},
            {"#481",iid_input_object,4,0},{"#509",iid_input_site,3,2}
        };
        for(const auto& input:input_methods){
            if(symbol!=input.ordinal)continue;
            const auto unknown=call(0);
            if(!unknown)co_return finish(symbol=="#189" || symbol=="#479"?0x80004005U:0x80070057U);
            const auto iid=guid(input.iid);if(!iid)co_return finish(0x8007000eU);
            auto result=status(co_await service_callback::call(method(unknown,0),{unknown,iid,slot}));
            if(failed(result) || (symbol=="#479" && result!=0))co_return finish(result);
            const auto object=take();
            if(object){
                std::vector<std::uint64_t> args{object};
                for(unsigned i=1;i<=input.arguments;++i)args.push_back(call(i));
                result=status(co_await service_callback::call(method(object,input.index),std::move(args)));
                (void)co_await service_callback::call(method(object,2),{object});
            }
            co_return finish(result);
        }
        if(symbol=="IUnknown_GetSite" || symbol=="IUnknown_SetSite"){
            const bool get=symbol=="IUnknown_GetSite";
            const auto object=call(0),site=call(1),output=get?call(2):0;
            if(get && (!object || !site || !output))co_return finish(0x80070057U);
            if(!object)co_return finish(0x80004005U);
            const auto site_iid=guid(iid_object_with_site);
            if(!site_iid)co_return finish(0x8007000eU);
            m.store(GuestAddress{slot},bits,0);
            auto hr=status(co_await service_callback::call(method(object,0),{object,site_iid,slot}));
            if(!failed(hr)){
                const auto holder=take();
                if(!holder)co_return finish(hr);
                if(get)hr=status(co_await service_callback::call(method(holder,4),{holder,site,output}));
                else hr=status(co_await service_callback::call(method(holder,3),{holder,site}));
                (void)co_await service_callback::call(method(holder,2),{holder});
                co_return finish(hr);
            }
            if(get)co_return finish(hr);
            // 安全管理員直接接收呼叫端傳入的 site 介面，包含解除關聯用的 NULL。
            const auto manager_iid=guid(iid_security_manager);
            if(!manager_iid)co_return finish(0x8007000eU);
            m.store(GuestAddress{slot},bits,0);
            hr=status(co_await service_callback::call(method(object,0),{object,manager_iid,slot}));
            if(failed(hr))co_return finish(hr);
            const auto manager=take();
            if(!manager)co_return finish(hr);
            hr=status(co_await service_callback::call(method(manager,3),{manager,site}));
            (void)co_await service_callback::call(method(manager,2),{manager});
            co_return finish(hr);
        }
        if(symbol=="SHGetThreadRef" || symbol=="SHGetInstanceExplorer" || symbol=="_SHGetInstanceExplorer"){
            // 回傳已 AddRef 的參照；沒有時執行緒參照回 E_NOINTERFACE，Explorer 參照回 E_FAIL。
            if(!call(0))co_return finish(0x80070057U);m.store(GuestAddress{call(0)},bits,0);const bool thread=symbol=="SHGetThreadRef";
            std::uint64_t object=0;if(thread){const auto found=shell_thread_refs_.find(threads_.current);object=found==shell_thread_refs_.end()?0:found->second;}
            else if(shell_instance_explorer_)object=m.load(GuestAddress{shell_instance_explorer_},bits);
            if(!object)co_return finish(thread?0x80004002U:0x80004005U);
            if(!thread)m.store(GuestAddress{call(0)},bits,object);
            (void)co_await service_callback::call(method(object,1),{object});
            if(thread)m.store(GuestAddress{call(0)},bits,object);
            co_return finish(0);
        }
        if(symbol=="SHReleaseThreadRef"){
            // 清除目前執行緒的參照並釋放一次（SHCreateThread 以 CTF_THREAD_REF 交給新執行緒的參照）。
            const auto found=shell_thread_refs_.find(threads_.current);if(found==shell_thread_refs_.end())co_return finish(0x80004002U);
            const auto object=found->second;shell_thread_refs_.erase(found);(void)co_await service_callback::call(method(object,2),{object});co_return finish(0);
        }
        const auto text_at=[&](std::uint64_t address)->std::u16string{if(!address)return {};return wide?read_utf16(m,GuestAddress{address},32768):utf8_to_utf16_lossy(read_ansi(m,address,32768));};
        if(symbol=="#542" || symbol=="WhichPlatform"){
            const auto library=temporary.text(u"shell32.dll"),info=temporary.allocate(20),proc=temporary.allocate(14);
            if(!library || !proc || !info)co_return finish(0);
            constexpr std::uint8_t version_name[]="DllGetVersion";
            m.write(GuestAddress{proc},version_name);
            const auto module=handle_of(co_await service_callback::call_value(entry("kernel32.dll","LoadLibraryW"),{library}));
            const auto fn=module?handle_of(co_await service_callback::call_value(entry("kernel32.dll","GetProcAddress"),{module,proc})):0;
            if(symbol=="WhichPlatform"){
                if(module)(void)co_await service_callback::call(entry("kernel32.dll","FreeLibrary"),{module});
                // DLL 回呼期間可能有另一執行緒已完成同一程序的查詢。
                if(shell_browser_platform_)co_return finish(shell_browser_platform_);
                shell_browser_platform_=fn?2:1;
                const auto path=temporary.text(u"Software\\Microsoft\\Internet Explorer");
                const auto value=temporary.text(u"IntegratedBrowser");
                if(!path || !value)co_return finish(shell_browser_platform_);
                const auto root=handle_of(UINT64_C(0xffffffff80000002));
                const auto opened=co_await service_callback::call(entry("advapi32.dll","RegOpenKeyExW"),{root,path,0,0xf003f,slot});
                if(!opened){
                    const auto key=m.load(GuestAddress{slot},bits);
                    m.store(GuestAddress{info},32,4);
                    const auto query=co_await service_callback::call(entry("advapi32.dll","RegQueryValueExW"),{key,value,0,0,info+4,info});
                    if(query && fn){
                        m.store(GuestAddress{info+4},32,1);
                        (void)co_await service_callback::call(entry("advapi32.dll","RegSetValueExW"),{key,value,0,4,info+4,4});
                    }else if(!query && !fn){
                        (void)co_await service_callback::call(entry("advapi32.dll","RegDeleteValueW"),{key,value});
                    }
                    (void)co_await service_callback::call(entry("advapi32.dll","RegCloseKey"),{key});
                }
                co_return finish(shell_browser_platform_);
            }
            if(!module)co_return finish(0);
            std::uint32_t major=0;
            if(fn){
                m.store(GuestAddress{info},32,20);
                if(!status(co_await service_callback::call(fn,{info})))
                    major=static_cast<std::uint32_t>(m.load(GuestAddress{info+4},32));
            }
            (void)co_await service_callback::call(entry("kernel32.dll","FreeLibrary"),{module});
            co_return finish(major?major:3);
        }
        if(service.family=="internal-dialog-font"){
            const auto parent=call(0),property=temporary.text(u"PropDlgFont");
            if(!property)co_return finish(symbol=="SHRemoveDefaultDialogFont"?0:0x8007000eU);
            if(symbol=="SHRemoveDefaultDialogFont"){
                const auto font=handle_of(co_await service_callback::call_value(entry("user32.dll","RemovePropW"),{parent,property}));
                if(font)(void)co_await service_callback::call(entry("gdi32.dll","DeleteObject"),{font});
                co_return finish(font);
            }
            const auto child=handle_of(co_await service_callback::call_value(entry("user32.dll","GetDlgItem"),{parent,call(1)}));
            if(!child)co_return finish(0);
            auto original=handle_of(co_await service_callback::call_value(entry("user32.dll","SendMessageW"),{parent,0x31,0,0}));
            if(!original)original=handle_of(co_await service_callback::call_value(entry("gdi32.dll","GetStockObject"),{17}));
            const auto current=temporary.allocate(92),system=temporary.allocate(92);
            if(!current || !system)co_return finish(0x8007000eU);
            if(!co_await service_callback::call(entry("gdi32.dll","GetObjectW"),{original,92,current}) ||
               !co_await service_callback::call(entry("user32.dll","SystemParametersInfoW"),{0x1f,92,system,0}))co_return finish(0);
            if(m.load(GuestAddress{current+23},8)==m.load(GuestAddress{system+23},8))co_return finish(0);
            auto font=handle_of(co_await service_callback::call_value(entry("user32.dll","GetPropW"),{parent,property}));
            if(!font){
                m.store(GuestAddress{system},32,m.load(GuestAddress{current},32));
                font=handle_of(co_await service_callback::call_value(entry("gdi32.dll","CreateFontIndirectW"),{system}));
                if(font && !co_await service_callback::call(entry("user32.dll","SetPropW"),{parent,property,font})){
                    (void)co_await service_callback::call(entry("gdi32.dll","DeleteObject"),{font});
                    font=0;
                }
            }
            (void)co_await service_callback::call(entry("user32.dll","SendMessageW"),{child,0x30,font?font:original,0});
            co_return finish(0);
        }
        if(symbol=="MLFreeLibrary"){
            const auto module=handle_of(call(0));
            (void)co_await service_callback::call(entry("shlwapi.dll","#431"),{module});
            co_return finish(co_await service_callback::call(entry("kernel32.dll","FreeLibrary"),{module}));
        }
        if(symbol=="#405" || symbol=="#406"){
            const bool unicode=symbol=="#406";
            const auto module=handle_of(call(1)),dest=call(4);
            const auto flags=status(call(2));
            if(!call(0) || !module || module==(x86_?UINT32_MAX:UINT64_MAX) || !call(3) || !dest || (flags && flags!=2))
                co_return finish(0x80070057U);
            const auto capacity=unicode?status(call(5)):std::min(status(call(5)),260U);
            if(capacity<7)co_return finish(0x80004005U);
            const auto read=[&](std::uint64_t address){return unicode?read_utf16(m,GuestAddress{address}):utf8_to_utf16_lossy(read_ansi(m,address));};
            const auto file=temporary.text(read(call(0)));
            const auto resource=read(call(3));
            std::u16string url=u"res://";
            if(unicode)m.write(GuestAddress{dest},utf16_bytes(url));
            if(!file)co_return finish(0x8007000eU);
            const auto library=handle_of(co_await service_callback::call_value(entry("shlwapi.dll","MLLoadLibraryW"),{file,module,flags}));
            if(!library)co_return finish(0x80004005U);
            const auto* image=resource_image(library);
            bool complete=false;
            if(image && !image->path.empty() && image->path.size()<260 && capacity>=8+image->path.size()){
                url+=image->path;
                if(capacity>=10+image->path.size()+resource.size()){
                    url+=u'/';url+=resource;complete=true;
                }
                if(unicode)m.write(GuestAddress{dest},utf16_bytes(url));
            }
            (void)co_await service_callback::call(entry("shlwapi.dll","MLFreeLibrary"),{library});
            if(!unicode && complete){
                const auto text=temporary.text(url);
                if(text)(void)co_await service_callback::call(entry("kernel32.dll","WideCharToMultiByte"),{0,0,text,UINT32_MAX,dest,capacity,0,0});
            }
            co_return finish(complete?0:0x80004005U);
        }
        if(symbol.starts_with("MLLoadLibrary")){
            // (檔名, HMODULE 基準, 跨碼頁旗標)：依序找 <模組目錄>\mui\<UI 語言>\檔名、<模組目錄>\檔名，以資料檔方式載入；最後交給一般搜尋。
            const auto file=text_at(call(0));if(file.empty())co_return finish(0);const auto buffer=temporary.allocate(2*32768);if(!buffer)co_return finish(0);
            const auto length=static_cast<std::uint32_t>(co_await service_callback::call(entry("kernel32.dll","GetModuleFileNameW"),{call(1),buffer,32767}));
            std::vector<std::u16string> candidates;
            const auto language=static_cast<std::uint16_t>(co_await service_callback::call(entry("kernel32.dll","GetUserDefaultUILanguage"),{}));
            if(length && length<32767){auto directory=read_utf16(m,GuestAddress{buffer},32767);directory.resize(directory.find_last_of(u'\\')+1);
                constexpr std::u16string_view hex=u"0123456789ABCDEF";std::u16string code;for(int shift=12;shift>=0;shift-=4)code.push_back(hex[(language>>shift)&15U]);
                candidates.push_back(directory+u"mui\\"+code+u"\\"+file);candidates.push_back(directory+file);}
            candidates.push_back(file);
            for(const auto& candidate:candidates){const auto path=temporary.text(candidate);if(!path)break;
                if(&candidate!=&candidates.back() && static_cast<std::uint32_t>(co_await service_callback::call(entry("kernel32.dll","GetFileAttributesW"),{path}))==UINT32_MAX)continue;
                const auto module=handle_of(co_await service_callback::call_value(entry("kernel32.dll","LoadLibraryExW"),{path,0,0x22}));if(module){
                    const auto lang=(&candidate==&candidates.front() && candidates.size()>1)?language:
                        static_cast<std::uint16_t>(co_await service_callback::call(entry("kernel32.dll","GetSystemDefaultUILanguage"),{}));
                    (void)co_await service_callback::call(entry("shlwapi.dll","#430"),{module,lang});
                    co_return finish(module);
                }} // AS_DATAFILE | AS_IMAGE_RESOURCE
            co_return finish(0);
        }
        if(symbol=="SHSkipJunction"){
            // 繫結內容的 "Skip Binding CLSID" 物件參數，其 IPersist::GetClassID 與指定 CLSID 相同時回 TRUE。
            if(!call(0) || !call(1))co_return finish(0);const auto key=temporary.text(u"Skip Binding CLSID");const auto persist_iid=guid(Iid{0x0c,0x01,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46});const auto clsid=temporary.allocate(16);
            if(!key || !persist_iid || !clsid)co_return finish(0);m.store(GuestAddress{slot},bits,0);
            if(failed(status(co_await service_callback::call(method(call(0),10),{call(0),key,slot}))))co_return finish(0);const auto object=take();if(!object)co_return finish(0);
            bool same=false;
            if(!failed(status(co_await service_callback::call(method(object,0),{object,persist_iid,slot})))){const auto persist=take();
                if(!failed(status(co_await service_callback::call(method(persist,3),{persist,clsid})))){std::array<std::uint8_t,16> a{},b{};m.read(GuestAddress{clsid},a);m.read(GuestAddress{call(1)},b);same=a==b;}
                (void)co_await service_callback::call(method(persist,2),{persist});}
            (void)co_await service_callback::call(method(object,2),{object});co_return finish(same?1U:0U);
        }
        if(symbol.starts_with("SHMessageBoxCheck")){
            // (HWND, 內文, 標題, MB 類型, 預設回傳, 登錄值名稱)：HKCU\...\Explorer\DontShowMeThisDialogAgain 的值為 "NO" 時直接回傳預設值；
            // 否則以 TaskDialog 的驗證核取方塊顯示，勾選且未取消時寫入 "NO"。
            const auto text=text_at(call(1)),caption=text_at(call(2)),value=text_at(call(5));const auto type=static_cast<std::uint32_t>(call(3));
            const auto subkey=temporary.text(u"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\DontShowMeThisDialogAgain");const std::uint64_t current_user=x86_?0x80000001ULL:0xffffffff80000001ULL;
            if(!value.empty()){const auto name_text=temporary.text(value),data=temporary.allocate(64),capacity=temporary.allocate(4);
                if(subkey && name_text && data && capacity){m.store(GuestAddress{capacity},32,64);
                    if(!static_cast<std::uint32_t>(co_await service_callback::call(entry("advapi32.dll","RegGetValueW"),{current_user,subkey,name_text,2,0,data,capacity}))){
                        auto stored=read_utf16(m,GuestAddress{data},31);for(auto& c:stored)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);if(stored==u"NO")co_return finish(static_cast<std::uint32_t>(call(4)));}}}
            const unsigned config_size=x86_?96U:160U;const auto config=temporary.allocate(config_size),button=temporary.allocate(4),checked=temporary.allocate(4);
            const auto content=temporary.text(text),title=temporary.text(caption),verification=temporary.text(u"In the future, do not show me this dialog box");
            if(!config || !button || !checked || !content || !title || !verification)co_return finish(static_cast<std::uint32_t>(call(4)));
            m.write(GuestAddress{config},std::vector<std::uint8_t>(config_size));m.store(GuestAddress{button},32,0);m.store(GuestAddress{checked},32,0);
            const auto field=[&](unsigned x86_offset,unsigned x64_offset,std::uint64_t value_,bool pointer){m.store(GuestAddress{config+(x86_?x86_offset:x64_offset)},pointer?bits:32,value_);};
            const auto buttons=(type&15U)==1?9U:(type&15U)==4?6U:(type&15U)==3?14U:1U; // OK、OK｜CANCEL、YES｜NO、YES｜NO｜CANCEL
            const auto icon=type&0xf0U;const std::uint64_t main_icon=icon==0x10?0xfffe:icon==0x30?0xffff:(icon==0x40 || icon==0x20)?0xfffd:0;
            field(0,0,config_size,false);field(4,4,call(0),true);field(16,24,buttons,false);field(20,28,title,true);field(24,36,main_icon,true);field(32,52,content,true);field(60,92,verification,true);
            const auto result=static_cast<std::uint32_t>(co_await service_callback::call(entry("comctl32.dll","TaskDialogIndirect"),{config,button,0,checked}));
            std::uint32_t chosen=static_cast<std::uint32_t>(m.load(GuestAddress{button},32));
            if(failed(result)){const auto box=temporary.text(text);chosen=static_cast<std::uint32_t>(co_await service_callback::call(entry("user32.dll","MessageBoxW"),{call(0),box,title,type}));}
            else if(m.load(GuestAddress{checked},32) && chosen!=2 && !value.empty()){const auto name_text=temporary.text(value),no=temporary.text(u"NO");
                if(subkey && name_text && no)(void)co_await service_callback::call(entry("advapi32.dll","RegSetKeyValueW"),{current_user,subkey,name_text,1,no,6});}
            co_return finish(chosen);
        }
        if(symbol.starts_with("ShellMessageBox")){
            // (HINSTANCE, HWND, 內文, 標題, 樣式, ...)：內文與標題可為資源 ID；標題為 NULL 時取 HWND 的視窗標題。內文以 FormatMessage 的 %1… 代入可變引數後交給 MessageBox。
            const auto resource=[&](std::uint64_t id,std::uint64_t buffer)->bool{return id && !(id>>16) && buffer;};
            const auto text_buffer=temporary.allocate(2*1024),title_buffer=temporary.allocate(2*512),formatted=temporary.allocate(8),va=temporary.allocate(8);
            if(!text_buffer || !title_buffer || !formatted || !va)co_return finish(0);
            std::u16string text,title;bool has_title=true;
            if(resource(call(2),text_buffer)){if(co_await service_callback::call(entry("user32.dll","LoadStringW"),{call(0),call(2),text_buffer,1024}))text=read_utf16(m,GuestAddress{text_buffer},1024);}else text=text_at(call(2));
            if(!call(3)){if(call(1) && co_await service_callback::call(entry("user32.dll","GetWindowTextW"),{call(1),title_buffer,512}))title=read_utf16(m,GuestAddress{title_buffer},512);else has_title=false;}
            else if(resource(call(3),title_buffer)){if(co_await service_callback::call(entry("user32.dll","LoadStringW"),{call(0),call(3),title_buffer,512}))title=read_utf16(m,GuestAddress{title_buffer},512);}else title=text_at(call(3));
            m.store(GuestAddress{va},bits,variadic);m.store(GuestAddress{formatted},bits,0);const auto source=temporary.text(text);if(!source)co_return finish(0);
            const auto length=static_cast<std::uint32_t>(co_await service_callback::call(entry("kernel32.dll","FormatMessageW"),{0x500,source,0,0,formatted,0,va})); // FROM_STRING｜ALLOCATE_BUFFER
            const auto message=m.load(GuestAddress{formatted},bits);const auto shown=length && message?message:source;const auto caption=has_title?temporary.text(title):0;
            const auto result=co_await service_callback::call(entry("user32.dll","MessageBoxW"),{call(1),shown,caption,static_cast<std::uint32_t>(call(4))});
            if(message)(void)co_await service_callback::call(entry("kernel32.dll","LocalFree"),{message});co_return finish(result);
        }
        if(symbol=="SHCreateThread"){
            // (ThreadProc, pData, CTF_*, Callback)：新執行緒先執行 Callback，呼叫端等它結束後返回，再執行 ThreadProc。
            // INSIST 1：無法建立時同步執行；THREAD_REF 2：新執行緒持有呼叫端的執行緒參照（呼叫端沒有時失敗）；PROCESS_REF 4：持有 Explorer 參照；
            // COINIT_STA 8／COINIT_MTA 0x1000／OLEINITIALIZE 0x800；FREELIBANDEXIT 0x10：持有 ThreadProc 所在模組並以 FreeLibraryAndExitThread 結束；
            // REF_COUNTED 0x20：新執行緒以 SHCreateThreadRef 物件為執行緒參照，ThreadProc 結束後抽取訊息直到參照歸零。
            const auto proc=call(0),data=call(1),callback=call(3);const auto flags=static_cast<std::uint32_t>(call(2));if(!proc)co_return finish(0);
            std::uint64_t thread_ref=0,process_ref=0,module=0;
            if(flags&2U){const auto found=shell_thread_refs_.find(threads_.current);if(found==shell_thread_refs_.end())co_return finish(0);thread_ref=found->second;(void)co_await service_callback::call(method(thread_ref,1),{thread_ref});}
            if((flags&4U) && shell_instance_explorer_){process_ref=m.load(GuestAddress{shell_instance_explorer_},bits);if(process_ref)(void)co_await service_callback::call(method(process_ref,1),{process_ref});}
            if(flags&0x10U){m.store(GuestAddress{slot},bits,0);if(co_await service_callback::call(entry("kernel32.dll","GetModuleHandleExW"),{4,proc,slot}))module=take();}
            const auto context=allocate_heap(m,80);const auto event=context?handle_of(co_await service_callback::call_value(entry("kernel32.dll","CreateEventW"),{0,1,0,0})):0;
            std::uint64_t thread=0;
            if(context && event){
                for(unsigned i=0;i<10;++i)m.store(GuestAddress{context+i*8},64,0);
                m.store(GuestAddress{context},64,proc);m.store(GuestAddress{context+8},64,data);m.store(GuestAddress{context+16},64,flags);m.store(GuestAddress{context+24},64,callback);
                m.store(GuestAddress{context+32},64,event);m.store(GuestAddress{context+40},64,thread_ref);m.store(GuestAddress{context+48},64,process_ref);m.store(GuestAddress{context+56},64,module);
                thread=handle_of(co_await service_callback::call_value(entry("kernel32.dll","CreateThread"),{0,0,resolve("internal-shell-thread","Start").value,context,0,0}));
            }
            if(thread){(void)co_await service_callback::call(entry("kernel32.dll","WaitForSingleObject"),{event,UINT32_MAX});
                (void)co_await service_callback::call(entry("kernel32.dll","CloseHandle"),{thread});(void)co_await service_callback::call(entry("kernel32.dll","CloseHandle"),{event});co_return finish(1);}
            if(event)(void)co_await service_callback::call(entry("kernel32.dll","CloseHandle"),{event});if(context)(void)free_heap(m,context,s);
            if(thread_ref)(void)co_await service_callback::call(method(thread_ref,2),{thread_ref});if(process_ref)(void)co_await service_callback::call(method(process_ref,2),{process_ref});
            if(module)(void)co_await service_callback::call(entry("kernel32.dll","FreeLibrary"),{module});
            if(!(flags&1U))co_return finish(0);
            if(callback)(void)co_await service_callback::call(callback,{data});(void)co_await service_callback::call(proc,{data});co_return finish(1);
        }
        if(service.family=="internal-shell-thread"){
            const auto context=call(0);const auto field=[&](unsigned index){return m.load(GuestAddress{context+index*8},64);};
            const auto proc=field(0),data=field(1),callback=field(3),event=field(4),thread_ref=field(5),process_ref=field(6),module=field(7);const auto flags=static_cast<std::uint32_t>(field(2));
            bool com=false,ole=false;
            if(flags&0x800U)ole=!failed(status(co_await service_callback::call(entry("ole32.dll","OleInitialize"),{0})));
            else if(flags&0x1008U)com=!failed(status(co_await service_callback::call(entry("ole32.dll","CoInitializeEx"),{0,(flags&8U)?2U:0U})));
            if(thread_ref)shell_thread_refs_[threads_.current]=thread_ref;
            std::uint64_t counted=0,counted_event=0;const auto count=context+64;
            if(flags&0x20U){counted_event=handle_of(co_await service_callback::call_value(entry("kernel32.dll","CreateEventW"),{0,1,0,0}));
                m.store(GuestAddress{slot},bits,0);if(counted_event && !failed(status(co_await service_callback::call(resolve("shlwapi.dll","SHCreateThreadRef").value,{count,slot})))){
                    counted=take();m.store(GuestAddress{counted+2*size},bits,counted_event);if(!thread_ref)shell_thread_refs_[threads_.current]=counted;}}
            if(callback)(void)co_await service_callback::call(callback,{data});
            (void)co_await service_callback::call(entry("kernel32.dll","SetEvent"),{event});
            const auto result=static_cast<std::uint32_t>(co_await service_callback::call(proc,{data}));
            if(counted){
                if(const auto found=shell_thread_refs_.find(threads_.current);found!=shell_thread_refs_.end() && found->second==counted)shell_thread_refs_.erase(found);
                (void)co_await service_callback::call(method(counted,2),{counted});
                const auto handles=temporary.allocate(8),message=temporary.allocate(64);
                if(handles && message){m.store(GuestAddress{handles},bits,counted_event);
                    while(m.load(GuestAddress{count},32)){
                        const auto wait=static_cast<std::uint32_t>(co_await service_callback::call(entry("user32.dll","MsgWaitForMultipleObjects"),{1,handles,0,UINT32_MAX,0x4ff}));
                        if(wait!=1)break;
                        while(co_await service_callback::call(entry("user32.dll","PeekMessageW"),{message,0,0,0,1})){
                            (void)co_await service_callback::call(entry("user32.dll","TranslateMessage"),{message});(void)co_await service_callback::call(entry("user32.dll","DispatchMessageW"),{message});}
                    }}
                (void)co_await service_callback::call(entry("kernel32.dll","CloseHandle"),{counted_event});
            }
            if(thread_ref)if(const auto found=shell_thread_refs_.find(threads_.current);found!=shell_thread_refs_.end() && found->second==thread_ref){shell_thread_refs_.erase(found);(void)co_await service_callback::call(method(thread_ref,2),{thread_ref});}
            if(process_ref)(void)co_await service_callback::call(method(process_ref,2),{process_ref});
            if(com)(void)co_await service_callback::call(entry("ole32.dll","CoUninitialize"),{});if(ole)(void)co_await service_callback::call(entry("ole32.dll","OleUninitialize"),{});
            (void)free_heap(m,context,s);
            if(module)(void)co_await service_callback::call(entry("kernel32.dll","FreeLibraryAndExitThread"),{module,result});
            co_return finish(result);
        }
        if(symbol.starts_with("SHSendMessageBroadcast")){
            co_return finish(status(co_await service_callback::call(entry("user32.dll",wide?"SendMessageTimeoutW":"SendMessageTimeoutA"),
                {0xffff,static_cast<std::uint32_t>(call(0)),call(1),call(2),2,2000,0})));
        }
        throw HostCallFault("未知的 shlwapi COM 輔助服務");
    });
}
}
