        if(symbol=="D3DXCreatePRTCompBuffer"){
            prt_handled=true;output(6);auto source=get(arg(5),Kind::PRT);require(!source->mapped);
            require((u(0)==1 || u(0)==2) && u(1) && u(1)<=source->width && u(2)<=source->height*source->format);
            D3DXPRTCompOperation* operation=nullptr;
            for(auto& [id,item]:bridge.prt_comp_operations){(void)id;if(item.thread==threads_.current && item.stack==state.reg(Gpr::Rsp) && item.service.symbol==service.symbol){operation=&item;break;}}
            if(!operation){
                D3DXPRTCompOperation next;next.id=++bridge.next_atlas_operation;next.thread=threads_.current;next.stack=state.reg(Gpr::Rsp);next.service=service;next.callback=arg(3);next.context=arg(4);next.output=std::make_shared<d3dx_prt::Compressed>();
                std::vector<float> values(source->pixels.size()/4);std::memcpy(values.data(),source->pixels.data(),source->pixels.size());
                const auto samples=source->width,coefficients=source->height,channels=source->format,width=source->viewport[0],height=source->viewport[1],clusters=u(1),components=u(2),quality=u(0);auto produced=next.output;
                next.worker=std::make_unique<NativeProgressJob>(next.callback!=0,[values=std::move(values),produced,samples,coefficients,channels,width,height,clusters,components,quality](const NativeProgressJob::Progress& progress){const auto code=d3dx_prt::compress(values,samples,coefficients,channels,clusters,components,quality,progress,*produced);if(!code){produced->width=width;produced->height=height;}return code;});
                operation=&bridge.prt_comp_operations.emplace(next.id,std::move(next)).first->second;
            }
            float progress=0;if(operation->worker->wait(progress)){GuestCallback callback{operation->callback,{bits(progress),operation->context}};callback.floating_arguments=1;start_callbacks(state,memory,{std::move(callback)},Continuation::D3DXProgressOperation,operation->id);return std::nullopt;}
            result=operation->worker->result;if(!result){auto object=create(Kind::PRTComp);object->compressed_prt=operation->output;store_pointer(arg(6),object->root);}bridge.prt_comp_operations.erase(operation->id);
        }else if(symbol.starts_with("D3DXLoadPRTCompBufferFromFile") || symbol.starts_with("D3DXSavePRTCompBufferToFile")){
            prt_handled=true;const bool load=symbol.starts_with("D3DXLoad");if(load)output(1);require(arg(0));const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
            if(load){
                const auto file=filesystem_.open_reader(d3dx_path(path));require(!file.error,0x80070000U|file.error);const auto metadata=file.file->information();require(!metadata.error && metadata.metadata.size>=32 && metadata.metadata.size<=512U*1024U*1024U);
                std::vector<std::uint8_t> data(metadata.metadata.size);const auto io=file.file->read(data,0);require(!io.error && io.bytes==data.size());
                auto compressed=std::make_shared<d3dx_prt::Compressed>();auto& c=*compressed;c.samples=word(data,0);c.coefficients=word(data,4);c.channels=word(data,8);c.width=word(data,12);c.height=word(data,16);const auto texture=word(data,20);c.clusters=word(data,24);c.components=word(data,28);
                require(c.samples && c.coefficients && c.channels && c.channels<=3 && texture<=1 && c.clusters && c.clusters<=c.samples && std::uint64_t(c.components)<=std::uint64_t(c.coefficients)*c.channels);
                require(!texture || (c.width && c.height && std::uint64_t(c.width)*c.height==c.samples));if(!texture)c.width=c.height=0;
                const std::uint64_t dimension=std::uint64_t(c.coefficients)*c.channels;require(dimension<=4096 && c.components<=4096 && std::uint64_t(c.samples)*std::max(1U,c.components)<=128U*1024U*1024U);
                const std::uint64_t bases=std::uint64_t(c.clusters)*(c.components+1)*dimension,weights=std::uint64_t(c.samples)*c.components,ids=c.clusters>1?c.samples:0;
                require(32+(bases+weights+ids)*4==data.size());c.basis.resize(bases);c.weights.resize(weights);c.ids.resize(c.samples);
                std::memcpy(c.basis.data(),data.data()+32,bases*4);if(weights)std::memcpy(c.weights.data(),data.data()+32+bases*4,weights*4);if(ids)std::memcpy(c.ids.data(),data.data()+32+(bases+weights)*4,ids*4);
                for(auto id:c.ids)require(id<c.clusters);for(float value:c.basis)require(std::isfinite(value));for(float value:c.weights)require(std::isfinite(value));
                auto object=create(Kind::PRTComp);object->compressed_prt=std::move(compressed);store_pointer(arg(1),object->root);
            }else{
                const auto& c=*get(arg(1),Kind::PRTComp)->compressed_prt;const auto ids=c.clusters>1?c.ids.size():0;const auto size=32+(c.basis.size()+c.weights.size()+ids)*4;require(size<=512U*1024U*1024U);std::vector<std::uint8_t> data(size);
                const std::array<unsigned,8> header{c.samples,c.coefficients,c.channels,c.width,c.height,c.width?1U:0U,c.clusters,c.components};for(unsigned i=0;i<header.size();++i)put(data,i*4,header[i]);
                std::memcpy(data.data()+32,c.basis.data(),c.basis.size()*4);if(!c.weights.empty())std::memcpy(data.data()+32+c.basis.size()*4,c.weights.data(),c.weights.size()*4);if(ids)std::memcpy(data.data()+32+(c.basis.size()+c.weights.size())*4,c.ids.data(),ids*4);
                const auto file=filesystem_.open_host(d3dx_path(path),0x40000000U,2,0x80,1);require(!file.error,0x80070000U|file.error);const auto io=file.file->write(data,0);require(!io.error && io.bytes==data.size(),0x80070000U|(io.error?io.error:29U));
            }
        }else if(symbol.starts_with("PRTComp.")){
            prt_handled=true;auto object=get(arg(0),Kind::PRTComp);auto& c=*object->compressed_prt;const auto method=symbol.substr(8);
            if(method=="AddRef")result=hold(object);else if(method=="Release")result=release(object->root);
            else if(method=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);if(requested==unknown || requested==iid(Kind::PRTComp)){hold(object);store_pointer(arg(2),object->root);}else result=no_interface;}
            else if(method=="GetNumSamples")result=c.samples;else if(method=="GetNumCoeffs")result=c.coefficients;else if(method=="GetNumChannels")result=c.channels;
            else if(method=="IsTexture")result=c.width!=0;else if(method=="GetWidth")result=c.width;else if(method=="GetHeight")result=c.height;
            else if(method=="GetNumClusters")result=c.clusters;else if(method=="GetNumPCA")result=c.components;
            else if(method=="NormalizeData")c.normalize();
            else if(method=="ExtractBasis"){require(u(1)<c.clusters && arg(2));const auto size=std::size_t(c.components+1)*c.coefficients*c.channels;memory.write(GuestAddress{arg(2)},std::span(reinterpret_cast<const std::uint8_t*>(c.basis.data()+u(1)*size),size*4));}
            else if(method=="ExtractClusterIDs"){require(arg(1));memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(c.ids.data()),c.ids.size()*4));}
            else if(method=="ExtractPCA"){require(u(1)<=c.components && u(2)<=c.components-u(1) && (!u(2) || arg(3)));for(unsigned sample=0;sample<c.samples;++sample)if(u(2))memory.write(GuestAddress{arg(3)+std::uint64_t(sample)*u(2)*4},std::span(reinterpret_cast<const std::uint8_t*>(c.weights.data()+std::size_t(sample)*c.components+u(1)),u(2)*4));}
            else if(method=="ExtractTexture"){
                require(c.width && u(1)<=c.components && u(2) && u(2)<=4 && u(2)<=c.components-u(1));auto texture=get(arg(3),Kind::Texture);require(texture->width==c.width && texture->height==c.height);auto surface=get(texture->subresources.at(0),Kind::Surface);require(!surface->mapped);
                std::vector<std::uint8_t> rgba(std::size_t(c.samples)*16);for(unsigned sample=0;sample<c.samples;++sample)for(unsigned component=0;component<u(2);++component)put(rgba,std::size_t(sample)*16+component*4,bits(c.weights[std::size_t(sample)*c.components+u(1)+component]));
                surface->pixels=texture_image::transform(rgba,2,{c.width,c.height,1},{0,0,0,c.width,c.height,1},native_format(surface->format),{c.width,c.height,1},1);write_surface(*surface);
            }else if(method=="ExtractToMesh"){
                require(!c.width && u(1)<=c.components && u(2)<=13 && u(3)<=15);auto mesh=get(arg(4),Kind::Mesh);require(mesh->width==c.samples);auto vertices=get(mesh->vertex_buffer,Kind::VertexBuffer);require(!vertices->mapped);
                struct Destination{unsigned offset,count;};std::vector<Destination> destinations;unsigned remaining=u(1),usage_index=u(3);
                while(remaining){const auto element=offset_of(*mesh,u(2),usage_index++);require(element!=UINT32_MAX && mesh->presentation[element+4]<=3);const auto count=std::min(remaining,unsigned(mesh->presentation[element+4])+1);destinations.push_back({unsigned(mesh->presentation[element+2])|unsigned(mesh->presentation[element+3])<<8,count});remaining-=count;}
                for(unsigned sample=0;sample<c.samples;++sample){unsigned coefficient=0;for(const auto& destination:destinations)for(unsigned k=0;k<destination.count;++k)put(vertices->pixels,std::size_t(sample)*mesh->stream_stride+destination.offset+k*4,bits(c.weights[std::size_t(sample)*c.components+coefficient++]));}
            }else throw HostCallFault("尚未實作 PRTCompBuffer 方法："+std::string(method));
        }
