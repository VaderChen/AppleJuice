// 同步載入與 Thread Pump 共用來源資訊、紋理設定及 Metal 提交。
        const auto emit_d3dx_image_info=[&](const texture_image::Decoded& decoded,std::uint64_t address){
            const auto& d=decoded.desc;
            const std::array fields{d.width,d.height,d.depth,d.layers,d.levels,d.misc,d.format,d.dimension,decoded.file_format};
            memory.check(GuestAddress{address},fields.size()*sizeof(std::uint32_t),Access::Write);
            for(unsigned i=0;i<fields.size();++i)memory.store(GuestAddress{address+4*i},32,fields[i]);
        };

        const auto prepare_d3dx_texture=[&](texture_image::Decoded& decoded,std::span<const std::uint8_t> load,bool view){
            D3dxPreparedTexture prepared{decoded.desc,0,decoded.desc.levels,0,0};
            auto& [d,first,source_levels,usage,access]=prepared;
            auto levels=source_levels;
            if(!load.empty()){
                const auto option=[&](unsigned at,std::uint32_t fallback){
                    const auto value=word(load,at);
                    const bool from_file=value==UINT32_MAX-2 && (at==0 || at==4 || at==8 || at==16 || at==36);
                    return value==UINT32_MAX || from_file?fallback:value;
                };
                first=option(12,0);
                require(first<source_levels);
                levels=option(16,source_levels-first);
                if(!levels)levels=source_levels-first;
                require(levels<=source_levels-first);
                const auto width=std::max(1U,d.width>>first),height=std::max(1U,d.height>>first),depth=std::max(1U,d.depth>>first);
                require(option(0,width)==width && option(4,height)==height && (d.dimension!=4 || option(8,depth)==depth));
                usage=option(20,0);
                d.bind=option(24,8);
                access=option(28,0);
                d.misc=option(32,d.misc);
                const auto target=option(36,d.format);
                const auto rgba=[](std::uint32_t f){return f>=27 && f<=29;};
                const auto bgra=[](std::uint32_t f){return f==87 || f==90 || f==91;};
                const auto bc=graphics_format_bc_family(target);
                if(target!=d.format && !(rgba(target) && rgba(d.format)) && !(bgra(target) && bgra(d.format)) &&
                    !(bc && bc==graphics_format_bc_family(d.format))){
                    try{texture_image::convert_float(decoded,target);}
                    catch(const texture_image::Error&){texture_image::convert_format(decoded,target,word(load,40));}
                }
                d.format=target;
                std::uint64_t src_info=0;
                std::memcpy(&src_info,load.data()+48,pointer_size);
                if(src_info)emit_d3dx_image_info(decoded,src_info);
            }
            if(std::getenv("X64ARM_TRACE_GRAPHICS_STATE"))std::fprintf(stderr,"[d3dx11_texture] format=%u usage=%u bind=0x%x cpu=0x%x size=%ux%ux%u levels=%u\n",d.format,usage,d.bind,access,d.width,d.height,d.depth,levels);
            require(usage<=3 && !(access&~0x30000U) && !(usage==0 && access) && !(usage==1 && access) &&
                !(usage==2 && access!=0x10000) && !(usage==3 && (d.bind || !access)));
            require(!view || (d.bind&8));
            d.width=std::max(1U,d.width>>first);
            d.height=std::max(1U,d.height>>first);
            d.depth=std::max(1U,d.depth>>first);
            d.levels=levels;
            return prepared;
        };

        const auto create_d3dx_texture=[&](const std::shared_ptr<Identity>& device,const texture_image::Decoded& decoded,
            const D3dxPreparedTexture& prepared,bool view){
            const auto& [d,first,source_levels,usage,access]=prepared;
            const auto resource=create(d.dimension==2?Kind::Texture1D:d.dimension==4?Kind::Texture3D:Kind::Texture,device->root);
            std::shared_ptr<Identity> srv;
            try{
                resource->resource=device->native->graphics_resource(d);
                resource->desc.resize(d.dimension==2?32:d.dimension==4?36:44);
                auto& desc=resource->desc;
                put_word(desc,0,d.width);
                if(d.dimension==2){
                    const std::array fields{d.levels,d.layers,d.format,usage,d.bind,access,d.misc};
                    for(unsigned i=0;i<fields.size();++i)put_word(desc,(i+1)*4,fields[i]);
                }else if(d.dimension==4){
                    const std::array fields{d.height,d.depth,d.levels,d.format,usage,d.bind,access,d.misc};
                    for(unsigned i=0;i<fields.size();++i)put_word(desc,(i+1)*4,fields[i]);
                }else{
                    const std::array fields{d.width,d.height,d.levels,d.layers,d.format,1U,0U,usage,d.bind,access,d.misc};
                    for(unsigned i=0;i<fields.size();++i){
                        put_word(desc,i*4,fields[i]);
                        resource->texture_desc[i]=fields[i];
                    }
                }
                for(unsigned layer=0;layer<d.layers;++layer)for(unsigned level=0;level<d.levels;++level){
                    const auto pitch=graphics_format_pitch(d.format,std::max(1U,d.width>>level),std::max(1U,d.height>>level));
                    const auto& pixels=decoded.pixels.at(layer*source_levels+first+level);
                    device->native->graphics_write(subresource(resource,layer*d.levels+level),pixels,pitch[0],pitch[0]*pitch[1]);
                }
                if(view){
                    srv=create(Kind::ShaderResource,device->root);
                    hold(resource->root);
                    srv->texture=resource->root;
                    srv->resource=resource->resource;
                    srv->graphics_view={resource->resource,d.format,0,d.levels,0,d.layers};
                    const auto dimension=d.dimension==2?(d.layers>1?3U:2U):d.dimension==4?8U:(d.misc&4)?(d.layers>6?10U:9U):d.layers>1?5U:4U;
                    srv->graphics_view.dimension=dimension;
                    srv->desc.resize(24);
                    put_word(srv->desc,0,d.format);
                    put_word(srv->desc,4,dimension);
                    put_word(srv->desc,12,d.levels);
                    if(d.layers>1)put_word(srv->desc,20,(d.misc&4)?d.layers/6:d.layers);
                }
            }catch(...){
                if(srv)release(srv->root);
                release(resource->root);
                throw;
            }
            if(!srv)return resource->root;
            release(resource->root);
            return srv->root;
        };
