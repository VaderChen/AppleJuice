    if(service.family=="internal-avi-vfw-choice"){
        const auto hwnd=call(0);
        const auto message=static_cast<unsigned>(call(1));
        const auto win=gui_.windows.find(hwnd);
        if(win==gui_.windows.end())return done(0);
        auto& window=win->second;
        const bool init=message==0x110;
        const auto found=store.compressor_dialogs.find(init?call(3):window.user_data);
        if(found==store.compressor_dialogs.end())return done(0);
        const auto dialog=found->second;
        const auto combo=gui_child(hwnd,vfw::choice_codec);
        if(!combo)return done(0);
        if(init){
            window.user_data=call(3);
            GuestScratch scratch(*this,memory,state);
            for(const auto& codec:dialog->choices){
                const auto label=scratch.text(codec.label);
                if(!label)return done(0);
                (void)gui_control_message(memory,{combo,0,label,0x143},true); // CB_ADDSTRING
            }
            (void)gui_control_message(memory,{combo,dialog->selected,0,0x14e},true); // CB_SETCURSEL
            const auto check=[&](unsigned id,bool checked){
                if(const auto child=gui_child(hwnd,id)){
                    gui_.windows.at(child).check=checked;
                    present_gui(child);
                }
            };
            check(vfw::choice_key_enable,dialog->key!=0);
            check(vfw::choice_rate_enable,dialog->rate!=0);
            if(const auto child=gui_child(hwnd,vfw::choice_preview);child && !dialog->stream){
                gui_set_window_enabled(gui_.windows.at(child),false);
                present_gui(child);
            }
            if(const auto child=gui_child(hwnd,vfw::choice_view)){
                auto& preview=gui_.windows.at(child);
                preview.visible=false;
                preview.style&=~0x10000000U;
                present_gui(child);
            }
            return done(1);
        }
        if(message!=0x111 && message!=0x10)return done(0);
        const auto id=message==0x10?2U:static_cast<unsigned>(call(2))&65535U;
        if(dialog->busy)return done(1);
        if(id==2){
            if(window.modal){window.ended=true;window.dialog_result=0;}
            return done(1);
        }
        if(id!=1 && id!=vfw::choice_preview && id!=vfw::choice_configure && id!=vfw::choice_about)return done(0);
        const auto selected=gui_control_message(memory,{combo,0,0,0x147},true).value_or(UINT64_MAX); // CB_GETCURSEL
        if(selected>=dialog->choices.size())return done(1);
        const auto number=[&](unsigned child_id,unsigned fallback,unsigned maximum)->std::optional<unsigned>{
            const auto child=gui_child(hwnd,child_id);
            if(!child || gui_.windows.at(child).title.empty())return fallback;
            std::uint64_t value=0;
            for(const auto c:gui_.windows.at(child).title){
                if(c<u'0' || c>u'9')return {};
                value=value*10+static_cast<unsigned>(c-u'0');
                if(value>maximum)return {};
            }
            return static_cast<unsigned>(value);
        };
        const auto checked=[&](unsigned child_id){
            const auto child=gui_child(hwnd,child_id);
            return child && gui_.windows.at(child).check;
        };
        const auto quality=number(vfw::choice_quality,UINT32_MAX,10000);
        if(!quality)return done(1);
        if(id==1){
            const auto key=checked(vfw::choice_key_enable)?number(vfw::choice_key,0,INT32_MAX):std::optional{0U};
            const auto rate=checked(vfw::choice_rate_enable)?number(vfw::choice_rate,0,INT32_MAX):std::optional{0U};
            if(!key || !rate)return done(1);
            dialog->selected=static_cast<unsigned>(selected);
            dialog->quality=*quality;
            dialog->key=*key;
            dialog->rate=*rate;
            if(window.modal){window.ended=true;window.dialog_result=1;}
            return done(1);
        }
        return nested_continuation(state,memory,"VFW 編碼器選項",
            [this,service,dialog,hwnd,id,hic=dialog->choices[selected].hic,quality=*quality]
            (GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                using service_callback::Task;
                const WindowsCall args(s,m,service);
                const auto finish=[&](){args.return_to(s,1);return std::optional<std::uint32_t>{};};
                const auto host=[&](std::string_view lib,std::string_view symbol,std::vector<std::uint64_t> values)
                    ->Task<std::uint64_t>{co_return co_await service_callback::call_value(resolve(lib,symbol).value,std::move(values));};
                dialog->busy=true;
                struct Clear {bool& busy;~Clear(){busy=false;}} clear{dialog->busy};
                if(id==vfw::choice_configure || id==vfw::choice_about){
                    const auto msg=id==vfw::choice_configure?0x500aU:0x500bU;
                    const auto all_bits=x86_?UINT32_MAX:UINT64_MAX;
                    if(!(co_await host("msvfw32.dll","ICSendMessage",{hic,msg,all_bits,0})))
                        (void)co_await host("msvfw32.dll","ICSendMessage",{hic,msg,hwnd,0});
                    co_return finish();
                }
                const auto view=gui_child(hwnd,vfw::choice_view);
                if(!view || !dialog->stream)co_return finish();
                GuestScratch scratch(*this,m,s);
                const auto status_text=[&](std::u16string_view text)->Task<bool>{
                    const auto ptr=scratch.text(text);
                    if(ptr)(void)co_await host("user32.dll","SetWindowTextW",{view,ptr});
                    co_return true;
                };
                if(!dialog->preview_visible){
                    const auto width=gui_.windows.at(hwnd).width;
                    const auto height=gui_.windows.at(hwnd).height+gui_.windows.at(view).height+12;
                    (void)co_await host("user32.dll","SetWindowPos",{hwnd,0,0,0,width,height,0x16});
                    (void)co_await host("user32.dll","ShowWindow",{view,5});
                    dialog->preview_visible=true;
                }
                const auto ptr_bits=static_cast<std::uint8_t>(x86_?32:64);
                const auto method=[&](std::uint64_t obj,unsigned slot){
                    return m.load(GuestAddress{m.load(GuestAddress{obj},ptr_bits)+slot*(ptr_bits/8U)},ptr_bits);
                };
                const auto info=[&](std::uint64_t address){
                    avi::Bytes header(40);
                    m.read(GuestAddress{address},header);
                    return avi::frame_header_size(header);
                };
                std::uint64_t getter=0,packed=0,decoded=0,dc=0,release=0;
                bool locked_pack=false,locked_decoded=false;
                const auto paint=[&]()->Task<bool>{
                    const auto start=co_await host("avifil32.dll","AVIStreamStart",{dialog->stream});
                    getter=co_await host("avifil32.dll","AVIStreamGetFrameOpen",{dialog->stream,0});
                    if(!getter)co_return false;
                    release=method(getter,2);
                    const auto frame=co_await service_callback::call_value(method(getter,3),{getter,start});
                    if(!frame)co_return false;
                    const auto header=info(frame);
                    if(!header)co_return false;
                    packed=co_await host("msvfw32.dll","ICImageCompress",{hic,0,frame,frame+header,0,quality,0});
                    if(!packed)co_return false;
                    const auto compressed=co_await host("kernel32.dll","GlobalLock",{packed});
                    if(!compressed)co_return false;
                    locked_pack=true;
                    const auto raw_offset=info(compressed);
                    if(!raw_offset)co_return false;
                    decoded=co_await host("msvfw32.dll","ICImageDecompress",{0,0,compressed,compressed+raw_offset,0});
                    if(!decoded)co_return false;
                    const auto dib=co_await host("kernel32.dll","GlobalLock",{decoded});
                    if(!dib)co_return false;
                    locked_decoded=true;
                    const auto bits=info(dib);
                    if(!bits)co_return false;
                    const auto width=static_cast<unsigned>(m.load(GuestAddress{dib+4},32));
                    const auto signed_height=static_cast<std::int32_t>(m.load(GuestAddress{dib+8},32));
                    if(!width || width>16384 || !signed_height || signed_height==INT32_MIN)co_return false;
                    const auto height=static_cast<unsigned>(signed_height<0?-signed_height:signed_height);
                    if(height>16384)co_return false;
                    const auto target=gui_.windows.find(view);
                    if(target==gui_.windows.end())co_return false;
                    const auto w=target->second.width,h=target->second.height;
                    if(!w || !h)co_return false;
                    const auto out_w=std::min(w,std::max(1U,h*width/height));
                    const auto out_h=std::min(h,std::max(1U,w*height/width));
                    (void)co_await status_text(u"");
                    dc=co_await host("user32.dll","GetDC",{view});
                    if(!dc)co_return false;
                    const auto painted=co_await host("gdi32.dll","StretchDIBits",{dc,(w-out_w)/2,(h-out_h)/2,
                        out_w,out_h,0,0,width,height,dib+bits,dib,0,0x00cc0020});
                    co_return painted && painted!=UINT32_MAX;
                };
                bool okay=false;
                try{okay=co_await paint();}
                catch(const MemoryFault&){}
                catch(const std::bad_alloc&){}
                if(dc){
                    (void)flush_gdi_batch(m);
                    (void)co_await host("user32.dll","ReleaseDC",{view,dc});
                }
                if(locked_decoded)(void)co_await host("kernel32.dll","GlobalUnlock",{decoded});
                if(decoded)(void)co_await host("kernel32.dll","GlobalFree",{decoded});
                if(locked_pack)(void)co_await host("kernel32.dll","GlobalUnlock",{packed});
                if(packed)(void)co_await host("kernel32.dll","GlobalFree",{packed});
                if(release)(void)co_await service_callback::call_value(release,{getter});
                if(!okay)(void)co_await status_text(u"無法預覽此壓縮格式");
                co_return finish();
            });
    }
