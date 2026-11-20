// 兩種列舉器各自保存快照與游標。Next 交給客體的名稱使用工作配置器。
if(object.enumerator){
    const auto size=object.collection?object.set_stats.size():object.property_stats.size();
    if(name=="Reset"){object.position=0;co_return done(0);}
    if(name=="Clone"){
        if(!arg(1))co_return done(pointer);
        m.store(GuestAddress{arg(1)},bits,0);
        auto copy=object;copy.references=1;create(std::move(copy),arg(1));
        co_return done(0);
    }
    const auto wanted=static_cast<std::uint32_t>(arg(1));
    const auto count=std::min<size_t>(wanted,size-object.position);
    if(name=="Next"){
        const auto out=arg(2),fetched=arg(3);
        if(fetched)m.store(GuestAddress{fetched},32,0);
        if((!out && wanted) || (!fetched && wanted>1))co_return done(pointer);
        const auto stride=object.collection?64U:(x86_?12U:16U);
        if(count)m.check(GuestAddress{out},count*stride,Access::Write);
        std::vector<std::uint64_t> allocated;
        try{
            if(!object.collection)allocated.reserve(count);
            for(size_t i=0;i<count;++i){
                const auto at=out+i*stride;
                const auto index=object.position+i;
                if(object.collection)m.write(GuestAddress{at},object.set_stats[index]);
                else{
                    const auto& property=object.property_stats[index];
                    std::uint64_t label=0;
                    if(property.name){
                        auto data=utf16_bytes(*property.name);
                        label=allocate_heap(m,data.size());
                        if(!label)throw std::bad_alloc{};
                        allocated.push_back(label);m.write(GuestAddress{label},data);
                        gui_.task_allocations.insert(label);
                    }
                    m.fill(GuestAddress{at},0,stride);
                    m.store(GuestAddress{at},bits,label);
                    m.store(GuestAddress{at+unit},32,property.id);
                    m.store(GuestAddress{at+unit+4},16,property.type);
                }
            }
        }catch(...){
            for(auto address:allocated){gui_.task_allocations.erase(address);(void)free_heap(m,address,s);}
            if(count)m.fill(GuestAddress{out},0,count*stride);
            throw;
        }
        if(fetched)m.store(GuestAddress{fetched},32,count);
    }else if(name!="Skip")co_return done(storage::invalid_function);
    object.position+=count;
    co_return done(count==wanted?0:1);
}
if(name=="Enum"){
    const auto output=arg(1);
    if(!output)co_return done(invalid);
    m.store(GuestAddress{output},bits,0);
    StorageState::PropertyObject result;
    result.enumerator=true;result.collection=object.collection;
    if(!object.collection){
        const auto& local=object.stream.sections.at(object.section);
        const auto names=property_set::decode_names(local);
        for(const auto& [id,data]:local.properties){
            if(id<2 || id>=0x80000000U)continue;
            StorageState::PropertyStat stat;
            stat.id=id;stat.type=static_cast<std::uint16_t>(property_set::read(data,0));
            const auto found=names.find(id);
            if(found!=names.end())stat.name=found->second;
            result.property_stats.push_back(std::move(stat));
        }
    }else{
        const auto slot=temp(unit),entry=temp(x86_?72:80),fetched=temp(4),format=temp(16);
        auto status=co_await method(object.backend,11,{0,0,0,slot});
        if(status)co_return done(status);
        const auto elements=ptr(slot);
        std::exception_ptr failure;
        std::uint64_t label=0;
        try{
            while(true){
                status=co_await method(elements,3,{1,entry,fetched});
                if(status || m.load(GuestAddress{fetched},32)!=1)break;
                label=ptr(entry);
                if(!label)continue;
                const auto type=m.load(GuestAddress{entry+unit},32);
                if((type==1 || type==2) && m.load(GuestAddress{label},16)==5){
                    const auto converted=co_await service_callback::call(resolve("ole32.dll","PropStgNameToFmtId").value,{label,format});
                    const auto base=entry+(x86_?8:16);
                    std::array<std::uint8_t,64> record{};
                    if(!converted)m.read(GuestAddress{format},std::span(record).first(16));
                    if(type==1){
                        m.read(GuestAddress{base+40},std::span(record).subspan(16,16));
                        record[32]=1;
                    }
                    m.read(GuestAddress{base+8},std::span(record).subspan(36,24));
                    std::fill(record.begin()+60,record.end(),0xff);
                    result.set_stats.push_back(record);
                }
                const auto to_free=std::exchange(label,0);
                (void)co_await service_callback::call(resolve("ole32.dll","CoTaskMemFree").value,{to_free});
            }
        }catch(...){failure=std::current_exception();}
        if(label)(void)co_await service_callback::call(resolve("ole32.dll","CoTaskMemFree").value,{label});
        (void)co_await method(elements,2,{});
        if(failure)std::rethrow_exception(failure);
        if(status>>31)co_return done(status);
    }
    create(std::move(result),output);
    co_return done(0);
}
