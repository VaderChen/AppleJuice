#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace x64arm {
namespace {
std::uint64_t word(std::int32_t value) { return static_cast<std::uint32_t>(value); }
std::int32_t signed_word(std::uint64_t value) { return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value)); }
bool mdi_member(const GuiState& gui,std::uint64_t client,std::uint64_t child,bool ready=true) {
    const auto found=gui.windows.find(child);
    return found!=gui.windows.end() && found->second.mdi_child && found->second.parent==client &&
        !found->second.destroying && (!ready || found->second.mdi_ready);
}
void put32(std::span<std::uint8_t> bytes,unsigned offset,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(8*i));
}
GuiMessage mdi_message(const GuestMemory& memory,std::uint64_t address,bool x86) {
    memory.check(GuestAddress{address},x86?28:48,Access::Read);
    GuiMessage message{memory.load(GuestAddress{address},x86?32:64),
        memory.load(GuestAddress{address+(x86?8U:16U)},x86?32:64),
        memory.load(GuestAddress{address+(x86?12U:24U)},x86?32:64),
        static_cast<std::uint32_t>(memory.load(GuestAddress{address+(x86?4U:8U)},32))};
    return message;
}
std::u16string menu_title(std::size_t index,std::u16string_view title) {
    auto text=utf8_to_utf16_lossy(std::to_string(index+1));
    if(index<9)text.insert(text.begin(),u'&');
    text.push_back(u' ');
    for(const auto c:title){text.push_back(c);if(c==u'&')text.push_back(c);}
    return text;
}
}

