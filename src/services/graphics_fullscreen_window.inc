    // D3D9／DXGI 共用視窗狀態、原生顯示模式與 Win32 尺寸通知。
    const auto notify_swap_window=[&](std::uint64_t hwnd,std::int32_t x,std::int32_t y,std::uint32_t width,std::uint32_t height){
        auto& window=gui_.windows.at(hwnd);const bool moved=window.x!=x || window.y!=y,sized=window.width!=width || window.height!=height;
        window.x=x;window.y=y;window.width=width;window.height=height;present_gui(hwnd);
        if(moved)enqueue_gui_message({hwnd,0,gui_move_lparam(window),3});
        if(sized)enqueue_gui_message({hwnd,0,static_cast<std::uint64_t>(width)|(static_cast<std::uint64_t>(height)<<16),5});
    };
    const auto restore_graphics_fullscreen=[&](auto& id){
        if(!id.fullscreen)return true;
        if(native_gui_ && !native_gui_->display_fullscreen(id.window,false))return false;
        if(gui_.windows.contains(id.window)){
            auto& window=gui_.windows.at(id.window);window.style=id.windowed_style;window.ex_style=id.windowed_ex_style;window.frame_insets=id.windowed_insets;
            notify_swap_window(id.window,id.windowed_rect[0],id.windowed_rect[1],static_cast<std::uint32_t>(id.windowed_rect[2]),static_cast<std::uint32_t>(id.windowed_rect[3]));
        }
        id.fullscreen=false;return true;
    };
    const auto enter_graphics_fullscreen=[&](auto& id,const std::optional<GuiDisplayMode>& mode){
        const auto found=gui_.windows.find(id.window);
        if(found==gui_.windows.end() || found->second.parent || !found->second.visible || found->second.show_command==2)return false;
        if(native_gui_ && !native_gui_->display_fullscreen(id.window,true,mode))return false;
        auto& window=found->second;
        if(!id.fullscreen){id.windowed_rect={window.x,window.y,static_cast<std::int32_t>(window.width),static_cast<std::int32_t>(window.height)};id.windowed_style=window.style;id.windowed_ex_style=window.ex_style;id.windowed_insets=window.frame_insets;}
        window.style=(window.style&~0x00cf0000U)|0x80000000U;window.frame_insets=std::array<std::uint32_t,4>{};
        const auto rect=gui_screen_area();notify_swap_window(id.window,rect[0],rect[1],static_cast<std::uint32_t>(rect[2]-rect[0]),static_cast<std::uint32_t>(rect[3]-rect[1]));
        id.fullscreen=true;return true;
    };
