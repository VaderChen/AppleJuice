// 可程式化 OpenGL 狀態與 Metal 轉接；沿用既有 command queue、資源額度及呈現。
void MetalGl::gl_link(GlProgram& p){
    p.linked=false;p.log.clear();std::vector<GlslSource> sources;
    for(const auto& [name,shader]:p.attached){(void)name;if(!shader->compiled){p.log="附加的 shader 尚未編譯成功";return;}sources.push_back(shader->source);}
    if(sources.empty()){p.log="program 沒有 shader";return;}
    auto result=link_glsl(sources);p.log=result.log;if(!result.success)return;
    std::vector<GlProgramStage> stages;
    for(auto& stage:result.stages){GlProgramStage native;NSError* error=nil;
        auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:stage.msl.c_str()] options:nil error:&error];
        if(!library){p.log+=error.localizedDescription.UTF8String;return;}native.function=[library newFunctionWithName:[NSString stringWithUTF8String:stage.entry.c_str()]];
        if(!stage.flipped_msl.empty()){auto flipped=[device_ newLibraryWithSource:[NSString stringWithUTF8String:stage.flipped_msl.c_str()] options:nil error:&error];if(!flipped){p.log+=error.localizedDescription.UTF8String;return;}native.flipped=[flipped newFunctionWithName:[NSString stringWithUTF8String:stage.entry.c_str()]];}
        native.uniforms=stage.initial_uniforms;for(const auto& sampler:stage.samplers)native.units.emplace_back(sampler.count,0);native.reflection=std::move(stage);stages.push_back(std::move(native));
    }
    p.stages=std::move(stages);p.locations.clear();p.uniforms.clear();p.pipelines.clear();
    for(unsigned s=0;s<p.stages.size();++s){auto& stage=p.stages[s];
        const auto add=[&](const std::string& name,unsigned type,unsigned components,unsigned columns,unsigned count,unsigned index,bool sampler){
            for(unsigned e=0;e<count;++e){const auto key=count==1?name:name+"["+std::to_string(e)+"]";auto it=p.locations.find(key);int location;
                if(it==p.locations.end()){location=static_cast<int>(p.uniforms.size());p.locations[key]=location;p.uniforms.push_back({type,components,columns,count-e,{}});}else location=it->second;
                p.uniforms[static_cast<std::size_t>(location)].targets.push_back({s,index,e,sampler});if(e==0)p.locations[name]=location;
            }};
        for(unsigned i=0;i<stage.reflection.uniforms.size();++i){const auto& u=stage.reflection.uniforms[i];if(u.name==stage.reflection.clip_mask || u.name==stage.reflection.point_size_enabled)continue;add(u.name,u.type,u.components,u.columns,u.count,i,false);}
        for(unsigned i=0;i<stage.reflection.samplers.size();++i){const auto& u=stage.reflection.samplers[i];add(u.name,0x8b5e,1,1,u.count,i,true);}
    }p.linked=true;
}
std::optional<std::uint64_t> MetalGl::programmable_execute(GlOperation op,std::span<const std::uint64_t> args,std::span<const std::uint8_t> data,std::span<std::uint8_t> output){
    if(const auto result=vertex_buffers_execute(op,args,data,output))return result;
    const auto u=[&](unsigned i){return static_cast<unsigned>(args[i]);};const auto n=[&](unsigned i){return std::bit_cast<int>(u(i));};
    const auto text=[&](){return std::string(reinterpret_cast<const char*>(data.data()),data.size());};
    const auto write=[&](unsigned value){if(output.size()>=4)std::memcpy(output.data(),&value,4);};
    const auto log=[&](const std::string& value)->std::uint64_t{if(output.empty())return 0;const auto length=std::min(value.size(),output.size()-1);std::memcpy(output.data(),value.data(),length);output[length]=0;return length;};
    const auto shader=[&]()->std::shared_ptr<GlShader>{auto it=gl_.shaders.find(u(0));if(it==gl_.shaders.end()){error(0x501);return {};}return it->second;};
    const auto program=[&]()->std::shared_ptr<GlProgram>{auto it=gl_.programs.find(u(0));if(it==gl_.programs.end()){error(0x501);return {};}return it->second;};
    switch(op){
    case GlOperation::Enable:case GlOperation::Disable:case GlOperation::IsEnabled:if(u(0)==0x884f || (u(0)>=0x3000 && u(0)<0x3008)){if(op==GlOperation::IsEnabled)return state_.enabled.contains(u(0));if(op==GlOperation::Enable)state_.enabled.insert(u(0));else state_.enabled.erase(u(0));return 0;}return std::nullopt;
    case GlOperation::CreateShader:{if(u(0)!=0x8b31 && u(0)!=0x8b30){error(0x500);return 0;}if(gl_.shaders.size()>=4096)throw std::bad_alloc{};const auto id=gl_.next_object++;auto s=std::make_shared<GlShader>();s->source.stage=u(0);gl_.shaders[id]=s;return id;}
    case GlOperation::ShaderSource:if(auto s=shader()){s->source.source=text();s->compiled=false;}return 0;
    case GlOperation::CompileShader:if(auto s=shader()){const auto begin=std::chrono::steady_clock::now();auto result=validate_glsl(s->source);s->compiled=result.success;s->log=std::move(result.log);++gl_.compiles;gl_.compile_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count());}return 0;
    case GlOperation::GetShaderiv:if(auto s=shader()){switch(u(1)){case 0x8b81:write(s->compiled);break;case 0x8b80:write(s->deleted);break;case 0x8b4f:write(s->source.stage);break;case 0x8b84:write(static_cast<unsigned>(s->log.size()+1));break;case 0x8b88:write(static_cast<unsigned>(s->source.source.size()+1));break;default:error(0x500);}}return 0;
    case GlOperation::GetShaderInfoLog:if(auto s=shader())return log(s->log);return 0;
    case GlOperation::DeleteShader:if(!u(0))return 0;if(auto s=shader()){s->deleted=true;bool attached=false;for(auto& [id,p]:gl_.programs){(void)id;attached|=p->attached.contains(u(0));}if(!attached)gl_.shaders.erase(u(0));}return 0;
    case GlOperation::CreateProgram:{if(gl_.programs.size()>=4096)throw std::bad_alloc{};const auto id=gl_.next_object++;gl_.programs[id]=std::make_shared<GlProgram>();return id;}
    case GlOperation::AttachShader:case GlOperation::DetachShader:if(auto p=program()){auto it=gl_.shaders.find(u(1));if(it==gl_.shaders.end()){error(0x501);return 0;}if(op==GlOperation::AttachShader){if(p->attached.contains(u(1)))error(0x502);else p->attached[u(1)]=it->second;}else{if(!p->attached.erase(u(1)))error(0x502);else if(it->second->deleted && it->second.use_count()==1)gl_.shaders.erase(it);}}return 0;
    case GlOperation::BindAttribLocation:if(u(1)>=16)error(0x501);else if(auto p=program()){auto name=text();if(name.starts_with("gl_"))error(0x502);else p->attribute_bindings[name]=u(1);}return 0;
    case GlOperation::LinkProgram:if(auto p=program()){const auto begin=std::chrono::steady_clock::now();gl_link(*p);if(!p->linked && std::getenv("X64ARM_GL_TRACE"))std::fprintf(stderr,"[gl-link] %s\n",p->log.c_str());++gl_.links;gl_.link_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count());}return 0;
    case GlOperation::GetProgramiv:if(auto p=program()){switch(u(1)){case 0x8b82:write(p->linked);break;case 0x8b80:write(p->deleted);break;case 0x8b84:write(static_cast<unsigned>(p->log.size()+1));break;case 0x8b85:write(static_cast<unsigned>(p->attached.size()));break;case 0x8b86:write(static_cast<unsigned>(p->uniforms.size()));break;default:error(0x500);}}return 0;
    case GlOperation::GetProgramInfoLog:if(auto p=program())return log(p->log);return 0;
    case GlOperation::UseProgram:{if(!u(0)){gl_.current.reset();gl_.program=0;return 1;}if(auto p=program()){if(!p->linked)error(0x502);else{gl_.current=p;gl_.program=u(0);return 1;}}return 0;}
    case GlOperation::DeleteProgram:if(!u(0))return 0;if(auto p=program()){p->deleted=true;if(gl_.current!=p)gl_.programs.erase(u(0));}return 0;
    case GlOperation::GetUniformLocation:if(auto p=program()){if(!p->linked)error(0x502);else if(auto it=p->locations.find(text());it!=p->locations.end())return static_cast<unsigned>(it->second);}return UINT32_MAX;
    case GlOperation::Uniform1fv:case GlOperation::Uniform2fv:case GlOperation::Uniform3fv:case GlOperation::Uniform4fv:
    case GlOperation::Uniform1iv:case GlOperation::Uniform2iv:case GlOperation::Uniform3iv:case GlOperation::Uniform4iv:case GlOperation::UniformMatrix4fv:{
        if(n(0)==-1)return 0;auto p=gl_.current;if(!p || n(0)<0 || u(0)>=p->uniforms.size()){error(0x502);return 0;}
        const bool matrix=op==GlOperation::UniformMatrix4fv,integer=op>=GlOperation::Uniform1iv && op<=GlOperation::Uniform4iv;
        const unsigned components=matrix?4U:1U+static_cast<unsigned>(op)-static_cast<unsigned>(integer?GlOperation::Uniform1iv:GlOperation::Uniform1fv),columns=matrix?4U:1U;auto& location=p->uniforms[u(0)];
        if(n(1)<0){error(0x501);return 0;}if(components!=location.components || columns!=location.columns || (integer?(location.type!=0x1404 && location.type!=0x8b56 && location.type!=0x8b5e):(location.type!=0x1406 && location.type!=0x8b56))){error(0x502);return 0;}
        const auto count=std::min(u(1),location.remaining);if(data.size()<static_cast<std::size_t>(count)*components*columns*4){error(0x501);return 0;}
        for(const auto& target:location.targets){auto& stage=p->stages[target.stage];if(target.sampler){for(unsigned i=0;i<count;++i){int value;std::memcpy(&value,data.data()+i*4,4);stage.units[target.index][target.element+i]=value;}}else{const auto& uniform=stage.reflection.uniforms[target.index];
            for(unsigned i=0;i<count;++i)for(unsigned c=0;c<columns;++c)for(unsigned r=0;r<components;++r){const auto at=uniform.offset+(target.element+i)*uniform.array_stride+c*uniform.matrix_stride+r*4;auto source=(i*columns+c)*components+r;if(matrix && u(2))source=(i*components+r)*columns+c;std::uint32_t value;std::memcpy(&value,data.data()+source*4,4);if(location.type==0x8b56)value=integer?(value!=0):(std::bit_cast<float>(value)!=0);std::memcpy(stage.uniforms.data()+at,&value,4);}}}return 0;}
    case GlOperation::GenFramebuffers:{if(n(0)<0 || output.size()<static_cast<std::size_t>(u(0))*4){error(0x501);return 0;}for(unsigned i=0;i<u(0);++i){while(!gl_.next_framebuffer || gl_.framebuffers.contains(gl_.next_framebuffer))++gl_.next_framebuffer;const auto id=gl_.next_framebuffer++;gl_.framebuffers[id]=std::make_shared<GlFramebuffer>();std::memcpy(output.data()+i*4,&id,4);}return 0;}
    case GlOperation::ActiveTexture:if(u(0)<0x84c0 || u(0)>=0x84c0+gl_.texture_units.size())error(0x500);else{gl_.texture_units[gl_.active_texture]=state_.binding;gl_.active_texture=u(0)-0x84c0;state_.binding=gl_.texture_units[gl_.active_texture];}return 0;
    case GlOperation::BindTexture:if(u(0)==0xde1 && u(1)!=UINT32_MAX)gl_.texture_units[gl_.active_texture]=u(1);return std::nullopt;
    case GlOperation::GenerateMipmap:{if(u(0)!=0xde1){error(0x500);return 0;}auto& t=textures_[state_.binding];if(!t.native){error(0x502);return 0;}begin_commands();auto encoder=[commands_ blitCommandEncoder];MetalEncoderScope encoder_scope{encoder};[encoder generateMipmapsForTexture:t.native];for(unsigned level=1;level<t.native.mipmapLevelCount;++level)t.formats[level]=t.formats.at(0);encoder_scope.end();pending_leases_.push_back(t.lease);return 0;}
    case GlOperation::GenTextures:return std::nullopt;
    case GlOperation::BindFramebuffer:{if(u(0)!=0x8d40 && u(0)!=0x8ca8 && u(0)!=0x8ca9){error(0x500);return 0;}if(u(1) && !gl_.framebuffers.contains(u(1)))gl_.framebuffers[u(1)]=std::make_shared<GlFramebuffer>();if(u(0)!=0x8ca8)gl_.draw_framebuffer=u(1);if(u(0)!=0x8ca9)gl_.read_framebuffer=u(1);return 0;}
    case GlOperation::FramebufferTexture2D:{if(u(0)!=0x8d40 && u(0)!=0x8ca8 && u(0)!=0x8ca9){error(0x500);return 0;}const auto id=u(0)==0x8ca8?gl_.read_framebuffer:gl_.draw_framebuffer;if(!id){error(0x502);return 0;}if(u(2)!=0xde1 || (u(1)!=0x8d00 && (u(1)<0x8ce0 || u(1)>=0x8ce8))){error(0x500);return 0;}if(n(4)<0){error(0x501);return 0;}auto& fb=*gl_.framebuffers.at(id);if(!u(3))fb.attachments.erase(u(1));else if(!textures_.contains(u(3)))error(0x502);else fb.attachments[u(1)]={u(3),u(4),textures_.at(u(3))};return 0;}
    case GlOperation::DeleteFramebuffers:for(std::size_t i=0;i+4<=data.size();i+=4){unsigned id;std::memcpy(&id,data.data()+i,4);if(gl_.draw_framebuffer==id)gl_.draw_framebuffer=0;if(gl_.read_framebuffer==id)gl_.read_framebuffer=0;gl_.framebuffers.erase(id);}return 0;
    case GlOperation::CheckFramebufferStatus:if(u(0)!=0x8d40 && u(0)!=0x8ca8 && u(0)!=0x8ca9){error(0x500);return 0;}return gl_framebuffer_status(u(0)==0x8ca8);
    case GlOperation::ReadPixels:if(!gl_.read_framebuffer)return std::nullopt;gl_read_pixels(args,output);return 0;
    case GlOperation::ReadBuffer:if(!gl_.read_framebuffer)return std::nullopt;if(u(0) && (u(0)<0x8ce0 || u(0)>=0x8ce8))error(0x500);else gl_.framebuffers.at(gl_.read_framebuffer)->read_buffer=u(0);return 0;
    case GlOperation::DrawBuffer:if(!gl_.draw_framebuffer)return std::nullopt;if(u(0) && (u(0)<0x8ce0 || u(0)>=0x8ce8))error(0x500);else gl_.framebuffers.at(gl_.draw_framebuffer)->draw_buffers={u(0)};return 0;
    case GlOperation::DrawBuffers:{if(!gl_.draw_framebuffer){if(u(0)!=1 || data.size()<4){error(0x502);return 0;}unsigned target;std::memcpy(&target,data.data(),4);if(target!=0x405 && target!=0x404 && target!=0){error(0x502);return 0;}state_.draw_buffer=target;return 0;}if(u(0)>8){error(0x501);return 0;}std::vector<unsigned> targets(u(0));std::memcpy(targets.data(),data.data(),targets.size()*4);std::set<unsigned> unique;for(auto value:targets){if(value && (value<0x8ce0 || value>=0x8ce8 || !unique.insert(value).second)){error(0x502);return 0;}}gl_.framebuffers.at(gl_.draw_framebuffer)->draw_buffers=std::move(targets);return 0;}
    case GlOperation::BlendFuncSeparate:if(!factor(u(0)) || !factor(u(1)) || !factor(u(2)) || !factor(u(3)))error(0x500);else{state_.blend_src=u(0);state_.blend_dst=u(1);gl_.src_alpha=u(2);gl_.dst_alpha=u(3);}return 0;
    case GlOperation::BlendFunc:gl_.src_alpha=u(0);gl_.dst_alpha=u(1);return std::nullopt;
    case GlOperation::BlendEquationSeparate:if((u(0)!=0x8006 && u(0)!=0x8007 && u(0)!=0x8008 && u(0)!=0x800a && u(0)!=0x800b) || (u(1)!=0x8006 && u(1)!=0x8007 && u(1)!=0x8008 && u(1)!=0x800a && u(1)!=0x800b))error(0x500);else{gl_.equation_rgb=u(0);gl_.equation_alpha=u(1);}return 0;
    case GlOperation::PolygonMode:if(u(0)!=0x408 || (u(1)!=0x1b01 && u(1)!=0x1b02))error(0x500);else gl_.polygon_mode=u(1);return 0;
    case GlOperation::DrawArrays:case GlOperation::DrawElements:if(!gl_.current)return std::nullopt;[[fallthrough]];
    case GlOperation::DrawArraysInstanced:case GlOperation::DrawElementsInstanced:gl_draw(op,args,data);return 0;
    case GlOperation::Clear:if(!gl_.draw_framebuffer)return std::nullopt;if(u(0)&~0x4500U){error(0x501);return 0;}if(gl_framebuffer_status()!=0x8cd5){error(0x506);return 0;}{auto pass=gl_pass(u(0));begin_commands();auto encoder=[commands_ renderCommandEncoderWithDescriptor:pass];MetalEncoderScope encoder_scope{encoder};encoder_scope.end();return 0;}
    default:return std::nullopt;
    }
}
void MetalGl::gl_texture(std::span<const std::uint64_t> supplied,std::span<const std::uint8_t> data,bool subimage){
    // 完整定義與局部更新共用格式轉換、UNPACK 版面及 GPU 同步。
    std::array<std::uint64_t,8> a{};for(unsigned i=0;i<a.size();++i)a[i]=static_cast<std::uint32_t>(supplied[i]);
    auto& t=textures_[state_.binding];unsigned xoffset=0,yoffset=0;
    if(subimage){
        if(a[0]!=0xde1){error(0x500);return;}
        if(static_cast<unsigned>(a[1])>14 || static_cast<unsigned>(a[2])>INT32_MAX || static_cast<unsigned>(a[3])>INT32_MAX || static_cast<unsigned>(a[4])>16384 || static_cast<unsigned>(a[5])>16384){error(0x501);return;}
        const auto level=static_cast<unsigned>(a[1]);
        if(!t.native || !t.formats.contains(level)){error(0x502);return;}
        xoffset=static_cast<unsigned>(a[2]);yoffset=static_cast<unsigned>(a[3]);
        const auto width=std::max<NSUInteger>(1,t.native.width>>level),height=std::max<NSUInteger>(1,t.native.height>>level);
        if(xoffset>width || yoffset>height || a[4]>width-xoffset || a[5]>height-yoffset){error(0x501);return;}
        a[2]=t.formats.at(level);a[3]=a[4];a[4]=a[5];a[5]=0;
    }
    const auto u=[&](unsigned i){return static_cast<unsigned>(a[i]);};
    const bool proxy=!subimage && u(0)==0x8064;
    if(u(0)!=0xde1 && !proxy){error(0x500);return;}if(u(1)>14 || u(3)>INT32_MAX || u(4)>INT32_MAX || u(5)>1 || (!proxy && (u(3)>16384 || u(4)>16384 || u(5)))){error(0x501);return;}
    const unsigned source_channels=u(6)==0x1903 || u(6)==0x1902?1U:u(6)==0x8227?2U:u(6)==0x1907?3U:u(6)==0x1908?4U:0U;
    const bool source_packed=u(7)==0x8c3b;
    const unsigned source_unit=u(7)==0x1401?1U:u(7)==0x1406 || source_packed?4U:(u(7)==0x140b || u(7)==0x1403)?2U:0U;if(!source_channels || !source_unit || (source_packed && u(6)!=0x1907)){error(0x500);return;}
    const auto source_bytes=source_packed?4U:source_channels*source_unit;
    unsigned channels=4,component=1;MTLPixelFormat format=MTLPixelFormatRGBA8Unorm;
    switch(u(2)){
    case 3:case 4:case 0x1907:case 0x1908:case 0x8051:case 0x8058:break;
    case 0x1903:case 0x8229:channels=1;format=MTLPixelFormatR8Unorm;break;
    case 0x822b:channels=2;format=MTLPixelFormatRG8Unorm;break;
    case 0x822a:channels=1;component=2;format=MTLPixelFormatR16Unorm;break;
    case 0x822c:channels=2;component=2;format=MTLPixelFormatRG16Unorm;break;
    case 0x8054:case 0x805b:component=2;format=MTLPixelFormatRGBA16Unorm;break;
    case 0x8c3a:channels=3;component=0;format=MTLPixelFormatRG11B10Float;break;
    case 0x822d:channels=1;component=2;format=MTLPixelFormatR16Float;break;
    case 0x822e:channels=1;component=4;format=MTLPixelFormatR32Float;break;
    case 0x822f:channels=2;component=2;format=MTLPixelFormatRG16Float;break;
    case 0x8230:channels=2;component=4;format=MTLPixelFormatRG32Float;break;
    case 0x881b:case 0x881a:component=2;format=MTLPixelFormatRGBA16Float;break;
    case 0x8815:case 0x8814:component=4;format=MTLPixelFormatRGBA32Float;break;
    case 0x1902:case 0x81a5:case 0x81a6:case 0x81a7:case 0x8cac:channels=1;component=4;format=MTLPixelFormatDepth32Float;break;
    default:error(0x501);return;}
    const bool packed=format==MTLPixelFormatRG11B10Float;const auto bytes_per_pixel=packed?4U:channels*component;
    if(proxy){
        TextureLevelInfo level{};
        // 依同一份格式映射及 Metal 的尺寸上限回報能力，不配置資源或讀取像素。
        if(!u(5) && u(3)<=(16384U>>u(1)) && u(4)<=(16384U>>u(1))){
            level.width=u(3);level.height=u(4);level.format=u(2);
            if(format==MTLPixelFormatDepth32Float)level.depth=32;
            else for(unsigned i=0;i<channels;++i)level.bits[i]=packed?(i==2?10U:11U):component*8;
        }
        proxy_levels_[u(1)]=level;return;
    }
    if(!subimage && u(1)==0){flush(true);if(!u(3) || !u(4)){t.native=nil;t.lease.reset();t.formats.clear();return;}const bool mipmapped=format!=MTLPixelFormatDepth32Float;
        auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:u(3) height:u(4) mipmapped:mipmapped];desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
        std::uint64_t allocation=0;for(NSUInteger level=0;level<desc.mipmapLevelCount;++level)allocation+=std::max<NSUInteger>(1,u(3)>>level)*std::max<NSUInteger>(1,u(4)>>level)*bytes_per_pixel;
        auto lease=reserve_gpu_memory(allocation);auto texture=[device_ newTextureWithDescriptor:desc];if(!texture)throw std::bad_alloc{};t.native=texture;t.lease=std::move(lease);t.formats.clear();
    }else if(!subimage && (!t.native || u(1)>=t.native.mipmapLevelCount || format!=t.native.pixelFormat || u(3)!=std::max<NSUInteger>(1,t.native.width>>u(1)) || u(4)!=std::max<NSUInteger>(1,t.native.height>>u(1)))){error(0x501);return;}
    if(!subimage)t.formats[u(1)]=u(2);
    if(!u(3) || !u(4))return;
    if(data.empty()){if(subimage)error(0x502);return;}const auto [stride,start]=pixel_layout(u(3),source_bytes,false);if(start+(u(4)-1)*stride+static_cast<std::size_t>(u(3))*source_bytes>data.size()){error(0x501);return;}
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(u(3))*u(4)*bytes_per_pixel);
    for(unsigned y=0;y<u(4);++y)for(unsigned x=0;x<u(3);++x){std::array<float,4> values{0,0,0,1};
        for(unsigned c=0;c<std::min(channels,source_channels);++c){const auto at=start+y*stride+x*source_bytes;
            if(source_packed){std::uint32_t word;std::memcpy(&word,data.data()+at,4);if(pixel_[0xcf0])word=__builtin_bswap32(word);values[c]=unpack_rg11b10(word,c);}
            else if(source_unit==1)values[c]=data[at+c]/255.F;
            else if(source_unit==4)std::memcpy(&values[c],data.data()+at+c*4,4);
            else if(u(7)==0x1403){std::uint16_t word;std::memcpy(&word,data.data()+at+c*2,2);if(pixel_[0xcf0])word=__builtin_bswap16(word);values[c]=word/65535.F;}
            else{__fp16 half;std::memcpy(&half,data.data()+at+c*2,2);values[c]=half;}}
        if(u(2)==3 || u(2)==0x1907 || u(2)==0x8051 || u(2)==0x8054 || u(2)==0x881b || u(2)==0x8815)values[3]=1;
        const auto at=(static_cast<std::size_t>(y)*u(3)+x)*bytes_per_pixel;
        if(packed){const auto word=pack_rg11b10(values[0],values[1],values[2]);std::memcpy(pixels.data()+at,&word,4);}
        else for(unsigned c=0;c<channels;++c){const auto value=values[c];if(component==1)pixels[at+c]=static_cast<std::uint8_t>(std::clamp(value,0.F,1.F)*255.F+.5F);else if(component==4)std::memcpy(pixels.data()+at+c*4,&value,4);else if(format==MTLPixelFormatR16Unorm || format==MTLPixelFormatRG16Unorm || format==MTLPixelFormatRGBA16Unorm){const auto word=static_cast<std::uint16_t>(std::clamp(std::isnan(value)?0.F:value,0.F,1.F)*65535.F+.5F);std::memcpy(pixels.data()+at+c*2,&word,2);}else{const __fp16 half=static_cast<__fp16>(value);std::memcpy(pixels.data()+at+c*2,&half,2);}}
    }
    flush(true);[t.native replaceRegion:MTLRegionMake2D(xoffset,yoffset,u(3),u(4)) mipmapLevel:u(1) withBytes:pixels.data() bytesPerRow:u(3)*bytes_per_pixel];
}
unsigned MetalGl::gl_framebuffer_status(bool read){
    const auto id=read?gl_.read_framebuffer:gl_.draw_framebuffer;if(!id)return 0x8cd5;auto& fb=*gl_.framebuffers.at(id);if(fb.attachments.empty())return 0x8cd7;
    for(auto& [slot,attachment]:fb.attachments){if(auto it=textures_.find(attachment.name);it!=textures_.end())attachment.texture=it->second;auto texture=attachment.texture.native;if(!texture || attachment.level>=texture.mipmapLevelCount)return 0x8cd6;const bool depth=texture.pixelFormat==MTLPixelFormatDepth32Float;if((slot==0x8d00)!=depth)return 0x8cd6;}
    if(read){if(fb.read_buffer && !fb.attachments.contains(fb.read_buffer))return 0x8cdc;}else for(auto target:fb.draw_buffers)if(target && !fb.attachments.contains(target))return 0x8cdb;return 0x8cd5;
}
MTLRenderPassDescriptor* MetalGl::gl_pass(unsigned clear_mask){
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
    const auto color=[&](unsigned index,id<MTLTexture> texture,unsigned level){auto target=pass.colorAttachments[index];target.texture=texture;target.level=level;target.loadAction=(clear_mask&0x4000)?MTLLoadActionClear:MTLLoadActionLoad;target.storeAction=MTLStoreActionStore;target.clearColor=MTLClearColorMake(state_.clear[0],state_.clear[1],state_.clear[2],state_.clear[3]);};
    const auto depth=[&](id<MTLTexture> texture,unsigned level){pass.depthAttachment.texture=texture;pass.depthAttachment.level=level;pass.depthAttachment.loadAction=(clear_mask&0x100)&&state_.depth_write?MTLLoadActionClear:MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=state_.depth_clear;};
    if(!gl_.draw_framebuffer){if(state_.draw_buffer)color(0,state_.draw_buffer==0x404?front_:color_,0);depth(depth_,0);pass.stencilAttachment.texture=depth_;pass.stencilAttachment.loadAction=MTLLoadActionLoad;pass.stencilAttachment.storeAction=MTLStoreActionStore;}
    else{auto& fb=*gl_.framebuffers.at(gl_.draw_framebuffer);for(unsigned i=0;i<fb.draw_buffers.size();++i){const auto target=fb.draw_buffers[i];if(!target)continue;const auto& a=fb.attachments.at(target);color(i,a.texture.native,a.level);pending_leases_.push_back(a.texture.lease);}if(auto it=fb.attachments.find(0x8d00);it!=fb.attachments.end()){depth(it->second.texture.native,it->second.level);pending_leases_.push_back(it->second.texture.lease);}}
    return pass;
}
void MetalGl::gl_draw(GlOperation op,std::span<const std::uint64_t> a,std::span<const std::uint8_t> supplied_indices){
    bool submitted=false;
    try{gl_draw_once(op,a,supplied_indices,submitted);}
    catch(const std::bad_alloc&){
        // 暫存 vertex／argument／uniform buffer 可能被尚未完成的批次占用。
        // encoder scope 已結束，先提交及回收，再重試尚未送出的這一次 draw。
        // 已送出的繪製絕不重播，以免混色等操作執行兩次。
        if(submitted)throw;
        flush(true);pending_leases_.clear();gl_draw_once(op,a,supplied_indices,submitted);
    }
}
void MetalGl::gl_draw_once(GlOperation op,std::span<const std::uint64_t> a,std::span<const std::uint8_t> supplied_indices,bool& submitted){
    if(state_.enabled.contains(0xbc0)){error(0x502);return;} // 可程式化 alpha test 尚未映射，避免靜默略過。
    auto program=gl_.current;if(!program || !program->linked){error(0x502);return;}if(gl_framebuffer_status()!=0x8cd5){error(0x506);return;}
    const bool indexed=op==GlOperation::DrawElements || op==GlOperation::DrawElementsInstanced,instanced=op==GlOperation::DrawArraysInstanced || op==GlOperation::DrawElementsInstanced;
    const auto count=static_cast<unsigned>(a[indexed?1:2]),first=indexed?0U:static_cast<unsigned>(a[1]),instances=instanced?static_cast<unsigned>(a[indexed?4:3]):1U;
    if(count>INT32_MAX || first>INT32_MAX || instances>INT32_MAX){error(0x501);return;}if(!count || !instances)return;if(a[0]>9){error(0x500);return;}if(a[0]>=4 && state_.enabled.contains(0xb44) && state_.cull_face==0x408)return;
    std::vector<std::uint32_t> indices;std::uint64_t last=static_cast<std::uint64_t>(first)+count-1;
    id<MTLBuffer> direct_indices=nil;MTLIndexType direct_index_type=MTLIndexTypeUInt16;
    if(indexed){const auto type=static_cast<unsigned>(a[2]);if(type!=0x1401 && type!=0x1403 && type!=0x1405){error(0x500);return;}const auto size=unit(type);std::span<const std::uint8_t> bytes=supplied_indices;
        if(const auto element=gl_.vao().elements){auto& b=element->bytes;if(a[3]>b.size() || static_cast<std::uint64_t>(count)*size>b.size()-a[3]){error(0x502);return;}bytes=std::span(b).subspan(static_cast<std::size_t>(a[3]),static_cast<std::size_t>(count)*size);}
        if(bytes.size()<static_cast<std::uint64_t>(count)*size){error(0x502);return;}
        if(gl_shared_buffers_enabled() && gl_.vao().elements && size>=2 && a[3]%size==0 && (a[0]==0 || a[0]==1 || a[0]==3 || a[0]==4 || a[0]==5)){
            last=graphics_index_range(bytes.first(static_cast<std::size_t>(count)*size),size).maximum;
            auto resource=gl_buffer_resource(gl_.vao().elements);direct_indices=metal_resource(resource).buffer;direct_index_type=size==2?MTLIndexTypeUInt16:MTLIndexTypeUInt32;graphics_pending_.push_back(resource);
        }else{indices.resize(count);last=0;for(unsigned i=0;i<count;++i){std::memcpy(&indices[i],bytes.data()+i*size,size);last=std::max<std::uint64_t>(last,indices[i]);}}}
    if(last>=16U*1024U*1024U){error(0x505);return;}const unsigned vertex_count=static_cast<unsigned>(last+1);
    unsigned mode=static_cast<unsigned>(a[0]);
    if(mode==2 || mode==6 || mode>=7){std::vector<std::uint32_t> expanded;const auto index=[&](unsigned i){return indexed?indices[i]:first+i;};const auto triangle=[&](unsigned x,unsigned y,unsigned z){expanded.push_back(index(x));expanded.push_back(index(y));expanded.push_back(index(z));};
        if(mode==2){if(count<2)return;for(unsigned i=1;i<count;++i){expanded.push_back(index(i-1));expanded.push_back(index(i));}expanded.push_back(index(count-1));expanded.push_back(index(0));mode=1;}
        else {if(mode==7){for(unsigned i=0;i+3<count;i+=4){triangle(i,i+1,i+2);triangle(i,i+2,i+3);}}
            else if(mode==8){for(unsigned i=0;i+3<count;i+=2){triangle(i,i+1,i+3);triangle(i,i+3,i+2);}}
            else {for(unsigned i=2;i<count;++i)triangle(0,i-1,i);}mode=4;}
        if(expanded.empty())return;indices=std::move(expanded);
    }
    auto descriptor=[MTLRenderPipelineDescriptor new];auto layout=[MTLVertexDescriptor vertexDescriptor];std::vector<id<MTLBuffer>> vertex_buffers(16);std::array<NSUInteger,16> vertex_offsets{};std::string key=gl_.draw_framebuffer?"f":"d";
    GlProgramStage* vertex=nullptr;GlProgramStage* fragment=nullptr;for(auto& stage:program->stages){if(stage.reflection.stage==0x8b31)vertex=&stage;else if(stage.reflection.stage==0x8b30)fragment=&stage;}
    if(!vertex || !fragment){error(0x502);return;}descriptor.vertexFunction=gl_.draw_framebuffer?vertex->flipped:vertex->function;descriptor.fragmentFunction=fragment->function;
    for(const auto& input:vertex->reflection.attributes){if(input.location>=16 || input.columns!=1){error(0x502);return;}const auto binding=program->attribute_bindings.find(input.name);const auto index=binding==program->attribute_bindings.end()?input.location:binding->second;if(index>=16){error(0x502);return;}const auto& attribute=gl_.vao().attributes[index];
        const auto elements=!attribute.enabled?1U:attribute.divisor?(instances-1)/attribute.divisor+1:vertex_count;
        const auto size=unit(attribute.type),stride=attribute.stride?attribute.stride:attribute.components*size;
        if(attribute.enabled){if(!attribute.buffer){error(0x502);return;}const auto& source=attribute.buffer->bytes;const auto extent=static_cast<std::uint64_t>(elements-1)*stride+attribute.components*size;if(attribute.offset>source.size() || extent>source.size()-attribute.offset){error(0x502);return;}}
        auto attr=layout.attributes[input.location];auto l=layout.layouts[15+input.location];
        const auto format=gl_shared_vertex_format(attribute);
        if(gl_shared_buffers_enabled() && attribute.enabled && format && stride%4==0 && stride<=2048 && attribute.offset%size==0 && attribute.offset%4+attribute.components*size<=stride){
            auto resource=gl_buffer_resource(attribute.buffer);vertex_buffers[input.location]=metal_resource(resource).buffer;
            vertex_offsets[input.location]=static_cast<NSUInteger>(attribute.offset&~UINT64_C(3));attr.offset=static_cast<NSUInteger>(attribute.offset%4);attr.format=*format;l.stride=stride;
            graphics_pending_.push_back(resource);++gl_.direct_vertex_bindings;
        }else{
            std::vector<std::array<float,4>> values(elements,gl_.attribute_values[index]);
            if(attribute.enabled){const auto& source=attribute.buffer->bytes;
                for(unsigned i=0;i<elements;++i){values[i]={0,0,0,1};for(unsigned c=0;c<attribute.components;++c){auto value=number(source,static_cast<std::size_t>(attribute.offset)+i*stride+c*size,attribute.type);if(attribute.normalized && attribute.type!=0x1406){const auto bits=size*8;const bool sign=(attribute.type&1)==0;value=std::max(-1.,value/(sign?std::pow(2.,bits-1)-1:std::pow(2.,bits)-1));}values[i][c]=static_cast<float>(value);}}gl_.converted_vertices+=elements;}
            auto lease=reserve_gpu_memory(values.size()*16);auto buffer=[device_ newBufferWithBytes:values.data() length:values.size()*16 options:MTLResourceStorageModeShared];if(!buffer)throw std::bad_alloc{};pending_leases_.push_back(std::move(lease));vertex_buffers[input.location]=buffer;
            ++gl_.buffer_uploads;gl_.buffer_upload_bytes+=values.size()*16;attr.format=MTLVertexFormatFloat4;attr.offset=0;l.stride=16;
        }
        attr.bufferIndex=15+input.location;l.stepFunction=!attribute.enabled?MTLVertexStepFunctionConstant:attribute.divisor?MTLVertexStepFunctionPerInstance:MTLVertexStepFunctionPerVertex;l.stepRate=!attribute.enabled?0:attribute.divisor?attribute.divisor:1;
        key+=":"+std::to_string(input.location)+","+std::to_string(attr.format)+","+std::to_string(attr.offset)+","+std::to_string(l.stride)+","+std::to_string(static_cast<unsigned>(l.stepFunction))+","+std::to_string(l.stepRate);
    }descriptor.vertexDescriptor=layout;
    auto pass=gl_pass();const auto equation=[](unsigned value){switch(value){case 0x800a:return MTLBlendOperationSubtract;case 0x800b:return MTLBlendOperationReverseSubtract;case 0x8007:return MTLBlendOperationMin;case 0x8008:return MTLBlendOperationMax;default:return MTLBlendOperationAdd;}};
    NSUInteger width=NSUIntegerMax,height=NSUIntegerMax;
    for(unsigned i=0;i<8;++i){auto target=pass.colorAttachments[i];auto format=target.texture?target.texture.pixelFormat:MTLPixelFormatInvalid;auto color=descriptor.colorAttachments[i];color.pixelFormat=format;key+=":"+std::to_string(format);if(target.texture){width=std::min(width,std::max<NSUInteger>(1,target.texture.width>>target.level));height=std::min(height,std::max<NSUInteger>(1,target.texture.height>>target.level));}
        color.blendingEnabled=state_.enabled.contains(0xbe2);color.sourceRGBBlendFactor=blend(state_.blend_src);color.destinationRGBBlendFactor=blend(state_.blend_dst);color.sourceAlphaBlendFactor=blend(gl_.src_alpha);color.destinationAlphaBlendFactor=blend(gl_.dst_alpha);color.rgbBlendOperation=equation(gl_.equation_rgb);color.alphaBlendOperation=equation(gl_.equation_alpha);}
    if(pass.depthAttachment.texture){auto texture=pass.depthAttachment.texture;descriptor.depthAttachmentPixelFormat=texture.pixelFormat;width=std::min(width,std::max<NSUInteger>(1,texture.width>>pass.depthAttachment.level));height=std::min(height,std::max<NSUInteger>(1,texture.height>>pass.depthAttachment.level));}
    if(pass.stencilAttachment.texture)descriptor.stencilAttachmentPixelFormat=pass.stencilAttachment.texture.pixelFormat;
    if(width==NSUIntegerMax || height==NSUIntegerMax){error(0x506);return;}
    key+=":"+std::to_string(descriptor.stencilAttachmentPixelFormat)+":"+std::to_string(descriptor.depthAttachmentPixelFormat)+":"+std::to_string(state_.enabled.contains(0xbe2));for(auto value:{state_.blend_src,state_.blend_dst,gl_.src_alpha,gl_.dst_alpha,gl_.equation_rgb,gl_.equation_alpha})key+=":"+std::to_string(value);
    id<MTLRenderPipelineState> pipeline;const auto found=program->pipelines.find(key);if(found!=program->pipelines.end())pipeline=found->second;else{NSError* failure=nil;pipeline=[device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];if(!pipeline)throw std::runtime_error(std::string("OpenGL Metal pipeline: ")+failure.localizedDescription.UTF8String);program->pipelines[key]=pipeline;}
    auto depth=[MTLDepthStencilDescriptor new];depth.depthCompareFunction=state_.enabled.contains(0xb71)?static_cast<MTLCompareFunction>(state_.depth_func-0x200):MTLCompareFunctionAlways;depth.depthWriteEnabled=state_.enabled.contains(0xb71)&&state_.depth_write;
    if(pass.stencilAttachment.texture)gl_stencil(depth);
    begin_commands();auto encoder=[commands_ renderCommandEncoderWithDescriptor:pass];MetalEncoderScope encoder_scope{encoder};[encoder setRenderPipelineState:pipeline];[encoder setDepthStencilState:cached_depth_state(depth)];[encoder setStencilReferenceValue:state_.stencil.reference];
    const auto& v=state_.viewport;[encoder setViewport:MTLViewport{static_cast<double>(v[0]),gl_.draw_framebuffer?static_cast<double>(v[1]):static_cast<double>(height)-v[1]-v[3],static_cast<double>(v[2]),static_cast<double>(v[3]),state_.depth_range[0],state_.depth_range[1]}];
    [encoder setCullMode:state_.enabled.contains(0xb44)?(state_.cull_face==0x404?MTLCullModeFront:MTLCullModeBack):MTLCullModeNone];const bool ccw=(state_.front_face==0x901)!=bool(gl_.draw_framebuffer);[encoder setFrontFacingWinding:ccw?MTLWindingCounterClockwise:MTLWindingClockwise];[encoder setTriangleFillMode:gl_.polygon_mode==0x1b01?MTLTriangleFillModeLines:MTLTriangleFillModeFill];
    if(state_.enabled.contains(0xc11)){const auto& r=state_.scissor;const auto x=std::clamp<std::int64_t>(r[0],0,static_cast<std::int64_t>(width)),y=std::clamp<std::int64_t>(r[1],0,static_cast<std::int64_t>(height));const auto right=std::clamp<std::int64_t>(static_cast<std::int64_t>(r[0])+r[2],x,static_cast<std::int64_t>(width)),top=std::clamp<std::int64_t>(static_cast<std::int64_t>(r[1])+r[3],y,static_cast<std::int64_t>(height));[encoder setScissorRect:MTLScissorRect{static_cast<NSUInteger>(x),static_cast<NSUInteger>(gl_.draw_framebuffer?y:static_cast<std::int64_t>(height)-top),static_cast<NSUInteger>(right-x),static_cast<NSUInteger>(top-y)}];}
    for(unsigned i=0;i<16;++i)if(vertex_buffers[i])[encoder setVertexBuffer:vertex_buffers[i] offset:vertex_offsets[i] atIndex:15+i];
    for(auto& stage:program->stages){const bool vertex_stage=stage.reflection.stage==0x8b31;
        if(stage.uniforms.empty() && stage.reflection.samplers.empty())continue;
        auto function=vertex_stage && gl_.draw_framebuffer?stage.flipped:stage.function;auto arguments=[function newArgumentEncoderWithBufferIndex:0];if(!arguments)throw std::runtime_error("Metal GLSL argument encoder 建立失敗");
        auto arguments_lease=reserve_gpu_memory(arguments.encodedLength);auto arguments_buffer=[device_ newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];if(!arguments_buffer)throw std::bad_alloc{};[arguments setArgumentBuffer:arguments_buffer offset:0];pending_leases_.push_back(std::move(arguments_lease));
        for(const auto& uniform:stage.reflection.uniforms){unsigned value;if(uniform.name==stage.reflection.clip_mask){value=0;for(unsigned i=0;i<8;++i)if(state_.enabled.contains(0x3000+i))value|=1U<<i;}else if(uniform.name==stage.reflection.point_size_enabled)value=state_.enabled.contains(0x884f);else continue;std::memcpy(stage.uniforms.data()+uniform.offset,&value,4);}
        if(!stage.uniforms.empty()){auto lease=reserve_gpu_memory(stage.uniforms.size());auto buffer=[device_ newBufferWithBytes:stage.uniforms.data() length:stage.uniforms.size() options:MTLResourceStorageModeShared];if(!buffer)throw std::bad_alloc{};pending_leases_.push_back(std::move(lease));[arguments setBuffer:buffer offset:0 atIndex:stage.reflection.buffer_index];[encoder useResource:buffer usage:MTLResourceUsageRead];}
        for(unsigned s=0;s<stage.reflection.samplers.size();++s){const auto& sampler=stage.reflection.samplers[s];for(unsigned e=0;e<sampler.count;++e){const auto unit=stage.units[s][e];unsigned texture_id=unit>=0 && static_cast<unsigned>(unit)<gl_.texture_units.size()?gl_.texture_units[static_cast<unsigned>(unit)]:0;auto found_texture=textures_.find(texture_id);const auto& texture=found_texture!=textures_.end() && found_texture->second.native?found_texture->second:textures_.at(UINT32_MAX);
            const std::array<unsigned,5> sampler_key{texture.min,texture.mag,texture.s,texture.t,texture.anisotropy};auto found_sampler=gl_.samplers.find(sampler_key);id<MTLSamplerState> state;
            if(found_sampler!=gl_.samplers.end())state=found_sampler->second;else{auto desc=[MTLSamplerDescriptor new];desc.supportArgumentBuffers=YES;desc.maxAnisotropy=texture.anisotropy;desc.minFilter=texture.min==0x2600 || texture.min==0x2700 || texture.min==0x2702?MTLSamplerMinMagFilterNearest:MTLSamplerMinMagFilterLinear;desc.magFilter=texture.mag==0x2600?MTLSamplerMinMagFilterNearest:MTLSamplerMinMagFilterLinear;desc.mipFilter=texture.min<=0x2601?MTLSamplerMipFilterNotMipmapped:texture.min<=0x2701?MTLSamplerMipFilterNearest:MTLSamplerMipFilterLinear;desc.sAddressMode=address_mode(texture.s);desc.tAddressMode=address_mode(texture.t);state=cached_sampler_state(desc);gl_.samplers[sampler_key]=state;}
            [arguments setTexture:texture.native atIndex:sampler.texture+e];[arguments setSamplerState:state atIndex:sampler.sampler+e];[encoder useResource:texture.native usage:MTLResourceUsageRead];pending_leases_.push_back(texture.lease);
        }}if(vertex_stage)[encoder setVertexBuffer:arguments_buffer offset:0 atIndex:0];else [encoder setFragmentBuffer:arguments_buffer offset:0 atIndex:0];
    }
    const std::array<MTLPrimitiveType,7> modes{MTLPrimitiveTypePoint,MTLPrimitiveTypeLine,MTLPrimitiveTypeLine,MTLPrimitiveTypeLineStrip,MTLPrimitiveTypeTriangle,MTLPrimitiveTypeTriangleStrip,MTLPrimitiveTypeTriangle};
    if(direct_indices){submitted=true;++gl_.direct_index_draws;[encoder drawIndexedPrimitives:modes[mode] indexCount:count indexType:direct_index_type indexBuffer:direct_indices indexBufferOffset:a[3] instanceCount:instances];}
    else if(!indices.empty()){auto lease=reserve_gpu_memory(indices.size()*4);auto buffer=[device_ newBufferWithBytes:indices.data() length:indices.size()*4 options:MTLResourceStorageModeShared];if(!buffer)throw std::bad_alloc{};pending_leases_.push_back(std::move(lease));++gl_.buffer_uploads;gl_.buffer_upload_bytes+=indices.size()*4;submitted=true;[encoder drawIndexedPrimitives:modes[mode] indexCount:indices.size() indexType:MTLIndexTypeUInt32 indexBuffer:buffer indexBufferOffset:0 instanceCount:instances];}
    else {submitted=true;[encoder drawPrimitives:modes[mode] vertexStart:first vertexCount:count instanceCount:instances];}encoder_scope.end();++draws_;if(++pending_>=128)flush(false);
}

