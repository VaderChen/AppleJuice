#include "d3dx9_image_helpers.inc"
        bool texture_handled=false;
#include "d3dx9_surface_load.inc"
        else if(symbol.starts_with("D3DXSaveTextureToFile")){
            texture_handled=true;mesh_handled=true;const bool to_memory=symbol.ends_with("InMemory");if(to_memory)output(0);else require(arg(0));require(!arg(3));auto texture=get_texture(arg(2));
            GraphicsResourceDesc desc;desc.dimension=texture->kind==Kind::VolumeTexture?4:3;desc.width=texture->width;desc.height=texture->height;desc.depth=texture->depth;desc.layers=texture->kind==Kind::CubeTexture?6:1;desc.levels=texture->levels;desc.format=native_format(texture->format);desc.misc=texture->kind==Kind::CubeTexture?4:0;
            if(u(1)!=4)desc.format=28;
            const auto data=texture_image::encode(desc,u(1),[&](unsigned index){auto surface=get(texture->subresources.at(index),texture->kind==Kind::VolumeTexture?Kind::Volume:Kind::Surface);require(!surface->mapped);read_surface(*surface);if(u(1)==4)return surface->pixels;return texture_image::transform(surface->pixels,native_format(surface->format),{surface->width,surface->height,surface->depth},{0,0,0,surface->width,surface->height,surface->depth},28,{surface->width,surface->height,surface->depth},1);});
            if(to_memory)store_pointer(arg(0),blob(data)->root);
            else {const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));const auto file=filesystem_.open_host(d3dx_path(path),0x40000000U,2,0x80,1);require(!file.error,0x80070000U|file.error);const auto written=file.file->write(data,0);require(!written.error && written.bytes==data.size(),0x80070000U|(written.error?written.error:29U));}
        }else if(symbol=="D3DXFilterTexture"){
            // (pBaseTexture, pPalette, SrcLevel, MipFilter)：自 SrcLevel 起逐級縮小產生後續 mip，與載入檔案時的 mip 產生共用 transform；
            // D3DX_DEFAULT 的 SrcLevel 為 0、MipFilter 為 BOX。調色盤只作用於 P8 類格式，共用像素轉換不支援該類格式，故不讀取。
            texture_handled=true;mesh_handled=true;auto texture=get_texture(arg(0));const bool volume=texture->kind==Kind::VolumeTexture,cube=texture->kind==Kind::CubeTexture;const auto part=volume?Kind::Volume:Kind::Surface;
            const unsigned source_level=u(2)==UINT32_MAX?0U:u(2),filter=u(3)==UINT32_MAX?5U:u(3);require(source_level<texture->levels && (filter&15)>=1 && (filter&15)<=5 && !(filter&~0x7f000fU));
            for(unsigned face=0;face<(cube?6U:1U);++face){
                auto previous=get(texture->subresources.at(face*texture->levels+source_level),part);require(!previous->mapped);read_surface(*previous);
                for(unsigned level=source_level+1;level<texture->levels;++level){
                    auto target=get(texture->subresources.at(face*texture->levels+level),part);require(!target->mapped);
                    const std::array<std::uint32_t,3> from{previous->width,previous->height,std::max(1U,previous->depth)},to{target->width,target->height,std::max(1U,target->depth)};
                    target->pixels=texture_image::transform(previous->pixels,native_format(previous->format),from,{0,0,0,from[0],from[1],from[2]},native_format(target->format),to,filter);write_surface(*target);previous=target;
                }
            }
        }else if(symbol.starts_with("D3DXSaveSurfaceToFile") || symbol.starts_with("D3DXSaveVolumeToFile")){
            // (pDest/ppDestBuf, DestFormat, pSrcSurface／pSrcVolume, pSrcPalette, pSrcRect／pSrcBox)：與 D3DXSaveTextureToFile 共用編碼；
            // 範圍以 texture_image::transform 擷取（DDS 保留原格式，其他格式轉成 RGBA8）；NULL 範圍為整個表面／體積。調色盤格式不支援。
            texture_handled=true;mesh_handled=true;const bool volume=symbol.starts_with("D3DXSaveVolume"),to_memory=symbol.ends_with("InMemory");
            if(to_memory)output(0);else require(arg(0));require(!arg(3));auto surface=get(arg(2),volume?Kind::Volume:Kind::Surface);require(!surface->mapped);read_surface(*surface);
            std::array<std::uint32_t,6> box{0,0,0,surface->width,surface->height,surface->depth};
            if(arg(4)){
                if(volume){for(unsigned i=0;i<6;++i){const auto v=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+i*4},32));box[i==0?0:i==1?1:i==2?3:i==3?4:i==4?2:5]=v;}}
                else{std::array<std::int32_t,4> r{};for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(4)+i*4},32));require(r[0]>=0 && r[1]>=0);box={static_cast<std::uint32_t>(r[0]),static_cast<std::uint32_t>(r[1]),0,static_cast<std::uint32_t>(std::max(0,r[2])),static_cast<std::uint32_t>(std::max(0,r[3])),1};}
            }
            require(box[0]<box[3] && box[1]<box[4] && box[2]<box[5] && box[3]<=surface->width && box[4]<=surface->height && box[5]<=surface->depth);
            const std::array<std::uint32_t,3> size{box[3]-box[0],box[4]-box[1],box[5]-box[2]};const auto source_format=native_format(surface->format);const bool whole=size[0]==surface->width && size[1]==surface->height && size[2]==surface->depth;
            GraphicsResourceDesc desc;desc.dimension=volume?4:3;desc.width=size[0];desc.height=size[1];desc.depth=size[2];desc.layers=1;desc.levels=1;desc.format=u(1)==4?source_format:28;
            const auto data=texture_image::encode(desc,u(1),[&](unsigned){if(u(1)==4 && whole)return surface->pixels;return texture_image::transform(surface->pixels,source_format,{surface->width,surface->height,surface->depth},box,desc.format,size,1);});
            if(to_memory)store_pointer(arg(0),blob(data)->root);
            else {const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));const auto file=filesystem_.open_host(d3dx_path(path),0x40000000U,2,0x80,1);require(!file.error,0x80070000U|file.error);const auto written=file.file->write(data,0);require(!written.error && written.bytes==data.size(),0x80070000U|(written.error?written.error:29U));}
        }else if(symbol=="D3DXCheckTextureRequirements" || symbol=="D3DXCheckCubeTextureRequirements" || symbol=="D3DXCheckVolumeTextureRequirements"){
            // 依 make_texture 的限制調整：尺寸為 0 或 D3DX_DEFAULT 時取另一邊或 256，上限 16384（體積 2048），立方體為正方形；
            // mip 層數為 0 或 D3DX_DEFAULT 時取完整鏈，否則不超過完整鏈；格式 D3DFMT_UNKNOWN 或不支援時依是否含 alpha 取 A8R8G8B8／X8R8G8B8。
            texture_handled=true;mesh_handled=true;(void)get(arg(0),Kind::Device);const bool cube=symbol=="D3DXCheckCubeTextureRequirements",volume=symbol=="D3DXCheckVolumeTextureRequirements";
            const unsigned levels_at=cube?2:volume?4:3,format_at=levels_at+2;
            const auto load=[&](unsigned i,std::uint32_t fallback){return arg(i)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(i)},32)):fallback;};
            const auto valid=[](std::uint32_t v){return v && v!=UINT32_MAX;};
            const unsigned limit=volume?2048:16384;
            auto width=load(1,0),height=cube?width:load(2,0),depth=volume?load(3,0):1;
            if(!valid(width))width=valid(height)?height:256;if(!valid(height))height=cube?width:valid(width)?width:256;if(volume && !valid(depth))depth=1;
            width=std::min(width,limit);height=std::min(height,limit);depth=std::min(depth,limit);if(cube)height=width=std::max(width,height);
            const auto full=static_cast<std::uint32_t>(std::bit_width(std::max({width,height,depth})));auto levels=load(levels_at,0);levels=valid(levels)?std::min(levels,full):full;
            if(u(levels_at+1)&0x400)levels=1; // D3DUSAGE_AUTOGENMIPMAP：只建立第一層
            auto format=load(format_at,0);
            if(!format || format==UINT32_MAX || (!native_format(format) && !d9_raw_depth(format))){
                // A8R8G8B8、A1R5G5B5、A4R4G4B4、A8、A2B10G10R10、A8B8G8R8、A2R10G10B10、A16B16G16R16、A16B16G16R16F、A32B32G32R32F、DXT1～DXT5 視為含 alpha。
                static constexpr std::array<std::uint32_t,15> alpha{21,25,26,28,31,32,35,36,113,116,0x31545844,0x32545844,0x33545844,0x34545844,0x35545844};
                format=!format || format==UINT32_MAX || std::find(alpha.begin(),alpha.end(),format)!=alpha.end()?21U:22U; // 未指定格式時為 A8R8G8B8
            }
            if(arg(1))memory.store(GuestAddress{arg(1)},32,width);if(!cube && arg(2))memory.store(GuestAddress{arg(2)},32,height);if(volume && arg(3))memory.store(GuestAddress{arg(3)},32,depth);
            if(arg(levels_at))memory.store(GuestAddress{arg(levels_at)},32,levels);if(arg(format_at))memory.store(GuestAddress{arg(format_at)},32,format);result=0;
        }else if(symbol=="D3DXComputeNormalMap"){
            texture_handled=true;mesh_handled=true;auto target=get(arg(0),Kind::Texture),source=get(arg(1),Kind::Texture);
            require(target->width==source->width && target->height==source->height && target->levels<=source->levels);
            std::vector<std::vector<std::uint8_t>> levels;levels.reserve(target->levels);
            for(unsigned level=0;level<target->levels;++level){auto from=get(source->subresources.at(level),Kind::Surface),to=get(target->subresources.at(level),Kind::Surface);require(!from->mapped && !to->mapped);read_surface(*from);levels.push_back(texture_image::normal_map(from->pixels,native_format(source->format),from->width,from->height,native_format(target->format),u(3),u(4),scalar_arg(5)));}
            for(unsigned level=0;level<target->levels;++level){auto to=get(target->subresources.at(level),Kind::Surface);to->pixels=std::move(levels[level]);write_surface(*to);}
        }else if(symbol=="D3DXCreateTexture" || symbol=="D3DXCreateCubeTexture" || symbol=="D3DXCreateVolumeTexture"){
            const bool cube=symbol=="D3DXCreateCubeTexture",volume=symbol=="D3DXCreateVolumeTexture";
            texture_handled=true;mesh_handled=true;const unsigned shift=volume?1:0,destination=cube?6:7+shift;output(destination);const auto device=get(arg(0),Kind::Device);const auto width=u(1)==UINT32_MAX?256:u(1),height=cube?width:u(2)==UINT32_MAX?256:u(2),depth=volume?(u(3)==UINT32_MAX?256:u(3)):1,levels=u(cube?2:3+shift)==UINT32_MAX?0:u(cube?2:3+shift);auto texture=make_texture(device,width,height,levels,u(cube?3:4+shift),u(cube?4:5+shift)?u(cube?4:5+shift):21,u(cube?5:6+shift),cube,depth,volume);store_pointer(arg(destination),texture->root);
        }else if((symbol.starts_with("D3DXCreateTextureFrom") || symbol.starts_with("D3DXCreateCubeTextureFrom") || symbol.starts_with("D3DXCreateVolumeTextureFrom"))){
            texture_handled=true;mesh_handled=true;const bool cube=symbol.starts_with("D3DXCreateCube"),volume=symbol.starts_with("D3DXCreateVolume"),resource=symbol.find("Resource")!=symbol.npos,from_memory=symbol.find("InMemory")!=symbol.npos,extended=symbol.find("Ex")!=symbol.npos;const unsigned shift=resource || from_memory?1:0,destination=extended?13+shift+(volume?1U:0U)-(cube?1U:0U):2+shift;output(destination);auto device=get(arg(0),Kind::Device);
            const auto tu=[&](unsigned index){return u(index+shift+(volume && index>=4?1U:0U)-(cube && index>=4?1U:0U));};const auto ta=[&](unsigned index){return arg(index+shift+(volume && index>=4?1U:0U)-(cube && index>=4?1U:0U));};
            auto image=load_d3dx_image(1,resource,from_memory);const auto source_desc=image.desc;require((volume?(source_desc.dimension==3 || source_desc.dimension==4):source_desc.dimension==3) && (cube?(source_desc.layers==6 && (source_desc.misc&4) && source_desc.width==source_desc.height):source_desc.layers==1),0x88760b59U);const auto format_from_file=legacy_format(source_desc.format);
            const auto dimension=[](unsigned requested,unsigned original){return !requested || requested==UINT32_MAX?std::bit_ceil(original):requested>=UINT32_MAX-2?original:requested;};
            const auto width=dimension(extended?u(2+shift):0,source_desc.width),height=cube?width:dimension(extended?u(3+shift):0,source_desc.height),depth=volume?dimension(extended?u(4+shift):0,source_desc.depth):1,requested_levels=extended?tu(4):0,levels=requested_levels==UINT32_MAX-2?source_desc.levels:requested_levels==UINT32_MAX?0:requested_levels;
            const auto requested_format=extended?tu(6):0,format=!requested_format || requested_format==UINT32_MAX-2?format_from_file:requested_format;
            auto texture=make_texture(device,width,height,levels,extended?tu(5):0,format,extended?tu(7):(device->ex?0:1),cube,depth,volume);
            try{
                const auto target_format=native_format(format),filter=extended && tu(8)!=UINT32_MAX?tu(8):4,mip_filter=extended && tu(9)!=UINT32_MAX?tu(9):5,key=extended?tu(10):0;
                if(extended)require(!ta(12),unavailable);
                if(key){texture_image::convert_format(image,28,1);image.desc.format=28;for(auto& data:image.pixels)for(std::size_t i=0;i<data.size();i+=4){const auto pixel=(unsigned(data[i+3])<<24)|(unsigned(data[i])<<16)|(unsigned(data[i+1])<<8)|data[i+2];if(pixel==key)std::fill_n(data.data()+i,4,0);}}
                for(unsigned face=0;face<(cube?6U:1U);++face)for(unsigned level=0;level<texture->levels;++level){auto surface=get(texture->subresources.at(face*texture->levels+level),volume?Kind::Volume:Kind::Surface);const bool original=level<image.desc.levels;const auto source_level=original?level:level-1;const auto source_width=original?std::max(1U,image.desc.width>>source_level):std::max(1U,width>>source_level),source_height=original?std::max(1U,image.desc.height>>source_level):std::max(1U,height>>source_level),source_depth=original?std::max(1U,image.desc.depth>>source_level):std::max(1U,depth>>source_level);const auto& source=original?image.pixels.at(face*image.desc.levels+source_level):get(texture->subresources.at(face*texture->levels+source_level),volume?Kind::Volume:Kind::Surface)->pixels;surface->pixels=texture_image::transform(source,original?image.desc.format:target_format,{source_width,source_height,source_depth},{0,0,0,source_width,source_height,source_depth},target_format,{surface->width,surface->height,surface->depth},level?mip_filter:filter);write_surface(*surface);}
                if(extended && ta(11)){const std::array<unsigned,7> info{source_desc.width,source_desc.height,source_desc.depth,source_desc.levels,format_from_file,volume?4U:cube?5U:3U,image.file_format};memory.write(GuestAddress{ta(11)},std::span(reinterpret_cast<const std::uint8_t*>(info.data()),sizeof(info)));}
                store_pointer(arg(destination),texture->root);
            }catch(...){release(texture->root);throw;}
        }else if(symbol.starts_with("D3DXGetImageInfoFrom")){
            texture_handled=true;mesh_handled=true;
            const bool resource=symbol.find("Resource")!=symbol.npos,from_memory=symbol.ends_with("InMemory");
            const auto image=load_d3dx_image(0,resource,from_memory);const auto& d=image.desc;
            require(d.layers==1 || ((d.misc&4) && d.layers==6),0x88760b59U);require(image.file_format<=8,0x88760b59U);
            d3dx_image_info(image,arg(resource || from_memory?2:1));
        }
