if(const auto found=wait_completion_handles_.find(arg(1));found!=wait_completion_handles_.end() && current_process_handle(arg(0)) && (!arg(2) || current_process_handle(arg(2)))){
    const auto id=found->second.first;const bool close=(dword(6)&1U)!=0;
    if(!current_process_handle(arg(0),0x40) || (arg(2) && !current_process_handle(arg(2),0x40)))fail(5);
    else if((dword(6)&~3U) || (!arg(2) && !close) || (arg(2) && !arg(3)))fail(87);
    else if(close && (runtime_.handle_flags[arg(1)]&2U))fail(5);
    else {
        if(arg(2)){
            memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);
            const auto access=(dword(6)&2U)?found->second.second:security::mapped_access(dword(4),{0x20001U,0x20001U,0x120000U,0x1f0001U});
            if(access&~found->second.second)fail(5);
            else {const auto copy=runtime_.allocate_wait_handle();wait_completion_handles_.emplace(copy,std::pair{id,access});runtime_.handle_flags[copy]=arg(5)?1:0;memory.store(GuestAddress{arg(3)},x86_?32:64,copy);result=1;}
        }else result=1;
        if(close){(void)close_wait_packet(memory,arg(1));runtime_.handle_flags.erase(arg(1));}
    }
    arg.return_to(state,result);return std::nullopt;
}
