// 將 ACM 驅動程式的訊息轉成相容層共用的格式目錄與串流操作。
if(name=="DriverProc"){
    const auto session=arg(0),endpoint=arg(1),first=arg(3),second=arg(4);
    const auto message=d(2);
    const auto module=std::find(acm::builtin_codec_modules.begin(),acm::builtin_codec_modules.end(),service.family);
    if(module==acm::builtin_codec_modules.end())throw HostCallFault("未登錄的內建 ACM 驅動程式");
    const auto index=static_cast<unsigned>(module-acm::builtin_codec_modules.begin());
    const unsigned unit=pointer_bits/8;
    const auto finish=[&](std::uint64_t value){
        arg.return_to(state,value);
        return std::optional<std::uint32_t>{};
    };
    const auto read32=[&](std::uint64_t p,unsigned offset){
        return static_cast<unsigned>(memory.load(GuestAddress{p+offset},32));
    };
    const auto read_pointer=[&](std::uint64_t p,unsigned offset){
        return memory.load(GuestAddress{p+offset},pointer_bits);
    };
    if(message==1 || message==2 || message==5 || message==6)return finish(1);
    if(message==7 || message==8)return finish(0);
    if(message==9 || message==10)return finish(2);
    if(message==acm::acm_driver_base+1)return finish(0); // 目錄內容不依賴其他驅動程式。
    if(!acm_)acm_=std::make_shared<AcmState>();
    if(message==3){
        if(second){
            try{
                if(read32(second,0)<(x86_?36U:48U))return finish(0);
                const bool supported=read32(second,4)==0x63647561 && !read32(second,8);
                memory.store(GuestAddress{second+20},32,supported?0U:512U);
                if(!supported)return finish(0);
            }catch(const MemoryFault&){return finish(0);}
        }
        if(acm_->codec_sessions.size()>=256)return finish(0);
        const auto handle=runtime_.allocate_wait_handle();
        acm_->codec_sessions.emplace(handle,AcmState::CodecSession{endpoint,index});
        return finish(handle);
    }
    const auto opened=acm_->codec_sessions.find(session);
    const bool owns_session=opened!=acm_->codec_sessions.end() &&
        opened->second.endpoint==endpoint && opened->second.codec==index;
    if(message==4){
        if(!owns_session || std::any_of(acm_->streams.begin(),acm_->streams.end(),[&](const auto& stream){
            return stream.second->codec_instance && stream.second->driver==session;
        }))return finish(0);
        acm_->codec_sessions.erase(opened);
        return finish(1);
    }
    if(message<acm::acm_driver_base)return finish(0);
    if(session && !owns_session)return finish(5);
    codec_driver=index;
    using Parameters=std::array<std::uint64_t,5>;
    switch(message){
    case acm::driver_details:
        name="acmDriverDetailsW";
        arg.message=Parameters{0,first,second};
        break;
    case acm::format_tag_details:
    case acm::format_details:
    case acm::filter_tag_details:
    case acm::filter_details:
        if(index!=acm::pcm_codec && message>=acm::filter_tag_details)return finish(8);
        if(message==acm::format_tag_details)name="acmFormatTagDetailsW";
        if(message==acm::format_details)name="acmFormatDetailsW";
        if(message==acm::filter_tag_details)name="acmFilterTagDetailsW";
        if(message==acm::filter_details)name="acmFilterDetailsW";
        arg.message=Parameters{0,first,second};
        break;
    case acm::format_suggest:{
        const auto destination=unit==4?16U:24U;
        if(!first || read32(first,0)<(unit==4?24U:40U))throw AcmError{11};
        const auto source=read_pointer(first,8);
        const auto capacity=read32(first,8+unit);
        if(!source || capacity<16)throw AcmError{11};
        memory.check(GuestAddress{source},capacity,Access::Read);
        source_format_capacity=capacity;
        name="acmFormatSuggest";
        arg.message=Parameters{0,source,read_pointer(first,destination),read32(first,destination+unit),read32(first,4)};
        break;
    }
    case acm::stream_open:
    case acm::stream_close:
    case acm::stream_size:
    case acm::stream_convert:
    case acm::stream_reset:
    case acm::stream_prepare:
    case acm::stream_unprepare:{
        if(!owns_session)return finish(5);
        const acm::StreamInstanceAbi abi{unit};
        if(!first || read32(first,0)<abi.size())throw AcmError{11};
        const auto slot=first+abi.private_data();
        const auto handle=read_pointer(first,abi.private_data());
        if(message==acm::stream_open){
            if(handle)return finish(513);
            const auto flags=read32(first,abi.flags());
            if(flags&~7U)throw AcmError{10};
            if(flags&2U)throw AcmError{512};
            const auto filter=read_pointer(first,abi.filter());
            const auto gain=filter?std::optional{read_volume(memory,filter)}:std::nullopt;
            auto stream=std::make_shared<Stream>(read_format(memory,read_pointer(first,abi.source())),
                read_format(memory,read_pointer(first,abi.destination())),gain);
            traced=stream;
            if(builtin_codec_for(stream->source.tag,stream->destination.tag)!=index)throw AcmError{512};
            if(flags&1U)return finish(0);
            if(acm_->registered_streams.size()+acm_->streams.size()>=256)throw AcmError{4};
            memory.check(GuestAddress{slot},unit,Access::Write);
            const auto stream_handle=runtime_.allocate_wait_handle();
            stream->driver=session;
            stream->codec_instance=first;
            acm_->streams.emplace(stream_handle,std::move(stream));
            memory.store(GuestAddress{slot},pointer_bits,stream_handle);
            return finish(0);
        }
        const auto found=acm_->streams.find(handle);
        if(found==acm_->streams.end() || found->second->driver!=session ||
           found->second->codec_instance!=first)return finish(5);
        if(message==acm::stream_close){
            if(!found->second->headers.empty())return finish(513);
            memory.store(GuestAddress{slot},pointer_bits,0);
            acm_->streams.erase(found);
            return finish(0);
        }
        if(message==acm::stream_size){
            if(!second || read32(second,0)<16)throw AcmError{11};
            const auto flags=read32(second,4);
            if(flags>1)throw AcmError{10};
            name="acmStreamSize";
            arg.message=Parameters{handle,read32(second,flags?12U:8U),second+(flags?8U:12U),flags};
        }else if(message==acm::stream_reset){
            name="acmStreamReset";
            arg.message=Parameters{handle,0};
        }else{
            if(!second || read32(second,0)<codec_header.size())throw AcmError{11};
            name=message==acm::stream_prepare?"acmStreamPrepareHeader":
                message==acm::stream_unprepare?"acmStreamUnprepareHeader":"acmStreamConvert";
            arg.message=Parameters{handle,second,message==acm::stream_convert?
                read32(second,codec_header.convert_flags()):0U};
        }
        break;
    }
    default:
        return finish(8);
    }
}
