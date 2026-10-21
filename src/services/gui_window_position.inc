// 單一與批次定位共用相同的回呼狀態機。所有前置回呼結束後才套用整批幾何，
// 讓 WINDOWPOSCHANGED 看見完整的新配置；客體暫存隨完成、例外或執行緒退出回收。
void WindowsServices::discard_gui_window_pos(GuestMemory& memory,const GuestState& state,std::uint64_t id) {
    const auto found=gui_.position_operations.find(id);if(found==gui_.position_operations.end())return;
    release_gui_scratch(memory,state,found->second.scratch);gui_.position_operations.erase(found);
}
std::optional<std::uint32_t> WindowsServices::start_gui_window_pos(GuestState& state,GuestMemory& memory,std::vector<GuiWindowPosition> items) {
    const auto id=gui_.next_object++;
    auto& op=gui_.position_operations[id];op.thread=threads_.current;op.items=std::move(items);
    try {
        if(!op.items.empty()) {
            const auto stride=(x86_?28U:40U)+72U;
            const auto base=allocate_gui_scratch(memory,op.scratch,{},stride*op.items.size());
            for(std::size_t i=0;i<op.items.size();++i)op.items[i].data=base+stride*i;
        }
        return finish_gui_window_pos(state,memory,id);
    } catch(const std::bad_alloc&) {
        discard_gui_window_pos(memory,state,id);last_error_=8;complete_call(state,memory,0);return {};
    } catch(...) {discard_gui_window_pos(memory,state,id);throw;}
}
std::optional<std::uint32_t> WindowsServices::finish_gui_window_pos(GuestState& state,GuestMemory& memory,std::uint64_t id) {
    auto& op=gui_.position_operations.at(id);
    const unsigned unit=x86_?4U:8U;const std::uint8_t bits=x86_?32:64;
    const auto fail=[&](std::uint32_t error)->std::optional<std::uint32_t>{
        discard_gui_window_pos(memory,state,id);last_error_=error;complete_call(state,memory,0);return {};
    };
    const auto callback=[&](std::uint64_t proc,std::uint64_t hwnd,std::uint32_t message,std::uint64_t wp,std::uint64_t lp){
        start_callbacks(state,memory,{{proc,{hwnd,message,wp,lp}}},Continuation::GuiWindowPos,id);
    };
    const auto write=[&](const GuiWindowPosition& item){
        const auto data=item.data;memory.store(GuestAddress{data},bits,item.window);memory.store(GuestAddress{data+unit},bits,item.after);
        memory.store(GuestAddress{data+2*unit},32,static_cast<std::uint32_t>(item.x));memory.store(GuestAddress{data+2*unit+4},32,static_cast<std::uint32_t>(item.y));
        memory.store(GuestAddress{data+2*unit+8},32,item.width);memory.store(GuestAddress{data+2*unit+12},32,item.height);memory.store(GuestAddress{data+2*unit+16},32,item.flags);
    };
    const auto validate=[&](const GuiWindowPosition& item)->std::uint32_t {
        const auto found=gui_.windows.find(item.window);if(found==gui_.windows.end() || found->second.destroying)return 1400;
        if(found->second.thread!=op.thread)return 5;
        if(!(item.flags&1U) && (item.width>8192 || item.height>8192))return 87;
        if(!(item.flags&4U) && item.after>1 && item.after<UINT64_MAX-1) {
            const auto after=gui_.windows.find(item.after);
            if(after==gui_.windows.end() || after->second.parent!=found->second.parent)return 1400;
        }
        return 0;
    };
    while(op.phase<3 && op.index<op.items.size()) {
        auto& item=op.items[op.index];if(const auto error=validate(item))return fail(error);
        auto& window=gui_.windows.at(item.window);const auto data=item.data,nc=data+(x86_?28U:40U);
        if(op.phase==0) {
            const auto outer=gui_outer_size(window);
            if(item.flags&1U){item.width=outer[0];item.height=outer[1];}
            if(item.flags&2U){item.x=window.x;item.y=window.y;}
            write(item);op.phase=1;
            if(window.procedure && !(item.flags&0x400U)){callback(window.procedure,item.window,0x46,0,data);return {};}
        }
        if(op.phase==1) {
            item.after=memory.load(GuestAddress{data+unit},bits);
            if(x86_ && item.after>=UINT32_MAX-1)item.after=static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(item.after)));
            item.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{data+2*unit+16},32));
            const auto outer=gui_outer_size(window);
            item.x=(item.flags&2U)?window.x:s32(memory.load(GuestAddress{data+2*unit},32));
            item.y=(item.flags&2U)?window.y:s32(memory.load(GuestAddress{data+2*unit+4},32));
            item.width=(item.flags&1U)?outer[0]:static_cast<std::uint32_t>(std::max(0,s32(memory.load(GuestAddress{data+2*unit+8},32))));
            item.height=(item.flags&1U)?outer[1]:static_cast<std::uint32_t>(std::max(0,s32(memory.load(GuestAddress{data+2*unit+12},32))));
            if(const auto error=validate(item))return fail(error);
            const bool combo=gui_control_class(window)==u"combobox" || gui_control_class(window)==u"comboboxex32";
            const auto height=combo && (window.style&3U)!=1U?24U:item.height;
            write(item);op.phase=2;
            if(window.procedure && ((!(item.flags&1U) && (outer[0]!=item.width || outer[1]!=height)) || (item.flags&0x20U))) {
                const auto origin=gui_client_origin(gui_,item.window);const auto inset=gui_frame_insets(window);
                const auto parent=window.parent && gui_.windows.contains(window.parent)?gui_client_origin(gui_,window.parent):std::array<std::int64_t,2>{};
                const auto rect=[&](std::uint64_t at,std::int64_t left,std::int64_t top,std::uint32_t w,std::uint32_t h){
                    const std::array<std::int64_t,4> values{left,top,left+w,top+h};for(unsigned j=0;j<4;++j)memory.store(GuestAddress{at+4*j},32,static_cast<std::uint32_t>(values[j]));
                };
                rect(nc,parent[0]+item.x,parent[1]+item.y,item.width,height);
                rect(nc+16,origin[0]-inset.left,origin[1]-inset.top,outer[0],outer[1]);
                rect(nc+32,origin[0],origin[1],window.width,window.height);memory.store(GuestAddress{nc+48},bits,data);
                for(unsigned j=0;j<4;++j)memory.store(GuestAddress{nc+56+4*j},32,memory.load(GuestAddress{nc+4*j},32));
                item.calculated=true;callback(window.procedure,item.window,0x83,1,nc);return {};
            }
        }
        if(item.calculated) {
            std::array<std::int32_t,4> outer{},client{};
            for(unsigned j=0;j<4;++j){outer[j]=s32(memory.load(GuestAddress{nc+56+4*j},32));client[j]=s32(memory.load(GuestAddress{nc+4*j},32));}
            GuiWindow geometry;gui_calculated_frame(geometry,outer,client);item.frame=geometry.frame_insets;
        } else if(item.flags&0x20U) {
            const auto inset=gui_frame_insets(window.style,window.ex_style,window.menu!=0);item.frame=std::array{inset.left,inset.top,inset.right,inset.bottom};
        }
        ++op.index;op.phase=0;
    }
    if(op.phase<3) {
        // 回呼可能銷毀其他待更新視窗；先重新核對整批，避免一半已套用才發現無效 HWND。
        for(const auto& item:op.items)if(const auto error=validate(item))return fail(error);
        for(auto& item:op.items) {
            auto& window=gui_.windows.at(item.window);auto& flags=item.flags;
            const auto old_outer=gui_outer_size(window);const bool was_visible=window.visible;
            if(flags&1U){item.width=old_outer[0];item.height=old_outer[1];}
            if(flags&2U){item.x=window.x;item.y=window.y;}
            if(item.frame)window.frame_insets=item.frame;
            const bool combo=gui_control_class(window)==u"combobox" || gui_control_class(window)==u"comboboxex32";
            const auto height=combo && (window.style&3U)!=1U?24U:item.height;
            if(combo && !(flags&1U) && item.height>24)window.combo_drop_height=item.height-24;
            const auto client=gui_client_size(item.width,height,gui_frame_insets(window));
            const bool moved=window.x!=item.x || window.y!=item.y,sized=window.width!=client[0] || window.height!=client[1];
            if(!moved)flags|=2U;
            if(!sized)flags|=1U;else if(flags&0x20U)flags&=~1U;
            if(!(flags&2U)){window.x=item.x;window.y=item.y;}if(!(flags&1U)){window.width=client[0];window.height=client[1];}
            if(flags&0x40U){window.visible=true;window.style|=0x10000000U;}if(flags&0x80U){window.visible=false;window.style&=~0x10000000U;}
            if(!(flags&4U) && item.after!=item.window) {
                if(item.after==UINT64_MAX)window.ex_style|=8U;else if(item.after==1 || item.after==UINT64_MAX-1)window.ex_style&=~8U;
                const auto relative=item.after>=UINT64_MAX-1?0:item.after;std::erase(gui_.z_order,item.window);
                auto at=relative==1?gui_.z_order.end():relative?std::find(gui_.z_order.begin(),gui_.z_order.end(),relative):gui_.z_order.begin();
                if(relative>1 && at!=gui_.z_order.end())++at;gui_.z_order.insert(at,item.window);
                if(native_gui_)native_gui_->reorder(item.window,relative);
            }
            const auto outer=gui_outer_size(window);item.width=outer[0];item.height=outer[1];write(item);
            item.changed=(flags&3U)!=3U || (flags&(0xc0U|0x20U)) || !(flags&4U);
            if(!window.parent && window.visible && window.enabled && !(flags&(0x10U|0x80U)))op.activate=item.window;
            if(!(flags&8U) && window.visible && (moved || sized || !was_visible || (flags&0x20U)))invalidate_window(item.window);
        }
        // 不在每個前置回呼之間向宿主發送半完成的版面。
        for(const auto& item:op.items) {
            if(item.changed)present_gui(item.window);
            if(native_gui_ && (item.flags&0xc0U))show_native_window(item.window,gui_.windows.at(item.window).visible);
        }
        op.index=0;op.phase=3;
    }
    if(op.phase==3) {
        while(op.index<op.items.size()) {
            const auto& item=op.items[op.index++];const auto found=gui_.windows.find(item.window);
            if(item.changed && found!=gui_.windows.end() && found->second.procedure) {
                callback(found->second.procedure,item.window,0x47,0,item.data);return {};
            }
        }
        op.phase=4;
        if(op.activate && gui_.windows.contains(op.activate)) {
            auto callbacks=activate_gui_window(op.activate);
            if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::GuiWindowPos,id);return {};}
        }
    }
    discard_gui_window_pos(memory,state,id);complete_call(state,memory,1);return {};
}
