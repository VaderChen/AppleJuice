        } else if(symbol=="D3D11On12CreateDevice"){
            for(unsigned i:{7U,8U})if(arg(i)){output(i);write_pointer(arg(i),0);}
            if(arg(9))memory.check(GuestAddress{arg(9)},4,Access::Write);
            std::uint32_t maximum=0;
            auto backend=on12_backend(d3d12_.get(),arg(0),maximum);
            require(backend && u32(6)<=1 && !(u32(1)&~0x8ffU));
            require(bool(arg(2))==bool(u32(3)) && (!u32(5) || arg(4)));
            auto level=maximum;
            if(arg(2)){
                level=0;
                constexpr std::array levels{0xc100U,0xc000U,0xb100U,0xb000U,0xa100U,0xa000U,0x9300U,0x9200U,0x9100U};
                for(std::uint64_t i=0;i<u32(3) && !level;++i){
                    const auto candidate=memory.load(GuestAddress{arg(2)+4*i},32);
                    if(candidate<=maximum && std::ranges::find(levels,candidate)!=levels.end())level=static_cast<std::uint32_t>(candidate);
                }
                require(level!=0);
            }
            std::set<std::uint64_t> queues;
            for(std::uint64_t i=0;i<u32(5);++i){
                const auto q=memory.load(GuestAddress{arg(4)+pointer_size*i},pointer_bits);
                require(on12_direct_queue(d3d12_.get(),arg(0),q) && queues.insert(q).second);
            }
            auto device=create(Kind::Device,0,false,std::move(backend));
            try{
                device->feature_level=level;
                device->creation_flags=u32(1);
                device->on12_refs.reserve(queues.size()+1);
                const auto retain12=[&](std::uint64_t address){on12_reference(address,true);device->on12_refs.push_back(address);};
                retain12(arg(0));
                for(auto q:queues)retain12(q);
                if(arg(8))write_pointer(arg(8),create(Kind::Context,device->root)->root);
                if(arg(7))write_pointer(arg(7),device->root);
                if(arg(9))memory.store(GuestAddress{arg(9)},32,level);
            }catch(...){release(device->root);throw;}
            if(!arg(7))release(device->root);
        } else if(symbol=="CreateWrappedResource"){
            auto device=typed(arg(0),Kind::On12Device);
            require(arg(6) && arg(5) && arg(2));
            output(6);write_pointer(arg(6),0);
            auto gpu=on12_resource(d3d12_.get(),device->on12_refs.front(),arg(1));
            require(bool(gpu));
            const auto& d=gpu->desc;
            const bool buffer=d.dimension==1;
            const auto flags=[&](unsigned i){return static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+4*i},32));};
            auto bind_flags=flags(0);
            const auto misc=flags(1),cpu=flags(2),stride=flags(3);
            require(!cpu && !misc && !stride && !(bind_flags&~(d.bind|(buffer?3U:0U))));
            if(!bind_flags)bind_flags=d.bind;
            const auto iid=read_guid(memory,arg(5));
            const std::array kinds{Kind::BufferObject,Kind::Texture1D,Kind::Texture,Kind::Texture3D};
            const std::array iids{IID_ID3D11Buffer,IID_ID3D11Texture1D,IID_ID3D11Texture2D,IID_ID3D11Texture3D};
            require(d.dimension>=1 && d.dimension<=4);
            if(iid!=IID_IUnknown && iid!=IID_ID3D11DeviceChild && iid!=IID_ID3D11Resource && iid!=iids[d.dimension-1])result=no_interface;
            else{
                std::vector<std::uint32_t> desc;
                if(buffer)desc={d.width,0,bind_flags,cpu,misc,stride};
                if(d.dimension==2)desc={d.width,d.levels,d.layers,d.format,0,bind_flags,cpu,misc};
                if(d.dimension==3)desc={d.width,d.height,d.levels,d.layers,d.format,d.samples,d.sample_quality,0,bind_flags,cpu,misc};
                if(d.dimension==4)desc={d.width,d.height,d.depth,d.levels,d.format,0,bind_flags,cpu,misc};
                auto wrapper=create(kinds[d.dimension-1],device->root);
                try{
                    wrapper->desc.resize(desc.size()*4);
                    std::memcpy(wrapper->desc.data(),desc.data(),wrapper->desc.size());
                    if(d.dimension==3)std::copy(desc.begin(),desc.end(),wrapper->texture_desc.begin());
                    wrapper->resource=std::move(gpu);
                    wrapper->wrapped_in=u32(3);wrapper->wrapped_out=u32(4);
                    wrapper->wrapped_acquired=true;
                    wrapper->on12_refs.reserve(1);
                    on12_reference(arg(1),true);
                    wrapper->on12_refs.push_back(arg(1));
                    write_pointer(arg(6),wrapper->root);
                }catch(...){release(wrapper->root);throw;}
            }
        } else if(symbol=="AcquireWrappedResources" || symbol=="ReleaseWrappedResources"){
            auto device=typed(arg(0),Kind::On12Device);
            const bool acquire=symbol=="AcquireWrappedResources";
            std::vector<std::shared_ptr<Identity>> resources;
            if(!arg(1)){
                require(!u32(2));
                for(const auto& [pointer,entry]:bridge.objects)
                    if(entry.identity->device==device->root && !entry.identity->on12_refs.empty() && pointer==entry.identity->root)
                        resources.push_back(entry.identity);
            }else for(std::uint64_t i=0;i<u32(2);++i){
                auto resource=object(memory.load(GuestAddress{arg(1)+pointer_size*i},pointer_bits)).identity;
                require(resource->device==device->root && !resource->on12_refs.empty());
                resources.push_back(std::move(resource));
            }
            // Metal 追蹤共用資源的轉換；UAV 路徑沿用 D3D12 重播的屏障。
            bool transition=false,uav=false;
            for(const auto& resource:resources)if(resource->wrapped_acquired!=acquire){
                transition=true;uav|=bool((resource->wrapped_in|resource->wrapped_out)&8);
            }
            if(transition){
                if(uav)device->native->graphics_uav_barrier();
                device->native->graphics_flush(true);
                for(auto& resource:resources)resource->wrapped_acquired=acquire;
            }
