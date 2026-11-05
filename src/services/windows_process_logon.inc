if(service.symbol=="CreateProcessWithLogonW")
return service_callback::invoke<std::optional<std::uint32_t>>(
    this,threads_.current,state,memory,"CreateProcessWithLogonW",
    [this](GuestState& s,GuestMemory& m,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
        start_callbacks(s,m,{{target,std::move(args)}},Continuation::NestedResult,id);
    },
    [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto flags=static_cast<std::uint32_t>(call(3));
        const auto done=[&](bool success,std::uint32_t error=0){if(!success)last_error_=error;call.return_to(s,success);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        struct Resources {
            WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t scratch{},token{},environment{};
            ~Resources(){
                if(environment){self.runtime_.environment_blocks.erase(environment);(void)self.free_heap(memory,environment,state);}
                if(token){self.runtime_.token_handles.erase(token);self.runtime_.token_handle_objects.erase(token);self.runtime_.handle_flags.erase(token);self.collect_tokens();}
                if(scratch)(void)self.free_heap(memory,scratch,state);
            }
        } resources{*this,s,m};
        try{
            if((flags&~3U) || flags==3 || !call(0) || !call(2) || !call(9) || !call(10))co_return done(false,87);
            if(call(5) && read_utf16(m,GuestAddress{call(5)},1025).size()>1024)co_return done(false,87);
            m.check(GuestAddress{call(10)},2*unit+8,Access::Write);
            resources.scratch=allocate_heap(m,2*unit);if(!resources.scratch)co_return done(false,8);
            m.write(GuestAddress{resources.scratch},std::vector<std::uint8_t>(2*unit));
            const auto logged=co_await service_callback::call(resolve("advapi32.dll","LogonUserW").value,
                {call(0),call(1),call(2),(flags&2U)?9U:2U,3,resources.scratch});
            if(!logged)co_return done(false,last_error_);
            resources.token=m.load(GuestAddress{resources.scratch},bits);
            auto environment=call(7);auto creation=static_cast<std::uint32_t>(call(6));
            // 虛擬使用者的 hive 已由 SID 隔離的 registry store 載入；環境由同一 profile 建立。
            if(!environment){
                if(!(co_await service_callback::call(resolve("userenv.dll","CreateEnvironmentBlock").value,{resources.scratch+unit,resources.token,0})))co_return done(false,last_error_);
                resources.environment=environment=m.load(GuestAddress{resources.scratch+unit},bits);creation|=0x400U;
            }
            const auto created=co_await service_callback::call(resolve("advapi32.dll","CreateProcessAsUserW").value,
                {resources.token,call(4),call(5),0,0,0,creation,environment,call(8),call(9),call(10)});
            co_return done(created!=0,last_error_);
        }catch(const MemoryFault&){co_return done(false,998);}catch(const std::bad_alloc&){co_return done(false,8);}
    });
