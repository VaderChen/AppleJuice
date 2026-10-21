    // mask: 0 render、1 sampler、2 texture stage、3 transform、4 binding/viewport/scissor、5..10 shader register。
    const auto state_binding=[](Object& object,unsigned index)->std::uint64_t&{switch(index){case 0:return object.shaders[0];case 1:return object.shaders[1];case 2:return object.vertex_declaration;case 3:return object.texture;case 4:return object.vertex_buffer;default:return object.index_buffer;}};
    const auto state_copy=[&](Object& target,const Object& source,const std::set<std::pair<unsigned,unsigned>>& mask,bool bind){
        for(auto [category,index]:mask){
            if(category==0)target.render[index]=source.render.at(index);
            else if(category==1)target.sampler[index]=source.sampler.at(index);
            else if(category==2)target.stage[index]=source.stage.at(index);
            else if(category==3)target.transforms[index]=source.transforms.at(index);
            else if(category==4){if(index<6){const auto pointer=saved_bindings(source)[index];auto& destination=state_binding(target,index);if(bind){if(pointer)++bridge.objects.at(pointer)->bound;drop_binding(destination);}destination=pointer;
                    if(index==2)target.fvf=source.fvf;if(index==4){target.stream_stride=source.stream_stride;target.stream_offset=source.stream_offset;}}
                else if(index==6)target.viewport=source.viewport;else target.scissor=source.scissor;
            }else if(category==15){auto& destination=target.extra_streams.at(index);const auto& input=source.extra_streams.at(index);if(bind){if(input.buffer)++bridge.objects.at(input.buffer)->bound;drop_binding(destination.buffer);}destination=input;}else if(category==16)target.stream_frequency.at(index)=source.stream_frequency.at(index);else if(category==14){auto& destination=target.extra_textures.at(index);const auto pointer=source.extra_textures.at(index);if(bind){if(pointer)++bridge.objects.at(pointer)->bound;drop_binding(destination);}destination=pointer;}else if(category==11)target.material=source.material;else if(category==12)target.lights[index]=source.lights.at(index);else if(category==13)target.light_enabled[index]=source.light_enabled.at(index);else{const unsigned stage=(category-5)/3,kind=(category-5)%3,stride=kind==2?4U:16U,limit=kind?16U:stage?224U:256U;auto& values=target.constants[stage][kind];if(values.empty())values.resize(limit*stride);const auto& input=source.constants[stage][kind];for(unsigned b=0;b<stride;++b)values[index*stride+b]=input.empty()?0:input.at(index*stride+b);}
        }
    };
    const auto capture_state=[&](const std::shared_ptr<Object>& block,const Object& source){
        auto captured=std::make_shared<Object>();state_copy(*captured,source,block->state_mask,false);for(auto pointer:saved_bindings(*captured))if(pointer)hold(bridge.objects.at(pointer));
        if(block->saved)for(auto pointer:saved_bindings(*block->saved))if(pointer)release(pointer);block->saved=std::move(captured);
    };
    const auto apply_state=[&](Object& target,const Object& source,const auto& mask){state_copy(target,source,mask,true);};
    const auto state_mask=[](const Object& device,unsigned type){
        std::set<std::pair<unsigned,unsigned>> mask;
        // 依 Microsoft 的 vertex/pixel stateblock 分類；只列本引擎實際持有的狀態。
        const std::set<unsigned> vertex_render{22,34,35,36,37,38,48,137,139,140,141,142,143,145,146,147,148,151,152,154,155,156,157,158,159,160,161,162,163,166,167,170,172,173,178,179,180,181,182,183,184};
        for(const auto& [key,v]:device.render){(void)v;if(type==1 || (type==3?vertex_render.contains(key):(!vertex_render.contains(key) || key==36 || key==37 || key==38 || key==142 || (key>=145 && key<=148))))mask.emplace(0,key);}
        for(const auto& [key,v]:device.sampler){(void)v;if(type==1 || (type==3?(key/32>=16 || key%32==13):(key/32<16 && key%32!=13)))mask.emplace(1,key);}
        for(const auto& [key,v]:device.stage){(void)v;if(type==1 || (type==3?(key%32==11 || key%32==24):true))mask.emplace(2,key);}
        if(type==1){for(const auto& [key,v]:device.transforms){(void)v;mask.emplace(3,key);}for(unsigned i=0;i<8;++i)mask.emplace(4,i);for(unsigned i=0;i<15;++i)mask.emplace(15,i);for(unsigned i=0;i<16;++i)mask.emplace(16,i);}
        else if(type==2){mask.emplace(4,1);mask.emplace(4,3);}else {mask.emplace(4,0);mask.emplace(4,2);}
        for(unsigned index=0;index<19;++index)if(type==1 || (type==2?index<15:index>=15))mask.emplace(14,index);
        if(type==1 || type==3){mask.emplace(11,0);for(const auto& [index,light]:device.lights){(void)light;mask.emplace(12,index);mask.emplace(13,index);}}
        for(unsigned stage=0;stage<2;++stage)if(type==1 || (type==2?stage==1:stage==0))for(unsigned kind=0;kind<3;++kind){const unsigned limit=kind?16U:stage?224U:256U;for(unsigned i=0;i<limit;++i)mask.emplace(5+stage*3+kind,i);}return mask;
    };
    const auto recordable=[](std::string_view name){return name=="SetMaterial" || name=="SetLight" || name=="LightEnable" || name=="SetRenderState" || name=="SetSamplerState" || name=="SetTextureStageState" || name=="SetTransform" || name=="SetFVF" || name=="SetVertexDeclaration" || name=="SetVertexShader" || name=="SetPixelShader" || name=="SetTexture" || name=="SetStreamSource" || name=="SetStreamSourceFreq" || name=="SetIndices" || name=="SetViewport" || name=="SetScissorRect" || name.starts_with("SetVertexShaderConstant") || name.starts_with("SetPixelShaderConstant");};
    const auto record_state=[](Object& device,std::string_view name,unsigned a,unsigned b,unsigned c){
        if(name=="SetMaterial")device.state_mask.emplace(11,0);else if(name=="SetLight"){device.state_mask.emplace(12,a);device.state_mask.emplace(13,a);}else if(name=="LightEnable"){device.state_mask.emplace(12,a);device.state_mask.emplace(13,a);}else if(name=="SetRenderState")device.state_mask.emplace(0,a);else if(name=="SetSamplerState")device.state_mask.emplace(1,texture_slot(a)*32+b);else if(name=="SetTextureStageState")device.state_mask.emplace(2,a*32+b);else if(name=="SetTransform")device.state_mask.emplace(3,a);
        else if(name.starts_with("SetVertexShaderConstant") || name.starts_with("SetPixelShaderConstant")){const unsigned stage=name.starts_with("SetPixel")?1:0,kind=name.back()=='F'?0U:name.back()=='I'?1U:2U;for(unsigned i=0;i<c;++i)device.state_mask.emplace(5+stage*3+kind,a+i);}
        else if(name=="SetStreamSourceFreq")device.state_mask.emplace(16,a);
        else if(name=="SetStreamSource" && a)device.state_mask.emplace(15,a-1);
        else if(name=="SetTexture" && a)device.state_mask.emplace(14,texture_slot(a)-1);
        else{const unsigned index=name=="SetVertexShader"?0U:name=="SetPixelShader"?1U:(name=="SetFVF" || name=="SetVertexDeclaration")?2U:name=="SetTexture"?3U:name=="SetStreamSource"?4U:name=="SetIndices"?5U:name=="SetViewport"?6U:7U;device.state_mask.emplace(4,index);}
    };
