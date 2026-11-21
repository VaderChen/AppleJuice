        const GuiTimerKey timer{msg.window,data.preview_timer,0x113};
        const auto screen=gui_child(msg.window,preview_screen);
        const auto caption=[&](unsigned control,std::u16string value){
            if(const auto child=gui_child(msg.window,control)){
                gui_.windows.at(child).title=std::move(value);
                present_gui(child);
            }
        };
        const auto preview_command=msg.message==0x111?static_cast<unsigned>(msg.wparam&0xffffU):0;
        const auto notice=static_cast<unsigned>((msg.wparam>>16)&0xffffU);
        const bool triggered=preview_command==preview_button;
        const bool timer_tick=msg.message==0x113 && msg.wparam==timer.id;
        const auto hdc=screen+0x10000000U;
        const auto clear_frame=[&]{
            if(!screen || !rasters_.contains(hdc))return;
            if(const auto surface=raster_surface(memory,hdc)){
                GuiDraw fill;
                fill.kind=GuiDraw::Kind::Fill;
                fill.right=static_cast<std::int32_t>(surface->width());
                fill.bottom=static_cast<std::int32_t>(surface->height());
                surface->draw(fill);
                (void)flush_gdi_batch(memory);
                present_gui(screen);
            }
        };
        const auto stop=[&](bool reset){
            gui_.timers.erase(timer);
            if(reset){
                data.preview.reset();
                data.preview_path.clear();
            }
            caption(preview_button,reset?u"預覽":u"繼續預覽");
        };
        if(screen && (triggered || timer_tick)){
            try {
                if(triggered){
                    const auto edit=gui_child(msg.window,file_name_edit);
                    const auto name=edit?slash(gui_.windows.at(edit).title):std::u16string{};
                    if(name.empty()){
                        stop(true);
                        clear_frame();
                        caption(499,u"請選擇要預覽的 AVI。");
                        return true;
                    }
                    auto path=file_dialog_path(data.directory,name,runtime_.current_directory);
                    const auto slash_at=name.find_last_of(u'\\');
                    const auto leaf=name.substr(slash_at==name.npos?0:slash_at+1);
                    if(leaf.find(u'.')==leaf.npos && !data.extension.empty())path+=u"."+data.extension;
                    if(data.preview_path==path && gui_.timers.contains(timer)){
                        stop(false);
                        return true;
                    }
                    if(!data.preview || data.preview_path!=path || !data.preview->has_next()){
                        clear_frame();
                        data.preview=AviPreviewClip::open(filesystem_,utf16_to_utf8(path));
                        data.preview_path=std::move(path);
                    }
                    caption(499,u"");
                }
                if(!data.preview){
                    stop(true);
                    if(triggered)caption(499,u"無法預覽指定的 AVI。");
                    return true;
                }
                const auto frame=data.preview->next();
                if(frame){
                    const auto& window=gui_.windows.at(screen);
                    const auto w=window.width,h=window.height;
                    if(!w || !h){stop(true);return true;}
                    const auto source=make_hardware_raster(frame->width,frame->height,frame->pixels);
                    auto& target=gui_.contexts.try_emplace(hdc).first->second;
                    const auto dw=std::min(w,std::max(1U,h*frame->width/frame->height));
                    const auto dh=std::min(h,std::max(1U,w*frame->height/frame->width));
                    const auto x=static_cast<std::int32_t>((w-dw)/2),y=static_cast<std::int32_t>((h-dh)/2);
                    const GuiRect dest{x,y,x+static_cast<std::int32_t>(dw),y+static_cast<std::int32_t>(dh)};
                    const GuiRect from{0,0,static_cast<std::int32_t>(frame->width),static_cast<std::int32_t>(frame->height)};
                    RasterBlit options;options.operation=0xcc;
                    if(!blit_raster(memory,hdc,source.get(),dest,from,target,UINT32_MAX,options)){
                        stop(true);
                        caption(499,u"無法繪製 AVI 預覽。");
                        return true;
                    }
                    (void)flush_gdi_batch(memory);
                    present_gui(screen);
                }
                if(!data.preview->has_next()){
                    stop(true);
                    if(!frame && triggered)caption(499,u"AVI 影格無法解碼。");
                }else if(triggered){
                    const auto interval=data.preview->interval();
                    gui_.timers[timer]={runtime_.clock_ticks+interval,interval,0,dialog.thread,0};
                    caption(preview_button,u"暫停預覽");
                }
            }catch(const std::runtime_error&){
                stop(true);
                caption(499,u"無法解碼指定的 AVI。");
            }catch(const std::invalid_argument&){
                stop(true);
                caption(499,u"無法預覽指定的 AVI。");
            }catch(const std::bad_alloc&){
                stop(true);
                caption(499,u"記憶體不足，無法預覽 AVI。");
            }
            return true;
        }
        if((preview_command==412 && notice==1) || (preview_command==file_name_edit && notice==0x300) ||
           (preview_command>=414 && preview_command<=417)){
            stop(true);
            clear_frame();
        }
