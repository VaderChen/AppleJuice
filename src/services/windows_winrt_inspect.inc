    if(service.symbol=="WindowsInspectString" || service.symbol=="WindowsInspectString2"){
        return nested_continuation(state,memory,"HSTRING 檢視",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);
            const auto finish=[&](std::uint32_t hr){call.return_to(s,hr);return std::optional<std::uint32_t>{};};
            const bool wide=service.symbol=="WindowsInspectString2";
            const std::uint8_t out_bits=wide || !x86_?64:32;
            const auto target=call(0),callback=call(2),context=call(3),out_len=call(4),out_ptr=call(5);
            const auto machine=static_cast<std::uint16_t>(call(1));
            const bool target64=machine==0x8664;
            constexpr std::uint32_t invalid=0x80070057U;
            try{
                if(!out_len || !out_ptr)co_return finish(invalid);
                m.store(GuestAddress{out_len},32,0);m.store(GuestAddress{out_ptr},out_bits,0);
                if(!callback || (!target64 && machine!=0x14c && machine!=0x1c0) ||
                   (target64 && out_bits==32) || (!target64 && target>UINT32_MAX))co_return finish(invalid);
                if(!target)co_return finish(0);
                const unsigned size=target64?24U:20U;
                const auto buf=allocate_heap(m,size);if(!buf)co_return finish(0x8007000eU);
                struct Release{WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t addr;
                    ~Release(){(void)self.free_heap(memory,addr,state);}} release{*this,m,s,buf};
                std::vector<std::uint64_t> args{context,target};
                if(x86_ && wide)args.push_back(target>>32);
                args.push_back(size);args.push_back(buf);
                const auto hr=static_cast<std::uint32_t>(co_await service_callback::call_value(callback,std::move(args)));
                if(hr&0x80000000U)co_return finish(hr);
                const auto length=m.load(GuestAddress{buf+4},32);
                const auto data=m.load(GuestAddress{buf+16},target64?64:32);
                const auto limit=target64?UINT64_MAX:std::uint64_t{UINT32_MAX};
                if((data&1) || (!data && length) || length*2>limit-data)co_return finish(invalid);
                m.store(GuestAddress{out_len},32,length);m.store(GuestAddress{out_ptr},out_bits,data);
                co_return finish(0);
            }catch(const MemoryFault&){co_return finish(0x80004003U);}
        });
    }