void MetalGl::gl_read_pixels(std::span<const std::uint64_t> args,std::span<std::uint8_t> output){
    const auto u=[&](unsigned i){return static_cast<unsigned>(args[i]);};const auto n=[&](unsigned i){return std::bit_cast<int>(u(i));};
    if(n(2)<0 || n(3)<0){error(0x501);return;}if(gl_framebuffer_status(true)!=0x8cd5){error(0x506);return;}
    const unsigned components=u(4)==0x1902 || u(4)==0x1903?1U:u(4)==0x8227?2U:u(4)==0x1907?3U:u(4)==0x1908?4U:0U;
    const bool packed=u(5)==0x8c3b;const unsigned size=u(5)==0x1401?1U:u(5)==0x1406 || packed?4U:u(5)==0x140b?2U:0U;if(!components || !size || (packed && u(4)!=0x1907)){error(0x500);return;}
    auto& fb=*gl_.framebuffers.at(gl_.read_framebuffer);const auto slot=u(4)==0x1902?0x8d00:fb.read_buffer;const auto attachment=fb.attachments.find(slot);if(attachment==fb.attachments.end()){error(0x502);return;}
    const auto texture=attachment->second.texture.native;const auto level=attachment->second.level;const auto width=std::max<NSUInteger>(1,texture.width>>level),height=std::max<NSUInteger>(1,texture.height>>level);
    const bool source_packed=texture.pixelFormat==MTLPixelFormatRG11B10Float;
    unsigned source_components=4,source_size=1;switch(texture.pixelFormat){case MTLPixelFormatRG11B10Float:source_components=3;break;case MTLPixelFormatR8Unorm:source_components=1;break;case MTLPixelFormatRG8Unorm:source_components=2;break;case MTLPixelFormatR16Float:source_components=1;source_size=2;break;case MTLPixelFormatRG16Float:source_components=2;source_size=2;break;case MTLPixelFormatRGBA16Float:source_size=2;break;case MTLPixelFormatR32Float:case MTLPixelFormatDepth32Float:source_components=1;source_size=4;break;case MTLPixelFormatRG32Float:source_components=2;source_size=4;break;case MTLPixelFormatRGBA32Float:source_size=4;break;default:break;}
    const auto target_bytes=packed?4U:components*size,source_bytes=source_packed?4U:source_components*source_size;
    const auto [stride,offset]=pixel_layout(u(2),target_bytes,true);if(u(2) && u(3) && offset+(u(3)-1)*stride+std::size_t{u(2)}*target_bytes>output.size())throw std::invalid_argument("FBO read pixels bounds");
    flush(true);std::vector<std::uint8_t> row(width*source_bytes);
    for(unsigned y=0;y<u(3);++y){const auto sy=static_cast<std::int64_t>(n(1))+y;if(sy<0 || static_cast<NSUInteger>(sy)>=height)continue;[texture getBytes:row.data() bytesPerRow:row.size() fromRegion:MTLRegionMake2D(0,static_cast<NSUInteger>(sy),width,1) mipmapLevel:level];
        for(unsigned x=0;x<u(2);++x){const auto sx=static_cast<std::int64_t>(n(0))+x;if(sx<0 || static_cast<NSUInteger>(sx)>=width)continue;std::array<float,4> values{0,0,0,1};
            for(unsigned c=0;c<std::min(source_components,components);++c){const auto at=static_cast<std::size_t>(sx)*source_bytes;
                if(source_packed){std::uint32_t word;std::memcpy(&word,row.data()+at,4);values[c]=unpack_rg11b10(word,c);}
                else if(source_size==1)values[c]=row[at+c]/255.F;else if(source_size==2){__fp16 half;std::memcpy(&half,row.data()+at+c*2,2);values[c]=half;}else std::memcpy(&values[c],row.data()+at+c*4,4);}
            const auto at=offset+y*stride+x*target_bytes;
            if(packed){auto word=pack_rg11b10(values[0],values[1],values[2]);if(pixel_[0xd00])word=__builtin_bswap32(word);std::memcpy(output.data()+at,&word,4);}
            else for(unsigned c=0;c<components;++c){const auto value=values[c];if(size==1)output[at+c]=static_cast<std::uint8_t>(std::clamp(value,0.F,1.F)*255.F+.5F);else if(size==2){const __fp16 half=static_cast<__fp16>(value);std::memcpy(output.data()+at+c*2,&half,2);}else std::memcpy(output.data()+at+c*4,&value,4);}
        }
    }
}

