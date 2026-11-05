        if(symbol.starts_with("D3DXLoadSurfaceFrom") || symbol.starts_with("D3DXLoadVolumeFrom")){
            texture_handled=true;mesh_handled=true;
            const bool volume=symbol.starts_with("D3DXLoadVolume"),raw=symbol.ends_with("FromMemory");
            const bool from_object=symbol.ends_with("FromSurface") || symbol.ends_with("FromVolume");
            const bool resource=symbol.find("Resource")!=symbol.npos,encoded_memory=symbol.ends_with("FileInMemory");
            auto target=get(arg(0),volume?Kind::Volume:Kind::Surface);
            require(!target->mapped && !target->multisample && !depth_format(target->format));
            const auto region=[&](std::uint64_t address,std::array<unsigned,3> size,bool required=false){
                require(address || !required);std::array<unsigned,6> box{0,0,0,size[0],size[1],size[2]};
                if(address){const auto bytes=read(address,volume?24:16);
                    box={word(bytes,0),word(bytes,4),volume?word(bytes,16):0U,word(bytes,8),word(bytes,12),volume?word(bytes,20):1U};}
                for(unsigned i=0;i<3;++i)require(box[i]<box[i+3] && box[i+3]<=size[i]);
                return box;
            };
            const auto destination=region(arg(2),{target->width,target->height,target->depth});
            const std::array destination_size{destination[3]-destination[0],destination[4]-destination[1],destination[5]-destination[2]};
            struct Source {unsigned format{};std::array<unsigned,3> size{};std::vector<std::uint8_t> pixels;};
            Source source;std::array<unsigned,6> source_box{};texture_image::Decoded image;
            unsigned filter{},key{};std::uint64_t info_address{};
            // 舊式 RGB／亮度／色盤輸入展開後，仍交給共用格式轉換與濾波器。
            const auto expand=[&](unsigned legacy,std::uint64_t palette){
                if(legacy==50 || legacy==51 || legacy==81){source.pixels=sampled_pixels(source.pixels,legacy);source.format=sampled_format(legacy);return;}
                if(legacy==24 || legacy==30){for(std::size_t at=1;at<source.pixels.size();at+=2)source.pixels[at]|=legacy==24?0x80:0xf0;return;}
                if(legacy==33){for(std::size_t at=3;at<source.pixels.size();at+=4)source.pixels[at]=255;return;}
                const unsigned step=legacy==20?3:(legacy==29 || legacy==40)?2:1;
                if(legacy!=20 && legacy!=27 && legacy!=29 && legacy!=40 && legacy!=41 && legacy!=52)return;
                std::vector<std::uint8_t> colors;if(legacy==40 || legacy==41){require(palette);colors=read(palette,1024);}
                std::vector<std::uint8_t> rgba(source.pixels.size()/step*4);
                for(std::size_t i=0;i<rgba.size()/4;++i){const auto* in=source.pixels.data()+i*step;auto* out=rgba.data()+i*4;
                    if(legacy==20){out[0]=in[2];out[1]=in[1];out[2]=in[0];out[3]=255;}
                    else if(legacy==40 || legacy==41){std::copy_n(colors.data()+unsigned(in[0])*4,4,out);if(legacy==40)out[3]=in[1];}
                    else if(legacy==52){out[0]=out[1]=out[2]=(in[0]&15)*17;out[3]=(in[0]>>4)*17;}
                    else {out[0]=((in[0]>>5)*255+3)/7;out[1]=(((in[0]>>2)&7)*255+3)/7;out[2]=(in[0]&3)*85;out[3]=legacy==29?in[1]:255;}
                }
                source.pixels=std::move(rgba);source.format=28;
            };
            if(raw){
                require(arg(3));const auto legacy=u(4);require(!depth_format(legacy));
                source.format=legacy==24?86:legacy==30?115:legacy==33?28:
                    (legacy==20 || legacy==27 || legacy==29 || legacy==40 || legacy==41 || legacy==52)?28:native_format(legacy);
                const auto block=graphics_format_bc_family(source.format)?4U:1U;
                source_box=region(arg(volume?8:7),{UINT32_MAX,UINT32_MAX,volume?UINT32_MAX:1U},true);
                const auto left=source_box[0]/block*block,top=source_box[1]/block*block;
                source.size={source_box[3]-left,source_box[4]-top,source_box[5]-source_box[2]};
                require(source.size[0]<=16384 && source.size[1]<=16384 && source.size[2]<=2048 && std::uint64_t(source.size[0])*source.size[1]*source.size[2]<=32U*1024U*1024U);
                const unsigned unit=legacy==20?3:(legacy==27 || legacy==41 || legacy==52)?1:(legacy==29 || legacy==40)?2:graphics_format_pitch(source.format,block,block)[0];
                const auto rows=(source.size[1]+block-1)/block;const auto row_bytes=std::uint64_t((source.size[0]+block-1)/block)*unit;
                const auto pitch=std::uint64_t(u(5)),slice=volume?std::uint64_t(u(6)):0;
                require(pitch>=(std::uint64_t(source_box[3])+block-1)/block*unit);
                if(volume && source_box[5]>1)require(slice>=pitch*((std::uint64_t(source_box[4])+block-1)/block));
                require(row_bytes*rows*source.size[2]<=512U*1024U*1024U);
                source.pixels.resize(row_bytes*rows*source.size[2]);
                for(unsigned z=0;z<source.size[2];++z)for(unsigned y=0;y<rows;++y){
                    const auto slice_offset=std::uint64_t(source_box[2]+z)*slice,row_offset=std::uint64_t(top/block+y)*pitch+std::uint64_t(left/block)*unit;
                    require(slice_offset<=UINT64_MAX-row_offset);const auto offset=slice_offset+row_offset;
                    require(offset<=UINT64_MAX-arg(3) && row_bytes<=UINT64_MAX-arg(3)-offset);
                    const auto row=read(arg(3)+offset,row_bytes);
                    std::memcpy(source.pixels.data()+(std::size_t(z)*rows+y)*row_bytes,row.data(),row.size());
                }
                source_box={source_box[0]-left,source_box[1]-top,0,source.size[0],source.size[1],source.size[2]};
                expand(legacy,arg(volume?7:6));filter=u(volume?9:8);key=u(volume?10:9);
            }else if(from_object){
                auto from=get(arg(3),volume?Kind::Volume:Kind::Surface);require(!from->mapped && !from->multisample && !depth_format(from->format));
                source.size={from->width,from->height,from->depth};source_box=region(arg(5),source.size);
                read_surface(*from);source.pixels=from->pixels;source.format=native_format(from->format);expand(from->format,arg(4));
                filter=u(6);key=u(7);
            }else{
                image=load_d3dx_image(3,resource,encoded_memory);const auto& d=image.desc;
                require((d.dimension==3 || d.dimension==4) && !image.pixels.empty(),0x88760b59U);
                source.size={d.width,d.height,volume?d.depth:1U};source.format=d.format;source.pixels=image.pixels.front();
                if(!volume && d.depth>1){const auto pitch=graphics_format_pitch(d.format,d.width,d.height);source.pixels.resize(std::size_t(pitch[0])*pitch[1]);}
                const auto shift=resource || encoded_memory?1U:0U;
                source_box=region(arg(4+shift),source.size);filter=u(5+shift);key=u(6+shift);info_address=arg(7+shift);
                if(info_address)memory.check(GuestAddress{info_address},28,Access::Write);
            }
            if(filter==UINT32_MAX)filter=0x80004U;
            require((filter&15)>=1 && (filter&15)<=5 && !(filter&~0x7f000fU));
            if(key){
                const std::array full{0U,0U,0U,source.size[0],source.size[1],source.size[2]};
                auto colors=texture_image::transform(source.pixels,source.format,source.size,full,28,source.size,1);
                // 色鍵比較使用 ARGB8，但 HDR 未匹配像素保留浮點精度與範圍。
                auto values=texture_image::transform(source.pixels,source.format,source.size,full,2,source.size,1);
                for(std::size_t at=0;at<colors.size();at+=4){const auto color=(unsigned(colors[at+3])<<24)|(unsigned(colors[at])<<16)|(unsigned(colors[at+1])<<8)|colors[at+2];if(color==key)std::fill_n(values.data()+at*4,16,0);}
                source.pixels=std::move(values);source.format=2;
            }
            const auto target_format=native_format(target->format);
            const bool partial_block=graphics_format_bc_family(target_format) &&
                ((destination[0]%4) || (destination[1]%4) || ((destination[3]%4) && destination[3]!=target->width) || ((destination[4]%4) && destination[4]!=target->height));
            if(partial_block){
                // 只重編碼相交的壓縮區塊，其餘區塊位元完全保留。
                const std::array aligned{destination[0]/4*4,destination[1]/4*4,destination[2],std::min(target->width,(destination[3]+3)/4*4),std::min(target->height,(destination[4]+3)/4*4),destination[5]};
                const std::array size{aligned[3]-aligned[0],aligned[4]-aligned[1],aligned[5]-aligned[2]};
                read_surface(*target);
                auto preserved=texture_image::transform(target->pixels,target_format,{target->width,target->height,target->depth},aligned,28,size,1);
                auto converted=texture_image::transform(source.pixels,source.format,source.size,source_box,28,destination_size,filter);
                for(unsigned z=0;z<destination_size[2];++z)for(unsigned y=0;y<destination_size[1];++y)
                    std::memcpy(preserved.data()+((std::size_t(z)*size[1]+destination[1]-aligned[1]+y)*size[0]+destination[0]-aligned[0])*4,converted.data()+(std::size_t(z)*destination_size[1]+y)*destination_size[0]*4,std::size_t(destination_size[0])*4);
                auto encoded=texture_image::transform(preserved,28,size,{0,0,0,size[0],size[1],size[2]},target_format,size,1);
                write_surface_region(*target,encoded,aligned);
            }else{
                auto converted=texture_image::transform(source.pixels,source.format,source.size,source_box,target_format,destination_size,filter);
                write_surface_region(*target,converted,destination);
            }
            d3dx_image_info(image,info_address);
        }
