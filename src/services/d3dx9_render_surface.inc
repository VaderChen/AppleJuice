        const bool render_surface_handled=symbol=="D3DXCreateRenderToSurface" || symbol.starts_with("RenderToSurface.");
        if(render_surface_handled){
            if(symbol=="D3DXCreateRenderToSurface"){
                output(6);auto device=get(arg(0),Kind::Device);require(u(1) && u(2) && u(1)<=16384 && u(2)<=16384 && render_color_format(u(3)) && u(4)<=1 && (!u(4) || depth_format(u(5))));
                auto helper=create(Kind::RenderToSurface,device->root);helper->presentation.resize(20);for(unsigned i=0;i<5;++i)put(helper->presentation,i*4,u(i+1));store_pointer(arg(6),helper->root);
            }else{
                auto helper=get(arg(0),Kind::RenderToSurface),device=get(helper->parent,Kind::Device);const auto method=symbol.substr(symbol.find('.')+1);auto& refs=helper->helper_refs;
                const auto restore=[&](){for(unsigned i=0;i<4;++i)bind_target(device->render_targets[i],refs[4+i]);bind_target(device->depth_target,refs[3]);device->viewport=helper->viewport;for(unsigned i=2;i<8;++i)if(refs[i]){release(refs[i]);refs[i]=0;}helper->scene=false;};
                if(method=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);if(requested!=unknown && requested!=iid(helper->kind))result=no_interface;else {hold(helper);store_pointer(arg(2),helper->root);}}
                else if(method=="AddRef")result=hold(helper);else if(method=="Release")result=release(helper->root);
                else if(method=="GetDevice"){output(1);hold(device);store_pointer(arg(1),device->root);}
                else if(method=="GetDesc"){require(arg(1));memory.write(GuestAddress{arg(1)},helper->presentation);}
                else if(method=="BeginScene"){
                    require(!helper->scene && !device->scene);auto surface=get(arg(1),Kind::Surface);const auto& desc=helper->presentation;require(surface_device(*surface)->root==device->root && !surface->mapped && !surface->multisample && surface->width==word(desc,0) && surface->height==word(desc,4) && surface->format==word(desc,8));
                    const auto output_pointer=temporary(std::vector<std::uint8_t>(ps));
                    if(!(surface->usage&1) && !refs[0]){device_call(device,"Device.CreateRenderTarget",{surface->width,surface->height,surface->format,0,0,0,output_pointer,0});refs[0]=memory.load(GuestAddress{output_pointer},pb);}
                    if(word(desc,12) && !refs[1]){device_call(device,"Device.CreateDepthStencilSurface",{surface->width,surface->height,word(desc,16),0,0,1,output_pointer,0});refs[1]=memory.load(GuestAddress{output_pointer},pb);}
                    helper->viewport=device->viewport;refs[2]=surface->root;hold(surface);refs[3]=device->depth_target;for(unsigned i=0;i<4;++i)refs[4+i]=device->render_targets[i];for(unsigned i=3;i<8;++i)if(refs[i])hold(get(refs[i],Kind::Surface));
                    try{device_call(device,"Device.SetRenderTarget",{0,(surface->usage&1)?surface->root:refs[0]});for(unsigned i=1;i<4;++i)device_call(device,"Device.SetRenderTarget",{i,0});device_call(device,"Device.SetDepthStencilSurface",{refs[1]});if(arg(2))device_call(device,"Device.SetViewport",{arg(2)});device_call(device,"Device.BeginScene",{});helper->scene=true;}
                    catch(...){restore();throw;}
                }else if(method=="EndScene"){
                    require(helper->scene && device->scene);device_call(device,"Device.EndScene",{});
                    try{auto target=get(refs[2],Kind::Surface);if(!(target->usage&1)){auto source=get(refs[0],Kind::Surface);target->pixels=device->native->graphics_read(source->native_view);write_surface(*target);}
                        auto texture=bridge.objects.at(target->parent);if(u(1) && texture_kind(texture->kind)){auto it=std::find(texture->subresources.begin(),texture->subresources.end(),target->root);require(it!=texture->subresources.end());const auto index=static_cast<unsigned>(it-texture->subresources.begin()),level=index%texture->levels;read_surface(*target);auto previous=target;for(unsigned next=level+1;next<texture->levels;++next){auto surface=get(texture->subresources.at(index+next-level),Kind::Surface);surface->pixels=texture_image::transform(previous->pixels,native_format(texture->format),{previous->width,previous->height,1},{0,0,0,previous->width,previous->height,1},native_format(texture->format),{surface->width,surface->height,1},u(1));write_surface(*surface);previous=surface;}}
                    }catch(...){restore();throw;}restore();
                }else if(method=="OnLostDevice"){require(!helper->scene);for(unsigned i=0;i<2;++i)if(refs[i]){release(refs[i]);refs[i]=0;}}
                else if(method=="OnResetDevice"){require(!helper->scene);}else require(false);
            }
        }