std::uint32_t WindowsServices::prepare_mdi_window(GuestMemory& memory,GuiWindow& window,
        std::uint64_t parameter,std::array<std::uint32_t,4> dimensions) {
    if(window.control_class==u"mdiclient") {
        if(!window.parent || !gui_.windows.contains(window.parent) || !parameter)return 87;
        const unsigned unit=x86_?4:8;
        memory.check(GuestAddress{parameter},x86_?8:16,Access::Read);
        GuiMdiClient client;
        client.window_menu=memory.load(GuestAddress{parameter},x86_?32:64);
        client.first_child=static_cast<std::uint32_t>(memory.load(GuestAddress{parameter+unit},32));
        if(client.window_menu && !gui_.menus.contains(client.window_menu))return 1401;
        client.frame_title=gui_.windows.at(window.parent).title;
        window.mdi=std::move(client);return 0;
    }
    if(!(window.ex_style&0x40U))return 0;
    const auto parent=gui_.windows.find(window.parent);
    if(parent==gui_.windows.end() || !parent->second.mdi)return 1400;
    const auto& client=parent->second;
    window.mdi_child=true;
    if(client.style&1U)window.style=(window.style&~0x80000000U)|0x40000000U;
    else window.style=(window.style&0x21300000U)|0x56cf0000U;
    window.enabled=(window.style&0x08000000U)==0;
    window.show_command=window.style&0x20000000U?2U:window.style&0x01000000U?3U:1U;
    gui_refresh_frame(window);
    const auto step=static_cast<std::uint32_t>(client.mdi->children.size()%10)*26;
    window.x=dimensions[0]==0x80000000U?static_cast<std::int32_t>(step):signed_word(dimensions[0]);
    window.y=dimensions[1]==0x80000000U?static_cast<std::int32_t>(step):signed_word(dimensions[1]);
    const auto width=dimensions[2]==0x80000000U?std::max(1U,client.width>step?client.width-step:client.width):dimensions[2];
    const auto height=dimensions[3]==0x80000000U?std::max(1U,client.height>step?client.height-step:client.height):dimensions[3];
    if(width>8192 || height>8192)return 87;
    const auto size=gui_client_size(width,height,gui_frame_insets(window));window.width=size[0];window.height=size[1];
    if(window.show_command==2 || window.show_command==3) {
        window.normal_rect=std::array{window.x,window.y,static_cast<std::int32_t>(window.width),static_cast<std::int32_t>(window.height)};
        const auto inset=gui_frame_insets(window);
        if(window.show_command==3){window.x=-static_cast<std::int32_t>(inset.left);window.y=-static_cast<std::int32_t>(inset.top);window.width=client.width;window.height=client.height;}
        else {window.x=0;window.y=static_cast<std::int32_t>(client.height)-32;const auto icon=gui_client_size(160,32,inset);window.width=icon[0];window.height=icon[1];}
    }
    return 0;
}
void WindowsServices::register_mdi_child(std::uint64_t handle) {
    auto& child=gui_.windows.at(handle);
    if(!child.mdi_child)return;
    const auto parent=gui_.windows.find(child.parent);
    if(parent==gui_.windows.end() || !parent->second.mdi)return;
    auto& state=*parent->second.mdi;
    child.id=state.first_child+static_cast<std::uint32_t>(state.children.size());
    state.children.push_back(handle);
}
std::uint64_t WindowsServices::next_mdi_child(std::uint64_t client,std::uint64_t current,bool previous) const {
    std::vector<std::uint64_t> order;
    for(const auto handle:gui_.z_order)if(mdi_member(gui_,client,handle)) {
        const auto& w=gui_.windows.at(handle);
        if(w.enabled && w.visible)order.push_back(handle);
    }
    if(order.empty())return 0;
    const auto found=std::find(order.begin(),order.end(),current);
    if(found==order.end())return previous?order.back():order.front();
    const auto index=static_cast<std::size_t>(found-order.begin());
    return order[(index+order.size()+(previous?order.size()-1:1))%order.size()];
}
void WindowsServices::refresh_mdi(std::uint64_t client,GuestMemory* memory) {
    const auto found=gui_.windows.find(client);
    if(found==gui_.windows.end() || !found->second.mdi)return;
    auto& window=found->second;auto& mdi=*window.mdi;
    std::erase_if(mdi.children,[&](auto child){return !mdi_member(gui_,client,child,false);});
    for(std::size_t i=0;i<mdi.children.size();++i)gui_.windows.at(mdi.children[i]).id=mdi.first_child+static_cast<std::uint32_t>(i);
    if(!mdi_member(gui_,client,mdi.active))mdi.active=0;
    if(!mdi.active)mdi.maximized=false;
    // 只移除本 client 加入的項目，保留程式自己的選單與其他 client。
    if(auto menu=gui_.menus.find(mdi.window_menu);menu!=gui_.menus.end()) {
        std::erase_if(menu->second,[&](const auto& item){return item.mdi_owner==client;});
        std::vector<GuiMenuItem> managed;
        for(std::size_t i=0;i<mdi.children.size();++i) {
            const auto child=mdi.children[i];const auto& w=gui_.windows.at(child);
            if(!w.mdi_ready)continue;
            GuiMenuItem item;item.id=w.id;item.flags=child==mdi.active?8U:0U;item.text=menu_title(i,w.title);item.mdi_owner=client;
            managed.push_back(std::move(item));
        }
        if(menu->second.size()+managed.size()+1>65536)last_error_=8;
        else {
            if(!managed.empty() && !menu->second.empty()){GuiMenuItem separator;separator.flags=0x800;separator.mdi_owner=client;menu->second.push_back(std::move(separator));}
            menu->second.insert(menu->second.end(),std::make_move_iterator(managed.begin()),std::make_move_iterator(managed.end()));
        }
    }
    std::array<std::int64_t,4> bounds{0,0,window.width,window.height};
    for(const auto child:mdi.children)if(mdi_member(gui_,client,child)) {
        const auto& w=gui_.windows.at(child);if(!w.visible || w.show_command==2)continue;
        const auto size=gui_outer_size(w);
        const auto x=static_cast<std::int64_t>(w.x)+mdi.scroll[0],y=static_cast<std::int64_t>(w.y)+mdi.scroll[1];
        bounds[0]=std::min(bounds[0],x);bounds[1]=std::min(bounds[1],y);
        bounds[2]=std::max(bounds[2],x+size[0]);bounds[3]=std::max(bounds[3],y+size[1]);
    }
    for(unsigned axis=0;axis<2;++axis)if(window.style&(axis?0x200000U:0x100000U)) {
        auto& info=gui_.scroll_info[{client,axis}];put32(info,0,28);put32(info,4,0x17);
        const auto minimum=mdi.maximized?0:signed_word(static_cast<std::uint32_t>(std::clamp<std::int64_t>(bounds[axis],INT32_MIN,INT32_MAX)));
        const auto maximum=mdi.maximized?0:signed_word(static_cast<std::uint32_t>(std::clamp<std::int64_t>(bounds[axis+2]-1,INT32_MIN,INT32_MAX)));
        put32(info,8,static_cast<std::uint32_t>(minimum));put32(info,12,static_cast<std::uint32_t>(maximum));
        put32(info,16,axis?window.height:window.width);put32(info,20,static_cast<std::uint32_t>(mdi.scroll[axis]));put32(info,24,static_cast<std::uint32_t>(mdi.scroll[axis]));
    }
    if(const auto frame=gui_.windows.find(window.parent);frame!=gui_.windows.end()) {
        frame->second.title=mdi.frame_title;
        if(mdi.maximized && mdi.active)frame->second.title+=u" - ["+gui_.windows.at(mdi.active).title+u"]";
        if(auto root=gui_.menus.find(frame->second.menu);root!=gui_.menus.end() && frame->second.menu!=mdi.window_menu) {
            std::erase_if(root->second,[&](const auto& item){return item.mdi_owner==client;});
            if(mdi.maximized && mdi.active && root->second.size()+3<=65536) {
                if(const auto system=gui_system_menu(mdi.active)) {
                    GuiMenuItem icon;icon.id=system;icon.flags=0x10;icon.text=u"▣";icon.mdi_owner=client;root->second.insert(root->second.begin(),std::move(icon));
                    GuiMenuItem restore;restore.id=0xf120;restore.text=u"❐";restore.mdi_owner=client;root->second.push_back(std::move(restore));
                    GuiMenuItem close;close.id=0xf060;close.text=u"×";close.mdi_owner=client;root->second.push_back(std::move(close));
                }
            }
        }
        if(mdi.active)if(const auto system=gui_.system_menus.find(mdi.active);system!=gui_.system_menus.end()) {
            const auto& active=gui_.windows.at(mdi.active);
            for(auto& item:gui_.menus.at(system->second)) {
                if(item.id==0xf120)item.flags=(item.flags&~3U)|(active.show_command==1?1U:0U);
                else if(item.id==0xf030)item.flags=(item.flags&~3U)|(!(active.style&0x10000) || active.show_command==3?1U:0U);
                else if(item.id==0xf020)item.flags=(item.flags&~3U)|(!(active.style&0x20000) || active.show_command==2?1U:0U);
            }
        }
        present_gui(window.parent);
        if(memory && frame->second.menu)present_gui_menu(*memory,window.parent);
    }
}
std::vector<GuestCallback> WindowsServices::activate_mdi(std::uint64_t client,std::uint64_t child,GuestMemory& memory) {
    auto& mdi=*gui_.windows.at(client).mdi;std::vector<GuestCallback> callbacks;
    if(child && !mdi_member(gui_,client,child))return callbacks;
    if(child && (!gui_.windows.at(child).enabled || !gui_.windows.at(child).visible))return callbacks;
    const auto old=mdi.active;if(old==child)return callbacks;
    mdi.active=child;
    const auto send=[&](std::uint64_t handle,std::uint32_t message,std::uint64_t wparam,std::uint64_t lparam) {
        const auto target=gui_.windows.find(handle);if(target==gui_.windows.end())return;
        callbacks.push_back({resolve("user32.dll",target->second.unicode?"SendMessageW":"SendMessageA").value,{handle,message,wparam,lparam}});
    };
    if(old && gui_.windows.contains(old)) {
        gui_.windows.at(old).mdi_active=false;send(old,0x86,0,child);send(old,0x222,old,child);
        if(mdi.maximized && gui_.windows.at(old).show_command==3)callbacks.push_back({resolve("user32.dll","ShowWindow").value,{old,9}});
    }
    if(child) {
        gui_.windows.at(child).mdi_active=true;
        callbacks.push_back({resolve("user32.dll","SetWindowPos").value,{child,0,0,0,0,0,0x13}});
        if(mdi.maximized || gui_.windows.at(child).show_command==2)callbacks.push_back({resolve("user32.dll","ShowWindow").value,{child,mdi.maximized?3U:9U}});
        send(child,0x86,1,old);send(child,0x222,old,child);
        callbacks.push_back({resolve("user32.dll","SetFocus").value,{child}});
    } else mdi.maximized=false;
    refresh_mdi(client,&memory);return callbacks;
}
std::vector<GuestCallback> WindowsServices::finish_mdi_creation(std::uint64_t handle,GuestMemory& memory) {
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return {};
    if(found->second.mdi){refresh_mdi(handle,&memory);return {};}
    if(!found->second.mdi_child)return {};
    found->second.mdi_ready=true;const auto client=found->second.parent;
    if(found->second.show_command==2){refresh_mdi(client,&memory);return {};}
    auto callbacks=activate_mdi(client,handle,memory);
    if(found->second.show_command==3)gui_.windows.at(client).mdi->maximized=true;
    refresh_mdi(client,&memory);return callbacks;
}
void WindowsServices::erase_mdi_window(std::uint64_t handle) {
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return;
    if(found->second.mdi) {
        for(auto& [menu,items]:gui_.menus){(void)menu;std::erase_if(items,[&](const auto& item){return item.mdi_owner==handle;});}
        if(const auto menu=gui_.menus.find(found->second.mdi->window_menu);menu!=gui_.menus.end())
            std::erase_if(menu->second,[&](const auto& item){return item.mdi_owner==handle;});
        if(const auto frame=gui_.windows.find(found->second.parent);frame!=gui_.windows.end()){frame->second.title=found->second.mdi->frame_title;present_gui(found->second.parent);}
    }
    if(found->second.mdi_child) {
        const auto client=found->second.parent;
        if(const auto parent=gui_.windows.find(client);parent!=gui_.windows.end() && parent->second.mdi) {
            auto& mdi=*parent->second.mdi;std::erase(mdi.children,handle);
            if(mdi.active==handle){mdi.active=next_mdi_child(client,handle,false);if(!mdi.active)mdi.maximized=false;}
            refresh_mdi(client);
        }
    }
}
bool WindowsServices::dispatch_mdi_client(GuestState& state,GuestMemory& memory,const GuiMessage& message,bool wide) {
    const auto client=message.window;
    auto& window=gui_.windows.at(client);auto& mdi=*window.mdi;
    std::vector<GuestCallback> callbacks;std::uint64_t result=0;
    const auto call=[&](std::string_view api,std::vector<std::uint64_t> args){callbacks.push_back({resolve("user32.dll",api).value,std::move(args)});};
    const auto activate=[&](std::uint64_t child){auto list=activate_mdi(client,child,memory);callbacks.insert(callbacks.end(),list.begin(),list.end());};
    const auto position=[&](std::uint64_t child,std::int32_t x,std::int32_t y,std::uint32_t width,std::uint32_t height){call("SetWindowPos",{child,0,word(x),word(y),width,height,0x14});};
    switch(message.message) {
    case 0x220: {
        const auto p=message.lparam;const unsigned unit=x86_?4:8,integers=x86_?12:24;
        if(!p){last_error_=87;break;}
        memory.check(GuestAddress{p},x86_?36:56,Access::Read);
        const auto pointer=[&](unsigned offset){return memory.load(GuestAddress{p+offset},x86_?32:64);};
        const auto dword=[&](unsigned offset){return memory.load(GuestAddress{p+offset},32);};
        call(wide?"CreateMDIWindowW":"CreateMDIWindowA",{pointer(0),pointer(unit),dword(integers+16),dword(integers),dword(integers+4),dword(integers+8),dword(integers+12),client,pointer(unit*2),pointer(x86_?32:48)});
        start_callbacks(state,memory,std::move(callbacks),Continuation::ReturnCallback);return true;
    }
    case 0x221:
        if(mdi_member(gui_,client,message.wparam,false))call("DestroyWindow",{message.wparam});
        break;
    case 0x222:activate(message.wparam);break;
    case 0x223:
        if(mdi_member(gui_,client,message.wparam)){activate(message.wparam);call("ShowWindow",{message.wparam,9});}
        break;
    case 0x224: {
        const auto current=message.wparam?message.wparam:mdi.active;
        const auto next=next_mdi_child(client,current,message.lparam!=0);
        if(!message.lparam && mdi_member(gui_,client,current))call("SetWindowPos",{current,1,0,0,0,0,0x13});
        activate(next);break;
    }
    case 0x225:
        if(mdi_member(gui_,client,message.wparam)){activate(message.wparam);call("ShowWindow",{message.wparam,3});}
        break;
    case 0x226:case 0x227:case 0x228: {
        const bool tile=message.message==0x226,icons=message.message==0x228;
        if(!icons && (message.wparam&~static_cast<std::uint64_t>(tile?3U:6U))){last_error_=87;break;}
        std::vector<std::uint64_t> children;
        const auto append=[&](std::uint64_t handle){if(!mdi_member(gui_,client,handle))return;const auto& w=gui_.windows.at(handle);
            if(w.visible && ((w.show_command==2)==icons) && (icons || !(message.wparam&2) || w.enabled))children.push_back(handle);};
        if(message.message==0x227 && (message.wparam&4))for(auto it=gui_.z_order.rbegin();it!=gui_.z_order.rend();++it)append(*it);
        else for(const auto child:mdi.children)append(child);
        if(!icons && mdi.maximized && mdi.active)call("ShowWindow",{mdi.active,9});
        const auto count=static_cast<unsigned>(children.size());
        for(unsigned i=0;i<count;++i) {
            if(icons){const auto columns=std::max(1U,window.width/160);position(children[i],static_cast<std::int32_t>((i%columns)*160),static_cast<std::int32_t>(window.height)-static_cast<std::int32_t>((i/columns+1)*32),160,32);}
            else if(tile) {
                auto columns=std::max(1U,static_cast<unsigned>(std::ceil(std::sqrt(count)))),rows=(count+columns-1)/columns;
                if(message.wparam&1)std::swap(columns,rows);
                const auto column=i%columns,row=i/columns;
                const auto in_row=std::min(columns,count-row*columns);
                const auto left=static_cast<std::uint64_t>(column)*window.width/in_row,right=static_cast<std::uint64_t>(column+1)*window.width/in_row;
                const auto top=static_cast<std::uint64_t>(row)*window.height/rows,bottom=static_cast<std::uint64_t>(row+1)*window.height/rows;
                position(children[i],static_cast<std::int32_t>(left),static_cast<std::int32_t>(top),static_cast<std::uint32_t>(right-left),static_cast<std::uint32_t>(bottom-top));
            } else {const auto step=std::min({26U,window.width/std::max(1U,count+1),window.height/std::max(1U,count+1)});
                position(children[i],static_cast<std::int32_t>(i*step),static_cast<std::int32_t>(i*step),window.width-(count-1)*step,window.height-(count-1)*step);}
        }
        result=icons?0:1;break;
    }
    case 0x229:
        if(message.lparam)memory.store(GuestAddress{message.lparam},32,mdi.active && gui_.windows.at(mdi.active).show_command==3);
        result=mdi.active;break;
    case 0x230: {
        const auto frame=gui_.windows.find(window.parent);
        if(frame==gui_.windows.end()){last_error_=1400;break;}
        if((message.wparam && !gui_.menus.contains(message.wparam)) || (message.lparam && !gui_.menus.contains(message.lparam))){last_error_=1401;break;}
        result=frame->second.menu;
        if(message.lparam && message.lparam!=mdi.window_menu){if(auto old=gui_.menus.find(mdi.window_menu);old!=gui_.menus.end())std::erase_if(old->second,[&](const auto& item){return item.mdi_owner==client;});mdi.window_menu=message.lparam;}
        if(message.wparam && message.wparam!=frame->second.menu){
            if(auto old=gui_.menus.find(frame->second.menu);old!=gui_.menus.end())std::erase_if(old->second,[&](const auto& item){return item.mdi_owner==client;});
            const auto size=gui_outer_size(frame->second);frame->second.menu=message.wparam;gui_refresh_frame(frame->second);const auto inner=gui_client_size(size[0],size[1],gui_frame_insets(frame->second));frame->second.width=inner[0];frame->second.height=inner[1];
        }
        refresh_mdi(client,&memory);break;
    }
    case 0x234:refresh_mdi(client,&memory);result=gui_.windows.at(window.parent).menu;break;
    case 5:
        if(mdi.active && mdi.maximized) {const auto& child=gui_.windows.at(mdi.active);const auto inset=gui_frame_insets(child);
            position(mdi.active,-static_cast<std::int32_t>(inset.left),-static_cast<std::int32_t>(inset.top),window.width+inset.left+inset.right,window.height+inset.top+inset.bottom);}
        refresh_mdi(client,&memory);break;
    case 7:if(mdi.active)call("SetFocus",{mdi.active});break;
    case 0x114:case 0x115: {
        const unsigned axis=message.message==0x115;refresh_mdi(client);
        const auto found=gui_.scroll_info.find({client,axis});if(found==gui_.scroll_info.end() || mdi.maximized)break;
        const auto get=[&](unsigned offset){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=static_cast<std::uint32_t>(found->second[offset+i])<<(8*i);return value;};
        const auto minimum=signed_word(get(8)),maximum=signed_word(get(12));const auto page=get(16);
        std::int64_t next=mdi.scroll[axis];switch(message.wparam&65535) {
        case 0:next-=16;break;case 1:next+=16;break;case 2:next-=page;break;case 3:next+=page;break;
        case 4:case 5:next=static_cast<std::int16_t>(message.wparam>>16);break;
        case 6:next=minimum;break;case 7:next=maximum;break;default:break;
        }
        next=std::clamp<std::int64_t>(next,minimum,std::max<std::int64_t>(minimum,static_cast<std::int64_t>(maximum)-page+1));
        const auto delta=static_cast<std::int64_t>(mdi.scroll[axis])-next;mdi.scroll[axis]=static_cast<std::int32_t>(next);
        for(const auto child:mdi.children)if(mdi_member(gui_,client,child)){const auto& w=gui_.windows.at(child);if(w.show_command==2)continue;
            call("SetWindowPos",{child,0,word(static_cast<std::int32_t>(static_cast<std::int64_t>(w.x)+(axis?0:delta))),word(static_cast<std::int32_t>(static_cast<std::int64_t>(w.y)+(axis?delta:0))),0,0,0x15});}
        refresh_mdi(client,&memory);break;
    }
    default:return false;
    }
    start_callbacks(state,memory,std::move(callbacks),Continuation::Return,result);return true;
}

