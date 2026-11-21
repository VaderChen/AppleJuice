                    if(drawdib){
                        const auto integer=[&](unsigned n){return static_cast<std::int32_t>(word_arg(n));};
                        const auto gdi=[&](std::string_view function,std::vector<std::uint64_t> params){
                            return nested_service(s,m,"gdi32.dll",function,std::move(params));
                        };
                        const auto signed_arg=[](int n){return static_cast<std::uint32_t>(n);};
                        if(name=="StretchDIB"){
                            const auto dest=vfw::stretch_layout(m,args(0));
                            const auto src=vfw::stretch_layout(m,args(6));
                            if(dest && src)
                                vfw::stretch_dib(m,*dest,args(1),{integer(2),integer(3),integer(4),integer(5)},
                                                 *src,args(7),{integer(8),integer(9),integer(10),integer(11)});
                            co_return finish(0);
                        }
                        if(name=="DrawDibOpen"){
                            const auto id=runtime_.allocate_wait_handle();
                            store.drawdibs.emplace(id,std::make_shared<vfw::DrawDib>());
                            co_return finish(id);
                        }
                        if(name=="DrawDibProfileDisplay"){
                            const auto input=header_at(args(0));
                            if(input.empty() || avi::word(input,8)>INT32_MAX || !avi::word(input,8))co_return finish(0);
                            const auto format=avi::frame_layout(input);
                            if(format && format->raw())co_return finish(3); // PD_CAN_DRAW_DIB | PD_CAN_STRETCHDIB
                            const auto hic=co_await open(vfw::video_type,avi::word(input,16),2,args(0),0);
                            if(!hic)co_return finish(0);
                            (void)co_await close(hic);
                            co_return finish(3);
                        }
                        const auto found=store.drawdibs.find(args(0));
                        if(found==store.drawdibs.end() || found->second->busy)co_return finish(0);
                        const auto entry=found->second;
                        auto& dib=*entry;
                        dib.busy=true;
                        struct ClearDraw {bool& busy;~ClearDraw(){busy=false;}} clear{dib.busy};
                        const auto cleanup=[&](vfw::DrawDib& resources)->Task<bool>{
                            if(resources.memory_dc)(void)gdi("DeleteDC",{resources.memory_dc});
                            if(resources.bitmap)(void)gdi("DeleteObject",{resources.bitmap});
                            resources.memory_dc=resources.bitmap=resources.pixels=resources.dc=0;
                            resources.has_frame=false;
                            resources.input.clear();
                            resources.output.clear();
                            if(const auto hic=std::exchange(resources.codec,0)){
                                (void)co_await send(hic,decompress_end,0,0);
                                (void)co_await close(hic);
                            }
                            co_return true;
                        };
                        const auto init=[&](std::uint64_t dc,std::uint64_t input_address,avi::Bytes input,
                                            const std::array<std::int32_t,4>& sizes,unsigned flags)->Task<bool>{
                            if(!dc || input.empty() || !avi::word(input,8) || avi::word(input,8)>INT32_MAX ||
                               std::any_of(sizes.begin(),sizes.end(),[](int n){return n<=0 && n!=-1;}))
                                co_return false;
                            const auto dc_type=gdi("GetObjectType",{dc});
                            if(dc_type!=3 && dc_type!=10 && dc_type!=12)co_return false;
                            if(dib.dc==dc && dib.sizes==sizes && vfw::same_draw_header(dib.input,input))co_return true;
                            vfw::DrawDib next;
                            next.input=std::move(input);
                            next.dc=dc;
                            next.sizes=sizes;
                            next.flags=flags;
                            const auto setup=[&]()->Task<bool>{
                                const auto src=avi::frame_layout(next.input);
                                if(src && src->raw())next.output=vfw::gdi_draw_header(avi::frame_output(next.input,*src,{}));
                                else{
                                    if(flags&vfw::draw_direct)co_return false;
                                    next.codec=co_await open(vfw::video_type,avi::word(next.input,16),2,input_address,0);
                                    if(!next.codec)co_return false;
                                    const auto count=co_await send(next.codec,decompress_format,input_address,0);
                                    if(count<40 || count>8192)co_return false;
                                    avi::Bytes format(count);
                                    const auto pointer=guest_bytes(format);
                                    if(co_await send(next.codec,decompress_format,input_address,pointer))co_return false;
                                    m.read(GuestAddress{pointer},format);
                                    const auto used=avi::frame_header_size(format);
                                    if(!used || used>format.size())co_return false;
                                    format.resize(used);
                                    next.output=vfw::gdi_draw_header(std::move(format));
                                }
                                const auto out=avi::frame_layout(next.output);
                                if(!out || !out->raw() || out->top_down)co_return false;
                                avi::put(next.output,20,out->image_size());
                                if(flags&vfw::draw_direct)co_return true;
                                const auto output=guest_bytes(next.output);
                                const auto pixels=guest_bytes(avi::Bytes(unit));
                                next.memory_dc=gdi("CreateCompatibleDC",{dc});
                                if(!next.memory_dc)co_return false;
                                next.bitmap=gdi("CreateDIBSection",{next.memory_dc,output,0,pixels,0,0});
                                if(!next.bitmap || !gdi("SelectObject",{next.memory_dc,next.bitmap}))co_return false;
                                next.pixels=m.load(GuestAddress{pixels},ptr_bits);
                                co_return next.pixels && (!next.codec ||
                                    !(co_await send(next.codec,decompress_begin,input_address,output)));
                            };
                            bool ready=false;
                            try{ready=co_await setup();}
                            catch(const MemoryFault&){}
                            catch(const std::bad_alloc&){}
                            if(!ready){
                                (void)co_await cleanup(next);
                                co_return false;
                            }
                            std::swap(next.dc,dib.dc);
                            std::swap(next.memory_dc,dib.memory_dc);
                            std::swap(next.bitmap,dib.bitmap);
                            std::swap(next.pixels,dib.pixels);
                            std::swap(next.codec,dib.codec);
                            dib.input=std::move(next.input);
                            dib.output=std::move(next.output);
                            dib.sizes=sizes;
                            dib.flags=flags;
                            dib.has_frame=false;
                            (void)co_await cleanup(next);
                            co_return true;
                        };
                        const auto realize=[&](std::uint64_t dc,bool background)->std::uint64_t{
                            if(!dc)return 0;
                            const auto selected=dib.palette?dib.palette:gdi("GetStockObject",{15});
                            if(!selected || !gdi("SelectPalette",{dc,selected,background}))return 0;
                            return gdi("RealizePalette",{dc});
                        };
                        if(name=="DrawDibClose"){
                            store.drawdibs.erase(found);
                            co_return finish(co_await cleanup(dib));
                        }
                        if(name=="DrawDibEnd" || name=="DrawDibStop"){
                            const bool active=dib.dc || dib.playback_us;
                            dib.flags=dib.playback_us=0;
                            (void)co_await cleanup(dib);
                            co_return finish(active);
                        }
                        if(name=="DrawDibStart"){
                            if(!word_arg(1))co_return finish(0);
                            dib.playback_us=word_arg(1);
                            co_return finish(1);
                        }
                        if(name=="DrawDibBegin"){
                            co_return finish(co_await init(args(1),args(4),header_at(args(4)),
                                {integer(2),integer(3),integer(5),integer(6)},word_arg(7)));
                        }
                        if(name=="DrawDibGetBuffer"){
                            if(!dib.pixels || word_arg(3))co_return finish(0);
                            if(args(1)){
                                if(word_arg(2)<dib.output.size())co_return finish(0);
                                m.write(GuestAddress{args(1)},dib.output);
                            }
                            co_return finish(dib.pixels);
                        }
                        if(name=="DrawDibGetPalette")co_return finish(dib.palette);
                        if(name=="DrawDibSetPalette"){
                            if(args(1) && gdi("GetObjectType",{args(1)})!=5)co_return finish(0);
                            dib.palette=args(1);
                            if(dib.dc)(void)realize(dib.dc,false);
                            co_return finish(1);
                        }
                        if(name=="DrawDibRealize")co_return finish(realize(args(1),word_arg(2)!=0));
                        if(name=="DrawDibChangePalette"){
                            const auto first=integer(1),count=integer(2);
                            if(first<0 || count<0 || first>256 || count>256-first || (count && !args(3)))co_return finish(0);
                            const auto output=avi::frame_layout(dib.output);
                            const bool table=output && !output->palette.empty() &&
                                static_cast<unsigned>(first+count)<=output->palette.size();
                            if(!table && !dib.palette)co_return finish(0);
                            if(!count)co_return finish(1);
                            avi::Bytes entries(static_cast<std::size_t>(count)*4);
                            m.read(GuestAddress{args(3)},entries);
                            if(dib.palette && gdi(dib.flags&vfw::draw_animate?"AnimatePalette":"SetPaletteEntries",
                                                  {dib.palette,word_arg(1),word_arg(2),args(3)})!=(dib.flags&vfw::draw_animate?1U:word_arg(2)))
                                co_return finish(0);
                            if(table){
                                for(std::size_t at=0;at<entries.size();at+=4){
                                    std::swap(entries[at],entries[at+2]);
                                    entries[at+3]=0;
                                }
                                if(dib.memory_dc && gdi("SetDIBColorTable",
                                    {dib.memory_dc,word_arg(1),word_arg(2),guest_bytes(entries)})!=word_arg(2))co_return finish(0);
                                std::copy(entries.begin(),entries.end(),dib.output.begin()+avi::word(dib.output,0)+first*4);
                            }
                            co_return finish(1);
                        }
                        if(name=="DrawDibTime"){
                            if(!args(1))co_return finish(0);
                            avi::Bytes data(28);
                            for(unsigned n=0;n<dib.timing.size();++n)
                                avi::put(data,n*4,static_cast<unsigned>(std::min<std::uint64_t>(dib.timing[n],INT32_MAX)));
                            m.write(GuestAddress{args(1)},data);
                            dib.timing={};
                            co_return finish(1);
                        }
                        const auto flags=word_arg(12);
                        const bool update=flags&vfw::draw_update;
                        const auto dc=(flags&vfw::draw_same_dc) || (update && !args(1))?dib.dc:args(1);
                        const auto sizes=update || (flags&vfw::draw_same_format) ?
                            dib.sizes:std::array{integer(4),integer(5),integer(10),integer(11)};
                        const auto header=args(6);
                        if(update){
                            if(!dib.has_frame)co_return finish(0);
                        }else{
                            auto input=header_at(header);
                            if(!header || input.empty())co_return finish(0);
                            if(!dib.dc || (!(flags&vfw::draw_same_format) &&
                                (!vfw::same_draw_header(dib.input,input) || dib.sizes!=sizes))){
                                if(!(co_await init(dc,header,input,sizes,flags)))co_return finish(0);
                            }
                            dib.input=std::move(input);
                        }
                        if(!dc)co_return finish(0);
                        auto sw=sizes[2],sh=sizes[3],dw=sizes[0],dh=sizes[1];
                        if(sw==-1)sw=static_cast<int>(avi::word(dib.output,4));
                        if(sh==-1)sh=static_cast<int>(avi::word(dib.output,8));
                        if(dw==-1)dw=sw;
                        if(dh==-1)dh=sh;
                        const auto x=integer(2),y=integer(3);
                        const auto sx=update?dib.origin[0]:integer(8),sy=update?dib.origin[1]:integer(9);
                        if(!vfw::draw_rectangle(x,y,dw,dh) || !vfw::draw_rectangle(sx,sy,sw,sh))co_return finish(0);
                        const auto destination=avi::frame_layout(dib.output);
                        if(!destination || sx<0 || sy<0 || std::int64_t{sx}+sw>destination->width ||
                           std::int64_t{sy}+sh>destination->height)co_return finish(0);
                        const auto pixels=args(7)?args(7):header+dib.input.size();
                        const bool hidden=(flags&vfw::draw_hidden) || (!update && (dib.flags&vfw::draw_hidden));
                        const bool direct=dib.flags&vfw::draw_direct;
                        if(direct && (update || sw!=dw || sh!=dh))co_return finish(0);
                        vfw::DrawTimer all{vfw::DrawTimer::Clock::now(),dib.timing[1]};
                        if(!update && !direct){
                            if(dib.codec){
                                const unsigned codec_flags=(flags&vfw::draw_non_key?0x08000000U:0U) |
                                    (hidden?0x20000000U:0U) |
                                    (flags&vfw::draw_hurry?0x80000000U:0U);
                                vfw::DrawTimer decode{vfw::DrawTimer::Clock::now(),dib.timing[2]};
                                dib.has_frame=false;
                                if(co_await decompress(dib.codec,codec_flags,header,pixels,guest_bytes(dib.output),dib.pixels))
                                    co_return finish(0);
                            }else{
                                vfw::DrawTimer copy{vfw::DrawTimer::Clock::now(),dib.timing[6]};
                                avi::Bytes bitmap(destination->image_size());
                                m.read(GuestAddress{pixels},bitmap);
                                m.write(GuestAddress{dib.pixels},bitmap);
                            }
                            dib.has_frame=true;
                            dib.origin={sx,sy};
                        }
                        dib.dc=dc;
                        if(!direct && (hidden || (flags&vfw::draw_hurry))){
                            ++dib.timing[0];
                            co_return finish(1);
                        }
                        if(dib.palette && !(flags&vfw::draw_same_dc))
                            (void)realize(dc,((flags|dib.flags)&vfw::draw_background)!=0);
                        const bool stretch=dw!=sw || dh!=sh;
                        vfw::DrawTimer blit{vfw::DrawTimer::Clock::now(),dib.timing[stretch?4:5]};
                        bool painted=false;
                        const auto paint_dib=[&](std::uint64_t bits,std::uint64_t header_address){
                            const auto lines=gdi("StretchDIBits",{dc,signed_arg(x),signed_arg(y),signed_arg(dw),signed_arg(dh),
                                signed_arg(sx),signed_arg(static_cast<int>(destination->height)-sy-sh),
                                signed_arg(sw),signed_arg(sh),bits,header_address,0,0xcc0020});
                            return lines && lines!=UINT32_MAX;
                        };
                        if(direct){
                            painted=paint_dib(pixels,guest_bytes(dib.output));
                        }else if(dib.flags&vfw::draw_halftone){
                            std::uint64_t info=0,bits=0;
                            {
                                vfw::DrawTimer dither{vfw::DrawTimer::Clock::now(),dib.timing[3]};
                                auto paletted=vfw::halftone_dib(m,dib.pixels,*destination);
                                if(paletted.first.empty())co_return finish(0);
                                info=guest_bytes(paletted.first);
                                bits=guest_bytes(paletted.second);
                            }
                            painted=paint_dib(bits,info);
                        }else{
                            painted=gdi("StretchBlt",{dc,signed_arg(x),signed_arg(y),signed_arg(dw),signed_arg(dh),
                                dib.memory_dc,signed_arg(sx),signed_arg(sy),signed_arg(sw),signed_arg(sh),0xcc0020})!=0;
                        }
                        if(painted)++dib.timing[0];
                        co_return finish(painted);
                    }
