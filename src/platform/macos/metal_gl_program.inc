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
    static std::atomic<std::uint64_t> next_generation{1};
    p.stages=std::move(stages);p.locations.clear();p.uniforms.clear();p.pipeline_generation=next_generation.fetch_add(1,std::memory_order_relaxed);
    for(unsigned s=0;s<p.stages.size();++s){auto& stage=p.stages[s];
        const auto add=[&](const std::string& name,unsigned type,unsigned components,unsigned columns,unsigned count,unsigned index,bool sampler){
            for(unsigned e=0;e<count;++e){const auto key=count==1?name:name+"["+std::to_string(e)+"]";auto it=p.locations.find(key);int location;
                if(it==p.locations.end()){location=static_cast<int>(p.uniforms.size());p.locations[key]=location;p.uniforms.push_back({type,components,columns,count-e,{}});}else location=it->second;
                p.uniforms[static_cast<std::size_t>(location)].targets.push_back({s,index,e,sampler});if(e==0)p.locations[name]=location;
            }};
        for(unsigned i=0;i<stage.reflection.uniforms.size();++i){const auto& u=stage.reflection.uniforms[i];if(u.name==stage.reflection.clip_mask || u.name==stage.reflection.point_size_enabled || u.name==stage.reflection.point_size_value)continue;add(u.name,u.type,u.components,u.columns,u.count,i,false);}
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
    case GlOperation::Enable:case GlOperation::Disable:case GlOperation::IsEnabled:if(u(0)==0x8642 || (u(0)>=0x3000 && u(0)<0x3008)){if(op==GlOperation::IsEnabled)return state_.enabled.contains(u(0));if(op==GlOperation::Enable)state_.enabled.insert(u(0));else state_.enabled.erase(u(0));return 0;}return std::nullopt;
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
    case GlOperation::Uniform1iv:case GlOperation::Uniform2iv:case GlOperation::Uniform3iv:case GlOperation::Uniform4iv:
    case GlOperation::UniformMatrix2fv:case GlOperation::UniformMatrix3fv:case GlOperation::UniformMatrix4fv:{
        if(n(0)==-1)return 0;auto p=gl_.current;if(!p || n(0)<0 || u(0)>=p->uniforms.size()){error(0x502);return 0;}
        const bool matrix=op==GlOperation::UniformMatrix2fv || op==GlOperation::UniformMatrix3fv || op==GlOperation::UniformMatrix4fv,integer=op>=GlOperation::Uniform1iv && op<=GlOperation::Uniform4iv;
        const unsigned order=op==GlOperation::UniformMatrix2fv?2U:op==GlOperation::UniformMatrix3fv?3U:4U;
        const unsigned components=matrix?order:1U+static_cast<unsigned>(op)-static_cast<unsigned>(integer?GlOperation::Uniform1iv:GlOperation::Uniform1fv),columns=matrix?order:1U;auto& location=p->uniforms[u(0)];
        if(n(1)<0){error(0x501);return 0;}if(components!=location.components || columns!=location.columns || (integer?(location.type!=0x1404 && location.type!=0x8b56 && location.type!=0x8b5e):(location.type!=0x1406 && location.type!=0x8b56))){error(0x502);return 0;}
        const auto count=std::min(u(1),location.remaining);if(data.size()<static_cast<std::size_t>(count)*components*columns*4){error(0x501);return 0;}
        for(const auto& target:location.targets){auto& stage=p->stages[target.stage];if(target.sampler){for(unsigned i=0;i<count;++i){int value;std::memcpy(&value,data.data()+i*4,4);stage.units[target.index][target.element+i]=value;}}else{const auto& uniform=stage.reflection.uniforms[target.index];
            for(unsigned i=0;i<count;++i)for(unsigned c=0;c<columns;++c)for(unsigned r=0;r<components;++r){const auto at=uniform.offset+(target.element+i)*uniform.array_stride+c*uniform.matrix_stride+r*4;auto source=(i*columns+c)*components+r;if(matrix && u(2))source=(i*components+r)*columns+c;std::uint32_t value;std::memcpy(&value,data.data()+source*4,4);if(location.type==0x8b56)value=integer?(value!=0):(std::bit_cast<float>(value)!=0);std::memcpy(stage.uniforms.data()+at,&value,4);}}}return 0;}
    case GlOperation::GenFramebuffers:{if(n(0)<0 || output.size()<static_cast<std::size_t>(u(0))*4){error(0x501);return 0;}for(unsigned i=0;i<u(0);++i){while(!gl_.next_framebuffer || gl_.framebuffers.contains(gl_.next_framebuffer))++gl_.next_framebuffer;const auto id=gl_.next_framebuffer++;gl_.framebuffers[id]=std::make_shared<GlFramebuffer>();std::memcpy(output.data()+i*4,&id,4);}return 0;}
    case GlOperation::IsFramebuffer:{const auto it=gl_.framebuffers.find(u(0));return u(0) && it!=gl_.framebuffers.end() && it->second && it->second->bound;}
    // renderbuffer：名稱與貼圖相同由共用群組配置，第一次綁定才建立物件；儲存與查詢見 gl_renderbuffer_storage。
    case GlOperation::GenRenderbuffers:{if(n(0)<0 || output.size()<static_cast<std::size_t>(u(0))*4){error(0x501);return 0;}for(unsigned i=0;i<u(0);++i){while(!gl_.next_renderbuffer || gl_.renderbuffers.contains(gl_.next_renderbuffer))++gl_.next_renderbuffer;const unsigned id=gl_.next_renderbuffer++;gl_.renderbuffers[id]=nullptr;std::memcpy(output.data()+i*4,&id,4);}return 0;}
    case GlOperation::BindRenderbuffer:{if(u(0)!=0x8d41){error(0x500);return 0;}if(u(1)){auto& object=gl_.renderbuffers[u(1)];if(!object)object=std::make_shared<GlRenderbuffer>();object->bound=true;}gl_.renderbuffer=u(1);return 0;}
    case GlOperation::DeleteRenderbuffers:for(std::size_t i=0;i+4<=data.size();i+=4){unsigned id;std::memcpy(&id,data.data()+i,4);if(!id)continue;const auto found=gl_.renderbuffers.find(id);if(found==gl_.renderbuffers.end())continue;const auto object=found->second;
            if(gl_.renderbuffer==id)gl_.renderbuffer=0;
            // 附加在目前綁定的 framebuffer 上者先解除附加；其他 framebuffer 保留物件直到解除附加。
            if(object)for(const auto bound:{gl_.draw_framebuffer,gl_.read_framebuffer})if(bound){auto& attachments=gl_.framebuffers.at(bound)->attachments;for(auto it=attachments.begin();it!=attachments.end();)it=it->second.renderbuffer==object?attachments.erase(it):std::next(it);}
            gl_.renderbuffers.erase(found);}return 0;
    case GlOperation::IsRenderbuffer:{const auto it=gl_.renderbuffers.find(u(0));return u(0) && it!=gl_.renderbuffers.end() && it->second && it->second->bound;}
    case GlOperation::RenderbufferStorage:gl_renderbuffer_storage(args);return 0;
    case GlOperation::GetRenderbufferParameteriv:{
        if(u(0)!=0x8d41){error(0x500);return 0;}const auto it=gl_.renderbuffers.find(gl_.renderbuffer);if(!gl_.renderbuffer || it==gl_.renderbuffers.end() || !it->second){error(0x502);return 0;}
        const auto& object=*it->second;const auto format=gl_renderbuffer_format(object.format);unsigned value=0;
        switch(u(1)){case 0x8d42:value=object.width;break;case 0x8d43:value=object.height;break;case 0x8d44:value=object.format;break;case 0x8cab:value=object.samples;break;
        case 0x8d50:case 0x8d51:case 0x8d52:case 0x8d53:case 0x8d54:case 0x8d55:value=object.texture.native && format?format->bits[u(1)-0x8d50]:0;break;default:error(0x500);return 0;}
        write(value);return 1;}
    case GlOperation::FramebufferRenderbuffer:{
        if(u(0)!=0x8d40 && u(0)!=0x8ca8 && u(0)!=0x8ca9){error(0x500);return 0;}const auto id=u(0)==0x8ca8?gl_.read_framebuffer:gl_.draw_framebuffer;if(!id){error(0x502);return 0;}
        if((u(1)<0x8ce0 || u(1)>=0x8ce8) && u(1)!=0x8d00 && u(1)!=0x8d20 && u(1)!=0x821a){error(0x500);return 0;}if(u(2)!=0x8d41){error(0x500);return 0;}
        auto& fb=*gl_.framebuffers.at(id);const auto slots=u(1)==0x821a?std::vector<unsigned>{0x8d00,0x8d20}:std::vector<unsigned>{u(1)};
        if(!u(3)){for(auto slot:slots)fb.attachments.erase(slot);return 0;}
        const auto found=gl_.renderbuffers.find(u(3));if(found==gl_.renderbuffers.end() || !found->second){error(0x502);return 0;}
        for(auto slot:slots)fb.attachments[slot]=GlAttachment{u(3),0,found->second->texture,found->second,0x8d41};return 0;}
    case GlOperation::GetFramebufferAttachmentParameteriv:if(const auto value=gl_attachment_parameter(args)){write(*value);return 1;}return 0;
    case GlOperation::BlitFramebuffer:gl_blit(args);return 0;
    case GlOperation::GetAttribLocation:{
        // 與繪製時相同的對應：glBindAttribLocation 指定者優先，其餘為連結時的反射位置；內建 gl_ 屬性與不存在的名稱為 -1。
        auto p=program();if(!p)return UINT32_MAX;if(!p->linked){error(0x502);return UINT32_MAX;}const auto name=text();if(name.starts_with("gl_"))return UINT32_MAX;
        for(const auto& stage:p->stages)if(stage.reflection.stage==0x8b31)for(const auto& input:stage.reflection.attributes)if(input.name==name){const auto binding=p->attribute_bindings.find(name);return binding==p->attribute_bindings.end()?input.location:binding->second;}
        return UINT32_MAX;}
    case GlOperation::IsShader:return u(0) && gl_.shaders.contains(u(0));
    case GlOperation::IsProgram:return u(0) && gl_.programs.contains(u(0));
    case GlOperation::BlendColor:for(unsigned i=0;i<4;++i){const auto value=std::bit_cast<float>(u(i));gl_.blend_color[i]=std::isnan(value)?0.F:std::clamp(value,0.F,1.F);}return 0;
    case GlOperation::ActiveTexture:if(u(0)<0x84c0 || u(0)>=0x84c0+gl_.texture_units.size())error(0x500);else{gl_.texture_units[gl_.active_texture]=state_.binding;gl_.active_texture=u(0)-0x84c0;state_.binding=gl_.texture_units[gl_.active_texture];}return 0;
    case GlOperation::BindTexture:{
        // 只有之後的固定管線綁定會成功時才更新貼圖單元（名稱已綁過 GL_TEXTURE_1D 時為 INVALID_OPERATION）。
        const auto found=textures_.find(u(1));if(u(0)==0xde1 && u(1)!=UINT32_MAX && (!u(1) || found==textures_.end() || !found->second.target || found->second.target==0xde1))gl_.texture_units[gl_.active_texture]=u(1);return std::nullopt;}
    case GlOperation::GenerateMipmap:{auto* bound=bound_texture(u(0));if(!bound){error(0x500);return 0;}auto& t=*bound;if(!t.native){error(0x502);return 0;}begin_commands(true,"mips");auto encoder=[commands_ blitCommandEncoder];MetalEncoderScope encoder_scope{encoder};[encoder generateMipmapsForTexture:t.native];for(unsigned level=1;level<t.native.mipmapLevelCount;++level)t.formats[level]=t.formats.at(0);encoder_scope.end();pending_leases_.push_back(t.lease);return 0;}
    case GlOperation::GenTextures:return std::nullopt;
    case GlOperation::BindFramebuffer:{if(u(0)!=0x8d40 && u(0)!=0x8ca8 && u(0)!=0x8ca9){error(0x500);return 0;}if(u(1)){auto& object=gl_.framebuffers[u(1)];if(!object)object=std::make_shared<GlFramebuffer>();object->bound=true;}if(u(0)!=0x8ca8)gl_.draw_framebuffer=u(1);if(u(0)!=0x8ca9)gl_.read_framebuffer=u(1);return 0;}
    case GlOperation::FramebufferTexture2D:{
        // glFramebufferTexture1D／2D／3D 與 glFramebufferTextureLayer（textarget 0）共用：本後端的貼圖只有 1D 與 2D，
        // 3D、立方體面與圖層附加只能解除附加，指定實際貼圖為 INVALID_OPERATION。
        if(u(0)!=0x8d40 && u(0)!=0x8ca8 && u(0)!=0x8ca9){error(0x500);return 0;}const auto id=u(0)==0x8ca8?gl_.read_framebuffer:gl_.draw_framebuffer;if(!id){error(0x502);return 0;}
        if((u(1)<0x8ce0 || u(1)>=0x8ce8) && u(1)!=0x8d00 && u(1)!=0x8d20 && u(1)!=0x821a){error(0x500);return 0;}
        const auto textarget=u(2);if(textarget && textarget!=0xde0 && textarget!=0xde1 && textarget!=0x806f && (textarget<0x8515 || textarget>0x851a)){error(0x500);return 0;}
        if(n(4)<0 || u(4)>14){error(0x501);return 0;}auto& fb=*gl_.framebuffers.at(id);const auto slots=u(1)==0x821a?std::vector<unsigned>{0x8d00,0x8d20}:std::vector<unsigned>{u(1)};
        if(!u(3)){for(auto slot:slots)fb.attachments.erase(slot);return 0;}
        const auto found=textures_.find(u(3));if(found==textures_.end()){error(textarget?0x502:0x501);return 0;}
        const auto type=found->second.target?found->second.target:0xde1U;
        if(!textarget || textarget==0x806f || textarget>=0x8515 || (textarget==0xde0)!=(type==0xde0)){error(0x502);return 0;}
        for(auto slot:slots)fb.attachments[slot]=GlAttachment{u(3),u(4),found->second,{},textarget};return 0;}
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
    const unsigned target=static_cast<unsigned>(a[0]);const bool one_d=target==0xde0 || target==0x8063;
    auto* bound=bound_texture(target==0x8063?0xde0U:target==0x8064?0xde1U:target);if(!bound){error(0x500);return;}
    auto& t=*bound;unsigned xoffset=0,yoffset=0;
    if(subimage){
        if(target!=0xde1 && target!=0xde0){error(0x500);return;}
        if(one_d && (a[3] || a[5]!=1)){error(0x501);return;}
        if(static_cast<unsigned>(a[1])>14 || static_cast<unsigned>(a[2])>INT32_MAX || static_cast<unsigned>(a[3])>INT32_MAX || static_cast<unsigned>(a[4])>16384 || static_cast<unsigned>(a[5])>16384){error(0x501);return;}
        const auto level=static_cast<unsigned>(a[1]);
        if(!t.native || !t.formats.contains(level)){error(0x502);return;}
        xoffset=static_cast<unsigned>(a[2]);yoffset=static_cast<unsigned>(a[3]);
        const auto width=std::max<NSUInteger>(1,t.native.width>>level),height=std::max<NSUInteger>(1,t.native.height>>level);
        if(xoffset>width || yoffset>height || a[4]>width-xoffset || a[5]>height-yoffset){error(0x501);return;}
        a[2]=t.formats.at(level);a[3]=a[4];a[4]=a[5];a[5]=0;
    }
    const auto u=[&](unsigned i){return static_cast<unsigned>(a[i]);};
    const bool proxy=!subimage && (u(0)==0x8064 || u(0)==0x8063);
    if(u(0)!=0xde1 && u(0)!=0xde0 && !proxy){error(0x500);return;}if(!subimage && one_d && u(4)!=1){error(0x501);return;}if(u(1)>14 || u(3)>INT32_MAX || u(4)>INT32_MAX || u(5)>1 || (!proxy && (u(3)>16384 || u(4)>16384 || u(5)))){error(0x501);return;}
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
        proxy_levels_[u(1)+(one_d?16U:0U)]=level;return;
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
    // 客體影像依規格先做像素傳輸（色彩縮放／偏移與 MAP_COLOR；深度來源為深度縮放／偏移），再換成內部格式。
    const bool depth_source=u(6)==0x1902;const auto& transfer=state_.transfer;
    const bool transfer_color=!depth_source && color_transfer_active(),transfer_depth=depth_source && (transfer.depth_scale!=1 || transfer.depth_bias!=0);
    for(unsigned y=0;y<u(4);++y)for(unsigned x=0;x<u(3);++x){std::array<float,4> values{0,0,0,1};
        for(unsigned c=0;c<std::min(channels,source_channels);++c){const auto at=start+y*stride+x*source_bytes;
            if(source_packed){std::uint32_t word;std::memcpy(&word,data.data()+at,4);if(pixel_[0xcf0])word=__builtin_bswap32(word);values[c]=unpack_rg11b10(word,c);}
            else if(source_unit==1)values[c]=data[at+c]/255.F;
            else if(source_unit==4)std::memcpy(&values[c],data.data()+at+c*4,4);
            else if(u(7)==0x1403){std::uint16_t word;std::memcpy(&word,data.data()+at+c*2,2);if(pixel_[0xcf0])word=__builtin_bswap16(word);values[c]=word/65535.F;}
            else{__fp16 half;std::memcpy(&half,data.data()+at+c*2,2);values[c]=half;}}
        if(transfer_color)color_transfer(values);else if(transfer_depth)values[0]=depth_transfer(values[0]);
        if(u(2)==3 || u(2)==0x1907 || u(2)==0x8051 || u(2)==0x8054 || u(2)==0x881b || u(2)==0x8815)values[3]=1;
        const auto at=(static_cast<std::size_t>(y)*u(3)+x)*bytes_per_pixel;
        if(packed){const auto word=pack_rg11b10(values[0],values[1],values[2]);std::memcpy(pixels.data()+at,&word,4);}
        else for(unsigned c=0;c<channels;++c){const auto value=values[c];if(component==1)pixels[at+c]=static_cast<std::uint8_t>(std::clamp(value,0.F,1.F)*255.F+.5F);else if(component==4)std::memcpy(pixels.data()+at+c*4,&value,4);else if(format==MTLPixelFormatR16Unorm || format==MTLPixelFormatRG16Unorm || format==MTLPixelFormatRGBA16Unorm){const auto word=static_cast<std::uint16_t>(std::clamp(std::isnan(value)?0.F:value,0.F,1.F)*65535.F+.5F);std::memcpy(pixels.data()+at+c*2,&word,2);}else{const __fp16 half=static_cast<__fp16>(value);std::memcpy(pixels.data()+at+c*2,&half,2);}}
    }
    flush(true);[t.native replaceRegion:MTLRegionMake2D(xoffset,yoffset,u(3),u(4)) mipmapLevel:u(1) withBytes:pixels.data() bytesPerRow:u(3)*bytes_per_pixel];
    // GL_GENERATE_MIPMAP：第 0 層改變後由第 0 層重建其餘各層。
    if(u(1)==0 && t.generate_mipmap && t.native.mipmapLevelCount>1){
        begin_commands(true,"mips");auto encoder=[commands_ blitCommandEncoder];MetalEncoderScope encoder_scope{encoder};[encoder generateMipmapsForTexture:t.native];encoder_scope.end();
        for(unsigned level=1;level<t.native.mipmapLevelCount;++level)t.formats[level]=t.formats.at(0);pending_leases_.push_back(t.lease);
    }
}
unsigned MetalGl::gl_framebuffer_status(bool read){
    // 每次檢查都依名稱取回附件的最新內容（貼圖可能重新定義、renderbuffer 可能重新配置）；格式須符合附加點，所有附件的樣本數相同。
    const auto id=read?gl_.read_framebuffer:gl_.draw_framebuffer;if(!id)return 0x8cd5;auto& fb=*gl_.framebuffers.at(id);if(fb.attachments.empty())return 0x8cd7;
    std::optional<NSUInteger> samples;
    for(auto& [slot,attachment]:fb.attachments){
        if(attachment.renderbuffer)attachment.texture=attachment.renderbuffer->texture;else if(auto it=textures_.find(attachment.name);it!=textures_.end())attachment.texture=it->second;
        auto texture=attachment.texture.native;if(!texture || attachment.level>=texture.mipmapLevelCount)return 0x8cd6;
        const auto format=texture.pixelFormat;const bool depth=format==MTLPixelFormatDepth32Float || format==MTLPixelFormatDepth32Float_Stencil8,stencil=format==MTLPixelFormatStencil8 || format==MTLPixelFormatDepth32Float_Stencil8;
        if(slot==0x8d00?!depth:slot==0x8d20?!stencil:(depth || stencil))return 0x8cd6;
        if(samples && *samples!=texture.sampleCount)return 0x8d56;samples=texture.sampleCount;
    }
    if(read){if(fb.read_buffer && !fb.attachments.contains(fb.read_buffer))return 0x8cdc;}else for(auto target:fb.draw_buffers)if(target && !fb.attachments.contains(target))return 0x8cdb;return 0x8cd5;
}
MTLRenderPassDescriptor* MetalGl::gl_pass(unsigned clear_mask){
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
    const auto color=[&](unsigned index,id<MTLTexture> texture,unsigned level){auto target=pass.colorAttachments[index];target.texture=texture;target.level=level;target.loadAction=(clear_mask&0x4000)?MTLLoadActionClear:MTLLoadActionLoad;target.storeAction=MTLStoreActionStore;target.clearColor=MTLClearColorMake(state_.clear[0],state_.clear[1],state_.clear[2],state_.clear[3]);};
    const auto depth=[&](id<MTLTexture> texture,unsigned level){pass.depthAttachment.texture=texture;pass.depthAttachment.level=level;pass.depthAttachment.loadAction=(clear_mask&0x100)&&state_.depth_write?MTLLoadActionClear:MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=state_.depth_clear;};
    if(!gl_.draw_framebuffer){if(state_.draw_buffer)color(0,state_.draw_buffer==0x404?front_:color_,0);depth(depth_,0);pass.stencilAttachment.texture=depth_;pass.stencilAttachment.loadAction=MTLLoadActionLoad;pass.stencilAttachment.storeAction=MTLStoreActionStore;}
    else{auto& fb=*gl_.framebuffers.at(gl_.draw_framebuffer);for(unsigned i=0;i<fb.draw_buffers.size();++i){const auto target=fb.draw_buffers[i];if(!target)continue;const auto& a=fb.attachments.at(target);color(i,a.texture.native,a.level);pending_leases_.push_back(a.texture.lease);}if(auto it=fb.attachments.find(0x8d00);it!=fb.attachments.end()){depth(it->second.texture.native,it->second.level);pending_leases_.push_back(it->second.texture.lease);}
        // 模板附件（DEPTH24_STENCIL8 同時附加在深度與模板時兩者是同一張貼圖）；完整寫入遮罩才能以 load action 清除。
        if(auto it=fb.attachments.find(0x8d20);it!=fb.attachments.end()){auto stencil=pass.stencilAttachment;stencil.texture=it->second.texture.native;stencil.level=it->second.level;
            stencil.loadAction=(clear_mask&0x400) && (state_.stencil.write_mask&0xffU)==0xffU?MTLLoadActionClear:MTLLoadActionLoad;stencil.storeAction=MTLStoreActionStore;stencil.clearStencil=state_.stencil.clear&0xffU;pending_leases_.push_back(it->second.texture.lease);}}
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
        // 用戶端陣列：服務層送入的位元組從指標起算（位移 0）；之後與緩衝物件共用同一套讀取與格式路徑。
        const auto& source_buffer=attribute.client?attribute.client_data:attribute.buffer;const std::uint64_t source_offset=attribute.client?0:attribute.offset;
        if(attribute.enabled){if(!source_buffer){error(0x502);return;}const auto& source=source_buffer->bytes;const auto extent=static_cast<std::uint64_t>(elements-1)*stride+attribute.components*size;if(source_offset>source.size() || extent>source.size()-source_offset){error(0x502);return;}}
        auto attr=layout.attributes[input.location];auto l=layout.layouts[15+input.location];
        const auto format=gl_shared_vertex_format(attribute);
        if(gl_shared_buffers_enabled() && attribute.enabled && format && stride%4==0 && stride<=2048 && source_offset%size==0 && source_offset%4+attribute.components*size<=stride){
            auto resource=gl_buffer_resource(source_buffer);vertex_buffers[input.location]=metal_resource(resource).buffer;
            vertex_offsets[input.location]=static_cast<NSUInteger>(source_offset&~UINT64_C(3));attr.offset=static_cast<NSUInteger>(source_offset%4);attr.format=*format;l.stride=stride;
            graphics_pending_.push_back(resource);++gl_.direct_vertex_bindings;
        }else{
            std::vector<std::array<float,4>> values(elements,gl_.attribute_values[index]);
            if(attribute.enabled){const auto& source=source_buffer->bytes;
                for(unsigned i=0;i<elements;++i){values[i]={0,0,0,1};for(unsigned c=0;c<attribute.components;++c){auto value=number(source,static_cast<std::size_t>(source_offset)+i*stride+c*size,attribute.type);if(attribute.normalized && attribute.type!=0x1406){const auto bits=size*8;const bool sign=(attribute.type&1)==0;value=std::max(-1.,value/(sign?std::pow(2.,bits-1)-1:std::pow(2.,bits)-1));}values[i][c]=static_cast<float>(value);}}gl_.converted_vertices+=elements;}
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
    // 多重取樣 renderbuffer：管線的光柵樣本數與附件相同（gl_framebuffer_status 已確認全部附件一致）。
    {NSUInteger samples=1;for(unsigned i=0;i<8;++i)if(auto texture=pass.colorAttachments[i].texture){samples=texture.sampleCount;break;}
        if(samples==1 && pass.depthAttachment.texture)samples=pass.depthAttachment.texture.sampleCount;if(samples==1 && pass.stencilAttachment.texture)samples=pass.stencilAttachment.texture.sampleCount;
        descriptor.rasterSampleCount=samples;key+=":s"+std::to_string(samples);}
    key+=":"+std::to_string(descriptor.stencilAttachmentPixelFormat)+":"+std::to_string(descriptor.depthAttachmentPixelFormat)+":"+std::to_string(state_.enabled.contains(0xbe2));for(auto value:{state_.blend_src,state_.blend_dst,gl_.src_alpha,gl_.dst_alpha,gl_.equation_rgb,gl_.equation_alpha})key+=":"+std::to_string(value);
    key="p"+std::to_string(program->pipeline_generation)+":"+key;
    id<MTLRenderPipelineState> pipeline;if(const auto found=gl_pipelines_.get(key))pipeline=*found;else{NSError* failure=nil;pipeline=[device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];if(!pipeline)throw std::runtime_error(std::string("OpenGL Metal pipeline: ")+failure.localizedDescription.UTF8String);gl_pipelines_.put(key,pipeline);}
    auto depth=[MTLDepthStencilDescriptor new];depth.depthCompareFunction=state_.enabled.contains(0xb71)?static_cast<MTLCompareFunction>(state_.depth_func-0x200):MTLCompareFunctionAlways;depth.depthWriteEnabled=state_.enabled.contains(0xb71)&&state_.depth_write;
    if(pass.stencilAttachment.texture)gl_stencil(depth);
    // 連續的可程式化繪製沿用同一個 render encoder（與 D3D 路徑共用 open_render_）：附件、層級與 load action 相同時不換 pass，
    // 每次繪製的固定功能狀態一律重設。鍵以 GL 標記開頭，不會與 D3D 路徑的 encoder 互相沿用。X64ARM_GL_COALESCE=0 關閉。
    static const bool coalesce=[] {const auto* value=std::getenv("X64ARM_GL_COALESCE");return !value || std::string_view(value)!="0";}();
    std::vector<std::uintptr_t> pass_key;
    {const auto object=[](id value){return reinterpret_cast<std::uintptr_t>((__bridge void*)value);};pass_key.reserve(32);pass_key.push_back(0x474c);
        for(unsigned i=0;i<8;++i){auto t=pass.colorAttachments[i];pass_key.insert(pass_key.end(),{object(t.texture),t.level,static_cast<std::uintptr_t>(t.loadAction)});}
        pass_key.insert(pass_key.end(),{object(pass.depthAttachment.texture),pass.depthAttachment.level,static_cast<std::uintptr_t>(pass.depthAttachment.loadAction),
            object(pass.stencilAttachment.texture),pass.stencilAttachment.level,static_cast<std::uintptr_t>(pass.stencilAttachment.loadAction)});}
    id<MTLRenderCommandEncoder> encoder=nil;std::optional<MetalEncoderScope> encoder_scope;
    if(coalesce && open_render_ && pending_clears_.empty() && pass_key==open_render_key_){encoder=open_render_;++coalesced_draws_;}
    else{begin_commands(true,"draw_gl");encoder=[commands_ renderCommandEncoderWithDescriptor:pass];if(!encoder)throw std::runtime_error("Metal GL draw encoder 建立失敗");++render_encoders_;
        if(coalesce){metal_chain_wait(encoder);open_render_=encoder;open_render_key_=std::move(pass_key);}else encoder_scope.emplace(encoder);}
    [encoder setRenderPipelineState:pipeline];[encoder setDepthStencilState:cached_depth_state(depth)];[encoder setStencilReferenceValue:state_.stencil.reference];
    {const auto& k=gl_.blend_color;[encoder setBlendColorRed:k[0] green:k[1] blue:k[2] alpha:k[3]];}
    const auto& v=state_.viewport;[encoder setViewport:MTLViewport{static_cast<double>(v[0]),gl_.draw_framebuffer?static_cast<double>(v[1]):static_cast<double>(height)-v[1]-v[3],static_cast<double>(v[2]),static_cast<double>(v[3]),state_.depth_range[0],state_.depth_range[1]}];
    [encoder setCullMode:state_.enabled.contains(0xb44)?(state_.cull_face==0x404?MTLCullModeFront:MTLCullModeBack):MTLCullModeNone];const bool ccw=(state_.front_face==0x901)!=bool(gl_.draw_framebuffer);[encoder setFrontFacingWinding:ccw?MTLWindingCounterClockwise:MTLWindingClockwise];[encoder setTriangleFillMode:gl_.polygon_mode==0x1b01?MTLTriangleFillModeLines:MTLTriangleFillModeFill];
    if(a[0]>=4 && state_.enabled.contains(gl_.polygon_mode==0x1b01?0x2a02:0x8037))[encoder setDepthBias:state_.polygon_offset[1] slopeScale:state_.polygon_offset[0] clamp:0];else [encoder setDepthBias:0 slopeScale:0 clamp:0];
    if(!state_.enabled.contains(0xc11))[encoder setScissorRect:MTLScissorRect{0,0,width,height}];
    else{const auto& r=state_.scissor;const auto x=std::clamp<std::int64_t>(r[0],0,static_cast<std::int64_t>(width)),y=std::clamp<std::int64_t>(r[1],0,static_cast<std::int64_t>(height));const auto right=std::clamp<std::int64_t>(static_cast<std::int64_t>(r[0])+r[2],x,static_cast<std::int64_t>(width)),top=std::clamp<std::int64_t>(static_cast<std::int64_t>(r[1])+r[3],y,static_cast<std::int64_t>(height));[encoder setScissorRect:MTLScissorRect{static_cast<NSUInteger>(x),static_cast<NSUInteger>(gl_.draw_framebuffer?y:static_cast<std::int64_t>(height)-top),static_cast<NSUInteger>(right-x),static_cast<NSUInteger>(top-y)}];}
    for(unsigned i=0;i<16;++i)if(vertex_buffers[i])[encoder setVertexBuffer:vertex_buffers[i] offset:vertex_offsets[i] atIndex:15+i];
    for(auto& stage:program->stages){const bool vertex_stage=stage.reflection.stage==0x8b31;
        if(stage.uniforms.empty() && stage.reflection.samplers.empty())continue;
        // argument encoder 只依函式而定：連結後第一次使用時建立並保存在 stage（翻轉與原版頂點函式各一）。
        const bool flipped=vertex_stage && gl_.draw_framebuffer;auto function=flipped?stage.flipped:stage.function;auto& cached=flipped?stage.flipped_arguments:stage.arguments;
        if(!cached)cached=[function newArgumentEncoderWithBufferIndex:0];auto arguments=cached;if(!arguments)throw std::runtime_error("Metal GLSL argument encoder 建立失敗");
        auto arguments_lease=reserve_gpu_memory(arguments.encodedLength);auto arguments_buffer=[device_ newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];if(!arguments_buffer)throw std::bad_alloc{};[arguments setArgumentBuffer:arguments_buffer offset:0];pending_leases_.push_back(std::move(arguments_lease));
        for(const auto& uniform:stage.reflection.uniforms){unsigned value;if(uniform.name==stage.reflection.clip_mask){value=0;for(unsigned i=0;i<8;++i)if(state_.enabled.contains(0x3000+i))value|=1U<<i;}else if(uniform.name==stage.reflection.point_size_enabled)value=state_.enabled.contains(0x8642);else if(uniform.name==stage.reflection.point_size_value)value=std::bit_cast<std::uint32_t>(state_.point_size);else continue;std::memcpy(stage.uniforms.data()+uniform.offset,&value,4);}
        if(!stage.uniforms.empty()){auto lease=reserve_gpu_memory(stage.uniforms.size());auto buffer=[device_ newBufferWithBytes:stage.uniforms.data() length:stage.uniforms.size() options:MTLResourceStorageModeShared];if(!buffer)throw std::bad_alloc{};pending_leases_.push_back(std::move(lease));[arguments setBuffer:buffer offset:0 atIndex:stage.reflection.buffer_index];[encoder useResource:buffer usage:MTLResourceUsageRead];}
        for(unsigned s=0;s<stage.reflection.samplers.size();++s){const auto& sampler=stage.reflection.samplers[s];for(unsigned e=0;e<sampler.count;++e){const auto unit=stage.units[s][e];unsigned texture_id=unit>=0 && static_cast<unsigned>(unit)<gl_.texture_units.size()?gl_.texture_units[static_cast<unsigned>(unit)]:0;auto found_texture=textures_.find(texture_id);const auto& texture=found_texture!=textures_.end() && found_texture->second.native?found_texture->second:textures_.at(UINT32_MAX);
            auto desc=[MTLSamplerDescriptor new];desc.supportArgumentBuffers=YES;desc.maxAnisotropy=texture.anisotropy;desc.minFilter=texture.min==0x2600 || texture.min==0x2700 || texture.min==0x2702?MTLSamplerMinMagFilterNearest:MTLSamplerMinMagFilterLinear;desc.magFilter=texture.mag==0x2600?MTLSamplerMinMagFilterNearest:MTLSamplerMinMagFilterLinear;desc.mipFilter=texture.min<=0x2601?MTLSamplerMipFilterNotMipmapped:texture.min<=0x2701?MTLSamplerMipFilterNearest:MTLSamplerMipFilterLinear;desc.sAddressMode=address_mode(texture.s);desc.tAddressMode=address_mode(texture.t);gl_sampler_detail(desc,texture);auto state=cached_sampler(desc);
            // argument buffer 內的 sampler 為間接引用；快取淘汰後仍由既有 flight 保留到完成。
            graphics_pending_.push_back(state);
            [arguments setTexture:texture.native atIndex:sampler.texture+e];[arguments setSamplerState:state->sampler atIndex:sampler.sampler+e];[encoder useResource:texture.native usage:MTLResourceUsageRead];pending_leases_.push_back(texture.lease);
        }}if(vertex_stage)[encoder setVertexBuffer:arguments_buffer offset:0 atIndex:0];else [encoder setFragmentBuffer:arguments_buffer offset:0 atIndex:0];
    }
    const std::array<MTLPrimitiveType,7> modes{MTLPrimitiveTypePoint,MTLPrimitiveTypeLine,MTLPrimitiveTypeLine,MTLPrimitiveTypeLineStrip,MTLPrimitiveTypeTriangle,MTLPrimitiveTypeTriangleStrip,MTLPrimitiveTypeTriangle};
    if(direct_indices){submitted=true;++gl_.direct_index_draws;[encoder drawIndexedPrimitives:modes[mode] indexCount:count indexType:direct_index_type indexBuffer:direct_indices indexBufferOffset:a[3] instanceCount:instances];}
    else if(!indices.empty()){auto lease=reserve_gpu_memory(indices.size()*4);auto buffer=[device_ newBufferWithBytes:indices.data() length:indices.size()*4 options:MTLResourceStorageModeShared];if(!buffer)throw std::bad_alloc{};pending_leases_.push_back(std::move(lease));++gl_.buffer_uploads;gl_.buffer_upload_bytes+=indices.size()*4;submitted=true;[encoder drawIndexedPrimitives:modes[mode] indexCount:indices.size() indexType:MTLIndexTypeUInt32 indexBuffer:buffer indexBufferOffset:0 instanceCount:instances];}
    else {submitted=true;[encoder drawPrimitives:modes[mode] vertexStart:first vertexCount:count instanceCount:instances];}if(encoder_scope)encoder_scope->end();++draws_;if(++pending_>=128)flush(false);
}

void MetalGl::gl_read_pixels(std::span<const std::uint64_t> args,std::span<std::uint8_t> output){
    const auto u=[&](unsigned i){return static_cast<unsigned>(args[i]);};const auto n=[&](unsigned i){return std::bit_cast<int>(u(i));};
    if(n(2)<0 || n(3)<0){error(0x501);return;}if(gl_framebuffer_status(true)!=0x8cd5){error(0x506);return;}
    // 格式：DEPTH_COMPONENT 讀深度附件、STENCIL_INDEX 讀模板附件，其餘讀 glReadBuffer 選定的色彩附件；BGRA 只交換紅藍通道。
    const bool stencil_index=u(4)==0x1901,bgra=u(4)==0x80e1;
    const unsigned components=u(4)==0x1902 || u(4)==0x1903 || stencil_index?1U:u(4)==0x8227?2U:u(4)==0x1907?3U:u(4)==0x1908 || bgra?4U:0U;
    const bool packed=u(5)==0x8c3b;const unsigned size=u(5)==0x1401?1U:u(5)==0x1406 || packed?4U:u(5)==0x140b?2U:0U;if(!components || !size || (packed && u(4)!=0x1907)){error(0x500);return;}
    auto& fb=*gl_.framebuffers.at(gl_.read_framebuffer);const auto slot=u(4)==0x1902?0x8d00U:stencil_index?0x8d20U:fb.read_buffer;const auto attachment=fb.attachments.find(slot);if(attachment==fb.attachments.end()){error(0x502);return;}
    const auto texture=attachment->second.texture.native;const auto level=attachment->second.level;const auto width=std::max<NSUInteger>(1,texture.width>>level),height=std::max<NSUInteger>(1,texture.height>>level);
    // 多重取樣的讀取 framebuffer 不可直接讀回（須先 glBlitFramebuffer 解析）。
    if(texture.sampleCount>1){error(0x502);return;}
    const bool source_packed=texture.pixelFormat==MTLPixelFormatRG11B10Float;bool unorm16=false,source_stencil=false;MTLBlitOption option=MTLBlitOptionNone;
    unsigned source_components=4,source_size=1;switch(texture.pixelFormat){case MTLPixelFormatRG11B10Float:source_components=3;break;case MTLPixelFormatR8Unorm:source_components=1;break;case MTLPixelFormatRG8Unorm:source_components=2;break;case MTLPixelFormatR16Float:source_components=1;source_size=2;break;case MTLPixelFormatRG16Float:source_components=2;source_size=2;break;case MTLPixelFormatRGBA16Float:source_size=2;break;case MTLPixelFormatR32Float:case MTLPixelFormatDepth32Float:source_components=1;source_size=4;break;case MTLPixelFormatRG32Float:source_components=2;source_size=4;break;case MTLPixelFormatRGBA32Float:source_size=4;break;
    case MTLPixelFormatR16Unorm:source_components=1;source_size=2;unorm16=true;break;case MTLPixelFormatRG16Unorm:source_components=2;source_size=2;unorm16=true;break;case MTLPixelFormatRGBA16Unorm:source_size=2;unorm16=true;break;
    case MTLPixelFormatStencil8:source_components=1;source_stencil=true;break;
    case MTLPixelFormatDepth32Float_Stencil8:source_components=1;if(stencil_index){source_stencil=true;option=MTLBlitOptionStencilFromDepthStencil;}else{source_size=4;option=MTLBlitOptionDepthFromDepthStencil;}break;default:break;}
    const auto target_bytes=packed?4U:components*size,source_bytes=source_packed?4U:source_components*source_size;
    const auto [stride,offset]=pixel_layout(u(2),target_bytes,true);if(u(2) && u(3) && offset+(u(3)-1)*stride+std::size_t{u(2)}*target_bytes>output.size())throw std::invalid_argument("FBO read pixels bounds");
    // 一次讀回涵蓋的各列；私有貼圖（深度模板 renderbuffer）由 gl_texture_bytes 經 blit 複製。
    const auto first=std::clamp<std::int64_t>(n(1),0,static_cast<std::int64_t>(height)),last=std::clamp<std::int64_t>(static_cast<std::int64_t>(n(1))+u(3),0,static_cast<std::int64_t>(height));if(last<=first)return;
    const auto row_bytes=width*source_bytes;const auto rows=gl_texture_bytes(texture,level,MTLRegionMake2D(0,static_cast<NSUInteger>(first),width,static_cast<NSUInteger>(last-first)),row_bytes,option);
    for(unsigned y=0;y<u(3);++y){const auto sy=static_cast<std::int64_t>(n(1))+y;if(sy<first || sy>=last)continue;const auto* row=rows.data()+static_cast<std::size_t>(sy-first)*row_bytes;
        for(unsigned x=0;x<u(2);++x){const auto sx=static_cast<std::int64_t>(n(0))+x;if(sx<0 || static_cast<NSUInteger>(sx)>=width)continue;std::array<float,4> values{0,0,0,1};
            for(unsigned c=0;c<std::min(source_components,components);++c){const auto at=static_cast<std::size_t>(sx)*source_bytes;
                if(source_packed){std::uint32_t word;std::memcpy(&word,row+at,4);values[c]=unpack_rg11b10(word,c);}
                else if(source_stencil)values[c]=size==1?row[at]/255.F:static_cast<float>(row[at]);
                else if(source_size==1)values[c]=row[at+c]/255.F;else if(source_size==2){if(unorm16){std::uint16_t word;std::memcpy(&word,row+at+c*2,2);values[c]=word/65535.F;}else{__fp16 half;std::memcpy(&half,row+at+c*2,2);values[c]=half;}}else std::memcpy(&values[c],row+at+c*4,4);}
            if(bgra)std::swap(values[0],values[2]);
            const auto at=offset+y*stride+x*target_bytes;
            if(packed){auto word=pack_rg11b10(values[0],values[1],values[2]);if(pixel_[0xd00])word=__builtin_bswap32(word);std::memcpy(output.data()+at,&word,4);}
            else for(unsigned c=0;c<components;++c){const auto value=values[c];if(size==1)output[at+c]=static_cast<std::uint8_t>(std::clamp(value,0.F,1.F)*255.F+.5F);else if(size==2){const __fp16 half=static_cast<__fp16>(value);std::memcpy(output.data()+at+c*2,&half,2);}else std::memcpy(output.data()+at+c*4,&value,4);}
        }
    }
}
std::vector<std::uint8_t> MetalGl::gl_texture_bytes(id<MTLTexture> texture,unsigned level,MTLRegion region,NSUInteger row_bytes,MTLBlitOption option){
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(row_bytes)*region.size.height);if(bytes.empty())return bytes;
    if(texture.storageMode!=MTLStorageModePrivate && option==MTLBlitOptionNone){flush(true);[texture getBytes:bytes.data() bytesPerRow:row_bytes fromRegion:region mipmapLevel:level];return bytes;}
    auto lease=reserve_gpu_memory(bytes.size());auto buffer=[device_ newBufferWithLength:bytes.size() options:MTLResourceStorageModeShared];if(!buffer)throw std::bad_alloc{};
    begin_commands(true,"copy_texture_buffer");auto encoder=[commands_ blitCommandEncoder];MetalEncoderScope encoder_scope{encoder};
    [encoder copyFromTexture:texture sourceSlice:0 sourceLevel:level sourceOrigin:region.origin sourceSize:region.size toBuffer:buffer destinationOffset:0 destinationBytesPerRow:row_bytes destinationBytesPerImage:bytes.size() options:option];
    encoder_scope.end();pending_leases_.push_back(std::move(lease));flush(true);std::memcpy(bytes.data(),buffer.contents,bytes.size());return bytes;
}

