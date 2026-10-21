namespace com_bind_detail {
constexpr std::uint32_t pointer_error=0x80004003U,invalid=0x80070057U,no_memory=0x8007000eU,failed=0x80004005U;
ComBindBridge::ReferenceKey key(const ComBindReference& reference){return {reference.owner,reference.object};}
bool same_apartment(const ComBindReference& reference,std::uint64_t thread,std::optional<std::uint64_t> apartment){
    return reference.owner==thread || (reference.apartment && apartment && !*reference.apartment && !*apartment);
}
void put(std::array<std::uint8_t,48>& bytes,unsigned offset,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8));}
}

std::vector<GuestCallback> WindowsServices::release_com_bind_references(GuestMemory& memory,std::span<const ComBindReference> references){
    auto& bridge=runtime_.com_bind;std::vector<GuestCallback> calls;calls.reserve(references.size());
    const auto apartment=com_apartment(threads_.current);
    for(const auto& reference:references){
        const auto key=com_bind_detail::key(reference);
        if(bridge.borrowed.contains(key)){bridge.deferred[key].push_back(reference);continue;}
        const auto owner=com_bind_detail::same_apartment(reference,threads_.current,apartment)?threads_.current:reference.owner;
        // 共用 COM 代理的 ReleaseAt，讓 STA／MTA 引用由正確的來源執行緒釋放。
        if(owner==threads_.current)calls.push_back({context_method(memory,reference.object,2),{reference.object}});
        else calls.push_back({resolve("internal-com-proxy","ReleaseAt").value,{owner,reference.object}});
    }
    return calls;
}
std::vector<GuestCallback> WindowsServices::dispose_com_bind(GuestState& state,GuestMemory& memory,std::uint64_t context){
    auto& objects=runtime_.com_bind.objects;const auto found=objects.find(context);
    if(found==objects.end() || found->second.references || found->second.pins)return {};
    auto references=found->second.bound;references.reserve(references.size()+found->second.parameters.size());
    for(const auto& [key,reference]:found->second.parameters){(void)key;references.push_back(reference);}
    auto calls=release_com_bind_references(memory,references);
    objects.erase(found);(void)free_heap(memory,context,state);return calls;
}
std::uint64_t WindowsServices::create_com_bind_enum(GuestState& state,GuestMemory& memory,ComBindEnum value){
    auto& bridge=runtime_.com_bind;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(bridge.enumerators.size()>=4096)throw std::bad_alloc();
    if(!bridge.enum_table){const auto table=runtime_allocate(memory,{},7*unit);unsigned i=0;
        for(const auto& method:com_context_service_descriptors())if(method.family=="internal-com-bind-enum")
            memory.store(GuestAddress{table+unit*i++},bits,resolve(method.family,method.symbol).value);
        bridge.enum_table=table;
    }
    const auto object=allocate_heap(memory,unit);if(!object)throw std::bad_alloc();
    try{memory.store(GuestAddress{object},bits,bridge.enum_table);bridge.enumerators.emplace(object,std::move(value));}
    catch(...){(void)free_heap(memory,object,state);throw;}return object;
}
void WindowsServices::discard_com_bind(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& bridge=runtime_.com_bind;const auto found=bridge.operations.find(id);if(found==bridge.operations.end())return;
    auto op=std::move(found->second);bridge.operations.erase(found);
    const auto defer=[&](const ComBindReference& reference){
        const auto owner=com_bind_detail::same_apartment(reference,threads_.current,com_apartment(threads_.current))?threads_.current:reference.owner;
        if(const auto retirement=release_com_proxy_owned(state,memory,{owner,reference.object}))queue_com_proxy_retirement(memory,retirement);
    };
    if(op.acquired)defer(op.kind==ComBindOperation::Kind::Get?ComBindReference{op.caller,op.delivered,com_apartment(op.caller)}:op.reference);
    if(op.borrowed){const auto key=com_bind_detail::key(op.reference);const auto borrowed=bridge.borrowed.find(key);
        if(borrowed!=bridge.borrowed.end() && !--borrowed->second){bridge.borrowed.erase(borrowed);
            if(const auto pending=bridge.deferred.find(key);pending!=bridge.deferred.end()){auto refs=std::move(pending->second);bridge.deferred.erase(pending);for(const auto& reference:refs)defer(reference);}}
    }
    const auto context=bridge.objects.find(op.context);
    if(context!=bridge.objects.end()){
        if(context->second.pins)--context->second.pins;
        if(!context->second.references && !context->second.pins){auto record=std::move(context->second);bridge.objects.erase(context);(void)free_heap(memory,op.context,state);
            for(const auto& reference:record.bound)defer(reference);for(const auto& [key,reference]:record.parameters){(void)key;defer(reference);}}
    }
    if(op.scratch)(void)free_heap(memory,op.scratch,state);
}

