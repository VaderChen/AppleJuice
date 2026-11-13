// MP4 媒體來源：沿用 Source／Stream／Presentation 物件與同一份 byte stream 讀取路徑，取樣表由 mf_mp4_demux.hpp 提供。
const auto source_bytes=[&](const FileSourceState& file,std::uint64_t at,std::size_t size){
    const auto position=file.base+at;
    if(file.external_stream)return external_stream_samples(file.external_stream,position,size);
    std::vector<std::uint8_t> bytes(size);auto remaining=std::span(bytes);auto cursor=position;
    while(!remaining.empty()){const auto r=file.file->read(remaining,cursor);require(!r.error && r.bytes && r.bytes<=remaining.size(),0x80070000U|((r.error?r.error:38)&0xffffU));cursor+=r.bytes;remaining=remaining.subspan(r.bytes);}
    return bytes;
};
const auto movie_media_type=[&](const mf::Mp4Track& track){
    const auto p=make(mf::MediaType);auto type=object(p,mf::MediaType);
    const auto generic=[&]{const auto c=track.codec;return mf::guid((c>>24)|((c>>8)&0xff00U)|((c<<8)&0xff0000U)|(c<<24),0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71});};
    const auto seconds=std::max<std::int64_t>(track.duration,1);
    const auto bitrate=static_cast<std::uint64_t>(static_cast<__int128>(track.total_bytes)*80000000/seconds);
    if(track.video){
        const bool avc=track.codec==mf::fourcc('a','v','c','1') || track.codec==mf::fourcc('a','v','c','3');
        const bool hevc=track.codec==mf::fourcc('h','v','c','1') || track.codec==mf::fourcc('h','e','v','1');
        set(*type,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Video));set(*type,mf::MF_MT_SUBTYPE,valueguid(avc?mf::h264:hevc?mf::hevc:generic()));
        set(*type,mf::MF_MT_FRAME_SIZE,value64((std::uint64_t{track.width}<<32)|track.height));
        if(track.rate_num)set(*type,mf::MF_MT_FRAME_RATE,value64((std::uint64_t{track.rate_num}<<32)|track.rate_den));
        set(*type,mf::MF_MT_PIXEL_ASPECT_RATIO,value64((std::uint64_t{track.par_num}<<32)|track.par_den));
        set(*type,mf::MF_MT_INTERLACE_MODE,value32(2));set(*type,mf::MF_MT_COMPRESSED,value32(1));set(*type,mf::MF_MT_FIXED_SIZE_SAMPLES,value32(0));set(*type,mf::MF_MT_ALL_SAMPLES_INDEPENDENT,value32(0));
        set(*type,mf::MF_MT_AVG_BITRATE,value32(static_cast<std::uint32_t>(std::min<std::uint64_t>(bitrate,UINT32_MAX))));
        if(avc && track.avcc.size()>=7){
            set(*type,mf::MF_MT_MPEG2_PROFILE,value32(track.avcc[1]));set(*type,mf::MF_MT_MPEG2_LEVEL,value32(track.avcc[3]));
            Value header;header.type=0x1011;header.blob=mf::h264_annex_header(mf::h264_config(track.avcc).parameter_sets);set(*type,mf::MF_MT_MPEG_SEQUENCE_HEADER,std::move(header));
        }
    }else{
        const bool mp4a=track.codec==mf::fourcc('m','p','4','a');
        const bool aac=mp4a && (track.object_type==0x40 || track.object_type==0x66 || track.object_type==0x67 || track.object_type==0x68);
        const bool mp3=mp4a && (track.object_type==0x69 || track.object_type==0x6b);
        set(*type,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Audio));set(*type,mf::MF_MT_SUBTYPE,valueguid(aac?mf::aac:mp3?mf::guid(0x55,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}):generic()));
        set(*type,mf::MF_MT_AUDIO_NUM_CHANNELS,value32(track.channels));set(*type,mf::MF_MT_AUDIO_SAMPLES_PER_SECOND,value32(track.sample_rate));set(*type,mf::MF_MT_AUDIO_BITS_PER_SAMPLE,value32(track.bits?track.bits:16));
        set(*type,mf::MF_MT_AUDIO_BLOCK_ALIGNMENT,value32(1));set(*type,mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND,value32(static_cast<std::uint32_t>(std::min<std::uint64_t>(bitrate/8,UINT32_MAX))));
        set(*type,mf::MF_MT_COMPRESSED,value32(1));set(*type,mf::MF_MT_FIXED_SIZE_SAMPLES,value32(0));set(*type,mf::MF_MT_ALL_SAMPLES_INDEPENDENT,value32(1));
        if(aac){
            set(*type,mf::MF_MT_AAC_PAYLOAD_TYPE,value32(0));set(*type,mf::MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION,value32(0x29));
            // HEAACWAVEINFO 尾段接 AudioSpecificConfig。
            Value user;user.type=0x1011;user.blob.assign(12,0);user.blob[2]=0xfe;user.blob.insert(user.blob.end(),track.audio_config.begin(),track.audio_config.end());set(*type,mf::MF_MT_USER_DATA,std::move(user));
        }
    }
    return p;
};
const auto movie_source_create=[&](std::shared_ptr<FileSourceState> file,std::uint64_t external){
    const auto movie=file->movie;const auto p=make(mf::Source),presentation=make(mf::Presentation);
    auto source=object(p,mf::Source);source->file_source=std::move(file);source->parent=external;hold(external);source->children={presentation};
    auto pd=object(presentation,mf::Presentation);
    for(const auto& track:movie->tracks){
        const auto type=movie_media_type(track),descriptor=make(mf::Stream);auto sd=object(descriptor,mf::Stream);
        sd->identifier=track.id;sd->children={type};sd->current_type=type;hold(type);pd->children.push_back(descriptor);pd->selected.push_back(true);
    }
    set(*pd,mf::MF_PD_DURATION,value64(static_cast<std::uint64_t>(movie->duration)));return p;
};
// 取得目前串流的下一個壓縮取樣；客體 byte stream 讀取會重入，游標只在讀取完成後前進。
const auto movie_sample=[&](const std::shared_ptr<Object>& source,Object& stream){
    const auto& file=*source->file_source;const auto& track=file.movie->tracks.at(stream.identifier);
    if(stream.read_frames>=track.samples.size())return UINT64_C(0);
    const auto& packet=track.samples[static_cast<std::size_t>(stream.read_frames)];const auto bytes=source_bytes(file,packet.offset,packet.size);
    const auto sample=bytes_sample(bytes,std::bit_cast<std::uint64_t>(packet.time),std::uint64_t{packet.duration});
    if(packet.key)set(*object(sample,mf::Sample),mf::MFSampleExtension_CleanPoint,value32(1));
    ++stream.read_frames;return sample;
};
const auto movie_source_start=[&](const std::shared_ptr<Object>& source,std::uint64_t presentation_pointer,std::uint64_t format,std::uint64_t position_pointer){
    const auto presentation=object(presentation_pointer,mf::Presentation);const auto original=object(source->children.at(0),mf::Presentation);
    require(presentation->children==original->children,0xc00d36b2U);require(!format || guid(format)==Guid{},0xc00d36c5U);
    const auto position=read_value(position_pointer);require(position.type==0 || (position.type==20 && std::bit_cast<std::int64_t>(position.scalar)>=0),0xc00d36b2U);
    const bool seek=position.type==20,was_streaming=source->streaming;
    // ASF：解多工由整個來源共用，跳轉（或從停止狀態重新開始）先完成；客體 byte stream 的讀取可能重新進入本呼叫，須在送出任何事件之前。
    if(source->file_source->asf){
        if(seek || !was_streaming)(void)asf_io(source->file_source,AsfOp::Seek,0,seek?std::bit_cast<std::int64_t>(position.scalar):0);
        asf_select(source->file_source,presentation->selected);
    }
    source_start(source);
    std::vector<std::uint64_t> active;
    for(std::size_t n=0;n<presentation->children.size();++n){
        if(!presentation->selected[n])continue;const auto descriptor=presentation->children[n];
        const auto found=std::find_if(source->media_streams.begin(),source->media_streams.end(),[&](std::uint64_t p){return object(p,mf::MediaStream)->children.at(0)==descriptor;});
        std::uint64_t pointer=0;Value value;value.type=13;
        if(found==source->media_streams.end()){
            pointer=make(mf::MediaStream);auto stream=object(pointer,mf::MediaStream);stream->identifier=static_cast<std::uint32_t>(n);stream->parent=source->pointers.at(mf::Source);hold(stream->parent);stream->children={descriptor};hold(descriptor);
            source->media_streams.push_back(pointer);value.scalar=pointer;queue_event(source,205,value);
        }else{pointer=*found;value.scalar=pointer;queue_event(source,206,value);}
        if(seek){
            auto stream=object(pointer,mf::MediaStream);
            if(source->file_source->movie){
                const auto& samples=source->file_source->movie->tracks.at(stream->identifier).samples;std::uint64_t key=0;const auto at=std::bit_cast<std::int64_t>(position.scalar);
                for(std::size_t i=0;i<samples.size();++i)if(samples[i].key && samples[i].time<=at)key=i;
                stream->read_frames=key;
            }
            stream->flags&=~1U;
        }else if(!was_streaming){auto stream=object(pointer,mf::MediaStream);stream->read_frames=0;stream->flags&=~1U;}
        active.push_back(pointer);
    }
    queue_event(source,seek && was_streaming?203:201);
    for(const auto pointer:active)queue_event(object(pointer,mf::MediaStream),seek?204:202);
};
