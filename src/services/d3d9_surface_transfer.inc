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
                        // 同格式、不縮放且兩端都是 GPU 資源的 StretchRect：直接在命令串流中 blit 區域，不等候 GPU、不經 CPU 回讀／
                        // 反 twiddle／重新上傳（原本每次都 graphics_read 等候在途命令）。順序由命令串流保證；
                        // 濾鏡在等大小時不影響結果。縮放、格式轉換與 UpdateSurface 仍走下方 CPU 路徑。X64ARM_D3D9_GPU_STRETCH=0 關閉。
                        static const bool gpu_stretch=[] {const auto* value=std::getenv("X64ARM_D3D9_GPU_STRETCH");return !value || std::string_view(value)!="0";}();
                        if(gpu_stretch && !update && source!=target && source->native_view.resource && target->native_view.resource && source->format==target->format &&
                           area[2]-area[0]==destination[2]-destination[0] && area[3]-area[1]==destination[3]-destination[1]){
                            object.native->graphics_copy(target->native_view,source->native_view,{destination[0],destination[1],0},{area[0],area[1],0,area[2],area[3],1});
                            surface_changed(*target);arg.return_to(state,0);return std::nullopt;
                        }
                        // 診斷：X64ARM_TRACE_D3D9_LOCK=1 一併列出 CPU 路徑的 StretchRect／UpdateSurface（前 32 次與之後每 1024 次）。
                        static const bool trace_transfer=std::getenv("X64ARM_TRACE_D3D9_LOCK")!=nullptr;
                        if(trace_transfer){static std::uint64_t transfers=0;if(++transfers<=32 || !(transfers&1023))std::fprintf(stderr,"[d3d9-transfer] n=%llu op=%s src=%ux%u fmt=%u pool=%u usage=0x%x native=%d dst=%ux%u fmt=%u pool=%u usage=0x%x native=%d area=%u,%u,%u,%u dest=%u,%u,%u,%u filter=%u\n",
                            static_cast<unsigned long long>(transfers),update?"update":"stretch",source->width,source->height,source->format,source->pool,source->usage,source->native_view.resource?1:0,
                            target->width,target->height,target->format,target->pool,target->usage,target->native_view.resource?1:0,area[0],area[1],area[2],area[3],destination[0],destination[1],destination[2],destination[3],update?0U:u(5));}
                        read_surface(*source);converted=texture_image::transform(source->pixels,native_format(source->format),{source->width,source->height,1},{area[0],area[1],0,area[2],area[3],1},destination_format,{destination[2]-destination[0],destination[3]-destination[1],1},!update && u(5)==2?3:2);
                    }
                    write_surface_region(*target,converted,{destination[0],destination[1],0,destination[2],destination[3],1});
