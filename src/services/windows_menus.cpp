#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/interpreter.hpp"
#include <algorithm>
#include <bit>
#include <functional>
#include <set>
#include <cstdio>

namespace x64arm {
std::span<const WindowsServiceDescriptor> menu_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","CreateMenu",0},{"user32.dll","CreatePopupMenu",0},
        {"user32.dll","DestroyMenu",1},{"user32.dll","GetSystemMenu",2},{"user32.dll","IsMenu",1},
        {"user32.dll","AppendMenuA",4},{"user32.dll","AppendMenuW",4},
        {"user32.dll","InsertMenuA",5},{"user32.dll","InsertMenuW",5},
        {"user32.dll","ModifyMenuA",5},{"user32.dll","ModifyMenuW",5},
        {"user32.dll","DeleteMenu",3},{"user32.dll","RemoveMenu",3},
        {"user32.dll","EnableMenuItem",3},{"user32.dll","CheckMenuItem",3},
        {"user32.dll","GetMenuCheckMarkDimensions",0},{"user32.dll","SetMenuItemBitmaps",5},
        {"user32.dll","GetMenuState",3},{"user32.dll","GetMenuItemCount",1},
        {"user32.dll","GetMenuItemID",2},{"user32.dll","GetSubMenu",2},
        {"user32.dll","GetMenuBarInfo",4},{"user32.dll","GetMenuItemRect",4},{"user32.dll","MenuItemFromPoint",3,4},
        {"user32.dll","GetMenuStringA",5},{"user32.dll","GetMenuStringW",5},
        {"user32.dll","GetMenuItemInfoA",4},{"user32.dll","GetMenuItemInfoW",4},
        {"user32.dll","SetMenuItemInfoA",4},{"user32.dll","SetMenuItemInfoW",4},
        {"user32.dll","InsertMenuItemA",4},{"user32.dll","InsertMenuItemW",4},
        {"user32.dll","CheckMenuRadioItem",5},
        {"user32.dll","GetMenuInfo",2},{"user32.dll","SetMenuInfo",2},{"user32.dll","GetMenuDefaultItem",3},{"user32.dll","SetMenuDefaultItem",3},
        {"user32.dll","GetMenuContextHelpId",1},{"user32.dll","SetMenuContextHelpId",2},
        {"user32.dll","TrackPopupMenu",7},{"user32.dll","TrackPopupMenuEx",6},{"user32.dll","EndMenu",0},
        {"internal-menu-loop","ReadMessage",0}
    };
    return table;
}
namespace {
constexpr std::uint32_t popup = 0x10, by_position = 0x400, separator = 0x800;
constexpr std::size_t menu_limit = 8192, item_limit = 65536, depth_limit = 64;
using Location = std::pair<std::uint64_t,std::size_t>;
std::int32_t signed32(std::uint64_t value) { return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value)); }

std::optional<Location> find_item(const GuiState& gui,std::uint64_t handle,std::uint32_t item,std::uint32_t flags) {
    const auto found=gui.menus.find(handle);
    if(found==gui.menus.end())return std::nullopt;
    if(flags&by_position)return item<found->second.size()?std::optional{Location{handle,item}}:std::nullopt;
    std::set<std::uint64_t> visited;std::vector<std::uint64_t> pending{handle};
    while(!pending.empty()){
        const auto current=pending.back();pending.pop_back();
        if(!visited.insert(current).second)continue;
        const auto menu=gui.menus.find(current);if(menu==gui.menus.end())continue;
        for(std::size_t i=0;i<menu->second.size();++i){const auto& entry=menu->second[i];if(((entry.flags&popup)?entry.command:static_cast<std::uint32_t>(entry.id))==item)return Location{current,i};}
        for(auto it=menu->second.rbegin();it!=menu->second.rend();++it)if(it->flags&popup)pending.push_back(it->id);
    }
    return std::nullopt;
}

// 共享子選單是合法的；只拒絕循環，不把多個 parent 當成錯誤。
bool contains_menu(const GuiState& gui,std::uint64_t root,std::uint64_t target) {
    std::vector<std::uint64_t> pending{root};std::set<std::uint64_t> visited;
    while(!pending.empty()){
        const auto handle=pending.back();pending.pop_back();if(handle==target)return true;
        if(!visited.insert(handle).second)continue;
        const auto found=gui.menus.find(handle);if(found==gui.menus.end())continue;
        for(const auto& item:found->second)if(item.flags&popup)pending.push_back(item.id);
    }
    return false;
}
void destroy_menu(GuiState& gui,std::uint64_t root) {
    std::vector<std::uint64_t> pending{root};std::set<std::uint64_t> destroyed;
    while(!pending.empty()){
        const auto handle=pending.back();pending.pop_back();if(!destroyed.insert(handle).second)continue;
        const auto found=gui.menus.find(handle);if(found==gui.menus.end())continue;
        for(const auto& item:found->second)if(item.flags&popup)pending.push_back(item.id);
        gui.menus.erase(found);gui.menu_info.erase(handle);
    }
    std::erase_if(gui.system_menus,[&](const auto& entry){return destroyed.contains(entry.second);});
}
bool system_menu(const GuiState& gui,std::uint64_t owner,std::uint64_t menu) {
    const auto found=gui.system_menus.find(owner);return found!=gui.system_menus.end() && found->second==menu;
}

// native 一次接收一棵 snapshot；callback 完成後才建立，避免把初始化前的文字與狀態顯示出來。
bool menu_snapshot(const GuiState& gui,std::uint64_t root,std::vector<GuiPopupItem>& out,const std::function<GuiIcon(std::uint64_t)>& bitmap) {
    const auto root_info=gui.menu_info.find(root);const auto style=root_info==gui.menu_info.end()?0:root_info->second.style;
    std::set<std::uint64_t> path;std::size_t count=0;
    std::function<bool(std::uint64_t,std::vector<GuiPopupItem>&)> copy=[&](std::uint64_t handle,std::vector<GuiPopupItem>& items){
        if(path.size()>=depth_limit || !path.insert(handle).second)return false;
        const auto menu=gui.menus.find(handle);if(menu==gui.menus.end()){path.erase(handle);return true;}
        for(const auto& entry:menu->second){
            if(++count>item_limit)return false;
            GuiPopupItem item{entry.id,entry.flags|style,entry.text,{},handle,static_cast<std::uint32_t>(&entry-menu->second.data())};
            item.bitmap=bitmap(entry.bitmap);item.checked_bitmap=bitmap(entry.checked_bitmap);item.unchecked_bitmap=bitmap(entry.unchecked_bitmap);
            item.custom_checkmarks=entry.checked_bitmap || entry.unchecked_bitmap;
            if(entry.flags&popup){if(!gui.menus.contains(entry.id))item.flags|=3;else if(!copy(entry.id,item.children))return false;}
            items.push_back(std::move(item));
        }
        path.erase(handle);return true;
    };
    return copy(root,out);
}
bool selectable(const std::vector<GuiPopupItem>& items,const GuiMenuSelection& selection) {
    for(const auto& item:items){
        if(item.flags&(3|separator))continue;
        if(item.flags&popup){if(selectable(item.children,selection))return true;}
        else if(item.id==selection.command && item.menu==selection.menu && item.position==selection.position)return true;
    }
    return false;
}
std::optional<Location> next_popup(const GuiState& gui,const GuiMenuTrack& track) {
    if(gui.menus.contains(track.menu) && std::find(track.initialized.begin(),track.initialized.end(),track.menu)==track.initialized.end())return Location{track.menu,track.root_index};
    return std::nullopt;
}
}