void MetalGl::texture_level(GlOperation op,std::span<const std::uint64_t> args,std::span<std::uint8_t> output){
    const auto target=static_cast<unsigned>(args[0]),level=static_cast<unsigned>(args[1]),pname=static_cast<unsigned>(args[2]);
    if(target!=0xde1 && target!=0x8064 && target!=0xde0 && target!=0x8063){error(0x500);return;}if(level>14){error(0x501);return;}
    TextureLevelInfo info{};
    if(target==0x8064 || target==0x8063){if(const auto found=proxy_levels_.find(level+(target==0x8063?16U:0U));found!=proxy_levels_.end())info=found->second;}
    else if(const auto* bound=bound_texture(target);bound->native && bound->formats.contains(level)){
        const auto& t=*bound;info.width=static_cast<unsigned>(std::max<NSUInteger>(1,t.native.width>>level));info.height=static_cast<unsigned>(std::max<NSUInteger>(1,t.native.height>>level));info.format=t.formats.at(level);
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

// 只列出入口與語義都已實作的擴充（wglGetProcAddress 可取得名稱完全相符的入口）；版本字串維持 1.1，其餘功能以擴充公開。
std::string MetalGl::gl_extensions(){
    return "GL_ARB_copy_buffer GL_ARB_framebuffer_object GL_ARB_instanced_arrays GL_ARB_texture_non_power_of_two GL_ARB_vertex_array_object "
           "GL_EXT_blend_color GL_EXT_blend_equation_separate GL_EXT_blend_func_separate GL_EXT_blend_minmax GL_EXT_blend_subtract "
           "GL_EXT_framebuffer_blit GL_EXT_framebuffer_multisample GL_EXT_framebuffer_object GL_EXT_texture_filter_anisotropic";
}
unsigned MetalGl::gl_max_samples(){
    // 色彩、深度與深度模板格式共用同一組樣本數；取裝置支援的最大值。
    for(unsigned count:{8U,4U,2U})if([device_ supportsTextureSampleCount:count])return count;return 0;
}
unsigned MetalGl::gl_framebuffer_samples(bool read){
    const auto id=read?gl_.read_framebuffer:gl_.draw_framebuffer;if(!id)return 1;if(gl_framebuffer_status(read)!=0x8cd5)return 0;
    for(const auto& [slot,attachment]:gl_.framebuffers.at(id)->attachments){(void)slot;return static_cast<unsigned>(attachment.texture.native.sampleCount);}return 1;
}
void MetalGl::gl_renderbuffer_storage(std::span<const std::uint64_t> a){
    // {target,samples,internalformat,width,height}：重新配置目前綁定的 renderbuffer，舊內容捨棄；尺寸為 0 時釋放儲存。
    const auto u=[&](unsigned i){return static_cast<unsigned>(a[i]);};const auto n=[&](unsigned i){return std::bit_cast<int>(u(i));};
    if(u(0)!=0x8d41){error(0x500);return;}
    const auto found=gl_.renderbuffers.find(gl_.renderbuffer);if(!gl_.renderbuffer || found==gl_.renderbuffers.end() || !found->second){error(0x502);return;}
    const auto format=gl_renderbuffer_format(u(2));if(!format){error(0x500);return;}
    if(n(1)<0 || n(3)<0 || n(4)<0 || n(3)>16384 || n(4)>16384 || u(1)>gl_max_samples()){error(0x501);return;}
    // 多重取樣：配置不少於要求的最小支援樣本數；0 為單一樣本。
    unsigned samples=0;if(u(1))for(unsigned count:{2U,4U,8U})if(count>=u(1) && [device_ supportsTextureSampleCount:count]){samples=count;break;}
    auto& object=*found->second;object.format=u(2);object.width=u(3);object.height=u(4);object.samples=samples;object.texture=Texture{};
    if(!u(3) || !u(4))return;
    auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format->format width:u(3) height:u(4) mipmapped:NO];
    desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
    // 色彩單一樣本 renderbuffer 與貼圖相同用共享儲存（glReadPixels 直接讀回）；多重取樣與含模板格式只能私有儲存。
    const bool private_storage=samples || format->format==MTLPixelFormatDepth32Float_Stencil8 || format->format==MTLPixelFormatStencil8;
    desc.storageMode=private_storage?MTLStorageModePrivate:MTLStorageModeShared;
    if(samples){desc.textureType=MTLTextureType2DMultisample;desc.sampleCount=samples;}
    auto lease=reserve_gpu_memory(static_cast<std::uint64_t>(u(3))*u(4)*format->bytes*std::max(1U,samples));
    auto texture=[device_ newTextureWithDescriptor:desc];if(!texture)throw std::bad_alloc{};
    object.texture.native=texture;object.texture.lease=std::move(lease);object.texture.created=true;object.texture.formats[0]=u(2);
}
std::optional<unsigned> MetalGl::gl_attachment_parameter(std::span<const std::uint64_t> a){
    const auto u=[&](unsigned i){return static_cast<unsigned>(a[i]);};
    if(u(0)!=0x8d40 && u(0)!=0x8ca8 && u(0)!=0x8ca9){error(0x500);return std::nullopt;}
    const bool read=u(0)==0x8ca8;const auto id=read?gl_.read_framebuffer:gl_.draw_framebuffer;const auto slot=u(1),pname=u(2);
    if(!id){
        // 預設 framebuffer：FRONT_LEFT、BACK_LEFT、DEPTH、STENCIL 存在（RGBA8、24 位元深度、8 位元模板），右眼緩衝不存在。
        if(slot<0x400 || (slot>0x403 && slot!=0x1801 && slot!=0x1802)){error(0x500);return std::nullopt;}
        const bool exists=slot==0x400 || slot==0x402 || slot==0x1801 || slot==0x1802;if(pname==0x8cd0)return exists?0x8218U:0U;
        if(!exists){error(0x500);return std::nullopt;}
        const bool color=slot<0x1801;
        switch(pname){case 0x8210:return 0x2601U;case 0x8211:return slot==0x1802?0x1405U:0x8c17U;
        case 0x8212:case 0x8213:case 0x8214:case 0x8215:return color?8U:0U;case 0x8216:return slot==0x1801?24U:0U;case 0x8217:return slot==0x1802?8U:0U;default:error(0x500);return std::nullopt;}
    }
    if((slot<0x8ce0 || slot>=0x8ce8) && slot!=0x8d00 && slot!=0x8d20 && slot!=0x821a){error(0x500);return std::nullopt;}
    gl_framebuffer_status(read);auto& fb=*gl_.framebuffers.at(id);const GlAttachment* attachment=nullptr;
    if(slot==0x821a){
        // DEPTH_STENCIL_ATTACHMENT：深度與模板附件必須是同一個物件。
        const auto depth=fb.attachments.find(0x8d00),stencil=fb.attachments.find(0x8d20);
        if(depth!=fb.attachments.end() && stencil!=fb.attachments.end() && (depth->second.name!=stencil->second.name || bool(depth->second.renderbuffer)!=bool(stencil->second.renderbuffer))){error(0x502);return std::nullopt;}
        attachment=depth!=fb.attachments.end()?&depth->second:stencil!=fb.attachments.end()?&stencil->second:nullptr;
    }else if(const auto it=fb.attachments.find(slot);it!=fb.attachments.end())attachment=&it->second;
    if(!attachment){if(pname==0x8cd0 || pname==0x8cd1)return 0U;error(0x500);return std::nullopt;}
    const bool renderbuffer=attachment->renderbuffer!=nullptr;const auto texture=attachment->texture.native;
    const auto bits=renderbuffer?[&]{const auto format=gl_renderbuffer_format(attachment->renderbuffer->format);return format?format->bits:std::array<unsigned,6>{};}():gl_pixel_format_bits(texture?texture.pixelFormat:MTLPixelFormatInvalid);
    switch(pname){
    case 0x8cd0:return renderbuffer?0x8d41U:0x1702U;case 0x8cd1:return attachment->name;
    case 0x8cd2:case 0x8cd3:case 0x8cd4:if(renderbuffer){error(0x500);return std::nullopt;}return pname==0x8cd2?attachment->level:0U;
    case 0x8210:return 0x2601U;
    case 0x8211:{
        if(slot==0x821a){error(0x502);return std::nullopt;}if(slot==0x8d20)return 0x1405U;
        const auto format=texture?texture.pixelFormat:MTLPixelFormatInvalid;
        const bool floating=format==MTLPixelFormatR16Float || format==MTLPixelFormatRG16Float || format==MTLPixelFormatRGBA16Float || format==MTLPixelFormatR32Float || format==MTLPixelFormatRG32Float ||
            format==MTLPixelFormatRGBA32Float || format==MTLPixelFormatRG11B10Float || format==MTLPixelFormatDepth32Float || format==MTLPixelFormatDepth32Float_Stencil8;
        return floating?0x1406U:0x8c17U;}
    case 0x8212:case 0x8213:case 0x8214:case 0x8215:case 0x8216:case 0x8217:
        if(slot==0x821a && pname!=0x8216 && pname!=0x8217){error(0x502);return std::nullopt;}
        // 單一附加點只回報該點用到的平面（深度附件不回報模板位元，反之亦然）。
        if((slot==0x8d00 && pname==0x8217) || (slot==0x8d20 && pname==0x8216))return 0U;return bits[pname-0x8212];
    default:error(0x500);return std::nullopt;}
}
void MetalGl::gl_blit(std::span<const std::uint64_t> a){@autoreleasepool{
    const auto n=[&](unsigned i){return std::bit_cast<int>(static_cast<unsigned>(a[i]));};const auto mask=static_cast<unsigned>(a[8]),filter=static_cast<unsigned>(a[9]);
    if(mask&~0x4500U){error(0x501);return;}if(filter!=0x2600 && filter!=0x2601){error(0x500);return;}if((mask&0x500U) && filter!=0x2600){error(0x502);return;}
    if(gl_framebuffer_status(true)!=0x8cd5 || gl_framebuffer_status(false)!=0x8cd5){error(0x506);return;}
    const std::int64_t sx0=n(0),sy0=n(1),sx1=n(2),sy1=n(3),dx0=n(4),dy0=n(5),dx1=n(6),dy1=n(7);
    if(!mask || sx0==sx1 || sy0==sy1 || dx0==dx1 || dy0==dy1)return;
    // 表面：window 為預設 framebuffer（Metal 列序上下顛倒）；framebuffer 物件的列序與 GL 相同。
    struct Surface {id<MTLTexture> texture;unsigned level{};std::shared_ptr<GpuMemoryReservation> lease;bool window{};};
    auto* read_fb=gl_.read_framebuffer?gl_.framebuffers.at(gl_.read_framebuffer).get():nullptr;auto* draw_fb=gl_.draw_framebuffer?gl_.framebuffers.at(gl_.draw_framebuffer).get():nullptr;
    const auto attachment=[&](GlFramebuffer* fb,unsigned slot)->std::optional<Surface>{const auto it=fb->attachments.find(slot);if(it==fb->attachments.end())return std::nullopt;return Surface{it->second.texture.native,it->second.level,it->second.texture.lease,false};};
    std::optional<Surface> source_color,source_depth,source_stencil;std::vector<Surface> target_colors;std::optional<Surface> target_depth,target_stencil;
    if(read_fb){if(read_fb->read_buffer)source_color=attachment(read_fb,read_fb->read_buffer);source_depth=attachment(read_fb,0x8d00);source_stencil=attachment(read_fb,0x8d20);}
    else{const auto b=state_.read_buffer;if(b)source_color=Surface{b==0x404 || b==0x400?front_:color_,0,{},true};source_depth=source_stencil=Surface{depth_,0,{},true};}
    if(draw_fb){for(const auto target:draw_fb->draw_buffers)if(target)if(auto surface=attachment(draw_fb,target))target_colors.push_back(*surface);target_depth=attachment(draw_fb,0x8d00);target_stencil=attachment(draw_fb,0x8d20);}
    else{const auto b=state_.draw_buffer;if(b==0x404 || b==0x400 || b==0x408 || b==0x406)target_colors.push_back({front_,0,{},true});if(b==0x405 || b==0x402 || b==0x408 || b==0x406)target_colors.push_back({color_,0,{},true});
        target_depth=target_stencil=Surface{depth_,0,{},true};}
    // 遮罩中的緩衝在讀或寫任一方不存在時，該位元直接忽略。
    const bool color=(mask&0x4000U) && source_color && !target_colors.empty(),depth=(mask&0x100U) && source_depth && target_depth,stencil=(mask&0x400U) && source_stencil && target_stencil;
    if(!color && !depth && !stencil)return;
    // 多重取樣：任一方為多重取樣時兩個矩形必須相同、格式必須相同；雙方樣本數不同時為 INVALID_OPERATION。
    const auto check=[&](const Surface& source,const Surface& target){
        const auto s=source.texture.sampleCount,t=target.texture.sampleCount;if(s==1 && t==1)return true;
        if((s>1 && t>1 && s!=t) || sx1-sx0!=dx1-dx0 || sy1-sy0!=dy1-dy0 || source.texture.pixelFormat!=target.texture.pixelFormat)return false;return true;};
    if(color)for(const auto& target:target_colors)if(!check(*source_color,target)){error(0x502);return;}
    if(depth && (source_depth->texture.pixelFormat!=target_depth->texture.pixelFormat || !check(*source_depth,*target_depth))){error(0x502);return;}
    if(stencil && (source_stencil->texture.pixelFormat!=target_stencil->texture.pixelFormat || !check(*source_stencil,*target_stencil))){error(0x502);return;}
    const auto level_size=[](const Surface& s){return std::array<std::int64_t,2>{static_cast<std::int64_t>(std::max<NSUInteger>(1,s.texture.width>>s.level)),static_cast<std::int64_t>(std::max<NSUInteger>(1,s.texture.height>>s.level))};};
    // 目的矩形與剪裁框（GL 座標）交集；空集合時不寫入。
    const auto clipped=[&](const Surface& target)->std::optional<std::array<std::int64_t,4>>{
        const auto [w,h]=level_size(target);std::int64_t x0=std::max<std::int64_t>(std::min(dx0,dx1),0),y0=std::max<std::int64_t>(std::min(dy0,dy1),0),x1=std::min(std::max(dx0,dx1),w),y1=std::min(std::max(dy0,dy1),h);
        if(state_.enabled.contains(0xc11)){const auto& r=state_.scissor;x0=std::max<std::int64_t>(x0,r[0]);y0=std::max<std::int64_t>(y0,r[1]);x1=std::min<std::int64_t>(x1,static_cast<std::int64_t>(r[0])+r[2]);y1=std::min<std::int64_t>(y1,static_cast<std::int64_t>(r[1])+r[3]);}
        if(x1<=x0 || y1<=y0)return std::nullopt;return std::array<std::int64_t,4>{x0,y0,x1,y1};};
    const auto keep=[&](const Surface& s){if(s.lease)pending_leases_.push_back(s.lease);};
    // 暫存可讀副本：來源沒有 ShaderRead／PixelFormatView 用途、與目的是同一張貼圖，或為多重取樣（色彩取平均、深度模板取第 0 個樣本）。
    const auto readable=[&](Surface& s,bool resolve_color,bool resolve_depth,bool resolve_stencil){
        auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s.texture.pixelFormat width:s.texture.width height:s.texture.height mipmapped:s.texture.mipmapLevelCount>1];
        desc.mipmapLevelCount=s.texture.sampleCount>1?1:s.texture.mipmapLevelCount;desc.storageMode=MTLStorageModePrivate;desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
        const auto bits=gl_pixel_format_bits(s.texture.pixelFormat);unsigned bytes=0;for(auto b:bits)bytes+=b;
        auto lease=reserve_gpu_memory(static_cast<std::uint64_t>(s.texture.width)*s.texture.height*std::max(4U,bytes/8U)*2U);auto copy=[device_ newTextureWithDescriptor:desc];if(!copy)throw std::bad_alloc{};keep(s);
        if(s.texture.sampleCount>1){
            auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
            if(resolve_color){auto c=pass.colorAttachments[0];c.texture=s.texture;c.loadAction=MTLLoadActionLoad;c.storeAction=MTLStoreActionStoreAndMultisampleResolve;c.resolveTexture=copy;}
            if(resolve_depth){auto d=pass.depthAttachment;d.texture=s.texture;d.loadAction=MTLLoadActionLoad;d.storeAction=MTLStoreActionStoreAndMultisampleResolve;d.resolveTexture=copy;d.depthResolveFilter=MTLMultisampleDepthResolveFilterSample0;}
            if(resolve_stencil){auto t=pass.stencilAttachment;t.texture=s.texture;t.loadAction=MTLLoadActionLoad;t.storeAction=MTLStoreActionStoreAndMultisampleResolve;t.resolveTexture=copy;t.stencilResolveFilter=MTLMultisampleStencilResolveFilterSample0;}
            begin_commands(true,"resolve");auto encoder=[commands_ renderCommandEncoderWithDescriptor:pass];MetalEncoderScope encoder_scope{encoder};encoder_scope.end();
        }else{begin_commands(true,"copy");auto encoder=[commands_ blitCommandEncoder];MetalEncoderScope encoder_scope{encoder};[encoder copyFromTexture:s.texture toTexture:copy];encoder_scope.end();}
        pending_leases_.push_back(lease);s=Surface{copy,s.texture.sampleCount>1?0U:s.level,lease,s.window};};
    const auto same=[&](const Surface& s,std::initializer_list<const Surface*> targets){for(const auto* t:targets)if(t && t->texture==s.texture)return true;return false;};
    // 參數：目的起點與每像素來源步長、來源起點、來源／目的視窗翻轉高度、來源層寬高與層級（fixed_gl.metal 的 blit_source）。
    const auto params=[&](const Surface& source,const Surface& target){
        const auto [sw,sh]=level_size(source);const auto [tw,th]=level_size(target);(void)tw;
        return std::array<float,12>{static_cast<float>(dx0),static_cast<float>(dy0),static_cast<float>(sx1-sx0)/static_cast<float>(dx1-dx0),static_cast<float>(sy1-sy0)/static_cast<float>(dy1-dy0),
            static_cast<float>(sx0),static_cast<float>(sy0),source.window?static_cast<float>(sh):0.F,target.window?static_cast<float>(th):0.F,static_cast<float>(sw),static_cast<float>(sh),0.F,static_cast<float>(source.level)};};
    const auto pipeline=[&](NSString* fragment,MTLPixelFormat color_format,MTLPixelFormat depth_format,MTLPixelFormat stencil_format,NSUInteger samples)->id<MTLRenderPipelineState>{
        const auto key="blit:"+std::string(fragment.UTF8String)+":"+std::to_string(color_format)+":"+std::to_string(depth_format)+":"+std::to_string(stencil_format)+":"+std::to_string(samples);
        if(const auto found=gl_pipelines_.get(key))return *found;
        auto desc=[MTLRenderPipelineDescriptor new];desc.vertexFunction=[library_ newFunctionWithName:@"blit_vertex"];desc.fragmentFunction=[library_ newFunctionWithName:fragment];
        desc.colorAttachments[0].pixelFormat=color_format;desc.depthAttachmentPixelFormat=depth_format;desc.stencilAttachmentPixelFormat=stencil_format;desc.rasterSampleCount=samples;
        NSError* failure=nil;id<MTLRenderPipelineState> state=[device_ newRenderPipelineStateWithDescriptor:desc error:&failure];if(!state)throw std::runtime_error(std::string("OpenGL blit pipeline: ")+failure.localizedDescription.UTF8String);
        gl_pipelines_.put(key,state);return state;};
    const auto scissor=[&](const Surface& target,const std::array<std::int64_t,4>& r){const auto [w,h]=level_size(target);(void)w;
        return MTLScissorRect{static_cast<NSUInteger>(r[0]),static_cast<NSUInteger>(target.window?h-r[3]:r[1]),static_cast<NSUInteger>(r[2]-r[0]),static_cast<NSUInteger>(r[3]-r[1])};};
    // 雙方都是多重取樣（只會是 framebuffer 物件，列序相同）：依剪裁後區域逐樣本複製。
    const auto copy_samples=[&](const Surface& source,const Surface& target){
        const auto r=clipped(target);if(!r)return;const auto [sw,sh]=level_size(source);const auto ox=sx0-dx0,oy=sy0-dy0;
        const auto x0=std::max((*r)[0],-ox),y0=std::max((*r)[1],-oy),x1=std::min((*r)[2],sw-ox),y1=std::min((*r)[3],sh-oy);if(x1<=x0 || y1<=y0)return;
        begin_commands(true,"copy");auto encoder=[commands_ blitCommandEncoder];MetalEncoderScope encoder_scope{encoder};
        [encoder copyFromTexture:source.texture sourceSlice:0 sourceLevel:source.level sourceOrigin:MTLOriginMake(static_cast<NSUInteger>(x0+ox),static_cast<NSUInteger>(y0+oy),0) sourceSize:MTLSizeMake(static_cast<NSUInteger>(x1-x0),static_cast<NSUInteger>(y1-y0),1)
            toTexture:target.texture destinationSlice:0 destinationLevel:target.level destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(x0),static_cast<NSUInteger>(y0),0)];
        encoder_scope.end();keep(source);keep(target);};
    if(color){
        auto source=*source_color;
        const bool both_multisample=source.texture.sampleCount>1 && std::all_of(target_colors.begin(),target_colors.end(),[](const Surface& t){return t.texture.sampleCount>1;});
        if(both_multisample){for(const auto& target:target_colors)copy_samples(source,target);}
        else{
            std::vector<const Surface*> targets;for(const auto& t:target_colors)targets.push_back(&t);
            if(source.texture.sampleCount>1 || !(source.texture.usage&MTLTextureUsageShaderRead) || std::any_of(targets.begin(),targets.end(),[&](const Surface* t){return t->texture==source.texture;}))readable(source,true,false,false);
            auto sampler_desc=[MTLSamplerDescriptor new];sampler_desc.minFilter=sampler_desc.magFilter=filter==0x2601?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;sampler_desc.mipFilter=MTLSamplerMipFilterNearest;
            sampler_desc.sAddressMode=sampler_desc.tAddressMode=MTLSamplerAddressModeClampToEdge;auto sampler=cached_sampler(sampler_desc);graphics_pending_.push_back(sampler);
            for(const auto& target:target_colors){const auto r=clipped(target);if(!r)continue;const auto p=params(source,target);const auto [w,h]=level_size(target);
                auto pass=[MTLRenderPassDescriptor renderPassDescriptor];auto c=pass.colorAttachments[0];c.texture=target.texture;c.level=target.level;c.loadAction=MTLLoadActionLoad;c.storeAction=MTLStoreActionStore;
                auto state=pipeline(@"blit_color",target.texture.pixelFormat,MTLPixelFormatInvalid,MTLPixelFormatInvalid,target.texture.sampleCount);
                begin_commands(true,"blit");auto encoder=[commands_ renderCommandEncoderWithDescriptor:pass];MetalEncoderScope encoder_scope{encoder};
                [encoder setRenderPipelineState:state];[encoder setViewport:MTLViewport{0,0,static_cast<double>(w),static_cast<double>(h),0,1}];[encoder setScissorRect:scissor(target,*r)];
                [encoder setFragmentTexture:source.texture atIndex:0];[encoder setFragmentSamplerState:sampler->sampler atIndex:0];[encoder setFragmentBytes:p.data() length:sizeof(p) atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];encoder_scope.end();keep(target);}
            keep(source);
        }
    }
    if(depth || stencil){
        // 深度與模板：同一張深度模板貼圖時一次畫完；多重取樣雙方逐樣本複製。
        auto target=depth?*target_depth:*target_stencil;auto source=depth?*source_depth:*source_stencil;
        const bool combined=depth && stencil && target_depth->texture==target_stencil->texture && source_depth->texture==source_stencil->texture;
        const auto run=[&](bool write_depth,bool write_stencil,Surface src,const Surface& dst){
            if(src.texture.sampleCount>1 && dst.texture.sampleCount>1){copy_samples(src,dst);return;}
            const auto r=clipped(dst);if(!r)return;
            if(src.texture.sampleCount>1 || !(src.texture.usage&MTLTextureUsageShaderRead) || !(src.texture.usage&MTLTextureUsagePixelFormatView) || same(src,{&dst}))readable(src,false,write_depth,write_stencil);
            const auto p=params(src,dst);const auto [w,h]=level_size(dst);
            auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
            if(write_depth){auto d=pass.depthAttachment;d.texture=dst.texture;d.level=dst.level;d.loadAction=MTLLoadActionLoad;d.storeAction=MTLStoreActionStore;}
            if(write_stencil){auto t=pass.stencilAttachment;t.texture=dst.texture;t.level=dst.level;t.loadAction=MTLLoadActionLoad;t.storeAction=MTLStoreActionStore;}
            auto state=pipeline(write_depth && write_stencil?@"blit_depth_stencil":write_depth?@"blit_depth":@"blit_stencil",MTLPixelFormatInvalid,write_depth?dst.texture.pixelFormat:MTLPixelFormatInvalid,write_stencil?dst.texture.pixelFormat:MTLPixelFormatInvalid,dst.texture.sampleCount);
            auto ds=[MTLDepthStencilDescriptor new];ds.depthCompareFunction=MTLCompareFunctionAlways;ds.depthWriteEnabled=write_depth;
            if(write_stencil){auto stencil_state=make_metal_stencil(MTLCompareFunctionAlways,MTLStencilOperationKeep,MTLStencilOperationKeep,MTLStencilOperationReplace,0xff,0xff);ds.frontFaceStencil=stencil_state;ds.backFaceStencil=stencil_state;}
            id<MTLTexture> stencil_view=nil;if(write_stencil){stencil_view=src.texture.pixelFormat==MTLPixelFormatDepth32Float_Stencil8?[src.texture newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8]:src.texture;if(!stencil_view)throw std::bad_alloc{};}
            begin_commands(true,"blit");auto encoder=[commands_ renderCommandEncoderWithDescriptor:pass];MetalEncoderScope encoder_scope{encoder};
            [encoder setRenderPipelineState:state];[encoder setDepthStencilState:cached_depth_state(ds)];[encoder setViewport:MTLViewport{0,0,static_cast<double>(w),static_cast<double>(h),0,1}];[encoder setScissorRect:scissor(dst,*r)];
            if(write_depth)[encoder setFragmentTexture:src.texture atIndex:0];if(write_stencil)[encoder setFragmentTexture:stencil_view atIndex:1];[encoder setFragmentBytes:p.data() length:sizeof(p) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];encoder_scope.end();keep(src);keep(dst);};
        if(combined || !(depth && stencil))run(depth,stencil && (combined || !depth),source,target);
        else{run(true,false,*source_depth,*target_depth);run(false,true,*source_stencil,*target_stencil);}
    }
}}
