else if(name.starts_with("V_")){
    require(item->kind==mf::Activate,no_interface);
    if(name=="V_ActivateObject"){
        out(arg(2));if(!item->result_object){
            if(item->identifier==1 || item->identifier==3){require(item->identifier==3?video::hardware_encode_supported(video::Codec::H264):video::hardware_decode_supported(video::Codec::H264),0xc00d3704U);const auto p=make(mf::Transform);auto transform=object(p,mf::Transform);transform->identifier=item->identifier;transform->attribute_store=make(mf::Attributes);transform->input_attributes=make(mf::Attributes);transform->output_attributes=make(mf::Attributes);set(*object(transform->attribute_store,mf::Attributes),mf::MF_TRANSFORM_ASYNC,value32(1));item->result_object=p;}
            else if(item->identifier==2)item->result_object=source_create(*item);else throw Failure{invalid};
        }query(item->result_object,guid(arg(1)),arg(2));
    }else if(name=="V_DetachObject"){release(item->result_object);item->result_object=0;}
    else if(name=="V_ShutdownObject"){if(item->result_object){auto target=entry(item->result_object).object;if(!target->closed)object_shutdown(target);release(item->result_object);item->result_object=0;}}
}else if(name.starts_with("Z_")){
    require(item->kind==mf::Transform,no_interface);
    if(name=="Z_Shutdown")object_shutdown(item);
    else if(name=="Z_GetShutdownStatus"){require(item->closed,0xc00d36b2U);put(arg(1),1);}
}else if(name.starts_with("F_")){
    require(item->kind==mf::Transform,no_interface);require(!item->closed,shutdown);
    if(name=="F_GetAttributes"){out(arg(1));hold(item->attribute_store);put(arg(1),item->attribute_store,bits);}
    else {
        const auto attrs=object(item->attribute_store,mf::Attributes);const auto unlocked=attrs->attributes.find(mf::MF_TRANSFORM_ASYNC_UNLOCK);require(unlocked!=attrs->attributes.end() && unlocked->second.type==19 && unlocked->second.scalar!=0,0xc00d6d77U);
        const auto stream=[&](unsigned at){require(d(at)==0,0xc00d36b3U);};
        if(name=="F_GetStreamLimits"){for(unsigned n=1;n<5;++n)put(arg(n),1);}
        else if(name=="F_GetStreamCount"){put(arg(1),1);put(arg(2),1);}
        else if(name=="F_GetStreamIDs"){require(d(1)>=1 && d(3)>=1,small);put(arg(2),0);put(arg(4),0);}
        else if(name=="F_GetInputStreamInfo"){
            stream(1);check(arg(2),24);memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(24));put(arg(2)+8,item->identifier==3?7:3);put(arg(2)+12,1);
            if(item->identifier==3 && item->current_type){const auto type=object(item->current_type,mf::MediaType);const auto extent=get(*type,mf::MF_MT_FRAME_SIZE,21).scalar;const auto stride=type->attributes.contains(mf::MF_MT_DEFAULT_STRIDE)?get(*type,mf::MF_MT_DEFAULT_STRIDE,19).scalar:extent>>32;put(arg(2)+12,stride*(extent&UINT32_MAX)*3/2);}
        }
        else if(name=="F_GetOutputStreamInfo"){stream(1);check(arg(2),12);memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(12));put(arg(2),item->identifier==3?0x103:0x107);if(item->output_type){const auto type=object(item->output_type,mf::MediaType);const auto extent=get(*type,mf::MF_MT_FRAME_SIZE,21).scalar;put(arg(2)+4,item->identifier==3?0:(extent>>32)*(extent&UINT32_MAX)*3/2);}}
        else if(name=="F_GetInputStreamAttributes" || name=="F_GetOutputStreamAttributes"){stream(1);out(arg(2));const auto p=name=="F_GetInputStreamAttributes"?item->input_attributes:item->output_attributes;hold(p);put(arg(2),p,bits);}
        else if(name=="F_DeleteInputStream" || name=="F_AddInputStreams")throw Failure{0x80004001U}; // 單一固定輸入串流不支援動態拓撲。
        else if(name=="F_GetInputAvailableType" || name=="F_GetOutputAvailableType"){stream(1);out(arg(3));require(d(2)==0,0xc00d36b9U);const bool input=name=="F_GetInputAvailableType";require(item->identifier==3?(!input || item->output_type!=0):(input || item->current_type!=0),0xc00d6d60U);put(arg(3),video_type(item,input),bits);}
        else if(name=="F_GetInputCurrentType" || name=="F_GetOutputCurrentType"){stream(1);out(arg(2));const auto p=name=="F_GetInputCurrentType"?item->current_type:item->output_type;require(p!=0,0xc00d6d60U);if(item->identifier==3)put(arg(2),copy_media_type(p),bits);else {hold(p);put(arg(2),p,bits);}}
        else if(name=="F_SetInputType" || name=="F_SetOutputType"){
            stream(1);require(!(d(3)&~1U));require(!item->decoding.valid() && !item->encoding.valid() && item->frames.empty() && item->packets.empty(),0xc00d6d74U);const bool input=name=="F_SetInputType";auto& destination=input?item->current_type:item->output_type;
            if(!arg(2)){if(!d(3)){release(destination);destination=0;if(input)item->decoder.reset();item->encoder.reset();}}
            else if(item->identifier==3){
                const auto type=object(arg(2),mf::MediaType);require(get(*type,mf::MF_MT_MAJOR_TYPE,72).id==mf::MFMediaType_Video && get(*type,mf::MF_MT_SUBTYPE,72).id==(input?mf::nv12:mf::h264),0xc00d36b4U);
                std::shared_ptr<video::HardwareEncoder> encoder;
                if(input){
                    require(item->output_type!=0,0xc00d6d60U);const auto output=object(item->output_type,mf::MediaType);
                    const auto extent=get(*type,mf::MF_MT_FRAME_SIZE,21).scalar;require(extent==get(*output,mf::MF_MT_FRAME_SIZE,21).scalar,0xc00d36b4U);
                    if(type->attributes.contains(mf::MF_MT_FRAME_RATE))require(get(*type,mf::MF_MT_FRAME_RATE,21).scalar==get(*output,mf::MF_MT_FRAME_RATE,21).scalar,0xc00d36b4U);
                    if(type->attributes.contains(mf::MF_MT_INTERLACE_MODE))require(get(*type,mf::MF_MT_INTERLACE_MODE,19).scalar==2,0xc00d36b4U);
                    if(type->attributes.contains(mf::MF_MT_DEFAULT_STRIDE)){const auto stride=get(*type,mf::MF_MT_DEFAULT_STRIDE,19).scalar;require(stride>=(extent>>32) && stride<=INT32_MAX && !(stride&1) && stride*(extent&UINT32_MAX)*3/2<=256U*1024U*1024U,0xc00d36b4U);}
                }else {
                    const auto config=encoder_config(*type);
                    if(item->current_type){const auto previous=object(item->current_type,mf::MediaType);require(get(*previous,mf::MF_MT_FRAME_SIZE,21).scalar==get(*type,mf::MF_MT_FRAME_SIZE,21).scalar,0xc00d36b4U);if(previous->attributes.contains(mf::MF_MT_FRAME_RATE))require(get(*previous,mf::MF_MT_FRAME_RATE,21).scalar==get(*type,mf::MF_MT_FRAME_RATE,21).scalar,0xc00d36b4U);}
                    encoder=std::make_shared<video::HardwareEncoder>(config);
                }
                if(!d(3)){const auto copy=copy_media_type(arg(2));release(destination);destination=copy;if(!input)item->encoder=std::move(encoder);else if(!item->encoder)item->encoder=std::make_shared<video::HardwareEncoder>(encoder_config(*object(item->output_type,mf::MediaType)));}
            }
            else {
                const auto type=object(arg(2),mf::MediaType);require(get(*type,mf::MF_MT_MAJOR_TYPE,72).id==mf::MFMediaType_Video && get(*type,mf::MF_MT_SUBTYPE,72).id==(input?mf::h264:mf::nv12),0xc00d36b4U);
                std::shared_ptr<video::HardwareDecoder> decoder;unsigned length_size=4;
                if(input){if(const auto header=type->attributes.find(mf::MF_MT_MPEG_SEQUENCE_HEADER);header!=type->attributes.end()){require(header->second.type==0x1011,type_error);auto config=mf::h264_config(header->second.blob);length_size=config.nal_length_size;config.nal_length_size=4;require(config.parameter_sets.size()>=2,0xc00d36b4U);decoder=std::make_shared<video::HardwareDecoder>(config);}}
                else {require(item->current_type!=0,0xc00d6d60U);const auto extent=get(*type,mf::MF_MT_FRAME_SIZE,21).scalar;require((extent>>32) && (extent&UINT32_MAX) && !(extent>>32&1) && !(extent&1),0xc00d36b4U);if(item->decoder)require((extent>>32)==item->decoder->width() && (extent&UINT32_MAX)==item->decoder->height(),0xc00d36b4U);}
                if(!d(3)){hold(arg(2));release(destination);destination=arg(2);if(input){item->decoder=std::move(decoder);item->nal_length_size=length_size;}}
            }
        }else if(name=="F_GetInputStatus"){stream(1);put(arg(2),item->input_credit?1:0);}
        else if(name=="F_GetOutputStatus")put(arg(1),item->output_credit?1:0);
        else if(name=="F_SetOutputBounds"){item->lower_bound=std::bit_cast<std::int64_t>(arg(1));item->upper_bound=std::bit_cast<std::int64_t>(arg(2));require(item->lower_bound<=item->upper_bound);}
        else if(name=="F_ProcessEvent"){stream(1);(void)object(arg(2),mf::Event);throw Failure{0x80004001U};} // 解碼器不消費呼叫端的 in-band 事件。
        else if(name=="F_ProcessMessage"){
            const auto message=d(1);
            if(message==0){item->streaming=false;discard_codec_jobs(item);if(item->decoder)item->decoder->flush();if(item->encoder)item->encoder->flush();item->frames.clear();item->packets.clear();for(const auto p:item->events)release(p);item->events.clear();item->input_credit=item->output_credit=0;item->draining=item->drain_pending=item->codec_drained=false;}
            else if(message==1){require(item->identifier==3?item->encoder!=nullptr:item->decoder!=nullptr,0xc00d6d60U);require(!item->draining,0xc00d36b2U);item->streaming=false;item->draining=true;item->input_credit=0;hold_job(item);if(item->decoding.valid() || item->encoding.valid())item->drain_pending=true;else drain_codec(item);}
            else if(message==2)throw Failure{0x80004001U}; // 此 MFT 輸出系統記憶體 NV12；沒有宣告 D3D-aware。
            else if(message==0x10000000U){require(item->current_type && item->output_type,0xc00d6d60U);}
            else if(message==0x10000003U){require(item->current_type && item->output_type,0xc00d6d60U);require(!item->draining,0xc00d36b2U);if(item->codec_drained){if(item->decoder)item->decoder->flush();if(item->encoder)item->encoder->flush();item->codec_drained=false;}item->streaming=true;request_input(item);}
            else if(message==0x10000001U || message==0x10000002U){item->streaming=false;item->input_credit=0;}
            else if(message==0x20000000U){auto notice=queue_event(item,604);set(*notice,mf::MF_EVENT_MFT_CONTEXT,value64(arg(2)));}
            else throw Failure{0x80004001U};
        }else if(name=="F_ProcessInput"){
            stream(1);require(d(3)==0);require(item->current_type && item->output_type,0xc00d6d60U);require(item->streaming && item->input_credit && !item->decoding.valid() && !item->encoding.valid(),0xc00d36b5U);auto sample=object(arg(2),mf::Sample);const auto bytes=sample_bytes(*sample);require(!bytes.empty(),0xc00d36b4U);
            const auto pts=sample->time?video::Timestamp{std::bit_cast<std::int64_t>(*sample->time),10000000}:video::Timestamp{};const auto duration=sample->duration?video::Timestamp{std::bit_cast<std::int64_t>(*sample->duration),10000000}:video::Timestamp{};
            if(item->identifier==3){
                require(item->encoder!=nullptr,0xc00d6d60U);require(sample->time.has_value(),0xc00d36c8U);require(sample->duration.has_value(),0xc00d36c9U);require(duration.value>0);
                const auto type=object(item->current_type,mf::MediaType);const auto extent=get(*type,mf::MF_MT_FRAME_SIZE,21).scalar,width=extent>>32,height=extent&UINT32_MAX;
                const auto stride=type->attributes.contains(mf::MF_MT_DEFAULT_STRIDE)?get(*type,mf::MF_MT_DEFAULT_STRIDE,19).scalar:width;
                require(stride>=width && stride<=INT32_MAX && !(stride&1) && height<=8192 && stride*height*3/2==bytes.size(),0xc00d36b4U);
                std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width*height*3/2));
                for(std::size_t row=0;row<height*3/2;++row)std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(row*stride),width,pixels.begin()+static_cast<std::ptrdiff_t>(row*width));
                const auto encoder=item->encoder;item->encoding=std::async(std::launch::async,[encoder,pixels=std::move(pixels),pts,duration]{return encoder->encode(pixels,pts,duration);});
            }else {
                if(!item->decoder){auto config=mf::h264_config(bytes);require(config.parameter_sets.size()>=2,0xc00d36b4U);item->decoder=std::make_shared<video::HardwareDecoder>(config);}
                auto packet=mf::h264_packet(bytes,item->nal_length_size);const auto decoder=item->decoder;
                item->decoding=std::async(std::launch::async,[decoder,packet=std::move(packet),pts,duration]{return decoder->decode(packet,pts,duration);});
            }
            hold_job(item);item->input_credit=0;
        }else if(name=="F_ProcessOutput"){
            require(d(1)==0 && d(2)==1);check(arg(3),x86_?16:32);put(arg(4),0);const auto at=arg(3),sample_at=at+(x86_?4U:8U),status_at=at+(x86_?8U:16U),events_at=at+(x86_?12U:24U);require(memory.load(GuestAddress{at},32)==0,0xc00d36b3U);require(memory.load(GuestAddress{sample_at},bits)==0);put(status_at,0);out(events_at);require(item->output_credit && (item->identifier==3?!item->packets.empty():!item->frames.empty()),0x8000ffffU);
            if(item->identifier==3){
                const auto& packet=item->packets.front();const auto bytes=mf::h264_annex_packet(packet);
                const auto sample=bytes_sample(bytes,media_time(packet.pts),media_time(packet.duration));
                set(*object(sample,mf::Sample),mf::MFSampleExtension_CleanPoint,value32(packet.keyframe));
                if(const auto dts=media_time(packet.dts))set(*object(sample,mf::Sample),mf::MFSampleExtension_DecodeTimestamp,value64(*dts));
                Value header;header.type=0x1011;header.blob=mf::h264_annex_header(packet.parameter_sets);set(*object(item->output_type,mf::MediaType),mf::MF_MT_MPEG_SEQUENCE_HEADER,std::move(header));
                put(sample_at,sample,bits);item->packets.pop_front();
            }else {
                auto& frame=item->frames.front();const auto extent=get(*object(item->output_type,mf::MediaType),mf::MF_MT_FRAME_SIZE,21).scalar;require((extent>>32)==frame.width && (extent&UINT32_MAX)==frame.height,0xc00d6d61U);const auto bytes=frame.materialize();
                const auto sample=bytes_sample(bytes,media_time(frame.pts),media_time(frame.duration));put(sample_at,sample,bits);item->frames.pop_front();
            }
            --item->output_credit;
            if(item->draining && item->frames.empty() && item->packets.empty() && !item->decoding.valid() && !item->encoding.valid() && !item->drain_pending){queue_event(item,603);item->draining=false;}request_input(item);
        }else throw Failure{no_interface};
    }
}else if(name.starts_with("C_")){
    require(item->kind==mf::Source,no_interface);require(!item->closed,shutdown);
    if(name=="C_GetCharacteristics")put(arg(1),5);
    else if(name=="C_CreatePresentationDescriptor"){
        out(arg(1));const auto original=object(item->children.at(0),mf::Presentation);const auto p=make(mf::Presentation);auto clone=object(p,mf::Presentation);clone->children=original->children;clone->selected=original->selected;for(const auto child:clone->children)hold(child);put(arg(1),p,bits);
    }else if(name=="C_Start"){
        const auto presentation=object(arg(1),mf::Presentation);require(presentation->children.size()==1 && presentation->selected.at(0),0xc00d36b2U);require(!arg(2) || guid(arg(2))==Guid{},0xc00d36c5U);const auto position=read_value(arg(3));require(position.type==0 || (position.type==20 && position.scalar==0),0xc00d36b2U);
        source_start(item);if(!item->state){const auto p=make(mf::MediaStream);auto stream=object(p,mf::MediaStream);stream->parent=item->pointers.at(mf::Source);hold(stream->parent);stream->children={presentation->children.at(0)};hold(stream->children.at(0));item->state=p;Value value;value.type=13;value.scalar=p;queue_event(item,205,value);}else {Value value;value.type=13;value.scalar=item->state;queue_event(item,206,value);}
        queue_event(item,201);queue_event(object(item->state,mf::MediaStream),202);
    }else if(name=="C_Stop" || name=="C_Pause"){require(item->streaming,0xc00d3e82U);source_stop(item);queue_event(item,name=="C_Stop"?207:209);if(item->state)queue_event(object(item->state,mf::MediaStream),name=="C_Stop"?208:210);}
    else if(name=="C_Shutdown")object_shutdown(item);
}else if(name.starts_with("W_")){
    require(item->kind==mf::MediaStream,no_interface);require(!item->closed,shutdown);
    if(name=="W_GetMediaSource"){out(arg(1));require(item->parent && bridge.objects.contains(item->parent),shutdown);hold(item->parent);put(arg(1),item->parent,bits);}
    else if(name=="W_GetStreamDescriptor"){out(arg(1));hold(item->children.at(0));put(arg(1),item->children.at(0),bits);}
    else if(name=="W_RequestSample"){auto source=object(item->parent,mf::Source);require(source->streaming,0xc00d36b2U);require(!item->request_pending,0xc00d36b5U);hold(arg(1));item->pending_token=arg(1);item->request_pending=true;hold_job(item);}
}else if(name.starts_with("L_")){
    require(item->kind==mf::Reader,no_interface);auto source=object(item->parent,mf::Source);require(!source->closed,shutdown);
    const auto stream=[&](unsigned n,bool all=false){require(d(n)==0 || d(n)==0xfffffffdU || (all && d(n)==0xfffffffeU),0xc00d36b3U);};
    if(name=="L_GetStreamSelection"){stream(1);put(arg(2),item->selected.at(0));}
    else if(name=="L_SetStreamSelection"){stream(1,true);require(!item->request_pending,0xc00d36b2U);item->selected[0]=d(2)!=0;}
    else if(name=="L_GetNativeMediaType" || name=="L_GetCurrentMediaType"){stream(1);const bool native=name=="L_GetNativeMediaType";out(arg(native?3:2));if(native)require(d(2)==0,0xc00d36b9U);const auto p=native?source_type(source):item->current_type;hold(p);put(arg(native?3:2),p,bits);}
    else if(name=="L_SetCurrentMediaType"){stream(1);require(!arg(2));const auto type=object(arg(3),mf::MediaType),native=object(source_type(source),mf::MediaType);for(const auto& [key,value]:native->attributes)require(get(*type,key)==value,0xc00d36b4U);require(!item->request_pending,0xc00d36b2U);hold(arg(3));release(item->current_type);item->current_type=arg(3);}
    else if(name=="L_SetCurrentPosition")throw Failure{0xc00d36b2U}; // Live capture 不宣告可 seek。
    else if(name=="L_ReadSample"){
        stream(1,true);require(!(d(2)&~1U));require(item->selected.at(0),0xc00d36b2U);require(!item->request_pending,0xc00d36b2U);
        if(item->callback){require(!arg(3) && !arg(4) && !arg(5) && !arg(6));source_start(source);item->request_pending=true;hold_job(item);}
        else {
            out(arg(6));check(arg(4),4);check(arg(5),8);if(arg(3))put(arg(3),0);put(arg(4),0);put(arg(5),0,64);source_start(source);const auto p=captured_sample(source,item->read_frames);
            if(p){put(arg(6),p,bits);put(arg(5),*object(p,mf::Sample)->time,64);}
            else if(d(2)&1)put(arg(4),2);
            else {auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::MediaFoundation;thread.wait.handles={arg(0)};schedule(state,memory);return std::nullopt;}
        }
    }else if(name=="L_Flush"){
        stream(1,true);item->request_pending=false;{std::lock_guard lock(source->capture->mutex);item->read_frames=source->capture->captured_frames;}
        if(item->callback)callbacks.push_back({foreign_method(item->callback,4),{item->callback,0}});
    }else if(name=="L_GetServiceForStream"){
        out(arg(4));require(d(1)==UINT32_MAX,0xc00d36b3U);require(guid(arg(2))==Guid{},0xc00d36d4U);query(item->parent,guid(arg(3)),arg(4));
    }else if(name=="L_GetPresentationAttribute"){
        if(d(1)==UINT32_MAX && guid(arg(2))==mf::MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS)write_value(arg(3),value32(5));
        else if(d(1)==UINT32_MAX)write_value(arg(3),get(*object(source->children.at(0),mf::Presentation),guid(arg(2))));else {stream(1);write_value(arg(3),get(*object(object(source->children.at(0),mf::Presentation)->children.at(0),mf::Stream),guid(arg(2))));}
    }else throw Failure{no_interface};
}
