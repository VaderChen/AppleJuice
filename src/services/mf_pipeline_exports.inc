if(name=="MFTEnumEx" || name=="MFTEnum"){
    const bool legacy=name=="MFTEnum";const auto output_arg=legacy?5U:4U,count_arg=legacy?6U:5U;
    out(arg(output_arg));put(arg(count_arg),0);const auto category=guid(arg(0));auto flags=d(1);
    // 舊列舉僅包含同步軟體 MFT；共用實際可用的目錄與型別篩選，不能把非同步硬體解碼器列入。
    if(legacy){require(flags==0);flags=1;}else {require(!(flags&~0x1ffU));if(!flags)flags=0x51;if(!(flags&7))flags|=1;}
    const auto matches=[&](std::uint64_t filter,const Guid& subtype){return !filter || (guid(filter)==mf::MFMediaType_Video && guid(filter+16)==subtype);};
    const bool encode=category==mf::MFT_CATEGORY_VIDEO_ENCODER;
    const auto input_subtype=encode?mf::nv12:mf::h264,output_subtype=encode?mf::h264:mf::nv12;
    if((encode || category==mf::MFT_CATEGORY_VIDEO_DECODER) && (flags&4) && matches(arg(2),input_subtype) && matches(arg(3),output_subtype) &&
        (encode?video::hardware_encode_supported(video::Codec::H264):video::hardware_decode_supported(video::Codec::H264))){
        const auto p=make(mf::Activate);auto activation=object(p,mf::Activate);activation->identifier=encode?3:1;
        set(*activation,mf::MFT_TRANSFORM_CLSID_Attribute,valueguid(encode?mf::native_h264_encoder:mf::native_h264));set(*activation,mf::MFT_FRIENDLY_NAME_Attribute,valuestring(encode?u"MacWin VideoToolbox H.264 硬體編碼器":u"MacWin VideoToolbox H.264 硬體解碼器"));
        set(*activation,mf::MF_TRANSFORM_ASYNC,value32(1));set(*activation,mf::MFT_ENUM_HARDWARE_URL_Attribute,valuestring(encode?u"macwin://videotoolbox/h264/encoder":u"macwin://videotoolbox/h264"));set(*activation,mf::MFT_ENUM_HARDWARE_VENDOR_ID_Attribute,valuestring(u"VEN_106B"));
        for(const auto& [key,subtype]:std::array<std::pair<Guid,Guid>,2>{{{mf::MFT_INPUT_TYPES_Attributes,input_subtype},{mf::MFT_OUTPUT_TYPES_Attributes,output_subtype}}}){Value value;value.type=0x1011;value.blob.insert(value.blob.end(),mf::MFMediaType_Video.begin(),mf::MFMediaType_Video.end());value.blob.insert(value.blob.end(),subtype.begin(),subtype.end());set(*activation,key,std::move(value));}
        const auto array=allocate(unit);put(array,p,bits);put(arg(output_arg),array,bits);put(arg(count_arg),1);
    }
}else if(name=="MFEnumDeviceSources"){
    out(arg(1));put(arg(2),0);auto attributes=entry(arg(0)).object;const auto type=get(*attributes,mf::MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,72).id;
    require(type==mf::MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID || type==mf::MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if(type==mf::MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID){
        if(const auto role=attributes->attributes.find(mf::MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ROLE);role!=attributes->attributes.end())require(role->second.type==19 && role->second.scalar<=2);
        if(audio_available()){
            const auto p=make(mf::Activate);auto activation=object(p,mf::Activate);activation->identifier=2;set(*activation,mf::MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,valueguid(type));set(*activation,mf::MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID,valuestring(audio_endpoint));set(*activation,mf::MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,valuestring(u"macOS Core Audio PCM input"));
            Value value;value.type=0x1011;value.blob.insert(value.blob.end(),mf::MFMediaType_Audio.begin(),mf::MFMediaType_Audio.end());value.blob.insert(value.blob.end(),mf::pcm.begin(),mf::pcm.end());set(*activation,mf::MF_DEVSOURCE_ATTRIBUTE_MEDIA_TYPE,std::move(value));
            const auto array=allocate(unit);put(array,p,bits);put(arg(1),array,bits);put(arg(2),1);
        }
    }
    // 客體裝置目錄目前沒有相機映射；不把未提供的宿主相機宣告為可用。
}else if(name=="MFCreateDeviceSource"){out(arg(1));put(arg(1),source_create(*entry(arg(0)).object),bits);}
else if(name=="MFCreateSourceReaderFromMediaSource"){
    out(arg(2));put(arg(2),reader_create(arg(0),arg(1)),bits);
}else if(name=="PipelinePoll"){
    auto target=entry(arg(0)).object;if(target->kind!=mf::Reader && target->kind!=mf::MediaStream)target->poll_queued=false;
    if(target->kind==mf::Transform){
        if((target->decoding.valid() && target->decoding.wait_for(std::chrono::seconds(0))!=std::future_status::ready) || (target->encoding.valid() && target->encoding.wait_for(std::chrono::seconds(0))!=std::future_status::ready)){arg.return_to(state,0);return std::nullopt;}
        if(target->decoding.valid()){
            try {auto frames=target->decoding.get();if(!target->closed)for(auto& frame:frames){const auto stamp=media_time(frame.pts);if(stamp && (std::bit_cast<std::int64_t>(*stamp)<target->lower_bound || std::bit_cast<std::int64_t>(*stamp)>target->upper_bound))continue;require(target->frames.size()<32,0x8007000eU);target->frames.push_back(std::move(frame));++target->output_credit;queue_event(target,602);}}
            catch(const std::exception&){if(!target->closed)queue_event(target,1,{},0xc00d36b4U);}
        }
        if(target->encoding.valid()){
            try {auto packets=target->encoding.get();if(!target->closed)for(auto& packet:packets){
                const auto stamp=media_time(packet.pts);if(stamp && (std::bit_cast<std::int64_t>(*stamp)<target->lower_bound || std::bit_cast<std::int64_t>(*stamp)>target->upper_bound))continue;
                require(target->packets.size()<32,0x8007000eU);target->packets.push_back(std::move(packet));++target->output_credit;queue_event(target,602);
            }}catch(const std::exception&){if(!target->closed)queue_event(target,1,{},0xc00d36b4U);}
        }
        if(!target->closed && target->drain_pending){target->drain_pending=false;drain_codec(target);}
        else {if(!target->closed){if(target->draining && target->frames.empty() && target->packets.empty()){queue_event(target,603);target->draining=false;}request_input(target);}release_job(target);}
    }else if(target->kind==mf::Reader || target->kind==mf::MediaStream){
        if(target->request_pending || target->file_read_active){
            std::uint32_t status=0,flags=0;std::uint64_t sample=0;
            try {
                auto source=object(target->parent,mf::Source);
                if(!target->file_read_active){require(!source->closed,shutdown);target->file_read_active=true;target->file_read_generation=target->generation;}
                sample=source->file_source && source->file_source->movie?movie_sample(source,*target):captured_sample(source,target->read_frames,true);if(source->closed){release(sample);sample=0;throw Failure{shutdown};}
                if(!sample && source->file_source)flags=2;else require(sample!=0,0xc00d36b2U);
            }catch(const Failure& error){status=error.code;}
            const bool cancelled=!target->request_pending || target->file_read_generation!=target->generation;
            target->file_read_active=false;target->request_pending=false;
            if(cancelled){release(sample);sample=0;}

            if(!cancelled && target->kind==mf::Reader){
                const auto time=sample?*object(sample,mf::Sample)->time:0;
                callbacks.push_back({foreign_method(target->callback,3),{target->callback,status,0,status?1U:flags,time,sample},false,false,1U<<4});if(sample)callbacks.push_back({resolve("internal-mf","Release").value,{sample}});
            }else if(!cancelled && !target->closed){if(sample){if(target->pending_token){Value token;token.type=13;token.scalar=target->pending_token;set(*object(sample,mf::Sample),mf::MFSampleExtension_Token,token);}Value value;value.type=13;value.scalar=sample;queue_event(target,213,value);release(sample);}
                else if(flags&2U){
                    queue_event(target,212);target->flags|=1U;auto source=object(target->parent,mf::Source);
                    if(!source->file_source || !source->file_source->movie || std::all_of(source->media_streams.begin(),source->media_streams.end(),[&](std::uint64_t p){return object(p,mf::MediaStream)->flags&1U;}))queue_event(source,211);
                }else queue_event(target,1,{},status);}
            release(target->pending_token);target->pending_token=0;
        }
        if(target->flush_pending){target->flush_pending=false;if(target->callback)callbacks.push_back({foreign_method(target->callback,4),{target->callback,0}});}
        target->poll_queued=false;release_job(target);
    }
}
