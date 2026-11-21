                    if(video1 && message>=vfw::decompress_ex_begin && message<=vfw::decompress_ex_frame){
                        const auto fail=vfw::deliver_error;
                        if(!first || second<5*unit+32)co_return fail(vfw::bad_parameter);
                        const auto pointer=[&](unsigned index){return m.load(GuestAddress{first+unit*index},ptr_bits);};
                        const auto field=[&](unsigned offset){return static_cast<std::int32_t>(m.load(GuestAddress{first+offset},32));};
                        const auto input=header_at(pointer(1));
                        const auto source=avi::frame_layout(input);
                        if(!source || !vfw::accepts(backend,*source))co_return fail(vfw::bad_format);
                        if(message==vfw::decompress_ex_query && !pointer(3))co_return 0;
                        const auto target=avi::frame_layout(header_at(pointer(3)));
                        if(!target || !target->raw())co_return fail(vfw::bad_format);
                        const auto rectangle=[&](unsigned offset,const avi::FrameLayout& bitmap)
                            ->std::optional<std::array<unsigned,4>>{
                            const auto x=field(offset),y=field(offset+4),w=field(offset+8),h=field(offset+12);
                            if(x<0 || y<0 || w< -1 || h< -1 || static_cast<unsigned>(x)>=bitmap.width ||
                               static_cast<unsigned>(y)>=bitmap.height)return {};
                            const auto left=static_cast<unsigned>(x),top=static_cast<unsigned>(y);
                            const auto width=w>0?static_cast<unsigned>(w):bitmap.width-left;
                            const auto height=h>0?static_cast<unsigned>(h):bitmap.height-top;
                            if(width>bitmap.width-left || height>bitmap.height-top)return {};
                            return std::array<unsigned,4>{left,top,width,height};
                        };
                        const auto destination=rectangle(5*unit,*target),crop=rectangle(5*unit+16,*source);
                        if(!destination || !crop)co_return fail(vfw::bad_format);
                        if(message==vfw::decompress_ex_begin){
                            if(!begin_video1(input,*source))co_return fail(vfw::bad_format);
                            codec->decompressing=true;
                        }
                        if(message!=vfw::decompress_ex_frame)co_return 0;
                        if(!codec->decompressing)co_return fail(vfw::bad_parameter);
                        const auto flags=static_cast<std::uint32_t>(field(0));
                        if(flags&vfw::renderer_nullframe)co_return 0;
                        const auto pixels=pointer(4),packet=pointer(2);
                        const auto size=avi::word(input,20);
                        if(!pixels || !packet || !size || size>memory_limit)co_return fail(vfw::bad_size);
                        const auto dib=avi::frame_layout(codec->video1_source);
                        if(!dib)co_return fail(vfw::bad_format);
                        avi::Bytes encoded(size),output(target->image_size());
                        m.read(GuestAddress{packet},encoded);
                        m.check(GuestAddress{pixels},output.size(),Access::Write);
                        m.read(GuestAddress{pixels},output);
                        const auto decoded=decode_video1(input,encoded,!(flags&vfw::renderer_non_key));
                        const auto [x,y,width,height]=*destination;
                        if(!avi::frame_pixels(codec->video1_source,*dib,decoded,*target,output,x,y,width,height,{},*crop))
                            co_return fail(vfw::bad_format);
                        m.write(GuestAddress{pixels},output);
                        co_return 0;
                    }
