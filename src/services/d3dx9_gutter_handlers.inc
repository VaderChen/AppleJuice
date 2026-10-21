        const bool gutter_handled=symbol=="D3DXCreateTextureGutterHelper" || symbol.starts_with("Gutter.");
        if(gutter_handled){
            mesh_handled=true;
            if(symbol=="D3DXCreateTextureGutterHelper"){
                output(4);require(u(0) && u(1) && u(0)<=16384 && u(1)<=16384 && std::uint64_t{u(0)}*u(1)<=16777216);const auto size=scalar_arg(3);require(std::isfinite(size) && size>=0 && size<=static_cast<float>(std::max(u(0),u(1))));
                auto mesh=get(arg(2),Kind::Mesh);std::vector<std::array<float,2>> uv;for(const auto& v:positions(*mesh,5,0,2)){require(std::isfinite(v[0]) && std::isfinite(v[1]));uv.push_back({v[0],v[1]});}
                auto data=std::make_shared<d3dx_gutter::Map>(u(0),u(1),size,uv,mesh_indices(*mesh));auto helper=create(Kind::Gutter);helper->gutter=std::move(data);store_pointer(arg(4),helper->root);
            }else{
                auto helper=get(arg(0),Kind::Gutter);auto& map=*helper->gutter;const auto method=symbol.substr(7);const auto count=static_cast<std::size_t>(map.width)*map.height;
                const auto read_float_texture=[&](const std::shared_ptr<Object>& texture){auto surface=get(texture->subresources.at(0),Kind::Surface);require(!surface->mapped);read_surface(*surface);auto bytes=texture_image::transform(surface->pixels,native_format(surface->format),{surface->width,surface->height,1},{0,0,0,surface->width,surface->height,1},2,{surface->width,surface->height,1},1);std::vector<float> values(bytes.size()/4);std::memcpy(values.data(),bytes.data(),bytes.size());return values;};
                const auto write_float_texture=[&](const std::shared_ptr<Object>& texture,const std::vector<float>& values){auto surface=get(texture->subresources.at(0),Kind::Surface);require(!surface->mapped && texture->width==map.width && texture->height==map.height);surface->pixels=texture_image::transform(std::span(reinterpret_cast<const std::uint8_t*>(values.data()),values.size()*4),2,{map.width,map.height,1},{0,0,0,map.width,map.height,1},native_format(surface->format),{map.width,map.height,1},1);write_surface(*surface);};
                if(method=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);if(supports(Kind::Gutter,requested)){hold(helper);store_pointer(arg(2),helper->root);}else result=no_interface;}
                else if(method=="AddRef")result=hold(helper);else if(method=="Release")result=release(helper->root);
                else if(method=="GetWidth")result=map.width;else if(method=="GetHeight")result=map.height;
                else if(method.ends_with("Map")){
                    require(arg(1));const bool write=method.starts_with("Set");
                    const auto transfer=[&](auto& values){const auto bytes=values.size()*sizeof(values[0]);if(write){auto data=read(arg(1),bytes);auto next=values;std::memcpy(next.data(),data.data(),bytes);
                        if constexpr(std::is_same_v<typename std::remove_reference_t<decltype(values)>::value_type,unsigned>){for(auto value:next)require(value==UINT32_MAX || value<map.face_count);}
                        else if constexpr(std::is_same_v<typename std::remove_reference_t<decltype(values)>::value_type,std::uint8_t>){for(auto value:next)require(value==0 || value==1 || value==2 || value==4);}
                        else for(const auto& value:next)for(auto component:value)require(std::isfinite(component) && std::abs(component)<=32768);
                        values=std::move(next);
                    }else memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(values.data()),bytes));};
                    if(method.ends_with("FaceMap"))transfer(map.faces);else if(method.ends_with("BaryMap"))transfer(map.bary);else if(method.ends_with("TexelMap"))transfer(map.texels);else if(method.ends_with("GutterMap"))transfer(map.kinds);else require(false);
                }else if(method=="ApplyGuttersFloat"){
                    require(arg(1) && u(2) && u(2)<=4096 && u(3)==map.width && u(4)==map.height && count*u(2)<=128U*1024U*1024U);const auto bytes=read(arg(1),count*u(2)*4);std::vector<float> values(count*u(2));std::memcpy(values.data(),bytes.data(),bytes.size());map.apply(values,u(2));memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(values.data()),bytes.size()));
                }else if(method=="ApplyGuttersPRT"){
                    auto prt=get(arg(1),Kind::PRT);require(prt->flags && prt->viewport[0]==map.width && prt->viewport[1]==map.height && !prt->mapped);std::vector<float> values(prt->pixels.size()/4);std::memcpy(values.data(),prt->pixels.data(),prt->pixels.size());map.apply(values,prt->height*prt->format);std::memcpy(prt->pixels.data(),values.data(),prt->pixels.size());
                }else if(method=="ApplyGuttersTex"){
                    auto texture=get(arg(1),Kind::Texture);require(texture->width==map.width && texture->height==map.height);auto values=read_float_texture(texture);map.apply(values,4);write_float_texture(texture,values);
                }else if(method=="ResampleTex"){
                    auto source=get(arg(1),Kind::Texture),mesh=get(arg(2),Kind::Mesh),target=get(arg(5),Kind::Texture);require(target->width==map.width && target->height==map.height && mesh->height==map.face_count);
                    const auto uv=positions(*mesh,u(3),u(4),2);const auto indices=mesh_indices(*mesh);const auto from=read_float_texture(source);auto to=read_float_texture(target);
                    for(std::size_t i=0;i<count;++i)if(map.kinds[i]){
                        require(map.faces[i]<mesh->height);const auto at=map.faces[i]*3;const std::array<float,3> weights{map.bary[i][0],map.bary[i][1],1-map.bary[i][0]-map.bary[i][1]};double x=0,y=0;for(unsigned c=0;c<3;++c){x+=weights[c]*uv[indices[at+c]][0];y+=weights[c]*uv[indices[at+c]][1];}
                        require(std::isfinite(x) && std::isfinite(y));x=std::clamp(x*source->width-.5,0.,double(source->width-1));y=std::clamp(y*source->height-.5,0.,double(source->height-1));const auto ix=static_cast<unsigned>(std::floor(x)),iy=static_cast<unsigned>(std::floor(y));
                        for(unsigned channel=0;channel<4;++channel){double value=0;for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){const auto px=std::min(ix+dx,source->width-1),py=std::min(iy+dy,source->height-1);value+=(dx?x-ix:1-(x-ix))*(dy?y-iy:1-(y-iy))*from[(static_cast<std::size_t>(py)*source->width+px)*4+channel];}to[i*4+channel]=static_cast<float>(value);}
                    }
                    write_float_texture(target,to);
                }else require(false);
            }
        }
