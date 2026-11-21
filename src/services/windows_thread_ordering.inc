void WindowsServices::release_thread_ordering(std::uint64_t id) noexcept {
    for(auto it=thread_ordering_contexts_.begin();it!=thread_ordering_contexts_.end();){
        if(it->second->thread()!=id){++it;continue;}
        try{(void)it->second->remove(true);}catch(...){}
        it=thread_ordering_contexts_.erase(it);
    }
}

std::optional<std::uint32_t> WindowsServices::invoke_thread_ordering(GuestState& state,GuestMemory& memory,
    const WindowsServiceDescriptor& service) {
    using Context=ThreadOrderingContext;
    using Role=Context::Role;
    const WindowsCall arg(state,memory,service);
    const auto symbol=service.symbol;
    const auto id=threads_.current;
    const std::uint8_t width=x86_?32:64;
    const auto done=[&](std::uint32_t error){
        if(error)last_error_=error;
        arg.return_to(state,error?0:1);
        return std::optional<std::uint32_t>{};
    };
    try {
        const bool create=symbol.starts_with("AvRtCreateThreadOrderingGroup");
        const bool join=symbol=="AvRtJoinThreadOrderingGroup";
        if(create || join){
            const auto guid_address=GuestAddress{arg(create?2:1)};
            const auto output=GuestAddress{arg(0)};
            if(!output.value || !guid_address.value || (create && !arg(1)))return done(87);
            memory.check(output,width/8,Access::Write);
            Context::Guid guid{};
            memory.read(guid_address,guid);
            const bool generate=create && guid==Context::Guid{};
            if(generate)memory.check(guid_address,guid.size(),Access::Write);
            std::uint64_t period=0,timeout=0;
            int priority=8;
            if(create){
                constexpr std::int64_t minimum=5000,maximum=0x1fffffffffffffff;
                const auto duration=[&](std::uint64_t raw){return static_cast<std::uint64_t>(
                    std::clamp(std::bit_cast<std::int64_t>(raw),minimum,maximum));};
                period=duration(memory.load(GuestAddress{arg(1)},64));
                const auto raw=arg(3)?memory.load(GuestAddress{arg(3)},64):0;
                timeout=raw==UINT64_MAX?raw:raw?duration(raw):std::min<std::uint64_t>(period*5,maximum);
                if(symbol.ends_with('A') || symbol.ends_with('W')){
                    if(!arg(4))return done(87);
                    auto name=symbol.ends_with('W')?read_utf16(memory,GuestAddress{arg(4)},256)
                        :utf8_to_utf16_lossy(read_ansi(memory,arg(4),256));
                    for(auto& unit:name)unit=unicode_case_unit(unit,false);
                    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
                    if(transaction.error)return done(transaction.error);
                    const auto profile=multimedia_profile(std::move(name));
                    if(profile.error)return done(profile.error);
                    priority=profile.priority;
                }
            } else if(guid==Context::Guid{}) {
                return done(87);
            }
            const auto role=create?Role::Parent:static_cast<std::uint32_t>(arg(2))?Role::Before:Role::After;
            const auto session=mapping_session();
            const auto wake=mapping_wake();
            const auto handle=runtime_.allocate_wait_handle();
            const auto node=thread_ordering_contexts_.try_emplace(handle).first;
            try {
                // 只在自動配置的小機率碰撞時換 GUID。明確指定的識別碼保留 ERROR_ALREADY_EXISTS。
                do {
                    if(generate){
                        if(!random_bytes(guid))throw Context::Error{31};
                        guid[7]=(guid[7]&0x0fU)|0x40U;
                        guid[8]=(guid[8]&0x3fU)|0x80U;
                    }
                    try{node->second=Context::open(session,guid,wake,id,thread_identifier(id),role,period,timeout,priority);}
                    catch(const Context::Error& error){if(!generate || error.code!=183)throw;}
                }while(!node->second);
                if(generate)memory.write(guid_address,guid);
                memory.store(output,width,handle);
            } catch(...) {
                if(node->second){try{(void)node->second->remove(true);}catch(...){}}
                thread_ordering_contexts_.erase(node);
                throw;
            }
            return done(0);
        }

        const auto found=thread_ordering_contexts_.find(arg(0));
        if(found==thread_ordering_contexts_.end())return done(6);
        const auto& context=*found->second;
        const bool remove_parent=symbol=="AvRtDeleteThreadOrderingGroup";
        if(context.thread()!=id)return done(remove_parent?1:5);
        if(remove_parent || symbol=="AvRtLeaveThreadOrderingGroup"){
            if(remove_parent!=context.parent())return done(remove_parent?1:87);
            const auto error=found->second->remove(false);
            thread_ordering_contexts_.erase(found);
            return done(error);
        }
        ThreadWait wait;
        wait.kind=ThreadWaitKind::ThreadOrdering;
        wait.handles={arg(0)};
        wait.order=static_cast<std::uint64_t>(context.broker_priority());
        const auto turn=found->second->turn(true);
        if(!turn.waiting)return done(turn.error);
        wait.deadline=runtime_.host_clock?turn.deadline:UINT64_MAX;
        auto& thread=threads_.items.at(id);
        thread.wait=std::move(wait);
        thread.status=ThreadStatus::Waiting;
        return {};
    } catch(const Context::Error& error){return done(error.code);}
      catch(const MemoryFault&){return done(998);}
      catch(const std::bad_alloc&){return done(8);}
      catch(const std::invalid_argument&){return done(87);}
      catch(const HostCallFault&){return done(5);}
}
