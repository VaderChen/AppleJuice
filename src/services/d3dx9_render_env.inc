        const bool render_env_handled=symbol=="D3DXCreateRenderToEnvMap" || symbol.starts_with("RenderToEnvMap.");
        if(render_env_handled){
            if(symbol=="D3DXCreateRenderToEnvMap"){
                output(6);auto device=get(arg(0),Kind::Device);require(u(1) && u(1)<=16384 && u(2)<=15 && render_color_format(u(3)) && u(4)<=1 && (!u(4) || depth_format(u(5))));
                auto helper=create(Kind::RenderToEnvMap,device->root);helper->presentation.resize(20);for(unsigned i=0;i<5;++i)put(helper->presentation,i*4,u(i+1));store_pointer(arg(6),helper->root);
            }else{
                auto helper=get(arg(0),Kind::RenderToEnvMap),device=get(helper->parent,Kind::Device);const auto method=symbol.substr(symbol.find('.')+1);auto& refs=helper->helper_refs;const auto size=word(helper->presentation,0),format=word(helper->presentation,8);
                const auto finish_face=[&](unsigned filter){if(refs[0]){auto surface_helper=get(refs[0],Kind::RenderToSurface);if(surface_helper->scene)device_call(surface_helper,"RenderToSurface.EndScene",{filter});}};
                const auto clear_targets=[&](){for(unsigned n=1;n<4;++n)if(refs[n]){release(refs[n]);refs[n]=0;}helper->scene=false;};
                if(method=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);if(requested==unknown || requested==iid(helper->kind)){hold(helper);store_pointer(arg(2),helper->root);}else result=no_interface;}
                else if(method=="AddRef")result=hold(helper);else if(method=="Release")result=release(helper->root);
                else if(method=="GetDevice"){output(1);hold(device);store_pointer(arg(1),device->root);}
                else if(method=="GetDesc"){require(arg(1));memory.write(GuestAddress{arg(1)},helper->presentation);}
                else if(method.starts_with("Begin")){
                    require(!helper->scene && !device->scene);const bool cube=method=="BeginCube",sphere=method=="BeginSphere",hemisphere=method=="BeginHemisphere";auto first=get(arg(1),cube?Kind::CubeTexture:Kind::Texture);require(first->parent==device->root && first->width==size && first->height==size && first->format==format);
                    std::shared_ptr<Object> second;if(!cube && !sphere){require(hemisphere || method=="BeginParabolic");second=get(arg(2),Kind::Texture);require(second->parent==device->root && second->width==size && second->height==size && second->format==format);}
                    if(!refs[0]){auto surface_helper=create(Kind::RenderToSurface,device->root);surface_helper->presentation.resize(20);put(surface_helper->presentation,0,size);put(surface_helper->presentation,4,size);for(unsigned n=2;n<5;++n)put(surface_helper->presentation,n*4,word(helper->presentation,n*4));refs[0]=surface_helper->root;}
                    try{if(cube){hold(first);refs[1]=first->root;}else{const auto destination=temporary(std::vector<std::uint8_t>(ps));device_call(device,"Device.CreateCubeTexture",{size,1,0,format,1,destination,0});refs[1]=memory.load(GuestAddress{destination},pb);hold(first);refs[2]=first->root;if(second){hold(second);refs[3]=second->root;}}
                        helper->flags=cube?0U:sphere?1U:hemisphere?2U:3U;helper->scene=true;
                    }catch(...){clear_targets();throw;}
                }else if(method=="Face"){
                    require(helper->scene && u(1)<6);finish_face(u(2));auto cube=get(refs[1],Kind::CubeTexture);auto surface_helper=get(refs[0],Kind::RenderToSurface);device_call(surface_helper,"RenderToSurface.BeginScene",{cube->subresources.at(u(1)*cube->levels),0});
                }else if(method=="End"){
                    require(helper->scene);try{finish_face(u(1));
                        if(helper->flags){auto cube=get(refs[1],Kind::CubeTexture);std::array<std::vector<std::uint8_t>,6> faces;
                            for(unsigned face=0;face<6;++face){auto surface=get(cube->subresources.at(face*cube->levels),Kind::Surface);read_surface(*surface);faces[face]=texture_image::transform(surface->pixels,native_format(format),{size,size,1},{0,0,0,size,size,1},2,{size,size,1},1);}
                            const auto sample=[&](const Vector& d){unsigned face=0;float ucoord=0,vcoord=0;const float ax=std::abs(d[0]),ay=std::abs(d[1]),az=std::abs(d[2]);
                                if(ax>=ay && ax>=az){face=d[0]>=0?0:1;ucoord=(d[0]>=0?-d[2]:d[2])/ax;vcoord=-d[1]/ax;}
                                else if(ay>=az){face=d[1]>=0?2:3;ucoord=d[0]/ay;vcoord=(d[1]>=0?d[2]:-d[2])/ay;}
                                else {face=d[2]>=0?4:5;ucoord=(d[2]>=0?d[0]:-d[0])/az;vcoord=-d[1]/az;}
                                const float fx=std::clamp((ucoord*.5F+.5F)*static_cast<float>(size)-.5F,0.F,static_cast<float>(size-1)),fy=std::clamp((vcoord*.5F+.5F)*static_cast<float>(size)-.5F,0.F,static_cast<float>(size-1));const unsigned x=static_cast<unsigned>(fx),y=static_cast<unsigned>(fy),x1=std::min(x+1,size-1),y1=std::min(y+1,size-1);const float tx=fx-static_cast<float>(x),ty=fy-static_cast<float>(y);Vector color{};
                                for(unsigned channel=0;channel<4;++channel){const auto at=[&](unsigned px,unsigned py){return real(word(faces[face],(std::size_t(py)*size+px)*16+channel*4));};color[channel]=(at(x,y)*(1-tx)+at(x1,y)*tx)*(1-ty)+(at(x,y1)*(1-tx)+at(x1,y1)*tx)*ty;}return color;
                            };
                            for(unsigned side=0;side<(helper->flags==1?1U:2U);++side){auto texture=get(refs[2+side],Kind::Texture),surface=get(texture->subresources.at(0),Kind::Surface);require(!surface->mapped);std::vector<std::uint8_t> rgba(std::size_t(size)*size*16);
                                for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){const float ucoord=2*(static_cast<float>(x)+.5F)/static_cast<float>(size)-1,vcoord=1-2*(static_cast<float>(y)+.5F)/static_cast<float>(size),r2=ucoord*ucoord+vcoord*vcoord;Vector direction{};
                                    if(r2>1)continue;
                                    if(helper->flags==1){const float z=std::sqrt(1-r2);direction={2*ucoord*z,2*vcoord*z,1-2*r2,0};}
                                    else if(helper->flags==2)direction={ucoord,vcoord,(side?-1.F:1.F)*std::sqrt(1-r2),0};
                                    else direction={2*ucoord/(1+r2),2*vcoord/(1+r2),(side?-1.F:1.F)*(1-r2)/(1+r2),0};
                                    const auto color=sample(direction);for(unsigned channel=0;channel<4;++channel)put(rgba,(std::size_t(y)*size+x)*16+channel*4,bits(color[channel]));
                                }
                                surface->pixels=texture_image::transform(rgba,2,{size,size,1},{0,0,0,size,size,1},native_format(format),{size,size,1},1);write_surface(*surface);
                                if(u(1))for(unsigned level=1;level<texture->levels;++level){auto next=get(texture->subresources.at(level),Kind::Surface);require(!next->mapped);next->pixels=texture_image::transform(surface->pixels,native_format(format),{surface->width,surface->height,1},{0,0,0,surface->width,surface->height,1},native_format(format),{next->width,next->height,1},u(1));write_surface(*next);surface=next;}
                            }
                        }
                    }catch(...){clear_targets();throw;}clear_targets();
                }else if(method=="OnLostDevice"){require(!helper->scene);if(refs[0]){release(refs[0]);refs[0]=0;}}
                else if(method=="OnResetDevice"){require(!helper->scene);}else require(false);
            }
        }
