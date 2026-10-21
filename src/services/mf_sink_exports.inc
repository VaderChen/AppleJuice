if(name=="MFCreateSinkWriterFromURL"){
    out(arg(3));require(!bridge.startups.empty(),not_initialized);require(arg(0) || arg(1),invalid);
    const auto path=arg(0)?read_utf16(memory,GuestAddress{arg(0)}):std::u16string{};
    require(!path.empty() || (arg(1) && arg(2)));
    Guid container{};std::uint64_t callback=0;
    if(arg(2)){
        const auto attributes=entry(arg(2)).object;require(mf::attributes(attributes->kind),no_interface);
        if(const auto found=attributes->attributes.find(mf::MF_TRANSCODE_CONTAINERTYPE);found!=attributes->attributes.end()){require(found->second.type==72,type_error);container=found->second.id;}
        if(const auto found=attributes->attributes.find(mf::MF_SINK_WRITER_ASYNC_CALLBACK);found!=attributes->attributes.end()){require(found->second.type==13,type_error);callback=found->second.scalar;require(callback!=0,pointer_error);}
    }
    if(container==Guid{}){auto folded=path;for(auto& c:folded)if(c>=u'A' && c<=u'Z')c+=u'a'-u'A';if(folded.ends_with(u".wav") || folded.ends_with(u".wave"))container=mf::MFTranscodeContainerType_WAVE;else if(folded.ends_with(u".mp4") || folded.ends_with(u".m4v"))container=mf::MFTranscodeContainerType_MPEG4;}
    require(container==mf::MFTranscodeContainerType_WAVE || container==mf::MFTranscodeContainerType_MPEG4,0xc00d36c4U);
    const auto sink=std::make_shared<SinkWriterState>();sink->mp4=container==mf::MFTranscodeContainerType_MPEG4;
    if(arg(1)){
        sink->stream=object(arg(1),mf::ByteStream)->byte_stream;require(!sink->stream->closed,shutdown);
        require((sink->stream->capabilities&6)==6,0x80070005U);sink->base=sink->stream->position;
    }else{
        const auto opened=filesystem_.open_host(utf16_to_utf8(path),0x40000000U,2,0,1);
        require(!opened.error && bool(opened.file),sink_io_error(opened.error?opened.error:2));
        sink->stream=std::make_shared<ByteStreamState>();sink->stream->file=opened.file;sink->stream->capabilities=6;
    }
    const auto p=make(mf::Writer);auto writer=object(p,mf::Writer);writer->writer=sink;writer->callback=callback;hold(callback);writer->parent=arg(1);hold(arg(1));put(arg(3),p,bits);
}else if(name=="PipelinePoll" && entry(arg(0)).object->kind==mf::Writer){
    const auto writer=object(arg(0),mf::Writer);writer->poll_queued=false;
    while(!writer->writer->notices.empty()){
        const auto notice=writer->writer->notices.front();writer->writer->notices.pop_front();
        if(notice.final)callbacks.push_back({foreign_method(writer->callback,3),{writer->callback,notice.status}});
        else callbacks.push_back({foreign_method(writer->callback,4),{writer->callback,0,notice.context}});
    }
    release_job(writer);
}
