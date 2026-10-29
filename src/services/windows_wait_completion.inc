// 完成封包沿用現有物件狀態與協調者喚醒，不建立額外輪詢執行緒。
std::uint64_t WindowsServices::retain_wait_completion_target(std::uint64_t handle){
    std::uint64_t copy=0;
    if(const auto at=runtime_.wait_objects.find(handle);at!=runtime_.wait_objects.end()){
        if(at->second.kind==WaitObject::Kind::Mutex)return 0;
        if(at->second.kind==WaitObject::Kind::Event || at->second.kind==WaitObject::Kind::MemoryResource){
            InheritedHandle item;if(export_transfer_handle(handle,item))return 0;
        }
        copy=runtime_.allocate_wait_handle();runtime_.wait_objects.emplace(copy,at->second);
    }else if(const auto id=thread_id(handle)){
        copy=runtime_.allocate_wait_handle();runtime_.process_thread_handles.emplace(copy,std::pair{id,false});runtime_.thread_handle_access[copy]=thread_access(handle);
    }else if(files_.contains(handle)){
        copy=runtime_.allocate_wait_handle();files_.emplace(copy,files_.at(handle));
    }else copy=retain_wait_completion_process(handle);
    if(copy)wait_completion_targets_.insert(copy);return copy;
}
void WindowsServices::release_wait_completion_target(GuestMemory& memory,std::uint64_t handle){
    if(!handle)return;wait_completion_targets_.erase(handle);
    if(close_wait_object(handle) || close_process(handle))return;
    if(runtime_.process_thread_handles.erase(handle)){runtime_.thread_handle_access.erase(handle);return;}
    if(files_.contains(handle))close_transfer_handle(memory,handle);
}
void WindowsServices::complete_wait_packet(std::uint64_t id){
    if(!id)return;const auto at=wait_completion_packets_.find(id);if(at==wait_completion_packets_.end())return;
    at->second.phase=WaitCompletionPacketState::Phase::Idle;at->second.port=0;
}
void WindowsServices::poll_wait_completion_packets(GuestMemory& memory){
    for(auto& [id,wait]:wait_completion_packets_){
        (void)id;if(wait.phase!=WaitCompletionPacketState::Phase::Armed)continue;
        const auto port=runtime_.completion_ports.find(wait.port);
        if(port==runtime_.completion_ports.end()){
            release_wait_completion_target(memory,std::exchange(wait.target,0));wait.phase=WaitCompletionPacketState::Phase::Idle;wait.port=0;continue;
        }
        bool signaled=false;
        if(const auto object=runtime_.wait_objects.find(wait.target);object!=runtime_.wait_objects.end()){
            if(!runtime_.wait_state(object->second).signaled())continue;
            SharedEventLock lock(runtime_.wait_lock_descriptor(object->second));auto& signal=runtime_.wait_state(object->second);
            signaled=signal.signaled();if(signaled){port->second.packets.push_back(wait.packet);signal.consume_signal();}
        }else{
            if(const auto thread=thread_id(wait.target))signaled=threads_.items.at(thread).status==ThreadStatus::Exited;
            else if(const auto file=file_io_signaled(wait.target))signaled=*file;
            else if(const auto process=process_signaled(wait.target))signaled=*process;
            if(signaled)port->second.packets.push_back(wait.packet);
        }
        if(signaled){wait.phase=WaitCompletionPacketState::Phase::Queued;release_wait_completion_target(memory,std::exchange(wait.target,0));}
    }
}
std::uint32_t WindowsServices::cancel_wait_packet(GuestMemory& memory,std::uint64_t id,bool remove){
    auto& wait=wait_completion_packets_.at(id);
    if(wait.phase==WaitCompletionPacketState::Phase::Idle)return 0xc0000120U;
    if(wait.phase==WaitCompletionPacketState::Phase::Queued){
        if(!remove)return 0x103U;
        if(const auto port=runtime_.completion_ports.find(wait.port);port!=runtime_.completion_ports.end())
            std::erase_if(port->second.packets,[&](const auto& packet){return packet.wait_packet==id;});
    }
    release_wait_completion_target(memory,std::exchange(wait.target,0));complete_wait_packet(id);return 0;
}
bool WindowsServices::close_wait_packet(GuestMemory& memory,std::uint64_t handle){
    const auto at=wait_completion_handles_.find(handle);if(at==wait_completion_handles_.end())return false;
    const auto id=at->second.first;wait_completion_handles_.erase(at);
    if(std::none_of(wait_completion_handles_.begin(),wait_completion_handles_.end(),[&](const auto& item){return item.second.first==id;})){
        (void)cancel_wait_packet(memory,id,true);wait_completion_packets_.erase(id);
    }
    return true;
}
bool WindowsServices::close_completion_port(GuestMemory& memory,std::uint64_t handle){
    if(!runtime_.completion_ports.contains(handle))return false;
    for(auto& [id,wait]:wait_completion_packets_)if(wait.port==handle)(void)cancel_wait_packet(memory,id,true);
    runtime_.completion_ports.erase(handle);return true;
}
WindowsServices::CompletionResult WindowsServices::poll_native_completion(GuestState& state,GuestMemory& memory,std::uint64_t handle,bool extended,bool expired){
    const auto port=runtime_.completion_ports.find(handle);if(port==runtime_.completion_ports.end())return {0xc0000008U,0,true,false};
    auto& packets=port->second.packets;if(packets.empty())return {0x102U,0,expired,false};
    const WindowsCall call(state,memory,extended?6:5);const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    try{
        const auto count=extended?std::min<std::uint64_t>(static_cast<std::uint32_t>(call(2)),packets.size()):1U;
        if(extended){memory.check(GuestAddress{call(1)},count*unit*4,Access::Write);memory.check(GuestAddress{call(3)},4,Access::Write);}
        else {memory.check(GuestAddress{call(1)},unit,Access::Write);memory.check(GuestAddress{call(2)},unit,Access::Write);memory.check(GuestAddress{call(3)},unit*2,Access::Write);}
        for(std::size_t i=0;i<count;++i){
            const auto& packet=packets[i];const auto row=call(1)+i*unit*4;
            memory.store(GuestAddress{extended?row:call(1)},bits,packet.key);
            memory.store(GuestAddress{extended?row+unit:call(2)},bits,packet.overlapped);
            memory.store(GuestAddress{extended?row+unit*2:call(3)},bits,static_cast<std::uint32_t>(packet.status));
            memory.store(GuestAddress{extended?row+unit*3:call(3)+unit},bits,packet.bytes);
        }
        if(extended)memory.store(GuestAddress{call(3)},32,count);
        for(std::size_t i=0;i<count;++i){complete_wait_packet(packets.front().wait_packet);packets.pop_front();}
        return {0,0,true,false};
    }catch(const MemoryFault&){return {0xc0000005U,0,true,false};}
}
