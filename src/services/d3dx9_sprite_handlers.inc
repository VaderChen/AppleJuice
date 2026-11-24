        // Sprite 經共用 D3D9 DrawPrimitiveUP 繪製；字型共用既有 GDI 排版與原生字形光柵化。
        bool sprite_handled=symbol.starts_with("Sprite.") || symbol.starts_with("Font.") || symbol.starts_with("Line.") || symbol=="D3DXCreateSprite" || symbol=="D3DXCreateLine" || symbol.starts_with("D3DXCreateFont");
        std::vector<std::uint64_t> scratch_allocations;
        const auto scratch_release=[&](){for(auto pointer:scratch_allocations)(void)free_heap(memory,pointer,state);};
        struct ScratchCleanup {const decltype(scratch_release)& cleanup;~ScratchCleanup(){cleanup();}} scratch_cleanup{scratch_release};
        const auto temporary=[&](std::span<const std::uint8_t> bytes){const auto pointer=allocate_heap(memory,std::max<std::size_t>(bytes.size(),8));require(pointer!=0,out_of_memory);scratch_allocations.push_back(pointer);if(!bytes.empty())memory.write(GuestAddress{pointer},bytes);return pointer;};
        const auto local_call=[&](std::string_view family,std::string_view name,std::span<const std::uint64_t> arguments){
            auto local=state;const auto stack=temporary(std::vector<std::uint8_t>(128))+8;local.reg(Gpr::Rsp)=stack;
            if(x86_){for(unsigned i=0;i<arguments.size();++i)memory.store(GuestAddress{stack+4+i*4},32,arguments[i]);}
            else {constexpr std::array registers{Gpr::Rcx,Gpr::Rdx,Gpr::R8,Gpr::R9};for(unsigned i=0;i<arguments.size();++i)if(i<4)local.reg(registers[i])=arguments[i];else memory.store(GuestAddress{stack+40+(i-4)*8},64,arguments[i]);}
            const WindowsServiceDescriptor descriptor{family,name,static_cast<unsigned>(arguments.size())};if(family=="user32.dll")(void)invoke_gdi_text(local,memory,descriptor);else (void)invoke_d3d9(local,memory,descriptor);return static_cast<std::uint32_t>(local.reg(Gpr::Rax));
        };
        const auto device_call=[&](const std::shared_ptr<Object>& device,std::string_view name,std::initializer_list<std::uint64_t> values){std::vector<std::uint64_t> arguments{device->root};arguments.insert(arguments.end(),values);const auto hr=local_call("internal-d3d9",name,arguments);require(!(hr&0x80000000U),hr);};
        const auto sprite_begin=[&](const std::shared_ptr<Object>& sprite,unsigned flags){
            require(!sprite->scene && !(flags&~511U) && (flags&192)!=192);auto device=get(sprite->parent,Kind::Device);require(device->scene);
            if(!(flags&1)){sprite->saved=std::make_shared<Object>(*device);for(auto pointer:saved_bindings(*sprite->saved))if(pointer)hold(bridge.objects.at(pointer));}
            sprite->flags=flags;sprite->scene=true;
            if(!(flags&2)){
                device->stream_frequency.fill(1);
                for(auto& shader:device->shaders)drop_binding(shader);
                device->render[52]=0;device->render[7]=0;device->render[14]=0;device->render[15]=1;device->render[24]=0;device->render[25]=5;device->render[27]=1;device->render[19]=5;device->render[20]=6;device->render[22]=1;device->render[28]=0;device->render[29]=0;device->render[137]=0;device->render[168]=15;device->render[171]=1;device->render[206]=0;
                device->stage[1]=4;device->stage[2]=2;device->stage[3]=1;device->stage[4]=4;device->stage[5]=2;device->stage[6]=1;device->sampler[1]=3;device->sampler[2]=3;device->sampler[5]=3;device->sampler[6]=3;device->sampler[7]=2;device->sampler[10]=16;
            }
            if(!(flags&4)){device->transforms[256]=identity();device->transforms[2]=identity();const auto& v=device->viewport;Matrix projection{};projection[0]=2/static_cast<float>(v[2]);projection[5]=-2/static_cast<float>(v[3]);projection[10]=1;projection[12]=-1-1/static_cast<float>(v[2]);projection[13]=1+1/static_cast<float>(v[3]);projection[15]=1;device->transforms[3]=projection;}
        };
        const auto sprite_restore=[&](const std::shared_ptr<Object>& sprite){
            if(!sprite->saved)return;auto device=get(sprite->parent,Kind::Device),saved=std::move(sprite->saved);unbind(*device);
            for(auto pointer:saved_bindings(*saved))if(pointer)++bridge.objects.at(pointer)->bound;
            device->shaders=saved->shaders;device->vertex_declaration=saved->vertex_declaration;device->texture=saved->texture;device->extra_textures=saved->extra_textures;device->vertex_buffer=saved->vertex_buffer;device->index_buffer=saved->index_buffer;
            device->extra_streams=saved->extra_streams;device->stream_frequency=saved->stream_frequency;
            device->stream_offset=saved->stream_offset;device->stream_stride=saved->stream_stride;device->fvf=saved->fvf;device->render=saved->render;device->sampler=saved->sampler;device->stage=saved->stage;device->transforms=saved->transforms;
            for(auto pointer:saved_bindings(*saved))if(pointer)release(pointer);
        };
        const auto sprite_enqueue=[&](const std::shared_ptr<Object>& sprite,std::uint64_t texture_pointer,std::array<std::int32_t,4> rect,Vector center,Vector position,unsigned tint){
            require(sprite->scene);auto texture=get(texture_pointer,Kind::Texture);require(texture->parent==sprite->parent && rect[2]>=rect[0] && rect[3]>=rect[1]);SpriteItem item;item.texture=texture_pointer;item.color=tint;
            constexpr std::array<std::array<unsigned,2>,6> corners{{{0,0},{1,0},{0,1},{0,1},{1,0},{1,1}}};
            for(unsigned i=0;i<6;++i){const auto x=corners[i][0],y=corners[i][1];Vector vertex{static_cast<float>(rect[2]-rect[0])*static_cast<float>(x)-center[0],static_cast<float>(rect[3]-rect[1])*static_cast<float>(y)-center[1],-center[2],1};
                if(sprite->flags&8){Vector rotated{};rotated[3]=1;for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)rotated[r]+=vertex[c]*sprite->sprite_view[r*4+c];vertex=rotated;}
                for(unsigned c=0;c<3;++c)vertex[c]+=position[c];item.positions[i]=transform(vertex,sprite->sprite_transform);item.uv[i]={static_cast<float>(rect[x?2:0])/static_cast<float>(texture->width),static_cast<float>(rect[y?3:1])/static_cast<float>(texture->height)};
            }
            if(!(sprite->flags&256)){hold(texture);item.retained=true;}sprite->sprites.push_back(item);
        };
        const auto sprite_flush=[&](const std::shared_ptr<Object>& sprite){
            require(sprite->scene);auto device=get(sprite->parent,Kind::Device);require(device->scene);auto& items=sprite->sprites;
            d3dx_sort_sprites(items,(sprite->flags&32)!=0,(sprite->flags&192)!=0,(sprite->flags&64)!=0,[&](const auto& item){auto p=transform(transform(item.positions[0],sprite->sprite_world),sprite->sprite_view);return sprite->sprite_rh?-p[2]:p[2];});
            if(const auto* frame=std::getenv("X64ARM_D3D9_FONT_CAPTURE_FRAME");frame && device->frames==std::strtoull(frame,nullptr,10)){
                std::fprintf(stderr,"[d3d9-sprite-order] flags=%u count=%zu\n",sprite->flags,items.size());
                for(const auto& item:items)std::fprintf(stderr,"[d3d9-sprite-item] texture=%llu position=%.9g,%.9g color=%08x\n",static_cast<unsigned long long>(item.texture),item.positions[0][0],item.positions[0][1],item.color);
            }
            for(const auto& item:items){std::vector<std::uint8_t> vertices(6*24);for(unsigned i=0;i<6;++i){for(unsigned c=0;c<3;++c)put(vertices,i*24+c*4,bits(item.positions[i][c]));put(vertices,i*24+12,item.color);put(vertices,i*24+16,bits(item.uv[i][0]));put(vertices,i*24+20,bits(item.uv[i][1]));}
                device_call(device,"Device.SetTexture",{0,item.texture});device_call(device,"Device.SetFVF",{0x142});const auto pointer=temporary(vertices);device_call(device,"Device.DrawPrimitiveUP",{4,2,pointer,24});
            }
            for(const auto& item:items)if(item.retained)release(item.texture);items.clear();
        };
        const auto sprite_end=[&](const std::shared_ptr<Object>& sprite){sprite_flush(sprite);sprite_restore(sprite);sprite->scene=false;};
        // ID3DXLine：沿用 Sprite 的狀態保存／還原與 2D 繪製狀態，頂點為螢幕座標（XYZRHW＋DIFFUSE），經 DrawPrimitiveUP 繪製。
        // 線寬展開成垂直於線段的四邊形；圖樣每一位元代表 pattern scale 個像素，沿整條折線連續推進；GL 線段且寬度 ≤ 1 時改畫線段。
        // 未呼叫 Begin 的 Draw 自行 Begin／End。
        const auto line_begin=[&](const std::shared_ptr<Object>& line){
            sprite_begin(line,0);auto device=get(line->parent,Kind::Device);
            // 沒有貼圖：色彩與 alpha 直接取頂點色（SELECTARG1＋DIFFUSE）。
            device->stage[1]=2;device->stage[2]=0;device->stage[4]=2;device->stage[5]=0;
            device_call(device,"Device.SetTexture",{0,0});
        };
        const auto line_end=[&](const std::shared_ptr<Object>& line){require(line->scene);sprite_restore(line);line->scene=false;};
        using LinePoint=std::array<float,4>;
        const auto line_draw=[&](const std::shared_ptr<Object>& line,const std::vector<std::vector<LinePoint>>& strips,std::uint32_t color){
            const bool implicit=!line->scene;if(implicit)line_begin(line);
            auto device=get(line->parent,Kind::Device);
            const bool segments=line->line_gl && line->line_width<=1;
            const float half=std::max(line->line_width,0.f)/2;
            const double scale=line->line_pattern_scale>0?static_cast<double>(line->line_pattern_scale):1.0;
            std::vector<std::uint8_t> vertices;
            const auto emit=[&](const LinePoint& point){const auto at=vertices.size();vertices.resize(at+20);for(unsigned c=0;c<4;++c)put(vertices,at+c*4,bits(point[c]));put(vertices,at+16,color);};
            const auto lerp=[](const LinePoint& a,const LinePoint& b,double t){LinePoint point{};for(unsigned c=0;c<4;++c)point[c]=a[c]+(b[c]-a[c])*static_cast<float>(t);return point;};
            const auto piece=[&](const LinePoint& a,const LinePoint& b,float nx,float ny){
                if(segments){emit(a);emit(b);return;}
                auto a0=a,a1=a,b0=b,b1=b;a0[0]+=nx;a0[1]+=ny;a1[0]-=nx;a1[1]-=ny;b0[0]+=nx;b0[1]+=ny;b1[0]-=nx;b1[1]-=ny;
                emit(a0);emit(a1);emit(b0);emit(b0);emit(a1);emit(b1);
            };
            for(const auto& strip:strips){
                double travelled=0;
                for(std::size_t i=0;i+1<strip.size();++i){
                    const auto& a=strip[i];const auto& b=strip[i+1];const float dx=b[0]-a[0],dy=b[1]-a[1];const float length=std::sqrt(dx*dx+dy*dy);
                    if(!(length>0) || !std::isfinite(length))continue;
                    const float nx=-dy/length*half,ny=dx/length*half;
                    if(line->line_pattern==UINT32_MAX){piece(a,b,nx,ny);travelled+=length;continue;}
                    // 依圖樣格切開；連續的 1 位元合併成同一段，格數上限避免極小比例造成過量頂點。
                    double t=0,run=-1;std::size_t cells=0;
                    while(t<length && cells++<65536){
                        const double cell=std::floor((travelled+t)/scale);
                        const bool on=((line->line_pattern>>static_cast<unsigned>(std::fmod(cell,32.0)))&1U)!=0;
                        double end=(cell+1)*scale-travelled;if(!(end>t))end=t+scale;end=std::min<double>(end,length);
                        if(on && run<0)run=t;
                        if(!on && run>=0){piece(lerp(a,b,run/length),lerp(a,b,t/length),nx,ny);run=-1;}
                        t=end;
                    }
                    if(run>=0)piece(lerp(a,b,run/length),b,nx,ny);
                    travelled+=length;
                }
            }
            if(!vertices.empty()){
                device_call(device,"Device.SetFVF",{0x44});device_call(device,"Device.SetRenderState",{176,line->line_antialias?1U:0U});
                const auto count=vertices.size()/20;const auto pointer=temporary(vertices);
                device_call(device,"Device.DrawPrimitiveUP",{segments?2U:4U,segments?count/2:count/3,pointer,20});
            }
            if(implicit)line_end(line);
        };
        const auto font_format=[&](std::string_view name,const std::array<std::uint64_t,5>& arguments){return local_call("user32.dll",name,arguments);};
        const auto font_texture=[&](const std::shared_ptr<Object>& font,const std::string& key,unsigned width,unsigned height,const std::vector<std::uint8_t>& rgba,std::array<unsigned,2> canvas=std::array<unsigned,2>{}){
            auto device=get(font->parent,Kind::Device);const bool padded=canvas[0] && canvas!=std::array<unsigned,2>{width,height};
            // GPU 字型貼圖採與既有固定管線上傳相同的 RGBA8 格式，避免換格式改變取樣進位。
            auto texture=make_texture(device,padded?canvas[0]:width,padded?canvas[1]:height,1,padded?1:0,padded?32:21,padded?0:1);
            if(padded)d3dx_font_upload_padded(*device->native,texture->native_view,width,height,rgba);
            else {auto surface=get(texture->back,Kind::Surface);surface->pixels=rgba;for(std::size_t i=0;i<rgba.size();i+=4)std::swap(surface->pixels[i],surface->pixels[i+2]);write_surface(*surface);}
            if(auto it=font->font_textures.find(key);it!=font->font_textures.end())release(it->second);font->font_textures[key]=texture->root;return texture->root;
        };
        const auto font_glyph=[&](const std::shared_ptr<Object>& font,unsigned glyph){
            require(glyph<=65535);const auto key="Glyph:"+std::to_string(glyph);if(font->font_textures.contains(key))return font->font_textures.at(key);
            const auto model=gui_font(gui_,font->font_handle);const auto face=native_resolve_font(model);require(bool(face));const double em=model.height<0?-static_cast<double>(model.height):model.height?static_cast<double>(model.height)*face->em/std::max(1,face->ascent+face->descent):13;
            NativeGlyphRun run;run.em=static_cast<float>(em);run.indices={static_cast<std::uint16_t>(glyph)};run.advances={0};if(model.width)run.transform[0]=static_cast<float>(std::abs(static_cast<double>(model.width))*face->em/std::max(1,face->average_width)/em);const auto raster=native_font_glyph_raster(*face,run);font->font_bounds[glyph]=raster.bounds;
            const auto width=static_cast<unsigned>(std::max(1,raster.bounds[2]-raster.bounds[0])),height=static_cast<unsigned>(std::max(1,raster.bounds[3]-raster.bounds[1]));std::vector<std::uint8_t> pixels(std::size_t{width}*height*4,255);for(std::size_t i=0;i<std::size_t{width}*height;++i){unsigned alpha=0;for(unsigned c=0;c<raster.channels;++c)if(i*raster.channels+c<raster.alpha.size())alpha+=raster.alpha[i*raster.channels+c];pixels[i*4+3]=static_cast<std::uint8_t>(alpha/std::max(1U,raster.channels));}return font_texture(font,key,width,height,pixels);
        };
        if(sprite_handled){
            if(symbol=="D3DXCreateSprite"){output(1);auto device=get(arg(0),Kind::Device);auto sprite=create(Kind::Sprite,device->root);store_pointer(arg(1),sprite->root);}
            else if(symbol=="D3DXCreateLine"){output(1);auto device=get(arg(0),Kind::Device);auto line=create(Kind::Line,device->root);store_pointer(arg(1),line->root);}
            else if(symbol.starts_with("Sprite.") || symbol.starts_with("Font.") || symbol.starts_with("Line.")){
                const auto kind=symbol.starts_with("Sprite.")?Kind::Sprite:symbol.starts_with("Line.")?Kind::Line:Kind::Font;auto object=get(arg(0),kind);const auto method=symbol.substr(symbol.find('.')+1);
                if(method=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);if(requested!=unknown && requested!=iid(kind))result=no_interface;else {hold(object);store_pointer(arg(2),object->root);}}
                else if(method=="AddRef")result=hold(object);else if(method=="Release")result=release(object->root);
                else if(method=="GetDevice"){output(1);hold(get(object->parent,Kind::Device));store_pointer(arg(1),object->parent);}
                else if(kind==Kind::Sprite){
                    if(method=="GetTransform")memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(object->sprite_transform.data()),64));
                    else if(method=="SetTransform"){auto data=read(arg(1),64);std::memcpy(object->sprite_transform.data(),data.data(),64);}
                    else if(method=="SetWorldViewLH" || method=="SetWorldViewRH"){if(arg(1)){auto data=read(arg(1),64);std::memcpy(object->sprite_world.data(),data.data(),64);}else object->sprite_world=identity();if(arg(2)){auto data=read(arg(2),64);std::memcpy(object->sprite_view.data(),data.data(),64);}else object->sprite_view=identity();object->sprite_rh=method.ends_with("RH");}
                    else if(method=="Begin")sprite_begin(object,u(1));else if(method=="Flush")sprite_flush(object);else if(method=="End")sprite_end(object);
                    else if(method=="Draw"){auto texture=get(arg(1),Kind::Texture);std::array<std::int32_t,4> rect{0,0,static_cast<int>(texture->width),static_cast<int>(texture->height)};if(arg(2)){auto data=read(arg(2),16);std::memcpy(rect.data(),data.data(),16);}Vector center{},position{};if(arg(3)){auto data=read(arg(3),12);std::memcpy(center.data(),data.data(),12);}if(arg(4)){auto data=read(arg(4),12);std::memcpy(position.data(),data.data(),12);}sprite_enqueue(object,arg(1),rect,center,position,u(5));}
                    else if(method=="OnLostDevice"){for(const auto& item:object->sprites)if(item.retained)release(item.texture);object->sprites.clear();sprite_restore(object);object->scene=false;}
                    else if(method=="OnResetDevice"){require(!object->scene);object->sprite_transform=identity();}else require(false);
                }else if(kind==Kind::Line){
                    const auto real_arg=[&](unsigned i){std::uint32_t v=u(i);if(!x86_ && i<4)std::memcpy(&v,state.xmm[i].data(),4);return real(v);};
                    if(method=="Begin"){require(!object->scene);line_begin(object);}
                    else if(method=="End")line_end(object);
                    else if(method=="Draw" || method=="DrawTransform"){
                        // Draw(D3DXVECTOR2*, count, color)：螢幕座標；DrawTransform(D3DXVECTOR3*, count, matrix, color)：乘上矩陣後依視口投影，相機後方的點切斷折線。
                        const bool transformed=method=="DrawTransform";const auto count=u(2);require(count<=(1U<<24) && (!count || arg(1)) && (!transformed || arg(3)));
                        std::vector<std::vector<LinePoint>> strips(1);
                        if(count>=2){
                            const auto data=read(arg(1),std::size_t{count}*(transformed?12U:8U));
                            Matrix matrix=identity();if(transformed){const auto m=read(arg(3),64);std::memcpy(matrix.data(),m.data(),64);}
                            auto device=get(object->parent,Kind::Device);const auto& v=device->viewport;
                            const float vx=static_cast<float>(v[0]),vy=static_cast<float>(v[1]),vw=static_cast<float>(v[2]),vh=static_cast<float>(v[3]),minz=real(v[4]),maxz=real(v[5]);
                            for(std::uint32_t i=0;i<count;++i){
                                const auto coordinate=[&](unsigned c){return real(word(data,std::size_t{i}*(transformed?12U:8U)+c*4));};
                                if(!transformed){strips.back().push_back({coordinate(0),coordinate(1),0,1});continue;}
                                const auto clip=transform(Vector{coordinate(0),coordinate(1),coordinate(2),1},matrix);
                                if(!(clip[3]>0)){if(!strips.back().empty())strips.emplace_back();continue;}
                                const float w=clip[3];
                                strips.back().push_back({vx+(clip[0]/w+1)*0.5f*vw,vy+(1-clip[1]/w)*0.5f*vh,minz+clip[2]/w*(maxz-minz),1/w});
                            }
                            line_draw(object,strips,u(transformed?4:3));
                        }
                    }
                    else if(method=="SetPattern")object->line_pattern=u(1);
                    else if(method=="GetPattern")result=object->line_pattern;
                    else if(method=="SetPatternScale")object->line_pattern_scale=real_arg(1);
                    else if(method=="GetPatternScale"){return_scalar(object->line_pattern_scale,true);result=0;}
                    else if(method=="SetWidth")object->line_width=real_arg(1);
                    else if(method=="GetWidth"){return_scalar(object->line_width,true);result=0;}
                    else if(method=="SetAntialias")object->line_antialias=u(1)!=0;
                    else if(method=="GetAntialias")result=object->line_antialias?1U:0U;
                    else if(method=="SetGLLines")object->line_gl=u(1)!=0;
                    else if(method=="GetGLLines")result=object->line_gl?1U:0U;
                    else if(method=="OnLostDevice"){if(object->scene){sprite_restore(object);object->scene=false;}}
                    else if(method=="OnResetDevice"){}
                    else require(false);
                }else {
#include "d3dx9_font_methods.inc"
                }
            }else {
#include "d3dx9_font_create.inc"
            }
        }
