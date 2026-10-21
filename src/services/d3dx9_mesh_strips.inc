        bool strips_handled=symbol=="D3DXConvertMeshSubsetToSingleStrip" || symbol=="D3DXConvertMeshSubsetToStrips";
        if(strips_handled){
            const bool single=symbol=="D3DXConvertMeshSubsetToSingleStrip";output(3);if(!single)output(5);
            if(arg(4))memory.store(GuestAddress{arg(4)},32,0);if(!single && arg(6))memory.store(GuestAddress{arg(6)},32,0);
            auto mesh=get(arg(0),Kind::Mesh);require(!mesh->mapped && !(u(2)&~0xf00U) && (u(2)&0x300U)!=0x300U);
            const auto ix=mesh_indices(*mesh);std::vector<bool> used(mesh->height,true);
            std::map<std::pair<unsigned,unsigned>,std::vector<std::pair<unsigned,unsigned>>> edges;
            for(unsigned face=0;face<mesh->height;++face)if(word(mesh->pixels,face*4)==u(1)){
                used[face]=false;for(unsigned e=0;e<3;++e)edges[{ix[face*3+e],ix[face*3+(e+1)%3]}].push_back({face,ix[face*3+(e+2)%3]});
            }
            std::vector<std::vector<unsigned>> strips;
            for(unsigned face=0;face<mesh->height;++face)if(!used[face]){
                auto& strip=strips.emplace_back(ix.begin()+face*3,ix.begin()+face*3+3);used[face]=true;
                for(;;){const auto size=strip.size();unsigned a=strip[size-2],b=strip[size-1];if((size-2)&1)std::swap(a,b);
                    auto found=edges.find({a,b});if(found==edges.end())break;auto& candidates=found->second;
                    while(!candidates.empty() && used[candidates.back().first])candidates.pop_back();if(candidates.empty())break;
                    const auto [next,index]=candidates.back();candidates.pop_back();used[next]=true;strip.push_back(index);
                }
            }
            require(!strips.empty());std::vector<unsigned> indices,lengths;
            for(const auto& strip:strips){
                if(single && !indices.empty()){
                    // 以退化三角形串接，並維持新 strip 首面為偶數繞向。
                    const bool odd=indices.size()&1;indices.push_back(indices.back());indices.push_back(strip.front());if(odd)indices.push_back(strip.front());
                }
                indices.insert(indices.end(),strip.begin(),strip.end());lengths.push_back(static_cast<unsigned>(strip.size()-2));
            }
            require(indices.size()<=UINT32_MAX/4,out_of_memory);auto buffer=create(Kind::IndexBuffer,mesh->parent);std::uint64_t lengths_buffer=0;
            try{
                buffer->format=(mesh->flags&1)?102:101;buffer->pool=(u(2)&0x100)?2:(u(2)&0x200)?1:0;buffer->usage=((u(2)&0x400)?8:0)|((u(2)&0x800)?0x200:0);
                const unsigned bytes=(mesh->flags&1)?4:2;buffer->storage=reserve_gpu_memory(indices.size()*bytes);buffer->pixels.resize(indices.size()*bytes);
                for(std::size_t i=0;i<indices.size();++i)if(bytes==4)put(buffer->pixels,i*4,indices[i]);else{buffer->pixels[i*2]=static_cast<std::uint8_t>(indices[i]);buffer->pixels[i*2+1]=static_cast<std::uint8_t>(indices[i]>>8);}
                if(!single)lengths_buffer=blob(std::span(reinterpret_cast<const std::uint8_t*>(lengths.data()),lengths.size()*4))->root;
                if(arg(4))memory.store(GuestAddress{arg(4)},32,indices.size());if(!single){store_pointer(arg(5),lengths_buffer);if(arg(6))memory.store(GuestAddress{arg(6)},32,lengths.size());}
                store_pointer(arg(3),buffer->root);
            }catch(...){if(lengths_buffer)release(lengths_buffer);release(buffer->root);throw;}
        }
