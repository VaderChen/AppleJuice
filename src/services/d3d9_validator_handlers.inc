    if(service.symbol=="ShaderValidator.Begin" || service.symbol=="ShaderValidator.Instruction" ||
       service.symbol=="ShaderValidator.End"){
        return nested_continuation(state,memory,"D3D9 shader validator",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                const auto finish=[&](std::uint32_t status){call.return_to(s,status);return std::optional<std::uint32_t>{};};
                constexpr std::uint32_t failure=0x80004005U;
                constexpr unsigned shader_word_limit=1U<<20;
                const auto found=d3d9_->objects.find(call(0));
                if(found==d3d9_->objects.end() || !found->second->validator)co_return finish(failure);
                const auto validator=found->second->validator;
                using namespace d3d9_validator;
                std::array<Issue,2> issues;
                unsigned issue_count=0;
                std::uint64_t file=0;
                std::uint32_t line=0,result=0;
                auto& shader=*validator;
                try{
                    if(service.symbol=="ShaderValidator.Begin"){
                        if(shader.stage==Stage::Idle){
                            shader={};
                            shader.stage=Stage::AwaitingVersion;
                            shader.callback=call(1);shader.context=call(2);
                        }else{
                            issues[issue_count++]={6,0xeb,"Begin requires an idle shader validator."};
                            result=failure;
                        }
                    }else if(service.symbol=="ShaderValidator.End"){
                        if(shader.stage==Stage::Ended){
                            if(!shader.failed && shader.bytes.size()>8){
                                if(const auto error=legacy_shader_validation_error(shader.bytes)){
                                    issues[issue_count++]={2,0xf0,*error};
                                    shader.failed=true;
                                }
                            }
                            result=shader.failed?failure:0;
                        }else result=failure;
                        if(shader.stage!=Stage::Broken){
                            shader.stage=Stage::Idle;
                            shader.bytes.clear();
                        }
                    }else if(shader.stage==Stage::AwaitingVersion || shader.stage==Stage::Body){
                        const auto address=call(3);
                        const auto count=static_cast<std::uint32_t>(call(4));
                        if(!count || count>shader_word_limit-shader.bytes.size()/4 || !address)
                            co_return finish(failure);
                        const auto token=static_cast<std::uint32_t>(m.load(GuestAddress{address},32));
                        if(shader.stage==Stage::AwaitingVersion && (count!=1 || !shader.accepts_version(token))){
                            shader.stage=Stage::Broken;
                            issues[issue_count++]={6,0xef,"A shader must start with a version token."};
                            issues[issue_count++]={6,0xf0,"The shader version token is invalid."};
                            result=failure;
                        }else{
                            file=call(1);line=static_cast<std::uint32_t>(call(2));
                            const auto words=shader.stage==Stage::AwaitingVersion?1U:
                                legacy_shader_instruction_words(shader.version,token).value_or(0);
                            if(words!=count || ((token&0xffffU)==0xffffU && token!=0xffffU)){
                                issues[issue_count++]={2,0xf0,"The shader instruction length or token is invalid."};
                                shader.failed=true;
                            }else{
                                std::vector<std::uint32_t> packet(count);
                                const auto bytes=std::span(reinterpret_cast<std::uint8_t*>(packet.data()),std::size_t(count)*4);
                                m.read(GuestAddress{address},bytes);
                                if(shader.stage==Stage::AwaitingVersion){
                                    shader.version=token;shader.stage=Stage::Body;
                                }else if(token==0xffffU)shader.stage=Stage::Ended;
                                else if(const auto issue=shader.validate_pixel_inputs(packet)){
                                    issues[issue_count++]=*issue;
                                    shader.failed=true;
                                }
                                shader.bytes.insert(shader.bytes.end(),bytes.begin(),bytes.end());
                            }
                        }
                    }else result=failure;
                }catch(const MemoryFault&){co_return finish(failure);}
                catch(const std::bad_alloc&){co_return finish(out_of_memory);}

                const auto callback=shader.callback,context=shader.context;
                if(callback){
                    for(unsigned i=0;i<issue_count;++i){
                        const auto& issue=issues[i];
                        const auto length=issue.text.size()+1;
                        const auto message=allocate_heap(m,length);
                        if(!message)co_return finish(out_of_memory);
                        struct MessageStorage {
                            WindowsServices& services;GuestMemory& memory;GuestState& state;std::uint64_t address;
                            ~MessageStorage(){(void)services.free_heap(memory,address,state);}
                        } storage{*this,m,s,message};
                        m.write(GuestAddress{message},std::span(reinterpret_cast<const std::uint8_t*>(issue.text.c_str()),length));
                        const auto source_line=issue.id==0xef?UINT32_MAX:line;
                        (void)co_await service_callback::call_value(callback,{file,source_line,issue.severity,issue.id,message,context});
                    }
                }
                co_return finish(result);
            });
    }
