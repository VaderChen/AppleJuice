                }else if(symbol=="StretchRect" || symbol=="UpdateSurface" || symbol=="ColorFill"){
                    const bool fill=symbol=="ColorFill",update=symbol=="UpdateSurface";
                    auto source=fill?std::shared_ptr<Object>{}:get(arg(1),Kind::Surface);
                    auto target=get(arg(fill?1:3),Kind::Surface);
                    require(surface_device(*target)->root==object.root && !target->mapped);
                    const auto rectangle=[&](const Object& surface,std::uint64_t pointer){
                        std::array<unsigned,4> r{0,0,surface.width,surface.height};if(pointer){const auto data=read(pointer,16);for(unsigned i=0;i<4;++i)r[i]=word(data,i*4);}
                        require(r[0]<r[2] && r[1]<r[3] && r[2]<=surface.width && r[3]<=surface.height);return r;
                    };
                    const auto destination_format=depth_format(target->format)?depth_format(target->format):native_format(target->format);
                    auto destination=rectangle(*target,fill?arg(2):update?0:arg(4));
                    std::vector<std::uint8_t> converted;
                    if(fill){
                        require(target->pool==0 && !target->multisample && !depth_format(target->format) && !graphics_format_bc_family(destination_format));
                        const auto c=u(3);const std::array<std::uint8_t,4> bgra{static_cast<std::uint8_t>(c),static_cast<std::uint8_t>(c>>8),static_cast<std::uint8_t>(c>>16),static_cast<std::uint8_t>(c>>24)};
                        converted=texture_image::transform(bgra,87,{1,1,1},{0,0,0,1,1,1},destination_format,{destination[2]-destination[0],destination[3]-destination[1],1},2);
                    }else{
                        require(surface_device(*source)->root==object.root && !source->mapped);const auto area=rectangle(*source,arg(2));
                        if(update){require(source->pool==2 && target->pool==0 && source->format==target->format && !source->multisample && !target->multisample);if(arg(4)){const auto data=read(arg(4),8);destination[0]=word(data,0);destination[1]=word(data,4);}destination[2]=destination[0]+area[2]-area[0];destination[3]=destination[1]+area[3]-area[1];require(destination[2]>=destination[0] && destination[3]>=destination[1] && destination[2]<=target->width && destination[3]<=target->height);}
                        else require(source->pool==0 && target->pool==0 && u(5)<=2);
                        const bool full=area==std::array<unsigned,4>{0,0,source->width,source->height} && destination==std::array<unsigned,4>{0,0,target->width,target->height};
                        if(source->multisample || target->multisample || depth_format(source->format) || depth_format(target->format)){
                            require(!update && full && source->format==target->format && source->width==target->width && source->height==target->height && source->native_view.resource && target->native_view.resource);
                            if(depth_format(source->format))require(!object.scene && source->multisample==target->multisample);
                            if(source->multisample && !target->multisample)object.native->graphics_resolve(target->native_view,source->native_view,destination_format);
                            else {require(source->multisample==target->multisample);object.native->graphics_copy(target->native_view,source->native_view,{0,0,0},{});}
                            arg.return_to(state,0);return std::nullopt;
                        }
                        read_surface(*source);converted=texture_image::transform(source->pixels,native_format(source->format),{source->width,source->height,1},{area[0],area[1],0,area[2],area[3],1},destination_format,{destination[2]-destination[0],destination[3]-destination[1],1},!update && u(5)==2?3:2);
                    }
                    // 共用格式轉換與子區域寫入；GPU 表面保留未覆寫區域。
                    const auto width=destination[2]-destination[0],height=destination[3]-destination[1];const auto pitch=graphics_format_pitch(destination_format,width,height);
                    if(target->native_view.resource)object.native->graphics_write_region(target->native_view,converted,pitch[0],pitch[0]*pitch[1],{destination[0],destination[1],0,destination[2],destination[3],1});
                    else {const auto block=graphics_format_bc_family(destination_format)?4U:1U;require(!(destination[0]%block) && !(destination[1]%block));const auto row=graphics_format_pitch(destination_format,target->width,target->height),unit=graphics_format_pitch(destination_format,block,block);for(unsigned y=0;y<pitch[1];++y)std::copy_n(converted.data()+std::size_t(y)*pitch[0],pitch[0],target->pixels.data()+std::size_t(destination[1]/block+y)*row[0]+std::size_t(destination[0]/block)*unit[0]);}
