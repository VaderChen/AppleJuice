        if(symbol=="CreateClassLinkage"){
            graphics_handled=true;auto device=typed(arg(0),Kind::Device);output(1);write_pointer(arg(1),0);
            auto linkage=create(Kind::ClassLinkage,device->root);write_pointer(arg(1),linkage->root);
        }else if(symbol=="GetClassInstance" || symbol=="CreateClassInstance"){
            graphics_handled=true;const bool created=symbol=="CreateClassInstance";auto linkage=typed(arg(0),Kind::ClassLinkage);
            const unsigned out=created?6U:3U;output(out);write_pointer(arg(out),0);require(arg(1));
            const auto name=read_ansi(memory,arg(1));require(!name.empty() && name.size()<4096);
            const auto index=created?0U:u32(2);require(index<65536);
            if(created)require(u32(2)<14 && u32(3)<4096 && u32(4)<128 && u32(5)<16);
            // 名稱先登錄，建立 shader 時再依各 shader 的型別／變數資料解析。
            // 同一 linkage 可在建立 shader 前取得 instance，不能依單一 shader 做快取。
            const auto key=std::pair{name,index};
            if(!created){
                const auto at=linkage->class_instances.find(key);
                if(at!=linkage->class_instances.end() && bridge.objects.contains(at->second.second)){
                    hold(at->second.second);write_pointer(arg(out),at->second.second);complete_call(state,memory,0);return std::nullopt;
                }
                require(at!=linkage->class_instances.end() || linkage->class_instances.size()<65536);
            }
            auto instance=create(Kind::ClassInstance,linkage->device);
            try{
                bind_one(instance,"ClassLinkage",linkage->root);
                instance->class_desc={0,index,0,0,0,127,15,created?1U:0U};
                if(created){
                    auto [type,inserted]=linkage->class_types.emplace(name,static_cast<std::uint32_t>(linkage->class_types.size()));(void)inserted;
                    instance->class_type=name;
                    instance->class_desc={static_cast<std::uint32_t>(linkage->sequence++),0,type->second,u32(2),u32(3),u32(4),u32(5),1};
                }else {
                    instance->class_name=name;
                    for(const auto& reflected:linkage->linked_shaders)if(reflected->linkage){
                        const auto& known=reflected->linkage->instances;
                        const auto found=std::find_if(known.begin(),known.end(),[&](const auto& item){return item.name==name && index<item.count;});
                        if(found!=known.end()){instance->class_type=found->type;break;}
                    }
                    auto [at,inserted]=linkage->class_instances.emplace(key,std::pair{static_cast<std::uint32_t>(linkage->class_instances.size()),instance->root});
                    if(!inserted)at->second.second=instance->root;
                }
                write_pointer(arg(out),instance->root);
            }catch(...){release(instance->root);throw;}
        }else if(symbol=="ClassInstanceLinkage" || symbol=="ClassInstanceDesc" || symbol=="ClassInstanceName" || symbol=="ClassInstanceTypeName"){
            graphics_handled=true;auto instance=typed(arg(0),Kind::ClassInstance);
            if(symbol=="ClassInstanceLinkage"){
                output(1);const auto linkage=binding(instance,"ClassLinkage");hold(linkage);write_pointer(arg(1),linkage);
            }else if(symbol=="ClassInstanceDesc"){
                require(arg(1));memory.check(GuestAddress{arg(1)},32,Access::Write);
                for(unsigned i=0;i<8;++i)memory.store(GuestAddress{arg(1)+4*i},32,instance->class_desc[i]);
            }else {
                require(arg(2));const auto& name=symbol=="ClassInstanceName"?instance->class_name:instance->class_type;
                const auto capacity=memory.load(GuestAddress{arg(2)},pointer_bits);
                if(arg(1) && capacity){const auto count=std::min<std::uint64_t>(capacity-1,name.size());
                    memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(name.data()),static_cast<std::size_t>(count)));
                    memory.store(GuestAddress{arg(1)+count},8,0);
                }
                write_pointer(arg(2),name.size()+1);
            }
        }
