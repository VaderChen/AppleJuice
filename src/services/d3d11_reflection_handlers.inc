        if(symbol=="D3DReflect"){
            graphics_handled=true;output(3);write_pointer(arg(3),0);
            const auto iid=read_guid(memory,arg(2));
            if(iid!=IID_ID3D11ShaderReflection && iid!=IID_ID3D11ShaderReflection43){complete_call(state,memory,no_interface);return std::nullopt;}
            require(arg(1)>0 && arg(1)<=16U*1024U*1024U);
            auto reflection=std::make_shared<dxbc::Reflection>(read_bytes(memory,arg(0),static_cast<std::size_t>(arg(1))));
            require(reflection->version!=0);
            auto id=create(Kind::Reflection);id->reflection=std::move(reflection);id->reflection_owner=id->root;
            id->sequence=iid==IID_ID3D11ShaderReflection43?43U:47U;write_pointer(arg(3),id->root);
        }else if(symbol.starts_with("Reflect")){
            graphics_handled=true;auto id=object(arg(0)).identity;require(bool(id->reflection));
            auto root=typed(id->reflection_owner,Kind::Reflection);const auto& r=*id->reflection;
            const auto string=[&](const std::string& text){
                if(const auto it=root->reflection_strings.find(text);it!=root->reflection_strings.end())return it->second;
                const auto address=allocate_heap(memory,text.size()+1);if(!address)throw std::bad_alloc{};
                root->allocations.push_back(address);memory.write(GuestAddress{address},std::span(reinterpret_cast<const std::uint8_t*>(text.c_str()),text.size()+1));
                root->reflection_strings.emplace(text,address);return address;
            };
            const auto sub=[&](Kind kind,unsigned buffer,unsigned variable,const dxbc::Type* type,unsigned offset)->std::uint64_t{
                for(const auto pointer:root->bindings["ReflectionChildren"]){
                    auto old=object(pointer).identity;if(old->kind==kind && old->reflection_buffer==buffer && old->reflection_variable==variable && old->reflection_type==type && old->reflection_offset==offset)return pointer;
                }
                auto child_id=create(kind);child_id->reflection=id->reflection;child_id->reflection_owner=root->root;
                child_id->reflection_buffer=buffer;child_id->reflection_variable=variable;child_id->reflection_type=type;child_id->reflection_offset=offset;
                root->bindings["ReflectionChildren"].push_back(child_id->root);return child_id->root;
            };
            const auto buffer=[&]()->const dxbc::Buffer*{return id->reflection_buffer<r.buffers.size()?&r.buffers[id->reflection_buffer]:nullptr;};
            const auto variable=[&]()->const dxbc::Variable*{const auto b=buffer();return b && id->reflection_variable<b->variables.size()?&b->variables[id->reflection_variable]:nullptr;};
            const auto stat=[&](unsigned index){return index<r.statistics.size()?r.statistics[index]:0U;};
            const auto words=[&](std::uint64_t out,std::span<const std::uint32_t> values){memory.check(GuestAddress{out},values.size()*4,Access::Write);for(std::size_t i=0;i<values.size();++i)memory.store(GuestAddress{out+i*4},32,values[i]);};
            if(symbol=="ReflectDesc"){
                require(arg(1));const auto ptr_at=x86_?4U:8U,fields_at=ptr_at+pointer_size;
                std::vector<std::uint32_t> fields{r.flags,static_cast<unsigned>(r.buffers.size()),static_cast<unsigned>(r.bindings.size()),static_cast<unsigned>(r.signatures[0].size()),static_cast<unsigned>(r.signatures[1].size())};
                for(const unsigned index:std::array<unsigned,22>{0,1,10,2,3,14,15,16,17,18,4,5,6,7,8,9,11,12,13,24,25,23})fields.push_back(stat(index));
                fields.push_back(static_cast<unsigned>(r.signatures[2].size()));
                for(unsigned index=27;index<35;++index)fields.push_back(stat(index));
                const auto size=(fields_at+fields.size()*4+pointer_size-1)&~std::size_t(pointer_size-1);
                memory.fill(GuestAddress{arg(1)},0,size);memory.store(GuestAddress{arg(1)},32,r.version);write_pointer(arg(1)+ptr_at,string(r.creator));words(arg(1)+fields_at,fields);
            }else if(symbol=="ReflectBufferByIndex" || symbol=="ReflectBufferByName"){
                unsigned index=u32(1);
                if(symbol=="ReflectBufferByName"){const auto name=arg(1)?read_ansi(memory,arg(1)):std::string{};index=UINT32_MAX;for(unsigned i=0;i<r.buffers.size();++i)if(r.buffers[i].name==name){index=i;break;}}
                result=sub(Kind::ReflectionBuffer,index,UINT32_MAX,nullptr,0);
            }else if(symbol=="ReflectVariableByName" || symbol=="ReflectBufferVariableByName" || symbol=="ReflectBufferVariableByIndex"){
                unsigned bi=UINT32_MAX,vi=UINT32_MAX;
                if(symbol=="ReflectBufferVariableByIndex"){bi=id->reflection_buffer;vi=u32(1);}
                else {const auto name=arg(1)?read_ansi(memory,arg(1)):std::string{};
                    for(unsigned b=0;b<r.buffers.size();++b){if(symbol=="ReflectBufferVariableByName" && b!=id->reflection_buffer)continue;
                        for(unsigned v=0;v<r.buffers[b].variables.size();++v)if(r.buffers[b].variables[v].name==name){bi=b;vi=v;break;}
                        if(vi!=UINT32_MAX)break;
                    }
                }
                result=sub(Kind::ReflectionVariable,bi,vi,nullptr,0);
            }else if(symbol=="ReflectBinding" || symbol=="ReflectBindingByName"){
                unsigned index=u32(1);if(symbol=="ReflectBindingByName"){const auto name=arg(1)?read_ansi(memory,arg(1)):std::string{};index=UINT32_MAX;for(unsigned i=0;i<r.bindings.size();++i)if(r.bindings[i].name==name){index=i;break;}}
                require(index<r.bindings.size() && arg(2));const auto& b=r.bindings[index];write_pointer(arg(2),string(b.name));const std::array values{b.type,b.slot,b.count,b.flags,b.format,b.dimension,b.samples};words(arg(2)+pointer_size,values);
            }else if(symbol=="ReflectInput" || symbol=="ReflectOutput" || symbol=="ReflectPatch"){
                const auto kind=symbol=="ReflectInput"?0U:symbol=="ReflectOutput"?1U:2U;require(u32(1)<r.signatures[kind].size() && arg(2));const auto& s=r.signatures[kind][u32(1)];
                const auto size=pointer_size+(root->sequence>=47?28U:24U);memory.fill(GuestAddress{arg(2)},0,size);write_pointer(arg(2),string(s.name));
                const std::array values{s.index,s.reg,s.system,s.component,s.mask|(s.used<<8),s.stream};words(arg(2)+pointer_size,values);
                if(root->sequence>=47)memory.store(GuestAddress{arg(2)+pointer_size+24},32,s.precision);
            }else if(symbol=="ReflectBufferDesc"){
                const auto b=buffer();require(b && arg(1));write_pointer(arg(1),string(b->name));const std::array values{b->type,static_cast<unsigned>(b->variables.size()),b->size,b->flags};words(arg(1)+pointer_size,values);
            }else if(symbol=="ReflectVariableDesc"){
                const auto v=variable();require(v && arg(1));const unsigned value_at=x86_?16U:24U,tail=value_at+pointer_size;
                memory.fill(GuestAddress{arg(1)},0,tail+16);write_pointer(arg(1),string(v->name));const std::array values{v->offset,v->size,v->flags};words(arg(1)+pointer_size,values);
                if(!v->initial.empty()){
                    const auto address=allocate_heap(memory,v->initial.size());if(!address)throw std::bad_alloc{};root->allocations.push_back(address);memory.write(GuestAddress{address},v->initial);write_pointer(arg(1)+value_at,address);
                }
                const std::array bindings{v->texture,v->textures,v->sampler,v->samplers};words(arg(1)+tail,bindings);
            }else if(symbol=="ReflectVariableType"){
                const auto v=variable();result=sub(Kind::ReflectionType,id->reflection_buffer,id->reflection_variable,v?v->type.get():nullptr,0);
            }else if(symbol=="ReflectVariableBuffer")result=sub(Kind::ReflectionBuffer,id->reflection_buffer,UINT32_MAX,nullptr,0);
            else if(symbol=="ReflectInterfaceSlot"){
                const auto v=variable();result=UINT32_MAX;
                if(v && buffer()->type==2 && u32(1)<std::max(1U,v->type->elements))result=v->offset+u32(1);
            }else if(symbol=="ReflectTypeDesc"){
                const auto t=id->reflection_type;require(t && arg(1));const unsigned name_at=x86_?28U:32U;
                memory.fill(GuestAddress{arg(1)},0,name_at+pointer_size);const std::array values{t->classification,t->base,t->rows,t->columns,t->elements,static_cast<unsigned>(t->members.size()),id->reflection_offset};words(arg(1),values);
                if(!t->name.empty())write_pointer(arg(1)+name_at,string(t->name));
            }else if(symbol=="ReflectMemberByIndex" || symbol=="ReflectMemberByName" || symbol=="ReflectMemberName"){
                const auto t=id->reflection_type;unsigned index=u32(1);
                if(symbol=="ReflectMemberByName"){const auto name=arg(1)?read_ansi(memory,arg(1)):std::string{};index=UINT32_MAX;if(t)for(unsigned i=0;i<t->members.size();++i)if(t->members[i].name==name){index=i;break;}}
                const auto m=t && index<t->members.size()?&t->members[index]:nullptr;
                if(symbol=="ReflectMemberName")result=m?string(m->name):0;
                else result=sub(Kind::ReflectionType,id->reflection_buffer,id->reflection_variable,m?m->type.get():nullptr,m?m->offset:0);
            }else if(symbol=="ReflectSubType" || symbol=="ReflectBaseClass" || symbol=="ReflectTypeInterface" || symbol=="ReflectTypeInterfaces"){
                const auto t=id->reflection_type;
                if(symbol=="ReflectTypeInterfaces")result=t?t->interfaces.size():0;
                else {
                    const dxbc::Type* target=nullptr;
                    if(t){if(symbol=="ReflectSubType")target=t->subtype.get();else if(symbol=="ReflectBaseClass")target=t->parent.get();else if(u32(1)<t->interfaces.size())target=t->interfaces[u32(1)].get();}
                    result=sub(Kind::ReflectionType,id->reflection_buffer,id->reflection_variable,target,0);
                }
            }else if(symbol=="ReflectTypeEqual" || symbol=="ReflectIsOfType" || symbol=="ReflectImplements"){
                require(id->reflection_type);const auto other=arg(1)?typed(arg(1),Kind::ReflectionType)->reflection_type:nullptr;
                const auto equal=[&](auto&& self,const dxbc::Type* a,const dxbc::Type* b,unsigned depth)->bool{
                    if(!a || !b)return a==b;if(a==b)return true;if(depth>64)return false;
                    if(std::tie(a->name,a->classification,a->base,a->rows,a->columns,a->elements)!=std::tie(b->name,b->classification,b->base,b->rows,b->columns,b->elements) || a->members.size()!=b->members.size() || a->interfaces.size()!=b->interfaces.size())return false;
                    if(!self(self,a->parent.get(),b->parent.get(),depth+1) || !self(self,a->subtype.get(),b->subtype.get(),depth+1))return false;
                    for(std::size_t i=0;i<a->members.size();++i)if(a->members[i].name!=b->members[i].name || a->members[i].offset!=b->members[i].offset || !self(self,a->members[i].type.get(),b->members[i].type.get(),depth+1))return false;
                    for(std::size_t i=0;i<a->interfaces.size();++i)if(!self(self,a->interfaces[i].get(),b->interfaces[i].get(),depth+1))return false;
                    return true;
                };
                bool match=false;
                if(other){
                    if(symbol=="ReflectTypeEqual")match=equal(equal,id->reflection_type,other,0);
                    else for(auto t=id->reflection_type;t && !match;t=t->parent.get()){
                        if(symbol=="ReflectIsOfType")match=equal(equal,t,other,0);
                        else for(const auto& implemented:t->interfaces)if(equal(equal,implemented.get(),other,0)){match=true;break;}
                    }
                }
                result=match?0U:1U;
            }else if(symbol=="ReflectInterfaceCount")result=r.interface_slots;
            else if(symbol=="ReflectThreads"){for(unsigned i=0;i<3;++i)if(arg(1+i))memory.store(GuestAddress{arg(1+i)},32,r.threads[i]);result=std::uint64_t{r.threads[0]}*r.threads[1]*r.threads[2];}
            else if(symbol=="ReflectMinFeature"){require(arg(1));memory.store(GuestAddress{arg(1)},32,((r.version>>4)&15)>=5?0xb000U:(r.version&15)?0xa100U:0xa000U);}
            else if(symbol=="ReflectRequires"){result=r.requires_flags;if(x86_)state.reg(Gpr::Rdx)=result>>32;}
            else if(symbol=="ReflectMov")result=stat(19);
            else if(symbol=="ReflectMovc")result=stat(20);
            else if(symbol=="ReflectConversion")result=stat(21);
            else if(symbol=="ReflectBitwise")result=stat(22);
            else if(symbol=="ReflectPrimitive")result=stat(23);
            else if(symbol=="ReflectSampleFrequency")result=stat(26);
            else throw HostCallFault("尚未實作的 shader reflection 方法："+std::string(symbol));
        }