std::optional<std::uint32_t> WindowsServices::resume_com_bind(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t returned){
    using namespace com_bind_detail;auto& bridge=runtime_.com_bind;auto& op=bridge.operations.at(id);
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    try{
        std::uint32_t status=0;std::vector<GuestCallback> releases;
        if(op.awaiting){
            op.awaiting=false;
            if(op.proxy){status=static_cast<std::uint32_t>(memory.load(GuestAddress{op.scratch},32));op.delivered=returned;
                if(!status && !returned)status=failed;op.acquired=!status;
            }else{op.acquired=true;if(op.kind==ComBindOperation::Kind::Get)op.delivered=op.reference.object;}
        }
        auto& context=bridge.objects.at(op.context);
        if(!status){
            if(op.kind==ComBindOperation::Kind::Bound){if(context.bound.size()>=4096)status=no_memory;else{context.bound.push_back(op.reference);op.acquired=false;}}
            else if(op.kind==ComBindOperation::Kind::Set){
                const auto found=context.parameters.find(op.key);
                if(found==context.parameters.end()){if(context.parameters.size()>=4096)status=no_memory;else{context.parameters.emplace(op.key,op.reference);op.acquired=false;}}
                else{const auto previous=found->second;releases=release_com_bind_references(memory,std::span{&previous,1});found->second=op.reference;op.acquired=false;}
            }else{memory.store(GuestAddress{op.output},bits,op.delivered);op.acquired=false;}
        }
        if(op.borrowed){const auto borrowed_key=key(op.reference);const auto borrowed=bridge.borrowed.find(borrowed_key);
            if(borrowed==bridge.borrowed.end() || !borrowed->second)throw HostCallFault("IBindCtx 借用引用不存在");
            if(!--borrowed->second){bridge.borrowed.erase(borrowed);
                if(const auto pending=bridge.deferred.find(borrowed_key);pending!=bridge.deferred.end()){
                    auto calls=release_com_bind_references(memory,pending->second);releases.insert(releases.end(),std::make_move_iterator(calls.begin()),std::make_move_iterator(calls.end()));bridge.deferred.erase(pending);}}
            op.borrowed=false;
        }
        if(op.acquired){const auto reference=op.kind==ComBindOperation::Kind::Get?ComBindReference{op.caller,op.delivered,com_apartment(op.caller)}:op.reference;
            auto calls=release_com_bind_references(memory,std::span{&reference,1});releases.insert(releases.end(),std::make_move_iterator(calls.begin()),std::make_move_iterator(calls.end()));op.acquired=false;}
        if(!context.pins)throw HostCallFault("IBindCtx operation 沒有 context pin");--context.pins;
        const auto address=op.context,scratch=op.scratch;bridge.operations.erase(id);
        auto calls=dispose_com_bind(state,memory,address);releases.insert(releases.end(),std::make_move_iterator(calls.begin()),std::make_move_iterator(calls.end()));
        if(scratch)(void)free_heap(memory,scratch,state);
        if(releases.empty())complete_call(state,memory,status);else start_callbacks(state,memory,std::move(releases),Continuation::Return,status);
        return std::nullopt;
    }catch(const std::bad_alloc&){discard_com_bind(state,memory,id);complete_call(state,memory,no_memory);return std::nullopt;}
    catch(...){discard_com_bind(state,memory,id);throw;}
}