void WindowsServices::destroy_gui_menu(std::uint64_t handle){
    if(handle)destroy_menu(gui_,handle);
}
bool WindowsServices::snapshot_gui_menu(GuestMemory& memory,std::uint64_t handle,std::vector<GuiPopupItem>& items){
    return menu_snapshot(gui_,handle,items,[&](std::uint64_t bitmap){
        GuiIcon image;const auto found=gui_.bitmaps.find(bitmap);if(found==gui_.bitmaps.end())return image;
        // DIB 直接寫入與先前 GPU 繪圖都要在交給 AppKit 前同步。
        sync_raster_bitmap(memory,bitmap);const auto& source=found->second;
        image.width=source.width;image.height=source.height;image.monochrome=source.bits_per_pixel==1;image.rgba.reserve(source.pixels.size());
        for(auto pixel:source.pixels)image.rgba.push_back(image.monochrome?((pixel&0xffffffU)?0U:0xff000000U):(pixel&0xffffffU)|0xff000000U);
        return image;
    });
}
void WindowsServices::present_gui_menu(GuestMemory& memory,std::uint64_t handle){
    if(!native_gui_)return;const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return;
    std::vector<GuiPopupItem> items;if(found->second.menu && !snapshot_gui_menu(memory,found->second.menu,items))throw HostCallFault("視窗選單階層超出上限");
    native_gui_->set_menu(handle,items);
}
std::optional<std::uint32_t> WindowsServices::begin_gui_main_menu(GuestState& state,GuestMemory& memory,const GuiMessage& message){
    const auto window=gui_.windows.find(message.window);
    const auto fail=[&]{complete_call(state,memory,0);return std::optional<std::uint32_t>{};};
    if(window==gui_.windows.end() || !gui_window_input_enabled(gui_,message.window) || std::any_of(gui_.menu_tracks.begin(),gui_.menu_tracks.end(),[&](const auto& item){return item.second.active && item.second.thread==threads_.current;}))return fail();
    const auto menu=gui_.menus.find(window->second.menu);if(menu==gui_.menus.end() || message.wparam>=menu->second.size())return fail();
    const auto entry=menu->second[static_cast<std::size_t>(message.wparam)];if(entry.flags&(3U|separator))return fail();
    if(!(entry.flags&popup)){const bool bypos=gui_.menu_info[menu->first].style&0x08000000U;enqueue_gui_message({message.window,bypos?message.wparam:entry.id,bypos?menu->first:0,bypos?0x126U:0x111U});return fail();}
    GuiMenuTrack track;track.menu=entry.id;track.owner=message.window;track.thread=threads_.current;track.root_menu=menu->first;track.root_index=static_cast<std::uint32_t>(message.wparam);track.main_bar=true;
    const auto origin=gui_client_origin(gui_,message.window);
    track.x=static_cast<std::int32_t>(origin[0]+static_cast<std::int32_t>(message.lparam));track.y=static_cast<std::int32_t>(origin[1]);
    const auto token=gui_.next_object++;gui_.menu_tracks.emplace(token,track);
    const auto send=resolve("user32.dll","SendMessageW").value;
    std::vector<GuestCallback> callbacks{{send,{message.window,0x211,0,0}},{send,{message.window,0x116,track.root_menu,0}}};
    try{start_callbacks(state,memory,std::move(callbacks),Continuation::GuiMenuInit,token);}catch(...){gui_.menu_tracks.erase(token);throw;}
    return std::nullopt;
}
std::uint64_t WindowsServices::gui_system_menu(std::uint64_t handle) {
    const auto window=gui_.windows.find(handle);
    if(window==gui_.windows.end()){last_error_=1400;return 0;}
    if(!(window->second.style&0x80000))return 0;
    if(const auto found=gui_.system_menus.find(handle);found!=gui_.system_menus.end())return found->second;
    if(gui_.menus.size()>=menu_limit){last_error_=8;return 0;}
    const auto result=gui_.next_object++;gui_.system_menus[handle]=result;
    const auto style=window->second.style;
    gui_.menus[result]={{0xf120,window->second.show_command==1?1U:0U,u"&Restore"},{0xf010,0,u"&Move"},
        {0xf000,(style&0x40000)?0U:1U,u"&Size"},{0xf020,(style&0x20000)?0U:1U,u"Mi&nimize"},
        {0xf030,(style&0x10000)?0U:1U,u"Ma&ximize"},{0,separator,u""},{0xf060,0,u"&Close"}};
    return result;
}
std::optional<std::uint32_t> WindowsServices::invoke_menu(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(service.family=="internal-menu-loop")return advance_menu_idle(state,memory,state.reg(Gpr::Rax));
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const bool wide=symbol.ends_with('W');
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto read=[&](std::uint64_t address){return address?(wide?read_utf16(memory,GuestAddress{address}):utf8_to_utf16_lossy(read_ansi(memory,address))):std::u16string{};};
    std::uint64_t result=0;
    const auto fail=[&](std::uint32_t error,std::uint64_t value=0){last_error_=error;result=value;};
    if(symbol=="EndMenu"){
        // 遞迴追蹤只結束本執行緒最內層選單；不影響其他執行緒的選單。
        for(auto it=gui_.menu_tracks.rbegin();it!=gui_.menu_tracks.rend();++it)if(it->second.thread==threads_.current && it->second.active){it->second.cancel();result=1;break;}
    }else if(symbol=="GetMenuBarInfo" || symbol=="GetMenuItemRect" || symbol=="MenuItemFromPoint"){
        const bool bar=symbol=="GetMenuBarInfo",hit_test=symbol=="MenuItemFromPoint";
        const auto owner=gui_.windows.find(arg(0));const auto invalid=hit_test?UINT64_MAX:0;
        const auto object=signed32(arg(1));std::uint64_t menu=bar?(owner==gui_.windows.end()?0:object==-1?gui_system_menu(arg(0)):owner->second.menu):arg(1);
        const auto items=gui_.menus.find(menu);const auto index=static_cast<std::int64_t>(signed32(arg(2)))-(bar?1:0);
        const auto size=x86_?32U:48U;const auto output=hit_test?0:arg(3);
        if(owner==gui_.windows.end())fail(1400,invalid);
        else if(bar && object!=-1 && object!=-3 && object!=-4)fail(87);
        else if(bar && (!output || memory.load(GuestAddress{output},32)!=size))fail(87);
        else if(items==gui_.menus.end())fail(1401,invalid);
        else if(!hit_test && (index<(bar?-1:0) || index>=static_cast<std::int64_t>(items->second.size())))fail(1456);
        else if(bar && object==-4)fail(1400); // AppKit popup 沒有對應的客體 #32768 HWND。
        else {
            const auto origin=gui_client_origin(gui_,owner->first);
            const auto rectangle=[&](std::int32_t item)->std::array<std::int32_t,4>{
                std::array<std::int32_t,4> r{};
                if(bar && object==-1){const auto inset=gui_frame_insets(owner->second);r={0,-static_cast<std::int32_t>(inset.top),18,-static_cast<std::int32_t>(inset.top)+18};}
                else if(native_gui_ && owner->second.menu==menu){if(const auto native=native_gui_->menu_rectangle(owner->first,item))r=*native;}
                if(r==std::array<std::int32_t,4>{}){
                    const auto font=GuiFont{};const auto metrics=fallback_font_metrics(font);std::int32_t x=2;
                    if(item<0)r={0,-20,static_cast<std::int32_t>(owner->second.width),0};
                    else for(std::int32_t i=0;i<=item;++i){const auto& entry=items->second[static_cast<std::size_t>(i)];std::u16string label;
                        for(std::size_t n=0;n<entry.text.size() && entry.text[n]!=u'\t';++n){if(entry.text[n]!=u'&')label.push_back(entry.text[n]);else if(n+1<entry.text.size() && entry.text[n+1]==u'&'){label.push_back(u'&');++n;}}
                        std::int64_t width=static_cast<std::int64_t>(label.size())*metrics.average_width;
                        if(native_gui_)if(const auto advances=native_gui_->text_advances(font,label);advances && !advances->empty())width=advances->back();
                        const auto end=static_cast<std::int32_t>(std::min<std::int64_t>(INT32_MAX,static_cast<std::int64_t>(x)+width+16));r={x,-20,end,0};x=end;
                    }
                }
                for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(r[i])+origin[i%2],INT32_MIN,INT32_MAX));return r;
            };
            if(hit_test){result=UINT64_MAX;const auto point=arg(2);const auto x=signed32(point),y=signed32(point>>32);
                for(std::size_t i=0;i<items->second.size();++i){const auto r=rectangle(static_cast<std::int32_t>(i));if(x>=r[0] && x<r[2] && y>=r[1] && y<r[3]){result=i;break;}}
            }else {
                const auto r=rectangle(static_cast<std::int32_t>(index));memory.check(GuestAddress{output},bar?size:16U,Access::Write);
                if(bar){memory.write(GuestAddress{output},std::vector<std::uint8_t>(size));memory.store(GuestAddress{output},32,size);}
                for(unsigned i=0;i<4;++i)memory.store(GuestAddress{output+(bar?4U:0U)+i*4},32,static_cast<std::uint32_t>(r[i]));
                if(bar){memory.store(GuestAddress{output+(x86_?20U:24U)},x86_?32:64,menu);std::uint32_t focus=0;
                    for(const auto& [token,track]:gui_.menu_tracks){(void)token;if(track.active && track.owner==owner->first && (track.root_menu==menu || track.menu==menu)){focus|=1;if(index<0 || (track.main_bar && track.root_index==static_cast<std::uint32_t>(index)))focus|=2;}}
                    memory.store(GuestAddress{output+(x86_?28U:40U)},32,focus);
                }result=1;
            }
        }
    }else if(symbol=="GetMenuCheckMarkDimensions")result=18U|(18U<<16);
    else if(symbol=="CreateMenu" || symbol=="CreatePopupMenu"){
        if(gui_.menus.size()>=menu_limit)fail(8);else {result=gui_.next_object++;gui_.menus[result]={};}
    }else if(symbol=="IsMenu")result=gui_.menus.contains(arg(0));
    else if(symbol=="GetSystemMenu"){
        const auto window=gui_.windows.find(arg(0));
        if(window==gui_.windows.end())fail(1400);
        else if(d(1)){
            const auto found=gui_.system_menus.find(arg(0));if(found!=gui_.system_menus.end())destroy_menu(gui_,found->second);
        }else result=gui_system_menu(arg(0));
    }else if(!gui_.menus.contains(arg(0))){
        const bool minus_one=symbol=="GetMenuDefaultItem" || symbol=="GetMenuState" || symbol=="GetMenuItemCount" || symbol=="GetMenuItemID" || symbol=="CheckMenuItem" || symbol=="EnableMenuItem";
        fail(1401,minus_one?UINT32_MAX:0);
    }else if(symbol=="SetMenuItemBitmaps"){
        const auto where=find_item(gui_,arg(0),d(1),d(2));
        if(d(2)&~by_position)fail(87);
        else if(!where)fail(1456);
        else if((arg(3) && !gui_.bitmaps.contains(arg(3))) || (arg(4) && !gui_.bitmaps.contains(arg(4))))fail(6);
        else {auto& item=gui_.menus.at(where->first)[where->second];item.unchecked_bitmap=arg(3);item.checked_bitmap=arg(4);result=1;}
    }else if(symbol=="DestroyMenu"){destroy_menu(gui_,arg(0));result=1;}
    else if(symbol=="GetMenuContextHelpId")result=gui_.menu_info[arg(0)].help_id;
    else if(symbol=="SetMenuContextHelpId"){gui_.menu_info[arg(0)].help_id=d(1);result=1;}
    else if(symbol=="GetMenuInfo" || symbol=="SetMenuInfo"){
        const bool get=symbol=="GetMenuInfo";const auto p=arg(1);const unsigned size=x86_?28:40,help=x86_?20:24,data=x86_?24:32;const std::uint8_t bits=x86_?32:64;
        if(!p || memory.load(GuestAddress{p},32)!=size)fail(87);
        else {memory.check(GuestAddress{p},size,get?Access::Write:Access::Read);const auto mask=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
            if(mask&~0x8000001fU)fail(87);
            else if(get){const auto& info=gui_.menu_info[arg(0)];if(mask&1)memory.store(GuestAddress{p+12},32,0);if(mask&2)memory.store(GuestAddress{p+16},bits,0);if(mask&4)memory.store(GuestAddress{p+help},32,info.help_id);if(mask&8)memory.store(GuestAddress{p+data},bits,info.data);if(mask&16)memory.store(GuestAddress{p+8},32,info.style);result=1;}
            else {const auto style=static_cast<std::uint32_t>(memory.load(GuestAddress{p+8},32));
                if(((mask&16) && (style&~0x08000000U)) || ((mask&1) && memory.load(GuestAddress{p+12},32)) || ((mask&2) && memory.load(GuestAddress{p+16},bits)))fail(50);
                else {std::vector<std::uint64_t> pending{arg(0)};std::set<std::uint64_t> visited;
                    while(!pending.empty()){const auto h=pending.back();pending.pop_back();if(!visited.insert(h).second || !gui_.menus.contains(h))continue;auto& info=gui_.menu_info[h];
                        if(mask&16)info.style=style;if(mask&4)info.help_id=static_cast<std::uint32_t>(memory.load(GuestAddress{p+help},32));if(mask&8)info.data=memory.load(GuestAddress{p+data},bits);
                        if(mask&0x80000000U)for(const auto& item:gui_.menus.at(h))if(item.flags&popup)pending.push_back(item.id);
                    }result=1;
                }
            }
        }
    }else if(symbol=="SetMenuDefaultItem"){
        auto& items=gui_.menus.at(arg(0));const auto where=d(1)==UINT32_MAX?std::optional<Location>{}:find_item(gui_,arg(0),d(1),d(2)?by_position:0);
        if(d(1)!=UINT32_MAX && !where)fail(1456);
        else {auto& entries=where?gui_.menus.at(where->first):items;for(auto& item:entries)item.flags&=~0x1000U;if(where)entries[where->second].flags|=0x1000;result=1;}
    }else if(symbol=="GetMenuDefaultItem"){
        result=UINT32_MAX;
        if(d(2)&~3U)fail(87,UINT32_MAX);
        else {std::set<std::uint64_t> visited;
            std::function<std::uint32_t(std::uint64_t)> search=[&](std::uint64_t h){if(visited.size()>=depth_limit || !visited.insert(h).second || !gui_.menus.contains(h))return UINT32_MAX;const auto& items=gui_.menus.at(h);
                for(std::size_t i=0;i<items.size();++i){const auto& item=items[i];if(!(item.flags&0x1000) || ((item.flags&3) && !(d(2)&1)))continue;
                    if((d(2)&2) && (item.flags&popup)){const auto nested=search(item.id);if(nested!=UINT32_MAX)return nested;}
                    return d(1)?static_cast<std::uint32_t>(i):(item.flags&popup)?item.command:static_cast<std::uint32_t>(item.id);
                }return UINT32_MAX;};result=search(arg(0));
        }
    }
    else if(symbol=="GetMenuItemCount")result=gui_.menus.at(arg(0)).size();
    else if(symbol=="GetMenuItemID" || symbol=="GetSubMenu"){
        const auto where=find_item(gui_,arg(0),d(1),by_position);
        if(!where)fail(1456,symbol=="GetMenuItemID"?UINT32_MAX:0);
        else {const auto& item=gui_.menus.at(where->first)[where->second];result=symbol=="GetSubMenu"?((item.flags&popup)?item.id:0):((item.flags&popup)?UINT32_MAX:static_cast<std::uint32_t>(item.id));}
    }else if(symbol=="CheckMenuRadioItem"){
        if(d(4)&~by_position || d(1)>d(2))fail(87);
        else {
            bool selected=false;auto& entries=gui_.menus.at(arg(0));
            for(std::size_t index=0;index<entries.size();++index){auto& entry=entries[index];const auto id=(d(4)&by_position)?index:((entry.flags&popup)?entry.command:static_cast<std::uint32_t>(entry.id));
                if(id<d(1) || id>d(2))continue;
                if(id==d(3)){entry.flags|=0x208;selected=true;}else entry.flags&=~0x208U;
            }
            result=selected;
        }
    }else if(symbol.starts_with("GetMenuItemInfo") || symbol.starts_with("SetMenuItemInfo") || symbol.starts_with("InsertMenuItem")){
        const bool get=symbol.starts_with("Get"),insert=symbol.starts_with("Insert");const auto p=arg(3);
        if(!p)fail(87);
        else {
            const auto size=memory.load(GuestAddress{p},32);const unsigned legacy=x86_?44:72,full=x86_?48:80,sub=x86_?20:24,check=x86_?24:32,uncheck=x86_?28:40,data=x86_?32:48,text=x86_?36:56,count=x86_?40:64,bitmap=x86_?44:72;const std::uint8_t bits=x86_?32:64;
            if(size!=legacy && size!=full)fail(87);
            else {
                memory.check(GuestAddress{p},static_cast<std::size_t>(size),get?Access::Write:Access::Read);
                const auto mask=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
                auto where=find_item(gui_,arg(0),d(1),d(2)?by_position:0);
                if(insert && !where)where=Location{arg(0),gui_.menus.at(arg(0)).size()};
                if(mask&~0x1ffU || ((mask&0x10) && (mask&0x1c0)) || ((mask&0x80) && size<full))fail(87);
                else if(!where)fail(1456);
                else if(insert && gui_.menus.at(where->first).size()>=item_limit)fail(8);
                else if(get){
                    const auto& item=gui_.menus.at(where->first)[where->second];
                    if(mask&1)memory.store(GuestAddress{p+12},32,item.flags&0x108b);
                    if(mask&2)memory.store(GuestAddress{p+16},32,(item.flags&popup)?item.command:static_cast<std::uint32_t>(item.id));
                    if(mask&4)memory.store(GuestAddress{p+sub},bits,(item.flags&popup)?item.id:0);
                    if(mask&8){memory.store(GuestAddress{p+check},bits,item.checked_bitmap);memory.store(GuestAddress{p+uncheck},bits,item.unchecked_bitmap);}
                    if(mask&0x20)memory.store(GuestAddress{p+data},bits,item.data);
                    if(mask&0x80)memory.store(GuestAddress{p+bitmap},bits,item.bitmap);
                    if(mask&0x110)memory.store(GuestAddress{p+8},32,item.flags&0x6b64);
                    if(mask&0x50){
                        if((mask&0x10) && (item.flags&(4|0x100|separator))){memory.store(GuestAddress{p+text},bits,(item.flags&4)?item.bitmap:(item.flags&0x100)?item.data:0);memory.store(GuestAddress{p+count},32,0);}
                        else {
                            const auto target=memory.load(GuestAddress{p+text},bits),capacity=memory.load(GuestAddress{p+count},32);
                            const auto narrow=wide?std::string{}:utf16_to_utf8(item.text);const auto length=wide?item.text.size():narrow.size();
                            auto copied=length;
                            if(target){copied=capacity?std::min(length,static_cast<std::size_t>(capacity-1)):0;
                                if(capacity){memory.check(GuestAddress{target},(copied+1)*(wide?2:1),Access::Write);for(std::size_t i=0;i<copied;++i)memory.store(GuestAddress{target+i*(wide?2:1)},wide?16:8,wide?item.text[i]:static_cast<unsigned char>(narrow[i]));memory.store(GuestAddress{target+copied*(wide?2:1)},wide?16:8,0);}
                            }
                            memory.store(GuestAddress{p+count},32,copied);
                        }
                    }
                    result=1;
                }else {
                    auto item=insert?GuiMenuItem{}:gui_.menus.at(where->first)[where->second];
                    auto id=(item.flags&popup)?item.command:static_cast<std::uint32_t>(item.id);
                    auto submenu=(item.flags&popup)?item.id:0;
                    if(mask&1)item.flags=(item.flags&~0x108bU)|(static_cast<std::uint32_t>(memory.load(GuestAddress{p+12},32))&0x108b);
                    if(mask&2)id=static_cast<std::uint32_t>(memory.load(GuestAddress{p+16},32));
                    if(mask&4)submenu=memory.load(GuestAddress{p+sub},bits);
                    if(mask&8){item.checked_bitmap=memory.load(GuestAddress{p+check},bits);item.unchecked_bitmap=memory.load(GuestAddress{p+uncheck},bits);}
                    if(mask&0x20)item.data=memory.load(GuestAddress{p+data},bits);
                    if(mask&0x80)item.bitmap=memory.load(GuestAddress{p+bitmap},bits);
                    if(mask&0x110)item.flags=(item.flags&~0x6b64U)|(static_cast<std::uint32_t>(memory.load(GuestAddress{p+8},32))&0x6b64);
                    if((mask&8) && ((item.checked_bitmap && !gui_.bitmaps.contains(item.checked_bitmap)) || (item.unchecked_bitmap && !gui_.bitmaps.contains(item.unchecked_bitmap))))fail(6);
                    else if(submenu && !gui_.menus.contains(submenu))fail(1401);
                    else if(submenu && contains_menu(gui_,submenu,where->first))fail(87);
                    else if(item.flags&(4|0x100))fail(50); // owner-draw／bitmap-only 需 WM_DRAWITEM 契約。
                    else {
                        if(mask&0x50)item.text=(item.flags&separator)?std::u16string{}:read(memory.load(GuestAddress{p+text},bits));
                        item.command=id;item.id=submenu?submenu:id;item.flags=(item.flags&~popup)|(submenu?popup:0);
                        auto& entries=gui_.menus.at(where->first);
                        if(item.flags&0x1000U)for(auto& other:entries)other.flags&=~0x1000U;
                        if(insert)entries.insert(entries.begin()+static_cast<std::ptrdiff_t>(where->second),std::move(item));else entries[where->second]=std::move(item);
                        result=1;
                    }
                }
            }
        }
    }else if(symbol.starts_with("AppendMenu") || symbol.starts_with("InsertMenu") || symbol.starts_with("ModifyMenu")){
        const bool append=symbol.starts_with("Append"),modify=symbol.starts_with("Modify");
        const auto flags=d(append?1:2);const auto id=arg(append?2:3),text=arg(append?3:4);
        auto where=append?std::optional{Location{arg(0),gui_.menus.at(arg(0)).size()}}:find_item(gui_,arg(0),d(1),flags);
        if(!append && !modify && (d(1)==UINT32_MAX || ((flags&by_position) && d(1)==gui_.menus.at(arg(0)).size())))where=Location{arg(0),gui_.menus.at(arg(0)).size()};
        if(!where)fail(1456);
        else if(flags&(0x4U|0x100U|0x20U|0x40U))fail(50); // bitmap、owner-draw、分欄需額外 native 契約。
        else if((flags&popup) && (!(gui_.menus.contains(id)) || (flags&separator)))fail(1401);
        else if((flags&popup) && contains_menu(gui_,id,where->first))fail(87);
        else if(!modify && gui_.menus.at(where->first).size()>=item_limit)fail(8);
        else {
            auto& entries=gui_.menus.at(where->first);
            // ModifyMenu 的文字／狀態修改不包含 MIIM_SUBMENU；只有 MF_POPUP 才換子選單。
            // command 與 submenu handle 分開，新的命令 ID 不可覆蓋既有的子選單 handle。
            auto item=modify?entries[where->second]:GuiMenuItem{};
            const auto previous_submenu=(item.flags&popup)?item.id:0;
            const auto submenu=(flags&popup)?id:modify?previous_submenu:0;
            item.id=submenu?submenu:id;if(modify)item.command=static_cast<std::uint32_t>(id);
            item.flags=(flags&~(by_position|popup|(modify?0x1000U:0U)))|(submenu?popup:0);
            item.text=(flags&separator)?std::u16string{}:read(text);item.bitmap=0;
            if(modify){entries[where->second]=std::move(item);if(previous_submenu && (flags&popup) && previous_submenu!=submenu)destroy_menu(gui_,previous_submenu);}
            else entries.insert(entries.begin()+static_cast<std::ptrdiff_t>(where->second),std::move(item));
            result=1;
        }
    }else if(symbol=="TrackPopupMenu" || symbol=="TrackPopupMenuEx"){
        const bool ex=symbol.ends_with("Ex");const auto owner=arg(ex?4:5);const auto flags=d(1);
        if(!gui_.windows.contains(owner))fail(1400);
        else if(!gui_.menus.contains(arg(0)))fail(1401);
        else if((!ex && d(4)) || (flags&~0x1ffffU) || ((flags&12)==12) || ((flags&48)==48))fail(87);
        else if(!(flags&1) && std::any_of(gui_.menu_tracks.begin(),gui_.menu_tracks.end(),[&](const auto& item){return item.second.active && item.second.thread==threads_.current;}))fail(1446);
        else if(gui_.menu_tracks.size()>=64)fail(8);
        else {
            // TPMPARAMS 的排除區域不可靜默忽略；純零 RECT 等同沒有排除區域。
            if(ex && arg(5)){
                if(memory.load(GuestAddress{arg(5)},32)!=20){fail(87);arg.return_to(state,result);return std::nullopt;}
                memory.check(GuestAddress{arg(5)},20,Access::Read);
                for(unsigned offset=4;offset<20;offset+=4)if(memory.load(GuestAddress{arg(5)+offset},32)){fail(50);arg.return_to(state,result);return std::nullopt;}
            }
            GuiMenuTrack track;track.menu=arg(0);track.owner=owner;track.thread=threads_.current;track.flags=flags;track.x=signed32(arg(2));track.y=signed32(arg(3));
            const auto token=gui_.next_object++;gui_.menu_tracks[token]=track;
            const auto send=resolve("user32.dll",gui_.windows.at(owner).unicode?"SendMessageW":"SendMessageA").value;
            std::vector<GuestCallback> callbacks;
            if(!(flags&0x80)){callbacks.push_back({send,{owner,0x211,1,0}});callbacks.push_back({send,{owner,0x116,track.menu,0}});}
            start_callbacks(state,memory,std::move(callbacks),Continuation::GuiMenuInit,token);return std::nullopt;
        }
    }else {
        const auto flags=d(symbol.starts_with("GetMenuString")?4:2);
        const auto where=find_item(gui_,arg(0),d(1),flags);
        const bool minus_one=symbol=="GetMenuState" || symbol=="CheckMenuItem" || symbol=="EnableMenuItem";
        if(!where)fail(1456,minus_one?UINT32_MAX:0);
        else {
            auto& entries=gui_.menus.at(where->first);auto& item=entries[where->second];
            if(symbol=="DeleteMenu" || symbol=="RemoveMenu"){
                const auto previous=item;entries.erase(entries.begin()+static_cast<std::ptrdiff_t>(where->second));
                if(symbol=="DeleteMenu" && (previous.flags&popup))destroy_menu(gui_,previous.id);result=1;
            }else if(symbol=="CheckMenuItem" || symbol=="EnableMenuItem"){
                const auto mask=symbol=="CheckMenuItem"?8U:3U;result=item.flags&mask;item.flags=(item.flags&~mask)|(flags&mask);
            }else if(symbol=="GetMenuState"){
                result=item.flags;
                if(item.flags&popup){const auto nested=gui_.menus.find(item.id);result=(item.flags&0xff)|((nested==gui_.menus.end()?0:nested->second.size()&0xff)<<8);}
            }else if(symbol.starts_with("GetMenuString")){
                const auto capacity=signed32(arg(3));
                const auto text=wide?std::string{}:utf16_to_utf8(item.text);
                const auto length=wide?item.text.size():text.size();
                if(!arg(2) || !capacity)result=length;
                else if(capacity<0)fail(87);
                else {
                    result=std::min(length,static_cast<std::size_t>(capacity-1));
                    memory.check(GuestAddress{arg(2)},(result+1)*(wide?2:1),Access::Write);
                    for(std::uint64_t i=0;i<result;++i)memory.store(GuestAddress{arg(2)+i*(wide?2:1)},wide?16:8,wide?item.text[static_cast<std::size_t>(i)]:static_cast<unsigned char>(text[static_cast<std::size_t>(i)]));
                    memory.store(GuestAddress{arg(2)+result*(wide?2:1)},wide?16:8,0);
                }
            }else throw HostCallFault("未實作的 menu service");
        }
    }
    if(!symbol.starts_with("Get") && symbol!="IsMenu")for(const auto& [handle,window]:gui_.windows)if(window.menu)present_gui_menu(memory,handle);
    arg.return_to(state,result);return std::nullopt;
}

