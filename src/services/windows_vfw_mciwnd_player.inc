                const auto layout=[&]()->Task<bool>{
                    const auto current=gui_.windows.find(hwnd);
                    if(current==gui_.windows.end())co_return false;
                    const auto width=current->second.width,height=current->second.height;
                    const auto y=height>28?height-28:0;
                    const auto place=[&](unsigned id,unsigned x,unsigned cx)->Task<bool>{
                        const auto child=gui_child(hwnd,id);
                        if(child)(void)co_await api("user32.dll","MoveWindow",{child,x,y,cx,28,1});
                        co_return child!=0;
                    };
                    if(!(player.styles&2)){
                        (void)co_await place(play_button,0,58);
                        (void)co_await place(menu_button,60,56);
                        const auto x=(player.styles&0x8008U)?60U:118U;
                        (void)co_await place(track_bar,x,width>x?width-x:0);
                    }
                    if(avi_device() && !(player.styles&4) && width && height){
                        clean();
                        field(unit+8,32,width);
                        field(unit+12,32,(player.styles&2)?height:std::max(y,1U));
                        (void)co_await send(mci::video_put,0x50000);
                    }
                    co_return true;
                };
                const auto update=[&](bool redraw)->Task<bool>{
                    const auto mode=static_cast<unsigned>(co_await status(4));
                    const auto position=static_cast<unsigned>(co_await status(2));
                    const bool position_change=position!=player.last_position,mode_change=mode!=player.last_mode;
                    player.last_position=position;
                    player.last_mode=mode;
                    if(redraw || position_change)(void)co_await paint();
                    if(mode_change){
                        if(const auto button=gui_child(hwnd,play_button)){
                            const auto label=scratch.text(mode==526?u"停止":u"播放");
                            if(label)(void)co_await api("user32.dll","SetWindowTextW",{button,label});
                        }
                        (void)co_await notify(0x100,1224,mode);
                    }
                    if(position_change){
                        if(const auto track=gui_child(hwnd,track_bar))
                            (void)co_await api("user32.dll","SendMessageW",{track,0x405,1,position});
                        (void)co_await notify(0x200,1225,position);
                    }
                    if((player.styles&0x70) && (redraw || position_change || mode_change)){
                        std::u16string caption;
                        if(player.styles&0x10)caption=player.file;
                        if(player.styles&0x40){if(!caption.empty())caption+=u"  ";caption+=get_mode_text(mode);}
                        if(player.styles&0x20){if(!caption.empty())caption+=u"  ";caption+=numeric(position);}
                        const auto title=scratch.text(caption);
                        if(title)(void)co_await api("user32.dll","SetWindowTextW",{hwnd,title});
                    }
                    co_return true;
                };
                const auto close_device=[&]()->Task<unsigned>{
                    if(!player.device)co_return 0;
                    clean();
                    const auto result=co_await send(mci::close);
                    if(result && result!=mci::invalid_device)co_return result;
                    player.device=0;
                    player.file.clear();
                    player.alias.clear();
                    (void)co_await timer();
                    co_return 0;
                };
                const auto announce_media=[&]()->Task<bool>{
                    if((player.styles&0x800) && player.owner){
                        const auto bytes=(player.styles&0x80)?utf16_to_utf8(player.file):std::string{};
                        const auto name=(player.styles&0x80)?scratch.allocate(bytes.size()+1):scratch.text(player.file);
                        if(name && (player.styles&0x80)){
                            m.write(GuestAddress{name},std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()));
                            m.store(GuestAddress{name+bytes.size()},8,0);
                        }
                        (void)co_await notify(0x800,1227,name);
                    }
                    co_return true;
                };
                const auto open_device=[&](std::u16string file,bool create_only,unsigned open_flags)->Task<unsigned>{
                    if(file.empty())co_return mci::missing_parameter;
                    if(const auto closing=co_await close_device())co_return closing;
                    auto lower=file;
                    for(auto& c:lower)c=unicode_case_unit(c,false);
                    const auto by_type=create_only || lower==u"waveaudio";
                    player.alias=numeric(hwnd);
                    const auto alias=scratch.text(player.alias),source=scratch.text(file);
                    if(!source || !alias)co_return mci::memory;
                    clean();
                    field((by_type?unit:2*unit)+4,ptr_bits,source);
                    field(3*unit+4,ptr_bits,alias);
                    const auto result=co_await send(mci::open,0x400U|(by_type?0x2000U:0x200U)|(open_flags&0x100U));
                    if(result){player.alias.clear();co_return result;}
                    player.device=static_cast<unsigned>(query(unit));
                    player.file=std::move(file);
                    if(const auto video=avi_device()){
                        clean();
                        field(unit,ptr_bits,hwnd);
                        (void)co_await send(mci::video_window,0x10000U);
                        if(!(player.styles&1)){
                            const auto width=std::max(1U,video->clip->width()*player.zoom/100);
                            const auto height=std::max(1U,video->clip->height()*player.zoom/100)+(player.styles&2?0U:28U);
                            (void)co_await api("user32.dll","SetWindowPos",{hwnd,0,0,0,width,height,0x16});
                        }
                    }
                    (void)co_await layout();
                    if(const auto track=gui_child(hwnd,track_bar)){
                        const auto length=co_await status(1);
                        (void)co_await api("user32.dll","SendMessageW",{track,0x408,1,length});
                    }
                    (void)co_await timer();
                    (void)co_await announce_media();
                    (void)co_await update(true);
                    co_return 0;
                };
                const auto open_dialog=[&]()->Task<unsigned>{
                    if(player.styles&0x8000)co_return mci::unsupported;
                    const unsigned size=x86_?88U:152U,flags=x86_?52U:96U,buffer=x86_?28U:48U;
                    const auto ofn=scratch.allocate(size),name=scratch.allocate(4096);
                    if(!ofn || !name)co_return mci::memory;
                    m.store(GuestAddress{ofn},32,size);
                    m.store(GuestAddress{ofn+(x86_?4:8)},ptr_bits,hwnd);
                    m.store(GuestAddress{ofn+buffer},ptr_bits,name);
                    m.store(GuestAddress{ofn+buffer+unit},32,2048);
                    m.store(GuestAddress{ofn+flags},32,0x81000);
                    if(!(co_await api("msvfw32.dll","GetOpenFileNamePreviewW",{ofn})))co_return 0;
                    co_return co_await open_device(text(name,true),false,0);
                };
                try {
                    if(message==1){
                        const auto instance=window->second.instance;
                        const auto child=[&](std::u16string_view cls,std::u16string_view caption,unsigned id)->Task<bool>{
                            const auto name=scratch.text(cls),label=scratch.text(caption);
                            if(!name || !label)co_return false;
                            co_return 0!=co_await api("user32.dll","CreateWindowExW",
                                {0,name,label,0x50010000,0,0,50,28,hwnd,id,instance,0});
                        };
                        (void)co_await child(u"button",u"播放",play_button);
                        (void)co_await child(u"button",u"開啟",menu_button);
                        (void)co_await child(u"msctls_trackbar32",u"",track_bar);
                        (void)co_await visibility(play_button,!(player.styles&2));
                        (void)co_await visibility(menu_button,!(player.styles&0x800a));
                        (void)co_await visibility(track_bar,!(player.styles&2));
                        (void)co_await layout();
                        if(lp){
                            const auto filename=m.load(GuestAddress{lp},ptr_bits);
                            if(filename)(void)co_await error(co_await open_device(text(filename,true),false,0));
                        }
                        co_return finish(0);
                    }
                    if(message==2){
                        (void)co_await close_device();
                        bridge.mci_windows.erase(hwnd);
                        co_return finish(0);
                    }
                    if(message==0x10){
                        (void)co_await api("user32.dll","DestroyWindow",{hwnd});
                        co_return finish(0);
                    }
                    if(message==0x113 && wp==clip_timer){
                        if(!player.busy)(void)co_await update(false);
                        co_return finish(0);
                    }
                    if(message==0x3b9){
                        if(wp==1 && lp==player.device && player.repeat && !avi_device()){
                            clean();
                            field(0,ptr_bits,hwnd);
                            (void)co_await error(co_await send(mci::play,mci::notify|mci::from));
                        }
                        (void)co_await update(true);
                        co_return finish(0);
                    }
                    if(message==0x86 || message==0x18){
                        if(message==0x86)player.active=wp!=0;
                        (void)co_await timer();
                        co_return finish(co_await def());
                    }
                    if(message==5){
                        (void)co_await layout();
                        (void)co_await paint();
                        (void)co_await notify(0x400,1226,lp);
                        co_return finish(0);
                    }
                    if(message==0xf && avi_device()){
                        const auto ps=scratch.allocate(x86_?64:72);
                        const auto dc=wp?wp:ps?co_await api("user32.dll","BeginPaint",{hwnd,ps}):0;
                        (void)co_await paint(dc);
                        if(!wp && dc)(void)co_await api("user32.dll","EndPaint",{hwnd,ps});
                        co_return finish(0);
                    }
                    if(message==0x111){
                        const auto id=wp&0xffffU;
                        if(id==menu_button)co_return finish(co_await error(co_await open_dialog()));
                        if(id!=play_button)co_return finish(co_await def());
                        message=(co_await status(4))==526?mci::stop:mci::play;
                    }
                    if(message==0x100 && wp==27)message=mci::stop;
                    if(message==wnd_open_a || message==wnd_open_w || message==wnd_new_a || message==wnd_new_w){
                        if(player.busy)co_return finish(mci::not_ready);
                        player.busy=true;
                        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{player.busy};
                        if(lp==(x86_?UINT32_MAX:UINT64_MAX))co_return finish(co_await error(co_await open_dialog()));
                        const bool create=message==wnd_new_a || message==wnd_new_w;
                        const bool wide=message==wnd_open_w || message==wnd_new_w;
                        co_return finish(co_await error(co_await open_device(text(lp,wide),create,static_cast<unsigned>(wp))));
                    }
                    if(message==mci::close){
                        const auto result=co_await close_device();
                        if(!result){(void)co_await announce_media();(void)co_await update(false);}
                        co_return finish(co_await error(result));
                    }
                    const bool wide_message=message>=1224;
                    if(message==wnd_device)co_return finish(player.device);
                    if(message==wnd_alias)co_return finish(player.device?hwnd&UINT32_MAX:0);
                    if(message==wnd_filename_a || message==wnd_filename_w){copy(player.file,wide_message);co_return finish(0);}
                    if(message==wnd_name_a || message==wnd_name_w){
                        copy(!player.device?u"":avi_device()?u"avivideo":u"waveaudio",wide_message);co_return finish(0);
                    }
                    if(message==wnd_error_a || message==wnd_error_w){
                        const auto result=player.error;
                        if(wp && lp)(void)co_await api("winmm.dll",wide_message?"mciGetErrorStringW":"mciGetErrorStringA",{result,lp,wp});
                        co_return finish(result);
                    }
                    if(message==wnd_reply_a || message==wnd_reply_w){copy(player.reply,wide_message);co_return finish(0);}
                    if(message==wnd_command_a || message==wnd_command_w){
                        const auto instruction=text(lp,wide_message);
                        if(instruction.empty() || !player.device)co_return finish(co_await error(mci::missing_parameter));
                        const auto split=instruction.find(u' ');
                        const auto command=split==instruction.npos?instruction+u" "+player.alias:
                            instruction.substr(0,split)+u" "+player.alias+instruction.substr(split);
                        const auto input=scratch.text(command),output=scratch.allocate(2048);
                        if(!input || !output)co_return finish(mci::memory);
                        const auto result=co_await api("winmm.dll","mciSendStringW",{input,output,1024,0});
                        if(!result){
                            player.reply=text(output,true);
                            (void)co_await update(false);
                        }
                        co_return finish(co_await error(static_cast<unsigned>(result)));
                    }
                    if(message==wnd_mode_a || message==wnd_mode_w){
                        const auto mode=co_await status(4);copy(get_mode_text(static_cast<unsigned>(mode)),wide_message);co_return finish(mode);
                    }
                    if(message==wnd_position_a || message==wnd_position_w || message==wnd_start || message==wnd_length || message==wnd_end){
                        const auto result=co_await status(message==wnd_length || message==wnd_end?1:2,message==wnd_start?0x200U:0U);
                        if(message==wnd_position_a || message==wnd_position_w)copy(numeric(result),wide_message);
                        co_return finish(result);
                    }
                    if(message==wnd_time_get_a || message==wnd_time_get_w){
                        const auto format=co_await status(6);
                        copy(format==3?u"frames":format==8?u"bytes":format==9?u"samples":u"milliseconds",wide_message);
                        co_return finish(format);
                    }
                    if(message==wnd_time_set_a || message==wnd_time_set_w){
                        auto format=text(lp,wide_message);
                        for(auto& c:format)c=unicode_case_unit(c,false);
                        const auto value=format==u"ms" || format==u"milliseconds"?0U:format==u"frames"?3U:
                            format==u"bytes"?8U:format==u"samples"?9U:UINT32_MAX;
                        if(value==UINT32_MAX)co_return finish(co_await error(mci::bad_time));
                        clean();
                        field(unit,32,value);
                        co_return finish(co_await error(co_await send(mci::set,0x400U)));
                    }
                    if(message>=wnd_can_play && message<=wnd_can_configure){
                        if(message==wnd_can_window)co_return finish(avi_device()!=nullptr);
                        if(message==wnd_can_configure)co_return finish(0);
                        const auto item=message==wnd_can_play?8:message==wnd_can_record?1:message==wnd_can_save?9:7;
                        co_return finish(co_await cap(static_cast<unsigned>(item)));
                    }
                    if(message==wnd_repeat_set){player.repeat=lp!=0;co_return finish(0);}
                    if(message==wnd_repeat_get)co_return finish(player.repeat);
                    if(message==wnd_styles_get)co_return finish(player.styles);
                    if(message==wnd_styles_set){
                        const auto mask=static_cast<unsigned>(wp)&0xffffU;
                        player.styles=(player.styles&~mask)|(static_cast<unsigned>(lp)&mask);
                        (void)co_await visibility(play_button,!(player.styles&2));
                        (void)co_await visibility(menu_button,!(player.styles&0x800a));
                        (void)co_await visibility(track_bar,!(player.styles&2));
                        (void)co_await layout();
                        co_return finish(0);
                    }
                    if(message==wnd_owner_set){
                        if(wp && !gui_.windows.contains(wp))co_return finish(co_await error(mci::bad_constant));
                        player.owner=wp;
                        co_return finish(0);
                    }
                    if(message==wnd_active_get || message==wnd_inactive_get)
                        co_return finish(message==wnd_active_get?player.active_timer:player.inactive_timer);
                    if(message==wnd_timers_set || message==wnd_active_set || message==wnd_inactive_set){
                        const auto rate=[](std::uint64_t value){return static_cast<unsigned>(std::clamp<std::uint64_t>(value,10,60000));};
                        if(message!=wnd_inactive_set)player.active_timer=rate(wp);
                        if(message!=wnd_active_set)player.inactive_timer=rate(message==wnd_timers_set?lp:wp);
                        (void)co_await timer();
                        co_return finish(0);
                    }
                    if(message==wnd_zoom_get)co_return finish(player.zoom);
                    if(message==wnd_zoom_set){
                        const auto video=avi_device();
                        if(!video || !lp || lp>1000)co_return finish(co_await error(mci::out_of_range));
                        player.zoom=static_cast<unsigned>(lp);
                        const auto w=std::max(1U,video->clip->width()*player.zoom/100);
                        const auto h=std::max(1U,video->clip->height()*player.zoom/100);
                        if(!(player.styles&1))
                            (void)co_await api("user32.dll","SetWindowPos",{hwnd,0,0,0,w,h+(player.styles&2?0U:28U),0x16});
                        else{
                            clean();
                            field(unit+8,32,w);
                            field(unit+12,32,h);
                            (void)co_await send(mci::video_put,0x50000U);
                        }
                        (void)co_await paint();
                        co_return finish(0);
                    }
                    if(message==wnd_speed_get || message==wnd_volume_get){
                        if(avi_device())co_return finish(co_await status(message==wnd_speed_get?0x4003U:0x4019U));
                        if(const auto single=mci_state().devices.find(player.device);single!=mci_state().devices.end()){
                            if(message==wnd_volume_get)co_return finish(static_cast<unsigned>((single->second->left+single->second->right)*500.F));
                            co_return finish(1000);
                        }
                        co_return finish(0);
                    }
                    if(message==wnd_speed_set || message==wnd_volume_set){
                        if(!avi_device() && message==wnd_volume_set && lp<=1000){
                            if(const auto single=mci_state().devices.find(player.device);single!=mci_state().devices.end()){
                                auto& wave=*single->second;
                                wave.left=wave.right=static_cast<float>(lp)/1000.F;
                                if(wave.buffer){std::lock_guard lock(wave.buffer->mutex);wave.buffer->left=wave.left;wave.buffer->right=wave.right;}
                                co_return finish(co_await error(0));
                            }
                        }
                        clean();
                        if(message==wnd_speed_set){
                            field(unit+12,32,lp);
                            co_return finish(co_await error(co_await send(mci::set,0x20000)));
                        }
                        field(unit,32,0x4002);
                        field(unit+4,32,lp);
                        co_return finish(co_await error(co_await send(mci::video_set_audio,0x1800000)));
                    }
                    if(message>=wnd_source_get && message<=wnd_dest_put){
                        if(!lp)co_return finish(co_await error(mci::missing_parameter));
                        const bool put=(message==wnd_source_put || message==wnd_dest_put);
                        const auto which=message<=wnd_source_put?0x20000U:0x40000U;
                        clean();
                        std::array<std::uint8_t,16> rect{};
                        if(put){m.read(GuestAddress{lp},rect);m.write(GuestAddress{block+unit},rect);}
                        const auto code=co_await send(put?mci::video_put:mci::video_where,which|(put?0x10000U:0U));
                        if(!code && !put){m.read(GuestAddress{block+unit},rect);m.write(GuestAddress{lp},rect);}
                        if(!code && put)(void)co_await paint();
                        co_return finish(co_await error(code));
                    }
                    if(message==wnd_palette_set){
                        if(wp && (co_await api("gdi32.dll","GetObjectType",{wp}))!=5)
                            co_return finish(co_await error(mci::bad_constant));
                        player.palette=wp;
                        co_return finish(0);
                    }
                    if(message==wnd_palette_get)co_return finish(player.palette);
                    if(message==wnd_realize || message==wnd_palette_kick){
                        const auto dc=co_await api("user32.dll","GetDC",{hwnd});
                        if(!dc)co_return finish(0);
                        std::uint64_t changed=0;
                        if(player.palette){
                            (void)co_await api("gdi32.dll","SelectPalette",{dc,player.palette,wp!=0});
                            changed=co_await api("gdi32.dll","RealizePalette",{dc});
                        }
                        (void)co_await api("user32.dll","ReleaseDC",{hwnd,dc});
                        co_return finish(changed);
                    }
                    if(message==wnd_validate){(void)co_await update(true);co_return finish(0);}
                    if(message==wnd_eject || message==wnd_interface_open)co_return finish(co_await error(mci::unsupported));
                    unsigned command=message,flags=0;
                    clean();
                    if(message==mci::seek || message==0x114){
                        auto position=static_cast<std::uint32_t>(lp);
                        if(message==0x114){
                            const auto track=gui_child(hwnd,track_bar);
                            if(!track)co_return finish(0);
                            position=static_cast<std::uint32_t>(co_await api("user32.dll","SendMessageW",{track,0x400,0,0}));
                        }
                        command=mci::seek;
                        flags=position==UINT32_MAX?0x100U:position==UINT32_MAX-1?0x200U:mci::to;
                        field(unit,32,position);
                    }else if(message==mci::video_step){
                        const auto amount=static_cast<std::int32_t>(lp);
                        flags=0x20000U|(amount<0?0x10000U:0U);
                        field(unit,32,static_cast<unsigned>(amount<0?-std::int64_t{amount}:amount));
                    }else if(message==mci::play || message==wnd_play_from || message==wnd_play_to || message==wnd_play_reverse){
                        command=mci::play;
                        if(message==wnd_play_reverse)flags|=mci::video_reverse;
                        if(player.repeat && avi_device())flags|=mci::video_repeat;
                        if(message==wnd_play_from){flags|=mci::from;field(unit,32,lp);}
                        if(message==wnd_play_to){flags|=mci::to;field(unit+4,32,lp);}
                        if(message==mci::play){
                            const auto position=co_await status(2),length=co_await status(1);
                            clean();
                            if(position==length){flags|=mci::from;field(unit,32,0);}
                        }
                        flags|=mci::notify;
                        field(0,ptr_bits,hwnd);
                    }else if(message==mci::save){
                        if(!lp || lp==(x86_?UINT32_MAX:UINT64_MAX))co_return finish(co_await error(mci::unsupported));
                        flags=0x100U;
                        field(unit,ptr_bits,lp);
                    }else if(message!=mci::stop && message!=mci::pause && message!=mci::resume && message!=mci::record)
                        co_return finish(co_await def());
                    const auto result=co_await send(command,flags);
                    if(!result)(void)co_await update(true);
                    co_return finish(co_await error(result));
                }catch(const MemoryFault&){player.error=mci::missing_parameter;co_return finish(player.error);}
                catch(const std::bad_alloc&){player.error=mci::memory;co_return finish(player.error);}
