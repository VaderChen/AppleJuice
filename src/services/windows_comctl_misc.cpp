// comctl32 雜項家族（Windows SDK 公開語義）：
// - Flat scroll bar：InitializeFlatSB／UninitializeFlatSB 與 FlatSB_GetScrollProp／FlatSB_SetScrollProp（WSB_PROP_* 屬性存於視窗）；
//   其餘 FlatSB_* 轉發到 user32 標準捲軸（comctl32 v6 的 flat scroll bar 即以標準捲軸呈現）。
// - InitMUILanguage／GetMUILanguage、GetEffectiveClientRect、ShowHideMenuCtl、DrawStatusTextA／W、CreateUpDownControl、
//   CreateToolbar（OLDTBBUTTON，經 CreateToolbarEx）、RegisterClassNameW、CreateMappedBitmap（經 LoadBitmapW 後對映色彩）。
// - ImageList：GetFlags／SetFlags、SetIconSize、GetImageRect、Copy、Duplicate、Merge、SetOverlayImage、DrawIndirect、LoadImageA／W、SetFilter。
//   影像沿用 GuiImageList（windows_controls.cpp）的資料與繪製函式 draw_image_list。
// - MRU 清單：CreateMRUListW、AddMRUStringW、EnumMRUListW、FreeMRUList。資料存於呼叫者指定的登錄鍵（值名 a～z，MRUList 為由新到舊
//   的格位字母）；MRU_CACHEWRITE 時延到 FreeMRUList 才寫入。字串比較預設不分大小寫（lstrcmpiW），可由 lpfnCompare 指定。
// - ImageList 拖曳：BeginDrag、DragEnter、DragMove、DragShowNolock、DragLeave、EndDrag、GetDragImage、SetDragCursorImage。座標相對於鎖定
//   視窗的左上角（NULL 為桌面），在工作區 DC 上繪製（扣掉非工作區位移，非工作區部分被裁切）。顯示前以 BitBlt 保存被覆蓋的畫面，
//   隱藏或移動時貼回。
// - ImageList_Write／ImageList_Read：經客體 IStream（Read＝vtable 3、Write＝vtable 4）讀寫 ILHEAD（'IL'、版本 0x101、影像數、容量、增量、
//   cx、cy、背景色、旗標、前 4 個覆蓋圖）＋每列 4 格的影像條帶 BMP（ILC_COLOR32 為 32 位元，其餘 24 位元）＋ILC_MASK 時的 1 位元遮罩 BMP。
// - HIMAGELIST_QueryInterface：回傳包裝該清單的 IImageList（IUnknown 亦可；IImageList2 回 E_NOINTERFACE），32 個方法轉給 ImageList_*；
//   同一清單只有一個物件（COM 識別一致），Release 到 0 時銷毀清單。
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_icon.hpp"
#include "x64arm/windows_userdraw.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <functional>

