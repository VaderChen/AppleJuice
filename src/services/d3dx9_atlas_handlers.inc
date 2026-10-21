        const bool atlas_handled=symbol=="D3DXUVAtlasCreate" || symbol=="D3DXComputeIMTFromPerVertexSignal" || symbol=="D3DXComputeIMTFromPerTexelSignal" || symbol=="D3DXComputeIMTFromTexture";
        if(atlas_handled){
            mesh_handled=true;const bool atlas=symbol=="D3DXUVAtlasCreate";
            D3DXAtlasOperation* operation=nullptr;
            for(auto& [key,item]:bridge.atlas_operations){(void)key;if(item.thread==threads_.current && item.stack==state.reg(Gpr::Rsp) && item.service.symbol==service.symbol){operation=&item;break;}}
            if(!operation){
                auto mesh=get(arg(0),Kind::Mesh);d3dx_atlas::Request request;request.indices=mesh_indices(*mesh);
                for(const auto& position:positions(*mesh))request.positions.push_back({position[0],position[1],position[2]});
                D3DXAtlasOperation next;next.id=++bridge.next_atlas_operation;next.thread=threads_.current;next.stack=state.reg(Gpr::Rsp);next.service=service;next.mesh=mesh;
                next.stride=mesh->stream_stride;next.flags=mesh->flags;next.declaration=mesh->presentation;next.attributes=mesh->pixels;next.vertices=get(mesh->vertex_buffer,Kind::VertexBuffer)->pixels;
                const auto floats=[&](std::uint64_t pointer,std::uint64_t count){require(pointer && count<=128U*1024U*1024U);auto bytes=read(pointer,count*4);std::vector<float> values(count);std::memcpy(values.data(),bytes.data(),bytes.size());for(auto v:values)require(std::isfinite(v));return values;};
                const auto integers=[&](std::uint64_t pointer,std::size_t count,bool adjacency_values=true){require(pointer);auto bytes=read(pointer,count*4);std::vector<unsigned> values(count);std::memcpy(values.data(),bytes.data(),bytes.size());if(adjacency_values)for(auto v:values)require(v==UINT32_MAX || v<mesh->height);return values;};
                if(atlas){
                    output(14);if(arg(15))output(15);if(arg(16))output(16);
                    request.charts=u(1);request.stretch=scalar_arg(2);request.width=u(3);request.height=u(4);request.gutter=scalar_arg(5);next.texture_index=u(6);
                    require(request.width && request.height && request.width<=16384 && request.height<=16384 && std::isfinite(request.stretch) && request.stretch>=0 && request.stretch<=1 && std::isfinite(request.gutter) && request.gutter>=0);
                    const auto element=offset_of(*mesh,5,next.texture_index);require(element!=UINT32_MAX && mesh->presentation[element+4]>=1 && mesh->presentation[element+4]<=3);
                    request.adjacency=integers(arg(7),request.indices.size());if(arg(8))request.false_edges=integers(arg(8),request.indices.size(),false);if(arg(9))request.imt=floats(arg(9),mesh->height*3ULL);
                    next.callback=arg(10);next.context=arg(12);request.frequency=scalar_arg(11);request.options=u(13);require(!(request.options&~3U) && request.options!=3 && std::isfinite(request.frequency) && request.frequency>=0);
                }else if(symbol=="D3DXComputeIMTFromPerVertexSignal"){
                    output(7);request.mode=1;request.dimension=u(2);request.stride=u(3);require(!u(4) && request.dimension && request.dimension<=4096 && request.stride>=request.dimension*4 && request.stride%4==0);
                    request.signal=floats(arg(1),std::uint64_t{request.stride/4}*(mesh->width-1)+request.dimension);next.callback=arg(5);next.context=arg(6);
                }else{
                    const bool texture=symbol=="D3DXComputeIMTFromTexture";request.mode=texture?3:2;next.texture_index=u(texture?2:1);
                    for(const auto& uv:positions(*mesh,5,next.texture_index,2))request.texcoords.push_back({uv[0],uv[1]});
                    if(texture){
                        output(6);auto texture_object=get(arg(1),Kind::Texture);auto surface=get(texture_object->subresources.at(0),Kind::Surface);require(!surface->mapped);read_surface(*surface);
                        request.width=surface->width;request.height=surface->height;request.options=u(3);request.components=request.dimension=4;next.callback=arg(4);next.context=arg(5);
                        auto rgba=texture_image::transform(surface->pixels,native_format(surface->format),{surface->width,surface->height,1},{0,0,0,surface->width,surface->height,1},2,{surface->width,surface->height,1},1);
                        request.signal.resize(rgba.size()/4);std::memcpy(request.signal.data(),rgba.data(),rgba.size());
                    }else{
                        output(10);request.width=u(3);request.height=u(4);request.dimension=u(5);request.components=u(6);request.options=u(7);next.callback=arg(8);next.context=arg(9);
                        require(request.width && request.height && request.width<=16384 && request.height<=16384 && request.dimension && request.dimension<=request.components && request.components<=4096);
                        request.signal=floats(arg(2),std::uint64_t{request.width}*request.height*request.components);
                    }require(!(request.options&~3U));
                }
                next.output=std::make_shared<d3dx_atlas::Result>();auto result_data=next.output;
                next.worker=std::make_unique<NativeProgressJob>(next.callback!=0,[request=std::move(request),result_data](const NativeProgressJob::Progress& progress){return d3dx_atlas::run(request,progress,*result_data);});
                operation=&bridge.atlas_operations.emplace(next.id,std::move(next)).first->second;
            }
            float progress=0;
            if(operation->worker->wait(progress)){
                require(std::isfinite(progress));GuestCallback callback{operation->callback,{bits(progress),operation->context}};callback.floating_arguments=1;
                start_callbacks(state,memory,{std::move(callback)},Continuation::D3DXProgressOperation,operation->id);return std::nullopt;
            }
            result=operation->worker->result;const auto& produced=*operation->output;
            if(atlas){if(arg(17))memory.store(GuestAddress{arg(17)},32,bits(produced.stretch));if(arg(18))memory.store(GuestAddress{arg(18)},32,produced.charts);}
            if(result&0x80000000U){if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3dx9 atlas] %s HRESULT=%08x %s\n",std::string(symbol).c_str(),static_cast<unsigned>(result),operation->worker->error.c_str());}
            else if(atlas){
                require(produced.uv.size()==produced.remap.size() && produced.indices.size()==operation->mesh->height*3ULL && produced.partitions.size()==operation->mesh->height);
                auto mesh=make_mesh(operation->mesh->height,static_cast<unsigned>(produced.uv.size()),operation->flags|(produced.uv.size()>65536?1U:0U),operation->declaration,operation->mesh->parent);
                std::shared_ptr<Object> partition,remap;
                try{
                    auto vb=get(mesh->vertex_buffer,Kind::VertexBuffer),ib=get(mesh->index_buffer,Kind::IndexBuffer);const auto element=offset_of(*mesh,5,operation->texture_index);const unsigned offset=unsigned(mesh->presentation[element+2])|unsigned(mesh->presentation[element+3])<<8;
                    for(unsigned i=0;i<mesh->width;++i){require(produced.remap[i]<operation->vertices.size()/operation->stride);std::copy_n(operation->vertices.begin()+produced.remap[i]*operation->stride,operation->stride,vb->pixels.begin()+i*operation->stride);put(vb->pixels,i*operation->stride+offset,bits(produced.uv[i][0]));put(vb->pixels,i*operation->stride+offset+4,bits(produced.uv[i][1]));}
                    for(unsigned i=0;i<produced.indices.size();++i){require(produced.indices[i]<mesh->width);if(mesh->flags&1)put(ib->pixels,i*4,produced.indices[i]);else {ib->pixels[i*2]=static_cast<std::uint8_t>(produced.indices[i]);ib->pixels[i*2+1]=static_cast<std::uint8_t>(produced.indices[i]>>8);}}
                    mesh->pixels=operation->attributes;
                    if(arg(15))partition=blob(std::span(reinterpret_cast<const std::uint8_t*>(produced.partitions.data()),produced.partitions.size()*4));
                    if(arg(16))remap=blob(std::span(reinterpret_cast<const std::uint8_t*>(produced.remap.data()),produced.remap.size()*4));
                    if(partition)store_pointer(arg(15),partition->root);if(remap)store_pointer(arg(16),remap->root);store_pointer(arg(14),mesh->root);
                }catch(...){if(partition)release(partition->root);if(remap)release(remap->root);release(mesh->root);throw;}
            }else{
                const unsigned destination=symbol=="D3DXComputeIMTFromPerVertexSignal"?7:symbol=="D3DXComputeIMTFromTexture"?6:10;
                store_pointer(arg(destination),blob(std::span(reinterpret_cast<const std::uint8_t*>(produced.imt.data()),produced.imt.size()*4))->root);
            }
            bridge.atlas_operations.erase(operation->id);
        }
