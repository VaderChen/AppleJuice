#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_region.hpp"
#include "x64arm/windows_transform.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_version.hpp"
#include <algorithm>
#include <bit>
#include <arpa/inet.h>
#include <optional>

namespace x64arm {
std::span<const WindowsServiceDescriptor> legacy_user_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","EnableScrollBar",3},{"user32.dll","ShowScrollBar",3},{"user32.dll","SetScrollPos",4},
        {"user32.dll","SetScrollRange",5},{"user32.dll","GetScrollRange",4},
        {"user32.dll","ScrollWindow",5},{"user32.dll","ScrollWindowEx",8},{"user32.dll","ScrollDC",7},
        {"user32.dll","ShowOwnedPopups",2},
        {"user32.dll","IsCharLowerA",1},{"user32.dll","IsCharLowerW",1},{"user32.dll","IsCharUpperA",1},{"user32.dll","IsCharUpperW",1},
        {"user32.dll","IsCharAlphaA",1},{"user32.dll","IsCharAlphaW",1},{"user32.dll","IsCharAlphaNumericA",1},{"user32.dll","IsCharAlphaNumericW",1},
        {"user32.dll","CreateCursor",7},{"user32.dll","CreateIcon",7},
        {"user32.dll","PackDDElParam",3},{"user32.dll","UnpackDDElParam",4},{"user32.dll","FreeDDElParam",2},{"user32.dll","ReuseDDElParam",5},
        {"kernel32.dll","WinExec",2},
        {"comdlg32.dll","GetFileTitleA",3},{"comdlg32.dll","GetFileTitleW",3},
        {"shell32.dll","InitNetworkAddressControl",0},{"iphlpapi.dll","ParseNetworkString",5},{"internal-netaddress","WindowProc",4},
        {"ole32.dll","IsAccelerator",4},{"ole32.dll","OleTranslateAccelerator",3},{"ole32.dll","OleCreateMenuDescriptor",2},{"ole32.dll","OleDestroyMenuDescriptor",1},
        {"ole32.dll","CoLockObjectExternal",3},
        {"comctl32.dll","DllGetVersion",1},
        {"uxtheme.dll","BufferedPaintInit",0},{"uxtheme.dll","BufferedPaintUnInit",0},{"uxtheme.dll","BeginBufferedPaint",5},{"uxtheme.dll","EndBufferedPaint",2},
        {"uxtheme.dll","GetBufferedPaintBits",3},{"uxtheme.dll","GetBufferedPaintDC",1},{"uxtheme.dll","GetBufferedPaintTargetDC",1},{"uxtheme.dll","GetBufferedPaintTargetRect",2},
        {"uxtheme.dll","BufferedPaintClear",2},{"uxtheme.dll","BufferedPaintSetAlpha",3},
        {"uxtheme.dll","DrawThemeParentBackground",3},{"uxtheme.dll","DrawThemeParentBackgroundEx",4}
    };return table;
}
namespace {
bool dde_packed(std::uint32_t message){return message==0x3e4 || message==0x3e2 || message==0x3e5 || message==0x3e7;} // ACK、ADVISE、DATA、POKE
constexpr std::uint32_t dde_execute=0x3e8;
GuiRect normalized(GuiRect r){return {std::min(r[0],r[2]),std::min(r[1],r[3]),std::max(r[0],r[2]),std::max(r[1],r[3])};}
GuiRect intersect(const GuiRect& a,const GuiRect& b){GuiRect r{std::max(a[0],b[0]),std::max(a[1],b[1]),std::min(a[2],b[2]),std::min(a[3],b[3])};return gui_rect_empty(r)?GuiRect{}:r;}
GuiRect offset(GuiRect r,std::int32_t dx,std::int32_t dy){r[0]+=dx;r[2]+=dx;r[1]+=dy;r[3]+=dy;return r;}
// ParseNetworkString（iphlpapi）與網路位址控制項共用：依 NET_STRING_* 類型逐一比對，輸出 NET_ADDRESS_INFO 欄位。
struct NetworkString {unsigned format{};std::array<std::uint8_t,16> address{};std::uint32_t scope{};std::string name;std::uint16_t port{};std::uint8_t prefix{255};};
std::optional<std::uint16_t> network_port(std::string_view text){
    if(text.empty() || text.size()>5 || !std::all_of(text.begin(),text.end(),[](char c){return c>='0' && c<='9';}))return std::nullopt;
    const auto value=std::stoul(std::string(text));return value<=65535?std::optional<std::uint16_t>{static_cast<std::uint16_t>(value)}:std::nullopt;
}
std::optional<std::uint8_t> network_prefix(std::string_view text,unsigned limit){const auto value=network_port(text);return value && *value<=limit?std::optional<std::uint8_t>{static_cast<std::uint8_t>(*value)}:std::nullopt;}
bool parse_ipv4(std::string_view text,NetworkString& out){
    if(std::count(text.begin(),text.end(),'.')!=3 || !std::all_of(text.begin(),text.end(),[](char c){return (c>='0' && c<='9') || c=='.';}))return false;
    if(inet_pton(AF_INET,std::string(text).c_str(),out.address.data())!=1)return false;out.format=2;return true;
}
bool parse_ipv6(std::string_view text,bool scope_allowed,NetworkString& out){
    const auto percent=text.find('%');std::uint32_t scope=0;
    if(percent!=std::string_view::npos){if(!scope_allowed)return false;const auto digits=text.substr(percent+1);
        if(digits.empty() || digits.size()>10 || !std::all_of(digits.begin(),digits.end(),[](char c){return c>='0' && c<='9';}))return false;
        const auto value=std::stoull(std::string(digits));if(value>UINT32_MAX)return false;scope=static_cast<std::uint32_t>(value);text=text.substr(0,percent);}
    if(text.find(':')==std::string_view::npos || inet_pton(AF_INET6,std::string(text).c_str(),out.address.data())!=1)return false;out.format=3;out.scope=scope;return true;
}
bool parse_dns_name(std::string_view text,NetworkString& out){
    if(text.empty() || text.size()>255)return false;bool alpha=false;std::size_t label=0;char previous='.';
    for(const auto c:text){
        if(c=='.'){if(!label || previous=='-')return false;label=0;}
        else if((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_'){if((!label && c=='-') || ++label>63)return false;if(!(c>='0' && c<='9'))alpha=true;}
        else return false;
        previous=c;
    }
    if(previous=='-' || !alpha)return false;out.format=1;out.name=std::string(text);return true;
}
std::optional<NetworkString> parse_network_string(std::u16string_view wide,std::uint32_t types){
    std::string text;for(const auto c:wide){if(c>=128 || !c)return std::nullopt;text.push_back(static_cast<char>(c));}
    const auto split_last=[&](char separator)->std::optional<std::pair<std::string_view,std::string_view>>{const auto at=text.rfind(separator);if(at==std::string::npos)return std::nullopt;return std::pair{std::string_view(text).substr(0,at),std::string_view(text).substr(at+1)};};
    NetworkString out;
    if((types&1U) && parse_ipv4(text,out))return out;
    if((types&0x18U) && parse_ipv6(text,(types&8U)!=0,(out=NetworkString{})))return out;
    if(types&2U)if(const auto parts=split_last(':');parts){out={};const auto port=network_port(parts->second);if(port && parse_ipv4(parts->first,out)){out.port=*port;return out;}}
    if(types&0x60U)if(const auto parts=split_last(':');parts && parts->first.size()>2 && parts->first.front()=='[' && parts->first.back()==']'){
        out={};const auto port=network_port(parts->second);if(port && parse_ipv6(parts->first.substr(1,parts->first.size()-2),(types&0x20U)!=0,out)){out.port=*port;return out;}}
    if(types&4U)if(const auto parts=split_last('/');parts){
        // CIDR 網路允許省略尾端八位元組（例如 192.168.100/24）。
        std::string padded(parts->first);for(auto dots=std::count(padded.begin(),padded.end(),'.');dots<3;++dots)padded+=".0";
        out={};const auto prefix=network_prefix(parts->second,32);if(prefix && !parts->first.empty() && parse_ipv4(padded,out)){out.prefix=*prefix;return out;}}
    if(types&0x80U)if(const auto parts=split_last('/');parts){out={};const auto prefix=network_prefix(parts->second,128);if(prefix && parse_ipv6(parts->first,false,out)){out.prefix=*prefix;return out;}}
    if(types&0x100U){out={};if(parse_dns_name(text,out))return out;}
    if(types&0x200U)if(const auto parts=split_last(':');parts){out={};const auto port=network_port(parts->second);if(port && parse_dns_name(parts->first,out)){out.port=*port;return out;}}
    return std::nullopt;
}
void store_network_address(GuestMemory& memory,std::uint64_t at,const NetworkString& value){
    // NET_ADDRESS_INFO：Format 後接 NamedAddress（WCHAR[256]＋WCHAR[6]）或 SOCKADDR_IN／SOCKADDR_IN6 的 union，共 528 bytes。
    memory.check(GuestAddress{at},528,Access::Write);memory.write(GuestAddress{at},std::vector<std::uint8_t>(528));memory.store(GuestAddress{at},32,value.format);
    const auto port=static_cast<std::uint16_t>((value.port>>8)|(value.port<<8));
    if(value.format==1){for(std::size_t i=0;i<value.name.size();++i)memory.store(GuestAddress{at+4+i*2},16,static_cast<unsigned char>(value.name[i]));
        if(value.port){const auto digits=std::to_string(value.port);for(std::size_t i=0;i<digits.size();++i)memory.store(GuestAddress{at+516+i*2},16,static_cast<unsigned char>(digits[i]));}}
    else if(value.format==2){memory.store(GuestAddress{at+4},16,2);memory.store(GuestAddress{at+6},16,port);memory.write(GuestAddress{at+8},std::span(value.address).first(4));}
    else {memory.store(GuestAddress{at+4},16,23);memory.store(GuestAddress{at+6},16,port);memory.write(GuestAddress{at+12},std::span(value.address));memory.store(GuestAddress{at+28},32,value.scope);}
}
}
std::optional<std::uint32_t> WindowsServices::invoke_legacy_user(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(0);};
    const auto integer=[&](unsigned i){return static_cast<std::int32_t>(arg(i));};
    const unsigned unit=x86_?4U:8U;
    std::vector<std::uint64_t> scratch;
    struct Release {WindowsServices& self;GuestMemory& memory;const GuestState& state;std::vector<std::uint64_t>& pointers;~Release(){for(const auto p:pointers)(void)self.free_heap(memory,p,state);}} release{*this,memory,state,scratch};
    const auto temporary=[&](std::size_t size){const auto p=allocate_heap(memory,std::max<std::size_t>(size,8));if(!p)throw std::bad_alloc();memory.write(GuestAddress{p},std::vector<std::uint8_t>(std::max<std::size_t>(size,8)));scratch.push_back(p);return p;};
    const auto read_rect=[&](std::uint64_t p){GuiRect r{};for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(memory.load(GuestAddress{p+i*4},32));return r;};
    const auto write_rect=[&](std::uint64_t p,const GuiRect& r){memory.check(GuestAddress{p},16,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{p+i*4},32,static_cast<std::uint32_t>(r[i]));};
    const auto callbacks=[&](std::vector<GuestCallback> calls,std::uint64_t value){
        if(calls.empty())return finish(value);start_callbacks(state,memory,std::move(calls),Continuation::Return,value);return std::optional<std::uint32_t>{};
    };
    // 標準捲軸與 SB_CTL 共用 SetScrollInfo／GetScrollInfo 的資料模型與範圍修正。
    const auto scroll_info=[&](bool set,std::uint32_t mask,std::int32_t a,std::int32_t b){
        const auto info=temporary(28);memory.store(GuestAddress{info},32,28);memory.store(GuestAddress{info+4},32,mask);
        if(set){if(mask&1U){memory.store(GuestAddress{info+8},32,static_cast<std::uint32_t>(a));memory.store(GuestAddress{info+12},32,static_cast<std::uint32_t>(b));}if(mask&4U)memory.store(GuestAddress{info+20},32,static_cast<std::uint32_t>(a));}
        const auto result=set?nested_service(state,memory,"user32.dll","SetScrollInfo",{arg(0),arg(1),info,arg(set && name=="SetScrollRange"?4:3)}):nested_service(state,memory,"user32.dll","GetScrollInfo",{arg(0),arg(1),info});
        return std::pair{result,info};
    };
    try {
        if(name=="IsAccelerator" || name=="OleTranslateAccelerator"){
            // 與 ole32 相同的比對：WM_CHAR 對非 ALT 字元項目、虛擬鍵比對 SHIFT／CTRL、ALT 字元需 lParam 的 context 位元。
            const bool translate=name=="OleTranslateAccelerator";const auto message_pointer=arg(translate?2:2);
            std::uint64_t table=arg(0);std::int64_t count=static_cast<std::int32_t>(arg(1));
            if(translate){if(!arg(1))return finish(0x80070057U);table=memory.load(GuestAddress{arg(1)+(x86_?12U:16U)},static_cast<std::uint8_t>(unit*8));count=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+(x86_?16U:24U)},32));}
            std::optional<std::uint16_t> command;
            const auto found=gui_.accelerators.find(table);
            if(message_pointer && found!=gui_.accelerators.end() && count>0){
                const auto message=static_cast<std::uint32_t>(memory.load(GuestAddress{message_pointer+unit},32));
                const auto wparam=memory.load(GuestAddress{message_pointer+2*unit},static_cast<std::uint8_t>(unit*8)),lparam=memory.load(GuestAddress{message_pointer+3*unit},static_cast<std::uint8_t>(unit*8));
                if(message==0x100 || message==0x104 || message==0x106 || message==0x102){
                    const auto limit=std::min<std::size_t>(static_cast<std::size_t>(count),found->second.size());
                    for(std::size_t i=0;i<limit && !command;++i){
                        const auto& entry=found->second[i];if(entry[1]!=wparam)continue;const auto flags=entry[0];
                        if(message==0x102){if(!(flags&0x10U) && !(flags&1U))command=entry[2];}
                        else if(flags&1U){const unsigned mask=((gui_.keyboard[16]&128)?4U:0U)|((gui_.keyboard[17]&128)?8U:0U);if(mask==(flags&12U))command=entry[2];}
                        else if((lparam&0x20000000U) && (flags&0x10U) && (message==0x106 || message==0x104))command=entry[2];
                    }
                }
            }
            if(!translate){if(command && arg(3))memory.store(GuestAddress{arg(3)},16,*command);return finish(command?1:0);}
            if(!command)return finish(1); // S_FALSE
            const auto frame=arg(0);if(!frame)return finish(0x80070057U);
            const auto vtable=memory.load(GuestAddress{frame},static_cast<std::uint8_t>(unit*8)),method=memory.load(GuestAddress{vtable+14*unit},static_cast<std::uint8_t>(unit*8));
            start_callbacks(state,memory,{{method,{frame,message_pointer,*command}}},Continuation::ReturnCallback,0);return std::nullopt;
        }
        if(name=="OleCreateMenuDescriptor"){
            // OleMenuDescriptor：hwndFrame、hwndActiveObject、OLEMENUGROUPWIDTHS、hmenuCombined、bIsServerItem，置於可移動全域記憶體。
            if(!arg(0))return finish(0);if(!gui_.menus.contains(arg(0)))return fail(1401);
            const std::uint64_t size=x86_?40U:56U;const auto handle=nested_service(state,memory,"kernel32.dll","GlobalAlloc",{0x42,size});if(!handle)return finish(0);
            const auto data=nested_service(state,memory,"kernel32.dll","GlobalLock",{handle});if(!data){(void)nested_service(state,memory,"kernel32.dll","GlobalFree",{handle});return finish(0);}
            if(arg(1))for(unsigned i=0;i<6;++i)memory.store(GuestAddress{data+2*unit+i*4},32,memory.load(GuestAddress{arg(1)+i*4},32));
            memory.store(GuestAddress{data+2*unit+24},static_cast<std::uint8_t>(unit*8),arg(0));
            (void)nested_service(state,memory,"kernel32.dll","GlobalUnlock",{handle});return finish(handle);
        }
        if(name=="OleDestroyMenuDescriptor"){if(arg(0))(void)nested_service(state,memory,"kernel32.dll","GlobalFree",{arg(0)});return finish(0);}
        if(name=="CoLockObjectExternal"){
            if(!arg(0))return finish(0x80070057U);
            const auto initialized=gui_.com_initializations.find(threads_.current);if(initialized==gui_.com_initializations.end() || initialized->second<16)return finish(0x800401f0U);
            const auto vtable=memory.load(GuestAddress{arg(0)},static_cast<std::uint8_t>(unit*8));auto& count=gui_.com_external_locks[arg(0)];
            if(arg(1)){if(count==UINT32_MAX)return finish(0x8007000eU);if(count++==0)return callbacks({{memory.load(GuestAddress{vtable+unit},static_cast<std::uint8_t>(unit*8)),{arg(0)}}},0);return finish(0);}
            if(!count){gui_.com_external_locks.erase(arg(0));return finish(0);}
            if(--count==0){gui_.com_external_locks.erase(arg(0));return callbacks({{memory.load(GuestAddress{vtable+2*unit},static_cast<std::uint8_t>(unit*8)),{arg(0)}}},0);}
            return finish(0);
        }
        if(name=="DllGetVersion"){
            // comctl32 v6（通用控制項 6.10），build 與虛擬 Windows 相同；DLLVERSIONINFO2 另填 ullVersion。
            if(!arg(0))return finish(0x80004003U);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},32));if(size!=20 && size!=32)return finish(0x80070057U);
            memory.store(GuestAddress{arg(0)+4},32,6);memory.store(GuestAddress{arg(0)+8},32,10);memory.store(GuestAddress{arg(0)+12},32,windows_version_build);memory.store(GuestAddress{arg(0)+16},32,2);
            if(size==32){memory.store(GuestAddress{arg(0)+20},32,0);memory.store(GuestAddress{arg(0)+24},64,(std::uint64_t{6}<<48)|(std::uint64_t{10}<<32)|(std::uint64_t{windows_version_build}<<16));}
            return finish(0);
        }
        if(name=="BufferedPaintInit"){++gui_.buffered_paint_inits;return finish(0);}
        if(name=="BufferedPaintUnInit"){if(gui_.buffered_paint_inits)--gui_.buffered_paint_inits;return finish(0);}
        if(name=="BeginBufferedPaint"){
            const auto target=arg(0);if(!target || !arg(1) || !arg(4))return finish(0);
            const auto rect=normalized(read_rect(arg(1)));const auto format=static_cast<std::uint32_t>(arg(2));
            const auto width=rect[2]-rect[0],height=rect[3]-rect[1];if(width<=0 || height<=0 || format>3)return fail(87);
            GuiState::BufferedPaint paint;paint.target=target;paint.rect=rect;paint.format=format;
            if(arg(3)){const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+4},32));
                if(const auto exclude=memory.load(GuestAddress{arg(3)+8},static_cast<std::uint8_t>(unit*8))){paint.exclude=normalized(read_rect(exclude));paint.excluded=true;}
                if(const auto blend=memory.load(GuestAddress{arg(3)+8+unit},static_cast<std::uint8_t>(unit*8))){paint.blend=static_cast<std::uint32_t>(memory.load(GuestAddress{blend},32));paint.blended=true;}
                paint.excluded=paint.excluded && !(flags&2U);if(flags&1U)paint.format|=0x100U;}
            paint.dc=nested_service(state,memory,"gdi32.dll","CreateCompatibleDC",{target});if(!paint.dc)return finish(0);
            if((format&255U)==0)paint.bitmap=nested_service(state,memory,"gdi32.dll","CreateCompatibleBitmap",{target,static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height)});
            else {
                const bool mono=format==3;const auto info=temporary(48+8+8);memory.store(GuestAddress{info},32,40);memory.store(GuestAddress{info+4},32,static_cast<std::uint32_t>(width));
                memory.store(GuestAddress{info+8},32,static_cast<std::uint32_t>(format==1?height:-height));memory.store(GuestAddress{info+12},16,1);memory.store(GuestAddress{info+14},16,mono?1U:32U);
                if(mono)memory.store(GuestAddress{info+44},32,0xffffff);
                paint.bitmap=nested_service(state,memory,"gdi32.dll","CreateDIBSection",{paint.dc,info,0,info+56,0,0});paint.bits=memory.load(GuestAddress{info+56},static_cast<std::uint8_t>(unit*8));
            }
            if(!paint.bitmap){(void)nested_service(state,memory,"gdi32.dll","DeleteDC",{paint.dc});return finish(0);}
            paint.previous=nested_service(state,memory,"gdi32.dll","SelectObject",{paint.dc,paint.bitmap});
            // 緩衝 DC 使用目標座標：視埠原點平移，使 rect 左上角對應緩衝的 (0,0)。
            (void)nested_service(state,memory,"gdi32.dll","SetViewportOrgEx",{paint.dc,static_cast<std::uint32_t>(-rect[0]),static_cast<std::uint32_t>(-rect[1]),0});
            if(paint.format&0x100U)(void)nested_service(state,memory,"gdi32.dll","PatBlt",{paint.dc,static_cast<std::uint32_t>(rect[0]),static_cast<std::uint32_t>(rect[1]),static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),0x42});
            paint.format&=255U;const auto handle=gui_.next_object++;gui_.buffered_paints.emplace(handle,paint);
            memory.store(GuestAddress{arg(4)},static_cast<std::uint8_t>(unit*8),paint.dc);return finish(handle);
        }
        if(name=="EndBufferedPaint" || name=="GetBufferedPaintBits" || name=="GetBufferedPaintDC" || name=="GetBufferedPaintTargetDC" || name=="GetBufferedPaintTargetRect" || name=="BufferedPaintClear" || name=="BufferedPaintSetAlpha"){
            const auto found=gui_.buffered_paints.find(arg(0));
            const bool handle_result=name=="GetBufferedPaintDC" || name=="GetBufferedPaintTargetDC";
            if(found==gui_.buffered_paints.end())return finish(handle_result?0U:0x80070057U);
            auto& paint=found->second;const auto width=paint.rect[2]-paint.rect[0],height=paint.rect[3]-paint.rect[1];
            if(name=="GetBufferedPaintDC")return finish(paint.dc);if(name=="GetBufferedPaintTargetDC")return finish(paint.target);
            if(name=="GetBufferedPaintTargetRect"){if(!arg(1))return finish(0x80070057U);write_rect(arg(1),paint.rect);return finish(0);}
            if(name=="GetBufferedPaintBits"){
                if(!arg(1) || !arg(2))return finish(0x80070057U);if(!paint.bits)return finish(0x80004005U);
                (void)nested_service(state,memory,"gdi32.dll","GdiFlush",{});memory.store(GuestAddress{arg(1)},static_cast<std::uint8_t>(unit*8),paint.bits);
                memory.store(GuestAddress{arg(2)},32,static_cast<std::uint32_t>(paint.format==3?((width+31)/32*32):width));return finish(0);
            }
            const auto area=[&]{return name!="EndBufferedPaint" && arg(1)?intersect(normalized(read_rect(arg(1))),paint.rect):paint.rect;};
            if(name=="BufferedPaintClear"){const auto r=area();if(!gui_rect_empty(r))(void)nested_service(state,memory,"gdi32.dll","PatBlt",{paint.dc,static_cast<std::uint32_t>(r[0]),static_cast<std::uint32_t>(r[1]),static_cast<std::uint32_t>(r[2]-r[0]),static_cast<std::uint32_t>(r[3]-r[1]),0x42});return finish(0);}
            if(name=="BufferedPaintSetAlpha"){
                // 只有 32 位 DIB 有 alpha；直接改寫 backing 後讓 GDI 重新匯入。
                if(!paint.bits || paint.format==3)return finish(0x80004005U);const auto r=area();const auto alpha=static_cast<std::uint32_t>(arg(2)&255U);
                (void)nested_service(state,memory,"gdi32.dll","GdiFlush",{});
                for(auto y=r[1];y<r[3];++y)for(auto x=r[0];x<r[2];++x){const auto row=paint.format==1?height-1-(y-paint.rect[1]):y-paint.rect[1];memory.store(GuestAddress{paint.bits+(static_cast<std::uint64_t>(row)*static_cast<std::uint64_t>(width)+static_cast<std::uint64_t>(x-paint.rect[0]))*4+3},8,alpha);}
                import_dib_bitmap(memory,paint.bitmap);rasters_.erase(paint.bitmap);return finish(0);
            }
            if(arg(1)){
                (void)nested_service(state,memory,"gdi32.dll","SetViewportOrgEx",{paint.dc,0,0,0});
                if(paint.excluded){(void)nested_service(state,memory,"gdi32.dll","SaveDC",{paint.target});(void)nested_service(state,memory,"gdi32.dll","ExcludeClipRect",{paint.target,static_cast<std::uint32_t>(paint.exclude[0]),static_cast<std::uint32_t>(paint.exclude[1]),static_cast<std::uint32_t>(paint.exclude[2]),static_cast<std::uint32_t>(paint.exclude[3])});}
                const std::uint64_t x=static_cast<std::uint32_t>(paint.rect[0]),y=static_cast<std::uint32_t>(paint.rect[1]),w=static_cast<std::uint32_t>(width),h=static_cast<std::uint32_t>(height);
                if(paint.blended)(void)nested_service(state,memory,"msimg32.dll","AlphaBlend",{paint.target,x,y,w,h,paint.dc,0,0,w,h,paint.blend});
                else (void)nested_service(state,memory,"gdi32.dll","BitBlt",{paint.target,x,y,w,h,paint.dc,0,0,0x00cc0020U});
                if(paint.excluded)(void)nested_service(state,memory,"gdi32.dll","RestoreDC",{paint.target,UINT32_MAX});
            }
            (void)nested_service(state,memory,"gdi32.dll","SelectObject",{paint.dc,paint.previous});(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{paint.bitmap});(void)nested_service(state,memory,"gdi32.dll","DeleteDC",{paint.dc});
            gui_.buffered_paints.erase(found);return finish(0);
        }
        if(name=="DrawThemeParentBackground" || name=="DrawThemeParentBackgroundEx"){
            // 以父視窗座標重繪子視窗背景：平移視埠後依序送 WM_ERASEBKGND 與 WM_PRINTCLIENT，最後還原 DC。
            const bool extended=name.ends_with("Ex");const auto hdc=arg(1);const auto flags=extended?static_cast<std::uint32_t>(arg(2)):0U;const auto prc=arg(extended?3:2);
            const auto child=gui_.windows.find(arg(0));if(child==gui_.windows.end())return finish(0x80070578U);
            const auto parent=child->second.parent;if(!parent || !gui_.windows.contains(parent))return finish(0);
            const auto child_origin=gui_client_origin(gui_,arg(0)),parent_origin=gui_client_origin(gui_,parent);
            const auto dx=static_cast<std::int32_t>(child_origin[0]-parent_origin[0]),dy=static_cast<std::int32_t>(child_origin[1]-parent_origin[1]);
            if(!nested_service(state,memory,"gdi32.dll","SaveDC",{hdc}))return finish(0x80070006U);
            (void)nested_service(state,memory,"gdi32.dll","OffsetViewportOrgEx",{hdc,static_cast<std::uint32_t>(-dx),static_cast<std::uint32_t>(-dy),0});
            if(prc){const auto r=offset(normalized(read_rect(prc)),dx,dy);(void)nested_service(state,memory,"gdi32.dll","IntersectClipRect",{hdc,static_cast<std::uint32_t>(r[0]),static_cast<std::uint32_t>(r[1]),static_cast<std::uint32_t>(r[2]),static_cast<std::uint32_t>(r[3])});}
            const auto send=resolve("user32.dll","SendMessageW").value;std::vector<GuestCallback> calls;
            // DTPB_* 旗標目前沿用預設序列（WM_ERASEBKGND＋WM_PRINTCLIENT）；WM_CTLCOLORSTATIC 筆刷變體尚未分流。
            (void)flags;calls.push_back({send,{parent,0x14,hdc,0}});calls.push_back({send,{parent,0x318,hdc,4}});
            calls.push_back({resolve("gdi32.dll","RestoreDC").value,{hdc,static_cast<std::uint64_t>(x86_?UINT32_MAX:UINT64_MAX)}});
            return callbacks(std::move(calls),0);
        }
        if(name=="ParseNetworkString"){
            if(!arg(0))return finish(87);const auto parsed=parse_network_string(read_utf16(memory,GuestAddress{arg(0)}),static_cast<std::uint32_t>(arg(1)));if(!parsed)return finish(87);
            if(arg(2))store_network_address(memory,arg(2),*parsed);if(arg(3))memory.store(GuestAddress{arg(3)},16,parsed->port);if(arg(4))memory.store(GuestAddress{arg(4)},8,parsed->prefix);
            return finish(0);
        }
        if(name=="InitNetworkAddressControl"){
            // msctls_netaddress 是 shell32 的 Edit 超類別；全域類別讓任何模組都能以 WC_NETADDRESS 建立。
            static constexpr std::u16string_view class_name=u"msctls_netaddress";
            if(gui_class_record(class_name,false))return finish(1);
            const auto text=temporary((class_name.size()+1)*2+32);for(std::size_t i=0;i<class_name.size();++i)memory.store(GuestAddress{text+i*2},16,class_name[i]);
            const auto module_name=text+(class_name.size()+1)*2;const std::u16string_view shell=u"shell32.dll";for(std::size_t i=0;i<shell.size();++i)memory.store(GuestAddress{module_name+i*2},16,shell[i]);
            const auto instance=nested_service(state,memory,"kernel32.dll","GetModuleHandleW",{module_name});
            const auto info=temporary(x86_?48:80);const auto field=[&](unsigned x86_offset,unsigned x64_offset,std::uint64_t value,bool pointer){memory.store(GuestAddress{info+(x86_?x86_offset:x64_offset)},static_cast<std::uint8_t>(pointer?unit*8:32),value);};
            field(0,0,x86_?48U:80U,false);field(4,4,0x4008U,false);field(8,8,resolve("internal-netaddress","WindowProc").value,true);field(20,24,instance,true);field(32,48,6,true);field(40,64,text,true);
            return finish(nested_service(state,memory,"user32.dll","RegisterClassExW",{info})?1:0);
        }
        if(name=="WindowProc"){
            const auto window=gui_.windows.find(arg(0));const auto message=static_cast<std::uint32_t>(arg(1));
            if(window!=gui_.windows.end() && message==0x81){window->second.builtin=true;window->second.control_class=u"edit";}
            auto& types=gui_.net_address_types.try_emplace(arg(0),0x209U).first->second;
            if(message==0x402){const auto previous=types;types=static_cast<std::uint32_t>(arg(2));return finish(previous);} // NCM_SETALLOWTYPE
            if(message==0x403)return finish(types); // NCM_GETALLOWTYPE
            if(message==0x401){ // NCM_GETADDRESS；HRESULT 經 LRESULT 回傳，x64 需符號延伸。
                if(!arg(3))return finish(static_cast<std::uint64_t>(std::int64_t{static_cast<std::int32_t>(0x80070057U)}));if(window==gui_.windows.end() || window->second.title.empty())return finish(1);
                const auto parsed=parse_network_string(window->second.title,types);if(!parsed)return finish(87);
                const auto info=memory.load(GuestAddress{arg(3)},static_cast<std::uint8_t>(unit*8));if(info)store_network_address(memory,info,*parsed);
                memory.store(GuestAddress{arg(3)+unit},16,parsed->port);memory.store(GuestAddress{arg(3)+unit+2},8,parsed->format==1?0U:parsed->prefix);return finish(0);
            }
            if(message==2)gui_.net_address_types.erase(arg(0));
            if(message==0x404){ // NCM_DISPLAYERRORTIP：以 Edit 的錯誤氣球提示顯示，完成後釋放提示文字。
                static constexpr std::u16string_view title=u"Invalid network address",body=u"The address does not match the allowed network address types.";
                const auto size=static_cast<std::size_t>(x86_?16:32)+(title.size()+body.size()+2)*2;const auto tip=allocate_heap(memory,size);if(!tip)return finish(static_cast<std::uint64_t>(std::int64_t{static_cast<std::int32_t>(0x8007000eU)}));
                const auto strings=tip+(x86_?16U:32U);for(std::size_t i=0;i<=title.size();++i)memory.store(GuestAddress{strings+i*2},16,i<title.size()?title[i]:0);
                const auto text_at=strings+(title.size()+1)*2;for(std::size_t i=0;i<=body.size();++i)memory.store(GuestAddress{text_at+i*2},16,i<body.size()?body[i]:0);
                memory.store(GuestAddress{tip},32,x86_?16U:32U);memory.store(GuestAddress{tip+unit},static_cast<std::uint8_t>(unit*8),strings);memory.store(GuestAddress{tip+2*unit},static_cast<std::uint8_t>(unit*8),text_at);memory.store(GuestAddress{tip+3*unit},32,3);
                start_callbacks(state,memory,{{resolve("user32.dll","SendMessageW").value,{arg(0),0x1503,0,tip}}},Continuation::ReturnZeroFreeHeap,tip);return std::nullopt;
            }
            start_callbacks(state,memory,{{stock_gui_proc(u"edit",true),{arg(0),arg(1),arg(2),arg(3)}}},Continuation::ReturnCallback,0);return std::nullopt;
        }
        if(name=="SetScrollPos" || name=="SetScrollRange" || name=="GetScrollRange" || name=="EnableScrollBar" || name=="ShowScrollBar"){
            const auto found=gui_.windows.find(arg(0));if(found==gui_.windows.end())return fail(1400);
            const auto bar=static_cast<std::uint32_t>(arg(1));
            if(name=="SetScrollPos"){
                if(bar>2)return fail(87);const auto previous=nested_service(state,memory,"user32.dll","GetScrollPos",{arg(0),arg(1)});
                (void)scroll_info(true,4,integer(2),0);return finish(static_cast<std::uint32_t>(previous));
            }
            if(name=="SetScrollRange"){
                if(bar>2)return fail(87);if(static_cast<std::int64_t>(integer(3))-integer(2)>INT32_MAX)return fail(1448);
                (void)scroll_info(true,1,integer(2),integer(3));return finish(1);
            }
            if(name=="GetScrollRange"){
                if(bar>2)return fail(87);if(!arg(2) || !arg(3))return fail(87);
                const auto [ok,info]=scroll_info(false,1,0,0);if(!ok)return finish(0);
                memory.check(GuestAddress{arg(2)},4,Access::Write);memory.check(GuestAddress{arg(3)},4,Access::Write);
                memory.store(GuestAddress{arg(2)},32,memory.load(GuestAddress{info+8},32));memory.store(GuestAddress{arg(3)},32,memory.load(GuestAddress{info+12},32));return finish(1);
            }
            if(name=="EnableScrollBar"){
                // ESB_*：位元 0 停用左／上箭頭，位元 1 停用右／下箭頭；SB_BOTH 同時套用兩個標準捲軸。
                if(bar>3)return fail(87);const auto flags=static_cast<std::uint32_t>(arg(2))&3U;
                bool changed=false;
                for(const auto axis:bar==3?std::vector<std::uint32_t>{0,1}:std::vector<std::uint32_t>{bar}){auto& current=gui_.scroll_arrows[{arg(0),axis}];if(current!=flags){current=flags;changed=true;}}
                if(!changed && bar!=2)return finish(0);
                if(bar==2 && (flags==0 || flags==3))return callbacks({{resolve("user32.dll","EnableWindow").value,{arg(0),flags==0?1U:0U}}},1);
                return finish(changed?1:0);
            }
            if(bar>3)return fail(87);
            if(bar==2)return callbacks({{resolve("user32.dll","ShowWindow").value,{arg(0),arg(2)?5U:0U}}},1);
            auto& window=found->second;const auto bits=bar==0?0x100000U:bar==1?0x200000U:0x300000U;
            const auto style=arg(2)?window.style|bits:window.style&~bits;if(style==window.style)return finish(1);
            // 與 SetWindowLong 相同：先保留舊 nonclient 幾何，再由 SWP_FRAMECHANGED 套用新的捲軸邊界。
            if(!window.frame_insets)gui_refresh_frame(window);window.style=style;
            return callbacks({{resolve("user32.dll","SetWindowPos").value,{arg(0),0,0,0,0,0,0x37}}},1);
        }
        if(name=="ScrollDC" || name=="ScrollWindow" || name=="ScrollWindowEx"){
            const bool dc_call=name=="ScrollDC";const auto dx=integer(1),dy=integer(2);
            const auto scroll_pointer=arg(3),clip_pointer=arg(4);
            const auto region_handle=dc_call?arg(5):name=="ScrollWindowEx"?arg(5):0;const auto update_pointer=dc_call?arg(6):name=="ScrollWindowEx"?arg(6):0;
            const auto flags=name=="ScrollWindowEx"?static_cast<std::uint32_t>(arg(7)):name=="ScrollWindow"?(6U|(scroll_pointer?0U:1U)):0U;
            if(region_handle && !gui_.regions.contains(region_handle))return fail(6);
            std::uint64_t hdc=arg(0),window_handle=0;
            if(dc_call){if(!gui_.contexts.contains(hdc) && !gui_.dc_objects.contains(hdc) && !(hdc>=0x10000000 && gui_.windows.contains(hdc-0x10000000)))return fail(6);}
            else {window_handle=arg(0);if(!gui_.windows.contains(window_handle))return fail(1400);hdc=nested_service(state,memory,"user32.dll","GetDC",{window_handle});if(!hdc)return finish(0);}
            struct ReleaseDc {WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t window,dc;~ReleaseDc(){if(window)try{(void)self.nested_service(state,memory,"user32.dll","ReleaseDC",{window,dc});}catch(...){}}} release_dc{*this,state,memory,window_handle,hdc};
            const auto box=temporary(16);if(!nested_service(state,memory,"gdi32.dll","GetClipBox",{hdc,box}))return fail(6);
            const auto visible=read_rect(box);const auto clip=clip_pointer?intersect(normalized(read_rect(clip_pointer)),visible):visible;
            // 只捲動仍落在裁切矩形內的像素；指定捲動矩形時再限制來源。
            auto source=intersect(clip,offset(clip,-dx,-dy));if(scroll_pointer)source=intersect(source,normalized(read_rect(scroll_pointer)));
            if(!gui_rect_empty(source) && (dx || dy))
                if(!nested_service(state,memory,"gdi32.dll","BitBlt",{hdc,static_cast<std::uint32_t>(source[0]+dx),static_cast<std::uint32_t>(source[1]+dy),static_cast<std::uint32_t>(source[2]-source[0]),static_cast<std::uint32_t>(source[3]-source[1]),hdc,static_cast<std::uint32_t>(source[0]),static_cast<std::uint32_t>(source[1]),0x00cc0020U}))return finish(0);
            const auto area=scroll_pointer?intersect(clip,normalized(read_rect(scroll_pointer))):clip;
            auto update=gui_rect_empty(area)?GuiRegion{}:gui_region_combine(GuiRegion{area},gui_rect_empty(source)?GuiRegion{}:GuiRegion{intersect(offset(source,dx,dy),area)},4);
            if(!dx && !dy)update.clear();
            if(region_handle)gui_.regions[region_handle]=update;
            if(update_pointer)write_rect(update_pointer,gui_rect_empty(gui_region_bounds(update))?GuiRect{}:gui_region_bounds(update));
            const auto kind=gui_region_kind(update);
            if(dc_call)return finish(1);
            std::vector<GuestCallback> calls;
            if((flags&2U) && !update.empty()){
                // SW_INVALIDATE／SW_ERASE 透過一般 InvalidateRgn 排入 WM_PAINT／WM_ERASEBKGND。
                const auto temporary_region=gui_.next_object++;gui_.regions.emplace(temporary_region,update);
                (void)nested_service(state,memory,"user32.dll","InvalidateRgn",{window_handle,temporary_region,(flags&4U)?1U:0U});gui_.regions.erase(temporary_region);
            }
            if(flags&1U){
                const auto limit=scroll_pointer?std::optional<GuiRect>{normalized(read_rect(scroll_pointer))}:std::nullopt;
                for(const auto& [child,w]:gui_.windows){
                    if(w.parent!=window_handle || (w.style&0x40000000U)==0)continue;
                    const GuiRect bounds{w.x,w.y,static_cast<std::int32_t>(w.x+static_cast<std::int64_t>(w.width)),static_cast<std::int32_t>(w.y+static_cast<std::int64_t>(w.height))};
                    if(limit && gui_rect_empty(intersect(*limit,bounds)))continue;
                    calls.push_back({resolve("user32.dll","SetWindowPos").value,{child,0,static_cast<std::uint32_t>(w.x+dx),static_cast<std::uint32_t>(w.y+dy),0,0,0x2015}});
                }
            }
            const std::uint64_t value=name=="ScrollWindow"?1U:kind;
            return callbacks(std::move(calls),value);
        }
        if(name=="ShowOwnedPopups"){
            if(!gui_.windows.contains(arg(0)))return fail(1400);
            // 與 Windows 相同：以 WM_SHOWWINDOW（SW_PARENTCLOSING／SW_PARENTOPENING）通知，由預設視窗程序實際切換。
            std::vector<GuestCallback> calls;const auto send=resolve("user32.dll","SendMessageW").value;
            for(const auto& [handle,w]:gui_.windows){
                if(w.owner!=arg(0) || w.parent || w.destroying)continue;
                if(arg(1)?w.owned_popup_hidden:w.visible)calls.push_back({send,{handle,0x18,arg(1)?1U:0U,arg(1)?3U:1U}});
            }
            return callbacks(std::move(calls),1);
        }
        if(name.starts_with("IsChar")){
            const bool wide=name.ends_with('W');const auto buffer=temporary(8);std::uint64_t character=arg(0)&0xffffU;
            if(!wide){
                memory.store(GuestAddress{buffer},8,arg(0)&255U);
                if(nested_service(state,memory,"kernel32.dll","MultiByteToWideChar",{0,0,buffer,1,buffer+2,1})!=1)return finish(0);
                character=memory.load(GuestAddress{buffer+2},16);
            }
            memory.store(GuestAddress{buffer},16,character);
            if(!nested_service(state,memory,"kernel32.dll","GetStringTypeW",{1,buffer,1,buffer+4}))return finish(0);
            const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{buffer+4},16));
            const auto mask=name.starts_with("IsCharLower")?2U:name.starts_with("IsCharUpper")?1U:name.starts_with("IsCharAlphaNumeric")?0x104U:0x100U;
            return finish((type&mask)?1:0);
        }
        if(name=="CreateCursor" || name=="CreateIcon"){
            const bool cursor=name=="CreateCursor";
            const auto width=integer(cursor?3:1),height=integer(cursor?4:2);const auto planes=cursor?1U:static_cast<std::uint32_t>(arg(3)&255U),bpp=cursor?1U:static_cast<std::uint32_t>(arg(4)&255U);
            const auto and_bits=arg(5),xor_bits=arg(6);
            if(width<=0 || height<=0 || width>256 || height>256 || !and_bits || !xor_bits)return fail(87);
            const auto mono_stride=static_cast<std::size_t>((width+15)/16*2);const bool monochrome=planes==1 && bpp==1;
            std::vector<std::uint64_t> bitmaps;
            struct Delete {WindowsServices& self;GuestState& state;GuestMemory& memory;std::vector<std::uint64_t>& handles;~Delete(){for(const auto h:handles)try{(void)self.nested_service(state,memory,"gdi32.dll","DeleteObject",{h});}catch(...){}}} cleanup{*this,state,memory,bitmaps};
            std::uint64_t mask=0,color=0;
            if(monochrome){
                // 單色游標／圖示的遮罩上半為 AND、下半為 XOR 平面，與 CreateIconIndirect 的 hbmColor=NULL 格式一致。
                const auto size=mono_stride*static_cast<std::size_t>(height);std::vector<std::uint8_t> planes_bytes(size*2);
                memory.read(GuestAddress{and_bits},std::span(planes_bytes).first(size));memory.read(GuestAddress{xor_bits},std::span(planes_bytes).subspan(size));
                const auto bits=temporary(planes_bytes.size());memory.write(GuestAddress{bits},planes_bytes);
                mask=nested_service(state,memory,"gdi32.dll","CreateBitmap",{static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height*2),1,1,bits});if(!mask)return finish(0);bitmaps.push_back(mask);
            }else{
                mask=nested_service(state,memory,"gdi32.dll","CreateBitmap",{static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),1,1,and_bits});if(!mask)return finish(0);bitmaps.push_back(mask);
                color=nested_service(state,memory,"gdi32.dll","CreateBitmap",{static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),planes,bpp,xor_bits});if(!color)return finish(0);bitmaps.push_back(color);
            }
            const auto info=temporary(x86_?20:32);memory.store(GuestAddress{info},32,cursor?0U:1U);
            memory.store(GuestAddress{info+4},32,cursor?static_cast<std::uint32_t>(arg(1)):0U);memory.store(GuestAddress{info+8},32,cursor?static_cast<std::uint32_t>(arg(2)):0U);
            memory.store(GuestAddress{info+(x86_?12U:16U)},static_cast<std::uint8_t>(unit*8),mask);memory.store(GuestAddress{info+(x86_?16U:24U)},static_cast<std::uint8_t>(unit*8),color);
            return finish(nested_service(state,memory,"user32.dll","CreateIconIndirect",{info}));
        }
        if(name=="PackDDElParam" || name=="UnpackDDElParam" || name=="FreeDDElParam" || name=="ReuseDDElParam"){
            const auto pack=[&](std::uint32_t message,std::uint64_t low,std::uint64_t high)->std::uint64_t{
                if(message==dde_execute)return high;
                if(!dde_packed(message))return (low&0xffffU)|((high&0xffffU)<<16);
                // Win32 DDE 以 GMEM_DDESHARE 全域記憶體保存兩個 UINT_PTR，lParam 即該 handle。
                const auto handle=nested_service(state,memory,"kernel32.dll","GlobalAlloc",{0x2000,2ULL*unit});if(!handle)return 0;
                const auto data=nested_service(state,memory,"kernel32.dll","GlobalLock",{handle});if(!data)return 0;
                memory.store(GuestAddress{data},static_cast<std::uint8_t>(unit*8),low);memory.store(GuestAddress{data+unit},static_cast<std::uint8_t>(unit*8),high);
                (void)nested_service(state,memory,"kernel32.dll","GlobalUnlock",{handle});return handle;
            };
            const auto release_packed=[&](std::uint32_t message,std::uint64_t lparam)->std::uint64_t{
                if(!dde_packed(message) || !lparam)return 1;
                if(!nested_service(state,memory,"kernel32.dll","GlobalHandle",{lparam}))return 1;
                return nested_service(state,memory,"kernel32.dll","GlobalFree",{lparam})?0:1;
            };
            if(name=="PackDDElParam")return finish(pack(static_cast<std::uint32_t>(arg(0)),arg(1),arg(2)));
            if(name=="FreeDDElParam")return finish(release_packed(static_cast<std::uint32_t>(arg(0)),arg(1)));
            if(name=="UnpackDDElParam"){
                const auto message=static_cast<std::uint32_t>(arg(0));const auto lparam=arg(1);std::uint64_t low=0,high=0;bool ok=true;
                if(message==dde_execute)high=lparam;
                else if(!dde_packed(message)){low=lparam&0xffffU;high=(lparam>>16)&0xffffU;}
                else {
                    const auto data=lparam?nested_service(state,memory,"kernel32.dll","GlobalLock",{lparam}):0;
                    if(!data)ok=false;else {low=memory.load(GuestAddress{data},static_cast<std::uint8_t>(unit*8));high=memory.load(GuestAddress{data+unit},static_cast<std::uint8_t>(unit*8));(void)nested_service(state,memory,"kernel32.dll","GlobalUnlock",{lparam});}
                }
                if(arg(2))memory.store(GuestAddress{arg(2)},static_cast<std::uint8_t>(unit*8),low);if(arg(3))memory.store(GuestAddress{arg(3)},static_cast<std::uint8_t>(unit*8),high);
                return finish(ok?1:0);
            }
            const auto lparam=arg(0);const auto in=static_cast<std::uint32_t>(arg(1));const auto out=static_cast<std::uint32_t>(arg(2));
            if(!dde_packed(in))return finish(pack(out,arg(3),arg(4)));
            if(dde_packed(out)){
                if(!lparam)return finish(0);const auto data=nested_service(state,memory,"kernel32.dll","GlobalLock",{lparam});if(!data)return finish(0);
                memory.store(GuestAddress{data},static_cast<std::uint8_t>(unit*8),arg(3));memory.store(GuestAddress{data+unit},static_cast<std::uint8_t>(unit*8),arg(4));
                (void)nested_service(state,memory,"kernel32.dll","GlobalUnlock",{lparam});return finish(lparam);
            }
            (void)release_packed(in,lparam);return finish(out==dde_execute?arg(4):(arg(3)&0xffffU)|((arg(4)&0xffffU)<<16));
        }
        if(name=="WinExec"){
            if(!arg(0))return fail(87);
            const auto text=read_ansi(memory,arg(0),32767);const auto command=temporary(text.size()+1);memory.write(GuestAddress{command},std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
            const auto startup=temporary(x86_?68:104),process=temporary(x86_?16:24);
            memory.store(GuestAddress{startup},32,x86_?68U:104U);memory.store(GuestAddress{startup+(x86_?44U:60U)},32,1);memory.store(GuestAddress{startup+(x86_?48U:64U)},16,arg(1)&0xffffU);
            if(!nested_service(state,memory,"kernel32.dll","CreateProcessA",{0,command,0,0,0,0,0,0,startup,process})){
                // WinExec 以小於 32 的錯誤碼回報；其他 CreateProcess 錯誤對應 ERROR_BAD_FORMAT。
                return finish(last_error_<32?last_error_:11U);
            }
            const auto process_handle=memory.load(GuestAddress{process},static_cast<std::uint8_t>(unit*8)),thread_handle=memory.load(GuestAddress{process+unit},static_cast<std::uint8_t>(unit*8));
            return callbacks({{resolve("user32.dll","WaitForInputIdle").value,{process_handle,30000}},{resolve("kernel32.dll","CloseHandle").value,{thread_handle}},{resolve("kernel32.dll","CloseHandle").value,{process_handle}}},33);
        }
        if(name=="GetFileTitleA" || name=="GetFileTitleW"){
            const bool wide=name.ends_with('W');if(!arg(0) || !arg(1))return finish(0xffff);
            const auto path=wide?read_utf16(memory,GuestAddress{arg(0)}):utf8_to_utf16_lossy(read_ansi(memory,arg(0),32767));
            // 與 comdlg32 相同：萬用字元、空字串或以分隔符結尾皆屬無效名稱，回傳負值。
            if(path.empty() || path.find_first_of(u"*[]")!=std::u16string::npos || path.back()==u'/' || path.back()==u'\\' || path.back()==u':')return finish(0xffff);
            const auto split=path.find_last_of(u"/\\:");const auto title=split==std::u16string::npos?path:path.substr(split+1);
            const auto size=static_cast<std::uint32_t>(arg(2)&0xffffU);
            if(wide){
                if(size<title.size()+1)return finish(title.size()+1);
                memory.check(GuestAddress{arg(1)},(title.size()+1)*2,Access::Write);for(std::size_t i=0;i<title.size();++i)memory.store(GuestAddress{arg(1)+i*2},16,title[i]);memory.store(GuestAddress{arg(1)+title.size()*2},16,0);
            }else{
                const auto split_narrow=[&]{const auto narrow=read_ansi(memory,arg(0),32767);const auto at=narrow.find_last_of("/\\:");return at==std::string::npos?narrow:narrow.substr(at+1);}();
                if(size<split_narrow.size()+1)return finish(split_narrow.size()+1);
                memory.check(GuestAddress{arg(1)},split_narrow.size()+1,Access::Write);memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(split_narrow.data()),split_narrow.size()));memory.store(GuestAddress{arg(1)+split_narrow.size()},8,0);
            }
            return finish(0);
        }
    }catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::invalid_argument&){return fail(87);}
    throw HostCallFault("尚未實作舊式 USER 服務");
}
}