std::optional<std::uint32_t> WindowsServices::invoke_mdi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const bool wide=service.symbol.ends_with('W');
    const auto api=[&](std::string_view name,std::vector<std::uint64_t> args,Continuation continuation=Continuation::ReturnCallback,std::uint64_t result=0){start_callbacks(state,memory,{{resolve("user32.dll",name).value,std::move(args)}},continuation,result);};
    const auto fail=[&](std::uint32_t error){last_error_=error;arg.return_to(state,0);};
    if(service.symbol.starts_with("CreateMDIWindow")) {
        const auto client=gui_.windows.find(arg(7));if(client==gui_.windows.end() || !client->second.mdi){fail(1400);return std::nullopt;}
        api(wide?"CreateWindowExW":"CreateWindowExA",{0x40,arg(0),arg(1),arg(2),arg(3),arg(4),arg(5),arg(6),arg(7),0,arg(8),arg(9)});return std::nullopt;
    }
    if(service.symbol=="TranslateMDISysAccel") {
        const auto client=gui_.windows.find(arg(0));if(client==gui_.windows.end() || !client->second.mdi){fail(1400);return std::nullopt;}
        if(!arg(1)){fail(87);return std::nullopt;}
        const auto message=mdi_message(memory,arg(1),x86_);const auto child=client->second.mdi->active;
        if(!child || !gui_.windows.at(child).enabled || message.message!=0x100 || !(gui_.keyboard[17]&128) || (gui_.keyboard[18]&128)) {arg.return_to(state,0);return std::nullopt;}
        std::uint64_t command=0;
        if(message.wparam==0x73)command=0xf060;
        else if(message.wparam==0x75 || message.wparam==9)command=gui_.keyboard[16]&128?0xf050:0xf040;
        if(!command){arg.return_to(state,0);return std::nullopt;}
        api(gui_.windows.at(child).unicode?"SendMessageW":"SendMessageA",{child,0x112,command,0},Continuation::Return,1);return std::nullopt;
    }
    const bool frame=service.symbol.starts_with("DefFrameProc");const auto handle=arg(0);
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end()){fail(1400);return std::nullopt;}
    const auto client=frame?arg(1):found->second.parent;
    const auto message=static_cast<std::uint32_t>(arg(frame?2:1));const auto wp=arg(frame?3:2),lp=arg(frame?4:3);
    const auto fallback=[&](){api(wide?"DefWindowProcW":"DefWindowProcA",{handle,message,wp,lp});};
    const auto parent=gui_.windows.find(client);
    if(parent==gui_.windows.end() || !parent->second.mdi || (frame?parent->second.parent!=handle:!found->second.mdi_child)){fallback();return std::nullopt;}
    auto& mdi=*parent->second.mdi;std::vector<GuestCallback> callbacks;
    const auto call=[&](std::string_view name,std::vector<std::uint64_t> args){callbacks.push_back({resolve("user32.dll",name).value,std::move(args)});};
    const auto send=[&](std::uint64_t target,std::uint32_t code,std::uint64_t wparam,std::uint64_t lparam){const auto w=gui_.windows.find(target);if(w!=gui_.windows.end())call(w->second.unicode?"SendMessageW":"SendMessageA",{target,code,wparam,lparam});};
    const auto activate=[&](std::uint64_t target){auto list=activate_mdi(client,target,memory);callbacks.insert(callbacks.end(),list.begin(),list.end());};
    std::uint64_t result=0;
    if(frame) {
        if(message==0x111 && !lp && !(wp>>16)) {
            const auto child=std::find_if(mdi.children.begin(),mdi.children.end(),[&](auto w){return mdi_member(gui_,client,w) && gui_.windows.at(w).id==(wp&65535);});
            if(child!=mdi.children.end())activate(*child);
            else if((wp&0xfff0)>=0xf000 && mdi.active)send(mdi.active,0x112,wp,lp);
            else {fallback();return std::nullopt;}
        } else if(message==7)call("SetFocus",{client});
        else if(message==5)call("SetWindowPos",{client,0,0,0,found->second.width,found->second.height,0x14});
        else if(message==12){mdi.frame_title=lp?(wide?read_utf16(memory,GuestAddress{lp}):utf8_to_utf16_lossy(read_ansi(memory,lp))):std::u16string{};refresh_mdi(client,&memory);result=1;}
        else if(message==0x86){if(mdi.active)send(mdi.active,0x86,wp,lp);call(wide?"DefWindowProcW":"DefWindowProcA",{handle,message,wp,lp});start_callbacks(state,memory,std::move(callbacks),Continuation::ReturnCallback);return std::nullopt;}
        else if(message==0x10){for(const auto child:mdi.children)call("DestroyWindow",{child});call(wide?"DefWindowProcW":"DefWindowProcA",{handle,message,wp,lp});start_callbacks(state,memory,std::move(callbacks),Continuation::ReturnCallback);return std::nullopt;}
        else if(message==0x120 && (wp&65535)==u'-' && mdi.active) {
            const auto menu=gui_system_menu(mdi.active);if(!menu){arg.return_to(state,0);return std::nullopt;}
            const auto origin=gui_client_origin(gui_,mdi.active);api("TrackPopupMenuEx",{menu,0,word(static_cast<std::int32_t>(origin[0])),word(static_cast<std::int32_t>(origin[1])),mdi.active,0},Continuation::Return,0);return std::nullopt;
        } else {fallback();return std::nullopt;}
    } else {
        auto& child=found->second;
        if(message==0xa1 || message==0xa3) {
            if(wp>=10 && wp<=17){constexpr std::array<unsigned,8> edges{1,2,3,4,5,6,7,8};start_mdi_move(state,memory,handle,true,edges[wp-10]);return std::nullopt;}
            if(wp==2){if(message==0xa3)call("ShowWindow",{handle,child.show_command==3?9U:3U});else {start_mdi_move(state,memory,handle,false,0);return std::nullopt;}}
            else if(wp==20)send(handle,0x112,0xf060,lp);
            else if(wp==9)send(handle,0x112,0xf030,lp);
            else if(wp==8)send(handle,0x112,0xf020,lp);
            else if(wp==3){const auto menu=gui_system_menu(handle);if(menu)call("TrackPopupMenuEx",{menu,0,word(static_cast<std::int16_t>(lp)),word(static_cast<std::int16_t>(lp>>16)),handle,0});}
        } else if(message==0x21){activate(handle);result=1;}
        else if(message==0x201){activate(handle);call(wide?"DefWindowProcW":"DefWindowProcA",{handle,message,wp,lp});}
        else if(message==0x22 || message==7)activate(handle);
        else if(message==0x24) {
            if(!lp){fail(87);return std::nullopt;}memory.check(GuestAddress{lp},40,Access::Write);const auto inset=gui_frame_insets(child);
            memory.store(GuestAddress{lp+8},32,parent->second.width+inset.left+inset.right);memory.store(GuestAddress{lp+12},32,parent->second.height+inset.top+inset.bottom);
            memory.store(GuestAddress{lp+16},32,word(-static_cast<std::int32_t>(inset.left)));memory.store(GuestAddress{lp+20},32,word(-static_cast<std::int32_t>(inset.top)));
            memory.store(GuestAddress{lp+24},32,112);memory.store(GuestAddress{lp+28},32,inset.top+inset.bottom+1);
            memory.store(GuestAddress{lp+32},32,8192);memory.store(GuestAddress{lp+36},32,8192);
        } else if(message==12){child.title=lp?(wide?read_utf16(memory,GuestAddress{lp}):utf8_to_utf16_lossy(read_ansi(memory,lp))):std::u16string{};present_gui(handle);refresh_mdi(client,&memory);result=1;}
        else if(message==0x86){child.mdi_active=wp!=0;present_gui(handle);result=1;}
        else if(message==3 || message==5) {
            if(message==5 && mdi.active==handle){mdi.maximized=wp==2;if(wp==1)activate(next_mdi_child(client,handle,false));}
            refresh_mdi(client,&memory);
        } else if(message==0x120)send(parent->second.parent,message,wp,lp);
        else if(message==0x112) {
            switch(wp&0xfff0) {
            case 0xf060:send(handle,0x10,0,0);break;
            case 0xf020:call("ShowWindow",{handle,2});break;
            case 0xf030:activate(handle);call("ShowWindow",{handle,3});break;
            case 0xf120:call("ShowWindow",{handle,9});break;
            case 0xf040:case 0xf050:send(client,0x224,handle,(wp&0xfff0)==0xf050);break;
            case 0xf010:case 0xf000:start_mdi_move(state,memory,handle,(wp&0xfff0)==0xf000,static_cast<std::uint32_t>(wp&15));return std::nullopt;
            default:fallback();return std::nullopt;
            }
        } else {fallback();return std::nullopt;}
    }
    start_callbacks(state,memory,std::move(callbacks),Continuation::Return,result);return std::nullopt;
}
void WindowsServices::start_mdi_move(GuestState& state,GuestMemory& memory,std::uint64_t handle,bool sizing,std::uint32_t edge) {
    const auto& window=gui_.windows.at(handle);
    if(window.show_command==3 || (sizing && !(window.style&0x40000))){complete_call(state,memory,0);return;}
    GuiMdiMove move;move.window=handle;move.thread=threads_.current;move.capture=gui_.capture;move.sizing=sizing;move.edge=edge>=1 && edge<=8?edge:8;
    const auto size=gui_outer_size(window);move.original=move.rectangle={window.x,window.y,static_cast<std::int32_t>(size[0]),static_cast<std::int32_t>(size[1])};
    move.anchor=gui_.pointer_position;move.message_buffer=allocate_heap(memory,80);
    if(!move.message_buffer){last_error_=8;complete_call(state,memory,0);return;}
    const auto token=gui_.next_object++;
    try {
        gui_.mdi_moves.emplace(token,move);
        auto callbacks=activate_mdi(window.parent,handle,memory);
        callbacks.push_back({resolve("user32.dll","SetCapture").value,{handle}});
        callbacks.push_back({resolve("user32.dll",window.unicode?"SendMessageW":"SendMessageA").value,{handle,0x231,0,0}});
        start_callbacks(state,memory,std::move(callbacks),Continuation::MdiMoveOperation,token);
    } catch(...) {discard_mdi_move(memory,state,token);throw;}
}
void WindowsServices::discard_mdi_move(GuestMemory& memory,const GuestState& state,std::uint64_t token) {
    const auto found=gui_.mdi_moves.find(token);if(found==gui_.mdi_moves.end())return;
    const auto move=found->second;
    if(gui_.capture==move.window)gui_.capture=gui_.windows.contains(move.capture)?move.capture:0;
    (void)free_heap(memory,move.message_buffer,state);gui_.mdi_moves.erase(found);
}
std::optional<std::uint32_t> WindowsServices::resume_mdi_move(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t value) {
    const auto token=frame.value;const auto found=gui_.mdi_moves.find(token);
    if(found==gui_.mdi_moves.end()){complete_call(state,memory,0);return std::nullopt;}
    auto& move=found->second;std::vector<GuestCallback> callbacks;
    const auto call=[&](std::string_view api,std::vector<std::uint64_t> args){callbacks.push_back({resolve("user32.dll",api).value,std::move(args)});};
    const auto position=[&](const std::array<std::int32_t,4>& r){call("SetWindowPos",{move.window,0,word(r[0]),word(r[1]),word(r[2]),word(r[3]),0x14});};
    const auto finish=[&]() {
        if(move.cancel && gui_.windows.contains(move.window))position(move.original);
        if(move.capture && gui_.windows.contains(move.capture))call("SetCapture",{move.capture});else call("ReleaseCapture",{});
        if(gui_.windows.contains(move.window))call(gui_.windows.at(move.window).unicode?"SendMessageW":"SendMessageA",{move.window,0x232,0,0});
        move.stage=GuiMdiMove::Stage::Finish;
    };
    if(move.stage==GuiMdiMove::Stage::Finish){discard_mdi_move(memory,state,token);complete_call(state,memory,0);return std::nullopt;}
    if(!gui_.windows.contains(move.window))finish();
    else if(move.stage==GuiMdiMove::Stage::Message) {
        if(value==0 || static_cast<std::uint32_t>(value)==UINT32_MAX) {
            if(!value){GuiMessage quit{0,memory.load(GuestAddress{move.message_buffer+(x86_?8U:16U)},x86_?32:64),0,18};quit.thread=move.thread;enqueue_gui_message(quit);}
            finish();
        } else {
            const auto message=mdi_message(memory,move.message_buffer,x86_);
            if(message.message==0x202 || message.message==0xa2 || (message.message==0x100 && (message.wparam==13 || message.wparam==27))){move.cancel=message.wparam==27 && message.message==0x100;finish();}
            else if(message.message==0x200 || message.message==0xa0 || (message.message==0x100 && message.wparam>=37 && message.wparam<=40)) {
                std::int64_t dx{},dy{};const bool pointer=message.message!=0x100;
                if(pointer){const auto at=move.message_buffer+(x86_?20U:36U);dx=static_cast<std::int64_t>(signed_word(memory.load(GuestAddress{at},32)))-move.anchor[0];dy=static_cast<std::int64_t>(signed_word(memory.load(GuestAddress{at+4},32)))-move.anchor[1];move.rectangle=move.original;}
                else {dx=message.wparam==37?-8:message.wparam==39?8:0;dy=message.wparam==38?-8:message.wparam==40?8:0;}
                const auto clamp=[](std::int64_t n){return static_cast<std::int32_t>(std::clamp<std::int64_t>(n,INT32_MIN,INT32_MAX));};
                if(!move.sizing){move.rectangle[0]=clamp(static_cast<std::int64_t>(move.rectangle[0])+dx);move.rectangle[1]=clamp(static_cast<std::int64_t>(move.rectangle[1])+dy);}
                else {
                    const bool left=move.edge==1 || move.edge==4 || move.edge==7,right=move.edge==2 || move.edge==5 || move.edge==8;
                    const bool top=move.edge>=3 && move.edge<=5,bottom=move.edge>=6;
                    if(left || right){const auto next=static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(move.rectangle[2])+(left?-dx:dx),112,8192));if(left)move.rectangle[0]=clamp(static_cast<std::int64_t>(move.rectangle[0])+move.rectangle[2]-next);move.rectangle[2]=next;}
                    if(top || bottom){const auto inset=gui_frame_insets(gui_.windows.at(move.window));const auto next=static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(move.rectangle[3])+(top?-dy:dy),inset.top+inset.bottom+1,8192));if(top)move.rectangle[1]=clamp(static_cast<std::int64_t>(move.rectangle[1])+move.rectangle[3]-next);move.rectangle[3]=next;}
                }
                const auto parent_origin=gui_client_origin(gui_,gui_.windows.at(move.window).parent);const auto rect=move.message_buffer+48;
                memory.store(GuestAddress{rect},32,word(clamp(move.rectangle[0]+parent_origin[0])));memory.store(GuestAddress{rect+4},32,word(clamp(move.rectangle[1]+parent_origin[1])));
                memory.store(GuestAddress{rect+8},32,word(clamp(static_cast<std::int64_t>(move.rectangle[0])+move.rectangle[2]+parent_origin[0])));memory.store(GuestAddress{rect+12},32,word(clamp(static_cast<std::int64_t>(move.rectangle[1])+move.rectangle[3]+parent_origin[1])));
                call(gui_.windows.at(move.window).unicode?"SendMessageW":"SendMessageA",{move.window,move.sizing?0x214U:0x216U,move.sizing?move.edge:0,rect});move.stage=GuiMdiMove::Stage::Apply;
            } else {call("TranslateMessage",{move.message_buffer});call("DispatchMessageW",{move.message_buffer});move.stage=GuiMdiMove::Stage::Dispatch;}
        }
    } else if(move.stage==GuiMdiMove::Stage::Apply) {
        const auto origin=gui_client_origin(gui_,gui_.windows.at(move.window).parent);const auto rect=move.message_buffer+48;
        const auto left=signed_word(memory.load(GuestAddress{rect},32)),top=signed_word(memory.load(GuestAddress{rect+4},32));
        const auto right=signed_word(memory.load(GuestAddress{rect+8},32)),bottom=signed_word(memory.load(GuestAddress{rect+12},32));
        move.rectangle={static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(left)-origin[0],INT32_MIN,INT32_MAX)),static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(top)-origin[1],INT32_MIN,INT32_MAX)),static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(right)-left,1,8192)),static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(bottom)-top,1,8192))};
        position(move.rectangle);move.stage=GuiMdiMove::Stage::Dispatch;
    } else {call("GetMessageW",{move.message_buffer,0,0,0});move.stage=GuiMdiMove::Stage::Message;}
    start_callbacks(state,memory,std::move(callbacks),Continuation::MdiMoveOperation,token);return std::nullopt;
}
}