std::optional<std::uint32_t> WindowsServices::invoke_com_bind(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using namespace com_bind_detail;const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    auto& bridge=runtime_.com_bind;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto release=[&](std::vector<GuestCallback> calls,std::uint64_t value){if(calls.empty())return done(value);start_callbacks(state,memory,std::move(calls),Continuation::Return,value);return std::optional<std::uint32_t>{};};
    try{
        if(name=="CreateBindCtx"){
            if(!arg(1))return done(pointer_error);memory.store(GuestAddress{arg(1)},bits,0);if(static_cast<std::uint32_t>(arg(0)))return done(invalid);
            if(bridge.objects.size()>=4096)return done(no_memory);
            if(!bridge.table){const auto table=runtime_allocate(memory,{},13*unit);unsigned i=0;
                for(const auto& method:com_context_service_descriptors()){if(method.family=="internal-com-bindctx")
                    memory.store(GuestAddress{table+unit*i++},bits,resolve(method.family,method.symbol).value);}
                bridge.table=table;}
            ComBindObject value;put(value.options,0,16);put(value.options,8,2);put(value.options,20,0x15);put(value.options,24,0x409);
            const auto object=allocate_heap(memory,unit);if(!object)return done(no_memory);
            try{memory.store(GuestAddress{object},bits,bridge.table);bridge.objects.emplace(object,std::move(value));memory.store(GuestAddress{arg(1)},bits,object);}
            catch(...){bridge.objects.erase(object);(void)free_heap(memory,object,state);throw;}return done(0);
        }
        const auto object=arg(0);const bool enumeration=service.family=="internal-com-bind-enum";
        auto context=bridge.objects.find(object);auto enumerator=bridge.enumerators.find(object);
        if(enumeration?enumerator==bridge.enumerators.end():context==bridge.objects.end())return done(name=="AddRef" || name=="Release"?0:pointer_error);
        auto& references=enumeration?enumerator->second.references:context->second.references;
        if(name=="QueryInterface"){
            if(!arg(2))return done(pointer_error);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(pointer_error);
            WindowsGuid iid{};memory.read(GuestAddress{arg(1)},iid);
            if(iid!=context_iid(0) && iid!=context_iid(enumeration?0x101:0x0e))return done(0x80004002U);
            if(references==UINT32_MAX)return done(no_memory);++references;memory.store(GuestAddress{arg(2)},bits,object);return done(0);
        }
        if(name=="AddRef"){if(references==UINT32_MAX)return done(0);return done(++references);}
        if(name=="Release"){
            if(!references)return done(0);if(--references)return done(references);
            if(enumeration){bridge.enumerators.erase(enumerator);(void)free_heap(memory,object,state);return done(0);}
            return release(dispose_com_bind(state,memory,object),0);
        }
        if(enumeration){
            auto& value=enumerator->second;
            if(name=="Reset"){value.cursor=0;return done(0);}
            if(name=="Skip"){const auto requested=static_cast<std::uint32_t>(arg(1));const auto count=std::min<std::size_t>(requested,value.names.size()-value.cursor);value.cursor+=count;return done(count==requested?0:1);}
            if(name=="Clone"){if(!arg(1))return done(pointer_error);memory.store(GuestAddress{arg(1)},bits,0);auto copy=value;copy.references=1;const auto output=create_com_bind_enum(state,memory,std::move(copy));memory.store(GuestAddress{arg(1)},bits,output);return done(0);}
            if(name!="Next")throw HostCallFault("未知 IEnumString 方法");
            const auto requested=static_cast<std::uint32_t>(arg(1));if(!arg(3) && requested>1)return done(pointer_error);
            if(arg(3))memory.store(GuestAddress{arg(3)},32,0);if(requested && !arg(2))return done(pointer_error);
            const auto count=std::min<std::size_t>(requested,value.names.size()-value.cursor);memory.check(GuestAddress{arg(2)},count*unit,Access::Write);
            std::vector<std::uint64_t> allocated;allocated.reserve(count);
            try{for(std::size_t i=0;i<count;++i){const auto bytes=utf16_bytes(value.names[value.cursor+i]);const auto address=allocate_heap(memory,bytes.size());if(!address)throw std::bad_alloc();allocated.push_back(address);memory.write(GuestAddress{address},bytes);gui_.task_allocations.insert(address);}
                for(std::size_t i=0;i<count;++i)memory.store(GuestAddress{arg(2)+i*unit},bits,allocated[i]);if(arg(3))memory.store(GuestAddress{arg(3)},32,count);
            }catch(...){for(const auto address:allocated)(void)free_heap(memory,address,state);throw;}
            value.cursor+=count;return done(count==requested?0:1);
        }
        auto& value=context->second;
        if(name=="SetBindOptions" || name=="GetBindOptions"){
            if(!arg(1))return done(pointer_error);const auto size=memory.load(GuestAddress{arg(1)},32);
            const auto two=x86_?32U:40U,three=x86_?36U:48U;if(size!=16 && size!=two && size!=three)return done(invalid);
            if(name=="SetBindOptions"){auto options=value.options;memory.read(GuestAddress{arg(1)},std::span(options).first(size));value.options=options;}
            else{auto options=value.options;put(options,0,static_cast<std::uint32_t>(size));memory.write(GuestAddress{arg(1)},std::span(options).first(size));}return done(0);
        }
        if(name=="GetRunningObjectTable"){
            if(!arg(1))return done(pointer_error);memory.store(GuestAddress{arg(1)},bits,0);
            // ROT 的機器範圍 driver 接在公開入口；此家族尚未完成時不宣稱空表成功。
            const auto target=resolve("ole32.dll","GetRunningObjectTable").value;
            if(!target)throw HostCallFault("ROT 機器範圍 driver 尚待整合");
            start_callbacks(state,memory,{{target,{0,arg(1)}}},Continuation::ReturnCallback);return std::nullopt;
        }
        if(name=="EnumObjectParam"){
            if(!arg(1))return done(pointer_error);memory.store(GuestAddress{arg(1)},bits,0);ComBindEnum names_snapshot;
            names_snapshot.names.reserve(value.parameters.size());for(const auto& [key,reference]:value.parameters){(void)reference;names_snapshot.names.push_back(key);}
            const auto output=create_com_bind_enum(state,memory,std::move(names_snapshot));memory.store(GuestAddress{arg(1)},bits,output);return done(0);
        }
        if(name=="ReleaseBoundObjects"){auto calls=release_com_bind_references(memory,value.bound);value.bound.clear();return release(std::move(calls),0);}
        if(name=="RevokeObjectBound"){
            const auto item=std::find_if(value.bound.begin(),value.bound.end(),[&](const auto& reference){return reference.object==arg(1);});
            if(item==value.bound.end())return done(0x800401e9U);auto calls=release_com_bind_references(memory,std::span{&*item,1});value.bound.erase(item);return release(std::move(calls),0);
        }
        ComBindOperation op;op.context=object;op.caller=threads_.current;
        if(name=="RegisterObjectBound"){
            if(!arg(1))return done(invalid);if(value.bound.size()>=4096)return done(no_memory);op.kind=ComBindOperation::Kind::Bound;op.reference={threads_.current,arg(1),com_apartment(threads_.current)};
        }else{
            if(!arg(1))return done(invalid);op.key=read_utf16(memory,GuestAddress{arg(1)},32767);
            if(name=="RevokeObjectParam"){const auto found=value.parameters.find(op.key);if(found==value.parameters.end())return done(failed);auto calls=release_com_bind_references(memory,std::span{&found->second,1});value.parameters.erase(found);return release(std::move(calls),0);}
            if(name=="RegisterObjectParam"){if(!arg(2))return done(invalid);if(value.parameters.size()>=4096 && !value.parameters.contains(op.key))return done(no_memory);op.kind=ComBindOperation::Kind::Set;op.reference={threads_.current,arg(2),com_apartment(threads_.current)};}
            else if(name=="GetObjectParam"){
                if(!arg(2))return done(pointer_error);memory.store(GuestAddress{arg(2)},bits,0);const auto found=value.parameters.find(op.key);if(found==value.parameters.end())return done(failed);
                op.kind=ComBindOperation::Kind::Get;op.reference=found->second;op.output=arg(2);op.borrowed=true;
                op.proxy=!same_apartment(op.reference,threads_.current,com_apartment(threads_.current));
            }else throw HostCallFault("未知 IBindCtx 方法");
        }
        if(value.pins==UINT32_MAX || bridge.operations.size()>=4096)return done(no_memory);
        if(op.proxy){op.scratch=allocate_heap(memory,8);if(!op.scratch)return done(no_memory);memory.store(GuestAddress{op.scratch},32,0);}
        const auto scratch=op.scratch;const auto borrowed_key=key(op.reference);const bool borrowing=op.borrowed;bool borrowed_added=false;
        auto id=bridge.next;while(!id || bridge.operations.contains(id))++id;bridge.next=id+1;
        try{if(borrowing){++bridge.borrowed[borrowed_key];borrowed_added=true;}bridge.operations.emplace(id,std::move(op));}
        catch(...){if(borrowed_added && !--bridge.borrowed.at(borrowed_key))bridge.borrowed.erase(borrowed_key);if(scratch)(void)free_heap(memory,scratch,state);throw;}
        ++value.pins;auto& pending=bridge.operations.at(id);pending.awaiting=true;
        try{GuestCallback call;
            if(pending.proxy)call={resolve("internal-com-proxy","Marshal").value,{pending.reference.object,pending.reference.owner,threads_.current,static_cast<unsigned>(com_proxy::Kind::Unknown),pending.scratch}};
            else call={context_method(memory,pending.reference.object,1),{pending.reference.object}};
            start_callbacks(state,memory,{std::move(call)},Continuation::ComBindOperation,id);
        }catch(...){discard_com_bind(state,memory,id);throw;}return std::nullopt;
    }catch(const MemoryFault&){return done(pointer_error);}catch(const std::bad_alloc&){return done(no_memory);}
}
