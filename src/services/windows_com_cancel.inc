    if(name=="CoSetCancelObject" || name=="CoGetCancelObject" || name=="CoCancelCall" || name=="CoTestCancel"){
        return nested_continuation(state,memory,name,[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);
            const auto finish=[&](std::uint32_t hr){call.return_to(s,hr);return std::optional<std::uint32_t>{};};
            const auto op=service.symbol;
            const bool setting=op=="CoSetCancelObject",getting=op=="CoGetCancelObject",testing=op=="CoTestCancel";
            const std::uint8_t width=x86_?32:64;
            auto target=threads_.current;
            auto& registry=runtime_.com_contexts;
            try{
                if(getting){
                    if(!call(2))co_return finish(context_invalid);
                    m.store(GuestAddress{call(2)},width,0);
                    if(!call(1))co_return finish(context_invalid);
                }
                if(!setting && !testing){
                    const auto tid=static_cast<std::uint32_t>(call(0));
                    if(tid){
                        const auto found=std::find_if(threads_.items.begin(),threads_.items.end(),[&](const auto& row){
                            return row.second.status!=ThreadStatus::Exited && thread_identifier(row.first)==tid;});
                        if(found==threads_.items.end())co_return finish(context_invalid);
                        target=found->first;
                    }
                }
                const auto found=registry.cancel_stacks.find(target);
                const auto top=found==registry.cancel_stacks.end()?0:found->second.back();
                if(setting){
                    if(!com_apartment(target) && !mta_alive())co_return finish(not_initialized);
                    if(registry.borrowed.contains(top))co_return finish(0x80070005U);
                    if(!call(0)){
                        if(!top)co_return finish(context_unexpected);
                        found->second.pop_back();
                        if(found->second.empty())registry.cancel_stacks.erase(found);
                        (void)co_await service_callback::call(context_method(m,top,2),{top});
                        co_return finish(0);
                    }
                }else if(!getting && !testing && !threads_.items.at(target).com_cancel_enabled)co_return finish(cancel_disabled);
                const auto object=setting?call(0):testing?threads_.items.at(target).com_call_context:top;
                if(!object)co_return finish(getting?context_no_interface:call_complete);
                const auto buf=allocate_heap(m,24);
                if(!buf)co_return finish(context_no_memory);
                struct Scratch{WindowsServices& self;GuestMemory& m;GuestState& s;std::uint64_t p;~Scratch(){(void)self.free_heap(m,p,s);}} scratch{*this,m,s,buf};
                m.write(GuestAddress{buf+8},context_iid(0x29));
                m.store(GuestAddress{buf},width,0);
                const auto method=context_method(m,object,0);
                const bool pin=!setting && !testing;
                if(pin)++registry.borrowed[object];
                struct Pin{
                    ComContextBridge& registry;std::uint64_t object;bool active;
                    void drop(){if(active){auto it=registry.borrowed.find(object);if(!--it->second)registry.borrowed.erase(it);active=false;}}
                    ~Pin(){drop();}
                } hold{registry,object,pin};
                const auto hr=static_cast<std::uint32_t>(co_await service_callback::call(method,{object,getting?call(1):buf+8,buf}));
                hold.drop();
                if(pin && !registry.borrowed.contains(object)){
                    auto releases=registry.deferred_releases.extract(object);
                    if(releases)for(std::size_t i=0;i<releases.mapped();++i)
                        (void)co_await service_callback::call(context_method(m,object,2),{object});
                }
                if(hr&0x80000000U)co_return finish(hr);
                const auto iface=m.load(GuestAddress{buf},width);
                if(!iface)co_return finish(context_no_interface);
                std::uint32_t status=0;
                if(setting){
                    if(!com_apartment(target) && !mta_alive())status=not_initialized;
                    else try{
                        registry.cancel_stacks[target].push_back(iface);
                        co_return finish(0);
                    }catch(const std::bad_alloc&){
                        if(auto it=registry.cancel_stacks.find(target);it!=registry.cancel_stacks.end() && it->second.empty())registry.cancel_stacks.erase(it);
                        status=context_no_memory;
                    }
                }else if(getting){
                    try{m.store(GuestAddress{call(2)},width,iface);co_return finish(0);}
                    catch(const MemoryFault&){status=context_pointer_error;}
                }else{
                    std::vector<std::uint64_t> args{iface};
                    if(!testing)args.push_back(static_cast<std::uint32_t>(call(1)));
                    status=static_cast<std::uint32_t>(co_await service_callback::call(context_method(m,iface,testing?4:3),std::move(args)));
                }
                (void)co_await service_callback::call(context_method(m,iface,2),{iface});
                co_return finish(status);
            }catch(const MemoryFault&){co_return finish(context_pointer_error);}
             catch(const std::bad_alloc&){co_return finish(context_no_memory);}
        });
    }
