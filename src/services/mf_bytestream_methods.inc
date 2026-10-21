else if(name.starts_with("J_")){
    require(item->kind==mf::ByteStream && bool(item->byte_stream),no_interface);auto& stream=*item->byte_stream;
    if(name=="J_EndRead" || name=="J_EndWrite"){
        put(arg(2),0);const auto completion=object(arg(1),mf::Async);
        require(completion->queue_owner==arg(0) && completion->identifier==(name=="J_EndRead"?1U:2U) && !completion->closed,0xc00d36b2U);
        completion->closed=true;put(arg(2),completion->length);result=completion->status;
    }else{
        require(!stream.closed,shutdown);
        const auto status=[&](std::uint32_t code){require(!code,sink_io_error(code));};
        const auto length=[&]{const auto info=stream.file->information();status(info.error);return info.metadata.size;};
        if(name=="J_Close"){stream.closed=true;stream.file.reset();}
        else if(name=="J_GetCapabilities")put(arg(1),stream.capabilities);
        else if(name=="J_GetLength")put(arg(1),length(),64);
        else if(name=="J_GetCurrentPosition")put(arg(1),stream.position,64);
        else if(name=="J_SetLength"){require(stream.capabilities&2,0x80070005U);require(arg(1)<=INT64_MAX);status(stream.file->truncate(arg(1)));}
        else if(name=="J_SetCurrentPosition"){require(arg(1)<=INT64_MAX);stream.position=arg(1);}
        else if(name=="J_IsEndOfStream")put(arg(1),stream.position>=length());
        else if(name=="J_Seek"){
            require(d(1)<=1 && !(d(3)&~1U));if(arg(4))check(arg(4),8);
            const auto offset=std::bit_cast<std::int64_t>(arg(2));const auto base=d(1)?stream.position:0;
            require(offset>=0?std::uint64_t(offset)<=INT64_MAX-base:(std::uint64_t(-(offset+1))+1)<=base);
            stream.position=offset>=0?base+std::uint64_t(offset):base-(std::uint64_t(-(offset+1))+1);if(arg(4))put(arg(4),stream.position,64);
        }else if(name=="J_Flush")status(stream.file->flush());
        else if(name=="J_Read" || name=="J_Write" || name=="J_BeginRead" || name=="J_BeginWrite"){
            const bool writing=name=="J_Write" || name=="J_BeginWrite",async=name=="J_BeginRead" || name=="J_BeginWrite";
            require(stream.capabilities&(writing?2U:1U),0x80070005U);require(d(2)<=64U*1024U*1024U && d(2)<=INT64_MAX-stream.position);
            if(async){require(arg(3)!=0,pointer_error);require(item->events.size()<65536,0x8007000eU);}else put(arg(3),0);
            if(d(2)){require(arg(1)!=0,pointer_error);memory.check(GuestAddress{arg(1)},d(2),writing?Access::Read:Access::Write);}
            std::vector<std::uint8_t> bytes(d(2));if(writing && !bytes.empty())memory.read(GuestAddress{arg(1)},bytes);
            const auto io=writing?stream.file->write(bytes,stream.position):stream.file->read(bytes,stream.position);
            require(io.bytes<=bytes.size(),0x80004005U);if(!writing && io.bytes)memory.write(GuestAddress{arg(1)},std::span(bytes).first(static_cast<std::size_t>(io.bytes)));
            stream.position+=io.bytes;
            if(async){
                const auto p=make(mf::Async);auto completion=object(p,mf::Async);completion->queue_owner=arg(0);completion->parent=arg(0);hold(arg(0));completion->identifier=writing?2:1;
                completion->length=static_cast<std::uint32_t>(io.bytes);completion->status=sink_io_error(io.error);completion->callback=arg(3);completion->state=arg(4);hold(arg(3));hold(arg(4));item->events.push_back(p);hold_job(item);
            }else{put(arg(3),io.bytes);status(io.error);}
        }else throw Failure{no_interface};
    }
}
