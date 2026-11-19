// VCOMP 的鎖變數存放客體臨界區段指標。競爭時讓既有客體排程器接手等待。
    if(service.family=="vcomp140.dll"){
        return nested_continuation(state,memory,"OpenMP 鎖",[this,service](GuestState& current,GuestMemory& mem)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(current,mem,service);
            const auto cell=GuestAddress{call(0)};
            const std::uint8_t bits=x86_?32:64;
            const auto name=service.symbol;
            const auto kernel=[&](std::string_view api){return resolve("kernel32.dll",api).value;};
            std::uint64_t value=0;
            if(name.starts_with("omp_init_")){
                mem.check(cell,bits/8,Access::Write);
                const auto section=allocate_heap(mem,x86_?24:40);
                if(!section)(void)co_await service_callback::call(kernel("ExitProcess"),{1});
                try{
                    (void)co_await service_callback::call(kernel("InitializeCriticalSection"),{section});
                    mem.store(cell,bits,section);
                }catch(...){(void)free_heap(mem,section,current);throw;}
            }else{
                const auto section=mem.load(cell,bits);
                if(name.starts_with("omp_destroy_")){
                    if(section){
                        mem.check(cell,bits/8,Access::Write);
                        (void)co_await service_callback::call(kernel("DeleteCriticalSection"),{section});
                        (void)free_heap(mem,section,current);
                        mem.store(cell,bits,0);
                    }
                }else if(name.starts_with("omp_unset_")){
                    (void)co_await service_callback::call(kernel("LeaveCriticalSection"),{section});
                }else{
                    const bool test=name.starts_with("omp_test_");
                    const bool nested=name.ends_with("_nest_lock");
                    if(!nested && critical_owned_by(mem,section,runtime_.critical_sections.at(section),threads_.current)){
                        if(!test)(void)co_await service_callback::call(kernel("ExitProcess"),{1});
                    }else if(test){
                        if(co_await service_callback::call_value(kernel("TryEnterCriticalSection"),{section}))
                            value=nested?critical_recursion(mem,section,runtime_.critical_sections.at(section)):1;
                    }else (void)co_await service_callback::call(kernel("EnterCriticalSection"),{section});
                }
            }
            call.return_to(current,value);
            co_return std::nullopt;
        });
    }
