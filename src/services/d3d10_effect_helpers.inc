        const auto fx_string=[&](const std::shared_ptr<Identity>& owner,const std::string& text){
            if(text.empty())return std::uint64_t{};const auto key="string:"+text;if(owner->effect_handles.contains(key))return owner->effect_handles.at(key);
            const auto at=allocate_heap(memory,text.size()+1);if(!at)throw std::bad_alloc{};owner->allocations.push_back(at);memory.write(GuestAddress{at},std::span(reinterpret_cast<const std::uint8_t*>(text.c_str()),text.size()+1));owner->effect_handles[key]=at;return at;
        };
        const auto fx_owner=[&](const std::shared_ptr<Identity>& id){return id->effect_owner?object(id->effect_owner).identity:id;};
        const auto fx_pool=[&](const std::shared_ptr<Identity>& owner){
            return owner->effect_pool?object(owner->effect_pool).identity:std::shared_ptr<Identity>{};
        };
        struct FxLocation {
            std::shared_ptr<Identity> scope;
            unsigned index{UINT32_MAX};
            explicit operator bool() const {return bool(scope);}
        };
        const auto fx_index=[&](const std::shared_ptr<Identity>& owner,unsigned index,bool buffer){
            for(const auto& scope:std::array{owner,fx_pool(owner)})if(scope){
                const auto count=static_cast<unsigned>(buffer?scope->effect->buffers.size():scope->effect->variables.size());
                if(index<count)return FxLocation{scope,index};
                index-=count;
            }
            return FxLocation{};
        };
        const auto fx_named=[&](const std::shared_ptr<Identity>& owner,std::string_view name,bool buffer,bool semantic=false){
            const auto fold=[](char c){return c>='A' && c<='Z'?static_cast<char>(c-'A'+'a'):c;};
            if(!name.empty())for(const auto& scope:std::array{owner,fx_pool(owner)})if(scope){
                const auto& program=*scope->effect;
                if(buffer){
                    for(unsigned i=0;i<program.buffers.size();++i)
                        if(program.buffers[i].name==name)return FxLocation{scope,i};
                }else{
                    for(unsigned i=0;i<program.variables.size();++i){
                        const auto& v=program.variables[i];
                        if(semantic?std::ranges::equal(v.semantic,name,[&](char a,char b){return fold(a)==fold(b);}):v.name==name)
                            return FxLocation{scope,i};
                    }
                }
            }
            return FxLocation{};
        };
        const auto fx_handle=[&](const std::shared_ptr<Identity>& owner,Kind kind,const std::string& key){
            if(owner->effect_handles.contains(key))return object(owner->effect_handles.at(key)).identity;
            auto id=create(kind);id->effect=owner->effect;id->effect_owner=owner->root;owner->bindings["EffectHandles"].push_back(id->root);owner->effect_handles[key]=id->root;return id;
        };
        const auto fx_variable=[&](const std::shared_ptr<Identity>& owner,unsigned index){
            auto id=fx_handle(owner,Kind::EffectVariable10,"variable:"+std::to_string(index));id->effect_variable=index;id->effect_valid=index<owner->effect->variables.size();
            if(id->effect_valid){const auto& v=owner->effect->variables[index];id->effect_type=v.type;id->effect_buffer=v.buffer;id->effect_offset=v.offset;}return id;
        };
        const auto fx_buffer=[&](const std::shared_ptr<Identity>& owner,unsigned index){
            const auto& buffers=owner->effect->buffers;
            auto value=fx_handle(owner,Kind::EffectBuffer10,"buffer:"+std::to_string(index));
            value->effect_buffer=index;
            value->effect_valid=index<buffers.size();
            if(value->effect_valid && !value->effect_type){
                value->effect_type=std::make_shared<fx10::Type>();
                auto& type=*value->effect_type;
                type.name=buffers[index].name;
                type.kind=2;
                type.detail=18;
                type.size=static_cast<std::uint32_t>(buffers[index].bytes.size());
            }
            return value;
        };
        const auto fx_subscript=[&](const std::shared_ptr<Identity>& owner,std::string_view name){
            const auto location=fx_named(owner,name,false);
            require(bool(location));
            const auto& program=*location.scope->effect;
            const auto& variable=program.variables[location.index];
            require(variable.buffer<program.buffers.size());
            return word(program.buffers[variable.buffer].bytes,variable.offset);
        };
        const auto fx_shader=[&](const std::shared_ptr<Identity>& owner,unsigned stage,const std::vector<std::uint8_t>& bytes,const std::string& key,std::string_view so={}){
            if(owner->effect_handles.contains(key))return owner->effect_handles.at(key);if(bytes.empty())return std::uint64_t{};
            auto device=object(owner->device).identity;auto reflected=std::make_shared<dxbc::Reflection>(bytes);const auto native_stage=stage==2 && (reflected->version>>16)==1?0U:stage;auto shader=so.empty()?device->native->graphics_shader(native_stage,bytes):device->native->graphics_shader_private(native_stage,bytes);if(!so.empty())stream_output::parse(*shader,*reflected,so);
            auto id=create(stage==0?Kind::VertexShader:stage==1?Kind::PixelShader:Kind::GeometryShader,owner->device);id->effect_owner=owner->root;id->shader=std::move(shader);id->reflection=std::move(reflected);id->desc=bytes;owner->bindings["EffectShaders"].push_back(id->root);owner->effect_handles[key]=id->root;return id->root;
        };
        const auto fx_create=[&](std::uint64_t device_pointer,std::span<const std::uint8_t> bytes,
            unsigned flags,bool make_pool,std::uint64_t pool_pointer){
            require(device_pointer!=0 && !(flags&~0xbU) && bool(flags&1)==bool(pool_pointer) && (!make_pool || !pool_pointer));
            const auto device=typed(device_pointer,Kind::Device);
            require(device->api10);
            std::shared_ptr<Identity> pool;
            if(pool_pointer){
                const auto found=bridge.objects.find_fast(pool_pointer);
                require(found!=bridge.objects.end() && found->second.kind==Kind::EffectPool10);
                pool=found->second.identity;
                require(pool->device==device->root && pool->effect_is_pool && bool(pool->effect));
            }
            std::optional<std::vector<std::uint8_t>> section;
            if(bytes.size()>=4 && word(bytes,0)==0x43425844){
                section=graphics_dxbc_section(bytes,0x30315846);
                require(bool(section));
                bytes=*section;
            }
            auto program=std::make_shared<fx10::Program>(bytes,pool?pool->effect.get():nullptr);
            if(make_pool)for(auto& variable:program->variables)variable.flags|=1;
            auto owner=create(Kind::Effect10,device->root);
            owner->effect=std::move(program);
            owner->effect_is_pool=make_pool;
            try{
                if(pool){
                    bind_one(owner,"EffectPool",pool->root);
                    owner->effect_pool=pool->root;
                }
                for(unsigned i=0;i<owner->effect->buffers.size();++i){const auto& b=owner->effect->buffers[i];GraphicsResourceDesc desc;desc.width=static_cast<std::uint32_t>((b.bytes.size()+15)&~std::size_t{15});desc.bind=(b.flags&1)?8U:4U;if(!desc.width)desc.width=16;
                    auto resource=device->native->graphics_resource(desc);auto id=create(Kind::BufferObject,device_pointer);id->resource=std::move(resource);id->desc.resize(24);put_word(id->desc,0,desc.width);put_word(id->desc,8,desc.bind);owner->bindings["EffectBuffer:"+std::to_string(i)]={id->root};
                    if(b.flags&1){auto view=create(Kind::ShaderResource,device_pointer);view->texture=id->root;hold(id->root);view->graphics_view={id->resource,2,0,1,0,1};view->graphics_view.count=desc.width;view->graphics_view.dimension=1;view->desc.resize(24);put_word(view->desc,0,2);put_word(view->desc,4,1);put_word(view->desc,12,desc.width/16);owner->bindings["EffectBufferView:"+std::to_string(i)]={view->root};}
                }
                for(unsigned i=0;i<owner->effect->variables.size();++i){const auto& v=owner->effect->variables[i];if(v.type->kind!=2)continue;auto& objects=owner->bindings["EffectObject:"+std::to_string(i)];objects.resize(v.type->count());
                    if(v.type->detail>=5 && v.type->detail<=8)for(unsigned j=0;j<v.objects.size();++j){const auto shader=fx_shader(owner,v.type->detail==6?0U:v.type->detail==5?1U:2U,owner->effect->blob(v.objects[j].data),"shader:"+std::to_string(i)+":"+std::to_string(j),owner->effect->string(v.objects[j].stream_output));if(shader)hold(shader);objects[j]=shader;}
                }
                return make_pool?interface(owner,Kind::EffectPool10):owner->root;
            }catch(...){release(owner->root);throw;}
        };
        const auto fx_numeric=[&](const std::shared_ptr<Identity>& owner,const fx10::Assignment& a){
            std::vector<std::pair<unsigned,std::uint32_t>> values;
            if(a.kind==1){const auto count=fx10::Program::bounded(owner->effect->get(a.offset));for(unsigned i=0;i<count;++i)values.emplace_back(owner->effect->get(a.offset+4+i*8),owner->effect->get(a.offset+8+i*8));}
            else if(a.kind==2){
                const auto location=fx_named(owner,owner->effect->string(a.offset),false);
                require(bool(location));
                const auto& program=*location.scope->effect;
                const auto& v=program.variables[location.index];
                require(v.type->kind==1 && v.buffer<program.buffers.size());
                const auto& bytes=program.buffers[v.buffer].bytes;
                for(unsigned i=0;i<v.type->columns();++i)values.emplace_back(v.type->scalar(),word(bytes,v.offset+i*4));
            }
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
            else if(a.kind==4){name=program.string(program.get(a.offset));index=fx_subscript(owner,program.string(program.get(a.offset+4)));}
            else throw std::invalid_argument("FX4 物件 expression 尚未解碼");
            const auto variable=fx_named(owner,name,false);
            require(bool(variable));
            const auto& v=variable.scope->effect->variables[variable.index];
            require(index<v.type->count());
            if(v.type->detail==2 || v.type->detail==3 || v.type->detail==4 || v.type->detail==21)
                return fx_state(variable.scope,variable.index,index);
            return variable.scope->bindings.at("EffectObject:"+std::to_string(variable.index)).at(index);
        };
