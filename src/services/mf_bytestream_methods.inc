else if(name.starts_with("J_")){
    require(item->kind==mf::ByteStream && bool(item->byte_stream),no_interface);auto& stream=*item->byte_stream;
    if(name=="J_EndRead" || name=="J_EndWrite"){
        put(arg(2),0);const auto completion=object(arg(1),mf::Async);
        require(completion->queue_owner==arg(0) && completion->identifier==(name=="J_EndRead"?1U:2U) && !completion->closed,0xc00d36b2U);
        completion->closed=true;put(arg(2),completion->length);result=completion->status;
    }else{
        require(!stream.closed,shutdown);
        const auto status=[&](std::uint32_t code){require(!code,sink_io_error(code));};
        // 後端為客體 IStream 時每個方法只啟動一次客體續行；IStream 失敗的 HRESULT 原樣交回。
        const auto guest=[&](StreamOp op,std::uint64_t position=0){const auto r=guest_stream_io(stream.guest_stream,op,position);require(!(r.status&0x80000000U),r.status);return r;};
        const auto length=[&]{if(stream.guest_stream)return guest(StreamOp::Stat).value;const auto info=stream.file->information();status(info.error);return info.metadata.size;};
        // IStream 包裝的 Close 不結束串流（與 Windows 相同，之後仍可讀寫）；IStream 在物件釋放時歸還。
        if(name=="J_Close"){if(!stream.guest_stream){stream.closed=true;stream.file.reset();}}
        else if(name=="J_GetCapabilities"){
            check(arg(1),4);
            // 可讀、可搜尋固定成立；可寫依 Stat 的 grfMode（STGM_WRITE／STGM_READWRITE）。
            if(stream.guest_stream)stream.capabilities=5U|(guest(StreamOp::Stat).mode&3U?2U:0U);
            put(arg(1),stream.capabilities);
        }
        else if(name=="J_GetLength"){check(arg(1),8);put(arg(1),length(),64);}
        else if(name=="J_GetCurrentPosition")put(arg(1),stream.position,64);
        else if(name=="J_SetLength"){require(arg(1)<=INT64_MAX);if(stream.guest_stream)guest(StreamOp::SetSize,arg(1));else {require(stream.capabilities&2,0x80070005U);status(stream.file->truncate(arg(1)));}}
        else if(name=="J_SetCurrentPosition"){require(arg(1)<=INT64_MAX);stream.position=arg(1);}
        else if(name=="J_IsEndOfStream"){check(arg(1),4);put(arg(1),stream.position>=length());}
        else if(name=="J_Seek"){
            require(d(1)<=1 && !(d(3)&~1U));if(arg(4))check(arg(4),8);
            const auto offset=std::bit_cast<std::int64_t>(arg(2));const auto base=d(1)?stream.position:0;
            require(offset>=0?std::uint64_t(offset)<=INT64_MAX-base:(std::uint64_t(-(offset+1))+1)<=base);
            stream.position=offset>=0?base+std::uint64_t(offset):base-(std::uint64_t(-(offset+1))+1);if(arg(4))put(arg(4),stream.position,64);
        }else if(name=="J_Flush"){if(stream.guest_stream)guest(StreamOp::Commit);else status(stream.file->flush());}
        else if(name=="J_Read" || name=="J_Write" || name=="J_BeginRead" || name=="J_BeginWrite"){
            const bool writing=name=="J_Write" || name=="J_BeginWrite",async=name=="J_BeginRead" || name=="J_BeginWrite";
            // 客體 IStream 是否可讀寫由它自己的 Read／Write 決定。
            if(!stream.guest_stream)require(stream.capabilities&(writing?2U:1U),0x80070005U);
            require(d(2)<=64U*1024U*1024U && d(2)<=INT64_MAX-stream.position);
            if(async){require(arg(3)!=0,pointer_error);require(item->events.size()<65536,0x8007000eU);}else put(arg(3),0);
            if(d(2)){require(arg(1)!=0,pointer_error);memory.check(GuestAddress{arg(1)},d(2),writing?Access::Read:Access::Write);}
            std::uint64_t moved=0;std::uint32_t failure=0;
            if(stream.guest_stream){
                // 以上檢查不改變狀態；續行完成後回到這裡只取一次結果，位置在取回結果後才前進。
                if(d(2)){const auto r=guest_stream_io(stream.guest_stream,writing?StreamOp::Write:StreamOp::Read,stream.position,arg(1),d(2));if(r.status&0x80000000U)failure=r.status;else moved=r.value;}
            }else{
                std::vector<std::uint8_t> bytes(d(2));if(writing && !bytes.empty())memory.read(GuestAddress{arg(1)},bytes);
                const auto io=writing?stream.file->write(bytes,stream.position):stream.file->read(bytes,stream.position);
                require(io.bytes<=bytes.size(),0x80004005U);if(!writing && io.bytes)memory.write(GuestAddress{arg(1)},std::span(bytes).first(static_cast<std::size_t>(io.bytes)));
                moved=io.bytes;failure=sink_io_error(io.error);
            }
            stream.position+=moved;
            if(async){
                const auto p=make(mf::Async);auto completion=object(p,mf::Async);completion->queue_owner=arg(0);completion->parent=arg(0);hold(arg(0));completion->identifier=writing?2:1;
                completion->length=static_cast<std::uint32_t>(moved);completion->status=failure;completion->callback=arg(3);completion->state=arg(4);hold(arg(3));hold(arg(4));item->events.push_back(p);hold_job(item);
            }else{put(arg(3),moved);require(!failure,failure);}
        }else throw Failure{no_interface};
    }
}
else if(name.starts_with("KS_")){
    // IStream 包裝（MFCreateStreamOnMFByteStream）：parent 是被包裝的 IMFByteStream。
    require(item->kind==mf::ComStream && item->parent!=0,no_interface);const auto inner=item->parent;
    constexpr std::uint32_t stream_function=0x80030001U,stream_pointer=0x80030009U;
    if(name=="KS_Read" || name=="KS_Write"){
        const bool writing=name=="KS_Write";if(arg(3))put(arg(3),0);
        if(d(2)){
            require(arg(1)!=0,stream_pointer);memory.check(GuestAddress{arg(1)},d(2),writing?Access::Read:Access::Write);
            const auto r=wrapped_stream_io(inner,writing?WrapOp::Write:WrapOp::Read,arg(1),d(2));
            if(arg(3))put(arg(3),r.value);if(!writing && r.value<d(2))result=1; // 讀到結尾：S_FALSE。
        }
    }else if(name=="KS_Seek"){
        require(d(2)<=2,stream_function);if(arg(3))check(arg(3),8);
        const auto r=wrapped_stream_io(inner,WrapOp::Seek,arg(1),d(2));if(arg(3))put(arg(3),r.value,64);
    }else if(name=="KS_SetSize")wrapped_stream_io(inner,WrapOp::SetSize,arg(1));
    else if(name=="KS_Commit")wrapped_stream_io(inner,WrapOp::Commit);
    else if(name=="KS_Stat"){
        require(arg(1)!=0,stream_pointer);require(d(2)<=2);const std::size_t size=x86_?72:80;check(arg(1),size);
        const auto r=wrapped_stream_io(inner,WrapOp::Stat);
        // 包裝沒有名稱；STGTY_STREAM、長度取自 GetLength、模式取自 capabilities。
        memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(size));put(arg(1)+unit,2);put(arg(1)+(x86_?8U:16U),r.value,64);
        put(arg(1)+(x86_?40U:48U),r.capabilities&2U?(r.capabilities&1U?2U:1U):0U);
    }else if(name=="KS_CopyTo"){
        for(unsigned i=3;i<5;++i)if(arg(i))put(arg(i),0,64);require(arg(1)!=0,stream_pointer);
        if(arg(2)){const auto r=wrapped_stream_io(inner,WrapOp::CopyTo,arg(1),arg(2));if(arg(3))put(arg(3),r.value,64);if(arg(4))put(arg(4),r.second,64);}
    }else if(name=="KS_Clone"){out(arg(1));throw Failure{0x80004001U};}
    else if(name=="KS_Revert")throw Failure{0x80004001U};
    else throw Failure{stream_function}; // LockRegion／UnlockRegion：ByteStream 沒有區域鎖定。
}
