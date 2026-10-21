        // DXGI 裝置、父物件與資源共用原有 COM identity；強參照只朝父層，避免循環。
        const auto device_adapter=[&](const std::shared_ptr<Identity>& device){
            if(!device->adapter){
                auto factory=create(Kind::Factory);
                try {auto adapter=create(Kind::Adapter,factory->root);adapter->native=device->native;device->adapter=adapter->root;}
                catch(...){release(factory->root);throw;}
                release(factory->root);
            }
            return device->adapter;
        };
        const auto scanout_format=[](std::uint32_t format){return format==24 || format==28 || format==29 || format==87 || format==91;};
        const auto display_mode_bytes=[&](const GuiDisplayMode& mode,std::uint32_t format){
            std::vector<std::uint8_t> data(28);put_word(data,0,mode.width);put_word(data,4,mode.height);put_word(data,8,mode.refresh_numerator);put_word(data,12,mode.refresh_denominator);put_word(data,16,format);put_word(data,20,mode.scanline);put_word(data,24,mode.scaling);return data;
        };
        const auto closest_display_mode=[&](const std::vector<std::uint8_t>& desired)->std::optional<GuiDisplayMode>{
            require((!word(desired,0))==(!word(desired,4)) && (!word(desired,8) || word(desired,12)) && word(desired,20)<=3 && word(desired,24)<=2);
            if(!native_gui_)return {};auto modes=native_gui_->display_modes();const auto current=native_gui_->current_display_mode();if(!current || modes.empty())return {};
            const auto width=word(desired,0)?word(desired,0):current->width,height=word(desired,4)?word(desired,4):current->height;
            const double rate=word(desired,8)?static_cast<double>(word(desired,8))/word(desired,12):static_cast<double>(current->refresh_numerator)/current->refresh_denominator;
            const auto score=[&](const GuiDisplayMode& mode){return std::tuple{
                word(desired,20) && word(desired,20)!=mode.scanline,word(desired,24) && word(desired,24)!=mode.scaling,
                std::abs(static_cast<std::int64_t>(mode.width)-width)+std::abs(static_cast<std::int64_t>(mode.height)-height),
                std::abs(static_cast<double>(mode.refresh_numerator)/mode.refresh_denominator-rate),mode.native_id!=current->native_id};};
            return *std::min_element(modes.begin(),modes.end(),[&](const auto& a,const auto& b){return score(a)<score(b);});
        };
        const auto create_output=[&](std::uint64_t adapter){auto output=create(Kind::Output,adapter);output->slice=0;return output->root;};
        const auto set_fullscreen=[&](const std::shared_ptr<Identity>& swap,bool enabled,std::uint64_t target)->std::uint32_t{
            if(!enabled){if(target)return invalid;if(!restore_fullscreen(swap))return 0x887a0022U;if(swap->fullscreen_output){release(swap->fullscreen_output);swap->fullscreen_output=0;}return 0;}
            if(target)(void)typed(target,Kind::Output);
            for(const auto& [pointer,item]:bridge.objects){(void)pointer;if(item.kind==Kind::SwapChain && item.identity!=swap && item.identity->fullscreen)return 0x887a0022U;}
            const auto found=gui_.windows.find(swap->window);
            if(!native_gui_ || found==gui_.windows.end() || found->second.parent || !found->second.visible || found->second.show_command==2)return 0x887a0022U;
            auto mode=native_gui_->current_display_mode();if(word(swap->desc,x86_?56:64)&2U)mode=closest_display_mode(swap->target_mode);if(!mode)return not_found;
            if(!target)target=create_output(device_adapter(object(swap->device).identity));else hold(target);
            if(!native_gui_->display_fullscreen(swap->window,true,mode)){release(target);return 0x887a0022U;}
            auto& window=found->second;
            if(!swap->fullscreen){swap->windowed_rect={window.x,window.y,static_cast<std::int32_t>(window.width),static_cast<std::int32_t>(window.height)};swap->windowed_style=window.style;swap->windowed_ex_style=window.ex_style;swap->windowed_insets=window.frame_insets;}
            const auto previous=swap->fullscreen_output;swap->fullscreen_output=target;if(previous)release(previous);
            window.style=(window.style&~0x00cf0000U)|0x80000000U;window.frame_insets=std::array<std::uint32_t,4>{};
            const auto rect=gui_screen_area();notify_swap_window(swap->window,rect[0],rect[1],static_cast<std::uint32_t>(rect[2]-rect[0]),static_cast<std::uint32_t>(rect[3]-rect[1]));
            swap->fullscreen=true;swap->target_mode=display_mode_bytes(*mode,word(swap->desc,16));put_word(swap->desc,x86_?48:56,0);return 0;
        };
