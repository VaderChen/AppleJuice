            if(name.starts_with("WdtpInterfacePointer_User")){
                auto operation=name.substr(std::string_view("WdtpInterfacePointer_User").size());
                const bool ndr64=operation.ends_with("64");
                if(ndr64)operation.remove_suffix(2);
                if(operation=="Free"){
                    if(const auto object=call(0))(void)co_await com(object,2,{});
                    co_return finish(0);
                }
                const unsigned count_bytes=ndr64?8U:4U;
                const auto align=[&](std::uint64_t n){return (n+count_bytes-1)&~std::uint64_t(count_bytes-1);};
                const auto slot=scratch(16);
                if(operation=="Size"){
                    const auto start=static_cast<std::uint32_t>(call(2));
                    const auto hr=static_cast<std::uint32_t>(co_await api("ole32.dll","CoGetMarshalSizeMax",{slot,call(4),call(3),call(1)&0xffff,0,0}));
                    co_return finish(hr?start:static_cast<std::uint32_t>(align(start)+count_bytes+4+m.load(GuestAddress{slot},32)));
                }
                const bool writing=operation=="Marshal";
                const auto buffer=call(writing?2:1),header=align(buffer),data=header+count_bytes+4;
                const auto iid=call(writing?4:3),object=call(writing?3:2);
                const auto context=writing?(call(1)&0xffff):0;
                const auto raise=[&](std::uint32_t code){
                    start_callbacks(s,m,{{resolve("kernel32.dll","RaiseException").value,{code,0,0,0}}},Continuation::Return,0);
                    return std::optional<std::uint32_t>{};
                };
                std::uint64_t length=0,previous=0;
                if(!writing){
                    length=m.load(GuestAddress{header+count_bytes},32);
                    if(m.load(GuestAddress{header},static_cast<std::uint8_t>(count_bytes*8))!=length)co_return raise(1783);
                    m.check(GuestAddress{data},length,Access::Read);
                    previous=m.load(GuestAddress{object},bits);
                }
                if(co_await api("ole32.dll","CreateStreamOnHGlobal",{0,1,slot}))
                    co_return writing?finish(0):raise(14);
                const auto stream=m.load(GuestAddress{slot},bits);
                const auto seek=[&](unsigned origin,std::uint64_t result){
                    std::vector<std::uint64_t> args{0};
                    if(x86_)args.push_back(0);
                    args.push_back(origin);args.push_back(result);
                    return com(stream,5,std::move(args));
                };
                std::uint32_t hr=0;
                if(writing){
                    hr=static_cast<std::uint32_t>(co_await api("ole32.dll","CoMarshalInterface",{stream,iid,object,context,0,0}));
                    if(!hr){
                        hr=static_cast<std::uint32_t>(co_await seek(2,slot+8));
                        length=m.load(GuestAddress{slot+8},64);
                        if(length>UINT32_MAX)hr=e_outofmemory;
                        if(!hr)hr=static_cast<std::uint32_t>(co_await seek(0,0));
                        if(!hr){
                            try{m.check(GuestAddress{header},count_bytes+4+length,Access::Write);}
                            catch(const MemoryFault&){hr=e_pointer;}
                        }
                        if(!hr)hr=static_cast<std::uint32_t>(co_await com(stream,3,{data,length,0}));
                        if(hr){
                            if(!(co_await seek(0,0)))(void)co_await api("ole32.dll","CoReleaseMarshalData",{stream});
                        }else{
                            m.store(GuestAddress{header},static_cast<std::uint8_t>(count_bytes*8),length);
                            m.store(GuestAddress{header+count_bytes},32,length);
                        }
                    }
                }else{
                    hr=static_cast<std::uint32_t>(co_await com(stream,4,{data,length,0}));
                    if(!hr)hr=static_cast<std::uint32_t>(co_await seek(0,0));
                    if(!hr)hr=static_cast<std::uint32_t>(co_await api("ole32.dll","CoUnmarshalInterface",{stream,iid,object}));
                }
                (void)co_await com(stream,2,{});
                if(hr)co_return writing?finish(buffer):raise(hr);
                if(previous)(void)co_await com(previous,2,{});
                co_return finish(data+length);
            }
