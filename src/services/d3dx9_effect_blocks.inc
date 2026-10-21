                    // 錄製時使用獨立值；Get* 及同 pool 的其他 effect 仍看見原狀態。
                    const auto fx_update_value=[&](unsigned index,bool write)->fx9::Value&{
                        if(!fx.recording || !write)return *fx.values.at(index);
                        auto it=fx.recorded_values.find(index);if(it==fx.recorded_values.end()){
                            auto value=std::make_shared<fx9::Value>(*fx.values.at(index));it=fx.recorded_values.emplace(index,std::move(value)).first;
                            for(auto pointer:it->second->interfaces)if(pointer)hold(bridge.objects.at(pointer));
                        }return *it->second;
                    };
                    bool fx_block_handled=true;
                    if(method=="BeginParameterBlock"){require(!fx.recording && !fx.begun);fx.recording=true;}
                    else if(method=="EndParameterBlock"){
                        if(!fx.recording)result=0;
                        else {auto pointer=fx_memory(fx,std::vector<std::uint8_t>(4));fx.blocks.emplace(pointer,std::move(fx.recording_updates));fx.recording_updates.clear();for(const auto& [index,value]:fx.recorded_values){(void)index;for(auto texture:value->interfaces)if(texture)release(texture);}fx.recorded_values.clear();fx.recording=false;result=pointer;}
                    }else if(method=="DeleteParameterBlock"){
                        auto it=fx.blocks.find(arg(1));require(it!=fx.blocks.end());for(const auto& update:it->second)if(update.texture)release(update.texture);fx.blocks.erase(it);
                    }else if(method=="ApplyParameterBlock"){
                        auto it=fx.blocks.find(arg(1));require(it!=fx.blocks.end() && !it->second.empty());
                        for(const auto& update:it->second){
                            if(fx.recording){fx.recording_updates.push_back(update);if(update.texture)hold(bridge.objects.at(update.texture));continue;}
                            auto& value=*fx.values.at(update.index);
                            if(update.kind==1){if(update.texture)hold(bridge.objects.at(update.texture));auto& pointer=value.interfaces.at(update.offset/4);if(pointer)release(pointer);pointer=update.texture;}
                            else if(update.kind==2)value.strings.at(update.offset/4)=update.text;
                            else {require(update.offset<=value.bytes.size() && update.bytes.size()<=value.bytes.size()-update.offset);std::copy(update.bytes.begin(),update.bytes.end(),value.bytes.begin()+update.offset);}
                        }
                    }else if(method=="CloneEffect"){
                        output(2);require(!(fx.flags&0x800));auto device=get(arg(1),Kind::Device);auto shared=fx.pool?get(fx.pool,Kind::EffectPool):std::shared_ptr<Object>{};
                        auto runtime=std::make_shared<fx9::Runtime>(fx.bytecode,shared?shared->effect_pool.get():nullptr);auto clone=create(Kind::Effect,device->root);clone->effect=runtime;
                        try {runtime->flags=fx.flags;runtime->technique=fx.technique;if(shared){hold(shared);runtime->pool=shared->root;}
                            for(unsigned i=0;i<runtime->values.size();++i){if(shared && (fx.values[i]->flags&1))continue;*runtime->values[i]=*fx.values[i];for(auto pointer:runtime->values[i]->interfaces)if(pointer)hold(bridge.objects.at(pointer));}
                            fx_handles(*runtime);store_pointer(arg(2),clone->root);
                        }catch(...){release(clone->root);throw;}
                    }else fx_block_handled=false;
