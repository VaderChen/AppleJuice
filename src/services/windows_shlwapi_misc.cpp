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
constexpr Iid iid_security_site{0xed,0xc9,0xea,0x79,0xf9,0xba,0xce,0x11,0x8c,0x82,0x00,0xaa,0x00,0x4b,0xa9,0x0b};
constexpr Iid iid_ole_window{0x14,0x01,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Iid iid_shell_view{0xe3,0x14,0x02,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Iid iid_point_container{0x84,0xb2,0x96,0xb1,0xb4,0xba,0x1a,0x10,0xb6,0x9c,0x00,0xaa,0x00,0x34,0x1d,0x07};
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
        {"shlwapi.dll","#542",0},
        {"shlwapi.dll","#35",3,0,0,0x2},
        {"shlwapi.dll","ColorRGBToHLS",4,0,0,0x1},{"shlwapi.dll","ColorHLSToRGB",3,0,0,0x7},{"shlwapi.dll","ColorAdjustLuma",3,0,0,0x7},
        {"shlwapi.dll","GetMenuPosFromID",2,0,0,0x2},{"shlwapi.dll","GetAcceptLanguagesA",2},{"shlwapi.dll","GetAcceptLanguagesW",2},
        {"internal-shell-shared","SHMapHandle",5,0,0,0x1e},{"internal-shell-shared","SHLockSharedEx",3,0,0,0x6},
        {"shlwapi.dll","SHAllocShared",3,0,0,0x6},{"shlwapi.dll","SHLockShared",2,0,0,0x2},{"shlwapi.dll","SHUnlockShared",1},{"shlwapi.dll","SHFreeShared",2,0,0,0x2},
        {"shlwapi.dll","WhichPlatform",0},{"shlwapi.dll","SHIsLowMemoryMachine",1,0,0,0x1},{"shlwapi.dll","IsInternetESCEnabled",0},
        {"shlwapi.dll","IStream_Read",3,0,0,0x4},{"shlwapi.dll","IStream_Write",3,0,0,0x4},{"shlwapi.dll","IStream_Reset",1},{"shlwapi.dll","IStream_Size",2},
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
#include "windows_shlwapi_hash.inc"
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
    if(name=="GetMenuPosFromID"){
        // 回傳命令 ID 相符項目的位置；子選單項目的 ID 為 -1，不會相符。找不到時回 -1。
        const auto count=static_cast<std::int32_t>(static_cast<std::uint32_t>(host("user32.dll","GetMenuItemCount",{arg(0)})));
        for(std::int32_t i=0;i<count;++i)if(static_cast<std::uint32_t>(host("user32.dll","GetMenuItemID",{arg(0),static_cast<std::uint32_t>(i)}))==u32(1))return done(static_cast<std::uint32_t>(i));
        return done(UINT32_MAX);
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
    if(name=="WhichPlatform")return done(2); // PLATFORM_INTEGRATED：殼層與瀏覽器整合（Windows 2000 以後皆是）
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
    if(name=="SHSetInstanceExplorer"){shell_instance_explorer_=arg(0);return done(0);}
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
        // 修正 scheme 拼字：已知 scheme 原樣回 S_OK；去除重複字母後相同或只差一個字元，且只對應一個已知 scheme 時改正並回 S_OK；無法辨識回 S_FALSE（輸出原字串）。
        if(!arg(0) || !arg(1) || !u32(2))return done(0x80070057U);auto url=text_at(arg(0));const auto colon=url.find(u':');std::uint32_t status=1;
        static constexpr std::u16string_view schemes[]{u"http",u"https",u"ftp",u"file",u"mailto",u"news",u"nntp",u"telnet",u"gopher"};
        if(colon!=url.npos && colon){
            std::u16string scheme;for(std::size_t i=0;i<colon;++i){auto c=url[i];if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);scheme.push_back(c);}
            const auto collapse=[](std::u16string_view text){std::u16string out;for(const auto c:text)if(out.empty() || out.back()!=c)out.push_back(c);return out;};
            const auto near=[](std::u16string_view a,std::u16string_view b){
                if(a.size()==b.size()){std::size_t diff=0;for(std::size_t i=0;i<a.size();++i)diff+=a[i]!=b[i];return diff==1;}
                if(a.size()+1!=b.size() && b.size()+1!=a.size())return false;const auto& longer=a.size()>b.size()?a:b;const auto& shorter=a.size()>b.size()?b:a;
                for(std::size_t i=0;i<longer.size();++i){std::u16string removed(longer);removed.erase(i,1);if(removed==shorter)return true;}return false;};
            if(std::find(std::begin(schemes),std::end(schemes),scheme)!=std::end(schemes))status=0;
            else {std::u16string_view match;unsigned matches=0;for(const auto candidate:schemes)if(collapse(scheme)==collapse(candidate) || (candidate.size()>=3 && near(scheme,candidate))){match=candidate;++matches;}
                if(matches==1){url=std::u16string(match)+url.substr(colon);status=0;}}
        }
        if(url.size()>=u32(2))return done(0x80004003U);write_text(arg(1),u32(2),url);return done(status);
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
        // (HKEY, subkey, value, grfMode)：讀取登錄值到記憶體 IStream；寫入模式在最後一次 Release 時以 REG_BINARY 寫回。
        // SHOpenRegStream2 失敗回 NULL；舊版 SHOpenRegStream 失敗時回傳空的唯讀資料流。
        const bool legacy=!name.starts_with("SHOpenRegStream2");const auto mode=u32(3);const bool write=(mode&3U)!=0;
        const auto empty=[&]{return legacy?handle(host("shlwapi.dll","SHCreateMemStream",{0,0})):0;};
        GuestScratch temporary(*this,memory,state);const auto subkey=arg(1)?temporary.text(text_at(arg(1))):0,value=arg(2)?temporary.text(text_at(arg(2))):0;
        const auto key_slot=temporary.allocate(8),type=temporary.allocate(4),size=temporary.allocate(4);if(!key_slot || !type || !size)return done(empty());memory.store(GuestAddress{key_slot},64,0);
        const auto opened=write?host("advapi32.dll","RegCreateKeyExW",{arg(0),subkey,0,0,0,0x2001f,0,key_slot,0}):host("advapi32.dll","RegOpenKeyExW",{arg(0),subkey,0,0x20019,key_slot});
        if(opened&UINT32_MAX)return done(empty());const auto key=handle(memory.load(GuestAddress{key_slot},pointer_bits));
        memory.store(GuestAddress{size},32,0);std::uint64_t data=0;std::uint32_t length=0;
        if(!(host("advapi32.dll","RegQueryValueExW",{key,value,0,type,0,size})&UINT32_MAX) && !(write && (mode&0x1000U))){
            length=static_cast<std::uint32_t>(memory.load(GuestAddress{size},32));if(length){data=temporary.allocate(length);if(!data || (host("advapi32.dll","RegQueryValueExW",{key,value,0,type,data,size})&UINT32_MAX))length=0;}}
        else if(!write){(void)host("advapi32.dll","RegCloseKey",{key});return done(empty());}
        const auto stream=handle(host("shlwapi.dll","SHCreateMemStream",{length?data:0,length}));
        if(!stream){(void)host("advapi32.dll","RegCloseKey",{key});return done(empty());}
        if(write)(void)bind_registry_stream(stream,key,arg(2)?text_at(arg(2)):std::u16string{});else (void)host("advapi32.dll","RegCloseKey",{key});
        return done(stream);
    }
    if(name=="SHIsChildOrSelf"){
        // 相同視窗或子孫視窗回 S_OK，否則 S_FALSE。
        if(arg(0)==arg(1) && arg(0))return done(0);return done(arg(0) && arg(1) && (host("user32.dll","IsChild",{arg(0),arg(1)})&UINT32_MAX)?0U:1U);
    }
    return nested_continuation(state,memory,"ShlwapiCom",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto symbol=service.symbol;const unsigned size=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(size*8);
        const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const auto method=[&](std::uint64_t object,unsigned index){return m.load(GuestAddress{m.load(GuestAddress{object},bits)+index*size},bits);};
        const auto failed=[](std::uint32_t value){return (value&0x80000000U)!=0;};
        const auto status=[](std::uint64_t value){return static_cast<std::uint32_t>(value);};
        // ShellMessageBox 的可變引數起點（第 5 格；x86 為 ESP+4+5*4，x64 為 RSP+8+5*8），供 FormatMessage 當 va_list。
        const auto variadic=s.reg(Gpr::Rsp)+(x86_?24U:48U);(void)variadic;
        GuestScratch temporary(*this,m,s);const auto slot=temporary.allocate(16);if(!slot)co_return finish(0x8007000eU);
        const auto guid=[&](const Iid& iid){const auto at=temporary.allocate(16);if(at)m.write(GuestAddress{at},iid);return at;};
        const auto take=[&]{const auto value=m.load(GuestAddress{slot},bits);m.store(GuestAddress{slot},bits,0);return value;};
        if(symbol=="IStream_Read" || symbol=="IStream_Write"){
            // (stream, buffer, cb)：必須處理完全部位元組，否則回 E_FAIL。
            if(!call(0))co_return finish(0x80070057U);m.store(GuestAddress{slot},32,0);const auto bytes=static_cast<std::uint32_t>(call(2));
            const auto result=status(co_await service_callback::call(method(call(0),symbol=="IStream_Read"?3:4),{call(0),call(1),bytes,slot}));
            if(failed(result))co_return finish(result);co_return finish(m.load(GuestAddress{slot},32)==bytes?0U:0x80004005U);
        }
        if(symbol=="IStream_Reset"){
            // Seek(0, STREAM_SEEK_SET, NULL)；LARGE_INTEGER 在 x86 佔兩格。
            if(!call(0))co_return finish(0x80070057U);
            if(x86_)co_return finish(status(co_await service_callback::call(method(call(0),5),{call(0),0,0,0,0})));
            co_return finish(status(co_await service_callback::call(method(call(0),5),{call(0),0,0,0})));
        }
        if(symbol=="IStream_Size"){
            // Stat(STATFLAG_NONAME) 的 cbSize（x86 位移 8、x64 位移 16）。
            if(!call(0) || !call(1))co_return finish(0x80070057U);const auto stat=temporary.allocate(96);if(!stat)co_return finish(0x8007000eU);
            const auto result=status(co_await service_callback::call(method(call(0),12),{call(0),stat,1}));
            if(!failed(result))m.store(GuestAddress{call(1)},64,m.load(GuestAddress{stat+(x86_?8U:16U)},64));co_return finish(result);
        }
        if(symbol=="IUnknown_AtomicRelease"){
            if(!call(0))co_return finish(0);const auto object=m.load(GuestAddress{call(0)},bits);if(!object)co_return finish(0);
            m.store(GuestAddress{call(0)},bits,0);(void)co_await service_callback::call(method(object,2),{object});co_return finish(0);
        }
        if(symbol=="IUnknown_Set"){
            // 先 AddRef 新值再釋放舊值，兩者相同時也安全。
            if(!call(0))co_return finish(0);const auto replacement=call(1);if(replacement)(void)co_await service_callback::call(method(replacement,1),{replacement});
            const auto previous=m.load(GuestAddress{call(0)},bits);m.store(GuestAddress{call(0)},bits,replacement);
            if(previous)(void)co_await service_callback::call(method(previous,2),{previous});co_return finish(0);
        }
        if(symbol=="IUnknown_GetSite" || symbol=="IUnknown_SetSite"){
            const bool get=symbol=="IUnknown_GetSite";if(get){if(!call(2))co_return finish(0x80070057U);m.store(GuestAddress{call(2)},bits,0);}
            if(!call(0))co_return finish(0x80004005U);const auto site_iid=guid(iid_object_with_site);if(!site_iid)co_return finish(0x8007000eU);
            auto result=status(co_await service_callback::call(method(call(0),0),{call(0),site_iid,slot}));
            if(!failed(result)){const auto holder=take();
                result=status(co_await service_callback::call(method(holder,get?4:3),get?std::vector<std::uint64_t>{holder,call(1),call(2)}:std::vector<std::uint64_t>{holder,call(1)}));
                (void)co_await service_callback::call(method(holder,2),{holder});co_return finish(result);}
            if(get)co_return finish(result);
            // 不支援 IObjectWithSite 時改用 IInternetSecurityManager::SetSecuritySite（站台需支援 IInternetSecurityMgrSite）。
            const auto manager_iid=guid(iid_security_manager),site_type=guid(iid_security_site);if(!manager_iid || !site_type)co_return finish(0x8007000eU);
            result=status(co_await service_callback::call(method(call(0),0),{call(0),manager_iid,slot}));if(failed(result))co_return finish(result);
            const auto manager=take();std::uint64_t security_site=0;
            if(call(1) && !failed(status(co_await service_callback::call(method(call(1),0),{call(1),site_type,slot}))))security_site=take();
            result=call(1) && !security_site?0x80004002U:status(co_await service_callback::call(method(manager,3),{manager,security_site}));
            if(security_site)(void)co_await service_callback::call(method(security_site,2),{security_site});
            (void)co_await service_callback::call(method(manager,2),{manager});co_return finish(result);
        }
        if(symbol=="IUnknown_GetWindow"){
            // 依序嘗試 IOleWindow、IInternetSecurityMgrSite、IShellView 的 GetWindow（皆為 vtable 第 3 格）。
            if(!call(0) || !call(1))co_return finish(0x80004005U);m.store(GuestAddress{call(1)},bits,0);
            for(const auto& iid:{iid_ole_window,iid_security_site,iid_shell_view}){
                const auto requested=guid(iid);if(!requested)co_return finish(0x8007000eU);
                if(failed(status(co_await service_callback::call(method(call(0),0),{call(0),requested,slot}))))continue;
                const auto window=take();const auto result=status(co_await service_callback::call(method(window,3),{window,call(1)}));
                (void)co_await service_callback::call(method(window,2),{window});co_return finish(result);
            }
            co_return finish(0x80004005U);
        }
        if(symbol=="ConnectToConnectionPoint"){
            // (sink, riid, fConnect, target, cookie*, IConnectionPoint** 可為 NULL)：經 IConnectionPointContainer::FindConnectionPoint 後 Advise／Unadvise。
            if(call(5))m.store(GuestAddress{call(5)},bits,0);if(!call(3) || !call(1))co_return finish(0x80070057U);
            const auto container_iid=guid(iid_point_container);if(!container_iid)co_return finish(0x8007000eU);
            auto result=status(co_await service_callback::call(method(call(3),0),{call(3),container_iid,slot}));if(failed(result))co_return finish(result);
            const auto container=take();result=status(co_await service_callback::call(method(container,4),{container,call(1),slot}));
            (void)co_await service_callback::call(method(container,2),{container});if(failed(result))co_return finish(result);
            const auto point=take();const bool connect=static_cast<std::uint32_t>(call(2))!=0;
            if(connect)result=call(0) && call(4)?status(co_await service_callback::call(method(point,5),{point,call(0),call(4)})):0x80070057U;
            else result=call(4)?status(co_await service_callback::call(method(point,6),{point,m.load(GuestAddress{call(4)},32)})):0x80070057U;
            if(call(5) && !failed(result))m.store(GuestAddress{call(5)},bits,point);else (void)co_await service_callback::call(method(point,2),{point});
            co_return finish(result);
        }
        if(symbol=="SHGetThreadRef" || symbol=="SHGetInstanceExplorer" || symbol=="_SHGetInstanceExplorer"){
            // 回傳已 AddRef 的參照；沒有時執行緒參照回 E_NOINTERFACE，Explorer 參照回 E_FAIL。
            if(!call(0))co_return finish(0x80070057U);m.store(GuestAddress{call(0)},bits,0);const bool thread=symbol=="SHGetThreadRef";
            std::uint64_t object=shell_instance_explorer_;if(thread){const auto found=shell_thread_refs_.find(threads_.current);object=found==shell_thread_refs_.end()?0:found->second;}
            if(!object)co_return finish(thread?0x80004002U:0x80004005U);
            (void)co_await service_callback::call(method(object,1),{object});m.store(GuestAddress{call(0)},bits,object);co_return finish(0);
        }
        if(symbol=="SHReleaseThreadRef"){
            // 清除目前執行緒的參照並釋放一次（SHCreateThread 以 CTF_THREAD_REF 交給新執行緒的參照）。
            const auto found=shell_thread_refs_.find(threads_.current);if(found==shell_thread_refs_.end())co_return finish(0x80004002U);
            const auto object=found->second;shell_thread_refs_.erase(found);(void)co_await service_callback::call(method(object,2),{object});co_return finish(0);
        }
        const auto entry=[&](std::string_view dll,std::string_view target){const auto stub=host_service_stub(dll,target);return stub.value?stub.value:resolve(dll,target).value;};
        const auto handle_of=[&](std::uint64_t value){return x86_?value&UINT32_MAX:value;};
        const bool wide=symbol.ends_with('W');
        const auto text_at=[&](std::uint64_t address)->std::u16string{if(!address)return {};return wide?read_utf16(m,GuestAddress{address},32768):utf8_to_utf16_lossy(read_ansi(m,address,32768));};
        if(symbol=="#542"){ // GetUIVersion：以實際載入的 shell32 查詢介面版本。
            const auto library=temporary.text(u"shell32.dll"),info=temporary.allocate(20),proc=temporary.allocate(14);
            if(!library || !proc || !info)co_return finish(0);
            constexpr std::uint8_t version_name[]="DllGetVersion";
            m.write(GuestAddress{proc},version_name);
            const auto module=handle_of(co_await service_callback::call_value(entry("kernel32.dll","LoadLibraryW"),{library}));
            if(!module)co_return finish(0);
            const auto fn=handle_of(co_await service_callback::call_value(entry("kernel32.dll","GetProcAddress"),{module,proc}));
            std::uint32_t major=0;
            if(fn){
                m.store(GuestAddress{info},32,20);
                if(!status(co_await service_callback::call(fn,{info})))
                    major=static_cast<std::uint32_t>(m.load(GuestAddress{info+4},32));
            }
            (void)co_await service_callback::call(entry("kernel32.dll","FreeLibrary"),{module});
            co_return finish(major?major:3);
        }
        if(symbol.starts_with("MLLoadLibrary")){
            // (檔名, HMODULE 基準, 跨碼頁旗標)：依序找 <模組目錄>\mui\<UI 語言>\檔名、<模組目錄>\檔名，以資料檔方式載入；最後交給一般搜尋。
            const auto file=text_at(call(0));if(file.empty())co_return finish(0);const auto buffer=temporary.allocate(2*32768);if(!buffer)co_return finish(0);
            const auto length=static_cast<std::uint32_t>(co_await service_callback::call(entry("kernel32.dll","GetModuleFileNameW"),{call(1),buffer,32767}));
            std::vector<std::u16string> candidates;
            if(length && length<32767){auto directory=read_utf16(m,GuestAddress{buffer},32767);directory.resize(directory.find_last_of(u'\\')+1);
                const auto language=static_cast<std::uint16_t>(co_await service_callback::call(entry("kernel32.dll","GetUserDefaultUILanguage"),{}));
                constexpr std::u16string_view hex=u"0123456789ABCDEF";std::u16string code;for(int shift=12;shift>=0;shift-=4)code.push_back(hex[(language>>shift)&15U]);
                candidates.push_back(directory+u"mui\\"+code+u"\\"+file);candidates.push_back(directory+file);}
            candidates.push_back(file);
            for(const auto& candidate:candidates){const auto path=temporary.text(candidate);if(!path)break;
                if(&candidate!=&candidates.back() && static_cast<std::uint32_t>(co_await service_callback::call(entry("kernel32.dll","GetFileAttributesW"),{path}))==UINT32_MAX)continue;
                const auto module=handle_of(co_await service_callback::call_value(entry("kernel32.dll","LoadLibraryExW"),{path,0,0x22}));if(module)co_return finish(module);} // AS_DATAFILE | AS_IMAGE_RESOURCE
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
            if((flags&4U) && shell_instance_explorer_){process_ref=shell_instance_explorer_;(void)co_await service_callback::call(method(process_ref,1),{process_ref});}
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
            // SendMessageTimeout(HWND_BROADCAST, msg, wParam, lParam, SMTO_ABORTIFHUNG, 30 秒)，回傳結果值。
            const auto stub=host_service_stub("user32.dll",symbol.ends_with('W')?"SendMessageTimeoutW":"SendMessageTimeoutA");
            const auto target=stub.value?stub.value:resolve("user32.dll",symbol.ends_with('W')?"SendMessageTimeoutW":"SendMessageTimeoutA").value;
            m.store(GuestAddress{slot},bits,0);(void)co_await service_callback::call(target,{0xffff,static_cast<std::uint32_t>(call(0)),call(1),call(2),2,30000,slot});
            co_return finish(m.load(GuestAddress{slot},bits));
        }
        throw HostCallFault("未知的 shlwapi COM 輔助服務");
    });
}
}