void MetalGl::texture_level(GlOperation op,std::span<const std::uint64_t> args,std::span<std::uint8_t> output){
    const auto target=static_cast<unsigned>(args[0]),level=static_cast<unsigned>(args[1]),pname=static_cast<unsigned>(args[2]);
    if(target!=0xde1 && target!=0x8064){error(0x500);return;}if(level>14){error(0x501);return;}
    TextureLevelInfo info{};
    if(target==0x8064){if(const auto found=proxy_levels_.find(level);found!=proxy_levels_.end())info=found->second;}
    else if(const auto found=textures_.find(state_.binding);found!=textures_.end() && found->second.native && found->second.formats.contains(level)){
        const auto& t=found->second;info.width=static_cast<unsigned>(std::max<NSUInteger>(1,t.native.width>>level));info.height=static_cast<unsigned>(std::max<NSUInteger>(1,t.native.height>>level));info.format=t.formats.at(level);
        switch(t.native.pixelFormat){
        case MTLPixelFormatR8Unorm:info.bits={8,0,0,0};break;case MTLPixelFormatRG8Unorm:info.bits={8,8,0,0};break;case MTLPixelFormatRGBA8Unorm:info.bits={8,8,8,8};break;
        case MTLPixelFormatR16Unorm:case MTLPixelFormatR16Float:info.bits={16,0,0,0};break;case MTLPixelFormatRG16Unorm:case MTLPixelFormatRG16Float:info.bits={16,16,0,0};break;case MTLPixelFormatRGBA16Unorm:case MTLPixelFormatRGBA16Float:info.bits={16,16,16,16};break;
        case MTLPixelFormatR32Float:info.bits={32,0,0,0};break;case MTLPixelFormatRG32Float:info.bits={32,32,0,0};break;case MTLPixelFormatRGBA32Float:info.bits={32,32,32,32};break;
        case MTLPixelFormatRG11B10Float:info.bits={11,11,10,0};break;case MTLPixelFormatDepth32Float:info.depth=32;break;default:break;}
    }
    unsigned value=0;
    switch(pname){case 0x1000:value=info.width;break;case 0x1001:value=info.height;break;case 0x1003:value=info.format;break;case 0x1005:case 0x8060:case 0x8061:break;
    case 0x805c:case 0x805d:case 0x805e:case 0x805f:value=info.bits[pname-0x805c];break;case 0x884a:value=info.depth;break;default:error(0x500);return;}
    if(op==GlOperation::GetTexLevelParameterfv)value=std::bit_cast<unsigned>(static_cast<float>(value));
    if(output.size()>=4)std::memcpy(output.data(),&value,4);
}
