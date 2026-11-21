                    if((message>=vfw::draw_begin_message && message<=vfw::draw_end_message && message!=0x4014) ||
                       (message>=vfw::draw_query_message && message<=vfw::draw_buffers_message) ||
                       message==vfw::draw_suggest_message || message==vfw::draw_change_palette_message){
                        const auto result=vfw::deliver_error;
                        auto& draw=codec->drawing;
                        codec->busy=true;
                        struct ClearRenderer {bool& busy;~ClearRenderer(){busy=false;}} clear{codec->busy};
                        const auto display=[&](std::string_view fn,std::vector<std::uint64_t> values)->Task<std::uint64_t>{
                            co_return co_await api("msvfw32.dll",fn,std::move(values));
                        };
                        const auto dword=[&](unsigned offset){
                            return static_cast<std::uint32_t>(m.load(GuestAddress{first+offset},32));
                        };
                        const auto pointer=[&](unsigned offset){return m.load(GuestAddress{first+offset},ptr_bits);};
                        const auto format_ok=[&](const std::optional<avi::FrameLayout>& layout){
                            return layout && !layout->top_down && vfw::accepts(backend,*layout);
                        };
                        if(message==vfw::draw_query_message)
                            co_return result(format_ok(avi::frame_layout(header_at(first)))?vfw::ok:vfw::bad_format);
                        if(message==vfw::draw_suggest_message){
                            if(!first || second<3*unit+16)co_return result(vfw::bad_parameter);
                            const auto header=header_at(pointer(0));
                            const auto layout=avi::frame_layout(header);
                            if(!format_ok(layout))co_return result(vfw::bad_format);
                            const auto suggested=avi::frame_output(header,*layout,{});
                            if(suggested.empty())co_return result(vfw::bad_format);
                            const auto output=pointer(unit);
                            if(!output)co_return suggested.size();
                            m.write(GuestAddress{output},suggested);
                            co_return 0;
                        }
                        if(message==vfw::draw_begin_message){
                            if(!first || second<vfw::draw_begin_size(unit))co_return result(vfw::bad_parameter);
                            const auto flags=dword(0);
                            const auto header=pointer(4*unit+16);
                            auto input=header_at(header);
                            const auto layout=avi::frame_layout(input);
                            if(!format_ok(layout))co_return result(vfw::bad_format);
                            if((flags&vfw::renderer_fullscreen) && (flags&vfw::renderer_hdc))
                                co_return result(vfw::bad_flags);
                            if(flags&vfw::renderer_fullscreen)co_return result(vfw::unsupported);
                            if(flags&vfw::renderer_query)co_return 0;
                            vfw::DrawChannel next;
                            next.format=std::move(input);
                            next.flags=flags;
                            next.dc=pointer(3*unit);
                            const auto window=pointer(2*unit),palette=pointer(unit);
                            for(unsigned n=0;n<4;++n){
                                next.dest[n]=dword(4*unit+n*4);
                                next.source[n]=dword(5*unit+16+n*4);
                            }
                            next.rate=dword(5*unit+32);
                            next.scale=dword(5*unit+36);
                            if(!next.scale && next.rate)co_return result(vfw::bad_parameter);
                            if(!next.dc && !window)co_return result(vfw::bad_parameter);
                            if((flags&vfw::renderer_continue) && draw.surface &&
                               (next.dc?next.dc==draw.dc:window==draw.owner_window) &&
                               next.dest[2]==draw.dest[2] && next.dest[3]==draw.dest[3] &&
                               next.source[2]==draw.source[2] && next.source[3]==draw.source[3] &&
                               vfw::same_draw_header(draw.format,next.format)){
                                if(!(co_await display("DrawDibSetPalette",{draw.surface,palette})))
                                    co_return result(vfw::bad_parameter);
                                draw.dest=next.dest;
                                draw.source=next.source;
                                draw.sample=vfw::draw_sample(draw,runtime_.clock_ticks);
                                draw.ticks=runtime_.clock_ticks;
                                draw.rate=next.rate;
                                draw.scale=next.scale;
                                draw.flags=next.flags;
                                co_return 0;
                            }
                            const auto prepare=[&]()->Task<bool>{
                                if(!next.dc){
                                    next.dc=co_await api("user32.dll","GetDC",{window});
                                    if(!next.dc)co_return false;
                                    next.owner_window=window;
                                }
                                next.surface=co_await display("DrawDibOpen",{});
                                if(!next.surface)co_return false;
                                if(palette && !(co_await display("DrawDibSetPalette",{next.surface,palette})))co_return false;
                                const auto begin_flags=flags&vfw::renderer_animate?vfw::draw_animate:0;
                                co_return 0!=co_await display("DrawDibBegin",{next.surface,next.dc,next.dest[2],next.dest[3],
                                    header,next.source[2],next.source[3],begin_flags});
                            };
                            bool ready=false;
                            try{ready=co_await prepare();}
                            catch(const MemoryFault&){}
                            catch(const std::bad_alloc&){}
                            if(!ready){
                                (void)co_await release_draw(next);
                                co_return result(vfw::bad_format);
                            }
                            std::swap(draw,next);
                            (void)co_await release_draw(next);
                            co_return 0;
                        }
                        if(message==vfw::draw_buffers_message){
                            if(!first)co_return result(vfw::bad_parameter);
                            m.store(GuestAddress{first},32,draw.flags&vfw::renderer_render?1:0);
                            co_return 0;
                        }
                        if(!draw.surface)co_return result(vfw::bad_parameter);
                        const auto paint=[&](std::uint64_t header,std::uint64_t data,unsigned flags)->Task<bool>{
                            co_return 0!=co_await display("DrawDibDraw",{draw.surface,draw.dc,draw.dest[0],draw.dest[1],
                                draw.dest[2],draw.dest[3],header,data,draw.source[0],draw.source[1],draw.source[2],
                                draw.source[3],flags|vfw::draw_same_format|vfw::draw_same_dc});
                        };
                        const auto render=[&](bool queued_only)->Task<std::uint64_t>{
                            if(queued_only && !draw.pending)co_return 0;
                            if(!(co_await paint(0,0,vfw::draw_update)))co_return result(vfw::bad_format);
                            draw.pending=false;
                            co_return 0;
                        };
                        switch(message){
                        case vfw::draw_end_message:
                            (void)co_await release_draw(draw);
                            co_return 0;
                        case vfw::draw_start_message:
                            draw.ticks=runtime_.clock_ticks;
                            draw.running=true;
                            co_return 0;
                        case vfw::draw_stop_message:
                        case vfw::draw_stop_play_message:
                            draw.sample=vfw::draw_sample(draw,runtime_.clock_ticks);
                            draw.running=false;
                            if(message==vfw::draw_stop_play_message)draw.play_range=false;
                            co_return 0;
                        case vfw::draw_start_play_message: {
                            const auto start=static_cast<std::int32_t>(first),stop=static_cast<std::int32_t>(second);
                            if(stop<start)co_return result(vfw::bad_parameter);
                            draw.sample=start;
                            draw.to=stop;
                            draw.play_range=true;
                            draw.running=true;
                            draw.ticks=runtime_.clock_ticks;
                            co_return 0;
                        }
                        case vfw::draw_get_time_message:
                            if(!first)co_return result(vfw::bad_parameter);
                            m.store(GuestAddress{first},32,static_cast<std::uint32_t>(vfw::draw_sample(draw,runtime_.clock_ticks)));
                            co_return 0;
                        case vfw::draw_set_time_message:
                            draw.sample=static_cast<std::int32_t>(first);
                            draw.ticks=runtime_.clock_ticks;
                            co_return 0;
                        case vfw::draw_palette_message:
                            co_return co_await display("DrawDibGetPalette",{draw.surface});
                        case vfw::draw_realize_message:
                            co_return co_await display("DrawDibRealize",{draw.surface,first?first:draw.dc,second});
                        case vfw::draw_render_message:
                        case vfw::draw_flush_message:
                            co_return co_await render(true);
                        case vfw::draw_update_message:
                            co_return co_await render(false);
                        case vfw::draw_window_message:
                            if(!first || second<16)co_return result(vfw::bad_parameter);
                            m.check(GuestAddress{first},16,Access::Read);
                            if(static_cast<std::int32_t>(dword(8))<=static_cast<std::int32_t>(dword(0)) ||
                               static_cast<std::int32_t>(dword(12))<=static_cast<std::int32_t>(dword(4)))co_return 0;
                            if(const auto active=store.drawdibs.find(draw.surface);
                               active!=store.drawdibs.end() && active->second->has_frame)co_return co_await render(false);
                            co_return 0;
                        case vfw::draw_change_palette_message: {
                            const auto input=avi::frame_layout(header_at(first));
                            if(!input || input->palette.empty())co_return result(vfw::bad_format);
                            avi::Bytes entries(input->palette.size()*4);
                            for(std::size_t i=0;i<input->palette.size();++i)avi::put(entries,i*4,input->palette[i]|0x01000000U);
                            const auto address=guest_bytes(entries);
                            co_return result(co_await display("DrawDibChangePalette",{draw.surface,0,input->palette.size(),address})?
                                             vfw::ok:vfw::bad_format);
                        }
                        default: break;
                        }
                        if(!first || second<vfw::draw_frame_size(unit))co_return result(vfw::bad_parameter);
                        const auto flags=dword(0);
                        const auto data=pointer(2*unit);
                        if((flags&vfw::renderer_nullframe) || ((flags&vfw::renderer_update) && !data))
                            co_return co_await render(false);
                        const auto packet_size=dword(3*unit);
                        if(!data || !packet_size || packet_size>memory_limit)co_return result(vfw::bad_parameter);
                        const auto header=pointer(unit);
                        auto input=header?header_at(header):draw.format;
                        const auto workflow=avi::frame_layout(input);
                        const auto original=avi::frame_layout(draw.format);
                        if(!format_ok(workflow) || !original || workflow->width!=original->width ||
                           workflow->height!=original->height || workflow->depth!=original->depth ||
                           workflow->compression!=original->compression || workflow->masks!=original->masks ||
                           workflow->palette.size()!=original->palette.size())co_return result(vfw::bad_format);
                        if(workflow->raw() && packet_size<workflow->image_size())co_return result(vfw::bad_size);
                        avi::put(input,20,packet_size);
                        const bool waiting=(draw.flags&vfw::renderer_render) || (flags&(vfw::renderer_preroll|vfw::renderer_hurry));
                        const auto paint_flags=(flags&vfw::renderer_non_key?vfw::draw_non_key:0) |
                            (waiting?vfw::draw_hidden:0) | (flags&vfw::renderer_hurry?vfw::draw_hurry:0);
                        if(!(co_await paint(guest_bytes(input),data,paint_flags)))co_return result(vfw::bad_format);
                        draw.pending=waiting && !(flags&vfw::renderer_preroll);
                        if(!draw.running)draw.sample=static_cast<std::int32_t>(dword(3*unit+4));
                        co_return 0;
                    }