namespace x64arm {
namespace {
constexpr std::uint32_t clr_none=0xffffffffU,clr_default=0xff000000U;
std::int32_t as_int(std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));}
// 影像的 0xAARRGGBB 未預乘像素（不透明 alpha=255、透明 0）；Merge 以此合成後再由 finish_icon 建立遮罩／色彩平面。
// ImageList 串流的 BMP（BITMAPFILEHEADER＋BITMAPINFOHEADER＋調色盤＋由下而上的像素列）。pixel 依 GuiBitmap 的 COLORREF 排列。
std::vector<std::uint8_t> bitmap_file(std::uint32_t width,std::uint32_t height,unsigned depth,const std::function<std::uint32_t(std::uint32_t,std::uint32_t)>& pixel){
    const auto stride=(static_cast<std::size_t>(width)*depth+31)/32*4;const std::size_t palette=depth==1?8:0;
    const auto offset=14+40+palette,size=offset+stride*height;
    std::vector<std::uint8_t> out(size,0);
    const auto put=[&](std::size_t at,std::uint64_t value,unsigned bytes){for(unsigned i=0;i<bytes;++i)out[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    out[0]='B';out[1]='M';put(2,size,4);put(10,offset,4);
    put(14,40,4);put(18,width,4);put(22,height,4);put(26,1,2);put(28,depth,2);put(34,stride*height,4);
    if(depth==1){put(54,0,4);put(58,0xffffffU,4);} // 0 黑（不透明）、1 白（透明）。
    for(std::uint32_t y=0;y<height;++y){
        const auto row=offset+static_cast<std::size_t>(height-1-y)*stride;
        for(std::uint32_t x=0;x<width;++x){
            const auto color=pixel(x,y);
            if(depth==1){if(color)out[row+x/8]|=static_cast<std::uint8_t>(0x80U>>(x%8));continue;}
            const auto at=row+static_cast<std::size_t>(x)*(depth/8);
            out[at]=static_cast<std::uint8_t>(color>>16);out[at+1]=static_cast<std::uint8_t>(color>>8);out[at+2]=static_cast<std::uint8_t>(color);
            if(depth==32)out[at+3]=static_cast<std::uint8_t>(color>>24);
        }
    }
    return out;
}
std::vector<std::uint32_t> straight_pixels(GuiIcon icon){
    const auto count=static_cast<std::size_t>(icon.width)*icon.height;
    if(icon.rgba.size()==count)return icon.rgba;
    if(icon.mask_pixels.empty() || (!icon.monochrome && icon.color_pixels.empty()))finish_icon(icon);
    std::vector<std::uint32_t> out(count,0);
    for(std::size_t i=0;i<count && i<icon.mask_pixels.size();++i){
        if(icon.monochrome){if(!icon.mask_pixels[i])out[i]=0xff000000U|(icon.mask_pixels[count+i]?0xffffffU:0U);continue;}
        const auto pixel=icon.color_pixels[i];
        if(icon.alpha){const auto a=pixel>>24;if(!a)continue;std::uint32_t value=a<<24;for(unsigned c=0;c<3;++c)value|=std::min(255U,((pixel>>(c*8))&255U)*255U/a)<<(c*8);out[i]=value;}
        else if(!icon.mask_pixels[i])out[i]=0xff000000U|(pixel&0xffffffU);
    }
    return out;
}
// RegisterClassNameW 可登錄的 comctl32 視窗類別（v6 也包含標準控制項）。
constexpr std::u16string_view comctl_classes[]{u"button",u"static",u"edit",u"listbox",u"combobox",u"combolbox",u"scrollbar",
    u"syslistview32",u"systreeview32",u"sysheader32",u"toolbarwindow32",u"msctls_statusbar32",u"msctls_trackbar32",u"msctls_updown32",
    u"msctls_progress32",u"systabcontrol32",u"tooltips_class32",u"sysanimate32",u"sysmonthcal32",u"sysdatetimepick32",u"sysipaddress32",
    u"rebarwindow32",u"comboboxex32",u"msctls_hotkey32",u"syspager",u"nativefontctl",u"syslink",u"syslistview",u"systreeview"};
}
std::span<const WindowsServiceDescriptor> comctl_misc_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"comctl32.dll","InitializeFlatSB",1},{"comctl32.dll","UninitializeFlatSB",1},
        {"comctl32.dll","FlatSB_GetScrollProp",3,0,0,0x2},{"comctl32.dll","FlatSB_SetScrollProp",4,0,0,0xa},
        {"comctl32.dll","GetMUILanguage",0},{"comctl32.dll","InitMUILanguage",1,0,0,0x1},
        {"comctl32.dll","GetEffectiveClientRect",3},{"comctl32.dll","ShowHideMenuCtl",3},
        {"comctl32.dll","DrawStatusTextA",4,0,0,0x8},{"comctl32.dll","DrawStatusTextW",4,0,0,0x8},
        {"comctl32.dll","CreateUpDownControl",12,0,0,0xe5f},{"comctl32.dll","CreateToolbar",8,0,0,0x8e},
        {"comctl32.dll","RegisterClassNameW",1},{"comctl32.dll","CreateMappedBitmap",5,0,0,0x14},
        {"comctl32.dll","ImageList_GetFlags",1},{"comctl32.dll","ImageList_SetFlags",2,0,0,0x2},{"comctl32.dll","ImageList_SetIconSize",3,0,0,0x6},
        {"comctl32.dll","ImageList_GetImageRect",3,0,0,0x2},{"comctl32.dll","ImageList_Copy",5,0,0,0x1a},{"comctl32.dll","ImageList_Duplicate",1},
        {"comctl32.dll","ImageList_Merge",6,0,0,0x3a},{"comctl32.dll","ImageList_SetOverlayImage",3,0,0,0x6},{"comctl32.dll","ImageList_DrawIndirect",1},
        {"comctl32.dll","ImageList_LoadImageA",7,0,0,0x7c},{"comctl32.dll","ImageList_LoadImageW",7,0,0,0x7c},{"comctl32.dll","ImageList_SetFilter",3,0,0,0x6},
        {"comctl32.dll","CreateMRUListW",1},{"comctl32.dll","AddMRUStringW",2},{"comctl32.dll","EnumMRUListW",4,0,0,0xa},{"comctl32.dll","FreeMRUList",1},
        {"comctl32.dll","ImageList_BeginDrag",4,0,0,0xe},{"comctl32.dll","ImageList_DragEnter",3,0,0,0x6},{"comctl32.dll","ImageList_DragMove",2,0,0,0x3},
        {"comctl32.dll","ImageList_DragShowNolock",1,0,0,0x1},{"comctl32.dll","ImageList_DragLeave",1},{"comctl32.dll","ImageList_EndDrag",0},
        {"comctl32.dll","ImageList_GetDragImage",2},{"comctl32.dll","ImageList_SetDragCursorImage",4,0,0,0xe},
        {"comctl32.dll","ImageList_Write",2},{"comctl32.dll","ImageList_Read",1},{"comctl32.dll","HIMAGELIST_QueryInterface",3},
        // IImageList 的 vtable 順序（commoncontrols.h）。
        {"internal-imagelist","QueryInterface",3,0,0,0x0},{"internal-imagelist","AddRef",1,0,0,0x0},{"internal-imagelist","Release",1,0,0,0x0},{"internal-imagelist","Add",4,0,0,0x0},{"internal-imagelist","ReplaceIcon",4,0,0,0x2},{"internal-imagelist","SetOverlayImage",3,0,0,0x6},{"internal-imagelist","Replace",4,0,0,0x2},{"internal-imagelist","AddMasked",4,0,0,0x4},{"internal-imagelist","Draw",2,0,0,0x0},{"internal-imagelist","Remove",2,0,0,0x2},{"internal-imagelist","GetIcon",4,0,0,0x6},{"internal-imagelist","GetImageInfo",3,0,0,0x2},{"internal-imagelist","Copy",5,0,0,0x1a},{"internal-imagelist","Merge",8,0,0,0x3a},{"internal-imagelist","Clone",3,0,0,0x0},{"internal-imagelist","GetImageRect",3,0,0,0x2},{"internal-imagelist","GetIconSize",3,0,0,0x0},{"internal-imagelist","SetIconSize",3,0,0,0x6},{"internal-imagelist","GetImageCount",2,0,0,0x0},{"internal-imagelist","SetImageCount",2,0,0,0x2},{"internal-imagelist","SetBkColor",3,0,0,0x2},{"internal-imagelist","GetBkColor",2,0,0,0x0},{"internal-imagelist","BeginDrag",4,0,0,0xe},{"internal-imagelist","EndDrag",1,0,0,0x0},{"internal-imagelist","DragEnter",4,0,0,0xc},{"internal-imagelist","DragLeave",2,0,0,0x0},{"internal-imagelist","DragMove",3,0,0,0x6},{"internal-imagelist","SetDragCursorImage",5,0,0,0x1c},{"internal-imagelist","DragShowNolock",2,0,0,0x2},{"internal-imagelist","GetDragImage",5,0,0,0x0},{"internal-imagelist","GetItemFlags",3,0,0,0x2},{"internal-imagelist","GetOverlayImage",3,0,0,0x2}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_comctl_misc(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"comctl32 雜項",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto name=service.symbol;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto api=[&](std::string_view dll,std::string_view symbol,std::initializer_list<std::uint64_t> arguments){return nested_service(s,m,dll,symbol,arguments);};
        const auto entry=[&](std::string_view dll,std::string_view symbol){return resolve(dll,symbol).value;};
        const auto rect_at=[&](std::uint64_t at){std::array<std::int32_t,4> r{};for(unsigned i=0;i<4;++i)r[i]=as_int(m.load(GuestAddress{at+i*4},32));return r;};
        const auto store_rect=[&](std::uint64_t at,const std::array<std::int32_t,4>& r){for(unsigned i=0;i<4;++i)m.store(GuestAddress{at+i*4},32,static_cast<std::uint32_t>(r[i]));};
        // HMENU 等控制代碼在 LPINT 陣列中以 int 保存：符號延伸回原寬度。
        const auto handle_from_int=[&](std::uint32_t value){return x86_?std::uint64_t{value}:static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));};

        if(name=="InitializeFlatSB" || name=="UninitializeFlatSB" || name.starts_with("FlatSB_")){
            const auto window=gui_.windows.find(call(0));
            if(name=="InitializeFlatSB"){
                if(window==gui_.windows.end())co_return finish(0);
                if(!window->second.flat_scroll){
                    // WSB_PROP_CYVSCROLL、CXHSCROLL、CYHSCROLL、CXVSCROLL、CXHTHUMB、CYVTHUMB、VBKGCOLOR、HBKGCOLOR、VSTYLE、HSTYLE、WINSTYLE、PALETTE。
                    const auto metric=[&](unsigned index){return static_cast<std::int64_t>(as_int(api("user32.dll","GetSystemMetrics",{index})));};
                    window->second.flat_scroll=std::array<std::int64_t,12>{metric(20),metric(21),metric(3),metric(2),metric(10),metric(9),
                        static_cast<std::int64_t>(gui_system_color(0)),static_cast<std::int64_t>(gui_system_color(0)),0,0,0,0};
                }
                co_return finish(1);
            }
            if(name=="UninitializeFlatSB"){if(window!=gui_.windows.end())window->second.flat_scroll.reset();co_return finish(0);} // S_OK
            // WSB_PROP_* 是單一位元（0x1～0x800）；WINSTYLE 唯讀，回報視窗的 WS_HSCROLL／WS_VSCROLL。
            const auto index=static_cast<std::uint32_t>(call(1));
            if(window==gui_.windows.end() || !window->second.flat_scroll || !index || (index&(index-1)) || index>0x800U)co_return finish(0);
            const auto slot=static_cast<unsigned>(std::countr_zero(index));auto& props=*window->second.flat_scroll;
            if(name=="FlatSB_GetScrollProp"){
                if(!call(2))co_return finish(0);
                const auto value=index==0x400U?static_cast<std::int64_t>(window->second.style&0x00300000U):props[slot];
                m.store(GuestAddress{call(2)},32,static_cast<std::uint32_t>(value));co_return finish(1);
            }
            if(index==0x400U)co_return finish(0);
            props[slot]=x86_?static_cast<std::int64_t>(as_int(call(2))):static_cast<std::int64_t>(call(2));
            if(static_cast<std::uint32_t>(call(3)))present_gui(call(0));
            co_return finish(1);
        }
        if(name.ends_with("MRUListW") || name=="AddMRUStringW" || name=="FreeMRUList"){
            const auto value_name=[&](std::u16string_view text){const auto at=scratch(text.size()*2+2);m.write(GuestAddress{at},utf16_bytes(text));return at;};
            // 寫入一個格位與 MRUList（REG_SZ，含結尾 NUL）。
            const auto write_value=[&](const GuiMruList& list,std::u16string_view key,std::span<const std::uint8_t> bytes,std::uint32_t type)->service_callback::Task<std::uint32_t>{
                const auto data=scratch(bytes.size());m.write(GuestAddress{data},bytes);
                co_return static_cast<std::uint32_t>(co_await service_callback::call(entry("advapi32.dll","RegSetValueExW"),{list.key,value_name(key),0,type,data,bytes.size()}));
            };
            const auto order_bytes=[&](const GuiMruList& list){return utf16_bytes(list.order);}; // utf16_bytes 已含結尾 NUL。
            if(name=="CreateMRUListW"){
                // CREATEMRULISTW：Win64 {cbSize, nMaxItems, dwFlags, hKey@16, lpszSubKey@24, lpfnCompare@32}（40 bytes）；Win32 為 24 bytes。
                const auto info=call(0);if(!info)co_return finish(0);
                const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{info},32));if(size<(x86_?24U:40U))co_return finish(0);
                GuiMruList list;list.max=std::min<std::uint32_t>(static_cast<std::uint32_t>(m.load(GuestAddress{info+4},32)),26U);list.flags=static_cast<std::uint32_t>(m.load(GuestAddress{info+8},32));
                const auto bits=static_cast<std::uint8_t>(unit*8);
                const auto root=m.load(GuestAddress{info+(x86_?12U:16U)},bits),subkey=m.load(GuestAddress{info+(x86_?16U:24U)},bits);
                list.compare=m.load(GuestAddress{info+(x86_?20U:32U)},bits);
                if(!list.max || !root || !subkey || (list.flags&~3U))co_return finish(0);
                const auto opened=scratch(unit);
                if(api("advapi32.dll","RegCreateKeyExW",{root,subkey,0,0,0,0xf003fU,0,opened,0}))co_return finish(0);
                list.key=m.load(GuestAddress{opened},bits);
                // 讀取 MRUList 與各格位；不合法的字母、重複或已缺值的格位略過。
                const auto query=[&](std::u16string_view key)->std::optional<std::vector<std::uint8_t>>{
                    const auto type=scratch(4),length=scratch(4);const auto label=value_name(key);
                    if(api("advapi32.dll","RegQueryValueExW",{list.key,label,0,type,0,length}))return std::nullopt;
                    const auto bytes=static_cast<std::uint32_t>(m.load(GuestAddress{length},32));if(bytes>65536)return std::nullopt;
                    const auto buffer=scratch(bytes);
                    if(api("advapi32.dll","RegQueryValueExW",{list.key,label,0,type,buffer,length}))return std::nullopt;
                    std::vector<std::uint8_t> out(static_cast<std::size_t>(m.load(GuestAddress{length},32)));m.read(GuestAddress{buffer},out);return out;
                };
                if(const auto stored=query(u"MRUList")){
                    for(std::size_t i=0;i+1<stored->size();i+=2){
                        const auto letter=static_cast<char16_t>((*stored)[i]|((*stored)[i+1]<<8));if(!letter)break;
                        if(letter<u'a' || letter>=u'a'+list.max || list.order.find(letter)!=std::u16string::npos)continue;
                        if(auto data=query(std::u16string(1,letter))){list.slots[static_cast<std::size_t>(letter-u'a')]=std::move(*data);list.order.push_back(letter);}
                    }
                }
                const auto handle=gui_.next_object++;gui_.mru_lists.emplace(handle,std::move(list));co_return finish(handle);
            }
            const auto found=gui_.mru_lists.find(call(0));
            if(name=="FreeMRUList"){
                if(found==gui_.mru_lists.end())co_return finish(0);
                if(found->second.dirty){
                    for(const auto letter:found->second.order)(void)co_await write_value(found->second,std::u16string(1,letter),found->second.slots[static_cast<std::size_t>(letter-u'a')],(found->second.flags&1U)?3U:1U);
                    (void)co_await write_value(found->second,u"MRUList",order_bytes(found->second),1U);
                }
                (void)api("advapi32.dll","RegCloseKey",{found->second.key});gui_.mru_lists.erase(found);co_return finish(1);
            }
            if(found==gui_.mru_lists.end())co_return finish(UINT32_MAX);
            auto& list=found->second;
            if(name=="EnumMRUListW"){
                // 位置為負或沒有緩衝區時回傳項目數；字串項目以 lstrcpynW 語義複製，回傳複製的字元數（不含 NUL）。
                const auto position=as_int(call(1));const auto buffer=call(2);const auto capacity=static_cast<std::uint32_t>(call(3));
                if(position<0 || !buffer)co_return finish(list.order.size());
                if(static_cast<std::size_t>(position)>=list.order.size())co_return finish(UINT32_MAX);
                const auto& data=list.slots[static_cast<std::size_t>(list.order[static_cast<std::size_t>(position)]-u'a')];
                if(list.flags&1U){const auto bytes=std::min<std::size_t>(data.size(),capacity);m.write(GuestAddress{buffer},std::span(data).first(bytes));co_return finish(bytes);}
                if(!capacity)co_return finish(0);
                const auto characters=std::min<std::size_t>(data.size()/2?data.size()/2-1:0,capacity-1);
                m.write(GuestAddress{buffer},std::span(data).first(characters*2));m.store(GuestAddress{buffer+characters*2},16,0);
                co_return finish(characters);
            }
            // AddMRUStringW：已存在（比較為 0）就移到最前；否則用未使用的格位，滿了則取代最久未用的項目。回傳格位編號。
            if(!call(1) || (list.flags&1U))co_return finish(UINT32_MAX);
            const auto bytes=utf16_bytes(read_utf16(m,GuestAddress{call(1)}));
            std::optional<char16_t> match;
            for(const auto letter:list.order){
                const auto& data=list.slots[static_cast<std::size_t>(letter-u'a')];
                const auto stored=scratch(data.size()+2);m.write(GuestAddress{stored},data);
                const auto compared=list.compare?co_await service_callback::call(list.compare,{stored,call(1)}):api("kernel32.dll","lstrcmpiW",{stored,call(1)});
                if(!as_int(compared)){match=letter;break;}
            }
            char16_t letter=match.value_or(u'\0');
            if(match)list.order.erase(list.order.find(letter),1);
            else{
                if(list.order.size()<list.max){for(char16_t c=u'a';c<u'a'+list.max;++c)if(list.order.find(c)==std::u16string::npos){letter=c;break;}}
                else{letter=list.order.back();list.order.pop_back();}
                list.slots[static_cast<std::size_t>(letter-u'a')]=bytes;
            }
            list.order.insert(list.order.begin(),letter);
            if(list.flags&2U)list.dirty=true;
            else{
                if(!match && co_await write_value(list,std::u16string(1,letter),bytes,1U))co_return finish(UINT32_MAX);
                (void)co_await write_value(list,u"MRUList",order_bytes(list),1U);
            }
            co_return finish(static_cast<std::uint32_t>(letter-u'a'));
        }
        if(name.starts_with("ImageList_Drag") || name=="ImageList_BeginDrag" || name=="ImageList_EndDrag" || name=="ImageList_GetDragImage" || name=="ImageList_SetDragCursorImage"){
            auto& drag=gui_.image_drag;
            const auto window_dc=[&]{return api("user32.dll","GetDC",{drag.lock});};
            const auto release_dc=[&](std::uint64_t dc){(void)api("user32.dll","ReleaseDC",{drag.lock,dc});};
            const auto hide=[&]{
                if(!drag.saved)return;drag.saved=false;
                const auto dc=window_dc();if(!dc)return;
                (void)api("gdi32.dll","BitBlt",{dc,static_cast<std::uint32_t>(drag.saved_x),static_cast<std::uint32_t>(drag.saved_y),drag.saved_width,drag.saved_height,drag.saved_dc,0,0,0xcc0020U});
                release_dc(dc);
            };
            const auto show=[&]{
                const auto list=gui_.image_lists.find(drag.list);if(list==gui_.image_lists.end() || drag.saved)return;
                const auto width=static_cast<std::uint32_t>(list->second.width),height=static_cast<std::uint32_t>(list->second.height);
                const auto dc=window_dc();if(!dc)return;
                if(!drag.saved_dc || drag.saved_width!=width || drag.saved_height!=height){
                    if(drag.saved_dc){(void)api("gdi32.dll","SelectObject",{drag.saved_dc,drag.saved_previous});(void)api("gdi32.dll","DeleteDC",{drag.saved_dc});(void)api("gdi32.dll","DeleteObject",{drag.saved_bitmap});}
                    drag.saved_dc=api("gdi32.dll","CreateCompatibleDC",{dc});drag.saved_bitmap=api("gdi32.dll","CreateCompatibleBitmap",{dc,width,height});
                    drag.saved_previous=api("gdi32.dll","SelectObject",{drag.saved_dc,drag.saved_bitmap});drag.saved_width=width;drag.saved_height=height;
                }
                drag.saved_x=drag.x-drag.hotspot_x-drag.client_x;drag.saved_y=drag.y-drag.hotspot_y-drag.client_y;
                (void)api("gdi32.dll","BitBlt",{drag.saved_dc,0,0,width,height,dc,static_cast<std::uint32_t>(drag.saved_x),static_cast<std::uint32_t>(drag.saved_y),0xcc0020U});
                (void)draw_image_list(m,list->second,0,dc,drag.saved_x,drag.saved_y,0,0,0,0,clr_none,clr_default,1U);
                drag.saved=true;release_dc(dc);
            };
            const auto end=[&]{
                hide();
                if(drag.saved_dc){(void)api("gdi32.dll","SelectObject",{drag.saved_dc,drag.saved_previous});(void)api("gdi32.dll","DeleteDC",{drag.saved_dc});(void)api("gdi32.dll","DeleteObject",{drag.saved_bitmap});}
                gui_.image_lists.erase(drag.list);drag=GuiImageDrag{};
            };
            if(name=="ImageList_BeginDrag"){
                // 拖曳影像清單只含被拖曳的那一張；先結束進行中的拖曳。
                const auto source=gui_.image_lists.find(call(0));const auto index=static_cast<std::uint32_t>(call(1));
                if(source==gui_.image_lists.end() || index>=source->second.images.size())co_return finish(0);
                end();
                GuiImageList list;list.width=source->second.width;list.height=source->second.height;list.flags=source->second.flags;list.images.push_back(source->second.images[index]);
                drag.list=gui_.next_object++;gui_.image_lists.emplace(drag.list,std::move(list));
                drag.hotspot_x=as_int(call(2));drag.hotspot_y=as_int(call(3));co_return finish(1);
            }
            if(name=="ImageList_EndDrag"){end();co_return finish(0);}
            if(name=="ImageList_GetDragImage"){
                if(!drag.list)co_return finish(0);
                if(call(0)){m.store(GuestAddress{call(0)},32,static_cast<std::uint32_t>(drag.x));m.store(GuestAddress{call(0)+4},32,static_cast<std::uint32_t>(drag.y));}
                if(call(1)){m.store(GuestAddress{call(1)},32,static_cast<std::uint32_t>(drag.hotspot_x));m.store(GuestAddress{call(1)+4},32,static_cast<std::uint32_t>(drag.hotspot_y));}
                co_return finish(drag.list);
            }
            if(!drag.list)co_return finish(0);
            if(name=="ImageList_SetDragCursorImage"){
                // 新影像的熱點 (dx,dy) 對齊拖曳影像的熱點：以 ImageList_Merge 合成，熱點依合成後的原點平移。
                const auto cursor=gui_.image_lists.find(call(0));const auto index=static_cast<std::uint32_t>(call(1));
                if(cursor==gui_.image_lists.end() || index>=cursor->second.images.size())co_return finish(0);
                const auto ox=drag.hotspot_x-as_int(call(2)),oy=drag.hotspot_y-as_int(call(3));
                const auto merged=api("comctl32.dll","ImageList_Merge",{drag.list,0,call(0),index,static_cast<std::uint32_t>(ox),static_cast<std::uint32_t>(oy)});
                if(!merged)co_return finish(0);
                const bool visible=drag.saved;hide();
                gui_.image_lists.erase(drag.list);drag.list=merged;drag.hotspot_x-=std::min(0,ox);drag.hotspot_y-=std::min(0,oy);
                if(visible)show();
                co_return finish(1);
            }
            if(name=="ImageList_DragEnter"){
                hide();drag.lock=call(0);drag.x=as_int(call(1));drag.y=as_int(call(2));drag.client_x=0;drag.client_y=0;
                if(drag.lock){
                    const auto rect=scratch(16),origin=scratch(8);
                    if(api("user32.dll","GetWindowRect",{drag.lock,rect}) && api("user32.dll","ClientToScreen",{drag.lock,origin})){
                        drag.client_x=as_int(m.load(GuestAddress{origin},32))-as_int(m.load(GuestAddress{rect},32));
                        drag.client_y=as_int(m.load(GuestAddress{origin+4},32))-as_int(m.load(GuestAddress{rect+4},32));
                    }
                }
                drag.entered=true;drag.shown=true;show();co_return finish(1);
            }
            if(name=="ImageList_DragLeave"){hide();drag.entered=false;co_return finish(1);}
            if(name=="ImageList_DragMove"){
                const bool visible=drag.saved;if(visible)hide();
                drag.x=as_int(call(0));drag.y=as_int(call(1));
                if(visible)show();
                co_return finish(1);
            }
            // ImageList_DragShowNolock：只切換顯示，不鎖定視窗更新。
            if(static_cast<std::uint32_t>(call(0))){drag.shown=true;if(drag.entered)show();}
            else{hide();drag.shown=false;}
            co_return finish(1);
        }
        if(name=="HIMAGELIST_QueryInterface" || service.family=="internal-imagelist"){
            constexpr std::uint32_t s_ok=0,e_fail=0x80004005U,e_invalidarg=0x80070057U,e_nointerface=0x80004002U,e_pointer=0x80004003U;
            const auto bits=static_cast<std::uint8_t>(unit*8);
            // IID_IUnknown {00000000-0000-0000-C000-000000000046}、IID_IImageList {46EB5926-582E-4017-9FDF-E8998DAA0950}。
            const auto supported=[&](std::uint64_t iid){
                std::array<std::uint8_t,16> value{};m.read(GuestAddress{iid},value);
                constexpr std::array<std::uint8_t,16> unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},image_list{0x26,0x59,0xeb,0x46,0x2e,0x58,0x17,0x40,0x9f,0xdf,0xe8,0x99,0x8d,0xaa,0x09,0x50};
                return value==unknown || value==image_list;
            };
            // 取得（必要時建立）清單的 COM 物件並增加一份參照；新清單（Merge／Clone）只有這一份。
            const auto acquire=[&](std::uint64_t list)->std::uint64_t{
                if(!gui_.image_lists.contains(list))return 0;
                if(auto found=gui_.image_list_com.find(list);found!=gui_.image_list_com.end()){++found->second.references;return found->second.object;}
                if(!gui_.image_list_vtable){
                    std::size_t count=0;for(const auto& entry:comctl_misc_service_descriptors())if(entry.family=="internal-imagelist")++count;
                    const auto table=runtime_allocate(m,{},count*unit);std::size_t i=0;
                    for(const auto& entry:comctl_misc_service_descriptors())if(entry.family=="internal-imagelist")m.store(GuestAddress{table+unit*i++},bits,resolve(entry.family,entry.symbol).value);
                    gui_.image_list_vtable=table;
                }
                const auto object=allocate_heap(m,unit);if(!object)return 0;
                m.store(GuestAddress{object},bits,gui_.image_list_vtable);gui_.image_list_com[list]={object,1};return object;
            };
            const auto handle_of=[&](std::uint64_t object)->std::uint64_t{
                for(const auto& [list,com]:gui_.image_list_com)if(com.object==object && object)return list;
                return 0;
            };
            const auto hand_out=[&](std::uint64_t list,std::uint64_t iid,std::uint64_t out,bool fresh)->std::uint32_t{
                if(!out)return e_pointer;m.store(GuestAddress{out},bits,0);
                if(!list)return e_fail;
                if(!supported(iid)){if(fresh)(void)api("comctl32.dll","ImageList_Destroy",{list});return e_nointerface;}
                const bool existed=gui_.image_list_com.contains(list);
                const auto object=acquire(list);if(!object)return e_fail;
                if(!existed && !fresh)++gui_.image_list_com.at(list).references; // 既有清單第一次包裝：另計建立者的參照
                m.store(GuestAddress{out},bits,object);return s_ok;
            };
            if(name=="HIMAGELIST_QueryInterface"){
                if(!call(2))co_return finish(e_pointer);
                if(!gui_.image_lists.contains(call(0))){m.store(GuestAddress{call(2)},bits,0);co_return finish(e_invalidarg);}
                co_return finish(hand_out(call(0),call(1),call(2),false));
            }
            const auto self=call(0);const auto list=handle_of(self);
            if(!list)co_return finish(e_fail);
            const auto result_int=[&](std::uint64_t value,std::uint64_t out)->std::uint32_t{
                if(as_int(value)<0)return e_fail;if(out)m.store(GuestAddress{out},32,static_cast<std::uint32_t>(value));return s_ok;};
            const auto ok=[&](std::uint64_t value){return static_cast<std::uint32_t>(value)?s_ok:e_fail;};
            const auto release_list=[&]{
                auto& com=gui_.image_list_com.at(list);if(--com.references)return com.references;
                (void)api("comctl32.dll","ImageList_Destroy",{list});return 0U;
            };
            if(name=="QueryInterface"){
                if(!call(2))co_return finish(e_pointer);
                if(!supported(call(1))){m.store(GuestAddress{call(2)},bits,0);co_return finish(e_nointerface);}
                ++gui_.image_list_com.at(list).references;m.store(GuestAddress{call(2)},bits,self);co_return finish(s_ok);
            }
            if(name=="AddRef")co_return finish(++gui_.image_list_com.at(list).references);
            if(name=="Release")co_return finish(release_list());
            if(!gui_.image_lists.contains(list))co_return finish(e_fail);
            const auto il=[&](std::string_view symbol,std::initializer_list<std::uint64_t> arguments){return api("comctl32.dll",symbol,arguments);};
            if(name=="Add")co_return finish(result_int(il("ImageList_Add",{list,call(1),call(2)}),call(3)));
            if(name=="ReplaceIcon")co_return finish(result_int(il("ImageList_ReplaceIcon",{list,call(1),call(2)}),call(3)));
            if(name=="SetOverlayImage")co_return finish(ok(il("ImageList_SetOverlayImage",{list,call(1),call(2)})));
            if(name=="Replace")co_return finish(ok(il("ImageList_Replace",{list,call(1),call(2),call(3)})));
            if(name=="AddMasked")co_return finish(result_int(il("ImageList_AddMasked",{list,call(1),call(2)}),call(3)));
            if(name=="Draw"){
                // IMAGELISTDRAWPARAMS 的 himl 由介面本身決定：複製一份並填入這個清單。
                if(!call(1))co_return finish(e_invalidarg);
                const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{call(1)},32));if(size<(x86_?56U:72U) || size>(x86_?68U:88U))co_return finish(e_invalidarg);
                const auto params=scratch(size);std::vector<std::uint8_t> bytes(size);m.read(GuestAddress{call(1)},bytes);m.write(GuestAddress{params},bytes);
                m.store(GuestAddress{params+(x86_?4U:8U)},bits,list);co_return finish(ok(il("ImageList_DrawIndirect",{params})));
            }
            if(name=="Remove")co_return finish(ok(il("ImageList_Remove",{list,call(1)})));
            if(name=="GetIcon"){if(!call(3))co_return finish(e_pointer);const auto icon=il("ImageList_GetIcon",{list,call(1),call(2)});m.store(GuestAddress{call(3)},bits,icon);co_return finish(icon?s_ok:e_fail);}
            if(name=="GetImageInfo")co_return finish(ok(il("ImageList_GetImageInfo",{list,call(1),call(2)})));
            if(name=="Copy"){const auto source=handle_of(call(2));co_return finish(source?ok(il("ImageList_Copy",{list,call(1),source,call(3),call(4)})):e_invalidarg);}
            if(name=="Merge"){
                const auto other=handle_of(call(2));if(!other)co_return finish(e_invalidarg);
                const auto merged=il("ImageList_Merge",{list,call(1),other,call(3),call(4),call(5)});
                co_return finish(hand_out(merged,call(6),call(7),true));
            }
            if(name=="Clone")co_return finish(hand_out(il("ImageList_Duplicate",{list}),call(1),call(2),true));
            if(name=="GetImageRect")co_return finish(ok(il("ImageList_GetImageRect",{list,call(1),call(2)})));
            if(name=="GetIconSize")co_return finish(ok(il("ImageList_GetIconSize",{list,call(1),call(2)})));
            if(name=="SetIconSize")co_return finish(ok(il("ImageList_SetIconSize",{list,call(1),call(2)})));
            if(name=="GetImageCount"){if(!call(1))co_return finish(e_pointer);m.store(GuestAddress{call(1)},32,il("ImageList_GetImageCount",{list}));co_return finish(s_ok);}
            if(name=="SetImageCount")co_return finish(ok(il("ImageList_SetImageCount",{list,call(1)})));
            if(name=="SetBkColor"){const auto old=il("ImageList_SetBkColor",{list,call(1)});if(call(2))m.store(GuestAddress{call(2)},32,old);co_return finish(s_ok);}
            if(name=="GetBkColor"){if(!call(1))co_return finish(e_pointer);m.store(GuestAddress{call(1)},32,il("ImageList_GetBkColor",{list}));co_return finish(s_ok);}
            if(name=="BeginDrag")co_return finish(ok(il("ImageList_BeginDrag",{list,call(1),call(2),call(3)})));
            if(name=="EndDrag"){(void)il("ImageList_EndDrag",{});co_return finish(s_ok);}
            if(name=="DragEnter")co_return finish(ok(il("ImageList_DragEnter",{call(1),call(2),call(3)})));
            if(name=="DragLeave")co_return finish(ok(il("ImageList_DragLeave",{call(1)})));
            if(name=="DragMove")co_return finish(ok(il("ImageList_DragMove",{call(1),call(2)})));
            if(name=="SetDragCursorImage"){const auto cursor=handle_of(call(1));co_return finish(cursor?ok(il("ImageList_SetDragCursorImage",{cursor,call(2),call(3),call(4)})):e_invalidarg);}
            if(name=="DragShowNolock")co_return finish(ok(il("ImageList_DragShowNolock",{call(1)})));
            if(name=="GetDragImage"){
                const auto drag=il("ImageList_GetDragImage",{call(1),call(2)});
                if(!call(4))co_return finish(e_pointer);if(!drag){m.store(GuestAddress{call(4)},bits,0);co_return finish(e_fail);}
                co_return finish(hand_out(drag,call(3),call(4),false));
            }
            if(name=="GetItemFlags"){
                // ILIF_ALPHA：影像帶有 alpha 色版。
                const auto index=static_cast<std::uint32_t>(call(1));const auto& images=gui_.image_lists.at(list).images;
                if(!call(2) || index>=images.size())co_return finish(e_invalidarg);
                bool alpha=images[index].alpha;
                if(!alpha && images[index].rgba.size())alpha=std::any_of(images[index].rgba.begin(),images[index].rgba.end(),[](std::uint32_t p){const auto a=p>>24;return a && a!=255;});
                m.store(GuestAddress{call(2)},32,alpha?1U:0U);co_return finish(s_ok);
            }
            // GetOverlayImage(iOverlay, piIndex)：覆蓋遮罩 1～15。
            const auto overlay=static_cast<std::uint32_t>(call(1));
            if(!call(2) || overlay<1 || overlay>15)co_return finish(e_invalidarg);
            m.store(GuestAddress{call(2)},32,static_cast<std::uint32_t>(gui_.image_lists.at(list).overlays[overlay-1]));co_return finish(s_ok);
        }
        if(name=="ImageList_Write" || name=="ImageList_Read"){
            const auto bits=static_cast<std::uint8_t>(unit*8);
            const auto stream=name=="ImageList_Write"?call(1):call(0);if(!stream)co_return finish(0);
            // IStream::Read／Write(this, pv, cb, pcb)：要求完整傳輸。
            const auto transfer=[&](unsigned method,std::uint64_t buffer,std::uint64_t size)->service_callback::Task<bool>{
                const auto table=m.load(GuestAddress{stream},bits);const auto function=m.load(GuestAddress{table+method*unit},bits);
                const auto done=scratch(4);
                const auto status=static_cast<std::uint32_t>(co_await service_callback::call(function,{stream,buffer,size,done}));
                co_return status<0x80000000U && m.load(GuestAddress{done},32)==size;
            };
            constexpr std::uint32_t columns=4;
            if(name=="ImageList_Write"){
                const auto found=gui_.image_lists.find(call(0));if(found==gui_.image_lists.end())co_return finish(0);
                const auto list=found->second;const auto count=static_cast<std::uint32_t>(list.images.size());
                const auto cx=static_cast<std::uint32_t>(list.width),cy=static_cast<std::uint32_t>(list.height);
                const auto rows=std::max<std::uint32_t>(1,(count+columns-1)/columns);
                if(count>65535 || cx>65535 || cy>65535 || static_cast<std::uint64_t>(cx)*columns*cy*rows>16U*1024U*1024U)co_return finish(0);
                std::vector<std::vector<std::uint32_t>> pixels;for(const auto& icon:list.images)pixels.push_back(straight_pixels(icon));
                const auto sample=[&](std::uint32_t x,std::uint32_t y)->std::uint32_t{
                    const auto n=(y/cy)*columns+x/cx;if(n>=count)return 0;const auto& icon=list.images[n];
                    const auto ix=x%cx,iy=y%cy;if(ix>=icon.width || iy>=icon.height)return 0;
                    return pixels[n][static_cast<std::size_t>(iy)*icon.width+ix];
                };
                std::array<std::uint8_t,28> head{};
                const auto put=[&](std::size_t at,std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)head[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                put(0,0x4c49U,2);put(2,0x101U,2);put(4,count,2);put(6,rows*columns,2);put(8,columns,2);put(10,cx,2);put(12,cy,2);put(14,list.background,4);put(18,list.flags,2);
                for(unsigned i=0;i<4;++i)put(20+i*2,static_cast<std::uint16_t>(static_cast<std::int16_t>(list.overlays[i])),2);
                const bool alpha=(list.flags&0xfeU)==0x20U;
                // 色彩條帶：透明像素寫黑色；32 位元時保留 alpha。遮罩：透明為白（1）。
                const auto color=bitmap_file(cx*columns,cy*rows,alpha?32U:24U,[&](std::uint32_t x,std::uint32_t y){
                    const auto value=sample(x,y);if(!(value>>24))return 0U;
                    return alpha?value:(value&0xffffffU);});
                std::vector<std::uint8_t> payload(head.begin(),head.end());payload.insert(payload.end(),color.begin(),color.end());
                if(list.flags&1U){const auto mask=bitmap_file(cx*columns,cy*rows,1,[&](std::uint32_t x,std::uint32_t y){return (sample(x,y)>>24)?0U:1U;});
                    payload.insert(payload.end(),mask.begin(),mask.end());}
                const auto buffer=scratch(payload.size());m.write(GuestAddress{buffer},payload);
                co_return finish((co_await transfer(4,buffer,payload.size()))?1U:0U);
            }
            // ImageList_Read：逐段讀取，BMP 依 bfSize 取得完整大小。
            const auto head=scratch(28);if(!co_await transfer(3,head,28))co_return finish(0);
            const auto word=[&](unsigned at){return static_cast<std::uint32_t>(m.load(GuestAddress{head+at},16));};
            if(word(0)!=0x4c49U || word(2)!=0x101U)co_return finish(0);
            const auto count=word(4),cx=word(10),cy=word(12),flags=word(18);
            if(!cx || !cy || cx>4096 || cy>4096)co_return finish(0);
            const auto read_bitmap=[&]()->service_callback::Task<std::optional<GuiBitmap>>{
                const auto file=scratch(14);if(!co_await transfer(3,file,14))co_return std::nullopt;
                if(m.load(GuestAddress{file},16)!=0x4d42U)co_return std::nullopt;
                const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{file+2},32));if(size<=54 || size>64U*1024U*1024U)co_return std::nullopt;
                const auto body=scratch(size-14);if(!co_await transfer(3,body,size-14))co_return std::nullopt;
                std::vector<std::uint8_t> bytes(size-14);m.read(GuestAddress{body},bytes);
                try{co_return decode_dib_bitmap(bytes);}catch(const HostCallFault&){co_return std::nullopt;}
            };
            const auto color=co_await read_bitmap();if(!color)co_return finish(0);
            std::optional<GuiBitmap> mask;if(flags&1U){mask=co_await read_bitmap();if(!mask)co_return finish(0);}
            const auto rows=(count+columns-1)/columns;
            if(color->width<cx*columns || color->height<cy*rows || (mask && (mask->width<color->width || mask->height<color->height)))co_return finish(0);
            GuiImageList list;list.width=static_cast<std::int32_t>(cx);list.height=static_cast<std::int32_t>(cy);list.flags=flags;
            list.background=static_cast<std::uint32_t>(m.load(GuestAddress{head+14},32));
            for(std::uint32_t n=0;n<count;++n){
                GuiIcon icon;icon.width=cx;icon.height=cy;icon.rgba.resize(static_cast<std::size_t>(cx)*cy);
                const auto left=(n%columns)*cx,top=(n/columns)*cy;
                for(std::uint32_t y=0;y<cy;++y)for(std::uint32_t x=0;x<cx;++x){
                    const auto at=static_cast<std::size_t>(top+y)*color->width+left+x;
                    const bool clear=mask && (mask->pixels[at]&0xffffffU)!=0;
                    icon.rgba[static_cast<std::size_t>(y)*cx+x]=(color->pixels[at]&0xffffffU)|(clear?0U:0xff000000U);
                }
                finish_icon(icon);list.images.push_back(std::move(icon));
            }
            for(unsigned i=0;i<4;++i){const auto overlay=static_cast<std::int16_t>(word(20+i*2));list.overlays[i]=overlay>=0 && static_cast<std::uint32_t>(overlay)<count?overlay:-1;}
            const auto handle=gui_.next_object++;gui_.image_lists.emplace(handle,std::move(list));co_return finish(handle);
        }
        if(name=="GetMUILanguage")co_return finish(gui_.comctl_mui_language);
        if(name=="InitMUILanguage"){gui_.comctl_mui_language=static_cast<std::uint16_t>(call(0));co_return finish(0);}
        if(name=="GetEffectiveClientRect"){
            // 由工作區扣掉可見控制項：成對整數（非零值、控制項 ID），第一對代表選單而略過，以非零值為 0 的一對結束。
            const auto window=call(0),rect=call(1),info=call(2);
            if(!rect || !api("user32.dll","GetClientRect",{window,rect}))co_return finish(0);
            if(info){
                const auto control_rect=scratch(16);
                for(std::uint64_t at=info+8,n=0;n<4096;at+=8,++n){
                    if(!m.load(GuestAddress{at},32))break;
                    const auto control=api("user32.dll","GetDlgItem",{window,m.load(GuestAddress{at+4},32)});
                    if(!control || !api("user32.dll","IsWindowVisible",{control}))continue;
                    if(!api("user32.dll","GetWindowRect",{control,control_rect}))continue;
                    (void)api("user32.dll","MapWindowPoints",{0,window,control_rect,2});
                    (void)api("user32.dll","SubtractRect",{rect,rect,control_rect});
                }
            }
            co_return finish(0);
        }
        if(name=="ShowHideMenuCtl"){
            // 成對整數：第一對為（非零值、主選單 HMENU），之後為（選單項目 ID、控制項 ID）；依項目目前是否勾選切換勾選與顯示。
            const auto window=call(0),info=call(2);const auto item=static_cast<std::uint32_t>(call(1));
            if(!info)co_return finish(0);
            const auto menu=handle_from_int(static_cast<std::uint32_t>(m.load(GuestAddress{info+4},32)));
            std::optional<std::uint32_t> control;
            for(std::uint64_t at=info+8,n=0;n<4096;at+=8,++n){
                const auto first=static_cast<std::uint32_t>(m.load(GuestAddress{at},32));if(!first)break;
                if(first==item){control=static_cast<std::uint32_t>(m.load(GuestAddress{at+4},32));break;}
            }
            if(!control)co_return finish(0);
            const auto checked=static_cast<std::uint32_t>(api("user32.dll","GetMenuState",{menu,item,0}));
            if(checked==UINT32_MAX)co_return finish(0);
            const bool show=!(checked&8U);
            (void)api("user32.dll","CheckMenuItem",{menu,item,show?8U:0U});
            const auto target=api("user32.dll","GetDlgItem",{window,*control});
            if(target)(void)co_await service_callback::call(entry("user32.dll","ShowWindow"),{target,show?5U:0U});
            co_return finish(1);
        }
        if(name.starts_with("DrawStatusText")){
            // 外框：SBT_NOBORDERS 不畫、SBT_POPOUT 凸起，其餘凹陷；文字以 Tab 分段（一個 Tab 之後置中、兩個之後靠右），
            // 透明背景、單行垂直置中。A 版直接以原字串片段交給 DrawTextA，沿用其字碼頁轉換。
            const bool wide=name.ends_with('W');const auto dc=call(0),text=call(2);const auto flags=static_cast<std::uint32_t>(call(3));
            if(!call(1))co_return finish(0);
            const auto rect=scratch(16);store_rect(rect,rect_at(call(1)));
            if(!(flags&0x100U))(void)api("user32.dll","DrawEdge",{dc,rect,(flags&0x200U)?1U:2U,0x200fU});
            if(text){
                std::vector<std::size_t> starts{0};std::size_t length=0;
                if(wide){const auto value=read_utf16(m,GuestAddress{text});length=value.size();for(std::size_t i=0;i<value.size();++i)if(value[i]==u'\t')starts.push_back(i+1);}
                else{const auto value=read_ansi(m,text);length=value.size();for(std::size_t i=0;i<value.size();++i)if(value[i]=='\t')starts.push_back(i+1);}
                auto area=rect_at(rect);area[0]+=3;area[2]-=3;store_rect(rect,area);
                const auto mode=api("gdi32.dll","SetBkMode",{dc,1});
                const unsigned extra=(flags&0x400U)?0x20000U:0U;
                for(std::size_t part=0;part<starts.size() && part<3;++part){
                    const auto begin=starts[part];
                    const auto end=part+1<starts.size() && part<2?starts[part+1]-1:length;
                    if(end<=begin)continue;
                    const unsigned align=part==0?0U:part==1?1U:2U;
                    // 第三段之後的 Tab 留在靠右的文字內。
                    (void)api("user32.dll",wide?"DrawTextW":"DrawTextA",{dc,text+begin*(wide?2U:1U),end-begin,rect,align|0x24U|extra});
                }
                if(mode)(void)api("gdi32.dll","SetBkMode",{dc,mode});
            }
            co_return finish(0);
        }
        if(name=="CreateUpDownControl"){
            // 以 CreateWindowExW 建立 msctls_updown32，再依序設定夥伴視窗、範圍（MAKELONG(nUpper,nLower)）與位置。
            const std::u16string_view class_name=u"msctls_updown32";
            const auto text=scratch(class_name.size()*2+2);m.write(GuestAddress{text},utf16_bytes(class_name));
            const auto window=co_await service_callback::call_value(entry("user32.dll","CreateWindowExW"),
                {0,text,0,call(0),call(1),call(2),call(3),call(4),call(5),call(6),call(7),0});
            if(window){
                const auto send=entry("user32.dll","SendMessageW");
                (void)co_await service_callback::call(send,{window,0x469,call(8),0});
                (void)co_await service_callback::call(send,{window,0x465,0,(call(9)&0xffffU)|((call(10)&0xffffU)<<16)});
                (void)co_await service_callback::call(send,{window,0x467,0,call(11)&0xffffU});
            }
            co_return finish(window);
        }
        if(name=="CreateToolbar")
            co_return finish(api("comctl32.dll","CreateToolbarEx",{call(0),call(1),call(2),call(3),call(4),call(5),call(6),call(7),0,0,0,0,12}));
        if(name=="RegisterClassNameW"){
            if(!call(0))co_return finish(0);
            auto text=read_utf16(m,GuestAddress{call(0)},256);for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
            co_return finish(std::find(std::begin(comctl_classes),std::end(comctl_classes),std::u16string_view{text})!=std::end(comctl_classes)?1U:0U);
        }
        if(name=="CreateMappedBitmap"){
            // 預設對映：黑→COLOR_BTNTEXT、深灰→COLOR_BTNSHADOW、淺灰→COLOR_BTNFACE、白→COLOR_BTNHIGHLIGHT；lpColorMap 為 COLORMAP{from,to} 陣列。
            const auto bitmap=api("user32.dll","LoadBitmapW",{call(0),call(1)});
            if(!bitmap)co_return finish(0);
            std::vector<std::pair<std::uint32_t,std::uint32_t>> map;
            const auto count=as_int(call(4));
            if(call(3) && count>0){for(std::uint32_t i=0;i<static_cast<std::uint32_t>(count) && i<1024;++i)map.emplace_back(static_cast<std::uint32_t>(m.load(GuestAddress{call(3)+i*8U},32))&0xffffffU,
                static_cast<std::uint32_t>(m.load(GuestAddress{call(3)+i*8U+4},32))&0xffffffU);}
            else map={{0x000000U,gui_system_color(18)},{0x808080U,gui_system_color(16)},{0xc0c0c0U,gui_system_color(15)},{0xffffffU,gui_system_color(20)}};
            sync_raster_bitmap(m,bitmap);
            if(const auto found=gui_.bitmaps.find(bitmap);found!=gui_.bitmaps.end()){
                for(auto& pixel:found->second.pixels){const auto color=pixel&0xffffffU;
                    for(const auto& [from,to]:map)if(color==from){pixel=(pixel&0xff000000U)|(to&0xffffffU);break;}}
                // 像素已改寫：不再沿用資源樣板，光柵表面下次選入 DC 時依新像素建立（與 SetDIBits 相同）。
                rasters_.erase(bitmap);raster_shared_.erase(bitmap);found->second.resource_id=0;
            }
            co_return finish(bitmap);
        }
        // ImageList：控制代碼不存在時回傳 0（Duplicate／Merge／LoadImage 為 NULL）。
        if(name.starts_with("ImageList_LoadImage")){
            const bool wide=name.ends_with('W');const auto type=static_cast<std::uint32_t>(call(5));
            if(type>2)co_return finish(0);
            const auto width=as_int(call(2));const auto icon_size=width>0?static_cast<std::uint32_t>(width):32U;
            const auto image=api("user32.dll",wide?"LoadImageW":"LoadImageA",{call(0),call(1),type,type?icon_size:0U,type?icon_size:0U,call(6)});
            if(!image)co_return finish(0);
            std::uint64_t list=0;
            if(type==0){
                sync_raster_bitmap(m,image);const auto found=gui_.bitmaps.find(image);
                if(found!=gui_.bitmaps.end() && found->second.height){
                    const auto& bitmap=found->second;const auto cx=width>0?static_cast<std::uint32_t>(width):bitmap.height;
                    const auto depth=bitmap.bits_per_pixel;const std::uint32_t color=depth==32?0x20U:depth==24?0x18U:depth==16?0x10U:depth==8?8U:depth==4?4U:0U;
                    const bool masked=static_cast<std::uint32_t>(call(4))!=clr_none;
                    list=api("comctl32.dll","ImageList_Create",{cx,bitmap.height,color|(masked?1U:0U),std::max<std::uint32_t>(1,bitmap.width/std::max<std::uint32_t>(1,cx)),call(3)});
                    if(list){
                        const auto added=masked?api("comctl32.dll","ImageList_AddMasked",{list,image,call(4)}):api("comctl32.dll","ImageList_Add",{list,image,0});
                        if(as_int(added)<0){(void)api("comctl32.dll","ImageList_Destroy",{list});list=0;}
                    }
                }
                (void)api("gdi32.dll","DeleteObject",{image});
            }else{
                list=api("comctl32.dll","ImageList_Create",{icon_size,icon_size,0x21U,1,call(3)});
                if(list && as_int(api("comctl32.dll","ImageList_AddIcon",{list,image}))<0){(void)api("comctl32.dll","ImageList_Destroy",{list});list=0;}
                (void)api("user32.dll",type==1?"DestroyIcon":"DestroyCursor",{image});
            }
            co_return finish(list);
        }
        if(name=="ImageList_DrawIndirect"){
            // IMAGELISTDRAWPARAMS：Win64 {cbSize, himl@8, i@16, hdcDst@24, x,y,cx,cy,xBitmap,yBitmap@32..52, rgbBk@56, rgbFg@60, fStyle@64,
            // dwRop@68, fState@72, Frame@76, crEffect@80}（88 bytes，舊版 72）；Win32 全部 4 bytes 欄位（68 bytes，舊版 56）。
            const auto params=call(0);if(!params)co_return finish(0);
            const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{params},32));
            const unsigned head=x86_?4U:8U,base=x86_?16U:32U;const auto full=x86_?68U:88U,legacy=x86_?56U:72U;
            if(size!=full && size!=legacy)co_return finish(0);
            const auto found=gui_.image_lists.find(m.load(GuestAddress{params+head},static_cast<std::uint8_t>(unit*8)));
            if(found==gui_.image_lists.end())co_return finish(0);
            const auto field=[&](unsigned index){return static_cast<std::uint32_t>(m.load(GuestAddress{params+base+index*4},32));};
            const auto index=static_cast<std::uint32_t>(m.load(GuestAddress{params+(x86_?8U:16U)},32));
            const auto dc=m.load(GuestAddress{params+(x86_?12U:24U)},static_cast<std::uint8_t>(unit*8));
            const auto background=field(6)==clr_default?found->second.background:field(6);
            const auto effect=size==full?field(10):0U,alpha=size==full?field(11):255U;
            co_return finish(draw_image_list(m,found->second,index,dc,as_int(field(0)),as_int(field(1)),field(2),field(3),field(4),field(5),
                background,field(7),field(8),effect,std::min<std::uint32_t>(alpha,255U)));
        }
        if(name=="ImageList_Merge"){
            const auto first=gui_.image_lists.find(call(0)),second=gui_.image_lists.find(call(2));
            const auto i1=static_cast<std::uint32_t>(call(1)),i2=static_cast<std::uint32_t>(call(3));const auto dx=as_int(call(4)),dy=as_int(call(5));
            if(first==gui_.image_lists.end() || second==gui_.image_lists.end() || i1>=first->second.images.size() || i2>=second->second.images.size() ||
               std::abs(dx)>4096 || std::abs(dy)>4096)co_return finish(0);
            // 新清單只含一張影像：第一張在下、第二張以 (dx,dy) 疊在上；尺寸為兩者的聯集。
            const auto& bottom=first->second.images[i1];const auto& top=second->second.images[i2];
            const std::int32_t left=std::min(0,dx),upper=std::min(0,dy);
            const std::int32_t right=std::max<std::int32_t>(static_cast<std::int32_t>(bottom.width),dx+static_cast<std::int32_t>(top.width));
            const std::int32_t lower=std::max<std::int32_t>(static_cast<std::int32_t>(bottom.height),dy+static_cast<std::int32_t>(top.height));
            const auto width=static_cast<std::uint32_t>(right-left),height=static_cast<std::uint32_t>(lower-upper);
            if(!width || !height || width>4096 || height>4096)co_return finish(0);
            GuiIcon merged;merged.width=width;merged.height=height;merged.rgba.assign(static_cast<std::size_t>(width)*height,0);
            const auto place=[&](const GuiIcon& icon,std::int32_t ox,std::int32_t oy){
                const auto pixels=straight_pixels(icon);
                for(std::uint32_t y=0;y<icon.height;++y)for(std::uint32_t x=0;x<icon.width;++x){
                    const auto source=pixels[static_cast<std::size_t>(y)*icon.width+x];const auto a=source>>24;if(!a)continue;
                    auto& target=merged.rgba[static_cast<std::size_t>(y+static_cast<std::uint32_t>(oy))*width+x+static_cast<std::uint32_t>(ox)];
                    if(a==255){target=source;continue;}
                    // 未預乘的 source-over 合成。
                    const auto b=target>>24;const auto out=a+b*(255-a)/255;std::uint32_t value=out<<24;
                    for(unsigned c=0;c<3;++c){const auto sc=(source>>(c*8))&255U,tc=(target>>(c*8))&255U;value|=(out?(sc*a+tc*b*(255-a)/255)/out:0U)<<(c*8);}
                    target=value;
                }
            };
            place(bottom,-left,-upper);place(top,dx-left,dy-upper);
            finish_icon(merged);
            GuiImageList list;list.width=static_cast<std::int32_t>(width);list.height=static_cast<std::int32_t>(height);
            list.flags=first->second.flags|1U;list.images.push_back(std::move(merged));
            const auto handle=gui_.next_object++;gui_.image_lists.emplace(handle,std::move(list));co_return finish(handle);
        }
        if(name=="ImageList_Copy"){
            // himlSrc 必須與 himlDst 相同；ILCF_MOVE（0）複製來源影像到目的索引、ILCF_SWAP（1）互換。
            const auto found=gui_.image_lists.find(call(0));const auto target=static_cast<std::uint32_t>(call(1)),source=static_cast<std::uint32_t>(call(3));
            const auto flags=static_cast<std::uint32_t>(call(4));
            if(found==gui_.image_lists.end() || call(2)!=call(0) || flags>1 || target>=found->second.images.size() || source>=found->second.images.size())co_return finish(0);
            auto& images=found->second.images;
            if(flags)std::swap(images[target],images[source]);
            else if(target!=source)images[target]=images[source];
            co_return finish(1);
        }
        if(name=="ImageList_Duplicate"){
            const auto found=gui_.image_lists.find(call(0));if(found==gui_.image_lists.end() || gui_.image_lists.size()>=4096)co_return finish(0);
            auto copy=found->second;copy.image_bitmap=0;copy.mask_bitmap=0; // GetImageInfo 的條帶點陣圖屬原清單，複本查詢時另建。
            const auto handle=gui_.next_object++;gui_.image_lists.emplace(handle,std::move(copy));co_return finish(handle);
        }
        const auto found=gui_.image_lists.find(call(0));
        if(found==gui_.image_lists.end())co_return finish(0);
        auto& list=found->second;
        if(name=="ImageList_GetFlags")co_return finish(list.flags);
        if(name=="ImageList_SetFlags"){list.flags=static_cast<std::uint32_t>(call(1));co_return finish(1);}
        if(name=="ImageList_SetIconSize"){
            // 變更影像尺寸並移除全部影像。
            const auto cx=as_int(call(1)),cy=as_int(call(2));
            if(cx<=0 || cy<=0 || cx>4096 || cy>4096)co_return finish(0);
            list.width=cx;list.height=cy;list.images.clear();list.overlays.fill(-1);co_return finish(1);
        }
        if(name=="ImageList_GetImageRect"){
            // 與 ImageList_GetImageInfo 相同的內部條帶配置（每列 4 格）。
            const auto index=static_cast<std::uint32_t>(call(1));
            if(!call(2) || index>=list.images.size())co_return finish(0);
            constexpr std::uint32_t columns=4;
            store_rect(call(2),{static_cast<std::int32_t>(index%columns*static_cast<std::uint32_t>(list.width)),static_cast<std::int32_t>(index/columns*static_cast<std::uint32_t>(list.height)),
                static_cast<std::int32_t>((index%columns+1)*static_cast<std::uint32_t>(list.width)),static_cast<std::int32_t>((index/columns+1)*static_cast<std::uint32_t>(list.height))});
            co_return finish(1);
        }
        if(name=="ImageList_SetOverlayImage"){
            const auto image=as_int(call(1));const auto overlay=static_cast<std::uint32_t>(call(2));
            if(overlay<1 || overlay>15 || image<0 || static_cast<std::size_t>(image)>=list.images.size())co_return finish(0);
            list.overlays[overlay-1]=image;co_return finish(1);
        }
        // ImageList_SetFilter：Windows 文件載明未實作，一律回傳 FALSE。
        co_return finish(0);
    });
}
}
