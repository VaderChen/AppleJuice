if(name=="MFCreateMFByteStreamOnStream" || name=="MFCreateMFByteStreamOnStreamEx"){
    // Ex 的引數是 IUnknown：能查出 IStream 時走同一條路；引擎沒有 WinRT IRandomAccessStream 基礎，查不到時交回查詢的錯誤。
    const bool unknown=name.ends_with("Ex");out(arg(1));require(arg(0)!=0);
    const auto opened=guest_stream_io(arg(0),unknown?StreamOp::OpenUnknown:StreamOp::Open);
    if(unknown)require(opened.object!=0,opened.status&0x80000000U?opened.status:no_interface);
    auto stream=std::make_shared<ByteStreamState>();stream->guest_stream=unknown?opened.object:arg(0);
    stream->capabilities=5U|(!(opened.status&0x80000000U) && (opened.mode&3U)?2U:0U);
    std::uint64_t p=0;try{p=make(mf::ByteStream);}catch(...){if(unknown)release(opened.object);throw;}
    auto wrapper=object(p,mf::ByteStream);wrapper->parent=stream->guest_stream;if(!unknown)hold(arg(0));
    wrapper->byte_stream=std::move(stream);put(arg(1),p,bits);
}else if(name=="MFCreateStreamOnMFByteStream" || name=="MFCreateStreamOnMFByteStreamEx"){
    const bool typed=name.ends_with("Ex");const auto output=arg(typed?2:1);out(output);require(arg(0)!=0);
    if(typed){const auto iid=guid(arg(1));require(iid==mf::unknown || iid==mf::iid_ComStream || iid==mf::iid_SequentialStream,no_interface);}
    const auto p=make(mf::ComStream);object(p,mf::ComStream)->parent=arg(0);hold(arg(0));put(output,p,bits);
}else if(name=="PipelinePoll" && entry(arg(0)).object->kind==mf::ByteStream){
    const auto stream=object(arg(0),mf::ByteStream);stream->poll_queued=false;
    while(!stream->events.empty()){
        const auto p=stream->events.front();stream->events.pop_front();const auto completion=object(p,mf::Async);
        callbacks.push_back({foreign_method(completion->callback,4),{completion->callback,p}});
        callbacks.push_back({resolve("internal-mf","Release").value,{p}});
    }
    release_job(stream);
}
