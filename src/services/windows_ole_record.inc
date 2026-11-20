// OLE 私有串流以四個 DWORD 起始，後接含自身長度欄位的持久化 moniker。
if(name=="ReadOleStg" || name=="WriteOleStg"){
    const bool writing=name=="WriteOleStg";
    const auto storage=call(0),stream_out=call(writing?3:5),moniker_out=writing?0:call(4);
    for(auto out:{stream_out,moniker_out})if(out)m.store(GuestAddress{out},bits,0);
    if(!storage)co_return finish(e_invalidarg);
    if(!writing)for(unsigned i=1;i<=3;++i)if(call(i))m.check(GuestAddress{call(i)},4,Access::Write);
    constexpr std::uint32_t version=0x02000001U,bad_format=0x8004006aU;
    constexpr std::array<std::uint8_t,16> moniker_id{15,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    constexpr std::array<std::uint8_t,16> link_id{0x1d,1,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    const auto stream_name=bytes(utf16_bytes(u"\x0001" u"Ole")),cell=scratch(unit),header=scratch(16),count=scratch(4),pos=scratch(8);
    const auto iid=bytes(writing?link_id:moniker_id);
    auto hr=static_cast<std::uint32_t>(writing?
        co_await api("ole32.dll","OpenOrCreateStream",{storage,stream_name,cell}):
        co_await com(storage,4,{stream_name,0,0x10,0,cell}));
    if(hr)co_return finish(writing?hr:0x80030002U);
    const auto stream=m.load(GuestAddress{cell},bits);
    const auto seek=[&](std::uint64_t offset,unsigned origin,std::uint64_t result){
        return com(stream,5,x86_?std::vector<std::uint64_t>{static_cast<std::uint32_t>(offset),offset>>32,origin,result}:
                                 std::vector<std::uint64_t>{offset,origin,result});
    };
    const auto read=[&](std::uint64_t buf,unsigned size)->service_callback::Task<std::uint32_t>{
        m.store(GuestAddress{count},32,0);
        const auto status=static_cast<std::uint32_t>(co_await com(stream,3,{buf,size,count}));
        co_return status?status:static_cast<std::uint32_t>(m.load(GuestAddress{count},32)!=size);
    };
    std::uint64_t moniker=0,link=0;
    std::exception_ptr failure;
    try{
        if(!writing){
            hr=co_await read(header,16);
            if(!hr && m.load(GuestAddress{header},32)!=version)hr=bad_format;
            if(!hr){
                for(unsigned i=1;i<=2;++i)if(call(i))m.store(GuestAddress{call(i)},32,m.load(GuestAddress{header+4*i},32));
                if(m.load(GuestAddress{header+12},32))hr=bad_format;
            }
            if(!hr){
                if(call(3))m.store(GuestAddress{call(3)},32,0);
                hr=co_await read(header,4);
                if(!hr && m.load(GuestAddress{header},32)){
                    m.store(GuestAddress{cell},bits,0);
                    hr=static_cast<std::uint32_t>(co_await api("ole32.dll","OleLoadFromStream",{stream,iid,cell}));
                    if(!hr)moniker=m.load(GuestAddress{cell},bits);
                }
            }
        }else{
            const auto object=call(1);
            m.store(GuestAddress{header},32,version);
            hr=static_cast<std::uint32_t>(co_await com(stream,4,{header,4,0}));
            if(!hr){
                const auto read_status=co_await read(header,4);
                auto flags=read_status?0U:static_cast<std::uint32_t>(m.load(GuestAddress{header},32)&2U);
                m.store(GuestAddress{cell},bits,0);
                if(object && !static_cast<std::uint32_t>(co_await com(object,0,{iid,cell}))){
                    link=m.load(GuestAddress{cell},bits);
                    if(link){
                        flags|=1;
                        (void)co_await com(link,4,{header+4});
                        (void)co_await com(link,2,{});
                        link=0;
                    }
                }
                m.store(GuestAddress{header},32,flags);
                hr=static_cast<std::uint32_t>(co_await seek(4,0,0));
            }
            if(!hr)hr=static_cast<std::uint32_t>(co_await com(stream,4,{header,12,0}));
            if(!hr){
                m.store(GuestAddress{cell},bits,0);
                if(object && !static_cast<std::uint32_t>(co_await com(object,8,{1,2,cell})))moniker=m.load(GuestAddress{cell},bits);
                m.store(GuestAddress{header},32,0);
                hr=static_cast<std::uint32_t>(co_await com(stream,4,{header,4,0}));
                if(!hr && moniker){
                    hr=static_cast<std::uint32_t>(co_await api("ole32.dll","OleSaveToStream",{moniker,stream}));
                    if(!hr)hr=static_cast<std::uint32_t>(co_await seek(0,1,pos));
                    if(!hr){
                        const auto end=m.load(GuestAddress{pos},64);
                        if(end<20 || end-16>UINT32_MAX)hr=0x80030019U;
                        else{
                            m.store(GuestAddress{header},32,end-16);
                            hr=static_cast<std::uint32_t>(co_await seek(16,0,0));
                            if(!hr)hr=static_cast<std::uint32_t>(co_await com(stream,4,{header,4,0}));
                            if(!hr)hr=static_cast<std::uint32_t>(co_await seek(end,0,0));
                        }
                    }
                }
                if(!static_cast<std::uint32_t>(co_await seek(0,1,pos))){
                    const auto end=m.load(GuestAddress{pos},64);
                    (void)co_await com(stream,6,x86_?std::vector<std::uint64_t>{static_cast<std::uint32_t>(end),end>>32}:std::vector<std::uint64_t>{end});
                }
            }
        }
        if(!hr){
            if(moniker_out)m.store(GuestAddress{moniker_out},bits,moniker);
            if(stream_out)m.store(GuestAddress{stream_out},bits,stream);
        }
    }catch(...){failure=std::current_exception();}
    if(link)(void)co_await com(link,2,{});
    if(moniker && (failure || hr || !moniker_out))(void)co_await com(moniker,2,{});
    if(failure || hr || !stream_out)(void)co_await com(stream,2,{});
    if(failure)std::rethrow_exception(failure);
    co_return finish(hr);
}