std::optional<std::uint32_t> WindowsServices::resume_menu(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t value) {
    if(frame.hook.type==-1){
        GuiMessage message;
        try{if(!value)message=gui_read_message(memory,frame.hook.value);}
        catch(...){release_gui_scratch(memory,state,frame.hook.scratch);throw;}
        release_gui_scratch(memory,state,frame.hook.scratch);
        const auto current=gui_.menu_tracks.find(frame.value);
        if(!value && current!=gui_.menu_tracks.end() && !current->second.cancelled && gui_.windows.contains(current->second.owner) && gui_.menus.contains(current->second.menu)){
            dispatch_menu_message(state,memory,message);
        }
        // saved RIP 是 internal-menu-loop；保留外層 TrackPopupMenu 的原始呼叫框架。
        return std::nullopt;
    }
    const auto found=gui_.menu_tracks.find(frame.value);
    if(found==gui_.menu_tracks.end()){last_error_=1401;complete_call(state,memory,0);return std::nullopt;}
    auto& track=found->second;
    const auto owner=gui_.windows.find(track.owner);
    if(owner==gui_.windows.end() || !gui_.menus.contains(track.menu)){
        last_error_=owner==gui_.windows.end()?1400:1401;gui_.menu_tracks.erase(found);complete_call(state,memory,0);return std::nullopt;
    }
    const auto send=resolve("user32.dll",owner->second.unicode?"SendMessageW":"SendMessageA").value;
    if(frame.continuation==Continuation::GuiMenuFinish){
        const auto result=(track.flags&0x100)?(track.selected?track.selection:0):1;
        if(track.selected && !(track.flags&0x180)){
            const bool system=system_menu(gui_,track.owner,track.menu);
            const auto coords=static_cast<std::uint16_t>(track.x)|(static_cast<std::uint32_t>(static_cast<std::uint16_t>(track.y))<<16);
            const auto root=track.main_bar?track.root_menu:track.menu;const bool bypos=!system && (gui_.menu_info[root].style&0x08000000U);
            enqueue_gui_message({track.owner,bypos?track.selected_position:system?track.selection:track.selection&0xffff,bypos?track.selected_menu:system?coords:0,bypos?0x126U:system?0x112U:0x111U});
        }
        gui_.menu_tracks.erase(found);complete_call(state,memory,result);return std::nullopt;
    }
    if(!track.cancelled && !(track.flags&0x80)){
        if(const auto next=next_popup(gui_,track)){
            if(track.initialized.size()>=menu_limit){last_error_=8;gui_.menu_tracks.erase(found);complete_call(state,memory,0);return std::nullopt;}
            track.initialized.push_back(next->first);
            const auto lparam=next->second|(system_menu(gui_,track.owner,track.menu)?0x10000U:0U);
            start_callbacks(state,memory,{{send,{track.owner,0x117,next->first,lparam}}},Continuation::GuiMenuInit,frame.value);return std::nullopt;
        }
    }
    std::vector<GuiPopupItem> items;
    if(!snapshot_gui_menu(memory,track.menu,items)){last_error_=8;gui_.menu_tracks.erase(found);complete_call(state,memory,0);return std::nullopt;}
    if(!track.cancelled && native_gui_){
        const auto saved=state;const auto owner_thread=track.thread;const auto depth=runtime_.callbacks.size();
        std::uint64_t idle_cycles=0,snapshot_refreshes=0;
        const auto idle_entry=resolve("internal-menu-loop","ReadMessage");
        const auto idle=[&]{
            ++idle_cycles;
            const auto current=gui_.menu_tracks.find(frame.value);
            if(current==gui_.menu_tracks.end() || current->second.cancelled || !gui_.windows.contains(current->second.owner) || !gui_.menus.contains(current->second.menu))return false;
            if(!has_nested_runner())return true;
            const auto instructions=nested_instruction_count();
            const bool sent=pending_gui_send(threads_.current);
            state.rip=idle_entry;state.reg(Gpr::Rax)=frame.value;
            advance_menu_idle(state,memory,frame.value);
            run_nested(state,memory,[&]{return threads_.current==owner_thread && state.rip==idle_entry && runtime_.callbacks.size()==depth;});
            const auto tsc=state.tsc;state=saved;state.tsc=tsc;
            const auto after=gui_.menu_tracks.find(frame.value);
            if(after==gui_.menu_tracks.end() || after->second.cancelled || !gui_.windows.contains(after->second.owner) || !gui_.menus.contains(after->second.menu))return false;
            // 沒有客體回呼／執行緒前進時，選單模型仍是已呈現的版本。
            if(!sent && nested_instruction_count()==instructions)return true;
            // 客體回呼／其他執行緒可修改正在追蹤的選單；下次原生輸入前同步模型。
            ++snapshot_refreshes;
            std::vector<GuiPopupItem> updated;
            if(!snapshot_gui_menu(memory,after->second.menu,updated)){after->second.cancel();last_error_=8;return false;}
            items=std::move(updated);return true;
        };
        const auto notify_popup=[&](std::uint64_t menu,std::uint32_t position,bool opening){
            auto current=gui_.menu_tracks.find(frame.value);
            if(current==gui_.menu_tracks.end() || (opening && current->second.cancelled) || !gui_.windows.contains(current->second.owner) || !gui_.menus.contains(current->second.menu))return false;
            if(current->second.flags&0x80)return true;
            auto& opened=current->second.initialized;
            const auto previous=std::find(opened.begin(),opened.end(),menu);
            if(opening){
                if(!gui_.menus.contains(menu) || !contains_menu(gui_,current->second.menu,menu))return false;
                if(previous!=opened.end())return true;
                if(opened.size()>=menu_limit){last_error_=8;return false;}
                opened.push_back(menu);
            }else {
                if(previous==opened.end())return true;
                opened.erase(previous);
            }
            if(!has_nested_runner())return false;
            const auto callback_state=state;const auto callback_depth=runtime_.callbacks.size();const auto callback_thread=threads_.current;
            state.rip=idle_entry;state.reg(Gpr::Rax)=frame.value;
            const auto window=gui_.windows.find(current->second.owner);const auto api=resolve("user32.dll",window->second.unicode?"SendMessageW":"SendMessageA").value;
            const auto system=system_menu(gui_,current->second.owner,current->second.menu);
            start_callbacks(state,memory,{{api,{current->second.owner,opening?0x117U:0x125U,menu,opening?(position&0xffffU)|(system?0x10000U:0U):system?0x20000000U:0U}}},Continuation::GuiWinEvent);
            run_nested(state,memory,[&]{return threads_.current==callback_thread && state.rip==idle_entry && runtime_.callbacks.size()==callback_depth;});
            const auto tsc=state.tsc;state=callback_state;state.tsc=tsc;
            current=gui_.menu_tracks.find(frame.value);
            if(current==gui_.menu_tracks.end() || current->second.cancelled || !gui_.windows.contains(current->second.owner) || !gui_.menus.contains(current->second.menu))return false;
            if(opening && (!gui_.menus.contains(menu) || !contains_menu(gui_,current->second.menu,menu)))return false;
            std::vector<GuiPopupItem> updated;
            if(!snapshot_gui_menu(memory,current->second.menu,updated)){current->second.cancel();last_error_=8;return false;}
            items=std::move(updated);return true;
        };
        const auto notify_highlight=[&](const GuiMenuHighlight& selected){
            auto current=gui_.menu_tracks.find(frame.value);
            if(current==gui_.menu_tracks.end() || current->second.cancelled || !gui_.windows.contains(current->second.owner) || !gui_.menus.contains(current->second.menu))return false;
            const auto containing=gui_.menus.find(selected.menu);
            if(containing==gui_.menus.end() || selected.position>=containing->second.size() || !contains_menu(gui_,current->second.menu,selected.menu))return false;
            if(current->second.flags&0x80)return true;
            if(!has_nested_runner())return false;
            const auto entry=containing->second[selected.position];
            const auto identifier=(entry.flags&popup)?selected.position:static_cast<std::uint32_t>(entry.id);
            const auto system=system_menu(gui_,current->second.owner,current->second.menu);
            const auto flags=(entry.flags&(3U|4U|8U|popup|0x100U))|0x80U|(selected.mouse?0x8000U:0U)|(system?0x2000U:0U);
            const auto target_window=gui_.windows.find(current->second.owner);const auto api=resolve("user32.dll",target_window->second.unicode?"SendMessageW":"SendMessageA").value;
            const auto callback_state=state;const auto callback_depth=runtime_.callbacks.size();const auto callback_thread=threads_.current;
            state.rip=idle_entry;state.reg(Gpr::Rax)=frame.value;
            start_callbacks(state,memory,{{api,{current->second.owner,0x11f,(identifier&0xffffU)|(flags<<16),selected.menu}}},Continuation::GuiWinEvent);
            run_nested(state,memory,[&]{return threads_.current==callback_thread && state.rip==idle_entry && runtime_.callbacks.size()==callback_depth;});
            const auto tsc=state.tsc;state=callback_state;state.tsc=tsc;
            current=gui_.menu_tracks.find(frame.value);
            if(current==gui_.menu_tracks.end() || current->second.cancelled || !gui_.windows.contains(current->second.owner) || !gui_.menus.contains(current->second.menu))return false;
            std::vector<GuiPopupItem> updated;
            if(!snapshot_gui_menu(memory,current->second.menu,updated)){current->second.cancel();last_error_=8;return false;}
            items=std::move(updated);
            const auto after=gui_.menus.find(selected.menu);
            return after!=gui_.menus.end() && selected.position<after->second.size() && after->second[selected.position].id==entry.id;
        };
        const auto selection=native_gui_->popup_menu(track.owner,items,track.x,track.y,track.flags,idle,notify_popup,notify_highlight);
        if(trace_services_.find("MenuIdle")!=std::string::npos)std::fprintf(stderr,"[menu-idle] token=%llu cycles=%llu snapshots=%llu\n",static_cast<unsigned long long>(frame.value),static_cast<unsigned long long>(idle_cycles),static_cast<unsigned long long>(snapshot_refreshes));
        // 回呼可能 DestroyWindow／DestroyMenu；不得使用已失效的 owner iterator。
        const auto current=gui_.menu_tracks.find(frame.value);
        if(current==gui_.menu_tracks.end() || !gui_.windows.contains(current->second.owner) || !gui_.menus.contains(current->second.menu)){
            gui_.menu_tracks.erase(frame.value);complete_call(state,memory,0);return std::nullopt;
        }
        std::vector<GuiPopupItem> latest;
        if(!current->second.cancelled && selection && snapshot_gui_menu(memory,current->second.menu,latest) && selectable(latest,*selection)){
            current->second.selection=selection->command;current->second.selected_menu=selection->menu;current->second.selected_position=selection->position;current->second.selected=true;
        }
    }
    std::vector<GuestCallback> callbacks;
    track.active=false;
    if(!(track.flags&0x80)){
        for(auto it=track.initialized.rbegin();it!=track.initialized.rend();++it)callbacks.push_back({send,{track.owner,0x125,*it,system_menu(gui_,track.owner,track.menu)?0x20000000U:0}});
        callbacks.push_back({send,{track.owner,0x11f,0xffff0000U,0}});
        callbacks.push_back({send,{track.owner,0x212,track.main_bar?0U:1U,0}});
    }
    start_callbacks(state,memory,std::move(callbacks),Continuation::GuiMenuFinish,frame.value);return std::nullopt;
}

