// WinHTTP 的選項直接更新共用傳輸狀態；子 handle 在建立時取得快照。
const bool setting=symbol=="WinHttpSetOption";const auto option=d(1);
const bool timeout=option==2 || option==3 || option==5 || option==6;
const unsigned timeout_index=option==2?0:option==3?1:option==5?2:3;
const bool string=option==41 || option==0x1000 || option==0x1001;
const bool pointer=option==1 || option==21 || option==45;
const bool readonly=option==1 || option==9 || option==21;
if(!timeout && !string && !pointer && option!=9 && option!=38 && option!=63 && option!=84 && option!=88 && option!=89 && option!=118)throw HttpError{12009};
if(object.inet)throw HttpError{12018};
if(!readonly && option!=45 && object.kind!=HttpHandle::Kind::Session && object.kind!=HttpHandle::Kind::Request)throw HttpError{12018};
if(option==63 || option==0x1000 || option==0x1001)require(HttpHandle::Kind::Request);
if(setting){
    if(readonly)throw HttpError{12011};
    if(object.pending && option!=45)throw HttpError{12019};
    if(option==63 && object.sent)throw HttpError{12019};
    if(!arg(2))throw HttpError{87};
    if(string){
        if(d(3)&1U)throw HttpError{122};
        auto value=text(arg(2),d(3)/2);if(!value.empty() && !value.back())value.pop_back();
        if(value.find(u'\0')!=value.npos || value.find_first_of(u"\r\n")!=value.npos)throw HttpError{87};
        if(option==41){
            auto agent=latin(value);
            if(object.kind==HttpHandle::Kind::Request){auto headers=object.input.headers;
                std::erase_if(headers,[](const auto& h){return lower(h.first)=="user-agent";});
                if(!agent.empty())headers.emplace_back("User-Agent",agent);object.input.headers=std::move(headers);}
            object.agent=std::move(agent);
        }else (option==0x1000?object.input.username:object.input.password)=utf16_to_utf8(value);
    }else if(option==38){
        if(d(3)!=unit*3)throw HttpError{122};memory.check(GuestAddress{arg(2)},unit*3,Access::Read);
        const auto access=memory.load(GuestAddress{arg(2)},32);
        http_proxy::Configuration config;
        if(access==0){
            if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw HttpError{transaction.error};
            const auto path=http_proxy::key(x86_);if(runtime_.registry.keys.contains(path))(void)registry_security_access(path,1);
            config=http_proxy::read(runtime_.registry,x86_);
        }else if(access==3){
            const auto proxy=memory.load(GuestAddress{arg(2)+unit},pointer_bits),bypass=memory.load(GuestAddress{arg(2)+unit*2},pointer_bits);
            config.proxy=latin(text(proxy));config.bypass=latin(text(bypass));
            if(config.proxy.empty() || config.proxy.find_first_of("\r\n")!=std::string::npos || config.bypass.find_first_of("\r\n")!=std::string::npos)throw HttpError{87};
        }else if(access!=1)throw HttpError{87};
        object.input.proxy=std::move(config.proxy);object.input.bypass=std::move(config.bypass);
    }else{
        const auto width=pointer?unit:4U;if(d(3)!=width)throw HttpError{122};
        const auto value=memory.load(GuestAddress{arg(2)},static_cast<std::uint8_t>(width*8));
        if(timeout){if(value>INT32_MAX && value!=UINT32_MAX)throw HttpError{87};object.input.timeouts[timeout_index]=static_cast<std::int32_t>(value);}
        else if(option==45)object.context=value;
        else if(option==88){if(value>2)throw HttpError{87};object.input.redirect_policy=static_cast<std::uint32_t>(value);}
        else if(option==89)object.input.max_redirects=static_cast<std::uint32_t>(value);
        else if(option==63){if(value&~15U)throw HttpError{87};object.input.disabled_features|=static_cast<std::uint32_t>(value);}
        else if(option==118){if(value&~3U)throw HttpError{87};object.input.decompression=static_cast<std::uint32_t>(value);}
        else if(option==84){
            // curl 以最小／最大版本表達協定集合；不擴大不連續集合或接受無法提供的 SSL2/3。
            if(!value || (value&~0x2a80U))throw HttpError{87};
            constexpr unsigned protocols[]{0x80,0x200,0x800,0x2000};bool begun=false,gap=false;
            for(auto flag:protocols){if(value&flag){if(gap)throw HttpError{50};begun=true;}else if(begun)gap=true;}
            object.input.secure_protocols=static_cast<std::uint32_t>(value);
        }
    }
}else{
    if(!arg(3))throw HttpError{87};const auto capacity=memory.load(GuestAddress{arg(3)},32);memory.check(GuestAddress{arg(3)},4,Access::Write);
    std::vector<std::uint8_t> bytes;
    if(string){const auto& value=option==41?object.agent:option==0x1000?object.input.username:object.input.password;
        bytes=utf16_bytes(option==41?wide_latin(value):utf8_to_utf16(value));}
    else if(option==38)bytes.resize(unit*3);
    else{
        const auto value=timeout?static_cast<std::uint32_t>(object.input.timeouts[timeout_index]):option==45?object.context:option==21?object.parent:option==1?object.callback:
            option==9?(object.kind==HttpHandle::Kind::Session?1U:object.kind==HttpHandle::Kind::Connection?2U:3U):
            option==63?object.input.disabled_features:option==84?object.input.secure_protocols:option==88?object.input.redirect_policy:option==89?object.input.max_redirects:object.input.decompression;
        bytes.resize(pointer?unit:4U);for(unsigned i=0;i<bytes.size();++i)bytes[i]=static_cast<std::uint8_t>(value>>(i*8));
    }
    if(!arg(2) || capacity<bytes.size()){memory.store(GuestAddress{arg(3)},32,bytes.size());throw HttpError{122};}
    memory.check(GuestAddress{arg(2)},bytes.size(),Access::Write);
    if(option==38){
        // WINHTTP_PROXY_INFO 的字串由呼叫者以 GlobalFree 釋放，不能回傳宿主指標。
        std::uint64_t proxy=0,bypass=0;
        const auto copy=[&](const std::string& value){if(value.empty())return UINT64_C(0);const auto data=utf16_bytes(wide_latin(value));
            const auto address=allocate_heap(memory,data.size());if(!address)throw HttpError{8};
            try{memory.write(GuestAddress{address},data);gui_.global_memory.emplace(address,GuiGlobalMemory{address,data.size(),0,0,false});}
            catch(...){(void)free_heap(memory,address,state);throw;}return address;};
        try{proxy=copy(object.input.proxy);bypass=copy(object.input.bypass);}
        catch(...){for(auto address:{proxy,bypass})if(address){gui_.global_memory.erase(address);(void)free_heap(memory,address,state);}throw;}
        bytes[0]=proxy?3:1;for(unsigned i=0;i<unit;++i){bytes[unit+i]=static_cast<std::uint8_t>(proxy>>(i*8));bytes[unit*2+i]=static_cast<std::uint8_t>(bypass>>(i*8));}
    }
    memory.write(GuestAddress{arg(2)},bytes);memory.store(GuestAddress{arg(3)},32,bytes.size()-(string?2U:0U));
}
result=1;
