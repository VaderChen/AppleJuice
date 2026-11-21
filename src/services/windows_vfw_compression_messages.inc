                    if(message>=vfw::compress_format && message<=vfw::compress_end){
                        const auto fail=vfw::deliver_error;
                        if(message==vfw::compress_end){
                            codec->compressing=false;
                            codec->rle_encoded_format.clear();
                            codec->rle_input.clear();
                            codec->video1_target.clear();
                            codec->video1_encoder.reset();
                            co_return 0;
                        }
                        if(message==vfw::compress_frame){
                            if(!codec->compressing || !first || second<vfw::compress_wire_size(unit))
                                co_return fail(vfw::bad_parameter);
                            const auto slot=[&](unsigned n){return m.load(GuestAddress{first+unit*n},ptr_bits);};
                            const auto field=[&](unsigned n){return static_cast<std::uint32_t>(m.load(GuestAddress{first+n},32));};
                            if(field(0)&~1U)co_return fail(vfw::bad_flags);
                            const auto input_at=slot(3),output_at=slot(1);
                            const auto input=header_at(input_at),output=header_at(output_at);
                            const auto src=avi::frame_layout(input),dst=avi::frame_layout(output);
                            if(!src || !dst || !valid_depth(src->depth) || !vfw::compression_source(backend,*src) ||
                               !vfw::compression_destination(backend,*src,*dst))co_return fail(vfw::bad_format);
                            const auto from=slot(4),to=slot(2),chunk=slot(5),flags=slot(6);
                            if(!from || !to)co_return fail(vfw::bad_parameter);
                            const auto quality=field(7*unit+8);
                            if(quality>10000 && quality!=UINT32_MAX)co_return fail(vfw::bad_parameter);
                            const auto check_output=[&](std::size_t bytes){
                                m.check(GuestAddress{to},bytes,Access::Write);
                                m.check(GuestAddress{output_at+20},4,Access::Write);
                                if(chunk)m.check(GuestAddress{chunk},4,Access::Write);
                                if(flags)m.check(GuestAddress{flags},4,Access::Write);
                            };
                            avi::Bytes pixels(src->image_size());
                            m.read(GuestAddress{from},pixels);
                            avi::Bytes encoded;
                            bool keyframe=true;
                            if(video1){
                                if(!codec->video1_encoder || avi::uncompressed_frame_header(output)!=codec->video1_target)
                                    co_return fail(vfw::bad_format);
                                const auto dib=avi::frame_layout(codec->video1_target);
                                if(!dib)co_return fail(vfw::bad_format);
                                avi::Bytes rgb555(dib->image_size());
                                if(!avi::frame_pixels(input,*src,pixels,*dib,rgb555,0,0,dib->width,dib->height))
                                    co_return fail(vfw::bad_format);
                                check_output(vfw::compression_bound(backend,*dst));
                                try{
                                    auto packet=codec->video1_encoder->encode(rgb555,(field(0)&1U)!=0);
                                    encoded=std::move(packet.bytes);
                                    keyframe=packet.key;
                                }catch(const std::runtime_error&){co_return fail(vfw::bad_format);}
                            }else encoded=vfw::encode_image(backend,input,*src,pixels,*dst,
                                quality==UINT32_MAX?codec->quality:quality,field(7*unit+4));
                            if(rle && !(field(0)&1U)){
                                avi::Bytes caller_previous;
                                avi::View reference;
                                const auto previous_format=slot(vfw::compress_previous(unit)/unit);
                                const auto previous_data=slot(vfw::compress_previous(unit)/unit+1);
                                if(previous_format && previous_data && header_at(previous_format)==input){
                                    caller_previous.resize(src->image_size());
                                    m.read(GuestAddress{previous_data},caller_previous);
                                    reference=caller_previous;
                                }else if(input==codec->rle_encoded_format)reference=codec->rle_input;
                                if(!reference.empty()){
                                    auto delta=vfw::encode_run_length(*src,pixels,reference);
                                    if(delta.size()<encoded.size()){
                                        encoded=std::move(delta);
                                        keyframe=false;
                                    }
                                }
                            }
                            if(encoded.empty())co_return fail(vfw::bad_format);
                            if(encoded.size()>vfw::compression_bound(backend,*dst))co_return fail(vfw::bad_size);
                            if(!video1)check_output(encoded.size());
                            m.write(GuestAddress{to},encoded);
                            m.store(GuestAddress{output_at+20},32,encoded.size());
                            if(chunk)m.store(GuestAddress{chunk},32,avi::fourcc('0','0','d',dst->raw()?'b':'c'));
                            if(flags)m.store(GuestAddress{flags},32,keyframe?0x10U:0U);
                            if(rle){
                                codec->rle_encoded_format=input;
                                codec->rle_input=std::move(pixels);
                            }
                            co_return 0;
                        }
                        const auto input=header_at(first);
                        const auto src=avi::frame_layout(input);
                        if(!src || !valid_depth(src->depth) || !vfw::compression_source(backend,*src))
                            co_return fail(vfw::bad_format);
                        if(message==vfw::compress_format){
                            const auto proposed=vfw::compression_header(backend,input,*src);
                            if(proposed.empty())co_return fail(vfw::bad_format);
                            if(!second)co_return proposed.size();
                            m.write(GuestAddress{second},proposed);
                            co_return 0;
                        }
                        if(message==vfw::compress_query && !second)co_return 0;
                        const auto output=second?header_at(second):vfw::compression_header(backend,input,*src);
                        const auto dst=avi::frame_layout(output);
                        if(!dst || !vfw::compression_destination(backend,*src,*dst))co_return fail(vfw::bad_format);
                        if(message==vfw::compress_size)co_return vfw::compression_bound(backend,*dst);
                        if(message==vfw::compress_begin){
                            if(video1){
                                try{
                                    auto encoder=std::make_unique<media::Video1Encoder>(dst->width,dst->height);
                                    codec->video1_target=avi::uncompressed_frame_header(output);
                                    codec->video1_encoder=std::move(encoder);
                                }catch(const std::runtime_error&){co_return fail(vfw::bad_format);}
                            }
                            codec->compressing=true;
                            codec->rle_encoded_format.clear();
                            codec->rle_input.clear();
                        }
                        co_return 0;
                    }
                    if(message==vfw::default_quality || message==vfw::get_quality ||
                       message==vfw::set_quality || message==vfw::default_key_rate){
                        if(!first || (second && second<4))co_return vfw::deliver_error(vfw::bad_parameter);
                        if(video1 && message!=vfw::default_key_rate)co_return vfw::deliver_error(vfw::unsupported);
                        const unsigned quality=rle?vfw::rle_default_quality:7500;
                        if(message==vfw::set_quality){
                            const auto value=m.load(GuestAddress{first},32);
                            if(value>10000 && value!=UINT32_MAX)co_return vfw::deliver_error(vfw::bad_parameter);
                            codec->quality=value==UINT32_MAX?quality:static_cast<std::uint32_t>(value);
                        }else m.store(GuestAddress{first},32,message==vfw::default_key_rate?
                                     (rle?vfw::rle_key_rate:video1?media::video1_default_key_rate:1):
                                     message==vfw::default_quality?quality:codec->quality);
                        co_return 0;
                    }
