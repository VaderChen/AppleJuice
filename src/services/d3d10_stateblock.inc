        if(!graphics_handled && (symbol.starts_with("D3D10StateBlockMask") || symbol=="D3D10CreateStateBlock" || symbol.starts_with("StateBlock"))){
            graphics_handled=true;
            // SDK D3D10_STATE_BLOCK_MASK 是 76 個 BYTE，與客體指標寬度無關。
            struct MaskField { unsigned offset,count; };
            constexpr std::array<MaskField,24> fields{{{74,1},{68,1},{69,1},{70,1},{0,1},{1,16},{3,128},{19,14},{21,1},{22,16},{24,128},{40,14},{42,1},{43,16},{45,128},{61,14},{63,16},{65,1},{66,1},{67,1},{71,1},{72,1},{73,1},{75,1}}};
            if(symbol=="D3D10StateBlockMaskEnableAll" || symbol=="D3D10StateBlockMaskDisableAll"){
                memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(76,symbol=="D3D10StateBlockMaskEnableAll"?255:0));
            }else if(symbol=="D3D10StateBlockMaskUnion" || symbol=="D3D10StateBlockMaskIntersect" || symbol=="D3D10StateBlockMaskDifference"){
                auto a=read_bytes(memory,arg(0),76);const auto b=read_bytes(memory,arg(1),76);
                for(unsigned i=0;i<76;++i)a[i]=static_cast<std::uint8_t>(symbol.ends_with("Union")?(a[i]|b[i]):symbol.ends_with("Intersect")?(a[i]&b[i]):(a[i]&~b[i]));
                memory.write(GuestAddress{arg(2)},a);
            }else if(symbol.starts_with("D3D10StateBlockMask")){
                require(u32(1)>=1 && u32(1)<=fields.size());const auto field=fields[u32(1)-1];const bool get=symbol.ends_with("GetSetting");const auto first=u32(2),count=get?1U:u32(3);require(first<=field.count && count<=field.count-first);
                auto mask=read_bytes(memory,arg(0),76);
                if(get)result=bool(mask[field.offset+first/8]&(1U<<(first%8)));
                else {for(unsigned i=first;i<first+count;++i){auto& byte=mask[field.offset+i/8];const auto bit=1U<<(i%8);byte=static_cast<std::uint8_t>(symbol.ends_with("EnableCapture")?(byte|bit):(byte&~bit));}memory.write(GuestAddress{arg(0)},mask);}
            }else if(symbol=="D3D10CreateStateBlock"){
                auto device=typed(arg(0),Kind::Device);require(device->api10 && arg(2));output(2);write_pointer(arg(2),0);auto mask=read_bytes(memory,arg(1),76);auto block=create(Kind::StateBlock10,device->root);block->desc=std::move(mask);write_pointer(arg(2),block->root);
            }else {
                auto block=typed(arg(0),Kind::StateBlock10);auto context=object(object(block->device).identity->context10).identity;
                if(symbol=="StateBlockReleaseObjects")clear_bindings(block);
                else {
                    const bool capture=symbol=="StateBlockCapture";require(capture || symbol=="StateBlockApply");
                    const auto selected=[&](unsigned type,unsigned entry=0){const auto f=fields.at(type-1);return entry<f.count && bool(block->desc[f.offset+entry/8]&(1U<<(entry%8)));};
                    if(capture)clear_bindings(block);
                    const auto source=capture?context:block,destination=capture?block:context;
                    const auto transfer=[&](const std::string& name,unsigned type,unsigned limit,bool atomic=false){
                        for(unsigned i=0;i<limit;++i)if(selected(type,atomic?0:i)){
                            auto pointer=binding(source,name,i);
                            if(!capture && pointer){if(name.ends_with("ShaderResources") && outputs_conflict(destination,pointer))pointer=0;else if(name=="Targets" || name=="Depth" || name=="StreamOutput"){const std::array outputs{pointer};remove_conflicts(destination,outputs,name);}}
                            const std::array values{pointer};bind(destination,name,i,values,limit);
                        }
                    };
                    // 先恢復輸出，再依共用 hazard 規則恢復輸入。
                    transfer("Targets",2,8,true);transfer("Depth",2,1,true);transfer("StreamOutput",1,4,true);
                    for(unsigned stage=0;stage<3;++stage){const std::string name=stage==0?"VS":stage==1?"GS":"PS";const unsigned type=5+stage*4;transfer(name+"Shader",type,1);transfer(name+"Samplers",type+1,16);transfer(name+"ShaderResources",type+2,128);transfer(name+"ConstantBuffers",type+3,14);}
                    transfer("Vertices",17,16);transfer("Index",18,1);transfer("Layout",19,1);transfer("DepthState",3,1);transfer("Blend",4,1);transfer("Raster",23,1);transfer("Predicate",24,1);
                    for(unsigned i=0;i<16;++i)if(selected(17,i)){destination->strides[i]=source->strides[i];destination->offsets[i]=source->offsets[i];}
                    if(selected(1))destination->stream_offsets=source->stream_offsets;
                    if(selected(3))destination->stencil_reference=source->stencil_reference;
                    if(selected(4)){destination->blend_factor=source->blend_factor;destination->sample_mask=source->sample_mask;}
                    if(selected(18)){destination->index_format=source->index_format;destination->index_offset=source->index_offset;}
                    if(selected(20))destination->topology=source->topology;
                    if(selected(21))destination->viewports=source->viewports;
                    if(selected(22))destination->scissors=source->scissors;
                    if(selected(24))destination->predicate_value=source->predicate_value;
                }
            }
        }
