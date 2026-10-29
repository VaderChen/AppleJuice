// 包含於 Native 服務的共用參數擷取與 NTSTATUS 錯誤處理範圍。
if(name=="NtCreateWaitCompletionPacket" || name=="NtCreateIoCompletion"){
    const bool packet=name=="NtCreateWaitCompletionPacket";const auto attr=object_attributes(memory,call(2),x86_,true);
    if(!call(0))return finish(0xc0000005U);memory.check(GuestAddress{call(0)},unit,Access::Write);
    // 命名完成埠的跨程序 namespace 尚未有 provider，不能建立假的程序私有名稱。
    if(attr.root || !attr.name.empty())return finish(0xc00000bbU);
    const auto all=packet?0x1f0001U:0x1f0003U;
    const auto descriptor=attr.security?object_security_descriptor(security::read_descriptor(memory,attr.security,x86_,true),true,true):synchronization_descriptor(memory,0,false).bytes;
    const auto access=object_security_access(descriptor,static_cast<std::uint32_t>(call(1)),{0x20001U,packet?0x20001U:0x20002U,0x120000U,all});
    if(access&~all)return finish(0xc0000022U);
    const auto handle=runtime_.allocate_wait_handle();
    if(packet){WaitCompletionPacketState item;item.security_descriptor=descriptor;wait_completion_packets_.emplace(handle,std::move(item));wait_completion_handles_.emplace(handle,std::pair{handle,access});}
    else {CompletionPort item;item.access=access;runtime_.completion_ports.emplace(handle,std::move(item));}
    if(attr.flags&2U)runtime_.handle_flags[handle]=1;
    memory.store(GuestAddress{call(0)},bits,handle);return finish(0);
}
if(name=="NtAssociateWaitCompletionPacket" || name=="NtCancelWaitCompletionPacket"){
    const auto handle=wait_completion_handles_.find(call(0));
    if(handle==wait_completion_handles_.end())return finish(object_type(call(0)).empty()?0xc0000008U:0xc0000024U);
    if(!(handle->second.second&1U))return finish(0xc0000022U);
    const auto id=handle->second.first;auto& wait=wait_completion_packets_.at(id);
    if(name=="NtCancelWaitCompletionPacket")return finish(cancel_wait_packet(memory,id,static_cast<std::uint8_t>(call(1))!=0));
    if(wait.phase!=WaitCompletionPacketState::Phase::Idle)return finish(0xc000000dU);
    if(call(7))memory.check(GuestAddress{call(7)},1,Access::Write);
    const auto port=runtime_.completion_ports.find(call(1));if(port==runtime_.completion_ports.end())return finish(object_type(call(1)).empty()?0xc0000008U:0xc0000024U);
    if(!(port->second.access&2U))return finish(0xc0000022U);
    const auto target=object_metadata(call(2));if(!target)return finish(0xc0000008U);
    if(!(target->access&0x100000U))return finish(0xc0000022U);
    const auto retained=retain_wait_completion_target(call(2));if(!retained)return finish(0xc0000024U);
    wait.target=retained;wait.port=call(1);wait.packet={call(3),call(4),static_cast<std::uint32_t>(call(5)),call(6),id};wait.phase=WaitCompletionPacketState::Phase::Armed;
    poll_wait_completion_packets(memory);
    if(call(7))memory.store(GuestAddress{call(7)},8,wait.phase==WaitCompletionPacketState::Phase::Queued);
    return finish(0);
}
if(name=="NtSetIoCompletion" || name=="NtQueryIoCompletion" || name=="NtRemoveIoCompletion" || name=="NtRemoveIoCompletionEx"){
    const auto port=runtime_.completion_ports.find(call(0));if(port==runtime_.completion_ports.end())return finish(object_type(call(0)).empty()?0xc0000008U:0xc0000024U);
    const bool query=name=="NtQueryIoCompletion";if(!(port->second.access&(query?1U:2U)))return finish(0xc0000022U);
    if(query){
        if(call(1))return finish(0xc0000003U);if(call(4))memory.store(GuestAddress{call(4)},32,4);
        if(static_cast<std::uint32_t>(call(3))!=4)return finish(0xc0000004U);
        memory.store(GuestAddress{call(2)},32,port->second.packets.size());return finish(0);
    }
    if(name=="NtSetIoCompletion"){port->second.packets.push_back({call(1),call(2),static_cast<std::uint32_t>(call(3)),call(4)});return finish(0);}
    const bool extended=name.ends_with("Ex");const auto count=extended?static_cast<std::uint32_t>(call(2)):1U;
    if(!count || count>65536)return finish(0xc000000dU);
    memory.check(GuestAddress{call(1)},extended?std::uint64_t{count}*4*unit:unit,Access::Write);
    if(!extended)memory.check(GuestAddress{call(2)},unit,Access::Write);
    memory.check(GuestAddress{call(3)},extended?4:unit*2,Access::Write);
    if(extended)memory.store(GuestAddress{call(3)},32,0);
    std::uint64_t deadline=UINT64_MAX;
    if(call(4)){const auto timeout=memory.load(GuestAddress{call(4)},64);deadline=(timeout>>63)?runtime_.clock_ticks+std::min(~timeout+1,UINT64_MAX-runtime_.clock_ticks):runtime_.system_deadline(timeout);}
    auto& current=threads_.items.at(threads_.current);
    if(extended && static_cast<std::uint8_t>(call(5)) && !current.apcs.empty()){
        current.wait={};current.last_native_status=0xc0;
        if(dispatch_user_apc(state,memory,0xc0))return std::nullopt;
    }
    poll_wait_completion_packets(memory);
    const auto completed=poll_native_completion(state,memory,call(0),extended,deadline<=runtime_.clock_ticks);
    if(completed.ready)return finish(completed.value);
    auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::Completion;
    thread.wait.handles={call(0),1};thread.wait.all=extended;thread.wait.alertable=extended && static_cast<std::uint8_t>(call(5));thread.wait.deadline=deadline;
    schedule(state,memory);return std::nullopt;
}
