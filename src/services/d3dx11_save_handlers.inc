        if(symbol=="D3DX11SaveTextureToFileA" || symbol=="D3DX11SaveTextureToFileW" || symbol=="D3DX11SaveTextureToMemory"){
            graphics_handled=true;
            const bool to_memory=symbol=="D3DX11SaveTextureToMemory";
            require(arg(3)!=0);
            if(to_memory){output(3);write_pointer(arg(3),0);require(!u32(4));}
            const auto context_it=bridge.objects.find(arg(0)),resource_it=bridge.objects.find(arg(1));
            require(context_it!=bridge.objects.end() && context_it->second.kind==Kind::Context && resource_it!=bridge.objects.end());
            const auto context=context_it->second.identity,resource=resource_it->second.identity;
            require(resource_kind(resource->kind) && resource->resource && resource->device==context->device && !context->context_type && !resource->mapped && !resource->offered);
            const auto& desc=resource->resource->desc;
            require(desc.dimension>=2 && desc.samples==1);
            context->native->graphics_flush(true);
            auto data=texture_image::encode(desc,u32(2),[&](unsigned index){return context->native->graphics_read(subresource(resource,index));});
            if(to_memory){
                auto id=create(Kind::Blob);
                try{const auto ptr=allocate_heap(memory,std::max<std::size_t>(1,data.size()));if(!ptr)throw std::bad_alloc{};id->allocations.push_back(ptr);id->mapped=ptr;id->sequence=data.size();memory.write(GuestAddress{ptr},data);write_pointer(arg(3),id->root);}
                catch(...){release(id->root);throw;}
            }else{
                const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(3)})):read_ansi(memory,arg(3));
                require(!path.empty());
                auto file=filesystem_.open_host(path,0x40000000U,2,0x80,1);
                if(file.error)result=0x80070000U|(file.error&0xffffU);
                else{const auto written=file.file->write(data,0);if(written.error)result=0x80070000U|(written.error&0xffffU);else if(written.bytes!=data.size())result=0x8007001dU;}
            }
        }
