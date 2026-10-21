        bool clean_handled=false;
        if(symbol=="D3DXValidMesh" || symbol=="D3DXCleanMesh"){
            clean_handled=true;mesh_handled=true;const bool cleaning=symbol=="D3DXCleanMesh";const unsigned shift=cleaning?1:0;if(cleaning){output(3);require(u(0)>0 && !(u(0)&~3U));}if(arg(cleaning?5:2))output(cleaning?5:2);
            auto mesh=get(arg(shift),Kind::Mesh);auto ib=get(mesh->index_buffer,Kind::IndexBuffer),vb=get(mesh->vertex_buffer,Kind::VertexBuffer);require(!ib->mapped && !vb->mapped && !mesh->mapped);
            std::vector<std::uint32_t> ix(std::size_t(mesh->height)*3),adj,attributes(mesh->height);
            for(std::size_t i=0;i<ix.size();++i)ix[i]=(mesh->flags&1)?word(ib->pixels,i*4):unsigned(ib->pixels[i*2])|(unsigned(ib->pixels[i*2+1])<<8);
            if(arg(shift+1)){adj.resize(ix.size());memory.read(GuestAddress{arg(shift+1)},std::span(reinterpret_cast<std::uint8_t*>(adj.data()),adj.size()*4));}
            std::string message;
            if(!cleaning){result=d3dx_mesh::validate(ix,mesh->width,adj,message);if(result==0x80004005U)result=0x88760b55U;}
            else{
                for(unsigned i=0;i<mesh->height;++i)attributes[i]=word(mesh->pixels,i*4);
                std::vector<std::uint32_t> duplicates;result=d3dx_mesh::clean(ix,mesh->width,adj,attributes,duplicates,(u(0)&2)!=0);
                if(!result){
                    std::vector<unsigned> faces,remap(mesh->height,UINT32_MAX);for(unsigned i=0;i<mesh->height;++i)if(ix[i*3]!=UINT32_MAX && ix[i*3+1]!=UINT32_MAX && ix[i*3+2]!=UINT32_MAX){remap[i]=static_cast<unsigned>(faces.size());faces.push_back(i);}
                    require(!faces.empty(),0x88760b55U);require(duplicates.size()<=16777216U-mesh->width,out_of_memory);
                    auto cleaned=make_mesh(static_cast<unsigned>(faces.size()),mesh->width+static_cast<unsigned>(duplicates.size()),mesh->flags,mesh->presentation,mesh->parent);cleaned->fvf=mesh->fvf;
                    try{
                        auto target_vb=get(cleaned->vertex_buffer,Kind::VertexBuffer),target_ib=get(cleaned->index_buffer,Kind::IndexBuffer);std::copy(vb->pixels.begin(),vb->pixels.end(),target_vb->pixels.begin());
                        for(std::size_t i=0;i<duplicates.size();++i){require(duplicates[i]<mesh->width);std::copy_n(vb->pixels.data()+std::size_t(duplicates[i])*mesh->stream_stride,mesh->stream_stride,target_vb->pixels.data()+(mesh->width+i)*mesh->stream_stride);}
                        for(unsigned i=0;i<faces.size();++i){put(cleaned->pixels,i*4,attributes[faces[i]]);for(unsigned j=0;j<3;++j){const auto index=ix[faces[i]*3+j];require(index<cleaned->width);if(cleaned->flags&1)put(target_ib->pixels,(i*3+j)*4,index);else{require(index<=65535);target_ib->pixels[(i*3+j)*2]=static_cast<std::uint8_t>(index);target_ib->pixels[(i*3+j)*2+1]=static_cast<std::uint8_t>(index>>8);}}}
                        if(arg(4)){std::vector<unsigned> neighbors;if(adj.empty())neighbors=adjacency(*cleaned,0);else{neighbors.resize(faces.size()*3,UINT32_MAX);for(unsigned i=0;i<faces.size();++i)for(unsigned j=0;j<3;++j){const auto neighbor=adj[faces[i]*3+j];require(neighbor==UINT32_MAX || neighbor<mesh->height);if(neighbor!=UINT32_MAX)neighbors[i*3+j]=remap[neighbor];}}memory.write(GuestAddress{arg(4)},std::span(reinterpret_cast<const std::uint8_t*>(neighbors.data()),neighbors.size()*4));}
                        store_pointer(arg(3),cleaned->root);
                    }catch(...){release(cleaned->root);throw;}
                }
            }
            if(arg(cleaning?5:2) && !message.empty()){message.push_back(0);store_pointer(arg(cleaning?5:2),blob(std::span(reinterpret_cast<const std::uint8_t*>(message.data()),message.size()))->root);}
        }
