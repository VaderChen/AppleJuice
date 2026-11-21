        if(effect_compile){
            graphics_handled=true;
            if(!result){
                const auto pointer=memory.load(GuestAddress{effect_code_slot},pointer_bits);
                const auto compiled=typed(pointer,Kind::Blob);
                try{
                    const auto bytes=read_bytes(memory,compiled->mapped,static_cast<std::size_t>(compiled->sequence));
                    const auto pool=effect_compile_pool?0:original_arg(effect_device_arg+1);
                    const auto flags=static_cast<std::uint32_t>(original_arg(effect_device_arg-1));
                    const auto effect=fx_create(original_arg(effect_device_arg),bytes,flags,effect_compile_pool,pool);
                    write_pointer(original_arg(effect_output_arg),effect);
                }catch(...){release(pointer);throw;}
                release(pointer);
            }
        }else if(symbol=="D3D10CreateEffectFromMemory" || symbol=="D3D10CreateEffectPoolFromMemory"){
            graphics_handled=true;
            const bool make_pool=symbol=="D3D10CreateEffectPoolFromMemory";
            const unsigned destination=make_pool?4U:5U;
            require(arg(destination)!=0);
            output(destination);
            write_pointer(arg(destination),0);
            require(arg(0) && arg(1)>0 && arg(1)<=64U*1024U*1024U);
            const auto bytes=read_bytes(memory,arg(0),static_cast<std::size_t>(arg(1)));
            write_pointer(arg(destination),fx_create(arg(3),bytes,u32(2),make_pool,make_pool?0:arg(4)));
        }
