// 媒體工作階段：解析拓撲、驅動呈現時鐘與宿主解碼；所有客體回呼都排入 MF 工作執行緒，單一 session 同時只有一批 sink 回呼。
const auto session_unknown=[](std::uint64_t p){Value value;if(p){value.type=13;value.scalar=p;}return value;};
const auto session_post=[&](const std::shared_ptr<Object>& target,std::uint32_t type,std::uint32_t status=0,Value value=Value{}){
    require(target->events.size()<65536,0x8007000eU);const auto p=event(type,{},status,std::move(value));target->events.push_back(p);return object(p,mf::Event);
};
const auto session_topology_status=[&](const std::shared_ptr<Object>& target,std::uint64_t topology,std::uint32_t status){
    auto notice=session_post(target,111,0,session_unknown(topology));set(*notice,mf::MF_EVENT_TOPOLOGY_STATUS,value32(status));
};
const auto session_sync_job=[&](const std::shared_ptr<Object>& item){
    const auto& s=*item->session;const auto setup=std::any_of(s.media.begin(),s.media.end(),[](const auto& m){return m->qi_pending || m->read_pending;});
    if(item->poll_queued || item->event_dispatching || (!item->closed && (!s.commands.empty() || s.clock_state==1 || setup || (item->callback && !item->events.empty()))))hold_job(item);else release_job(item);
};
const auto session_sink_live=[&](const SessionMedia& m){return m.callback && !object(m.activate,mf::Activate)->closed;};
const auto session_notify=[&](SessionState& s,unsigned slot,const std::vector<std::uint64_t>& extra,std::uint32_t wide,std::uint32_t floating=0){
    std::vector<std::uint64_t> sinks;for(const auto& m:s.media)if(session_sink_live(*m))sinks.push_back(m->callback);sinks.insert(sinks.end(),s.clock_sinks.begin(),s.clock_sinks.end());
    for(const auto sink:sinks){
        std::vector<std::uint64_t> arguments{sink};arguments.insert(arguments.end(),extra.begin(),extra.end());
        GuestCallback call{foreign_method(sink,slot),std::move(arguments)};call.wide_arguments=wide;call.floating_arguments=floating;callbacks.push_back(std::move(call));
    }
};
const auto session_device_play=[&](SessionState& s,bool playing){for(const auto& m:s.media)if(m->device){std::lock_guard lock(m->device->mutex);m->device->playing=playing;}};
const auto session_apply_volume=[&](SessionState& s){
    const auto gain=s.muted?0.F:s.master_volume;
    for(const auto& m:s.media)if(m->device){std::lock_guard lock(m->device->mutex);m->device->left=gain*s.channel_volume[0];m->device->right=gain*s.channel_volume[1];}
};
const auto session_clock_start=[&](SessionState& s,std::int64_t position){
    const auto now=runtime_.clock_ticks;s.anchor_system=now;s.anchor_time=position;s.clock_state=1;++s.continuity;session_device_play(s,true);
    session_notify(s,mf::sink_clock_start,{now,std::bit_cast<std::uint64_t>(position)},6);
};
const auto session_clock_restart=[&](SessionState& s){const auto now=runtime_.clock_ticks;s.anchor_system=now;s.clock_state=1;session_device_play(s,true);session_notify(s,mf::sink_clock_restart,{now},2);};
const auto session_clock_pause=[&](SessionState& s){const auto now=runtime_.clock_ticks;s.anchor_time=s.clock_time(now);s.anchor_system=now;s.clock_state=3;session_device_play(s,false);session_notify(s,mf::sink_clock_pause,{now},2);};
const auto session_clock_stop=[&](SessionState& s){const auto now=runtime_.clock_ticks;s.anchor_time=0;s.anchor_system=now;s.clock_state=2;s.scrub=false;session_device_play(s,false);session_notify(s,mf::sink_clock_stop,{now},2);};
const auto session_seek=[&](SessionMedia& m,std::int64_t position){
    m.target=m.last_end=position;m.read_pending=m.packet_ready=false;m.packet.clear();m.demux_done=m.drain_submitted=m.drained=false;m.discontinuity=true;
    if(m.video){
        m.frames.clear();const auto& samples=m.track->samples;std::uint64_t key=0;
        for(std::size_t n=0;n<samples.size();++n)if(samples[n].key && samples[n].time<=position)key=n;
        m.next_packet=key;const auto& order=m.track->presentation;m.presented=static_cast<std::size_t>(std::lower_bound(order.begin(),order.end(),position)-order.begin());
        if(m.decoder->decoder)m.decoder->flush=true;
        return;
    }
    m.chunks.clear();m.out_frames=0;if(m.transcoder)m.transcoder->reset();
    if(m.device){std::lock_guard lock(m.device->mutex);m.device->pcm.clear();m.device->cursor=0;}
    if(m.track){
        const auto& samples=m.track->samples;
        const auto after=std::upper_bound(samples.begin(),samples.end(),position,[](std::int64_t value,const mf::Mp4Sample& sample){return value<sample.time;});
        auto index=after==samples.begin()?std::size_t{0}:static_cast<std::size_t>(after-samples.begin())-1;if(index)--index; // 前一封包提供 MDCT 重疊。
        m.next_packet=index;m.base_time=samples[index].time;
    }else{
        const auto& wave=m.file->wave;const auto frames=wave.bytes/wave.format.align;
        m.next_packet=static_cast<std::uint64_t>(std::min<__int128>(static_cast<__int128>(position)*wave.format.rate/10000000,frames));
        m.base_time=static_cast<std::int64_t>(static_cast<__int128>(m.next_packet)*10000000/wave.format.rate);
    }
};
const auto session_discard_media=[&](SessionMedia& m,bool shutdown_sink){
    if(m.decoding.valid())try{(void)m.decoding.get();}catch(const std::exception&){}
    if(m.qi_pending && m.guest_block){const auto extra=memory.load(GuestAddress{m.guest_block+32},bits);if(extra)callbacks.push_back({foreign_method(extra,2),{extra}});}
    m.qi_pending=m.read_pending=false;
    if(m.device){{std::lock_guard lock(m.device->mutex);m.device->stop();}m.device_output.reset();m.device.reset();}
    if(m.activate){
        auto activate=object(m.activate,mf::Activate);
        if(shutdown_sink && m.shutdown_on_remove && !activate->closed){activate->closed=true;if(m.callback)callbacks.push_back({foreign_method(m.callback,mf::sink_shutdown),{m.callback}});}
    }
    if(m.guest_block)(void)free_heap(memory,m.guest_block,state);if(m.guest_buffer)(void)free_heap(memory,m.guest_buffer,state);m.guest_block=m.guest_buffer=0;m.guest_capacity=0;
    release(m.attributes);release(m.activate);release(m.descriptor);release(m.source);m.attributes=m.activate=m.descriptor=m.source=m.callback=0;
    m.frames.clear();m.chunks.clear();m.transcoder.reset();m.decoder.reset();
};
const auto session_clear_media=[&](SessionState& s,bool shutdown_sink){const auto media=std::move(s.media);s.media.clear();for(const auto& m:media)session_discard_media(*m,shutdown_sink);};
// Sink 由 activate 提供：sample grabber 與音訊 renderer。來源必須是內建 Source，解碼路徑由 session 自行接上。
const auto session_resolve=[&](std::uint64_t topology_pointer){
    std::vector<std::shared_ptr<SessionMedia>> built;
    try{
        auto topology=object(topology_pointer,mf::Topology);
        for(const auto pointer:topology->children){
            auto node=object(pointer,mf::TopologyNode);if(node->node_type!=0)continue;
            require(!node->inputs.empty() && node->inputs.front().node,0xc00d5214U);
            auto upstream=object(node->inputs.front().node,mf::TopologyNode);require(upstream->node_type==1,0xc00d5214U);
            const auto unknown=[&](const Guid& key,std::uint32_t code){const auto found=upstream->attributes.find(key);require(found!=upstream->attributes.end() && found->second.type==13 && found->second.scalar,code);return found->second.scalar;};
            const auto source_pointer=unknown(mf::MF_TOPONODE_SOURCE,0xc00d521aU);(void)unknown(mf::MF_TOPONODE_PRESENTATION_DESCRIPTOR,0xc00d5217U);const auto descriptor=unknown(mf::MF_TOPONODE_STREAM_DESCRIPTOR,0xc00d5218U);
            require(bridge.objects.contains(source_pointer) && bridge.objects.contains(descriptor),0xc00d5214U);auto source=object(source_pointer,mf::Source);require(!source->closed,shutdown);require(source->file_source!=nullptr,0xc00d5214U);
            require(node->result_object && bridge.objects.contains(node->result_object),0xc00d521bU);auto activate=object(node->result_object,mf::Activate);require(activate->identifier==4 || activate->identifier==5,0xc00d521bU);
            auto m=std::make_shared<SessionMedia>();m->file=source->file_source;m->renderer=activate->identifier==5;m->callback=activate->callback;
            const auto presentation=object(source->children.at(0),mf::Presentation);const auto listed=std::find(presentation->children.begin(),presentation->children.end(),descriptor);require(listed!=presentation->children.end(),0xc00d5218U);
            if(m->file->movie)m->track=&m->file->movie->tracks.at(static_cast<std::size_t>(listed-presentation->children.begin()));
            const auto native=object(object(descriptor,mf::Stream)->current_type,mf::MediaType);const auto major=get(*native,mf::MF_MT_MAJOR_TYPE,72).id;
            std::shared_ptr<Object> requested;if(activate->output_type)requested=object(activate->output_type,mf::MediaType);
            const auto find=[&](const Guid& key,std::uint32_t type)->const Value*{if(!requested)return nullptr;const auto found=requested->attributes.find(key);return found!=requested->attributes.end() && found->second.type==type?&found->second:nullptr;};
            if(const auto* value=find(mf::MF_MT_MAJOR_TYPE,72))require(value->id==major,0xc00d5212U);
            const auto subtype=find(mf::MF_MT_SUBTYPE,72)?find(mf::MF_MT_SUBTYPE,72)->id:Guid{};
            if(major==mf::MFMediaType_Video){
                m->video=true;require(!m->renderer && m->track && m->track->video && (m->track->codec==mf::fourcc('a','v','c','1') || m->track->codec==mf::fourcc('a','v','c','3')) && video::hardware_decode_supported(video::Codec::H264),0xc00d5212U);
                require(m->track->width && m->track->height,0xc00d36b4U);
                mf::VideoFormat format=mf::VideoFormat::Nv12;
                if(subtype==Guid{} || subtype==mf::nv12)format=mf::VideoFormat::Nv12;else if(subtype==mf::yv12)format=mf::VideoFormat::Yv12;else if(subtype==mf::iyuv || subtype==mf::i420)format=mf::VideoFormat::I420;
                else if(subtype==mf::yuy2)format=mf::VideoFormat::Yuy2;else if(subtype==mf::uyvy)format=mf::VideoFormat::Uyvy;else if(subtype==mf::rgb32 || subtype==mf::argb32)format=mf::VideoFormat::Bgra;else throw Failure{0xc00d5212U};
                const auto aligned=[](std::uint32_t value){return (value+15U)&~15U;};bool tight=false;
                if(const auto* extent=find(mf::MF_MT_FRAME_SIZE,21)){
                    const auto w=static_cast<std::uint32_t>(extent->scalar>>32),h=static_cast<std::uint32_t>(extent->scalar);const bool same=w==m->track->width && h==m->track->height,padded=w==aligned(m->track->width) && h==aligned(m->track->height);
                    require(same || padded,0xc00d5212U);tight=same && !padded;
                }
                const auto* stride=find(mf::MF_MT_DEFAULT_STRIDE,19);
                m->layout=mf::video_layout(format,m->track->width,m->track->height,tight,stride?static_cast<std::int32_t>(stride->scalar):0);
                auto config=mf::h264_config(m->track->avcc);require(config.parameter_sets.size()>=2,0xc00d36b4U);m->nal_length=config.nal_length_size;config.nal_length_size=4;
                m->decoder=std::make_shared<SessionVideoDecoder>();m->decoder->config=std::move(config);
            }else if(major==mf::MFMediaType_Audio){
                mf::AudioInput input;
                if(m->track){require(m->track->audio && get(*native,mf::MF_MT_SUBTYPE,72).id==mf::aac,0xc00d5212U);input.aac=true;input.config=m->track->audio_config;input.rate=m->track->sample_rate;input.channels=m->track->channels;}
                else{const auto& wave=m->file->wave.format;input.rate=wave.rate;input.channels=wave.channels;input.bits=wave.bits;input.align=wave.align;input.floating=wave.floating;}
                require(subtype==Guid{} || subtype==mf::pcm || subtype==mf::ieee,0xc00d5212U);
                const auto number=[&](const Guid& key,unsigned fallback){const auto* value=find(key,19);return value && value->scalar?static_cast<unsigned>(value->scalar):fallback;};
                const bool floating=subtype==mf::ieee;
                if(m->renderer)m->output={input.rate,2,32,true};
                else {m->output={number(mf::MF_MT_AUDIO_SAMPLES_PER_SECOND,input.rate),number(mf::MF_MT_AUDIO_NUM_CHANNELS,input.channels),number(mf::MF_MT_AUDIO_BITS_PER_SAMPLE,floating?32:16),floating};require(floating?m->output.bits==32:m->output.bits==8 || m->output.bits==16 || m->output.bits==24 || m->output.bits==32,0xc00d36b4U);}
                const bool identical=!input.aac && input.rate==m->output.rate && input.channels==m->output.channels && input.bits==m->output.bits && input.floating==m->output.floating;
                if(!identical){try{m->transcoder=std::make_shared<mf::AudioTranscoder>(input,m->output);}catch(const std::runtime_error&){throw Failure{0xc00d5212U};}catch(const std::invalid_argument&){throw Failure{0xc00d36b4U};}}
                if(m->renderer){
                    auto device=std::make_shared<audio::Buffer>();device->channels=2;device->bits=32;device->floating=true;device->rate=device->frequency=m->output.rate;device->streaming=true;
                    try{m->device_output=audio::open_output(device);}catch(const std::runtime_error&){throw Failure{0xc00d4e84U};}m->device=std::move(device);
                }
            }else throw Failure{0xc00d5212U};
            const auto ignore=activate->attributes.find(mf::MF_SAMPLEGRABBERSINK_IGNORE_CLOCK);m->ignore_clock=ignore!=activate->attributes.end() && ignore->second.type==19 && ignore->second.scalar;
            const auto offset=activate->attributes.find(mf::MF_SAMPLEGRABBERSINK_SAMPLE_TIME_OFFSET);if(offset!=activate->attributes.end() && offset->second.type==21)m->time_offset=std::bit_cast<std::int64_t>(offset->second.scalar);
            const auto keep=node->attributes.find(mf::MF_TOPONODE_NOSHUTDOWN_ON_REMOVE);m->shutdown_on_remove=keep!=node->attributes.end() && keep->second.type==19 && !keep->second.scalar;
            activate->closed=false;
            if(m->callback){
                m->guest_block=allocate(40);memory.write(GuestAddress{m->guest_block},major);memory.write(GuestAddress{m->guest_block+16},mf::iid_SampleGrabberCallback2);memory.store(GuestAddress{m->guest_block+32},bits,0);
                m->attributes=make(mf::Attributes);
            }
            hold(source_pointer);m->source=source_pointer;hold(descriptor);m->descriptor=descriptor;hold(node->result_object);m->activate=node->result_object;
            session_seek(*m,0);built.push_back(std::move(m));
        }
        require(!built.empty(),0xc00d5214U);
    }catch(...){for(const auto& m:built)session_discard_media(*m,false);throw;}
    return built;
};
const auto session_apply_topology=[&](const std::shared_ptr<Object>& item,std::uint64_t topology){
    auto& s=*item->session;std::vector<std::shared_ptr<SessionMedia>> built;std::uint32_t status=0;
    try{built=session_resolve(topology);}catch(const Failure& error){status=error.code;}catch(const std::bad_alloc&){status=0x8007000eU;}catch(const std::exception&){status=0x80004005U;}
    session_post(item,101,status,session_unknown(topology));
    if(status){release(topology);return;}
    if(s.clock_state==1 || s.clock_state==3)session_clock_stop(s);
    session_clear_media(s,true);release(s.topology);s.topology=topology;s.media=std::move(built);s.eop_sent=s.failed=false;session_apply_volume(s);
    const auto clock=create(item,mf::Clock);
    for(const auto& m:s.media){
        if(!m->callback)continue;
        m->qi_pending=true;
        callbacks.push_back({foreign_method(m->callback,0),{m->callback,m->guest_block+16,m->guest_block+32}});
        callbacks.push_back({foreign_method(m->callback,mf::sink_set_clock),{m->callback,clock}});
    }
    auto caps=session_post(item,110);set(*caps,mf::MF_EVENT_SESSIONCAPS,value32(0x57));set(*caps,mf::MF_EVENT_SESSIONCAPS_DELTA,value32(0x57));
    session_topology_status(item,topology,100);
};
const auto session_start=[&](const std::shared_ptr<Object>& item,const Value& position){
    auto& s=*item->session;
    if(s.closed_session || s.media.empty()){session_post(item,103,0xc00d36b2U);return;}
    if(position.type==0 && s.clock_state==3)session_clock_restart(s);
    else if(position.type!=0 || s.clock_state!=1){
        const auto at=position.type==20?std::bit_cast<std::int64_t>(position.scalar):0;
        for(const auto& m:s.media)session_seek(*m,at);
        s.eop_sent=s.failed=false;session_clock_start(s,at);session_topology_status(item,s.topology,200);
    }
    auto started=session_post(item,103);set(*started,mf::MF_EVENT_PRESENTATION_TIME_OFFSET,value64(0));
};
const auto session_execute=[&](const std::shared_ptr<Object>& item,const SessionCommand& command){
    auto& s=*item->session;
    switch(command.kind){
    case SessionCommand::SetTopology:
        if(s.closed_session){session_post(item,101,0xc00d36b2U,session_unknown(command.topology));release(command.topology);break;}
        if(command.flags&4U){
            if(s.clock_state==1 || s.clock_state==3)session_clock_stop(s);
            session_clear_media(s,true);release(s.topology);s.topology=0;
            if(!command.topology){session_post(item,101);break;}
        }
        if(!(command.flags&1U) && s.topology && (s.clock_state==1 || s.clock_state==3)){s.queued.push_back(command.topology);break;}
        session_apply_topology(item,command.topology);break;
    case SessionCommand::ClearTopologies:
        for(const auto p:s.queued)release(p);s.queued.clear();session_post(item,102);break;
    case SessionCommand::Start:session_start(item,command.position);break;
    case SessionCommand::Pause:
        if(s.clock_state!=1){session_post(item,104,0xc00d36b2U);break;}
        session_clock_pause(s);session_post(item,104);break;
    case SessionCommand::Stop:
        if(s.clock_state==1 || s.clock_state==3){session_clock_stop(s);for(const auto& m:s.media)session_seek(*m,0);}
        session_post(item,105);break;
    case SessionCommand::Close:
        if(s.clock_state==1 || s.clock_state==3)session_clock_stop(s);
        session_clear_media(s,true);release(s.topology);s.topology=0;for(const auto p:s.queued)release(p);s.queued.clear();s.closed_session=true;session_post(item,106);break;
    case SessionCommand::SetRate:{
        const auto now=runtime_.clock_ticks;s.anchor_time=s.clock_time(now);s.anchor_system=now;s.rate=command.rate;s.scrub=command.rate==0;
        for(const auto& m:s.media)if(m->device){std::lock_guard lock(m->device->mutex);m->device->frequency=static_cast<unsigned>(std::max(1.0,m->device->rate*static_cast<double>(command.rate)));}
        session_notify(s,mf::sink_clock_rate,{now,std::bit_cast<std::uint32_t>(command.rate)},2,4);
        Value value;value.type=4;value.scalar=std::bit_cast<std::uint32_t>(command.rate);session_post(item,108,0,value);break;}
    }
};
const auto session_fail=[&](const std::shared_ptr<Object>& item,std::uint32_t status){
    auto& s=*item->session;s.failed=true;for(const auto& m:s.media)m->read_pending=false;
    if(s.clock_state==1 || s.clock_state==3)session_clock_stop(s);
    session_post(item,1,status);
};
const auto session_fetch=[&](SessionMedia& m,std::uint64_t at,std::size_t size,std::vector<std::uint8_t>& bytes){
    require(!object(m.source,mf::Source)->closed,shutdown);
    if(!m.file->external_stream){bytes=source_bytes(*m.file,at,size);return true;}
    if(m.packet_ready && m.packet_at==at){bytes=std::move(m.packet);m.packet.clear();m.packet_ready=false;return true;}
    m.packet_ready=false;m.packet_at=at;m.packet_size=size;m.read_pending=true;return false;
};
const auto session_deliver=[&](SessionMedia& m,std::span<const std::uint8_t> bytes,std::int64_t time,std::uint64_t duration,bool clean){
    m.last_end=std::max(m.last_end,time+static_cast<std::int64_t>(duration));
    if(!session_sink_live(m) || bytes.empty())return;
    if(bytes.size()>m.guest_capacity){
        const auto replacement=allocate(bytes.size());if(m.guest_buffer)(void)free_heap(memory,m.guest_buffer,state);m.guest_buffer=replacement;m.guest_capacity=bytes.size();
    }
    memory.write(GuestAddress{m.guest_buffer},bytes);
    std::vector<std::uint64_t> arguments{m.callback,m.guest_block,0,std::bit_cast<std::uint64_t>(time),duration,m.guest_buffer,bytes.size()};
    if(m.has_ex){
        auto attributes=object(m.attributes,mf::Attributes);attributes->attributes.clear();
        if(clean)set(*attributes,mf::MFSampleExtension_CleanPoint,value32(1));if(m.discontinuity)set(*attributes,mf::MFSampleExtension_Discontinuity,value32(1));arguments.push_back(m.attributes);
    }
    m.discontinuity=false;
    GuestCallback call{foreign_method(m.callback,m.has_ex?mf::sink_process_sample_ex:mf::sink_process_sample),std::move(arguments)};call.wide_arguments=0x18;callbacks.push_back(std::move(call));
};
const auto session_append_audio=[&](SessionMedia& m,std::vector<std::uint8_t> bytes){
    const auto frame_bytes=m.output.frame_bytes();const auto frames=bytes.size()/frame_bytes;if(!frames)return;
    auto time=m.base_time+static_cast<std::int64_t>(static_cast<__int128>(m.out_frames)*10000000/m.output.rate);m.out_frames+=frames;
    const auto duration=static_cast<std::int64_t>(static_cast<__int128>(frames)*10000000/m.output.rate);
    if(time+duration<=m.target)return;
    if(time<m.target){
        const auto skip=static_cast<std::size_t>(static_cast<__int128>(m.target-time)*m.output.rate/10000000);
        bytes.erase(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(std::min(skip,frames)*frame_bytes));
        if(bytes.empty())return;
        time=m.target;
    }
    m.chunks.push_back({time,static_cast<std::uint64_t>(static_cast<__int128>(bytes.size()/frame_bytes)*10000000/m.output.rate),std::move(bytes)});
};
const auto session_pump_video=[&](const std::shared_ptr<Object>& item,SessionMedia& m,std::int64_t clock){
    auto& s=*item->session;
    if(m.decoding.valid() && m.decoding.wait_for(std::chrono::seconds(0))==std::future_status::ready){
        auto frames=m.decoding.get();if(m.drain_submitted)m.drained=true;
        for(auto& frame:frames)if(frame.time>=m.target)m.frames.insert_or_assign(frame.time,std::move(frame));
    }
    if(!m.decoding.valid() && !m.drain_submitted && m.frames.size()<SessionMedia::max_frames){
        const auto& samples=m.track->samples;
        if(m.next_packet>=samples.size()){m.drain_submitted=true;m.decoding=std::async(std::launch::async,session_decode_job,m.decoder,std::vector<std::uint8_t>{},std::int64_t{},std::uint64_t{},m.layout,m.target,true);}
        else {
            const auto& sample=samples[static_cast<std::size_t>(m.next_packet)];std::vector<std::uint8_t> bytes;
            if(session_fetch(m,sample.offset,sample.size,bytes)){
                auto packet=mf::h264_packet(bytes,m.nal_length);++m.next_packet;
                m.decoding=std::async(std::launch::async,session_decode_job,m.decoder,std::move(packet),sample.time,std::uint64_t{sample.duration},m.layout,m.target,false);
            }
        }
    }
    if(const auto* frame=m.next_frame();frame && m.frame_due(*frame,clock,s.scrub)){
        const auto time=frame->time;session_deliver(m,frame->bytes,time,frame->duration,true);
        const auto& order=m.track->presentation;m.presented=static_cast<std::size_t>(std::upper_bound(order.begin(),order.end(),time)-order.begin());m.frames.erase(time);
        if(s.scrub){s.scrub=false;session_post(item,109);}
    }
};
const auto session_pump_audio=[&](SessionMedia& m,std::int64_t clock){
    while(!m.demux_done && m.chunks.size()<SessionMedia::max_chunks){
        std::uint64_t at=0,advance=1;std::size_t size=0;bool done=false;
        if(m.track){
            if(m.next_packet>=m.track->samples.size())done=true;
            else {const auto& sample=m.track->samples[static_cast<std::size_t>(m.next_packet)];at=sample.offset;size=sample.size;}
        }else{
            const auto& wave=m.file->wave;const auto total=wave.bytes/wave.format.align;
            if(m.next_packet>=total)done=true;
            else {advance=std::min<std::uint64_t>(total-m.next_packet,std::max(1U,wave.format.rate/50));at=wave.offset+m.next_packet*wave.format.align;size=static_cast<std::size_t>(advance*wave.format.align);}
        }
        if(done){if(m.transcoder)session_append_audio(m,m.transcoder->drain());m.demux_done=true;break;}
        std::vector<std::uint8_t> bytes;if(!session_fetch(m,at,size,bytes))break;
        m.next_packet+=advance;session_append_audio(m,m.transcoder?m.transcoder->push(bytes):std::move(bytes));
    }
    if(m.device){
        while(!m.chunks.empty() && m.chunk_due(m.chunks.front(),clock)){
            auto& chunk=m.chunks.front();m.last_end=std::max(m.last_end,chunk.time+static_cast<std::int64_t>(chunk.duration));
            {std::lock_guard lock(m.device->mutex);const auto consumed=static_cast<std::size_t>(m.device->cursor);const auto frame=m.device->frame_bytes();
                if(consumed){m.device->pcm.erase(m.device->pcm.begin(),m.device->pcm.begin()+static_cast<std::ptrdiff_t>(consumed*frame));m.device->cursor-=static_cast<double>(consumed);}
                m.device->pcm.insert(m.device->pcm.end(),chunk.bytes.begin(),chunk.bytes.end());}
            m.chunks.pop_front();
        }
    }else if(!m.chunks.empty() && m.chunk_due(m.chunks.front(),clock)){
        auto chunk=std::move(m.chunks.front());m.chunks.pop_front();session_deliver(m,chunk.bytes,chunk.time,chunk.duration,true);
    }
};
const auto session_finish=[&](const std::shared_ptr<Object>& item){
    auto& s=*item->session;session_clock_stop(s);session_topology_status(item,s.topology,400);
    if(!s.queued.empty()){
        const auto next=s.queued.front();s.queued.pop_front();session_apply_topology(item,next);
        SessionCommand restart;restart.kind=SessionCommand::Start;s.commands.push_front(std::move(restart));return;
    }
    session_post(item,107);
};
const auto session_teardown=[&](const std::shared_ptr<Object>& item){
    auto& s=*item->session;if(s.torn_down)return;s.torn_down=true;
    for(const auto& command:s.commands)release(command.topology);s.commands.clear();
    if(s.clock_state==1 || s.clock_state==3)session_clock_stop(s);
    session_clear_media(s,true);release(s.topology);s.topology=0;for(const auto p:s.queued)release(p);s.queued.clear();
    for(const auto sink:s.clock_sinks)release(sink);s.clock_sinks.clear();release(s.time_source);s.time_source=0;
};
close_session=[&](const std::shared_ptr<Object>& item){session_teardown(item);};
// 客體 byte stream 讀取可能以服務續行重入：開頭只完成已登錄的讀取，其餘狀態更新都在讀取之後。
const auto session_pump=[&](const std::shared_ptr<Object>& item,std::uint64_t pointer){
    auto& s=*item->session;
    try{
        for(const auto& m:s.media)if(m->read_pending){auto bytes=source_bytes(*m->file,m->packet_at,m->packet_size);m->packet=std::move(bytes);m->read_pending=false;m->packet_ready=true;}
        if(!item->closed){
            for(const auto& m:s.media)if(m->qi_pending){const auto extra=memory.load(GuestAddress{m->guest_block+32},bits);m->qi_pending=false;m->has_ex=extra!=0;if(extra)callbacks.push_back({foreign_method(extra,2),{extra}});}
            while(!s.commands.empty() && callbacks.empty()){
                // 會重設管線的命令才等候並丟棄解碼中的畫面；其餘命令保留解碼結果。
                if(s.command_resets_pipeline(s.commands.front())){
                    if(!s.quiescent())break;
                    for(const auto& m:s.media)if(m->decoding.valid())try{(void)m->decoding.get();}catch(const std::exception&){}
                }
                const auto command=std::move(s.commands.front());s.commands.pop_front();session_execute(item,command);
            }
            if(s.commands.empty() && callbacks.empty() && s.clock_state==1 && !s.media.empty()){
                const auto clock=s.clock_time(runtime_.clock_ticks);
                for(const auto& m:s.media)if(m->video)session_pump_video(item,*m,clock);else session_pump_audio(*m,clock);
                const auto done=[&](const auto& test){return std::all_of(s.media.begin(),s.media.end(),test);};
                if(!s.eop_sent && done([](const auto& m){return m->source_done() && !m->decoding.valid();})){s.eop_sent=true;session_post(item,211);}
                if(callbacks.empty() && done([&](const auto& m){return m->finished(clock);}))session_finish(item);
            }
        }
    }catch(const Failure& error){session_fail(item,error.code);}
    catch(const std::bad_alloc&){session_fail(item,0x8007000eU);}
    catch(const MemoryFault&){session_fail(item,pointer_error);}
    catch(const std::exception&){session_fail(item,0x80004005U);}
    if(item->closed){item->poll_queued=false;session_teardown(item);session_sync_job(item);return;}
    if(callbacks.empty()){item->poll_queued=false;session_sync_job(item);}
    else callbacks.push_back({resolve("internal-mf","SessionPumpDone").value,{pointer}});
};
const auto float_argument=[&](unsigned index){
    if(x86_)return std::bit_cast<float>(d(index));
    float value=0;std::memcpy(&value,state.xmm[index].data(),4);return value;
};
const auto node_disconnect=[&](Object& node,std::uint32_t index){
    if(index>=node.outputs.size() || !node.outputs[index].node)return false;
    const auto link=node.outputs[index];node.outputs[index]={};
    if(bridge.objects.contains(link.node)){auto target=object(link.node,mf::TopologyNode);if(link.index<target->inputs.size())target->inputs[link.index]={};}
    release(link.node);return true;
};
const auto node_connect=[&](std::uint64_t upstream,std::uint32_t index,std::uint64_t downstream,std::uint32_t input){
    auto from=object(upstream,mf::TopologyNode);auto to=object(downstream,mf::TopologyNode);
    require(from->node_type!=0 && to->node_type!=1 && index<4096 && input<4096,0x80004005U);
    if(input<to->inputs.size() && to->inputs[input].node && bridge.objects.contains(to->inputs[input].node))(void)node_disconnect(*object(to->inputs[input].node,mf::TopologyNode),to->inputs[input].index);
    (void)node_disconnect(*from,index);
    if(from->outputs.size()<=index)from->outputs.resize(index+1);if(to->inputs.size()<=input)to->inputs.resize(input+1);
    hold(downstream);from->outputs[index]={downstream,input};to->inputs[input]={upstream,index};
};
const auto node_copy=[&](Object& target,const Object& from){
    for(const auto& [key,value]:target.attributes){(void)key;if(value.type==13)release(value.scalar);}target.attributes.clear();
    for(const auto& [key,value]:from.attributes)set(target,key,value);
    hold(from.result_object);release(target.result_object);target.result_object=from.result_object;target.topo_id=from.topo_id;
    const auto copy=[&](std::map<std::uint32_t,std::uint64_t>& to,const std::map<std::uint32_t,std::uint64_t>& source){for(const auto& [index,type]:to){(void)index;release(type);}to=source;for(const auto& [index,type]:to){(void)index;hold(type);}};
    copy(target.output_prefs,from.output_prefs);copy(target.input_prefs,from.input_prefs);
};
const auto topology_clear=[&](Object& topology){auto nodes=std::move(topology.children);topology.children.clear();for(const auto p:nodes)release(p);};
const auto topology_collection=[&](const Object& topology,std::uint32_t type){
    const auto p=make(mf::Collection);auto list=object(p,mf::Collection);
    for(const auto node:topology.children)if(object(node,mf::TopologyNode)->node_type==type){hold(node);list->children.push_back(node);}
    return p;
};
