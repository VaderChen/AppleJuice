// 本檔在 MF 呼叫共用流程內使用，引用、配置與 COM 回呼沿用同一契約。
const auto value32=[](std::uint32_t scalar){Value value;value.type=19;value.scalar=scalar;return value;};
const auto value64=[](std::uint64_t scalar){Value value;value.type=21;value.scalar=scalar;return value;};
const auto valueguid=[](const Guid& id){Value value;value.type=72;value.id=id;return value;};
const auto valuestring=[](std::u16string text){Value value;value.type=31;value.text=std::move(text);return value;};
const auto audio_available=[&]{return audio_input_backend_?audio_input_backend_->available():audio::input_available();};
const std::u16string audio_endpoint=u"{0.0.1.00000000}.{x64arm-core-audio-input}";
const auto audio_type=[&]{
    const auto p=make(mf::MediaType);auto type=object(p,mf::MediaType);
    set(*type,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Audio));set(*type,mf::MF_MT_SUBTYPE,valueguid(mf::pcm));
    for(const auto& [key,value]:std::array<std::pair<Guid,std::uint32_t>,9>{{{mf::MF_MT_AUDIO_NUM_CHANNELS,2},{mf::MF_MT_AUDIO_SAMPLES_PER_SECOND,44100},{mf::MF_MT_AUDIO_BLOCK_ALIGNMENT,4},{mf::MF_MT_AUDIO_BITS_PER_SAMPLE,16},{mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND,176400},{mf::MF_MT_AUDIO_CHANNEL_MASK,3},{mf::MF_MT_COMPRESSED,0},{mf::MF_MT_FIXED_SIZE_SAMPLES,1},{mf::MF_MT_SAMPLE_SIZE,4}}})set(*type,key,value32(value));return p;
};
const auto source_type=[&](const std::shared_ptr<Object>& source){return object(object(source->children.at(0),mf::Presentation)->children.at(0),mf::Stream)->current_type;};
const auto source_create=[&](const Object& attributes){
    require(get(attributes,mf::MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,72).id==mf::MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID,0x80070490U);require(audio_available(),0x80070490U);
    if(const auto endpoint=attributes.attributes.find(mf::MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID);endpoint!=attributes.attributes.end())require(endpoint->second.type==31 && endpoint->second.text==audio_endpoint,0x80070490U);
    if(const auto role=attributes.attributes.find(mf::MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ROLE);role!=attributes.attributes.end())require(role->second.type==19 && role->second.scalar<=2);
    const auto p=make(mf::Source);auto source=object(p,mf::Source);source->capture=std::make_shared<audio::Buffer>();source->capture->pcm.resize(44100U*4U);source->capture->looping=true;
    const auto type=audio_type(),descriptor=make(mf::Stream),presentation=make(mf::Presentation);auto stream=object(descriptor,mf::Stream);stream->identifier=0;stream->children={type};stream->current_type=type;hold(type);
    auto desc=object(presentation,mf::Presentation);desc->children={descriptor};desc->selected={true};source->children={presentation};return p;
};
const auto source_start=[&](const std::shared_ptr<Object>& source){
    require(!source->closed,shutdown);if(!source->input)source->input=audio_input_backend_?audio_input_backend_->open(source->capture):audio::open_input(source->capture);
    require(bool(source->input),0x80004005U);{std::lock_guard lock(source->capture->mutex);require(!source->capture->capture_failed,0x80004005U);source->capture->playing=true;}source->streaming=true;
};
const auto source_stop=[&](const std::shared_ptr<Object>& source){if(source->capture){std::lock_guard lock(source->capture->mutex);source->capture->playing=false;}source->streaming=false;};
const auto discard_codec_jobs=[](const std::shared_ptr<Object>& target){
    // Flush／Shutdown 丟棄未消費結果；先收回工作，再釋放 native session。
    if(target->decoding.valid())try{(void)target->decoding.get();}catch(const std::exception&){}
    if(target->encoding.valid())try{(void)target->encoding.get();}catch(const std::exception&){}
};
const auto object_shutdown=[&](const std::shared_ptr<Object>& target){
    require(!target->closed,shutdown);target->closed=true;target->streaming=false;target->input_credit=target->output_credit=0;
    if(target->capture)source_stop(target);target->input.reset();discard_codec_jobs(target);target->decoder.reset();target->frames.clear();target->encoder.reset();target->packets.clear();
    for(const auto p:target->events)release(p);target->events.clear();release(target->callback);release(target->state);target->callback=0;
    // MediaSource／MediaStream 的互持引用只在 Shutdown 後解除。
    if(target->kind==mf::Source && target->state && bridge.objects.contains(target->state)){auto stream=entry(target->state).object;const auto source=stream->parent;stream->parent=0;stream->closed=true;release(source);}target->state=0;
};
close_source=[&](const std::shared_ptr<Object>& source){if(!source->closed)object_shutdown(source);};
const auto queue_event=[&](const std::shared_ptr<Object>& target,std::uint32_t type,Value value=Value{},std::uint32_t status=0){
    require(target->events.size()<65536,0x8007000eU);const auto p=event(type,{},status,std::move(value));target->events.push_back(p);deliver(target->pointers.at(target->kind),target);return object(p,mf::Event);
};
const auto request_input=[&](const std::shared_ptr<Object>& transform){if(transform->streaming && !transform->draining && !transform->input_credit && !transform->decoding.valid() && !transform->encoding.valid() && transform->frames.size()+transform->packets.size()<32){auto notice=queue_event(transform,601,value32(0));set(*notice,mf::MF_EVENT_MFT_INPUT_STREAM_ID,value32(0));transform->input_credit=1;}};
const auto bytes_sample=[&](std::span<const std::uint8_t> bytes,std::optional<std::uint64_t> time,std::optional<std::uint64_t> duration){
    require(bytes.size()<=256U*1024U*1024U);const auto buffer=memory_buffer(static_cast<std::uint32_t>(bytes.size()),0);auto b=object(buffer,mf::Buffer);memory.write(GuestAddress{b->data},bytes);b->length=b->maximum;
    const auto p=make(mf::Sample);auto sample=object(p,mf::Sample);sample->children={buffer};sample->time=time;sample->duration=duration;return p;
};
const auto captured_sample=[&](const std::shared_ptr<Object>& source,std::uint64_t& cursor){
    require(!source->closed,shutdown);std::vector<std::uint8_t> bytes;std::uint64_t begin{},count{};unsigned rate{},frame_bytes{};bool discontinuity=false;
    {std::lock_guard lock(source->capture->mutex);const auto& buffer=*source->capture;require(!buffer.capture_failed,0x80004005U);rate=buffer.rate;frame_bytes=buffer.frame_bytes();const auto ring=buffer.pcm.size()/frame_bytes;const auto total=buffer.captured_frames;
        if(total-cursor>ring){cursor=total-ring;discontinuity=true;}begin=cursor;count=std::min<std::uint64_t>(total-cursor,std::max(1U,rate/100));if(!count)return UINT64_C(0);
        bytes.resize(static_cast<std::size_t>(count)*frame_bytes);for(std::uint64_t n=0;n<count;++n)std::copy_n(buffer.pcm.begin()+static_cast<std::ptrdiff_t>(((cursor+n)%ring)*frame_bytes),frame_bytes,bytes.begin()+static_cast<std::ptrdiff_t>(n*frame_bytes));cursor+=count;
    }
    const auto sample=bytes_sample(bytes,begin*10000000U/rate,count*10000000U/rate);if(discontinuity)set(*object(sample,mf::Sample),mf::MFSampleExtension_Discontinuity,value32(1));return sample;
};
const auto hold_job=[&](const std::shared_ptr<Object>& target){if(!target->job_held){hold(target->pointers.at(target->kind));target->job_held=true;}};
const auto release_job=[&](const std::shared_ptr<Object>& target){if(target->job_held){target->job_held=false;release(target->pointers.at(target->kind));}};
const auto encoder_config=[&](const Object& type){
    video::EncoderConfig config;
    const auto extent=get(type,mf::MF_MT_FRAME_SIZE,21).scalar,rate=get(type,mf::MF_MT_FRAME_RATE,21).scalar;
    config.width=static_cast<std::uint32_t>(extent>>32);config.height=static_cast<std::uint32_t>(extent);
    config.frame_rate_numerator=static_cast<std::uint32_t>(rate>>32);config.frame_rate_denominator=static_cast<std::uint32_t>(rate);
    config.bitrate=static_cast<std::uint32_t>(get(type,mf::MF_MT_AVG_BITRATE,19).scalar);
    require(get(type,mf::MF_MT_INTERLACE_MODE,19).scalar==2,0xc00d36b4U);
    if(const auto found=type.attributes.find(mf::MF_MT_MPEG2_PROFILE);found!=type.attributes.end()){require(found->second.type==19,type_error);config.profile=static_cast<unsigned>(found->second.scalar);}
    if(const auto found=type.attributes.find(mf::MF_MT_MPEG2_LEVEL);found!=type.attributes.end())require(found->second.type==19 && found->second.scalar==UINT32_MAX,0xc00d36b4U);
    if(const auto found=type.attributes.find(mf::MF_MT_PIXEL_ASPECT_RATIO);found!=type.attributes.end())require(found->second.type==21 && found->second.scalar==UINT64_C(0x100000001),0xc00d36b4U);
    return config;
};
const auto copy_media_type=[&](std::uint64_t original){
    const auto p=make(mf::MediaType);auto copy=object(p,mf::MediaType);
    for(const auto& [key,value]:object(original,mf::MediaType)->attributes)set(*copy,key,value);
    return p;
};
const auto media_time=[](video::Timestamp stamp)->std::optional<std::uint64_t>{
    if(!stamp.timescale)return std::nullopt;
    const auto scaled=static_cast<__int128>(stamp.value)*10000000/stamp.timescale;
    if(scaled<INT64_MIN || scaled>INT64_MAX)throw Failure{invalid};
    return std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(scaled));
};
const auto drain_codec=[&](const std::shared_ptr<Object>& target){
    if(target->identifier==3){const auto encoder=target->encoder;target->encoding=std::async(std::launch::async,[encoder]{return encoder->drain();});}
    else {const auto decoder=target->decoder;target->decoding=std::async(std::launch::async,[decoder]{return decoder->drain();});}
    target->codec_drained=true;
};
const auto video_type=[&](const std::shared_ptr<Object>& transform,bool input){
    const auto p=make(mf::MediaType);auto type=object(p,mf::MediaType);set(*type,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Video));const bool compressed=transform->identifier==3?!input:input;set(*type,mf::MF_MT_SUBTYPE,valueguid(compressed?mf::h264:mf::nv12));set(*type,mf::MF_MT_COMPRESSED,value32(compressed));set(*type,mf::MF_MT_FIXED_SIZE_SAMPLES,value32(!compressed));
    if(transform->identifier==3){
        if(transform->output_type){const auto output=object(transform->output_type,mf::MediaType);for(const auto key:{mf::MF_MT_FRAME_SIZE,mf::MF_MT_FRAME_RATE,mf::MF_MT_INTERLACE_MODE})if(output->attributes.contains(key))set(*type,key,output->attributes.at(key));}
        if(input && transform->output_type){const auto extent=get(*type,mf::MF_MT_FRAME_SIZE,21).scalar;set(*type,mf::MF_MT_DEFAULT_STRIDE,value32(static_cast<std::uint32_t>(extent>>32)));set(*type,mf::MF_MT_SAMPLE_SIZE,value32(static_cast<std::uint32_t>((extent>>32)*(extent&UINT32_MAX)*3/2)));}
        return p;
    }
    if(!input){
        std::uint64_t extent=0;if(transform->decoder)extent=(static_cast<std::uint64_t>(transform->decoder->width())<<32)|transform->decoder->height();
        else if(transform->current_type){const auto source=object(transform->current_type,mf::MediaType);const auto found=source->attributes.find(mf::MF_MT_FRAME_SIZE);if(found!=source->attributes.end() && found->second.type==21)extent=found->second.scalar;}
        if(extent){set(*type,mf::MF_MT_FRAME_SIZE,value64(extent));set(*type,mf::MF_MT_DEFAULT_STRIDE,value32(static_cast<std::uint32_t>(extent>>32)));const auto size=(extent>>32)*(extent&UINT32_MAX)*3/2;require(size<=UINT32_MAX);set(*type,mf::MF_MT_SAMPLE_SIZE,value32(static_cast<std::uint32_t>(size)));}
    }return p;
};
