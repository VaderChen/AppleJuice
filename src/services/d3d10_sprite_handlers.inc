        // 呼叫既有 GDI 排版服務，共用換行、雙向文字、裁切與量測語義。
        const auto font_format=[&](std::string_view name,const std::array<std::uint64_t,5>& arguments){
            auto local=state;const auto stack=temporary(std::vector<std::uint8_t>(72))+8;local.reg(Gpr::Rsp)=stack;
            if(x86_){for(unsigned i=0;i<arguments.size();++i)memory.store(GuestAddress{stack+4+i*4},32,arguments[i]);}
            else {constexpr std::array registers{Gpr::Rcx,Gpr::Rdx,Gpr::R8,Gpr::R9};for(unsigned i=0;i<4;++i)local.reg(registers[i])=arguments[i];memory.store(GuestAddress{stack+40},64,arguments[4]);}
            const WindowsServiceDescriptor descriptor{"user32.dll",name,5};(void)invoke_gdi_text(local,memory,descriptor);return static_cast<std::uint32_t>(local.reg(Gpr::Rax));
        };
        const auto sprite_context=[&](const std::shared_ptr<Identity>& sprite){return object(object(sprite->device).identity->context10).identity;};
        const auto sprite_prepare=[&](const std::shared_ptr<Identity>& sprite){
            if(sprite->bindings.contains("SpriteVS"))return;auto device=object(sprite->device).identity;
            static const std::string source=R"(struct V{float4 p:POSITION;float3 uv:TEXCOORD;float4 c:COLOR;};struct P{float4 p:SV_Position;float3 uv:TEXCOORD;float4 c:COLOR;};P VS(V v){P p;p.p=v.p;p.uv=v.uv;p.c=v.c;return p;}Texture2DArray tex:register(t0);SamplerState samp:register(s0);float4 PS(P p):SV_Target{return tex.Sample(samp,p.uv)*p.c;})";
            static const auto vertex=graphics_compile_hlsl(std::span(reinterpret_cast<const std::uint8_t*>(source.data()),source.size()),"D3DX10Sprite","VS","vs_4_0",{},0);
            static const auto pixel=graphics_compile_hlsl(std::span(reinterpret_cast<const std::uint8_t*>(source.data()),source.size()),"D3DX10Sprite","PS","ps_4_0",{},0);
            require(!vertex.bytecode.empty() && !pixel.bytecode.empty());
            for(unsigned stage=0;stage<2;++stage){const auto& code=stage?pixel.bytecode:vertex.bytecode;auto shader=create(stage?Kind::PixelShader:Kind::VertexShader,sprite->device);sprite->bindings[stage?"SpritePS":"SpriteVS"]={shader->root};shader->shader=device->native->graphics_shader(stage,code);shader->reflection=std::make_shared<dxbc::Reflection>(code);}
            auto layout=create(Kind::Layout,sprite->device);layout->layout={{"POSITION",0,2,0,0,0,0},{"TEXCOORD",0,6,0,16,0,0},{"COLOR",0,2,0,28,0,0}};sprite->bindings["SpriteLayout"]={layout->root};
            const auto make_state=[&](Kind kind,const char* name,unsigned size){auto id=create(kind,sprite->device);id->desc.resize(size);sprite->bindings[name]={id->root};return id;};
            auto sampler=make_state(Kind::Sampler,"SpriteSampler",52);put_word(sampler->desc,0,0x15);for(unsigned at:{4U,8U,12U})put_word(sampler->desc,at,3);put_word(sampler->desc,20,1);put_word(sampler->desc,24,1);put_word(sampler->desc,48,0x7f7fffff);sampler->sampler=device->native->graphics_sampler(sampler->desc);
        };
        const auto sprite_begin=[&](const std::shared_ptr<Identity>& sprite,unsigned flags){
            require(!sprite->sprite.active && !(flags&~31U) && (flags&6)!=6);sprite_prepare(sprite);auto context=sprite_context(sprite);
            if(flags&8){sprite->sprite_saved=std::make_shared<Identity>();auto& saved=*sprite->sprite_saved;saved.bindings=context->bindings;saved.strides=context->strides;saved.offsets=context->offsets;saved.topology=context->topology;saved.blend_factor=context->blend_factor;saved.sample_mask=context->sample_mask;saved.stencil_reference=context->stencil_reference;for(const auto& [key,values]:sprite->sprite_saved->bindings){(void)key;for(auto pointer:values)if(pointer)hold(pointer);}}
            sprite->sprite.flags=flags;sprite->sprite.active=true;
        };
        const auto sprite_flush=[&](const std::shared_ptr<Identity>& sprite){
            require(sprite->sprite.active);auto context=sprite_context(sprite);auto& items=sprite->sprite.items;const auto flags=sprite->sprite.flags;
            d3dx_sort_sprites(items,(flags&7)!=0,(flags&6)!=0,(flags&2)==0,[&](const auto& item){float z=0;for(unsigned i=0;i<4;++i)z+=item.world[12+i]*sprite->sprite.view[i*4+2];return z;});
            if(!items.empty()){
                // D3DX10 Sprite 使用呼叫端的光柵化、混色與深度狀態；
                // DXUT 會在 Begin 與 Flush 之間自行設定這些狀態。
                for(const auto& pair:std::array<std::pair<const char*,const char*>,5>{{{"VSShader","SpriteVS"},{"PSShader","SpritePS"},{"Layout","SpriteLayout"},{"PSSamplers","SpriteSampler"},{"GSShader",""}}}){const std::array values{*pair.second?binding(sprite,pair.second):0};bind(context,pair.first,0,values,std::string_view(pair.first)=="PSSamplers"?16:1);}
                context->topology=4;
            }
            for(const auto& item:items){
                require(item.texture);auto texture=typed(item.texture,Kind::ShaderResource);require(texture->device==sprite->device);auto view=view_of(item.texture);require(view.resource->desc.dimension==3 && item.slice<view.slices);view.dimension=5;
                struct Vertex{std::array<float,4> p;std::array<float,3> uv;std::array<float,4> color;};std::array<Vertex,6> vertices;
                constexpr std::array<std::array<float,2>,6> corners{{{-.5f,.5f},{.5f,.5f},{-.5f,-.5f},{-.5f,-.5f},{.5f,.5f},{.5f,-.5f}}};
                for(unsigned i=0;i<6;++i){auto p=std::array<float,4>{corners[i][0],corners[i][1],0,1};for(const auto* matrix:std::array<const std::array<float,16>*,3>{&item.world,&sprite->sprite.view,&sprite->sprite.projection}){std::array<float,4> next{};for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)next[c]+=p[r]*(*matrix)[r*4+c];p=next;}vertices[i]={p,{item.uv[0]+(corners[i][0]+.5f)*item.size[0],item.uv[1]+(.5f-corners[i][1])*item.size[1],static_cast<float>(item.slice)},item.color};}
                GraphicsResourceDesc desc;desc.width=sizeof(vertices);desc.bind=1;auto buffer=create(Kind::BufferObject,sprite->device);buffer->resource=context->native->graphics_resource(desc);buffer->desc.resize(24);put_word(buffer->desc,0,desc.width);put_word(buffer->desc,8,1);context->native->graphics_write(GraphicsView{buffer->resource},std::span(reinterpret_cast<const std::uint8_t*>(vertices.data()),sizeof(vertices)),0,0);
                const std::array pointer{buffer->root};bind(context,"Vertices",0,pointer,16);context->strides[0]=sizeof(Vertex);context->offsets[0]=0;
                const std::array resources{outputs_conflict(context,item.texture)?0:item.texture};bind(context,"PSShaderResources",0,resources,128);auto draw=draw_state(context);draw.stages[1].resources[0]=outputs_conflict(context,item.texture)?GraphicsView{}:view;draw.count=6;submit(context,{[draw=std::move(draw)](auto& native){native.graphics_draw(draw);},1,0});
                release(buffer->root);if(item.retained)release(item.texture);
            }items.clear();
        };
        const auto sprite_end=[&](const std::shared_ptr<Identity>& sprite){
            sprite_flush(sprite);if(sprite->sprite_saved){auto context=sprite_context(sprite),saved=std::move(sprite->sprite_saved);auto previous=std::move(context->bindings);context->bindings=std::move(saved->bindings);context->strides=saved->strides;context->offsets=saved->offsets;context->topology=saved->topology;context->blend_factor=saved->blend_factor;context->sample_mask=saved->sample_mask;context->stencil_reference=saved->stencil_reference;const auto retained=context->weak_bindings?context->bindings:std::move(previous);for(const auto& [key,values]:retained){(void)key;for(auto pointer:values)if(pointer)release(pointer);}}sprite->sprite.active=false;
        };
        if(!graphics_handled && (symbol=="D3DX10CreateSprite" || symbol.starts_with("Sprite10"))){
            graphics_handled=true;
            if(symbol=="D3DX10CreateSprite"){auto device=typed(arg(0),Kind::Device);require(device->api10 && arg(2));write_pointer(arg(2),0);auto sprite=create(Kind::Sprite10,device->root);sprite->sprite.capacity=u32(1)?u32(1):512;write_pointer(arg(2),sprite->root);}
            else {auto sprite=typed(arg(0),Kind::Sprite10);
                if(symbol=="Sprite10Begin")sprite_begin(sprite,u32(1));else if(symbol=="Sprite10End")sprite_end(sprite);else if(symbol=="Sprite10Flush")sprite_flush(sprite);
                else if(symbol=="Sprite10GetView" || symbol=="Sprite10GetProjection" || symbol=="Sprite10SetView" || symbol=="Sprite10SetProjection"){auto& matrix=symbol.ends_with("View")?sprite->sprite.view:sprite->sprite.projection;const auto bytes=std::span(reinterpret_cast<std::uint8_t*>(matrix.data()),64);if(symbol.starts_with("Sprite10Get"))memory.write(GuestAddress{arg(1)},bytes);else memory.read(GuestAddress{arg(1)},bytes);}
                else {const bool immediate=symbol=="Sprite10Immediate";require(sprite->sprite.active && u32(2)<=1048576);const unsigned size=x86_?104:112,stride=immediate && u32(3)?u32(3):size;require(stride>=size);if(immediate)require(!u32(4));
                    for(unsigned i=0;i<u32(2);++i){const auto bytes=read_bytes(memory,arg(1)+std::uint64_t{i}*stride,size);D3D10SpriteItem item;std::memcpy(item.world.data(),bytes.data(),64);std::memcpy(item.uv.data(),bytes.data()+64,8);std::memcpy(item.size.data(),bytes.data()+72,8);std::memcpy(item.color.data(),bytes.data()+80,16);item.texture=x86_?word(bytes,96):static_cast<std::uint64_t>(word(bytes,96))|(static_cast<std::uint64_t>(word(bytes,100))<<32);item.slice=word(bytes,96+pointer_size);if(sprite->sprite.flags&16){hold(item.texture);item.retained=true;}sprite->sprite.items.push_back(item);if(sprite->sprite.items.size()>=sprite->sprite.capacity)sprite_flush(sprite);}if(immediate)sprite_flush(sprite);
                }
            }
        }
        const auto font_texture=[&](const std::shared_ptr<Identity>& font,const std::string& key,unsigned width,unsigned height,const std::vector<std::uint8_t>& pixels,std::array<unsigned,2> canvas=std::array<unsigned,2>{}){
            auto device=object(font->device).identity;const bool padded=canvas[0] && canvas!=std::array<unsigned,2>{width,height};GraphicsResourceDesc desc;desc.dimension=3;desc.width=padded?canvas[0]:width;desc.height=padded?canvas[1]:height;desc.format=28;desc.bind=padded?0x28:8;auto resource=device->native->graphics_resource(desc);
            if(padded)d3dx_font_upload_padded(*device->native,GraphicsView{resource,28},width,height,pixels);else device->native->graphics_write(GraphicsView{resource,28},pixels,width*4,width*height*4);
            auto texture=create(Kind::Texture,font->device);texture->resource=resource;texture->texture_desc={desc.width,desc.height,1,1,28,1,0,0,8,0,0};texture->desc.resize(44);for(unsigned i=0;i<11;++i)put_word(texture->desc,i*4,texture->texture_desc[i]);auto view=create(Kind::ShaderResource,font->device);view->texture=texture->root;view->graphics_view={resource,28,0,1,0,1};view->graphics_view.dimension=5;view->desc.resize(24);put_word(view->desc,0,28);put_word(view->desc,4,5);put_word(view->desc,12,1);put_word(view->desc,20,1);font->bindings[key]={view->root};return view->root;
        };
        const auto font_glyph=[&](const std::shared_ptr<Identity>& font,unsigned glyph){
            require(glyph<=65535);const auto key="Glyph:"+std::to_string(glyph);if(font->bindings.contains(key))return binding(font,key);
            const auto model=gui_font(gui_,font->font_handle);const auto face=native_resolve_font(model);require(bool(face));const double em=model.height<0?-static_cast<double>(model.height):model.height?static_cast<double>(model.height)*face->em/std::max(1,face->ascent+face->descent):13;
            NativeGlyphRun run;run.em=static_cast<float>(em);run.indices={static_cast<std::uint16_t>(glyph)};run.advances={0};if(model.width)run.transform[0]=static_cast<float>(std::abs(static_cast<double>(model.width))*face->em/std::max(1,face->average_width)/em);const auto raster=native_font_glyph_raster(*face,run);font->font_glyph_bounds[glyph]=raster.bounds;
            const auto width=static_cast<unsigned>(std::max(1,raster.bounds[2]-raster.bounds[0])),height=static_cast<unsigned>(std::max(1,raster.bounds[3]-raster.bounds[1]));std::vector<std::uint8_t> pixels(std::size_t{width}*height*4,255);for(std::size_t i=0;i<std::size_t{width}*height;++i){unsigned alpha=0;for(unsigned c=0;c<raster.channels;++c)if(i*raster.channels+c<raster.alpha.size())alpha+=raster.alpha[i*raster.channels+c];pixels[i*4+3]=static_cast<std::uint8_t>(alpha/std::max(1U,raster.channels));}return font_texture(font,key,width,height,pixels);
        };
        if(!graphics_handled && (symbol.starts_with("D3DX10CreateFont") || symbol.starts_with("Font10"))){
            graphics_handled=true;
            if(symbol.starts_with("D3DX10CreateFont")){
                const bool indirect=symbol.find("Indirect")!=symbol.npos,wide=symbol.ends_with("W");const auto out=indirect?2U:11U;require(arg(out));write_pointer(arg(out),0);auto device=typed(arg(0),Kind::Device);require(device->api10);std::vector<std::uint8_t> desc(88);
                if(indirect){const auto source=read_bytes(memory,arg(1),wide?88:56);std::copy_n(source.begin(),24,desc.begin());if(wide)std::copy(source.begin()+24,source.end(),desc.begin()+24);else {const auto end=std::find(source.begin()+24,source.end(),0);const auto name=utf8_to_utf16_lossy(std::string(source.begin()+24,end));for(unsigned i=0;i<std::min<std::size_t>(31,name.size());++i){desc[24+i*2]=static_cast<std::uint8_t>(name[i]);desc[25+i*2]=static_cast<std::uint8_t>(name[i]>>8);}}}
                else {for(unsigned i=0;i<5;++i)put_word(desc,i*4,u32(i+1));for(unsigned i=0;i<4;++i)desc[20+i]=static_cast<std::uint8_t>(u32(i+6));const auto name=arg(10)?(wide?read_utf16(memory,GuestAddress{arg(10)}):utf8_to_utf16_lossy(read_ansi(memory,arg(10)))):std::u16string{};for(unsigned i=0;i<std::min<std::size_t>(31,name.size());++i){desc[24+i*2]=static_cast<std::uint8_t>(name[i]);desc[25+i*2]=static_cast<std::uint8_t>(name[i]>>8);}}
                require(std::abs(static_cast<std::int64_t>(static_cast<std::int32_t>(word(desc,0))))<=4096 && word(desc,4)<=4096);auto font=create(Kind::Font10,device->root);font->desc=desc;font->font_handle=gui_.next_object++;font->font_dc=gui_.next_object++;
                std::vector<std::uint8_t> logical(92);put_word(logical,0,word(desc,0));put_word(logical,4,word(desc,4));put_word(logical,16,word(desc,8));logical[20]=word(desc,16)!=0;logical[23]=desc[20];logical[24]=desc[21];logical[26]=desc[22];logical[27]=desc[23];std::copy(desc.begin()+24,desc.end(),logical.begin()+28);gui_.fonts[font->font_handle]=std::move(logical);auto& dc=gui_.contexts[font->font_dc];dc.font=font->font_handle;dc.mode=1;dc.color=0xffffff;write_pointer(arg(out),font->root);
            }else {
                auto font=typed(arg(0),Kind::Font10);const auto model=gui_font(gui_,font->font_handle);const auto metrics=native_gui_?native_gui_->font_metrics(model).value_or(fallback_font_metrics(model)):fallback_font_metrics(model);
                if(symbol=="Font10DC")result=font->font_dc;
                else if(symbol=="Font10DescW")memory.write(GuestAddress{arg(1)},font->desc);
                else if(symbol=="Font10DescA"){std::vector<std::uint8_t> desc(56);std::copy_n(font->desc.begin(),24,desc.begin());const auto name=utf16_to_utf8(model.face);std::copy_n(name.begin(),std::min<std::size_t>(31,name.size()),desc.begin()+24);memory.write(GuestAddress{arg(1)},desc);}
                else if(symbol.starts_with("Font10Metrics")){
                    const bool wide=symbol.ends_with("W");std::vector<std::uint8_t> bytes(wide?60:56);for(const auto& [offset,value]:std::array<std::pair<unsigned,int>,10>{{{0,metrics.height},{4,metrics.ascent},{8,metrics.descent},{16,metrics.external_leading},{20,metrics.average_width},{24,metrics.maximum_width},{28,model.weight},{32,0},{36,96},{40,96}}})put_word(bytes,offset,static_cast<unsigned>(value));const unsigned chars=wide?2:1;bytes[44]=32;bytes[44+chars]=255;if(wide)bytes[45+chars]=255;bytes[44+chars*2]='?';bytes[44+chars*3]=' ';const auto at=44+chars*4;bytes[at]=model.italic;bytes[at+3]=static_cast<std::uint8_t>((model.pitch_family&0xf0)|4|(metrics.fixed_pitch?0:1));bytes[at+4]=model.charset;memory.write(GuestAddress{arg(1)},bytes);result=1;
                }else if(symbol=="Font10Glyph"){
                    const auto glyph=u32(1);const auto pointer=font_glyph(font,glyph);if(arg(2)){hold(pointer);write_pointer(arg(2),pointer);}const auto bounds=font->font_glyph_bounds.at(glyph);if(arg(3)){memory.fill(GuestAddress{arg(3)},0,16);memory.store(GuestAddress{arg(3)+8},32,static_cast<unsigned>(std::max(0,bounds[2]-bounds[0])));memory.store(GuestAddress{arg(3)+12},32,static_cast<unsigned>(std::max(0,bounds[3]-bounds[1])));}if(arg(4)){memory.store(GuestAddress{arg(4)},32,static_cast<unsigned>(bounds[0]));memory.store(GuestAddress{arg(4)+4},32,static_cast<unsigned>(metrics.ascent+bounds[1]));}
                }else if(symbol.starts_with("Font10Preload")){
                    std::vector<std::uint32_t> characters;std::vector<std::uint16_t> glyphs;
                    if(symbol=="Font10PreloadCharacters" || symbol=="Font10PreloadGlyphs"){require(u32(1)<=u32(2) && u32(2)<=65535);for(unsigned i=u32(1);i<=u32(2);++i)if(symbol=="Font10PreloadGlyphs")glyphs.push_back(static_cast<std::uint16_t>(i));else characters.push_back(i);}
                    else {const auto count=static_cast<std::int32_t>(u32(2));require(count>=-1 && count<=65536);std::u16string text;if(symbol.ends_with("W")){if(count<0)text=read_utf16(memory,GuestAddress{arg(1)});else for(int i=0;i<count;++i)text.push_back(static_cast<char16_t>(memory.load(GuestAddress{arg(1)+static_cast<unsigned>(i)*2},16)));}else {std::string text8;if(count<0)text8=read_ansi(memory,arg(1));else {const auto bytes=read_bytes(memory,arg(1),static_cast<unsigned>(count));text8.assign(bytes.begin(),bytes.end());}text=utf8_to_utf16_lossy(text8);}for(unsigned i=0;i<text.size();++i){unsigned cp=text[i];if(cp>=0xd800 && cp<=0xdbff && i+1<text.size() && text[i+1]>=0xdc00 && text[i+1]<=0xdfff)cp=0x10000+((cp-0xd800)<<10)+(text[++i]-0xdc00);characters.push_back(cp);}}
                    if(!characters.empty()){const auto face=native_resolve_font(model);require(bool(face));glyphs=native_font_glyph_indices(*face,characters);}for(auto glyph:glyphs)(void)font_glyph(font,glyph);
                }else if(symbol=="Font10DrawA" || symbol=="Font10DrawW"){
                    require(arg(4));const auto flags=u32(5);const std::array<std::uint64_t,5> call{font->font_dc,arg(2),arg(3),arg(4),flags};const auto text_symbol=symbol.ends_with("W")?"DrawTextW":"DrawTextA";
                    if(flags&0x400){result=font_format(text_symbol,call);}
                    else {
                        auto context=object(object(font->device).identity->context10).identity;require(!context->viewports.empty());const auto viewport=context->viewports[0];auto rect=read_bytes(memory,arg(4),16);const auto left=static_cast<std::int32_t>(word(rect,0)),top=static_cast<std::int32_t>(word(rect,4));const auto right=static_cast<std::int32_t>(word(rect,8)),bottom=static_cast<std::int32_t>(word(rect,12));
                        auto width=static_cast<unsigned>(std::max<std::int64_t>(1,(flags&0x100)?std::max<std::int64_t>(static_cast<int>(viewport[2])-left,static_cast<std::int64_t>(right)-left):static_cast<std::int64_t>(right)-left));auto height=static_cast<unsigned>(std::max<std::int64_t>(1,(flags&0x100)?std::max<std::int64_t>(static_cast<int>(viewport[3])-top,static_cast<std::int64_t>(bottom)-top):static_cast<std::int64_t>(bottom)-top));require(width<=16384 && height<=16384 && std::uint64_t{width}*height<=16777216);
                        const std::array<unsigned,2> canvas{width,height};auto& dc=gui_.contexts.at(font->font_dc);
                        const auto logical_font=gui_.fonts.find(font->font_handle);const auto selected_bitmap=gui_.bitmaps.find(dc.bitmap);
                        const bool cache_allowed=logical_font!=gui_.fonts.end() && !gui_.emf_recorders.contains(font->font_dc) && (selected_bitmap==gui_.bitmaps.end() || !selected_bitmap->second.dib_address);
                        const auto cache_key=cache_allowed?d3dx_font_draw_key(memory,dc,font->font_handle,logical_font->second,reinterpret_cast<std::uintptr_t>(model.native.get()),flags,canvas,rect,symbol.ends_with("W"),arg(2),static_cast<std::int32_t>(arg(3)),true):std::optional<std::string>{};
                        std::vector<std::uint8_t> rgba;
                        if(const auto* cached=font->font_draw_cache.find(cache_key)){
                            width=cached->width;height=cached->height;result=cached->result;rgba=cached->rgba;
                            if(!dc.bitmap)dc.bitmap=gui_.next_object++;auto& bitmap=gui_.bitmaps[dc.bitmap];bitmap.width=width;bitmap.height=height;bitmap.pixels=cached->pixels;bitmap.raw_alpha=true;
                            auto surface=raster_surface(memory,font->font_dc);require(bool(surface));surface->upload(bitmap.pixels,true);dc.color=0xffffff;dc.mode=1;
                        }else{
                            const auto extent=d3dx_font_raster_extent(dc,font->font_handle,flags,metrics,{width,height},[&]{auto measure=call;const auto bounds=temporary(rect);measure[3]=bounds;measure[4]=flags|0x400U;const auto measured=font_format(text_symbol,measure);if(!measured)return std::array<std::int64_t,2>{-1,-1};return std::array<std::int64_t,2>{std::int64_t{static_cast<std::int32_t>(memory.load(GuestAddress{bounds+8},32))}-left,std::int64_t{static_cast<std::int32_t>(memory.load(GuestAddress{bounds+12},32))}-top};});width=extent[0];height=extent[1];
                            if(!dc.bitmap)dc.bitmap=gui_.next_object++;auto& bitmap=gui_.bitmaps[dc.bitmap];bitmap.width=width;bitmap.height=height;bitmap.pixels.assign(std::size_t{width}*height,0xff000000);bitmap.raw_alpha=true;auto surface=raster_surface(memory,font->font_dc);require(bool(surface));surface->upload(bitmap.pixels,true);dc.color=0xffffff;dc.mode=1;
                            put_word(rect,0,0);put_word(rect,4,0);put_word(rect,8,static_cast<unsigned>(right-left));put_word(rect,12,static_cast<unsigned>(bottom-top));const auto scratch=temporary(rect);auto local=call;local[3]=scratch;result=font_format(text_symbol,local);
                            const auto pixels=surface->snapshot().pixels;rgba.assign(pixels.size()*4,255);for(std::size_t i=0;i<pixels.size();++i)rgba[i*4+3]=static_cast<std::uint8_t>(((pixels[i]&255)+((pixels[i]>>8)&255)+((pixels[i]>>16)&255))/3);
                            font->font_draw_cache.remember(cache_key,width,height,result,pixels,rgba);
                        }
                        auto sprite=arg(1)?typed(arg(1),Kind::Sprite10):create(Kind::Sprite10,font->device);require(sprite->device==font->device);const bool owned=!arg(1);if(owned){sprite_begin(sprite,8);sprite->sprite.projection={2/viewport[2],0,0,0,0,2/viewport[3],0,0,0,0,1,0,-1,-1,0,1};}else require(sprite->sprite.active);
                        const auto key="FontDraw";if(const auto previous=binding(font,key)){release(previous);font->bindings.erase(key);}const auto texture=font_texture(font,key,width,height,rgba,canvas);D3D10SpriteItem item;item.world={static_cast<float>(canvas[0]),0,0,0,0,static_cast<float>(canvas[1]),0,0,0,0,1,0,left+canvas[0]*.5f,viewport[3]-top-canvas[1]*.5f,.5f,1};const auto color=read_bytes(memory,arg(6),16);std::memcpy(item.color.data(),color.data(),16);item.texture=texture;hold(texture);item.retained=true;sprite->sprite.items.push_back(item);if(owned){sprite_end(sprite);release(sprite->root);}else if(sprite->sprite.items.size()>=sprite->sprite.capacity)sprite_flush(sprite);
                    }
                }else throw std::invalid_argument("D3DX10 字型方法無效");
            }
        }
