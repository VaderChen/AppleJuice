        const D3dxShaderEntry shader_entry(service.symbol);
        if(shader_entry.valid && WindowsCall(state,memory,service)(shader_entry.pump_arg)){
            return nested_continuation(state,memory,"D3DX 著色器非同步工作",
                [this,service,shader_entry,create_source_loader](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                    const WindowsCall args(s,m,service);
                    const auto bits=static_cast<std::uint8_t>(x86_?32:64);
                    const auto unit=x86_?4U:8U;
                    const auto finish=[&](std::uint32_t result){
                        args.return_to(s,result);
                        return std::optional<std::uint32_t>{};
                    };
                    const auto pump=args(shader_entry.pump_arg),code=args(shader_entry.code_arg),errors=args(shader_entry.code_arg+1);
                    const auto status=shader_entry.has_status?args(shader_entry.code_arg+2):0;
                    if(!code)co_return finish(0x80070057U);
                    GuestScratch scratch(*this,m,s);
                    std::uint64_t loader=0,processor=0;
                    std::uint32_t result=0x80070057U;
                    try{
                        m.check(GuestAddress{code},unit,Access::Write);
                        if(errors)m.check(GuestAddress{errors},unit,Access::Write);
                        if(status)m.check(GuestAddress{status},4,Access::Write);
                        const auto table=m.load(GuestAddress{pump},bits);
                        if(!table)co_return finish(result);
                        const auto add=m.load(GuestAddress{table+3*unit},bits);
                        if(!add)co_return finish(result);
                        const auto slots=scratch.allocate(unit*2);
                        if(!slots)co_return finish(0x8007000eU);
                        auto name=args(shader_entry.file?0:2);
                        if(name && shader_entry.wide){
                            const auto utf8=utf16_to_utf8(read_utf16(m,GuestAddress{name}));
                            name=scratch.allocate(utf8.size()+1);
                            if(!name)co_return finish(0x8007000eU);
                            m.write(GuestAddress{name},std::span(reinterpret_cast<const std::uint8_t*>(utf8.data()),utf8.size()));
                        }
                        const unsigned first=shader_entry.macros_arg;
                        const auto macros=args(first),specified_include=args(first+1);
                        const auto include=(!specified_include && shader_entry.file)?1:specified_include;
                        const std::string version=shader_entry.d3d10?"D3DX10":"D3DX11";
                        const auto factory=shader_entry.effect_pool?"CreateAsyncEffectPoolCreateProcessor":
                            shader_entry.effect?"CreateAsyncEffectCreateProcessor":
                            shader_entry.preprocess?"CreateAsyncShaderPreprocessProcessor":"CreateAsyncCompilerProcessor";
                        const auto create=resolve(service.family,version+factory).value;
                        const auto loaded=create_source_loader(s,m,service,shader_entry.file,shader_entry.resource,
                            args(0),shader_entry.file?0:args(1),slots);
                        if(loaded&0x80000000U)co_return finish(loaded);
                        loader=m.load(GuestAddress{slots},bits);
                        std::vector<std::uint64_t> inputs{name,macros,include};
                        if(shader_entry.effect){
                            inputs.insert(inputs.end(),{args(first+2),args(first+3),args(first+4),args(first+5)});
                            if(!shader_entry.effect_pool)inputs.push_back(args(first+6));
                        }else{
                            if(!shader_entry.preprocess)
                                inputs.insert(inputs.end(),{args(first+2),args(first+3),args(first+4),args(first+5)});
                            inputs.push_back(code);
                        }
                        inputs.insert(inputs.end(),{errors,slots+unit});
                        result=static_cast<std::uint32_t>(co_await service_callback::call_value(create,std::move(inputs)));
                        if(!(result&0x80000000U)){
                            processor=m.load(GuestAddress{slots+unit},bits);
                            result=static_cast<std::uint32_t>(co_await service_callback::call_value(add,
                                {pump,loader,processor,status,shader_entry.effect?code:0}));
                        }
                    }catch(const MemoryFault&){result=0x80070057U;}
                    catch(const std::bad_alloc&){result=0x8007000eU;}
                    if(result&0x80000000U){
                        if(loader)(void)nested_service(s,m,"internal-d3d11","DataLoaderDestroy",{loader});
                        if(processor){
                            const auto table=m.load(GuestAddress{processor},bits);
                            (void)(co_await service_callback::call_value(m.load(GuestAddress{table+2*unit},bits),{processor}));
                        }
                    }
                    co_return finish(result);
                });
        }
