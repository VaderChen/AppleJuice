    const D3dxTextureEntry entry(service.symbol);
    if(entry.valid && WindowsCall(state,memory,service)(entry.pump_arg)){
        return nested_continuation(state,memory,"D3DX 紋理非同步工作",
            [this,service,entry,create_source_loader](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall args(s,m,service);
                const auto bits=static_cast<std::uint8_t>(x86_?32:64);
                const auto unit=x86_?4U:8U;
                const auto finish=[&](std::uint32_t status){
                    args.return_to(s,status);
                    return std::optional<std::uint32_t>{};
                };
                const auto destination=args(entry.out_arg),status=args(entry.status_arg),pump=args(entry.pump_arg);
                if(!destination || (!entry.info && !args(0)))co_return finish(0x80070057U);
                GuestScratch scratch(*this,m,s);
                std::uint64_t loader=0,processor=0;
                std::uint32_t queued=0x80070057U;
                try{
                    m.check(GuestAddress{destination},entry.info?36U:unit,Access::Write);
                    if(status)m.check(GuestAddress{status},4,Access::Write);
                    const auto table=m.load(GuestAddress{pump},bits);
                    if(!table)co_return finish(queued);
                    const auto add=m.load(GuestAddress{table+3*unit},bits);
                    if(!add)co_return finish(queued);
                    const auto slots=scratch.allocate(unit*2);
                    if(!slots)co_return finish(0x8007000eU);
                    const std::string version=service.symbol.starts_with("D3DX10")?"D3DX10":"D3DX11";
                    const auto loaded=create_source_loader(s,m,service,entry.file,entry.resource,
                        args(entry.source_arg),entry.file?0:args(entry.source_arg+1),slots);
                    if(loaded&0x80000000U)co_return finish(loaded);
                    loader=m.load(GuestAddress{slots},bits);
                    const std::string processor_name=version+(entry.info?"CreateAsyncTextureInfoProcessor":
                        entry.view?"CreateAsyncShaderResourceViewProcessor":"CreateAsyncTextureProcessor");
                    queued=static_cast<std::uint32_t>(entry.info?
                        nested_service(s,m,service.family,processor_name,{destination,slots+unit}):
                        nested_service(s,m,service.family,processor_name,{args(0),args(entry.load_arg),slots+unit}));
                    if(!(queued&0x80000000U)){
                        processor=m.load(GuestAddress{slots+unit},bits);
                        queued=static_cast<std::uint32_t>(co_await service_callback::call_value(add,
                            {pump,loader,processor,status,entry.info?0:destination}));
                    }
                }catch(const MemoryFault&){queued=0x80070057U;}
                catch(const std::bad_alloc&){queued=0x8007000eU;}
                if(queued&0x80000000U){
                    if(loader)(void)nested_service(s,m,"internal-d3d11","DataLoaderDestroy",{loader});
                    if(processor)(void)nested_service(s,m,"internal-d3d11","DataProcessorDestroy",{processor});
                }
                co_return finish(queued);
            });
    }
