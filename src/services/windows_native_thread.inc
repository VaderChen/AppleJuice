// Native 建立家族共用 Win32 的執行緒、TLS、排程、物件安全與退出流程。
if(service.family=="ntdll.dll") {
    const auto name=service.symbol.starts_with("Zw")?"Nt"+std::string(service.symbol.substr(2)):std::string(service.symbol);
    const auto saved_error=last_error_;
    const auto finish=[&](std::uint32_t status)->std::optional<std::uint32_t>{
        last_error_=saved_error;threads_.items.at(threads_.current).last_native_status=status;
        arg.return_to(state,status);return std::nullopt;
    };
    if(name=="RtlExitUserThread")return exit_thread(state,memory,static_cast<std::uint32_t>(arg(0)));
    if(name=="NtTerminateThread") {
        const auto target=arg(0)?arg(0):(x86_?UINT64_C(0xfffffffe):UINT64_MAX-1);
        const auto type=object_type(target);
        if(type!=u"Thread")return finish(type.empty()?0xc0000008U:0xc0000024U);
        const auto id=thread_id(target);
        if(!id)return finish(0xc00000bbU); // 跨程序 thread control 尚未提供強制終止請求。
        if(!(thread_access(target)&1U))return finish(0xc0000022U);
        if(!arg(0) && std::none_of(threads_.items.begin(),threads_.items.end(),[&](const auto& item){return item.first!=id && item.second.status!=ThreadStatus::Exited;}))
            return finish(0xc00000dbU); // NULL 不得終止程序最後一個執行緒。
        auto& thread=threads_.items.at(id);
        if(thread.status==ThreadStatus::Exited || thread.forced_stop)return finish(0xc000004bU);
        (void)detail::prepare_forced_thread_stop(thread,static_cast<std::uint32_t>(arg(1)),GuestAddress{callback_return_.value+0xa0});
        notify_concurrency_changed();threads_.yield=true;
        if(id==threads_.current){state.rip=thread.context.rip;return force_finish_thread(state,memory);}
        return finish(0);
    }
    if(name=="NtCreateThread" || name=="NtCreateThreadEx" || name=="RtlCreateUserThread") {
        const bool legacy=name=="NtCreateThread",rtl=name=="RtlCreateUserThread";
        const unsigned unit=x86_?4U:8U;const std::uint8_t bits=x86_?32:64;
        try {
            const auto process=arg(rtl?0:3),output=arg(rtl?8:0),client=legacy?arg(4):rtl?arg(9):0;
            const auto process_type=object_type(process);
            if(process_type!=u"Process")return finish(process_type.empty()?0xc0000008U:0xc0000024U);
            if(!process_handle_pid(process,2))return finish(0xc0000022U);
            const bool remote=!current_process_handle(process);
            const auto probe=[&](std::uint64_t address,std::uint64_t size){memory.check(GuestAddress{address},static_cast<std::size_t>(size),Access::Write);};
            if(!output && !rtl)return finish(0xc0000005U);
            if((output && output%unit) || (client && client%unit))return finish(0x80000002U);
            if(output)probe(output,unit);
            if(client)probe(client,unit*2);
            GuestThreadBootstrap bootstrap;
            bootstrap.access=security::creation_access(rtl?0x1fffffU:static_cast<std::uint32_t>(arg(1)),security::thread_access_mapping);
            if(bootstrap.access&0x40U)bootstrap.access|=0x800U;
            if(bootstrap.access&0x20U)bootstrap.access|=0x400U;
            std::uint64_t descriptor=rtl?arg(1):0;
            if(!rtl && arg(2)) {
                const auto at=arg(2);if(at%unit)return finish(0x80000002U);
                memory.check(GuestAddress{at},6*unit,Access::Read);
                if(memory.load(GuestAddress{at},32)!=6*unit)return finish(0xc000000dU);
                const auto pointer=[&](unsigned index){return memory.load(GuestAddress{at+index*unit},bits);};
                const auto flags=static_cast<std::uint32_t>(pointer(3));
                if(flags&~0x42U)return finish(0xc000000dU);
                if(pointer(1) || pointer(2))return finish(0xc0000033U);
                if(pointer(5))return finish(0xc00000bbU);
                bootstrap.inherit=(flags&2U)!=0;descriptor=pointer(4);
            }
            bootstrap.security_descriptor=descriptor?object_security_descriptor(security::read_descriptor(memory,descriptor,x86_,true),true,true):synchronization_descriptor(memory,0).bytes;
            std::uint64_t entry=0,parameter=0,requested=0;
            bool suspended=false,default_stack=false;
            struct OutputAttribute {std::uint64_t kind,address,length;};
            std::vector<OutputAttribute> attributes;
            if(legacy) {
                const auto context=arg(5),initial=arg(6);const auto mode=x86_?ExecutionMode::X86:ExecutionMode::X64;
                if(context%(x86_?4U:16U) || initial%unit)return finish(0x80000002U);
                memory.check(GuestAddress{context},thread_context::size(mode),Access::Read);
                memory.check(GuestAddress{initial},5*unit,Access::Read);
                const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{context+thread_context::flags_offset(mode)},32));
                const auto error=thread_context::validate_flags(mode,flags);
                if(error || !(flags&1U))return finish(error==50?0xc00000bbU:0xc000000dU);
                std::vector<std::uint8_t> bytes(thread_context::size(mode));memory.read(GuestAddress{context},bytes);
                GuestState initial_state;initial_state.segments=state.segments;
                try{bootstrap.context=thread_context::restore(initial_state,mode,bytes);}catch(const HostCallFault& error){
                    if(std::getenv("X64ARM_TRACE_THREAD_CREATION"))std::fprintf(stderr,"native_thread_create_context pid=%u detail=%s\n",process_id(),error.what());
                    return finish(0xc00000bbU);
                }
                const auto pointer=[&](unsigned index){return memory.load(GuestAddress{initial+index*unit},bits);};
                if(pointer(0) || pointer(1))return finish(0xc00000bbU);
                bootstrap.stack_high=pointer(2);bootstrap.stack_low=pointer(3);bootstrap.stack_allocation=pointer(4);
                const auto sp=bootstrap.context->reg(Gpr::Rsp);
                if(!bootstrap.stack_allocation || bootstrap.stack_allocation>bootstrap.stack_low || bootstrap.stack_low>=bootstrap.stack_high ||
                   sp<bootstrap.stack_low || sp>=bootstrap.stack_high || bootstrap.stack_high>user_address_limit())return finish(0xc000000dU);
                if(!remote)memory.check(GuestAddress{sp},unit,Access::Write);
                entry=bootstrap.context->rip.value;suspended=static_cast<std::uint8_t>(arg(7))!=0;
            } else {
                entry=arg(rtl?6:4);parameter=arg(rtl?7:5);
                const auto flags=rtl?(static_cast<std::uint8_t>(arg(2))?1U:0U):static_cast<std::uint32_t>(arg(6));
                if(flags&~0x77U)return finish(0xc000000dU);
                if(flags&0x70U)return finish(0xc00000bbU);
                suspended=(flags&1U)!=0;bootstrap.skip_thread_attach=(flags&2U)!=0;bootstrap.hidden_from_debugger=(flags&4U)!=0;
                if(arg(rtl?3:7))return finish(0xc00000bbU);
                const auto commit=arg(rtl?5:8),reserve=arg(rtl?4:9);
                if(commit>UINT32_MAX || reserve>UINT32_MAX)return finish(0xc0000017U);
                if(reserve && commit>reserve)return finish(0xc000000dU);
                // 沿用共用堆疊的預先提交政策；保留容量至少涵蓋明確 commit 與 image 預設。
                requested=reserve?reserve:std::max(commit,runtime_.image.stack_size);
                if(remote && !reserve){requested=commit;default_stack=true;}
                if(!rtl && arg(10)) {
                    const auto list=arg(10);if(list%unit)return finish(0x80000002U);
                    const auto size=memory.load(GuestAddress{list},bits);
                    if(size<unit || size>unit+64*4*unit || (size-unit)%(4*unit))return finish(0xc000000dU);
                    memory.check(GuestAddress{list},size,Access::Read);
                    for(std::uint64_t offset=unit;offset<size;offset+=4*unit) {
                        const auto at=list+offset,kind=memory.load(GuestAddress{at},bits),length=memory.load(GuestAddress{at+unit},bits);
                        const auto buffer=memory.load(GuestAddress{at+2*unit},bits),returned=memory.load(GuestAddress{at+3*unit},bits);
                        if(kind!=0x10003 && kind!=0x10004)return finish(0xc00000bbU);
                        if(length!=(kind==0x10003?2*unit:unit) || std::any_of(attributes.begin(),attributes.end(),[&](const auto& attribute){return attribute.kind==kind;}))return finish(0xc000000dU);
                        if(buffer%unit || (returned && returned%unit))return finish(0x80000002U);
                        probe(buffer,length);if(returned)probe(returned,unit);
                        attributes.push_back({kind,buffer,returned});
                    }
                }
            }
            if(remote){
                RemoteThreadOutputs outputs;outputs.native=true;outputs.arguments=service.arguments;
                outputs.handle=output;outputs.client=client;outputs.suspended=suspended;outputs.default_stack=default_stack;
                for(const auto& attribute:attributes)outputs.attributes.push_back({attribute.kind,attribute.address,attribute.length});
                const auto error=begin_remote_thread_creation(memory,process,2,entry,parameter,requested,bootstrap,std::move(outputs));
                if(error)return finish(error==5?0xc0000022U:error==6?0xc0000008U:error==8?0xc0000017U:error==50?0xc00000bbU:0xc000000dU);
                schedule(state,memory);return std::nullopt;
            }
            // 先暫停建立；所有輸出與安全參數已探測，完成發布後才允許排程。
            const auto handle=create_guest_thread(state,memory,entry,parameter,requested,4,&bootstrap);
            if(!handle)return finish(0xc0000017U);
            const auto id=thread_id(handle);auto& thread=threads_.items.at(id);
            const auto write_client=[&](std::uint64_t address){memory.store(GuestAddress{address},bits,process_id());memory.store(GuestAddress{address+unit},bits,thread.public_id);};
            if(client)write_client(client);
            for(const auto& attribute:attributes) {
                if(attribute.kind==0x10003)write_client(attribute.address);
                else memory.store(GuestAddress{attribute.address},bits,thread.teb);
                if(attribute.length)memory.store(GuestAddress{attribute.length},bits,attribute.kind==0x10003?unit*2:unit);
            }
            if(output)memory.store(GuestAddress{output},bits,handle);
            else {runtime_.process_thread_handles.erase(handle);runtime_.thread_handle_access.erase(handle);runtime_.handle_flags.erase(handle);}
            thread.suspended=suspended?1:0;
            if(!suspended){notify_concurrency_changed();threads_.yield=true;}
            return finish(0);
        } catch(const MemoryFault&){return finish(0xc0000005U);}
          catch(const SecurityDescriptorError& error){return finish(error.code==5?0xc0000022U:error.code==1314?0xc0000061U:error.code==87?0xc000000dU:0xc0000079U);}
          catch(const std::bad_alloc&){return finish(0xc0000017U);}
          catch(const std::invalid_argument&){return finish(0xc000000dU);}
          catch(const HostCallFault&){return finish(0xc000009aU);}
    }
}
