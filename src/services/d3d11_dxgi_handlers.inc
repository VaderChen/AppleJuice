        if(symbol=="OfferResources" || symbol=="ReclaimResources"){
            graphics_handled=true;auto device=typed(arg(0),Kind::DxgiDevice);
            const bool offer=symbol=="OfferResources";require(u32(1)>0 && u32(1)<=2048 && arg(2));
            if(offer)require(u32(3)>=1 && u32(3)<=3);
            else if(arg(3))memory.check(GuestAddress{arg(3)},std::size_t{u32(1)}*4,Access::Write);
            std::vector<std::shared_ptr<Identity>> resources;resources.reserve(u32(1));
            for(unsigned i=0;i<u32(1);++i){
                const auto ptr=memory.load(GuestAddress{arg(2)+i*pointer_size},pointer_bits);
                auto id=typed(ptr,Kind::DxgiResource);require(id->device==device->root && id->resource && id->subresource_maps.empty());
                const auto dimension=id->resource->desc.dimension;
                const auto usage=word(id->desc,dimension==1?4:dimension==2?16:dimension==4?20:28);
                require(usage!=1 && !(id->resource->desc.misc&(2U|0x100U|0x800U)) && (offer || id->offered));
                resources.push_back(std::move(id));
            }
            if(offer){
                // Metal 資源仍實際駐留，不任意丟棄內容；先完成使用中命令，並解除所有 context 綁定。
                device->native->graphics_flush(true);
                std::vector<std::uint64_t> detached;
                for(auto& [pointer,item]:bridge.objects){
                    (void)pointer;if(item.kind!=Kind::Context || item.identity->device!=device->root)continue;
                    for(auto& [name,values]:item.identity->bindings){(void)name;for(auto& value:values)if(value){
                        auto bound=object(value).identity;if(bound->texture)bound=object(bound->texture).identity;
                        if(std::find(resources.begin(),resources.end(),bound)!=resources.end()){detached.push_back(value);value=0;}
                    }}
                }
                for(auto ptr:detached)release(ptr);
            }
            for(unsigned i=0;i<resources.size();++i){resources[i]->offered=offer;if(!offer && arg(3))memory.store(GuestAddress{arg(3)+4*i},32,0);}
        }else if(symbol=="EnqueueSetEvent"){
            graphics_handled=true;auto device=typed(arg(0),Kind::DxgiDevice);const auto event=runtime_.wait_objects.find(arg(1));
            require(event!=runtime_.wait_objects.end() && event->second.kind==WaitObject::Kind::Event && (event->second.access&2));
            device->native->graphics_flush(true);event->second.set_signal(true);
        }else if(symbol=="QueryResourceResidency"){
            graphics_handled=true;auto device=typed(arg(0),Kind::DxgiDevice);require(u32(3)<=2048);
            memory.check(GuestAddress{arg(2)},std::size_t{u32(3)}*4,Access::Write);
            for(unsigned i=0;i<u32(3);++i){const auto id=object(memory.load(GuestAddress{arg(1)+i*pointer_size},pointer_bits)).identity;require(resource_kind(id->kind) && id->device==device->root && id->resource);}
            for(unsigned i=0;i<u32(3);++i)memory.store(GuestAddress{arg(2)+4*i},32,1); // 實際由 Metal 持有的 UMA 資源。
        }else if(symbol=="DxgiResourceUsage" || symbol=="DxgiSetEvictionPriority" || symbol=="DxgiGetEvictionPriority" || symbol=="DxgiSharedHandle"){
            graphics_handled=true;auto id=typed(arg(0),Kind::DxgiResource);
            if(symbol=="DxgiSetEvictionPriority")id->priority=u32(1);
            else if(symbol=="DxgiGetEvictionPriority")memory.store(GuestAddress{arg(1)},32,id->priority);
            else if(symbol=="DxgiSharedHandle"){
                write_pointer(arg(1),0);if(!id->resource || !(id->resource->desc.misc&2))result=0x887a0001U;
                else{std::uint32_t handle=0;for(auto it=bridge.shared_resources.begin();it!=bridge.shared_resources.end();){auto resource=it->second.resource.lock();if(!resource)it=bridge.shared_resources.erase(it);else{if(resource==id->resource)handle=it->first;++it;}}
                    if(!handle){handle=bridge.next_shared_resource++;bridge.shared_resources.emplace(handle,D3D11SharedResource{id->resource,id->native,id->desc});}
                    write_pointer(arg(1),handle);}
            }
            else {require(bool(id->resource));const auto bind_flags=id->resource->desc.bind;std::uint32_t usage=0;
                if(bind_flags&8)usage|=0x10;if(bind_flags&0x20)usage|=0x20;if(bind_flags&0x80)usage|=0x400;
                memory.store(GuestAddress{arg(1)},32,usage);}
        }
