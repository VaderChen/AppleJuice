        if(effect_compile){
            graphics_handled=true;const bool file=service.symbol.find("FromFile")!=service.symbol.npos;const unsigned delta=file?0U:2U;
            if(!result){const auto pointer=memory.load(GuestAddress{effect_code_slot},pointer_bits);auto compiled=typed(pointer,Kind::Blob);
                try{const auto bytes=read_bytes(memory,compiled->mapped,static_cast<std::size_t>(compiled->sequence));const auto effect=fx_create(original_arg(6+delta),bytes);write_pointer(original_arg(9+delta),effect->root);}catch(...){release(pointer);throw;}release(pointer);
            }
            if(original_arg(11+delta))memory.store(GuestAddress{original_arg(11+delta)},32,result);
        }else if(symbol=="D3D10CreateEffectFromMemory"){
            graphics_handled=true;require(arg(5) && !arg(4) && !(u32(2)&~3U) && arg(1)>0 && arg(1)<=64*1024*1024);output(5);write_pointer(arg(5),0);const auto bytes=read_bytes(memory,arg(0),static_cast<std::size_t>(arg(1)));write_pointer(arg(5),fx_create(arg(3),bytes)->root);
        }
