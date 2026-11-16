if(const auto directory=runtime_.object_directories.find(arg(1));directory!=runtime_.object_directories.end()){
    const auto source=arg(1);auto object=directory->second;
    const bool close_source=(dword(6)&1U) && current_process_handle(arg(0),0x40) && !(runtime_.handle_flags[source]&2U);
    const auto close=[&]{if(close_source)close_transfer_handle(memory,source);};
    struct Cleanup{const decltype(close)& action;~Cleanup(){action();}} cleanup{close};
    if(!current_process_handle(arg(0),0x40) || (arg(2) && !current_process_handle(arg(2),0x40)))fail(5);
    else if(dword(6)&~3U)fail(87);
    else if((dword(6)&1U) && !close_source)fail(5);
    else if(!arg(2)){if(close_source)result=1;else fail(87);}
    else if(!arg(3))fail(87);
    else if(runtime_.object_directories.size()>=process_handle_limit)fail(8);
    else try{
        memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);
        if(!(dword(6)&2U)){initialize_tokens();object.access=security::duplicate_object_access(runtime_.token_objects.at(effective_token_id()),security::parse_relative_descriptor(shared_object_security(object.object->descriptor())),object.access,dword(4),{0x20001U,0x2000cU,0x20002U,0xf000fU});}
        const auto handle=runtime_.allocate_wait_handle();runtime_.object_directories.emplace(handle,std::move(object));runtime_.handle_flags[handle]=arg(5)?1U:0U;
        memory.store(GuestAddress{arg(3)},x86_?32:64,handle);result=1;
    }catch(const SecurityDescriptorError& error){fail(error.code);}
    arg.return_to(state,result);return std::nullopt;
}