std::optional<std::uint32_t> WindowsServices::advance_menu_idle(GuestState& state,GuestMemory& memory,std::uint64_t token) {
    const auto found=gui_.menu_tracks.find(token);
    if(found==gui_.menu_tracks.end() || found->second.cancelled)return std::nullopt;
    auto& track=found->second;
    if(!gui_.windows.contains(track.owner) || !gui_.menus.contains(track.menu)){track.cancel();return std::nullopt;}
    // AppKit 正在處理宿主輸入；只產生 Windows 計時器／繪圖／I/O，不能重入原生 poll。
    retired(memory,0);pump_gui(memory,false,gui_native_poll_deferred);
    if(dispatch_gui_send(state,memory)){track.idle_notified=false;return std::nullopt;}
    auto message=gui_.queue.end();
    for(auto it=gui_.queue.begin();it!=gui_.queue.end();++it)if(gui_message_matches(*it,threads_.current) &&
        (message==gui_.queue.end() || it->queue_priority<message->queue_priority))message=it;
    if(message!=gui_.queue.end()){
        if(message->message==18){track.cancel();return std::nullopt;} // WM_QUIT 留給外層訊息迴圈。
        const auto event=*message;consume_gui_input(event);consume_shell_message(event);remember_gui_message(event);gui_.queue.erase(message);track.idle_notified=false;
        if(std::any_of(gui_.hooks.begin(),gui_.hooks.end(),[&](const auto& entry){const auto& hook=entry.second;return (hook.type==-1 || hook.type==6) && (!hook.thread || hook.thread==threads_.current);})){
            std::vector<std::uint64_t> scratch;
            try{
                const auto buffer=allocate_gui_scratch(memory,scratch,{},x86_?28:48);gui_write_message(memory,buffer,event);
                const auto wide=gui_.windows.at(track.owner).unicode;
                start_callbacks(state,memory,{{resolve("user32.dll",wide?"CallMsgFilterW":"CallMsgFilterA").value,{buffer,2}}},Continuation::GuiMenuInit,token);
                auto& frame=runtime_.callbacks.back();frame.hook.type=-1;frame.hook.value=buffer;frame.hook.scratch=std::move(scratch);
            }catch(...){release_gui_scratch(memory,state,scratch);throw;}
            return std::nullopt;
        }
        dispatch_menu_message(state,memory,event);
        return std::nullopt;
    }
    if(!track.idle_notified && !(track.flags&0x80)){
        track.idle_notified=true;const auto& window=gui_.windows.at(track.owner);
        start_callbacks(state,memory,{{resolve("user32.dll",window.unicode?"SendMessageW":"SendMessageA").value,{track.owner,0x121,2,track.owner}}},Continuation::GuiWinEvent);
        return std::nullopt;
    }
    // 等待原生輸入時讓其他客體執行緒前進；其同步 SendMessage 仍由原有排程續接。
    schedule(state,memory);
    return std::nullopt;
}
void WindowsServices::dispatch_menu_message(GuestState& state,GuestMemory& memory,const GuiMessage& event){
    if(event.message==18){
        auto quit=event;quit.window=0;quit.thread=threads_.current;enqueue_gui_message(std::move(quit));
        for(auto it=gui_.menu_tracks.rbegin();it!=gui_.menu_tracks.rend();++it)if(it->second.active && it->second.thread==threads_.current){it->second.cancel();break;}
        return;
    }
    if(event.message==0x113 && event.lparam){
        GuestCallback callback{event.lparam,{event.window,0x113,event.wparam,static_cast<std::uint32_t>(runtime_.clock_ticks/10000)}};
        callback.user_mode_callback=true;start_callbacks(state,memory,{std::move(callback)},Continuation::GuiWinEvent);return;
    }
    const auto window=gui_.windows.find(event.window);
    if(window!=gui_.windows.end())start_callbacks(state,memory,{{resolve("user32.dll",window->second.unicode?"SendMessageW":"SendMessageA").value,{event.window,event.message,event.wparam,event.lparam}}},Continuation::GuiWinEvent);
}
}
