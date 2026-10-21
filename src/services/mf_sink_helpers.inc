// Sink writer 共用 MF 的引用、媒體型別、sandbox 檔案與工作回呼流程。
const auto sink_pcm_format=[&](const Object& type){
    require(get(type,mf::MF_MT_MAJOR_TYPE,72).id==mf::MFMediaType_Audio,0xc00d36b4U);
    const auto subtype=get(type,mf::MF_MT_SUBTYPE,72).id;
    require(subtype==mf::pcm || subtype==mf::ieee,0xc00d36b4U);
    const auto number=[&](const Guid& key){return static_cast<unsigned>(get(type,key,19).scalar);};
    PcmWaveFormat format{number(mf::MF_MT_AUDIO_NUM_CHANNELS),number(mf::MF_MT_AUDIO_SAMPLES_PER_SECOND),number(mf::MF_MT_AUDIO_BITS_PER_SAMPLE),number(mf::MF_MT_AUDIO_BLOCK_ALIGNMENT),subtype==mf::ieee};
    require(number(mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND)==std::uint64_t{format.rate}*format.align,0xc00d36b4U);
    require(encode_pcm_wave_file(format,{}).has_value(),0xc00d36b4U);
    return format;
};
const auto sink_same_format=[](const PcmWaveFormat& a,const PcmWaveFormat& b){return a.channels==b.channels && a.rate==b.rate && a.bits==b.bits && a.align==b.align && a.floating==b.floating;};
const auto sink_io_error=[](std::uint32_t error){return error?0x80070000U|(error&0xffffU):0U;};
const auto sink_write=[&](SinkWriterState& sink,std::span<const std::uint8_t> bytes,std::uint64_t position){
    require(!sink.failure,sink.failure);
    require(!sink.stream->closed,shutdown);require(position<=INT64_MAX-sink.base && bytes.size()<=INT64_MAX-sink.base-position);position+=sink.base;
    while(!bytes.empty()){
        const auto written=sink.stream->file->write(bytes,position);
        if(written.error || !written.bytes){sink.failure=sink_io_error(written.error?written.error:29);throw Failure{sink.failure};}
        require(written.bytes<=bytes.size(),0x80004005U);position+=written.bytes;bytes=bytes.subspan(static_cast<std::size_t>(written.bytes));
    }
    sink.stream->position=position;
};
const auto sink_flush=[&](SinkWriterState& sink){require(!sink.stream->closed,shutdown);const auto error=sink.stream->file->flush();if(error){sink.failure=sink_io_error(error);throw Failure{sink.failure};}};
const auto sink_packet=[&](SinkWriterState& sink,const video::EncodedPacket& packet){
    const auto pts=media_time(packet.pts),dts=media_time(packet.dts),duration=media_time(packet.duration);
    require(pts && dts && duration && *duration && *duration<=UINT32_MAX,0xc00d36b4U);
    require(!packet.avcc.empty() && packet.avcc.size()<=UINT32_MAX && sink.video_samples.size()<10000000,0x8007000eU);
    const auto decoded=std::bit_cast<std::int64_t>(*dts);require(sink.video_samples.empty() || decoded>sink.video_samples.back().dts,0xc00d36b4U);
    if(!packet.parameter_sets.empty()){require(sink.parameters.empty() || sink.parameters==packet.parameter_sets,0xc00d36b4U);sink.parameters=packet.parameter_sets;}
    require(sink.parameters.size()>=2,0xc00d36b4U);
    if(sink.video_samples.size()==sink.video_samples.capacity())sink.video_samples.reserve(std::max<std::size_t>(16,sink.video_samples.size()*2));
    sink_write(sink,packet.avcc,sink.header_size+sink.bytes);
    sink.video_samples.push_back({sink.base+sink.header_size+sink.bytes,static_cast<std::uint32_t>(packet.avcc.size()),static_cast<std::uint32_t>(*duration),std::bit_cast<std::int64_t>(*pts),decoded,packet.keyframe});
    sink.bytes+=packet.avcc.size();if(sink.encode){++sink.encoded;sink.last_encoded=*pts;}
};
const auto sink_finalize=[&](SinkWriterState& sink){
    require(!sink.failure,sink.failure);
    if(sink.mp4){
        if(sink.encoder)for(const auto& packet:sink.encoder->drain())sink_packet(sink,packet);
        const auto movie=mp4::movie(sink.video->width,sink.video->height,sink.parameters,sink.video_samples);
        sink_write(sink,movie,sink.header_size+sink.bytes);mp4::Boxes size;size.number(sink.bytes+16,8);sink_write(sink,size.bytes,40);sink_flush(sink);
        sink.stream->position=sink.base+sink.header_size+sink.bytes+movie.size();return;
    }
    auto header=*encode_pcm_wave_file(*sink.output,{});
    const auto patch=[&](unsigned offset,std::uint32_t value){for(unsigned n=0;n<4;++n)header[offset+n]=static_cast<std::uint8_t>(value>>(8*n));};
    patch(4,static_cast<std::uint32_t>(header.size()+sink.bytes+(sink.bytes&1U)-8));
    patch(static_cast<unsigned>(header.size()-4),static_cast<std::uint32_t>(sink.bytes));
    if(sink.output->floating)patch(46,static_cast<std::uint32_t>(sink.bytes/sink.output->align));
    if(sink.bytes&1U)sink_write(sink,std::array<std::uint8_t,1>{0},header.size()+sink.bytes);
    sink_write(sink,header,0);sink_flush(sink);
    sink.stream->position=sink.base+header.size()+sink.bytes+(sink.bytes&1U);
};
const auto sink_notice=[&](const std::shared_ptr<Object>& writer,bool final,std::uint64_t context,std::uint32_t status){
    if(!writer->callback)return;require(writer->writer->notices.size()<65536,0x8007000eU);
    writer->writer->notices.push_back({final,context,status});hold_job(writer);
};
