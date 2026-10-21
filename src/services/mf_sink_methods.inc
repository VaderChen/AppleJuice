else if(name.starts_with("I_")){
    require(item->kind==mf::Writer && bool(item->writer),no_interface);auto& sink=*item->writer;
    constexpr std::uint32_t bad_request=0xc00d36b2U,bad_stream=0xc00d36b3U;
    const auto stream=[&](bool all=false){require((sink.output || sink.video) && (d(1)==0 || (all && d(1)==0xfffffffeU)),bad_stream);};
    const auto writing=[&]{require(sink.begun && !sink.finalized,bad_request);require(!sink.failure,sink.failure);};
    if(name=="I_AddStream"){
        check(arg(2),4);require(!sink.begun && !sink.finalized,bad_request);require(!sink.output && !sink.video,0xc00d4a3bU);
        const auto type=object(arg(1),mf::MediaType);
        if(sink.mp4){
            require(get(*type,mf::MF_MT_MAJOR_TYPE,72).id==mf::MFMediaType_Video && get(*type,mf::MF_MT_SUBTYPE,72).id==mf::h264,0xc00d36b4U);
            const auto config=encoder_config(*type);require(config.width && config.height && !(config.width&1) && !(config.height&1) && config.width<=8192 && config.height<=8192 && config.frame_rate_numerator && config.frame_rate_denominator && config.bitrate,0xc00d36b4U);
            auto parameters=std::vector<std::vector<std::uint8_t>>{};if(type->attributes.contains(mf::MF_MT_MPEG_SEQUENCE_HEADER))parameters=mf::h264_config(get(*type,mf::MF_MT_MPEG_SEQUENCE_HEADER,0x1011).blob).parameter_sets;
            const auto copy=copy_media_type(arg(1));item->output_type=copy;sink.video=config;sink.parameters=std::move(parameters);
        }else sink.output=sink_pcm_format(*type);
        put(arg(2),0);
    }else if(name=="I_SetInputMediaType"){
        stream();require(!sink.finalized,bad_request);const auto type=object(arg(2),mf::MediaType);
        if(arg(3))require(mf::attributes(entry(arg(3)).object->kind),no_interface);
        if(sink.mp4){
            require(get(*type,mf::MF_MT_MAJOR_TYPE,72).id==mf::MFMediaType_Video,0xc00d36b4U);const auto subtype=get(*type,mf::MF_MT_SUBTYPE,72).id;
            require(subtype==mf::nv12 || subtype==mf::h264,0xc00d36b4U);require(get(*type,mf::MF_MT_FRAME_SIZE,21).scalar==(std::uint64_t{sink.video->width}<<32|sink.video->height),0xc00d36b4U);
            if(type->attributes.contains(mf::MF_MT_FRAME_RATE))require(get(*type,mf::MF_MT_FRAME_RATE,21).scalar==(std::uint64_t{sink.video->frame_rate_numerator}<<32|sink.video->frame_rate_denominator),0xc00d36b4U);
            if(type->attributes.contains(mf::MF_MT_INTERLACE_MODE))require(get(*type,mf::MF_MT_INTERLACE_MODE,19).scalar==2,0xc00d36b4U);
            const auto stride=type->attributes.contains(mf::MF_MT_DEFAULT_STRIDE)?get(*type,mf::MF_MT_DEFAULT_STRIDE,19).scalar:sink.video->width;
            require(stride>=sink.video->width && stride<=INT32_MAX && !(stride&1) && stride*sink.video->height*3/2<=256U*1024U*1024U,0xc00d36b4U);
            require(!sink.begun || sink.encode==(subtype==mf::nv12),0xc00d36b4U);
            std::unique_ptr<video::HardwareEncoder> encoder;if(subtype==mf::nv12 && !sink.encoder)encoder=std::make_unique<video::HardwareEncoder>(*sink.video);
            const auto copy=copy_media_type(arg(2));release(item->current_type);item->current_type=copy;sink.encode=subtype==mf::nv12;sink.stride=static_cast<unsigned>(stride);if(encoder)sink.encoder=std::move(encoder);
        }else{const auto format=sink_pcm_format(*type);require(sink_same_format(format,*sink.output),0xc00d36b4U);sink.input=format;}
    }else if(name=="I_BeginWriting"){
        require(!sink.begun && !sink.finalized,bad_request);require(sink.output || sink.video,0xc00d4a41U);require(sink.input || item->current_type,0xc00d36b4U);
        const auto header=sink.mp4?mp4::header():*encode_pcm_wave_file(*sink.output,{});sink_write(sink,header,0);sink.header_size=static_cast<std::uint32_t>(header.size());sink.begun=true;sink.started_at=runtime_.clock_ticks;
    }else if(name=="I_GetStatistics"){
        stream();check(arg(2),112);require(memory.load(GuestAddress{arg(2)},32)==112);memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(112));put(arg(2),112);
        put(arg(2)+8,sink.last_time,64);put(arg(2)+16,sink.last_encoded,64);put(arg(2)+24,sink.last_time,64);put(arg(2)+32,sink.last_tick,64);put(arg(2)+40,sink.last_request,64);
        put(arg(2)+48,sink.samples,64);put(arg(2)+56,sink.encoded,64);put(arg(2)+64,sink.mp4?sink.video_samples.size():sink.samples,64);put(arg(2)+72,sink.ticks,64);put(arg(2)+88,sink.bytes,64);
    }else if(name=="I_GetServiceForStream"){
        out(arg(4));require(d(1)==0 || d(1)==UINT32_MAX,bad_stream);throw Failure{no_interface};
    }else if(name=="I_Finalize"){
        writing();std::uint32_t status=0;try{sink_finalize(sink);}catch(const Failure& error){status=error.code;}catch(const std::bad_alloc&){status=0x8007000eU;}catch(const std::invalid_argument&){status=0xc00d36b4U;}catch(const std::runtime_error&){status=0x80004005U;}
        sink.finalized=true;if(item->callback)sink_notice(item,true,0,status);else require(!status,status);
    }else{
        writing();stream(name=="I_Flush" || name=="I_NotifyEndOfSegment");
        if(name=="I_WriteSample"){
            const auto sample=object(arg(2),mf::Sample);require(sample->time.has_value(),0xc00d36c8U);const auto bytes=sample_bytes(*sample);
            if(sink.mp4){
                require(sample->duration.has_value(),0xc00d36c9U);require(*sample->duration && *sample->duration<=UINT32_MAX,0xc00d36b4U);
                const video::Timestamp pts{std::bit_cast<std::int64_t>(*sample->time),10000000},duration{static_cast<std::int64_t>(*sample->duration),10000000};
                if(sink.encode){
                    require(bytes.size()==std::uint64_t{sink.stride}*sink.video->height*3/2,0xc00d36b4U);
                    std::vector<std::uint8_t> pixels(std::size_t{sink.video->width}*sink.video->height*3/2);
                    for(unsigned row=0;row<sink.video->height*3/2;++row)std::copy_n(bytes.begin()+std::ptrdiff_t(row)*sink.stride,sink.video->width,pixels.begin()+std::ptrdiff_t(row)*sink.video->width);
                    const auto force=sample->attributes.contains(mf::MFSampleExtension_CleanPoint) && get(*sample,mf::MFSampleExtension_CleanPoint,19).scalar;
                    for(const auto& packet:sink.encoder->encode(pixels,pts,duration,sink.samples,force))sink_packet(sink,packet);
                }else{
                    video::EncodedPacket packet;packet.avcc=mf::h264_packet(bytes,4);packet.pts=packet.dts=pts;packet.duration=duration;
                    if(sample->attributes.contains(mf::MFSampleExtension_DecodeTimestamp))packet.dts.value=std::bit_cast<std::int64_t>(get(*sample,mf::MFSampleExtension_DecodeTimestamp,21).scalar);
                    packet.parameter_sets=mf::h264_config(bytes).parameter_sets;
                    for(const auto& nal:mf::annex_nals(bytes))if((nal[0]&31)==5)packet.keyframe=true;
                    if(sample->attributes.contains(mf::MFSampleExtension_CleanPoint))packet.keyframe=get(*sample,mf::MFSampleExtension_CleanPoint,19).scalar!=0;
                    sink_packet(sink,packet);
                }
            }else{
                require(bytes.size()%sink.input->align==0,0xc00d36b4U);require(bytes.size()<=UINT32_MAX-sink.header_size-sink.bytes,0x800700dfU);
                sink_write(sink,bytes,sink.header_size+sink.bytes);sink.bytes+=bytes.size();
            }
            ++sink.samples;sink.last_time=*sample->time;sink.last_request=runtime_.clock_ticks;
        }else if(name=="I_SendStreamTick"){sink.last_tick=arg(2);++sink.ticks;}
        else if(name=="I_PlaceMarker"){require(item->callback!=0,bad_request);sink_notice(item,false,arg(2),0);}
        else if(name=="I_Flush" || name=="I_NotifyEndOfSegment"){if(name=="I_Flush" && sink.encoder)sink.encoder->flush();sink_flush(sink);}
        else throw Failure{no_interface};
    }
}
