std::optional<bool> WindowsServices::registry_key_signaled(std::uint64_t handle) const {
    const auto key=runtime_.registry.handles.find(handle);
    if(key==runtime_.registry.handles.end() || !(key->second.access&0x10U))return {};
    return key->second.notify_signaled;
}
std::optional<std::uint32_t> WindowsServices::invoke_registry_notify(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool native=service.symbol=="NtNotifyChangeKey";
    const auto saved_error=last_error_;const bool async=arg(native?9:4)!=0;
    const auto finish=[&](std::uint32_t code){last_error_=saved_error;arg.return_to(state,code);return std::optional<std::uint32_t>{};};
    const auto error=[&](std::uint32_t code){return finish(!native?code:code==5?0xc0000022U:code==6?0xc0000008U:code==8?0xc0000017U:code==1018?0xc000017cU:code==998?0xc0000005U:0xc000000dU);};
    auto& thread=threads_.items.at(threads_.current);
    if(!async && thread.wait.kind==ThreadWaitKind::RegistryNotify && thread.wait.signaled){
        const auto status=native && thread.wait.handles.size()>1?static_cast<std::uint32_t>(thread.wait.handles[1]):0U;thread.wait={};return finish(status);
    }
    try{
        if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
        auto& registry=runtime_.registry;auto transaction=std::make_unique<RegistryTransaction>(registry,registry_file_,&registry_store_cache_,true);
        if(transaction->error)return error(transaction->error);initialize_registry_views(registry);
        poll_registry_notifications(false,{},0,&memory);
        const auto key=registry.handles.find(arg(0));
        if(native && key==registry.handles.end())return error(6);
        const auto [path,access]=registry_security_handle(arg(0),0x10U);
        if(!(access&0x10U))return error(5);
        auto filter=static_cast<std::uint32_t>(arg(native?5:2));bool subtree=arg(native?6:1)!=0;
        if(native && key->second.notify_filter){filter=key->second.notify_filter;subtree=key->second.notify_subtree;}
        if(!(filter&15U) || (filter&~(native?15U:0x1000000fU)))return error(87);
        if(native && (!arg(4) || arg(7) || arg(8)))return error(87);
        const auto event=async?arg(native?1:3):0;
        if(async && (event || !native)){
            const auto object=runtime_.wait_objects.find(event);
            if(object==runtime_.wait_objects.end() || object->second.kind!=WaitObject::Kind::Event)return error(6);
            if(!(object->second.access&2U))return error(5);
        }
        if(registry_notifications_.size()>=128)return error(8);
        RegistryNotification watch;watch.key=key==registry.handles.end()?static_cast<std::uint32_t>(arg(0)):arg(0);
        watch.event=event;watch.thread=threads_.current;watch.path=path;watch.filter=filter;watch.subtree=subtree;
        if(native){
            watch.native_iosb=arg(4);watch.routine=async?arg(2):0;watch.context=async?arg(3):0;
            if(watch.routine && thread.apcs.size()+static_cast<std::size_t>(std::count_if(registry_notifications_.begin(),registry_notifications_.end(),[&](const auto& entry){return entry.second.thread==threads_.current && entry.second.routine;}))>=1024)return error(8);
            memory.check(GuestAddress{watch.native_iosb},x86_?8U:16U,Access::Write);
        }
        const auto prefix=path+u"\\";
        for(const auto& [name,values]:registry.keys)if(name==path || (name.starts_with(prefix) && (subtree || ((filter&1U) && name.find(u'\\',prefix.size())==std::u16string::npos)))){
            RegistryNotification::KeySnapshot snapshot;
            if((filter&4U) && (name==path || subtree))snapshot.values=values;
            if(filter&2U)snapshot.modified=registry.modified_times.contains(name)?registry.modified_times.at(name):runtime_.filetime_base;
            watch.snapshot.emplace(name,std::move(snapshot));
        }
        if(filter&8U)for(const auto& [name,snapshot]:watch.snapshot){(void)snapshot;watch.security_snapshot.emplace(name,registry_security_descriptor(name));}
        const auto token=next_registry_notification_++;registry_notifications_.emplace(token,std::move(watch));
        if(native){
            key->second.notify_filter=filter;key->second.notify_subtree=subtree;key->second.notify_signaled=false;
            const auto unit=x86_?4U:8U;memory.store(GuestAddress{arg(4)},static_cast<std::uint8_t>(unit*8),0x103U);memory.store(GuestAddress{arg(4)+unit},static_cast<std::uint8_t>(unit*8),0);
            if(event)runtime_.wait_objects.at(event).set_signal(false);
        }
        if(async)return finish(native?0x103U:0U);
        thread.wait={};thread.wait.kind=ThreadWaitKind::RegistryNotify;thread.wait.handles={token,0};thread.status=ThreadStatus::Waiting;
        transaction.reset();schedule(state,memory);return std::nullopt;
    }catch(const SecurityDescriptorError& failure){return error(failure.code);}catch(const MemoryFault&){return error(998);}catch(const std::bad_alloc&){return error(8);}
}
