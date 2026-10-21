        if(symbol.starts_with("D3DX11GetImageInfoFrom") || symbol.starts_with("D3DX11CreateTextureFrom") || symbol.starts_with("D3DX11CreateShaderResourceViewFrom")){
            graphics_handled=true;
            const bool info=symbol.starts_with("D3DX11GetImageInfo"),view=symbol.starts_with("D3DX11CreateShaderResourceView"),file=symbol.find("FromFile")!=std::string_view::npos,res=symbol.find("FromResource")!=std::string_view::npos;
            const unsigned source_arg=info?0:1,source_count=file?1:2,load_arg=source_arg+source_count,pump_arg=load_arg+(info?0:1),out_arg=pump_arg+1,status_arg=out_arg+1;
            require(arg(out_arg)!=0);if(info)memory.check(GuestAddress{arg(out_arg)},36,Access::Write);else{output(out_arg);write_pointer(arg(out_arg),0);}require(!arg(pump_arg));
            if(arg(status_arg))memory.check(GuestAddress{arg(status_arg)},4,Access::Write);
            std::vector<std::uint8_t> source;
            if(file){
                const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(source_arg)})):read_ansi(memory,arg(source_arg));
                const auto opened=filesystem_.open_reader(path);if(opened.error)throw texture_image::Error(0x80070000U|opened.error,"無法讀取紋理檔案");
                const auto metadata=opened.file->information();if(metadata.error)throw texture_image::Error(0x80070000U|metadata.error,"無法取得紋理檔案大小");require(metadata.metadata.size<=512U*1024U*1024U);
                source.resize(static_cast<std::size_t>(metadata.metadata.size));const auto io=opened.file->read(source,0);if(io.error || io.bytes!=source.size())throw texture_image::Error(0x80070000U|(io.error?io.error:38U),"紋理檔案讀取未完成");
            }else if(res){
                const auto found=gui_resource(memory,arg(source_arg),arg(source_arg+1),10,symbol.ends_with('W'));if(!found)throw texture_image::Error(0x80070716U,"找不到紋理 RCDATA 資源");
                const auto size=memory.load(GuestAddress{found+4},32);require(size<=512U*1024U*1024U);source=read_bytes(memory,resource_image(found,true)->base+memory.load(GuestAddress{found},32),size);
            }else{require(arg(source_arg+1)<=512U*1024U*1024U);source=read_bytes(memory,arg(source_arg),static_cast<std::size_t>(arg(source_arg+1)));}
            auto decoded=texture_image::decode(source);auto d=decoded.desc;
            const auto emit_info=[&](std::uint64_t pointer){const std::array<std::uint32_t,9> fields{decoded.desc.width,decoded.desc.height,decoded.desc.depth,decoded.desc.layers,decoded.desc.levels,decoded.desc.misc,decoded.desc.format,decoded.desc.dimension,decoded.file_format};for(unsigned i=0;i<fields.size();++i)memory.store(GuestAddress{pointer+4*i},32,fields[i]);};
            if(info)emit_info(arg(out_arg));
            else{
                const auto device=typed(arg(0),Kind::Device);std::uint32_t usage=0,access=0,first=0,levels=d.levels;
                if(arg(load_arg)){
                    const auto load=read_bytes(memory,arg(load_arg),48+pointer_size);
                    const auto option=[&](unsigned at,std::uint32_t fallback){const auto value=word(load,at);const bool from_file=value==UINT32_MAX-2 && (at==0 || at==4 || at==8 || at==16 || at==36);return value==UINT32_MAX || from_file?fallback:value;};
                    first=option(12,0);require(first<d.levels);levels=option(16,d.levels-first);if(!levels)levels=d.levels-first;require(levels<=d.levels-first);
                    require(option(0,std::max(1U,d.width>>first))==std::max(1U,d.width>>first) && option(4,std::max(1U,d.height>>first))==std::max(1U,d.height>>first) && (d.dimension!=4 || option(8,std::max(1U,d.depth>>first))==std::max(1U,d.depth>>first)));
                    usage=option(20,0);d.bind=option(24,8);access=option(28,0);d.misc=option(32,d.misc);
                    const auto target=option(36,d.format);
                    const auto rgba=[](std::uint32_t f){return f>=27 && f<=29;};const auto bgra=[](std::uint32_t f){return f==87 || f==90 || f==91;};
                    if(!(target==d.format || (rgba(target) && rgba(d.format)) || (bgra(target) && bgra(d.format)) || (graphics_format_bc_family(target) && graphics_format_bc_family(target)==graphics_format_bc_family(d.format)))){try{texture_image::convert_float(decoded,target);}catch(const texture_image::Error&){texture_image::convert_format(decoded,target,word(load,40));}}
                    d.format=target;const auto src_info=memory.load(GuestAddress{arg(load_arg)+48},pointer_bits);if(src_info)emit_info(src_info);
                }
                if(std::getenv("X64ARM_TRACE_GRAPHICS_STATE"))std::fprintf(stderr,"[d3dx11_texture] format=%u usage=%u bind=0x%x cpu=0x%x size=%ux%ux%u levels=%u\n",d.format,usage,d.bind,access,d.width,d.height,d.depth,levels);
                require(usage<=3 && !(access&~0x30000U) && !(usage==0 && access) && !(usage==1 && access) && !(usage==2 && access!=0x10000) && !(usage==3 && (d.bind || !access)));
                const auto source_levels=d.levels;d.width=std::max(1U,d.width>>first);d.height=std::max(1U,d.height>>first);d.depth=std::max(1U,d.depth>>first);d.levels=levels;
                auto resource=create(d.dimension==2?Kind::Texture1D:d.dimension==4?Kind::Texture3D:Kind::Texture,device->root);
                try{
                    resource->resource=device->native->graphics_resource(d);
                    resource->desc.resize(d.dimension==2?32:d.dimension==4?36:44);
                    auto& desc=resource->desc;put_word(desc,0,d.width);
                    if(d.dimension==2){put_word(desc,4,d.levels);put_word(desc,8,d.layers);put_word(desc,12,d.format);put_word(desc,16,usage);put_word(desc,20,d.bind);put_word(desc,24,access);put_word(desc,28,d.misc);}
                    else if(d.dimension==4){put_word(desc,4,d.height);put_word(desc,8,d.depth);put_word(desc,12,d.levels);put_word(desc,16,d.format);put_word(desc,20,usage);put_word(desc,24,d.bind);put_word(desc,28,access);put_word(desc,32,d.misc);}
                    else{const std::array fields{d.width,d.height,d.levels,d.layers,d.format,1U,0U,usage,d.bind,access,d.misc};for(unsigned i=0;i<fields.size();++i){put_word(desc,i*4,fields[i]);resource->texture_desc[i]=fields[i];}}
                    for(unsigned layer=0;layer<d.layers;++layer)for(unsigned level=0;level<d.levels;++level){const auto pitch=graphics_format_pitch(d.format,std::max(1U,d.width>>level),std::max(1U,d.height>>level));device->native->graphics_write(subresource(resource,layer*d.levels+level),decoded.pixels.at(layer*source_levels+first+level),pitch[0],pitch[0]*pitch[1]);}
                    if(view){
                        require(d.bind&8);auto srv=create(Kind::ShaderResource,device->root);
                        srv->texture=resource->root;hold(resource->root);srv->resource=resource->resource;srv->graphics_view={resource->resource,d.format,0,d.levels,0,d.layers};
                        const auto dimension=d.dimension==2?(d.layers>1?3U:2U):d.dimension==4?8U:(d.misc&4)?(d.layers>6?10U:9U):d.layers>1?5U:4U;srv->graphics_view.dimension=dimension;
                        srv->desc.resize(24);put_word(srv->desc,0,d.format);put_word(srv->desc,4,dimension);put_word(srv->desc,12,d.levels);if(d.layers>1)put_word(srv->desc,20,(d.misc&4)?d.layers/6:d.layers);
                        write_pointer(arg(out_arg),srv->root);release(resource->root);
                    }else write_pointer(arg(out_arg),resource->root);
                }catch(...){release(resource->root);throw;}
            }
            if(arg(status_arg))memory.store(GuestAddress{arg(status_arg)},32,result);
        }
