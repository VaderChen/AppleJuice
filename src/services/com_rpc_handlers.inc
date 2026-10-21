struct ComRpcState {
    com_rpc::Endpoint endpoint;
    std::shared_ptr<com_rpc::Mailbox> incoming;
    std::map<com_rpc::Endpoint,std::shared_ptr<com_rpc::Mailbox>> peers;
    std::function<void(int,bool,bool)> inspect;
    std::uint64_t next_request{1};
};
std::function<void(int,bool,bool)> WindowsServices::com_system_file_protector(){
#if !defined(_WIN32)
    const auto descriptor=com_running_table().duplicate_directory();HostFile directory(descriptor);
    const auto native=security::read_native_file_security(directory,true);
    if(!native.mapped_descriptor)throw com_rpc::TransportError{5};const auto parent=security::file_descriptor(native,0x17);
    const auto system=sddl_sid("SY");if(parent.data[0]!=system || parent.data[1]!=system)throw com_rpc::TransportError{5};
    auto child_directory=security::inherit_descriptor({},parent,system,system,true);
    auto child_file=security::inherit_descriptor({},parent,system,system,false);
    child_directory.control|=0x1000U;child_file.control|=0x1000U;
    return [child_directory,child_file](int descriptor,bool is_directory,bool created){
        const auto duplicate=::fcntl(descriptor,F_DUPFD_CLOEXEC,0);if(duplicate<0)throw com_rpc::TransportError{5};HostFile file(duplicate);
        const auto native=security::read_native_file_security(file,true);const auto& wanted=is_directory?child_directory:child_file;
        if(native.mapped_descriptor){const auto existing=security::file_descriptor(native,0x17);
            if(existing.data!=wanted.data || (existing.control&0x1000U)!=(wanted.control&0x1000U))throw com_rpc::TransportError{5};return;
        }
        if(!created)throw com_rpc::TransportError{5};
        security::write_virtual_file_security(file,wanted,7U|(wanted.control&16U?16U:0U));
    };
#else
    throw com_rpc::TransportError{50};
#endif
}
ComRpcState& WindowsServices::com_rpc_bridge(){
    if(com_rpc_)return *com_rpc_;
    if(!runtime_.network.external_io)throw com_rpc::TransportError{5};
#if !defined(_WIN32)
    auto value=std::make_shared<ComRpcState>();auto& table=com_running_table();
    value->endpoint.nonce=table.nonce();value->endpoint.native_pid=static_cast<std::uint32_t>(::getpid());value->endpoint.guest_pid=process_id();
    const auto identity=process_identity::query(value->endpoint.native_pid);if(identity.error)throw com_rpc::TransportError{identity.error};
    if(identity.exited || !identity.identity.birth)throw com_rpc::TransportError{87};value->endpoint.birth=identity.identity.birth;
    value->inspect=com_system_file_protector();
    const auto descriptor=table.duplicate_directory();
    try{value->incoming=com_rpc::Mailbox::create(descriptor,value->endpoint,value->inspect);::close(descriptor);}
    catch(...){::close(descriptor);throw;}
    runtime_.com_marshal.exports.configure(value->endpoint);com_rpc_=std::move(value);return *com_rpc_;
#else
    throw com_rpc::TransportError{50};
#endif
}
void WindowsServices::send_com_rpc(const com_rpc::Endpoint& peer,const com_rpc::Message& message){
    auto& bridge=com_rpc_bridge();if(message.sender!=bridge.endpoint)throw com_rpc::TransportError{87};
#if !defined(_WIN32)
    const auto found=bridge.peers.find(peer);std::shared_ptr<com_rpc::Mailbox> mailbox;
    if(found!=bridge.peers.end())mailbox=found->second;
    else{
        if(bridge.peers.size()>=256)throw com_rpc::TransportError{8};const auto descriptor=com_running_table().duplicate_directory();
        try{mailbox=com_rpc::Mailbox::open(descriptor,peer,bridge.inspect);::close(descriptor);}catch(...){::close(descriptor);throw;}
        bridge.peers.emplace(peer,mailbox);
    }
    try{mailbox->send(message);}catch(const com_rpc::TransportError& error){if(error.code==109)bridge.peers.erase(peer);throw;}
#else
    (void)peer;(void)message;throw com_rpc::TransportError{50};
#endif
}
std::optional<com_rpc::Message> WindowsServices::receive_com_rpc(){
    if(!com_rpc_)return std::nullopt;return com_rpc_->incoming->receive();
}
com_rpc::Endpoint WindowsServices::com_rpc_endpoint(){return com_rpc_bridge().endpoint;}
std::uint64_t WindowsServices::com_object_generation(GuestMemory& memory,std::uint64_t object,std::uint64_t owner){
    if(!object)throw MemoryFault(GuestAddress{object},Access::Read,1,"COM 物件為空");memory.check(GuestAddress{object},x86_?4:8,Access::Read);
    auto allocation=allocations_.upper_bound(object);if(allocation!=allocations_.begin()){
        --allocation;if(object-allocation->first<allocation->second){const auto generation=runtime_.heap_generations.find(allocation->first);
            if(generation==runtime_.heap_generations.end())throw HostCallFault("COM heap 世代未登錄");return generation->second;}}
    // 靜態、stack 與客體自有配置器仍依 CoDisconnectObject／來源公寓退役。
    // 不將 CPU code generation 誤作資料物件的生命週期。
    auto& bridge=runtime_.com_marshal;if(!owner)owner=threads_.current;const auto apartment=com_apartment(owner);
    const auto key=std::pair{apartment && !*apartment?0:owner,object};if(const auto found=bridge.object_generations.find(key);found!=bridge.object_generations.end())return found->second;
    if(!bridge.next_object_generation || bridge.next_object_generation>=UINT64_C(0x8000000000000000))throw std::bad_alloc();
    const auto generation=UINT64_C(0x8000000000000000)|bridge.next_object_generation++;bridge.object_generations.emplace(key,generation);return generation;
}
std::vector<GuestCallback> WindowsServices::com_exporter_retirements(GuestState&,GuestMemory&){
    std::vector<GuestCallback> callbacks;const auto& pending=runtime_.com_marshal.exports.retirements();callbacks.reserve(pending.size());
    for(const auto& [id,retirement]:pending)if(!retirement.started)callbacks.push_back({resolve("internal-com-rpc","Retire").value,{id}});return callbacks;
}
void WindowsServices::queue_com_exporter_retirements(std::uint64_t thread){
    if(!threads_.items.contains(thread) || threads_.items.at(thread).status==ThreadStatus::Exited)return;
    const auto target=resolve("internal-com-rpc","Retire").value;
    for(const auto& [id,action]:runtime_.com_marshal.exports.retirements()){
        if(action.started)continue;
        if(std::any_of(gui_.sent_messages.begin(),gui_.sent_messages.end(),[&](const auto& row){return row.second.rpc && row.second.rpc->target==target && row.second.rpc->arguments.size()==1 && row.second.rpc->arguments[0]==id;}))continue;
        if(gui_.sent_messages.size()>=8192)throw std::bad_alloc();GuiSentMessage request;request.receiver=thread;request.abandoned=true;request.rpc=GuestCallback{target,{id}};
        gui_.sent_messages.emplace(gui_.next_object++,std::move(request));
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_com_rpc(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);auto& bridge=runtime_.com_marshal;const auto done=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(service.symbol=="FinishRetire"){(void)bridge.exports.finish_retirement(call(0));return done(0);}
    if(service.symbol!="Retire")throw HostCallFault("未知 COM RPC 內部操作");
    const auto id=call(0);const auto pending=bridge.exports.retirements().find(id);if(pending==bridge.exports.retirements().end() || pending->second.started)return done(0);
    const auto action=pending->second;
    ComBindReference reference;
    if(action.kind==com_exporter::Retirement::Kind::Packet){const auto packet=bridge.packets.find(action.packet);
        if(packet && !com_marshal::weak_flags(packet->flags))reference={packet->owner,packet->object,com_apartment(packet->owner)};
    }else reference=action.source;
    if(!reference.object){(void)bridge.exports.start_retirement(id);if(action.kind==com_exporter::Retirement::Kind::Packet)(void)bridge.packets.release(action.packet);(void)bridge.exports.finish_retirement(id);return done(0);}
    const auto owner=com_bind_detail::same_apartment(reference,threads_.current,com_apartment(threads_.current))?threads_.current:reference.owner;
    const auto target=owner==threads_.current?context_method(memory,reference.object,2):resolve("internal-com-proxy","ReleaseAt").value;
    std::vector<GuestCallback> callbacks;callbacks.reserve(2);
    if(owner==threads_.current)callbacks.push_back({target,{reference.object}});else callbacks.push_back({target,{owner,reference.object}});
    callbacks.push_back({resolve("internal-com-rpc","FinishRetire").value,{id}});
    (void)bridge.exports.start_retirement(id);if(action.kind==com_exporter::Retirement::Kind::Packet)(void)bridge.packets.release(action.packet);
    try{start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);}
    catch(...){if(const auto retirement=release_com_proxy_owned(state,memory,{owner,reference.object}))queue_com_proxy_retirement(memory,retirement);(void)bridge.exports.finish_retirement(id);throw;}
    return std::nullopt;
}
