if(name=="MFCreateSourceResolver"){out(arg(0));put(arg(0),make(mf::Resolver),bits);}
else if(name=="MFRegisterLocalSchemeHandler" || name=="MFRegisterLocalByteStreamHandler"){
    // MFRegisterLocalSchemeHandler(szScheme, pActivate)／MFRegisterLocalByteStreamHandler(szFileExtension, szMimeType, pActivate)：只在本程序有效，持有啟用物件；
    // 解析器先比對本機登錄（後登錄者優先），符合時以 IMFActivate::ActivateObject 取得 handler，未符合時沿用內建來源。
    const bool scheme=name=="MFRegisterLocalSchemeHandler";const auto activate=arg(scheme?1:2);require(activate!=0,pointer_error);(void)foreign_method(activate,33);
    const auto folded=[&](std::uint64_t p){auto text=p?read_utf16(memory,GuestAddress{p},1024):std::u16string{};for(auto& c:text)c=unicode_case_unit(c,false);return text;};
    MediaFoundationState::LocalHandler handler;handler.activate=activate;
    if(scheme){require(arg(0)!=0,pointer_error);handler.scheme=folded(arg(0));require(!handler.scheme.empty());if(handler.scheme.back()!=u':')handler.scheme.push_back(u':');}
    else {require(arg(0) || arg(1),pointer_error);handler.extension=folded(arg(0));if(!handler.extension.empty() && handler.extension.front()==u'.')handler.extension.erase(0,1);handler.mime=folded(arg(1));
        require(!handler.extension.empty() || !handler.mime.empty());}
    require(bridge.local_handlers.size()<4096,0x8007000eU);hold(activate);bridge.local_handlers.insert(bridge.local_handlers.begin(),std::move(handler));
}else if(name=="ResolverDeliver"){
    // (completion, object, objectType, status)：本機 handler 完成後把結果放進解析器的完成佇列；已取消時丟棄結果。
    auto completion=object(arg(0),mf::Async);
    if(completion->status==0xc00d36edU)release(arg(1));else {completion->result_object=arg(1);completion->status=d(3);completion->value.scalar=d(2);completion->flags|=8U;}
    auto resolver=object(completion->queue_owner,mf::Resolver);resolver->events.push_back(arg(0));hold_job(resolver);
}
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
        put(arg(2),(completion->flags&8U)?static_cast<std::uint32_t>(completion->value.scalar):entry(completion->result_object).object->kind==mf::Source?0U:1U);hold(completion->result_object);put(arg(3),completion->result_object,bits);
    }else if(name=="O_CancelObjectCreation"){
        const auto completion=object(arg(1),mf::Async);require(completion->queue_owner==arg(0) && !(completion->flags&3U),0xc00d36b2U);
        completion->status=0xc00d36edU;release(completion->result_object);completion->result_object=0;
    }else {
        // 本機 handler 的續行完成後會重新進入此處取回結果：先取回，避免重做下方的輸出初始化與物件建立。
        if(service_callback::completed(this,threads_.current,service.symbol,state))return nested_continuation(state,memory,service.symbol,[](GuestState&,GuestMemory&)->service_callback::Task<std::optional<std::uint32_t>>{throw HostCallFault("服務續行重播不應重新建立");});
        const bool bytes=name.ends_with("ByteStream"),async=name.starts_with("O_Begin");const unsigned shift=bytes?1U:0U;
        const auto flags=d(2+shift);require(!(flags&~0x30033U) && ((flags&3U)==1U || (!bytes && (flags&3U)==2U)));
        // Property store 為格式處理器提供提示；來源以實際內容辨識，不依副檔名選擇。
        if(async){if(arg(4+shift))out(arg(4+shift));require(arg(5+shift)!=0,pointer_error);}
        else {out(arg(5+shift));put(arg(4+shift),2);}
        // 本機登錄的 handler：URL 依 scheme（含冒號），位元組串流依 URL 提示的副檔名（本引擎的串流沒有內容型別屬性，MIME 登錄不會比對到）。
        const auto url_pointer=arg(1+shift);std::u16string url=url_pointer?read_utf16(memory,GuestAddress{url_pointer},4096):std::u16string{};auto lower=url;for(auto& c:lower)c=unicode_case_unit(c,false);
        std::uint64_t activate=0;
        for(const auto& handler:bridge.local_handlers){
            if(!bytes){if(!handler.scheme.empty() && lower.starts_with(handler.scheme)){activate=handler.activate;break;}continue;}
            if(handler.extension.empty())continue;const auto path=lower.substr(0,lower.find_first_of(u"?#"));const auto dot=path.find_last_of(u'.'),slash=path.find_last_of(u"/\\");
            if(dot!=std::u16string::npos && (slash==std::u16string::npos || dot>slash) && path.substr(dot+1)==handler.extension){activate=handler.activate;break;}
        }
        if(activate){
            const auto host=make(mf::HostCallback);auto callback=object(host,mf::HostCallback);callback->parent=arg(0);hold(arg(0));callback->identifier=(async?1U:0U)|(bytes?2U:0U);
            std::uint64_t completion=0,signal=0;const auto cookie=async?arg(4+shift):0,type_out=async?0:arg(4+shift),object_out=async?0:arg(5+shift);
            const auto stream=bytes?arg(1):0,properties=arg(3+shift);
            try{
                if(async){
                    completion=make(mf::Async);auto pending=object(completion,mf::Async);pending->queue_owner=arg(0);pending->parent=arg(0);hold(arg(0));pending->identifier=bytes?1U:0U;
                    pending->callback=arg(5+shift);hold(pending->callback);pending->state=arg(6+shift);hold(pending->state);callback->state=completion;hold(completion);
                }else {signal=nested_service(state,memory,"kernel32.dll","CreateEventW",{0,1,0,0});require(signal!=0,0x8007000eU);callback->generation=signal;}
                if(stream){hold(stream);callback->children.push_back(stream);}if(properties){hold(properties);callback->children.push_back(properties);}
            }catch(...){if(completion)release(completion);release(host);throw;}
            const auto iid=bytes?mf::iid_ByteStreamHandler:mf::iid_SchemeHandler;
            return nested_continuation(state,memory,service.symbol,[this,service,host,completion,signal,activate,iid,url,bytes,async,flags,stream,properties,cookie,type_out,object_out](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);const std::uint8_t width=x86_?32:64;const unsigned size_of_pointer=width/8U;
                const auto method=[&](std::uint64_t object,unsigned slot){const auto table=m.load(GuestAddress{object},width);return m.load(GuestAddress{table+slot*size_of_pointer},width);};
                const auto internal=[&](std::string_view symbol){return resolve("internal-mf",symbol).value;};
                const auto bytes_url=utf16_bytes(url);const auto block=allocate_heap(m,48+bytes_url.size());std::uint32_t hr=0x8007000eU;std::uint64_t handler=0;
                if(block){
                    m.write(GuestAddress{block},iid);m.store(GuestAddress{block+16},width,0);m.store(GuestAddress{block+24},32,2);m.store(GuestAddress{block+32},width,0);m.write(GuestAddress{block+48},bytes_url);
                    const auto url_arg=url.empty()?0:block+48;
                    hr=static_cast<std::uint32_t>(co_await service_callback::call_value(method(activate,33),{activate,block,block+16}));
                    if(!hr){handler=m.load(GuestAddress{block+16},width);
                        hr=static_cast<std::uint32_t>(bytes?co_await service_callback::call_value(method(handler,3),{handler,stream,url_arg,flags,properties,0,host,0}):
                            co_await service_callback::call_value(method(handler,3),{handler,url_arg,flags,properties,0,host,0}));
                        if(hr){(void)co_await service_callback::call_value(method(handler,2),{handler});handler=0;}
                        else if(const auto found=media_foundation_->objects.find(host);found!=media_foundation_->objects.end())found->second.object->result_object=handler;}
                    if(!hr && !async){
                        // 同步解析：等待 handler 呼叫 Invoke，再以 EndCreateObject 取回物件。
                        (void)co_await service_callback::call_value(resolve("kernel32.dll","WaitForSingleObject").value,{signal,0xffffffffU});
                        std::uint64_t result=0;if(const auto found=media_foundation_->objects.find(host);found!=media_foundation_->objects.end()){result=found->second.object->callback;found->second.object->callback=0;}
                        hr=result?static_cast<std::uint32_t>(co_await service_callback::call_value(method(handler,4),{handler,result,block+24,block+32})):0x8000ffffU;
                        if(result)(void)co_await service_callback::call_value(method(result,2),{result});
                        if(!hr){m.store(GuestAddress{type_out},32,m.load(GuestAddress{block+24},32));m.store(GuestAddress{object_out},width,m.load(GuestAddress{block+32},width));}
                    }
                    (void)free_heap(m,block,s);
                }
                if(signal)(void)co_await service_callback::call_value(resolve("kernel32.dll","CloseHandle").value,{signal});
                if(async){
                    if(hr)(void)co_await service_callback::call_value(internal("Release"),{completion});
                    else if(cookie){(void)co_await service_callback::call_value(internal("AddRef"),{completion});m.store(GuestAddress{cookie},width,completion);}
                }
                (void)co_await service_callback::call_value(internal("Release"),{host});
                call.return_to(s,hr);co_return std::nullopt;
            });
        }
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
