        if(symbol=="D3DX11LoadTextureFromTexture"){
            graphics_handled=true;
            const auto context=typed(arg(0),Kind::Context),source=object(arg(1)).identity,destination=object(arg(3)).identity;
            require(!context->context_type && source->device==context->device && destination->device==context->device && source->resource && destination->resource && !source->offered && !destination->offered);
            const auto& sd=source->resource->desc;const auto& dd=destination->resource->desc;
            require(sd.dimension>=2 && sd.dimension==dd.dimension && sd.samples==1 && dd.samples==1);
            std::array<std::uint32_t,8> options{0,0,UINT32_MAX,0,0,UINT32_MAX,UINT32_MAX,UINT32_MAX};
            std::array<std::array<std::uint32_t,6>,2> boxes{};std::array<bool,2> boxed{};
            if(arg(2)){
                const auto info=read_bytes(memory,arg(2),pointer_size*2+32);
                for(unsigned i=0;i<8;++i)options[i]=word(info,pointer_size*2+i*4);
                for(unsigned i=0;i<2;++i){const auto pointer=memory.load(GuestAddress{arg(2)+i*pointer_size},pointer_bits);if(pointer){boxed[i]=true;const auto bytes=read_bytes(memory,pointer,24);for(unsigned j=0;j<6;++j)boxes[i][j]=word(bytes,j*4);}}
            }
            const auto sm=options[0],dm=options[1],se=options[3],de=options[4];
            require(sm<sd.levels && dm<dd.levels && se<sd.layers && de<dd.layers);
            const auto mips=options[2]==UINT32_MAX?dd.levels-dm:options[2];
            const auto elements=options[5]==UINT32_MAX?std::min(sd.layers-se,dd.layers-de):options[5];
            require(mips && mips<=dd.levels-dm && elements && elements<=sd.layers-se && elements<=dd.layers-de);
            const auto dimensions=[](const auto& desc,unsigned mip){return std::array{std::max(1U,desc.width>>mip),std::max(1U,desc.height>>mip),std::max(1U,desc.depth>>mip)};};
            const auto box_at=[&](unsigned which,unsigned relative,const auto& size){
                std::array<std::uint32_t,6> box{0,0,0,size[0],size[1],size[2]};
                if(boxed[which])for(unsigned j=0;j<3;++j){box[j]=boxes[which][j]>>relative;box[j+3]=std::max(1U,boxes[which][j+3]>>relative);}
                for(unsigned j=0;j<3;++j)require(box[j]<box[j+3] && box[j+3]<=size[j]);return box;
            };
            // 先讀回來源，避免來源與目的為同一資源時後續 mip／element 讀到已覆寫內容。
            std::vector<std::vector<std::uint8_t>> inputs;
            const auto available=std::min(mips,sd.levels-sm);
            for(unsigned element=0;element<elements;++element)for(unsigned mip=0;mip<available;++mip)
                inputs.push_back(context->native->graphics_read(subresource(source,(se+element)*sd.levels+sm+mip)));
            for(unsigned element=0;element<elements;++element){
                std::vector<std::uint8_t> previous;std::array<std::uint32_t,3> previous_size{};
                for(unsigned mip=0;mip<mips;++mip){
                    const auto dst_size=dimensions(dd,dm+mip);const auto dst_box=box_at(1,mip,dst_size);
                    const std::array extent{dst_box[3]-dst_box[0],dst_box[4]-dst_box[1],dst_box[5]-dst_box[2]};
                    std::vector<std::uint8_t> output_data;
                    if(mip<available){const auto src_size=dimensions(sd,sm+mip);const auto src_box=box_at(0,mip,src_size);
                        const auto& input=inputs.at(element*available+mip);
                        if(sd.format==dd.format && src_size==extent && src_box==std::array{0U,0U,0U,src_size[0],src_size[1],src_size[2]} && (options[6]==UINT32_MAX || !(options[6]&0x780000U)))output_data=input;
                        else output_data=texture_image::transform(input,sd.format,src_size,src_box,dd.format,extent,options[6]);
                    }else output_data=texture_image::transform(previous,dd.format,previous_size,{0,0,0,previous_size[0],previous_size[1],previous_size[2]},dd.format,extent,options[7]);
                    const auto pitch=graphics_format_pitch(dd.format,extent[0],extent[1]);
                    context->native->graphics_write_region(subresource(destination,(de+element)*dd.levels+dm+mip),output_data,pitch[0],pitch[0]*pitch[1],dst_box);
                    previous=std::move(output_data);previous_size=extent;
                }
            }
        }
