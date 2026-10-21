        const bool hierarchy_handled=symbol.starts_with("D3DXLoadMeshHierarchyFromX");
        if(hierarchy_handled){
            mesh_handled=true;const unsigned shift=symbol.ends_with("InMemory")?1U:0U;D3DXHierarchyOperation* operation=nullptr;
            for(auto& [key,item]:bridge.hierarchy_operations){(void)key;if(item.thread==threads_.current && item.stack==state.reg(Gpr::Rsp) && item.service.symbol==service.symbol){operation=&item;break;}}
            if(!operation){
                output(5+shift);if(arg(6+shift))output(6+shift);require(arg(3+shift) && !arg(4+shift),unavailable);(void)get(arg(2+shift),Kind::Device);std::vector<std::uint8_t> bytes;
                if(shift){require(arg(0) && u(1)<=512U*1024U*1024U);bytes=read(arg(0),u(1));}
                else{require(arg(0));const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));const auto file=filesystem_.open_reader(d3dx_path(path));require(!file.error,0x80070000U|file.error);const auto meta=file.file->information();require(!meta.error && meta.metadata.size<=512U*1024U*1024U);bytes.resize(meta.metadata.size);const auto io=file.file->read(bytes,0);require(!io.error && io.bytes==bytes.size());}
                D3DXHierarchyOperation next;next.id=++bridge.next_atlas_operation;next.thread=threads_.current;next.stack=state.reg(Gpr::Rsp);next.service=service;next.source=std::make_shared<d3d9_xfile::Hierarchy>(d3d9_xfile::Parser(d3d9_xfile::decompress(bytes)).hierarchy());require(!next.source->frames.empty());
                std::function<void(const d3d9_xfile::Frame&,unsigned,unsigned)> flatten=[&](const auto& source,unsigned parent,unsigned depth){require(depth<=256 && next.frames.size()<65536);const auto index=static_cast<unsigned>(next.frames.size());next.frames.push_back({&source,parent,0,0,0});for(unsigned m=0;m<source.meshes.size();++m)next.meshes.emplace_back(index,m);for(const auto& child:source.children)flatten(child,index,depth+1);};for(const auto& frame:next.source->frames)flatten(frame,UINT32_MAX,0);
                if(std::getenv("X64ARM_TRACE_D3DX_HIERARCHY"))std::fprintf(stderr,"[d3dx9 hierarchy] frames=%zu meshes=%zu sets=%zu\n",next.frames.size(),next.meshes.size(),next.source->sets.size());
                operation=&bridge.hierarchy_operations.emplace(next.id,std::move(next)).first->second;
            }
            auto& work=*operation;if(std::getenv("X64ARM_TRACE_D3DX_HIERARCHY"))std::fprintf(stderr,"[d3dx9 hierarchy] phase=%u index=%u returned=%u failure=%x\n",work.phase,work.index,unsigned(work.returned),work.failure);const auto allocator=arg(3+shift),table=memory.load(GuestAddress{allocator},pb);const auto slot=[&](unsigned index){return memory.load(GuestAddress{table+index*ps},pb);};
            const auto clear_temporary=[&](){for(auto pointer:work.owned)release(pointer);work.owned.clear();for(auto pointer:work.temporaries)(void)free_heap(memory,pointer,state);work.temporaries.clear();work.pending_output=0;};
            const auto hierarchy_temporary=[&](std::span<const std::uint8_t> bytes){const auto pointer=allocate_heap(memory,std::max<std::size_t>(bytes.size(),ps));require(pointer,out_of_memory);work.temporaries.push_back(pointer);if(!bytes.empty())memory.write(GuestAddress{pointer},bytes);return pointer;};
            const auto string_pointer=[&](const std::string& value){if(value.empty())return std::uint64_t{0};std::vector<std::uint8_t> bytes(value.begin(),value.end());bytes.push_back(0);return hierarchy_temporary(bytes);};
            if(work.returned){
                work.returned=work.awaiting=false;
                if(work.phase<2 && (work.callback_result&0x80000000U))work.failure=work.callback_result;
                else if(work.phase==0){const auto pointer=memory.load(GuestAddress{work.pending_output},pb);require(pointer);auto& frame=work.frames.at(work.index);frame.address=pointer;memory.write(GuestAddress{pointer+ps},std::span(reinterpret_cast<const std::uint8_t*>(frame.source->transform.data()),64));for(unsigned offset:{64+ps,64+2*ps,64+3*ps})store_pointer(pointer+offset,0);
                    auto& previous=frame.parent==UINT32_MAX?work.last_root:work.frames[frame.parent].last_child;if(previous)store_pointer(previous+64+2*ps,pointer);else if(frame.parent!=UINT32_MAX)store_pointer(work.frames[frame.parent].address+64+3*ps,pointer);previous=pointer;
                }else if(work.phase==1){const auto pointer=memory.load(GuestAddress{work.pending_output},pb);require(pointer);auto& frame=work.frames[work.meshes.at(work.index).first];store_pointer(pointer+8*ps,0);if(frame.last_mesh)store_pointer(frame.last_mesh+8*ps,pointer);else store_pointer(frame.address+64+ps,pointer);frame.last_mesh=pointer;work.containers.push_back(pointer);}
                ++work.index;clear_temporary();
            }
            if(work.failure && work.phase<2){work.phase=2;work.index=0;for(auto pointer:work.containers)work.cleanup.push_back({slot(3),{allocator,pointer}});for(auto frame=work.frames.rbegin();frame!=work.frames.rend();++frame)if(frame->address)work.cleanup.push_back({slot(2),{allocator,frame->address}});}
            if(work.phase==0 && work.index==work.frames.size()){work.phase=1;work.index=0;}
            if(work.phase==1 && work.index==work.meshes.size()){
                std::shared_ptr<Object> controller;
                try{if(arg(6+shift) && !work.source->sets.empty()){
                    controller=make_animation_controller(static_cast<unsigned>(work.frames.size()),static_cast<unsigned>(work.source->sets.size()),2,64);auto& data=*controller->animation_controller;
                    for(const auto& source:work.source->sets)data.sets.push_back(make_animation_set(d3dx_animation::from_x(source,work.source->ticks_per_second))->root);
                    for(const auto& frame:work.frames)if(!frame.source->name.empty())register_animation_output(data,frame.source->name,{frame.address+ps,0,0,0});
                    data.tracks[0].set=data.sets.front();hold(get_animation_set(data.tracks[0].set));data.tracks[1].enabled=false;
                }store_pointer(arg(5+shift),work.frames.front().address);if(controller)store_pointer(arg(6+shift),controller->root);}
                catch(...){if(controller)release(controller->root);throw;}
            }else if(work.phase==2 && work.index==work.cleanup.size())result=work.failure;
            else{
                GuestCallback callback;
                if(work.phase==0){work.pending_output=hierarchy_temporary(std::vector<std::uint8_t>(ps));callback={slot(0),{allocator,string_pointer(work.frames.at(work.index).source->name),work.pending_output}};}
                else if(work.phase==1){const auto [frame_index,mesh_index]=work.meshes.at(work.index);const auto& source=work.frames[frame_index].source->meshes[mesh_index];auto mesh=make_loaded_mesh(source,u(1+shift),arg(2+shift));work.owned.push_back(mesh->root);std::shared_ptr<Object> skin;
                    if(!source.bones.empty()){skin=make_skin(mesh->width,mesh->presentation,static_cast<unsigned>(source.bones.size()));work.owned.push_back(skin->root);for(unsigned b=0;b<source.bones.size();++b){auto& bone=skin->skin->bones[b];const auto& from=source.bones[b];bone.name=from.name;bone.vertices=from.vertices;bone.weights=from.weights;bone.offset=from.offset;}}
                    auto materials=material_buffer(source.materials);work.owned.push_back(materials->root);auto effects=effect_buffer(source.materials);work.owned.push_back(effects->root);
                    const auto adjacent=adjacency(*mesh,1e-6F);const auto adjacency_pointer=hierarchy_temporary(std::span(reinterpret_cast<const std::uint8_t*>(adjacent.data()),adjacent.size()*4));std::vector<std::uint8_t> mesh_data(2*ps);put(mesh_data,0,1);std::memcpy(mesh_data.data()+ps,&mesh->root,ps);const auto mesh_pointer=hierarchy_temporary(mesh_data);work.pending_output=hierarchy_temporary(std::vector<std::uint8_t>(ps));
                    callback={slot(1),{allocator,string_pointer(source.name),mesh_pointer,materials->mapped,effects->mapped,source.materials.size(),adjacency_pointer,skin?skin->root:0,work.pending_output}};
                }else callback=work.cleanup.at(work.index);
                work.awaiting=true;start_callbacks(state,memory,{std::move(callback)},Continuation::D3DXProgressOperation,work.id);return std::nullopt;
            }
        }
