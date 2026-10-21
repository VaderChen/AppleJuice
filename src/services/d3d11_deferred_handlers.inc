        // 命令保存完整繪製狀態及資料快照；錄製期間不提交 GPU，也不保留客體暫存指標。
        if(!graphics_handled && symbol=="CreateDeferredContext"){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);require(!u32(1) && !(device->creation_flags&1));
            if(arg(2)){output(2);write_pointer(arg(2),0);auto context=create(Kind::Context,device->root);context->context_type=1;write_pointer(arg(2),context->root);}
            else result=1;
        }else if(!graphics_handled && symbol=="FinishCommandList"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context);
            require(context->context_type && std::none_of(context->deferred_maps.begin(),context->deferred_maps.end(),[](const auto& entry){return entry.second.pointer!=0;}));
            if(arg(2)){output(2);write_pointer(arg(2),0);auto list=create(Kind::CommandList,context->device);list->commands=std::move(context->commands);write_pointer(arg(2),list->root);}
            context->commands.clear();context->deferred_maps.clear();if(!u32(1))clear_bindings(context);
        }else if(!graphics_handled && symbol=="ExecuteCommandList"){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context),list=typed(arg(1),Kind::CommandList);require(context->device==list->device);
            if(std::getenv("X64ARM_TRACE_GRAPHICS_COMMANDS"))std::fprintf(stderr,"[d3d11_commands] execute context=%llx type=%u list=%llx commands=%zu restore=%u\n",static_cast<unsigned long long>(context->root),context->context_type,static_cast<unsigned long long>(list->root),list->commands.size(),u32(2));
            for(const auto& command:list->commands)submit(context,command);
            if(!u32(2))clear_bindings(context);
        }else if(!graphics_handled && (symbol=="GetContextType" || symbol=="GetContextFlags")){
            graphics_handled=true;const auto value=object(arg(0));require(value.kind==Kind::Context || (symbol=="GetContextFlags" && value.kind==Kind::CommandList));result=symbol=="GetContextType"?value.identity->context_type:0;
        }else if(!graphics_handled && (symbol=="Map" || symbol=="Unmap") && typed(arg(0),Kind::Context)->context_type){
            graphics_handled=true;auto context=typed(arg(0),Kind::Context),resource=object(arg(1)).identity;require(resource->device==context->device && bool(resource->resource) && !resource->offered);
            const auto key=std::pair{resource->root,u32(2)};const auto view=subresource(resource,u32(2));const auto& d=resource->resource->desc;
            if(symbol=="Map"){
                const auto type=u32(3),usage=word(resource->desc,d.dimension==1?4:d.dimension==2?16:d.dimension==4?20:28),access=word(resource->desc,d.dimension==1?12:d.dimension==2?24:d.dimension==4?28:36);
                require(usage==2 && access==0x10000 && (type==4 || type==5) && !u32(4) && arg(5));
                memory.check(GuestAddress{arg(5)},pointer_size+8,Access::Write);
                auto found=context->deferred_maps.find(key);require(found==context->deferred_maps.end() || !found->second.pointer);
                if(type==5)require(d.dimension==1 && (d.bind&3) && found!=context->deferred_maps.end());
                const auto pitch=d.dimension==1?std::array{d.width,1U}:graphics_format_pitch(d.format,std::max(1U,d.width>>view.level),std::max(1U,d.height>>view.level));
                const auto size=std::uint64_t{pitch[0]}*pitch[1]*std::max(1U,d.depth>>view.level);require(size && size<=512U*1024U*1024U);
                auto& map=context->deferred_maps[key];map.view=view;map.row=pitch[0];map.image=pitch[0]*pitch[1];if(type==4)map.bytes.assign(size,0);
                const auto pointer=allocate_heap(memory,size);if(!pointer)throw std::bad_alloc{};context->allocations.push_back(pointer);map.pointer=pointer;memory.write(GuestAddress{pointer},map.bytes);
                write_pointer(arg(5),pointer);memory.store(GuestAddress{arg(5)+pointer_size},32,map.row);memory.store(GuestAddress{arg(5)+pointer_size+4},32,map.image);
            }else{
                const auto found=context->deferred_maps.find(key);require(found!=context->deferred_maps.end() && found->second.pointer);auto& map=found->second;
                map.bytes=read_bytes(memory,map.pointer,map.bytes.size());const auto row=map.row,image=map.image;
                submit(context,{[view,data=map.bytes,row,image](auto& native){native.graphics_write(view,data,row,image);}});
                require(free_heap(memory,map.pointer,state));std::erase(context->allocations,map.pointer);map.pointer=0;
            }
        }
