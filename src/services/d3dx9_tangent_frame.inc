            struct Semantic { unsigned usage, index; };
            struct TangentParameters {
                Semantic uv, du, dv, normal;
                unsigned flags;
                std::uint64_t adjacency_in;
                float partial_limit, singular_limit, normal_limit;
                std::uint64_t mesh_out, mapping_out;
            };
            // 舊入口依 D3DX 契約將 U 偏導數存入 BINORMAL、V 偏導數存入 TANGENT。
            const auto params=[&]() -> TangentParameters {
                constexpr unsigned texcoord=5, binormal=7, tangent=6, normal=3, in_place=0x400;
                if(symbol=="D3DXComputeTangent"){
                    const auto stage=u(1), tangent_index=u(2), binormal_index=u(3);
                    require(stage!=UINT32_MAX && (tangent_index!=UINT32_MAX || binormal_index!=UINT32_MAX));
                    return {{texcoord,stage},{binormal_index==UINT32_MAX?UINT32_MAX:binormal,binormal_index},
                            {tangent_index==UINT32_MAX?UINT32_MAX:tangent,tangent_index},{UINT32_MAX,0},
                            in_place|0x20U|(u(4)?3U:0U),arg(5),-1.01F,-0.01F,-1.01F,0,0};
                }
                if(symbol=="D3DXComputeTangentFrame")
                    return {{texcoord,0},{binormal,0},{tangent,0},{normal,0},u(1)|in_place,0,0.01F,0.25F,0.01F,0,0};
                return {{u(1),u(2)},{u(3),u(4)},{u(5),u(6)},{u(7),u(8)},u(9),arg(10),
                        scalar_arg(11),scalar_arg(12),scalar_arg(13),arg(14),arg(15)};
            }();
            const auto options=params.flags;require(!(options&~0x7ffU));
            const bool inplace=(options&0x400)!=0,calculate=(options&0x200)!=0;
            require(inplace || params.mesh_out);
            if(params.mesh_out)store_pointer(params.mesh_out,0);
            if(params.mapping_out)store_pointer(params.mapping_out,0);
            const auto partial_limit=params.partial_limit,singular_limit=params.singular_limit,normal_limit=params.normal_limit;
            require(std::isfinite(partial_limit) && std::isfinite(singular_limit) && std::isfinite(normal_limit));
            auto source=get(arg(0),Kind::Mesh);require(!source->mapped);
            const auto points=positions(*source);const auto indices=mesh_indices(*source);
            const bool textured=params.uv.usage!=UINT32_MAX;require(textured || calculate);
            const auto texcoords=textured?positions(*source,params.uv.usage,params.uv.index,2):std::vector<Vector>{};
            const auto input_normals=calculate?std::vector<Vector>{}:positions(*source,params.normal.usage==UINT32_MAX?3:params.normal.usage,params.normal.index);
            for(auto semantic:{params.du,params.dv,params.normal})if(semantic.usage!=UINT32_MAX){const auto e=offset_of(*source,semantic.usage,semantic.index);require(e!=UINT32_MAX && source->presentation[e+4]>=2 && source->presentation[e+4]<=3);}
            std::vector<Vector> face_u(source->height),face_v(source->height),face_n(source->height);
            std::vector<float> weights(indices.size());std::vector<std::vector<unsigned>> corners(source->width);
            for(unsigned face=0;face<source->height;++face){
                const auto a=indices[face*3],b=indices[face*3+1],c=indices[face*3+2];
                const auto e1=subtract(points[b],points[a]),e2=subtract(points[c],points[a]);const auto cross=cross3(e1,e2);
                auto& n=face_n[face];n=normalize(cross);if(options&0x100)for(unsigned k=0;k<3;++k)n[k]=-n[k];
                if(textured){float du1=texcoords[b][0]-texcoords[a][0],dv1=texcoords[b][1]-texcoords[a][1],du2=texcoords[c][0]-texcoords[a][0],dv2=texcoords[c][1]-texcoords[a][1];
                    if(options&1){du1=std::remainder(du1,1.F);du2=std::remainder(du2,1.F);}if(options&2){dv1=std::remainder(dv1,1.F);dv2=std::remainder(dv2,1.F);}
                    const auto determinant=du1*dv2-du2*dv1;if(std::abs(determinant)>1e-20F)for(unsigned k=0;k<3;++k){face_u[face][k]=(e1[k]*dv2-e2[k]*dv1)/determinant;face_v[face][k]=(e2[k]*du1-e1[k]*du2)/determinant;}
                    if(!(options&4)){face_u[face]=normalize(face_u[face]);face_v[face]=normalize(face_v[face]);}}
                for(unsigned c0=0;c0<3;++c0){const auto at=face*3+c0,v=indices[at];corners[v].push_back(at);
                    weights[at]=(options&0x80)?1.F:(options&0x40)?std::sqrt(dot3(cross,cross)):std::acos(std::clamp(dot3(normalize(subtract(points[indices[face*3+(c0+1)%3]],points[v])),normalize(subtract(points[indices[face*3+(c0+2)%3]],points[v]))),-1.F,1.F));}
            }
            std::vector<unsigned> neighbours;if(params.adjacency_in){const auto data=read(params.adjacency_in,indices.size()*4);neighbours.resize(indices.size());for(unsigned i=0;i<neighbours.size();++i){neighbours[i]=word(data,i*4);require(neighbours[i]==UINT32_MAX || neighbours[i]<source->height);}}
            else neighbours=adjacency(*source,0);
            auto remapped=indices;std::vector<unsigned> mapping(source->width);for(unsigned i=0;i<mapping.size();++i)mapping[i]=i;
            // 有明確鄰接時依其連通分量分割；否則共用頂點的面依切線相容性分群。
            // 位置焊接的單一邊配對在重疊／非流形表面可能漏掉合法的共用頂點，不能據此額外拆點。
            for(unsigned vertex=0;vertex<corners.size();++vertex){const auto& incident=corners[vertex];if(incident.empty())continue;
                std::map<unsigned,unsigned> assigned;unsigned groups=0;
                for(auto seed:incident){if(assigned.contains(seed))continue;std::vector<unsigned> pending{seed},group;assigned[seed]=groups;
                    while(!pending.empty()){const auto at=pending.back();pending.pop_back();group.push_back(at);const auto face=at/3;
                        for(auto other:incident){if(assigned.contains(other))continue;const auto adjacent=other/3;
                            if(params.adjacency_in && std::find(neighbours.begin()+face*3,neighbours.begin()+face*3+3,adjacent)==neighbours.begin()+face*3+3)continue;
                            if(dot3(face_n[face],face_n[adjacent])<normal_limit)continue;
                            if(textured && (dot3(normalize(face_u[face]),normalize(face_u[adjacent]))<=partial_limit || dot3(normalize(face_v[face]),normalize(face_v[adjacent]))<=partial_limit))continue;
                            assigned[other]=groups;pending.push_back(other);}}
                    Vector sum_u{},sum_v{};float weight_sum=0;for(auto at:group){weight_sum+=weights[at];for(unsigned k=0;k<3;++k){sum_u[k]+=face_u[at/3][k]*weights[at];sum_v[k]+=face_v[at/3][k]*weights[at];}}
                    const bool singular=textured && group.size()>1 && weight_sum>0 && (std::sqrt(dot3(sum_u,sum_u))/weight_sum<=singular_limit || std::sqrt(dot3(sum_v,sum_v))/weight_sum<=singular_limit);
                    unsigned target=vertex;for(unsigned j=0;j<group.size();++j){if(j==0 || singular){if(groups++){target=static_cast<unsigned>(mapping.size());mapping.push_back(vertex);}else target=vertex;}remapped[group[j]]=target;}
                }
            }
            if(std::getenv("X64ARM_TRACE_D3DX_MESH")){std::fprintf(stderr,"[d3dx9 tangent] input=%u output=%zu faces=%u thresholds=%g,%g,%g adjacency=%d\n",source->width,mapping.size(),source->height,double(partial_limit),double(singular_limit),double(normal_limit),params.adjacency_in!=0);unsigned printed=0;for(std::size_t v=source->width;v<mapping.size() && printed<8;++v,++printed){const auto original=mapping[v];std::fprintf(stderr,"[d3dx9 tangent split] vertex=%u incident=%zu",original,corners[original].size());for(auto c:corners[original])std::fprintf(stderr," %u:%u",c/3,remapped[c]);if(corners[original].size()>=2){const auto a=corners[original][0]/3,b=corners[original][1]/3;std::fprintf(stderr," adjacent=%d normal=%g u=%g v=%g lengths=%g,%g,%g,%g",std::find(neighbours.begin()+a*3,neighbours.begin()+a*3+3,b)!=neighbours.begin()+a*3+3,double(dot3(face_n[a],face_n[b])),double(dot3(normalize(face_u[a]),normalize(face_u[b]))),double(dot3(normalize(face_v[a]),normalize(face_v[b]))),double(dot3(face_u[a],face_u[a])),double(dot3(face_u[b],face_u[b])),double(dot3(face_v[a],face_v[a])),double(dot3(face_v[b],face_v[b])));}std::fprintf(stderr,"\n");}}
            require(!inplace || mapping.size()==source->width);
            std::vector<Vector> normals(mapping.size()),tangents(mapping.size()),binormals(mapping.size());
            for(unsigned at=0;at<indices.size();++at){const auto v=remapped[at];for(unsigned k=0;k<3;++k){normals[v][k]+=face_n[at/3][k]*weights[at];tangents[v][k]+=face_u[at/3][k]*weights[at];binormals[v][k]+=face_v[at/3][k]*weights[at];}}
            for(unsigned i=0;i<mapping.size();++i){auto& n=normals[i];n=normalize(calculate?n:input_normals[mapping[i]]);auto& t=tangents[i];auto& b=binormals[i];
                if(options&8){if(!(options&4)){t=normalize(t);b=normalize(b);}continue;}
                const auto sign=dot3(cross3(n,t),b)<0?-1.F:1.F;
                const auto project=[&](Vector v){const auto d=dot3(n,v);for(unsigned k=0;k<3;++k)v[k]-=n[k]*d;return normalize(v);};
                if(options&0x10){b=project(b);t=cross3(b,n);for(unsigned k=0;k<3;++k)t[k]*=sign;}
                else{t=project(t);if(!(options&0x20)){auto from_v=cross3(project(b),n);for(unsigned k=0;k<3;++k)t[k]+=from_v[k]*sign;t=normalize(t);}b=cross3(n,t);for(unsigned k=0;k<3;++k)b[k]*=sign;}
            }
            auto mesh=source;std::uint64_t mapping_blob=0;
            try{
                if(!inplace){mesh=make_mesh(source->height,static_cast<unsigned>(mapping.size()),source->flags|(mapping.size()>65536?1U:0U),source->presentation,source->parent);
                    auto from=get(source->vertex_buffer,Kind::VertexBuffer),to=get(mesh->vertex_buffer,Kind::VertexBuffer);for(unsigned i=0;i<mapping.size();++i)std::copy_n(from->pixels.data()+std::size_t(mapping[i])*source->stream_stride,source->stream_stride,to->pixels.data()+std::size_t(i)*mesh->stream_stride);
                    auto ib=get(mesh->index_buffer,Kind::IndexBuffer);const auto size=(mesh->flags&1)?4U:2U;for(unsigned i=0;i<remapped.size();++i)std::memcpy(ib->pixels.data()+std::size_t(i)*size,&remapped[i],size);mesh->pixels=source->pixels;mesh->attributes=source->attributes;}
                if(params.mapping_out)mapping_blob=blob(std::span(reinterpret_cast<const std::uint8_t*>(mapping.data()),mapping.size()*4))->root;
                if(params.normal.usage!=UINT32_MAX && calculate)write_vectors(*mesh,params.normal.usage,params.normal.index,normals);
                if(params.du.usage!=UINT32_MAX)write_vectors(*mesh,params.du.usage,params.du.index,tangents);if(params.dv.usage!=UINT32_MAX)write_vectors(*mesh,params.dv.usage,params.dv.index,binormals);
                if(params.mesh_out){if(inplace)hold(mesh);store_pointer(params.mesh_out,mesh->root);}if(params.mapping_out)store_pointer(params.mapping_out,mapping_blob);
            }catch(...){if(mapping_blob)release(mapping_blob);if(!inplace && mesh!=source)release(mesh->root);throw;}
