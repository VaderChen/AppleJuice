if(name=="MFCreateFile"){
    out(arg(4));require(d(0)>=1 && d(0)<=3 && d(1)<=4 && !(d(2)&~3U));require(arg(3)!=0,pointer_error);
    const auto path=read_utf16(memory,GuestAddress{arg(3)});require(!path.empty());
    constexpr std::array<std::uint32_t,5> dispositions{3,1,5,4,2};
    const auto access=(d(0)&1?0x80000000U:0U)|(d(0)&2?0x40000000U:0U);
    const auto opened=filesystem_.open_host(utf16_to_utf8(path),access,dispositions[d(1)],d(2)&1?0x20000000U:0U,d(2)&2?3U:1U);
    require(!opened.error && bool(opened.file),sink_io_error(opened.error?opened.error:2));
    auto stream=std::make_shared<ByteStreamState>();stream->file=opened.file;stream->capabilities=d(0)|4U|0x800U|(d(2)&2?0x400U:0U);
    const auto p=make(mf::ByteStream);object(p,mf::ByteStream)->byte_stream=std::move(stream);put(arg(4),p,bits);
}else if(name=="PipelinePoll" && entry(arg(0)).object->kind==mf::ByteStream){
    const auto stream=object(arg(0),mf::ByteStream);stream->poll_queued=false;
    while(!stream->events.empty()){
        const auto p=stream->events.front();stream->events.pop_front();const auto completion=object(p,mf::Async);
        callbacks.push_back({foreign_method(completion->callback,4),{completion->callback,p}});
        callbacks.push_back({resolve("internal-mf","Release").value,{p}});
    }
    release_job(stream);
}
