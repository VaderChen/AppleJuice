                else if(method=="ConvertToBlendedMesh" || method=="ConvertToIndexedBlendedMesh"){
                    const bool indexed=method=="ConvertToIndexedBlendedMesh";const unsigned shift=indexed?1U:0U;output(10+shift);for(unsigned n:{6U,9U})if(arg(n+shift))output(n+shift);
                    auto mesh=get(arg(1),Kind::Mesh);if(std::getenv("X64ARM_TRACE_D3DX_MESH"))std::fprintf(stderr,"[d3dx9 skin] mesh_vertices=%u skin_vertices=%u faces=%u\n",mesh->width,info.vertices,mesh->height);require(mesh->width==info.vertices);const auto indices=mesh_indices(*mesh);auto influences=info.influences();unsigned maximum_vertex=0,maximum_face=0;
                    // 索引混合採用最多四個最大權重；選取順序與 D3DX 的選擇排序一致。
                    // 僅轉換輸出的權重，不改動原始 SkinInfo；寫入頂點時再正規化。
                    if(indexed)for(auto& vertex:influences){
                        const auto count=std::min<std::size_t>(4,vertex.size());
                        for(std::size_t n=0;n<count;++n){auto largest=n;for(std::size_t k=n+1;k<vertex.size();++k)if(vertex[k].second>vertex[largest].second)largest=k;std::swap(vertex[n],vertex[largest]);}
                        vertex.resize(count);
                    }
                    std::vector<std::set<unsigned>> face_bones(mesh->height);
                    for(const auto& vertex:influences)maximum_vertex=std::max(maximum_vertex,static_cast<unsigned>(vertex.size()));
                    for(unsigned face=0;face<mesh->height;++face){for(unsigned corner=0;corner<3;++corner)for(const auto& [bone,weight]:influences[indices[face*3+corner]]){(void)weight;face_bones[face].insert(bone);}maximum_face=std::max(maximum_face,static_cast<unsigned>(face_bones[face].size()));}
                    const auto maximum=std::max(1U,indexed?maximum_vertex:maximum_face),palette=indexed?u(3):maximum;
                    if(std::getenv("X64ARM_TRACE_D3DX_MESH")){std::fprintf(stderr,"[d3dx9 skin palette] vertex_max=%u face_max=%u palette=%u bones=%zu\n",maximum_vertex,maximum_face,palette,info.bones.size());unsigned count=0;for(unsigned v=0;v<influences.size() && count<3;++v)if(influences[v].size()>4){++count;std::fprintf(stderr,"[d3dx9 skin weights] vertex=%u",v);for(const auto& [bone,weight]:influences[v])std::fprintf(stderr," %u:%g",bone,double(weight));std::fprintf(stderr,"\n");}}
                    require(maximum<=4 && palette && palette<=256 && maximum_face<=palette);
                    struct Group {unsigned material{};std::set<unsigned> bones;std::vector<unsigned> faces;unsigned first_vertex{},vertices{},first_face{};};
                    std::vector<Group> groups;
                    for(unsigned face=0;face<mesh->height;++face){
                        const auto material=word(mesh->pixels,face*4);auto selected=groups.end();
                        for(auto it=groups.begin();it!=groups.end();++it)if(it->material==material){auto combined=it->bones;combined.insert(face_bones[face].begin(),face_bones[face].end());if(combined.size()<=palette){selected=it;break;}}
                        if(selected==groups.end()){groups.push_back({material,{},{},0,0,0});selected=std::prev(groups.end());}selected->faces.push_back(face);selected->bones.insert(face_bones[face].begin(),face_bones[face].end());
                    }
                    std::vector<std::uint8_t> layout;unsigned stride=0;const auto add_element=[&](unsigned type,unsigned usage,unsigned usage_index=0){layout.insert(layout.end(),{0,0,static_cast<std::uint8_t>(stride),static_cast<std::uint8_t>(stride>>8),static_cast<std::uint8_t>(type),0,static_cast<std::uint8_t>(usage),static_cast<std::uint8_t>(usage_index)});stride+=element_bytes(type);};
                    add_element(2,0);const auto weights_offset=stride;if(maximum>1)add_element(maximum-2,1);const auto indices_offset=stride;if(indexed)add_element(5,2);
                    for(std::size_t e=0;e+8<mesh->presentation.size();e+=8){const auto usage=mesh->presentation[e+6];if(usage!=0 && usage!=1 && usage!=2)add_element(mesh->presentation[e+4],usage,mesh->presentation[e+7]);}
                    layout.insert(layout.end(),{255,0,0,0,17,0,0,0});
                    std::vector<unsigned> vertex_remap,face_remap,new_indices,attributes;std::vector<std::vector<unsigned>> palettes;std::vector<unsigned> vertex_group;
                    for(unsigned g=0;g<groups.size();++g){auto& group=groups[g];group.first_vertex=static_cast<unsigned>(vertex_remap.size());group.first_face=static_cast<unsigned>(face_remap.size());palettes.emplace_back(group.bones.begin(),group.bones.end());std::map<unsigned,unsigned> remap;
                        for(auto face:group.faces){face_remap.push_back(face);attributes.push_back(g);for(unsigned c=0;c<3;++c){const auto original=indices[face*3+c];auto [it,inserted]=remap.emplace(original,static_cast<unsigned>(vertex_remap.size()));if(inserted){vertex_remap.push_back(original);vertex_group.push_back(g);}new_indices.push_back(it->second);}}
                        group.vertices=static_cast<unsigned>(vertex_remap.size())-group.first_vertex;
                    }
                    auto converted=make_mesh(mesh->height,static_cast<unsigned>(vertex_remap.size()),u(2)|(vertex_remap.size()>65536?1U:0U),layout,mesh->parent);std::shared_ptr<Object> remap_blob,combinations;
                    try{
                        auto from=get(mesh->vertex_buffer,Kind::VertexBuffer),to=get(converted->vertex_buffer,Kind::VertexBuffer),ib=get(converted->index_buffer,Kind::IndexBuffer);require(!from->mapped);
                        for(unsigned v=0;v<vertex_remap.size();++v){const auto original=vertex_remap[v],g=vertex_group[v];const auto& bones=palettes[g];
                            for(std::size_t e=0;e+8<layout.size();e+=8){const auto usage=layout[e+6];if(usage==1 || usage==2)continue;const auto old=offset_of(*mesh,usage,layout[e+7]);if(old==UINT32_MAX)continue;require(mesh->presentation[old+4]==layout[e+4]);const auto src=std::size_t(original)*mesh->stream_stride+(unsigned(mesh->presentation[old+2])|unsigned(mesh->presentation[old+3])<<8);const auto dst=std::size_t(v)*stride+(unsigned(layout[e+2])|unsigned(layout[e+3])<<8);std::copy_n(from->pixels.data()+src,element_bytes(layout[e+4]),to->pixels.data()+dst);}
                            std::array<float,4> weights{};std::array<unsigned,4> bone_indices{};float total=0;
                            for(unsigned n=0;n<influences[original].size();++n){const auto [bone,weight]=influences[original][n];const auto where=std::find(bones.begin(),bones.end(),bone);require(where!=bones.end());const auto slot=static_cast<unsigned>(where-bones.begin());if(indexed){weights[n]=weight;bone_indices[n]=slot;}else weights[slot]=weight;total+=weight;}
                            require(total>0);for(unsigned n=0;n+1<maximum;++n)put(to->pixels,std::size_t(v)*stride+weights_offset+n*4,bits(weights[n]/total));if(indexed)for(unsigned n=0;n<4;++n)to->pixels[std::size_t(v)*stride+indices_offset+n]=static_cast<std::uint8_t>(bone_indices[n]);
                        }
                        for(unsigned n=0;n<new_indices.size();++n)if(converted->flags&1)put(ib->pixels,n*4,new_indices[n]);else{ib->pixels[n*2]=static_cast<std::uint8_t>(new_indices[n]);ib->pixels[n*2+1]=static_cast<std::uint8_t>(new_indices[n]>>8);}
                        for(unsigned n=0;n<attributes.size();++n)put(converted->pixels,n*4,attributes[n]);
                        if(arg(5+shift))memory.write(GuestAddress{arg(5+shift)},std::span(reinterpret_cast<const std::uint8_t*>(face_remap.data()),face_remap.size()*4));
                        if(arg(4+shift)){
                            require(arg(3+shift));std::vector<unsigned> reverse(mesh->height),adjacency_result(new_indices.size(),UINT32_MAX);for(unsigned n=0;n<face_remap.size();++n)reverse[face_remap[n]]=n;
                            for(unsigned n=0;n<face_remap.size();++n)for(unsigned c=0;c<3;++c){const auto old=memory.load(GuestAddress{arg(3+shift)+(face_remap[n]*3ULL+c)*4},32);require(old==UINT32_MAX || old<mesh->height);if(old!=UINT32_MAX)adjacency_result[n*3+c]=reverse[old];}memory.write(GuestAddress{arg(4+shift)},std::span(reinterpret_cast<const std::uint8_t*>(adjacency_result.data()),adjacency_result.size()*4));
                        }
                        if(arg(6+shift))remap_blob=blob(std::span(reinterpret_cast<const std::uint8_t*>(vertex_remap.data()),vertex_remap.size()*4));
                        if(arg(9+shift)){
                            const auto header=x86_?24U:32U;std::vector<std::uint8_t> data(groups.size()*(header+palette*4));for(unsigned g=0;g<groups.size();++g){const auto& group=groups[g];const unsigned base=g*header;for(auto [offset,value]:{std::pair{0U,group.material},{4U,group.first_face},{8U,static_cast<unsigned>(group.faces.size())},{12U,group.first_vertex},{16U,group.vertices}})put(data,base+offset,value);for(unsigned b=0;b<palette;++b)put(data,groups.size()*header+(std::size_t(g)*palette+b)*4,b<palettes[g].size()?palettes[g][b]:UINT32_MAX);}
                            combinations=blob(data);combinations->mapped=allocate_heap(memory,data.size());require(combinations->mapped,out_of_memory);
                            for(unsigned g=0;g<groups.size();++g){const auto pointer=combinations->mapped+groups.size()*header+g*palette*4ULL;std::memcpy(data.data()+g*header+(x86_?20:24),&pointer,ps);}combinations->pixels=data;memory.write(GuestAddress{combinations->mapped},data);
                        }
                        if(arg(7+shift))memory.store(GuestAddress{arg(7+shift)},32,maximum);if(arg(8+shift))memory.store(GuestAddress{arg(8+shift)},32,groups.size());
                        if(remap_blob)store_pointer(arg(6+shift),remap_blob->root);if(combinations)store_pointer(arg(9+shift),combinations->root);store_pointer(arg(10+shift),converted->root);
                    }catch(...){if(remap_blob)release(remap_blob->root);if(combinations)release(combinations->root);release(converted->root);throw;}
                }
