        const auto fx_string=[&](const std::shared_ptr<Identity>& owner,const std::string& text){
            if(text.empty())return std::uint64_t{};const auto key="string:"+text;if(owner->effect_handles.contains(key))return owner->effect_handles.at(key);
            const auto at=allocate_heap(memory,text.size()+1);if(!at)throw std::bad_alloc{};owner->allocations.push_back(at);memory.write(GuestAddress{at},std::span(reinterpret_cast<const std::uint8_t*>(text.c_str()),text.size()+1));owner->effect_handles[key]=at;return at;
        };
        const auto fx_owner=[&](const std::shared_ptr<Identity>& id){return id->effect_owner?object(id->effect_owner).identity:id;};
        const auto fx_handle=[&](const std::shared_ptr<Identity>& owner,Kind kind,const std::string& key){
            if(owner->effect_handles.contains(key))return object(owner->effect_handles.at(key)).identity;
            auto id=create(kind);id->effect=owner->effect;id->effect_owner=owner->root;owner->bindings["EffectHandles"].push_back(id->root);owner->effect_handles[key]=id->root;return id;
        };
        const auto fx_variable=[&](const std::shared_ptr<Identity>& owner,unsigned index){
            auto id=fx_handle(owner,Kind::EffectVariable10,"variable:"+std::to_string(index));id->effect_variable=index;id->effect_valid=index<owner->effect->variables.size();
            if(id->effect_valid){const auto& v=owner->effect->variables[index];id->effect_type=v.type;id->effect_buffer=v.buffer;id->effect_offset=v.offset;}return id;
        };
        const auto fx_find_variable=[&](const std::shared_ptr<Identity>& owner,const std::string& name){const auto& vars=owner->effect->variables;const auto found=std::find_if(vars.begin(),vars.end(),[&](const auto& v){return v.name==name;});return found==vars.end()?UINT32_MAX:static_cast<unsigned>(found-vars.begin());};
        const auto fx_shader=[&](const std::shared_ptr<Identity>& owner,unsigned stage,const std::vector<std::uint8_t>& bytes,const std::string& key,std::string_view so={}){
            if(owner->effect_handles.contains(key))return owner->effect_handles.at(key);if(bytes.empty())return std::uint64_t{};
            auto device=object(owner->device).identity;auto reflected=std::make_shared<dxbc::Reflection>(bytes);auto shader=device->native->graphics_shader(stage==2 && (reflected->version>>16)==1?0U:stage,bytes);if(!so.empty())stream_output::parse(*shader,*reflected,so);
            auto id=create(stage==0?Kind::VertexShader:stage==1?Kind::PixelShader:Kind::GeometryShader,owner->device);id->shader=std::move(shader);id->reflection=std::move(reflected);id->desc=bytes;owner->bindings["EffectShaders"].push_back(id->root);owner->effect_handles[key]=id->root;return id->root;
        };
        const auto fx_create=[&](std::uint64_t device_pointer,std::span<const std::uint8_t> bytes){
            auto device=typed(device_pointer,Kind::Device);require(device->api10);auto program=std::make_shared<fx10::Program>(bytes);auto owner=create(Kind::Effect10,device_pointer);owner->effect=std::move(program);
            try{
                for(unsigned i=0;i<owner->effect->buffers.size();++i){const auto& b=owner->effect->buffers[i];GraphicsResourceDesc desc;desc.width=static_cast<std::uint32_t>((b.bytes.size()+15)&~std::size_t{15});desc.bind=(b.flags&1)?8U:4U;if(!desc.width)desc.width=16;
                    auto resource=device->native->graphics_resource(desc);auto id=create(Kind::BufferObject,device_pointer);id->resource=std::move(resource);id->desc.resize(24);put_word(id->desc,0,desc.width);put_word(id->desc,8,desc.bind);owner->bindings["EffectBuffer:"+std::to_string(i)]={id->root};
                    if(b.flags&1){auto view=create(Kind::ShaderResource,device_pointer);view->texture=id->root;hold(id->root);view->graphics_view={id->resource,2,0,1,0,1};view->graphics_view.count=desc.width;view->graphics_view.dimension=1;view->desc.resize(24);put_word(view->desc,0,2);put_word(view->desc,4,1);put_word(view->desc,12,desc.width/16);owner->bindings["EffectBufferView:"+std::to_string(i)]={view->root};}
                }
                for(unsigned i=0;i<owner->effect->variables.size();++i){const auto& v=owner->effect->variables[i];if(v.type->kind!=2)continue;auto& objects=owner->bindings["EffectObject:"+std::to_string(i)];objects.resize(v.type->count());
                    if(v.type->detail>=5 && v.type->detail<=8)for(unsigned j=0;j<v.objects.size();++j){const auto shader=fx_shader(owner,v.type->detail==6?0U:v.type->detail==5?1U:2U,owner->effect->blob(v.objects[j].data),"shader:"+std::to_string(i)+":"+std::to_string(j),owner->effect->string(v.objects[j].stream_output));if(shader)hold(shader);objects[j]=shader;}
                }
            }catch(...){release(owner->root);throw;}return owner;
        };
        const auto fx_numeric=[&](const std::shared_ptr<Identity>& owner,const fx10::Assignment& a){
            std::vector<std::pair<unsigned,std::uint32_t>> values;
            if(a.kind==1){const auto count=fx10::Program::bounded(owner->effect->get(a.offset));for(unsigned i=0;i<count;++i)values.emplace_back(owner->effect->get(a.offset+4+i*8),owner->effect->get(a.offset+8+i*8));}
            else if(a.kind==2){const auto index=fx_find_variable(owner,owner->effect->string(a.offset));require(index!=UINT32_MAX);const auto& v=owner->effect->variables[index];require(v.type->kind==1 && v.buffer<owner->effect->buffers.size());const auto& bytes=owner->effect->buffers[v.buffer].bytes;for(unsigned i=0;i<v.type->columns();++i)values.emplace_back(v.type->scalar(),word(bytes,v.offset+i*4));}
            else throw std::invalid_argument("FX4 數值 assignment 尚需 expression 解碼");return values;
        };
        const auto fx_integer=[&](const std::pair<unsigned,std::uint32_t>& value){return value.first==1?static_cast<std::uint32_t>(static_cast<std::int32_t>(std::bit_cast<float>(value.second))):value.second;};
        const auto fx_float=[&](const std::pair<unsigned,std::uint32_t>& value){return value.first==1?value.second:value.first==4?(value.second?0x3f800000U:0U):std::bit_cast<std::uint32_t>(value.first==2?static_cast<float>(static_cast<std::int32_t>(value.second)):static_cast<float>(value.second));};
        const auto fx_state=[&](const std::shared_ptr<Identity>& owner,unsigned variable,unsigned element){
            const auto& v=owner->effect->variables.at(variable);const auto type=v.type->detail;require(element<v.objects.size());const bool blend=type==2,depth=type==3,raster=type==4;require(blend || depth || raster || type==21);
            std::vector<std::uint8_t> bytes(blend?264:depth?52:raster?40:52);
            if(blend){put_word(bytes,4,1);for(unsigned i=0;i<8;++i){const auto at=8+i*32;put_word(bytes,at+4,2);put_word(bytes,at+8,1);put_word(bytes,at+12,1);put_word(bytes,at+16,2);put_word(bytes,at+20,1);put_word(bytes,at+24,1);bytes[at+28]=15;}}
            else if(depth){put_word(bytes,0,1);put_word(bytes,4,1);put_word(bytes,8,2);bytes[16]=bytes[17]=255;for(unsigned at:{20U,36U}){put_word(bytes,at,1);put_word(bytes,at+4,1);put_word(bytes,at+8,1);put_word(bytes,at+12,8);}}
            else if(raster){put_word(bytes,0,3);put_word(bytes,4,3);put_word(bytes,24,1);}
            else {put_word(bytes,0,0x15);put_word(bytes,4,3);put_word(bytes,8,3);put_word(bytes,12,3);put_word(bytes,20,16);put_word(bytes,24,1);put_word(bytes,44,0xff7fffffU);put_word(bytes,48,0x7f7fffffU);}
            for(const auto& a:v.objects[element].assignments){if(a.state==55)continue;const auto values=fx_numeric(owner,a);require(!values.empty());unsigned offset=UINT32_MAX;bool floating=false,byte=false;
                if(raster && a.state>=12 && a.state<=21){offset=(a.state-12)*4;floating=a.state==16 || a.state==17;}
                else if(depth && a.state>=22 && a.state<=35){offset=a.state<26?(a.state-22)*4:a.state==26?16:a.state==27?17:20+(a.state-28)*4;byte=a.state==26 || a.state==27;}
                else if(blend && a.state>=36 && a.state<=44){require(a.index<8);offset=a.state==36?0:8+a.index*32+(a.state-37)*4;byte=a.state==44;}
                else if(type==21 && a.state>=45 && a.state<=54){offset=a.state<=51?(a.state-45)*4:a.state==52?28:a.state==53?44:48;floating=a.state==49 || a.state>=52;}
                require(offset!=UINT32_MAX);for(unsigned i=0;i<values.size();++i){const auto bits=floating?fx_float(values[i]):fx_integer(values[i]);if(byte){require(i==0 && offset<bytes.size());bytes[offset]=static_cast<std::uint8_t>(bits);}else put_word(bytes,offset+i*4,bits);}
                // fx_4_0 的混色運算由八個 render target 共用；4.1 才有逐目標運算。
                if(blend && !owner->effect->independent_blend && a.state>=38 && a.state<=43){require(a.index==0 && values.size()==1);for(unsigned i=1;i<8;++i)put_word(bytes,offset+i*32,fx_integer(values[0]));}
            }
            const auto key="state:"+std::to_string(variable)+":"+std::to_string(element);auto found=owner->effect_handles.find(key);if(found!=owner->effect_handles.end() && object(found->second).identity->desc==bytes)return found->second;
            auto device=object(owner->device).identity;std::shared_ptr<GraphicsSampler> sampler;if(type==21)sampler=device->native->graphics_sampler(bytes);auto id=create(blend?Kind::Blend:depth?Kind::DepthState:raster?Kind::Raster:Kind::Sampler,owner->device);id->desc=std::move(bytes);id->sampler=std::move(sampler);
            auto& saved=owner->bindings[key];if(!saved.empty())release(saved[0]);saved={id->root};owner->effect_handles[key]=id->root;return id->root;
        };
        const auto fx_object=[&](const std::shared_ptr<Identity>& owner,const fx10::Assignment& a,unsigned stage){
            auto& program=*owner->effect;
            if(a.kind==1){const auto values=fx_numeric(owner,a);require(values.size()==1 && fx_integer(values[0])==0);return std::uint64_t{};}
            if(a.kind==7){const auto shader=program.get(a.offset),so=program.get(a.offset+4);return fx_shader(owner,stage,program.blob(shader),"inline:"+std::to_string(a.offset),program.string(so));}
            unsigned index=0;std::string name;
            if(a.kind==2)name=program.string(a.offset);
            else if(a.kind==3){name=program.string(program.get(a.offset));index=program.get(a.offset+4);}
            else if(a.kind==4){name=program.string(program.get(a.offset));const auto selector=fx_find_variable(owner,program.string(program.get(a.offset+4)));require(selector!=UINT32_MAX);const auto& v=program.variables[selector];require(v.buffer<program.buffers.size());index=word(program.buffers[v.buffer].bytes,v.offset);}
            else throw std::invalid_argument("FX4 物件 expression 尚未解碼");
            const auto variable=fx_find_variable(owner,name);require(variable!=UINT32_MAX);const auto& v=program.variables[variable];require(index<v.type->count());
            if(v.type->detail==2 || v.type->detail==3 || v.type->detail==4 || v.type->detail==21)return fx_state(owner,variable,index);
            return owner->bindings.at("EffectObject:"+std::to_string(variable)).at(index);
        };
