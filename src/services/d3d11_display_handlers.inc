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
        }else if(!graphics_handled && (symbol=="GetGammaControlCapabilities" || symbol=="SetGammaControl" || symbol=="GetGammaControl" ||
                                       symbol=="WaitForVBlank" || symbol=="TakeOwnership" || symbol=="ReleaseOwnership" ||
                                       (symbol=="GetFrameStatistics" && object(arg(0)).identity->kind==Kind::Output))){
            // IDXGIOutput 家族：gamma 與 GDI SetDeviceGammaRamp 共用原生 256 階曲線（套用到實體輸出，程序結束由原生層還原）；
            // 只在全螢幕時支援的方法以目前全螢幕交換鏈判定。垂直空白依目前顯示模式更新率與客體時鐘推算。
            graphics_handled=true;(void)typed(arg(0),Kind::Output);
            const auto fullscreen_swap=[&]()->std::shared_ptr<Identity>{
                for(const auto& [pointer,item]:bridge.objects){(void)pointer;if(item.kind==Kind::SwapChain && item.identity->fullscreen)return item.identity;}
                return {};
            };
            std::uint64_t numerator=60,denominator=1;
            if(native_gui_)if(const auto mode=native_gui_->current_display_mode();mode && mode->refresh_numerator && mode->refresh_denominator){numerator=mode->refresh_numerator;denominator=mode->refresh_denominator;}
            const auto period=static_cast<unsigned __int128>(denominator)*10000000U; // 以 100 ns 計時單位 × 更新率分子
            const auto put_float=[&](std::uint64_t at,float value){memory.store(GuestAddress{at},32,std::bit_cast<std::uint32_t>(value));};
            if(symbol=="GetGammaControlCapabilities"){
                // DXGI_GAMMA_CONTROL_CAPABILITIES：不支援 scale／offset，256 個等距控制點（0～1）。
                require(arg(1));memory.check(GuestAddress{arg(1)},16+1025*4,Access::Write);
                memory.store(GuestAddress{arg(1)},32,0);put_float(arg(1)+4,1.0F);put_float(arg(1)+8,0.0F);memory.store(GuestAddress{arg(1)+12},32,256);
                for(unsigned i=0;i<1025;++i)put_float(arg(1)+16+4ULL*i,i<256?static_cast<float>(i)/255.0F:0.0F);
            }else if(symbol=="SetGammaControl" || symbol=="GetGammaControl"){
                // DXGI_GAMMA_CONTROL：Scale、Offset（DXGI_RGB）與 1025 組 GammaCurve；僅使用能力回報的前 256 組。
                require(arg(1));const bool set=symbol=="SetGammaControl";memory.check(GuestAddress{arg(1)},24+1025*12,set?Access::Read:Access::Write);
                if(!runtime_.network.external_io)throw HostCallFault("原生 gamma ramp 不支援 deterministic compare");
                if(set && !fullscreen_swap())result=0x887a0022U;
                else if(!native_gui_)result=0x887a0022U;
                else if(set){
                    const auto load=[&](std::uint64_t at){return std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{at},32)));};
                    std::array<std::uint16_t,768> ramp{};
                    for(unsigned c=0;c<3;++c){const auto scale=load(arg(1)+4ULL*c),offset=load(arg(1)+12+4ULL*c);
                        for(unsigned i=0;i<256;++i){auto value=load(arg(1)+24+12ULL*i+4ULL*c)*scale+offset;if(!std::isfinite(value))value=0;
                            ramp[c*256+i]=static_cast<std::uint16_t>(std::lround(std::clamp(value,0.0F,1.0F)*65535.0F));}}
                    if(!native_gui_->set_gamma_ramp(ramp))result=0x887a0022U;
                }else if(const auto ramp=native_gui_->gamma_ramp()){
                    for(unsigned c=0;c<3;++c){put_float(arg(1)+4ULL*c,1.0F);put_float(arg(1)+12+4ULL*c,0.0F);}
                    for(unsigned i=0;i<1025;++i)for(unsigned c=0;c<3;++c)put_float(arg(1)+24+12ULL*i+4ULL*c,i<256?static_cast<float>((*ramp)[c*256+i])/65535.0F:0.0F);
                }else result=0x887a0022U;
            }else if(symbol=="WaitForVBlank"){
                // 等到下一個垂直空白：交給客體 Sleep 讓出 vCPU，不阻塞協調者；剩餘不足 1 ms 時直接返回。
                const auto now=runtime_.clock_ticks;
                const auto next=static_cast<std::uint64_t>((static_cast<unsigned __int128>(now)*numerator/period+1)*period/numerator);
                const auto milliseconds=static_cast<std::uint32_t>((next-now)/10000);
                if(milliseconds){start_callbacks(state,memory,{{resolve("kernel32.dll","Sleep").value,{milliseconds}}},Continuation::Return,0);return std::nullopt;}
            }else if(symbol=="TakeOwnership"){
                if(!arg(1))result=invalid;
                else if(bridge.output_owner && bridge.output_owner!=arg(1))result=0x887a0022U;
                else {bridge.output_owner=arg(1);bridge.output_owner_exclusive=u32(2)!=0;}
            }else if(symbol=="ReleaseOwnership"){bridge.output_owner=0;bridge.output_owner_exclusive=false;}
            else {
                // DXGI_FRAME_STATISTICS：PresentCount 取全螢幕交換鏈，refresh 計數與 SyncQPCTime 對齊目前時間之前的最後一個 vblank。
                const auto swap=fullscreen_swap();
                if(!arg(1))result=invalid;
                else if(!swap)result=0x887a0001U;
                else {
                    const auto refresh=static_cast<std::uint64_t>(static_cast<unsigned __int128>(runtime_.clock_ticks)*numerator/period);
                    const auto sync=static_cast<std::uint64_t>(static_cast<unsigned __int128>(refresh)*period/numerator);
                    std::vector<std::uint8_t> stats(32);put_word(stats,0,static_cast<std::uint32_t>(swap->frames));put_word(stats,4,static_cast<std::uint32_t>(refresh));put_word(stats,8,static_cast<std::uint32_t>(refresh));
                    for(unsigned i=0;i<8;++i)stats[16+i]=static_cast<std::uint8_t>(sync>>(8*i));
                    memory.write(GuestAddress{arg(1)},stats);
                }
            }
        }else if(!graphics_handled && symbol=="GetContainingOutput"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);require(arg(1));output(1);write_pointer(arg(1),0);
            if(!native_gui_ || !native_gui_->current_display_mode())result=not_found;
            else if(swap->fullscreen_output){hold(swap->fullscreen_output);write_pointer(arg(1),swap->fullscreen_output);}
            else write_pointer(arg(1),create_output(device_adapter(object(swap->device).identity)));
        }else if(!graphics_handled && symbol=="GetFullscreenState"){
            graphics_handled=true;auto swap=typed(arg(0),Kind::SwapChain);if(arg(1))memory.store(GuestAddress{arg(1)},32,swap->fullscreen?1:0);
            if(arg(2)){output(2);if(swap->fullscreen_output)hold(swap->fullscreen_output);write_pointer(arg(2),swap->fullscreen_output);}
        }else if(!graphics_handled && symbol=="SetFullscreenState"){
            graphics_handled=true;const auto swap=typed(arg(0),Kind::SwapChain);result=set_fullscreen(swap,u32(1)!=0,arg(2));
            if(!result && u32(1))fullscreen_activation=swap->window;
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
