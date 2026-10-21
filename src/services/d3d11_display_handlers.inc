        // DXGI 與 User32 共用目前公開的主顯示器；原生模式來自 Core Graphics。
        if(!graphics_handled && symbol=="EnumOutputs"){
            graphics_handled=true;(void)typed(arg(0),Kind::Adapter);require(arg(2));output(2);write_pointer(arg(2),0);
            if(u32(1) || !native_gui_ || !native_gui_->current_display_mode())result=not_found;else write_pointer(arg(2),create_output(arg(0)));
        }else if(!graphics_handled && symbol=="OutputGetDesc"){
            graphics_handled=true;(void)typed(arg(0),Kind::Output);require(arg(1));std::vector<std::uint8_t> desc(x86_?92:96);
            const auto name=utf16_bytes(u"\\\\.\\DISPLAY1");std::copy(name.begin(),name.end(),desc.begin());const auto rect=gui_screen_area();for(unsigned i=0;i<4;++i)put_word(desc,64+4*i,static_cast<std::uint32_t>(rect[i]));
            put_word(desc,80,native_gui_ && native_gui_->current_display_mode()?1:0);put_word(desc,84,1);put_word(desc,88,1);memory.write(GuestAddress{arg(1)},desc);
        }else if(!graphics_handled && symbol=="GetDisplayModeList"){
            graphics_handled=true;(void)typed(arg(0),Kind::Output);require(arg(3) && !(u32(2)&~15U));const auto capacity=memory.load(GuestAddress{arg(3)},32);
            auto modes=native_gui_ && scanout_format(u32(1))?native_gui_->display_modes():std::vector<GuiDisplayMode>{};
            std::erase_if(modes,[&](const auto& mode){return (mode.scanline>1 && !(u32(2)&1)) || (mode.scaling==2 && !(u32(2)&2));});
            memory.store(GuestAddress{arg(3)},32,modes.size());
            if(arg(4)){if(capacity<modes.size())result=0x887a0003U;else {memory.check(GuestAddress{arg(4)},modes.size()*28,Access::Write);for(std::size_t i=0;i<modes.size();++i)memory.write(GuestAddress{arg(4)+28*i},display_mode_bytes(modes[i],u32(1)));}}
        }else if(!graphics_handled && symbol=="FindClosestMatchingMode"){
            graphics_handled=true;(void)typed(arg(0),Kind::Output);require(arg(1) && arg(2));auto desired=read_bytes(memory,arg(1),28);auto format=word(desired,16);
            if(arg(3)){require(object(arg(3)).identity->kind==Kind::Device);if(!format)format=87;}else require(format!=0);
            if(!scanout_format(format))result=not_supported;else if(const auto mode=closest_display_mode(desired))memory.write(GuestAddress{arg(2)},display_mode_bytes(*mode,format));else result=not_found;
        }else if(!graphics_handled && symbol=="GetContainingOutput"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);require(arg(1));output(1);write_pointer(arg(1),0);
            if(!native_gui_ || !native_gui_->current_display_mode())result=not_found;
            else if(swap->fullscreen_output){hold(swap->fullscreen_output);write_pointer(arg(1),swap->fullscreen_output);}
            else write_pointer(arg(1),create_output(device_adapter(object(swap->device).identity)));
        }else if(!graphics_handled && symbol=="GetFullscreenState"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);if(arg(1))memory.store(GuestAddress{arg(1)},32,swap->fullscreen?1:0);
            if(arg(2)){output(2);if(swap->fullscreen_output)hold(swap->fullscreen_output);write_pointer(arg(2),swap->fullscreen_output);}
        }else if(!graphics_handled && symbol=="SetFullscreenState"){
            graphics_handled=true;result=set_fullscreen(typed(arg(0),Kind::SwapChain),u32(1)!=0,arg(2));
        }else if(!graphics_handled && symbol=="GetFullscreenDesc"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);require(arg(1));std::vector<std::uint8_t> desc(20);
            std::copy_n(swap->target_mode.begin()+8,8,desc.begin());put_word(desc,8,word(swap->target_mode,20));put_word(desc,12,word(swap->target_mode,24));put_word(desc,16,swap->fullscreen?0:1);memory.write(GuestAddress{arg(1)},desc);
        }else if(!graphics_handled && symbol=="ResizeTarget"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);require(arg(1));auto desired=read_bytes(memory,arg(1),28);
            require(word(desired,0) && word(desired,4) && word(desired,0)<=16384 && word(desired,4)<=16384 && (!word(desired,8) || word(desired,12)) && word(desired,20)<=3 && word(desired,24)<=2);
            if(!word(desired,16))put_word(desired,16,word(swap->desc,16));require(scanout_format(word(desired,16)));
            const auto window=gui_.windows.find(swap->window);if(window==gui_.windows.end())result=0x887a0001U;
            else if(swap->fullscreen){const auto previous=swap->target_mode;swap->target_mode=desired;result=set_fullscreen(swap,true,swap->fullscreen_output);if(result)swap->target_mode=previous;}
            else {swap->target_mode=desired;notify_swap_window(swap->window,window->second.x,window->second.y,word(desired,0),word(desired,4));}
        }else if(!graphics_handled && symbol=="GetRestrictToOutput"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);require(arg(1));output(1);const auto target=binding(swap,"RestrictOutput");if(target)hold(target);write_pointer(arg(1),target);
        }
