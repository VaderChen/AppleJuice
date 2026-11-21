    if(name=="AVIStreamGetFrameOpen" || service.family=="internal-avi-frame"){
        return nested_continuation(state,memory,"AVI 影格",[this,service](GuestState& s,GuestMemory& m)
            ->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall args(s,m,service);
            const auto op=service.symbol;
            const auto finish=[&](std::uint64_t value){args.return_to(s,value);return std::optional<std::uint32_t>{};};
            const unsigned step=x86_?4U:8U;
            const auto pointer_bits=static_cast<std::uint8_t>(step*8);
            auto load=[&](std::uint64_t p){return m.load(GuestAddress{p},pointer_bits);};
            auto store_ptr=[&](std::uint64_t p,std::uint64_t v){m.store(GuestAddress{p},pointer_bits,v);};
            auto slot=[&](std::uint64_t p,unsigned index){return load(load(p)+index*step);};
            auto failed_hr=[](std::uint64_t hr){return static_cast<std::int32_t>(hr)<0;};
            auto& bridge=*avi_disk_;
            const auto owner=args(0);
            if(!owner)co_return finish(0);
            GuestScratch scratch(*this,m,s);
            if(op=="AVIStreamGetFrameOpen"){
                const auto temp=scratch.allocate(32);
                if(!temp)co_return finish(0);
                m.write(GuestAddress{temp+16},ole_standard_guid(0x20023));
                const auto hr=co_await service_callback::call(slot(owner,0),{owner,temp+16,temp});
                auto getter=load(temp);
                if(failed_hr(hr) || !getter){
                    if(!bridge.frame_table){
                        std::vector<std::uint64_t> slots;
                        for(const auto& d:avifil32_service_descriptors())
                            if(d.family=="internal-avi-frame")slots.push_back(resolve(d.family,d.symbol).value);
                        bridge.frame_table=runtime_allocate(m,{},slots.size()*step);
                        for(unsigned i=0;i<slots.size();++i)store_ptr(bridge.frame_table+i*step,slots[i]);
                    }
                    getter=allocate_heap(m,step);
                    if(!getter)co_return finish(0);
                    store_ptr(getter,bridge.frame_table);
                    auto frame=std::make_shared<AviDiskState::Frame>();
                    frame->stream=owner;bridge.frames.emplace(getter,frame);
                    (void)co_await service_callback::call(slot(owner,1),{owner});
                }
                const auto configured=co_await service_callback::call(slot(getter,6),{getter,args(1),0,0,0,UINT32_MAX,UINT32_MAX});
                if(failed_hr(configured)){
                    (void)co_await service_callback::call(slot(getter,2),{getter});
                    co_return finish(0);
                }
                co_return finish(getter);
            }
            const auto it=bridge.frames.find(owner);
            if(it==bridge.frames.end())co_return finish(op=="GetFrame"?0:bad_handle);
            const auto frame=it->second;
            const auto stream=frame->stream;
            if(op=="AddRef")co_return finish(++frame->references);
            if(op=="Release"){
                if(--frame->references)co_return finish(frame->references);
                bridge.frames.erase(it);
                if(frame->buffer)free_heap(m,frame->buffer,s);
                free_heap(m,owner,s);
                (void)co_await service_callback::call(slot(stream,2),{stream});
                co_return finish(0);
            }
            if(op=="QueryInterface"){
                if(!args(2) || !args(1))co_return finish(pointer_error);
                WindowsGuid id{};m.read(GuestAddress{args(1)},id);store_ptr(args(2),0);
                if(id!=ole_standard_guid(0) && id!=ole_standard_guid(0x20023))co_return finish(no_interface);
                ++frame->references;store_ptr(args(2),owner);co_return finish(0);
            }
            if(op=="Begin"){
                const auto result_ptr=co_await service_callback::call(slot(owner,3),{owner,args(1)});
                frame->fixed=result_ptr!=0;
                co_return finish(result_ptr?0:0x800440c7U);
            }
            if(op=="End"){frame->fixed=false;co_return finish(0);}
            const bool configure=op=="SetFormat";
            const auto fail_format=configure?bad_format:0U;
            const auto temp=scratch.allocate(208);
            if(!temp)co_return finish(configure?no_memory:0);
            auto position=static_cast<std::uint32_t>(args(1));
            if(configure){
                const auto hr=co_await service_callback::call(slot(stream,4),{stream,temp,204});
                if(failed_hr(hr))co_return finish(hr);
                if(m.load(GuestAddress{temp},32)!=avi::video)co_return finish(unsupported);
                position=static_cast<unsigned>(m.load(GuestAddress{temp+28},32));
            }
            m.store(GuestAddress{temp},32,0);
            auto hr=co_await service_callback::call(slot(stream,6),{stream,position,0,temp});
            const auto size=m.load(GuestAddress{temp},32);
            if(failed_hr(hr) || size<40 || size>65536)co_return finish(fail_format);
            const auto format_address=scratch.allocate(size);
            if(!format_address)co_return finish(configure?no_memory:0);
            hr=co_await service_callback::call(slot(stream,6),{stream,position,format_address,temp});
            const auto returned=m.load(GuestAddress{temp},32);
            if(failed_hr(hr) || returned<40 || returned>size)co_return finish(fail_format);
            Bytes format(returned);m.read(GuestAddress{format_address},format);
            const auto source=avi::frame_layout(format);
            if(!source)co_return finish(fail_format);
            auto requested=frame->requested;
            if(configure){
                requested.clear();
                if(args(1)>1){
                    requested.resize(40);m.read(GuestAddress{args(1)},requested);
                    const auto count=avi::frame_header_size(requested);
                    if(!count)co_return finish(bad_format);
                    requested.resize(count);m.read(GuestAddress{args(1)},requested);
                }
            }
            auto header=avi::frame_output(format,*source,requested);
            if(header.empty())co_return finish(fail_format);
            const auto target=*avi::frame_layout(header);
            if(configure){
                const auto x=static_cast<int>(args(3)),y=static_cast<int>(args(4));
                const auto w=static_cast<int>(args(5)),h=static_cast<int>(args(6));
                if(x<0 || y<0 || (w<=0 && w!=-1) || (h<=0 && h!=-1) ||
                   std::uint64_t(static_cast<unsigned>(x))+(w==-1?target.width:static_cast<unsigned>(w))>target.width ||
                   std::uint64_t(static_cast<unsigned>(y))+(h==-1?target.height:static_cast<unsigned>(h))>target.height)
                    co_return finish(bad_parameter);
                frame->position=UINT32_MAX;frame->requested=std::move(requested);frame->destination=args(2);
                frame->x=x;frame->y=y;frame->width=w;frame->height=h;
                co_return finish(0);
            }
            if(frame->fixed && frame->buffer && frame->position==position)co_return finish(frame->buffer);
            auto read_packet=[&](unsigned pos)->service_callback::Task<Bytes>{
                GuestScratch block(*this,m,s);
                auto status=co_await service_callback::call(slot(stream,8),{stream,pos,1,0,0,temp,temp+4});
                const auto n=m.load(GuestAddress{temp},32);
                if(failed_hr(status) || !n || n>128U*1024*1024 || m.load(GuestAddress{temp+4},32)!=1)co_return Bytes{};
                const auto address=block.allocate(n);
                if(!address)co_return Bytes{};
                status=co_await service_callback::call(slot(stream,8),{stream,pos,1,address,n,temp,temp+4});
                if(failed_hr(status) || m.load(GuestAddress{temp},32)!=n || m.load(GuestAddress{temp+4},32)!=1)co_return Bytes{};
                Bytes bytes(n);m.read(GuestAddress{address},bytes);co_return bytes;
            };
            Bytes packet,rgba;
            std::optional<avi::FrameLayout> decoded_source;
            if(media::video_decoder_available(source->compression)){
                const auto nearest=co_await service_callback::call(slot(stream,5),{stream,position,0x14});
                const auto key=static_cast<unsigned>(nearest);
                if(key>position || position>INT32_MAX)co_return finish(0);
                media::CompressedVideo compressed{source->compression,source->width,source->height,{format.begin()+40,format.end()}};
                std::unique_ptr<media::VideoDecoder> decoder;
                try{decoder=std::make_unique<media::VideoDecoder>(compressed);}
                catch(const std::runtime_error&){co_return finish(0);}
                auto collect=[&](const auto& frames){
                    for(const auto& decoded:frames)
                        if(decoded.pts.timescale && decoded.pts.value==position && decoded.width==source->width && decoded.height==source->height)
                            rgba=avi::frame_rgba(decoded);
                };
                for(unsigned pos=key;pos<=position;++pos){
                    packet=co_await read_packet(pos);
                    if(packet.empty())co_return finish(0);
                    try{collect(decoder->decode(packet,pos,pos==key));}
                    catch(const std::runtime_error&){co_return finish(0);}
                }
                if(rgba.empty()){
                    try{collect(decoder->drain());}
                    catch(const std::runtime_error&){co_return finish(0);}
                }
                if(rgba.empty())co_return finish(0);
            }else if(avi::is_run_length(*source) || media::video1_fourcc(source->compression)){
                const auto nearest=co_await service_callback::call(slot(stream,5),{stream,position,0x14});
                const auto key=static_cast<unsigned>(nearest);
                if(key>position || position>INT32_MAX)co_return finish(0);
                decoded_source=avi::frame_layout(avi::uncompressed_frame_header(format));
                if(!decoded_source || decoded_source->top_down)co_return finish(0);
                std::unique_ptr<media::Video1Decoder> video1;
                if(media::video1_fourcc(source->compression)){
                    try{video1=avi::open_video1(format,*source);}
                    catch(const std::runtime_error&){co_return finish(0);}
                }else packet.resize(decoded_source->image_size());
                for(unsigned pos=key;pos<=position;++pos){
                    const auto encoded=co_await read_packet(pos);
                    if(video1){
                        if(encoded.empty())co_return finish(0);
                        try{packet=video1->decode(encoded,pos==key);}
                        catch(const std::runtime_error&){co_return finish(0);}
                        if(packet.size()!=decoded_source->image_size())co_return finish(0);
                    }else if(!avi::decode_run_length(encoded,*decoded_source,packet))co_return finish(0);
                }
            }else{
                packet=co_await read_packet(position);
                if(packet.empty())co_return finish(0);
            }
            Bytes pixels(target.image_size());
            if(frame->destination)m.read(GuestAddress{frame->destination},pixels);
            if(!avi::frame_pixels(format,decoded_source?*decoded_source:*source,packet,target,pixels,
                       static_cast<unsigned>(frame->x),static_cast<unsigned>(frame->y),
                       frame->width==-1?target.width:static_cast<unsigned>(frame->width),
                       frame->height==-1?target.height:static_cast<unsigned>(frame->height),std::move(rgba)))co_return finish(0);
            const auto buffer=allocate_heap(m,header.size()+(frame->destination?0:pixels.size()));
            if(!buffer)co_return finish(0);
            m.write(GuestAddress{buffer},header);
            m.write(GuestAddress{frame->destination?frame->destination:buffer+header.size()},pixels);
            if(frame->buffer)free_heap(m,frame->buffer,s);
            frame->position=position;frame->buffer=buffer;co_return finish(buffer);
        });
    }
