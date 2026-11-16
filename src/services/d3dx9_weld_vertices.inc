            // D3DXWeldVertices(pMesh, Flags, pEpsilons, pAdjacencyIn, pAdjacencyOut, pFaceRemap, ppVertexRemap)：
            // 先以鄰接（未提供時依位置誤差產生）求同位置點代表，再逐一宣告元素依 D3DXWELDEPSILONS 與代表點比對。
            // WELDALL 不比對元素直接焊接；WELDPARTIALMATCHES 將誤差內的元素改為代表點的值；DONOTREMOVEVERTICES 只修改不移除。
            // 焊接不重排面，面重對應為恆等，輸出鄰接沿用求點代表所用的鄰接。
            if(arg(6))store_pointer(arg(6),0);
            auto mesh=get(arg(0),Kind::Mesh);const unsigned flags=u(1);require(!(flags&~0xfU));
            const bool weld_all=flags&1,partial=flags&2,keep=flags&4;
            auto vb=get(mesh->vertex_buffer,Kind::VertexBuffer),ib=get(mesh->index_buffer,Kind::IndexBuffer);require(!mesh->mapped && !vb->mapped && !ib->mapped);
            std::array<float,17> epsilon{};epsilon.fill(1.0e-6F);
            if(arg(2)){const auto bytes=read(arg(2),68);for(unsigned i=0;i<17;++i){epsilon[i]=real(word(bytes,i*4));require(std::isfinite(epsilon[i]) && epsilon[i]>=0);}}
            const auto ix=mesh_indices(*mesh);std::vector<unsigned> adj(ix.size());
            if(arg(3)){const auto bytes=read(arg(3),std::uint64_t(ix.size())*4);for(unsigned i=0;i<ix.size();++i)adj[i]=word(bytes,i*4);}else adj=adjacency(*mesh,epsilon[0]);
            const auto reps=point_reps(*mesh,ix,adj);
            // D3DXWELDEPSILONS：Position、BlendWeights、Normal、PSize、Specular、Diffuse、Texcoord[8]、Tangent、Binormal、TessFactor。
            const auto tolerance=[&](unsigned usage,unsigned index)->float{
                switch(usage){case 0:case 9:return epsilon[0];case 1:return epsilon[1];case 3:return epsilon[2];case 4:return epsilon[3];case 5:return index<8?epsilon[6+index]:0.F;
                    case 6:return epsilon[14];case 7:return epsilon[15];case 8:return epsilon[16];case 10:return index==0?epsilon[5]:index==1?epsilon[4]:0.F;default:return 0.F;}
            };
            const auto half=[](unsigned h){const unsigned e=(h>>10)&31,m=h&1023;const double v=e==0?std::ldexp(double(m),-24):e==31?(m?std::numeric_limits<double>::quiet_NaN():std::numeric_limits<double>::infinity()):std::ldexp(double(m|1024),int(e)-25);return (h&0x8000)?-v:v;};
            // 依 D3DDECLTYPE 解為數值；正規化型別換成 [0,1]／[-1,1]，與 epsilon 同尺度比較。
            const auto decode=[&](unsigned type,const std::uint8_t* at){
                std::array<double,4> v{};unsigned n=0;const auto u16=[&](unsigned i){return unsigned(at[i*2])|unsigned(at[i*2+1])<<8;};const auto s16=[&](unsigned i){return double(static_cast<std::int16_t>(u16(i)));};
                std::uint32_t packed=0;std::memcpy(&packed,at,4);
                switch(type){
                    case 0:case 1:case 2:case 3:n=type+1;for(unsigned c=0;c<n;++c){std::uint32_t bits_value;std::memcpy(&bits_value,at+c*4,4);v[c]=real(bits_value);}break;
                    case 4:case 8:n=4;for(unsigned c=0;c<4;++c)v[c]=at[c]/255.0;break;
                    case 5:n=4;for(unsigned c=0;c<4;++c)v[c]=at[c];break;
                    case 6:case 7:n=type==6?2:4;for(unsigned c=0;c<n;++c)v[c]=s16(c);break;
                    case 9:case 10:n=type==9?2:4;for(unsigned c=0;c<n;++c)v[c]=std::max(-1.0,s16(c)/32767.0);break;
                    case 11:case 12:n=type==11?2:4;for(unsigned c=0;c<n;++c)v[c]=u16(c)/65535.0;break;
                    case 13:n=3;for(unsigned c=0;c<3;++c)v[c]=(packed>>(c*10))&1023;break;
                    case 14:n=3;for(unsigned c=0;c<3;++c){int value=int((packed>>(c*10))&1023);if(value&512)value-=1024;v[c]=std::max(-1.0,value/511.0);}break;
                    case 15:case 16:n=type==15?2:4;for(unsigned c=0;c<n;++c)v[c]=half(u16(c));break;
                    default:require(false);
                }
                return std::pair{v,n};
            };
            const auto stride=mesh->stream_stride;auto data=vb->pixels;std::vector<unsigned> target(mesh->width);
            for(unsigned v=0;v<mesh->width;++v){
                target[v]=v;const auto r=reps[v];if(r==v)continue;
                auto* mine=data.data()+std::size_t(v)*stride;const auto* rep=data.data()+std::size_t(r)*stride;bool equal=true;
                std::vector<std::pair<unsigned,unsigned>> close;
                for(std::size_t e=0;e+8<mesh->presentation.size();e+=8){
                    const unsigned offset=unsigned(mesh->presentation[e+2])|unsigned(mesh->presentation[e+3])<<8,type=mesh->presentation[e+4],size=element_bytes(type);
                    if(std::memcmp(mine+offset,rep+offset,size)==0)continue;
                    bool within=weld_all;
                    if(!within){const auto [a,count]=decode(type,mine+offset);const auto [b,unused]=decode(type,rep+offset);(void)unused;const double limit=tolerance(mesh->presentation[e+6],mesh->presentation[e+7]);within=true;for(unsigned c=0;c<count;++c)if(!(std::abs(a[c]-b[c])<=limit))within=false;}
                    if(within)close.push_back({offset,size});else equal=false;
                }
                if(partial)for(const auto& [offset,size]:close)std::memcpy(mine+offset,rep+offset,size);
                if(equal && !keep)target[v]=r;
            }
            std::vector<unsigned> order,renumber(mesh->width,UINT32_MAX);for(unsigned v=0;v<mesh->width;++v)if(target[v]==v){renumber[v]=static_cast<unsigned>(order.size());order.push_back(v);}
            std::vector<std::uint8_t> vertices(order.size()*stride),indices(ib->pixels.size());
            for(unsigned i=0;i<order.size();++i)std::copy_n(data.data()+std::size_t(order[i])*stride,stride,vertices.data()+std::size_t(i)*stride);
            for(unsigned i=0;i<ix.size();++i){const auto value=renumber[target[ix[i]]];if(mesh->flags&1)put(indices,i*4,value);else{indices[i*2]=static_cast<std::uint8_t>(value);indices[i*2+1]=static_cast<std::uint8_t>(value>>8);}}
            const bool changed=order.size()!=mesh->width || vertices!=vb->pixels;
            // 共用頂點緩衝區的其他網格不得被改寫，與 OptimizeInplace 相同。
            if(changed)for(const auto& [pointer,other]:bridge.objects){(void)pointer;require(other->kind!=Kind::Mesh || other->root==mesh->root || other->vertex_buffer!=mesh->vertex_buffer);}
            std::vector<unsigned> faces(mesh->height);for(unsigned i=0;i<mesh->height;++i)faces[i]=i;
            std::shared_ptr<GpuMemoryReservation> storage;if(changed)storage=reserve_gpu_memory(vertices.size());
            std::shared_ptr<Object> remap;if(arg(6))remap=blob(std::span(reinterpret_cast<const std::uint8_t*>(order.data()),order.size()*4));
            try{
                if(arg(4))memory.write(GuestAddress{arg(4)},std::span(reinterpret_cast<const std::uint8_t*>(adj.data()),adj.size()*4));
                if(arg(5))memory.write(GuestAddress{arg(5)},std::span(reinterpret_cast<const std::uint8_t*>(faces.data()),faces.size()*4));
            }catch(...){if(remap)release(remap->root);throw;}
            if(remap)store_pointer(arg(6),remap->root);
            if(changed){if(order.size()!=mesh->width)mesh->attributes.clear();vb->storage=std::move(storage);vb->pixels=std::move(vertices);ib->pixels=std::move(indices);mesh->width=static_cast<unsigned>(order.size());}
