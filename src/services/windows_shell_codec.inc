if(name=="SHAnsiToAnsi" || name=="SHUnicodeToUnicode"){
    const unsigned width=name=="SHAnsiToAnsi"?1U:2U;
    const auto src=arg(0),dst=arg(1);
    const auto capacity=static_cast<std::int32_t>(u32(2));
    if(!src || !dst || capacity<=0)return done(0);
    const auto bits=static_cast<std::uint8_t>(8*width);
    for(unsigned i=0;;++i){
        const auto offset=static_cast<std::uint64_t>(i)*width;
        const auto c=memory.load(GuestAddress{src+offset},bits);
        if(!c || i+1==static_cast<unsigned>(capacity)){
            memory.store(GuestAddress{dst+offset},bits,0);
            return done(!c || src==dst?i+1:0);
        }
        memory.store(GuestAddress{dst+offset},bits,c);
    }
}
if(name=="SHAnsiToUnicode" || name=="SHUnicodeToAnsi" || name=="#216" || name=="#218" || name=="#152" || name=="#153"){
    const bool explicit_page=name.front()=='#';
    const bool to_wide=name=="SHAnsiToUnicode" || name=="#216" || name=="#153";
    const auto shift=explicit_page?1U:0U;
    const auto page=explicit_page?u32(0):0U;
    const auto src=arg(shift),dst=arg(shift+1);
    const bool core=service.family=="internal-shcore-codec";
    const auto convert=[&](std::uint64_t output,std::uint64_t size){
        if(to_wide)return nested_service(state,memory,"kernel32.dll","MultiByteToWideChar",{page,0,src,UINT32_MAX,output,size})&UINT32_MAX;
        return nested_service(state,memory,"kernel32.dll","WideCharToMultiByte",{page,0,src,UINT32_MAX,output,size,0,0})&UINT32_MAX;
    };
    const bool internet=!to_wide && (page==65001 || page==65000 || page==1200 || page==50000);
    if(name=="#152" || name=="#153"){
        if(!dst)return done(0x80004003U);
        memory.store(GuestAddress{dst},static_cast<std::uint8_t>(pointer_bits),0);
        if(!src)return done(0x80070057U);
        std::vector<std::uint8_t> payload;
        if(internet){
            const auto value=read_utf16(memory,GuestAddress{src});
            auto encoded=windows_crt_encode(value,page==1200?65001U:page);
            if(!encoded)return done(0x80070057U);
            payload=std::move(*encoded);
            payload.push_back(0);
        }else if(to_wide && page==65000){
            const auto decoded=windows_nls_decode(read_ansi(memory,src),page,false);
            if(!decoded)return done(0x80070459U);
            payload=utf16_bytes(*decoded);
        }
        const auto units=payload.empty()?convert(0,0):0;
        if(!units && payload.empty())return done(0x80070000U | (last_error_&0xffffU));
        const auto size=payload.empty()?units*(to_wide?2U:1U):payload.size();
        auto block=nested_service(state,memory,"ole32.dll","CoTaskMemAlloc",{size});
        if(x86_)block&=UINT32_MAX;
        if(!block)return done(0x8007000eU);
        if(!payload.empty())memory.write(GuestAddress{block},payload);
        else if(!convert(block,units)){
            const auto error=last_error_;
            (void)nested_service(state,memory,"ole32.dll","CoTaskMemFree",{block});
            return done(0x80070000U | (error&0xffffU));
        }
        memory.store(GuestAddress{dst},static_cast<std::uint8_t>(pointer_bits),block);
        return done(0);
    }
    const auto capacity=static_cast<std::int32_t>(u32(shift+2));
    if(to_wide && !core){
        if(page!=65000)return done(convert(dst,u32(shift+2)));
        if(!src || capacity<0 || (!dst && capacity) || src==dst){last_error_=87;return done(0);}
        const auto value=windows_nls_decode(read_ansi(memory,src),page,false);
        if(!value){last_error_=1113;return done(0);}
        const auto needed=value->size()+1;
        if(capacity && static_cast<unsigned>(capacity)<needed){last_error_=122;return done(0);}
        if(capacity)memory.write(GuestAddress{dst},utf16_bytes(*value));
        return done(needed);
    }
    if(!dst || capacity<=0)return done(0);
    const auto length=static_cast<unsigned>(capacity);
    const auto bits=static_cast<std::uint8_t>(to_wide?16:8);
    if(!src){memory.store(GuestAddress{dst},bits,0);return done(1);}
    if(core){
        const auto result=convert(dst,length);
        if(result)return done(result);
        memory.store(GuestAddress{dst+(length-1)*(bits/8U)},bits,0);
        return done(length);
    }
    memory.store(GuestAddress{dst},8,0);
    if(internet){
        const auto value=read_utf16(memory,GuestAddress{src});
        if(value.empty())return done(1);
        const auto out=windows_crt_encode(value,page==1200?65001U:page);
        if(!out)return done(0);
        const auto used=std::min<std::size_t>(length-1,out->size());
        if(used)memory.write(GuestAddress{dst},std::span(*out).first(used));
        if(used!=out->size())return done(0);
        memory.store(GuestAddress{dst+used},8,0);
        return done(used+1);
    }
    const auto result=convert(dst,length);
    if(result || last_error_!=122)return done(result);
    const auto needed=convert(0,0);
    if(!needed)return done(0);
    GuestScratch scratch(*this,memory,state);
    const auto buffer=scratch.allocate(needed);
    if(!buffer || !convert(buffer,needed))return done(0);
    auto used=length-1;
    if(!page || page==1 || page==3){
        while(used && (memory.load(GuestAddress{buffer+used},8)&0xc0)==0x80)--used;
    }
    std::vector<std::uint8_t> truncated(used+1);
    if(used)memory.read(GuestAddress{buffer},std::span(truncated).first(used));
    memory.write(GuestAddress{dst},truncated);
    return done(used+1);
}
