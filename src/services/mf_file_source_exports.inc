if(name=="MFCreateSourceResolver"){out(arg(0));put(arg(0),make(mf::Resolver),bits);}
else if(name=="MFCreateSourceReaderFromURL" || name=="MFCreateSourceReaderFromByteStream"){
    out(arg(2));const auto source=name=="MFCreateSourceReaderFromURL"?file_source_create(open_media_stream(arg(0),0x10000)):source_from_byte_stream(arg(0));try{put(arg(2),reader_create(source,arg(1)),bits);}catch(...){release(source);throw;}release(source);
}else if(name=="PipelinePoll" && entry(arg(0)).object->kind==mf::Resolver){
    const auto resolver=object(arg(0),mf::Resolver);resolver->poll_queued=false;
    while(!resolver->events.empty()){
        const auto p=resolver->events.front();resolver->events.pop_front();const auto completion=object(p,mf::Async);completion->flags|=1;
        callbacks.push_back({foreign_method(completion->callback,4),{completion->callback,p}});
        callbacks.push_back({resolve("internal-mf","Release").value,{p}});
    }release_job(resolver);
}else if(name.starts_with("O_")){
    const auto resolver=object(arg(0),mf::Resolver);
    if(name=="O_EndCreateObjectFromURL" || name=="O_EndCreateObjectFromByteStream"){
        out(arg(3));put(arg(2),2);const auto completion=object(arg(1),mf::Async);
        require(completion->queue_owner==arg(0) && completion->identifier==(name=="O_EndCreateObjectFromByteStream"?1U:0U) && !(completion->flags&2U),0xc00d36b2U);
        completion->flags|=2U;require(!completion->status,completion->status);require(completion->result_object!=0,0xc00d36b2U);
        put(arg(2),entry(completion->result_object).object->kind==mf::Source?0U:1U);hold(completion->result_object);put(arg(3),completion->result_object,bits);
    }else if(name=="O_CancelObjectCreation"){
        const auto completion=object(arg(1),mf::Async);require(completion->queue_owner==arg(0) && !(completion->flags&3U),0xc00d36b2U);
        completion->status=0xc00d36edU;release(completion->result_object);completion->result_object=0;
    }else {
        const bool bytes=name.ends_with("ByteStream"),async=name.starts_with("O_Begin");const unsigned shift=bytes?1U:0U;
        const auto flags=d(2+shift);require(!(flags&~0x30033U) && ((flags&3U)==1U || (!bytes && (flags&3U)==2U)));
        // Property store 為格式處理器提供提示；來源以實際內容辨識，不依副檔名選擇。
        if(async){if(arg(4+shift))out(arg(4+shift));require(arg(5+shift)!=0,pointer_error);}
        else {out(arg(5+shift));put(arg(4+shift),2);}
        std::uint64_t created=0;std::uint32_t status=0;
        try{
            if(bytes)created=source_from_byte_stream(arg(1));
            else {const auto stream=open_media_stream(arg(1),flags);if(flags&1U)created=file_source_create(stream);else {created=make(mf::ByteStream);object(created,mf::ByteStream)->byte_stream=stream;}}
        }catch(const Failure& failure){if(!async)throw;status=failure.code;}
        if(async){
            const auto p=make(mf::Async);auto completion=object(p,mf::Async);completion->queue_owner=arg(0);completion->parent=arg(0);hold(arg(0));
            completion->result_object=created;completion->status=status;completion->identifier=bytes?1U:0U;completion->callback=arg(5+shift);hold(completion->callback);completion->state=arg(6+shift);hold(completion->state);
            if(arg(4+shift)){hold(p);put(arg(4+shift),p,bits);}resolver->events.push_back(p);hold_job(resolver);
        }else {put(arg(4+shift),flags&1U?0U:1U);put(arg(5+shift),created,bits);}
    }
}
