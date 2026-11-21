            require(arg(4));
            memory.store(GuestAddress{arg(4)},32,0);
            output(5);
            for(unsigned i=6;i<9;++i)if(arg(i))output(i);

            auto source=get(arg(0),Kind::Mesh);
            auto source_vb=get(source->vertex_buffer,Kind::VertexBuffer);
            const auto flags=u(3);
            const auto limit=std::min(u(2),(flags&1)?source->width:65535U);
            require(u(2)>=3 && arg(1) && !(flags&~0x1ffffU) && !(flags&0x1000U));
            require(!source->mapped && !source_vb->mapped);
            const auto original_indices=mesh_indices(*source);
            std::vector<std::uint32_t> original_adjacency(original_indices.size());
            memory.read(GuestAddress{arg(1)},std::span(
                reinterpret_cast<std::uint8_t*>(original_adjacency.data()),original_adjacency.size()*4));
            for(const auto neighbor:original_adjacency)
                require(neighbor==UINT32_MAX || neighbor<source->height,0x88760b59U);

            struct Partition {
                std::vector<unsigned> faces,vertices,indices;
            };
            std::vector<Partition> partitions(1);
            std::vector<unsigned> vertex_map(source->width,UINT32_MAX),face_partition(source->height),face_local(source->height);
            // 保留原本面順序及屬性；分段時複製跨區共用的頂點，再將鄰接轉為個別網格的面索引。
            for(unsigned face=0;face<source->height;++face){
                const auto* indices=original_indices.data()+std::size_t(face)*3;
                unsigned new_vertices=0;
                for(unsigned corner=0;corner<3;++corner)
                    if(vertex_map[indices[corner]]==UINT32_MAX &&
                        std::find(indices,indices+corner,indices[corner])==indices+corner)
                        ++new_vertices;
                if(partitions.back().vertices.size()+new_vertices>limit){
                    for(const auto vertex:partitions.back().vertices)vertex_map[vertex]=UINT32_MAX;
                    partitions.emplace_back();
                }
                auto& partition=partitions.back();
                face_partition[face]=static_cast<unsigned>(partitions.size()-1);
                face_local[face]=static_cast<unsigned>(partition.faces.size());
                partition.faces.push_back(face);
                for(unsigned corner=0;corner<3;++corner){
                    const auto vertex=indices[corner];
                    auto& mapped=vertex_map[vertex];
                    if(mapped==UINT32_MAX){
                        mapped=static_cast<unsigned>(partition.vertices.size());
                        partition.vertices.push_back(vertex);
                    }
                    partition.indices.push_back(mapped);
                }
            }

            std::array<std::shared_ptr<Object>,4> outputs{};
            std::vector<std::uint64_t> meshes;
            meshes.reserve(partitions.size());
            const auto allocate_output=[&](unsigned index,std::size_t bytes){
                require(bytes<=UINT32_MAX,out_of_memory);
                auto& buffer=outputs[index];
                buffer=create(Kind::Blob);
                // COM 物件釋放時一併釋放 guest payload。指標表與 DWORD 資料共用同一塊配置。
                buffer->pixels.resize(bytes);
                buffer->mapped=allocate_heap(memory,bytes);
                require(buffer->mapped,out_of_memory);
                return buffer->mapped;
            };
            try{
                const auto pointer_bytes=partitions.size()*ps;
                const auto mesh_array=allocate_output(0,pointer_bytes);
                for(unsigned i=1;i<outputs.size();++i)if(arg(i+5)){
                    std::size_t words=0;
                    for(const auto& partition:partitions)
                        words+=i==1?partition.indices.size():i==2?partition.faces.size():partition.vertices.size();
                    allocate_output(i,pointer_bytes+words*4);
                }
                std::array<std::size_t,4> data_offsets{0,pointer_bytes,pointer_bytes,pointer_bytes};
                for(unsigned i=0;i<partitions.size();++i){
                    const auto& partition=partitions[i];
                    auto target=make_mesh(static_cast<unsigned>(partition.faces.size()),
                        static_cast<unsigned>(partition.vertices.size()),flags,source->presentation,source->parent);
                    meshes.push_back(target->root);
                    auto vb=get(target->vertex_buffer,Kind::VertexBuffer);
                    auto ib=get(target->index_buffer,Kind::IndexBuffer);
                    for(std::size_t vertex=0;vertex<partition.vertices.size();++vertex)
                        std::copy_n(source_vb->pixels.data()+std::size_t(partition.vertices[vertex])*source->stream_stride,
                            source->stream_stride,vb->pixels.data()+vertex*target->stream_stride);
                    for(std::size_t face=0;face<partition.faces.size();++face)
                        put(target->pixels,face*4,word(source->pixels,std::size_t(partition.faces[face])*4));
                    for(std::size_t at=0;at<partition.indices.size();++at){
                        const auto vertex=partition.indices[at];
                        if(flags&1)put(ib->pixels,at*4,vertex);
                        else{
                            ib->pixels[at*2]=static_cast<std::uint8_t>(vertex);
                            ib->pixels[at*2+1]=static_cast<std::uint8_t>(vertex>>8);
                        }
                    }
                    store_pointer(mesh_array+i*ps,target->root);
                    for(unsigned kind=1;kind<outputs.size();++kind)if(outputs[kind]){
                        const auto base=outputs[kind]->mapped;
                        store_pointer(base+i*ps,base+data_offsets[kind]);
                        const auto write_index=[&](unsigned index){
                            memory.store(GuestAddress{base+data_offsets[kind]},32,index);
                            data_offsets[kind]+=4;
                        };
                        if(kind==1){
                            for(const auto face:partition.faces)for(unsigned edge=0;edge<3;++edge){
                                const auto neighbor=original_adjacency[std::size_t(face)*3+edge];
                                write_index(neighbor!=UINT32_MAX && face_partition[neighbor]==i?face_local[neighbor]:UINT32_MAX);
                            }
                        }else for(const auto index:kind==2?partition.faces:partition.vertices)write_index(index);
                    }
                }
                for(unsigned i=0;i<outputs.size();++i)if(outputs[i])store_pointer(arg(5+i),outputs[i]->root);
                memory.store(GuestAddress{arg(4)},32,partitions.size());
            }catch(...){
                for(const auto& buffer:outputs)if(buffer)release(buffer->root);
                for(const auto mesh:meshes)release(mesh);
                throw;
            }
