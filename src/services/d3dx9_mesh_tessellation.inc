        bool tessellation_handled=symbol=="D3DXTessellateNPatches";
        if(tessellation_handled){
            output(4);if(arg(5))output(5);auto source=get(arg(0),Kind::Mesh);
            const auto segments=scalar_arg(2);require(std::isfinite(segments) && segments>=1 && segments<=256);
            const unsigned count=static_cast<unsigned>(std::ceil(segments));
            const std::uint64_t per_face=std::uint64_t(count+1)*(count+2)/2;
            const auto vertices=per_face*source->height,faces=std::uint64_t(count)*count*source->height;
            require(vertices<=16777216 && faces<=16777216,out_of_memory);
            auto input=get(source->vertex_buffer,Kind::VertexBuffer);require(!input->mapped && !source->mapped);
            const auto ix=mesh_indices(*source);const auto p=positions(*source),normals=positions(*source,3);
            // PN 三角形採三次 Bézier 位置、線性或二次法線；其他頂點屬性線性內插。
            // 保留材質分組與接縫，輸出鄰接由實際細分後幾何產生。
            for(std::size_t e=0;e+8<source->presentation.size();e+=8){const auto type=source->presentation[e+4];require(type<=5,unavailable);}
            if(arg(1))for(unsigned i=0;i<ix.size();++i){const auto neighbor=memory.load(GuestAddress{arg(1)+i*4},32);require(neighbor==UINT32_MAX || neighbor<source->height);}
            auto mesh=make_mesh(static_cast<unsigned>(faces),static_cast<unsigned>(vertices),(source->flags&~0x1000U)|(vertices>65536?1U:0U),source->presentation,source->parent);mesh->fvf=source->fvf;
            try{
                auto vb=get(mesh->vertex_buffer,Kind::VertexBuffer),ib=get(mesh->index_buffer,Kind::IndexBuffer);
                unsigned next_face=0;
                const auto emit=[&](unsigned a,unsigned b,unsigned c,unsigned attribute){
                    put(mesh->pixels,next_face*4,attribute);const std::array indices{a,b,c};
                    for(unsigned k=0;k<3;++k){const auto at=(std::size_t(next_face)*3+k)*((mesh->flags&1)?4:2);if(mesh->flags&1)put(ib->pixels,at,indices[k]);else{ib->pixels[at]=static_cast<std::uint8_t>(indices[k]);ib->pixels[at+1]=static_cast<std::uint8_t>(indices[k]>>8);}}++next_face;
                };
                for(unsigned face=0;face<source->height;++face){
                    const std::array ids{ix[face*3],ix[face*3+1],ix[face*3+2]};
                    std::array<Vector,3> points{p[ids[0]],p[ids[1]],p[ids[2]]},n{normalize(normals[ids[0]]),normalize(normals[ids[1]]),normalize(normals[ids[2]])};
                    Vector edge[3][3]{},middle{},normal_edge[3][3]{};
                    for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b)if(a!=b){
                        const auto delta=subtract(points[b],points[a]);const auto projection=dot3(delta,n[a]);
                        for(unsigned c=0;c<3;++c){edge[a][b][c]=(2*points[a][c]+points[b][c]-projection*n[a][c])/3;middle[c]+=edge[a][b][c]/6;}
                        const auto length=dot3(delta,delta);Vector sum{};for(unsigned c=0;c<3;++c)sum[c]=n[a][c]+n[b][c];
                        const auto correction=length>0?2*dot3(delta,sum)/length:0;
                        for(unsigned c=0;c<3;++c)normal_edge[a][b][c]=sum[c]-correction*delta[c];
                        normal_edge[a][b]=normalize(normal_edge[a][b]);
                    }
                    for(unsigned c=0;c<3;++c)middle[c]=1.5F*middle[c]-(points[0][c]+points[1][c]+points[2][c])/6;
                    const auto vertex=[&](unsigned row,unsigned column){return static_cast<unsigned>(face*per_face+std::uint64_t(row)*(count+1)-std::uint64_t(row)*(row?row-1:0)/2+column);};
                    for(unsigned row=0;row<=count;++row)for(unsigned column=0;column<=count-row;++column){
                        const std::array weights{float(count-row-column)/count,float(column)/count,float(row)/count};
                        const auto destination=std::size_t(vertex(row,column))*mesh->stream_stride;
                        for(std::size_t e=0;e+8<source->presentation.size();e+=8){
                            const auto type=source->presentation[e+4],usage=source->presentation[e+6],index=source->presentation[e+7];
                            const unsigned offset=source->presentation[e+2]|(unsigned(source->presentation[e+3])<<8);
                            if(type<=3){
                                Vector value{};
                                if(usage==0 && !index){
                                    for(unsigned c=0;c<3;++c){value[c]=6*weights[0]*weights[1]*weights[2]*middle[c];
                                        for(unsigned a=0;a<3;++a){value[c]+=weights[a]*weights[a]*weights[a]*points[a][c];for(unsigned b=0;b<3;++b)if(a!=b)value[c]+=3*weights[a]*weights[a]*weights[b]*edge[a][b][c];}}
                                    value[3]=1;
                                }else if(usage==3 && !index){
                                    for(unsigned c=0;c<3;++c)for(unsigned a=0;a<3;++a){value[c]+=(u(3)?weights[a]*weights[a]:weights[a])*n[a][c];if(u(3))for(unsigned b=a+1;b<3;++b)value[c]+=2*weights[a]*weights[b]*normal_edge[a][b][c];}
                                    value=normalize(value);
                                }else for(unsigned c=0;c<=type;++c)for(unsigned a=0;a<3;++a)value[c]+=weights[a]*real(word(input->pixels,std::size_t(ids[a])*source->stream_stride+offset+c*4));
                                for(unsigned c=0;c<=type;++c)put(vb->pixels,destination+offset+c*4,bits(value[c]));
                            }else for(unsigned c=0;c<4;++c){float value=0;for(unsigned a=0;a<3;++a)value+=weights[a]*input->pixels[std::size_t(ids[a])*source->stream_stride+offset+c];vb->pixels[destination+offset+c]=static_cast<std::uint8_t>(std::clamp(std::lround(value),0L,255L));}
                        }
                    }
                    const auto attribute=word(source->pixels,face*4);
                    for(unsigned row=0;row<count;++row)for(unsigned column=0;column<count-row;++column){
                        emit(vertex(row,column),vertex(row,column+1),vertex(row+1,column),attribute);
                        if(column+1<count-row)emit(vertex(row,column+1),vertex(row+1,column+1),vertex(row+1,column),attribute);
                    }
                }
                require(next_face==mesh->height);
                if(arg(5)){const auto adj=adjacency(*mesh,0);store_pointer(arg(5),blob(std::span(reinterpret_cast<const std::uint8_t*>(adj.data()),adj.size()*4))->root);}
                store_pointer(arg(4),mesh->root);
            }catch(...){release(mesh->root);throw;}
        }
