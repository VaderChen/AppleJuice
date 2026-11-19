// 舊版序號傳遞真正的號誌控制代碼，名稱前綴及 MAX_PATH 截斷與 Windows 相同。
if(name=="#222" || name=="#422" || name=="#423"){
    std::u16string object_name=u"shell.";
    std::uint32_t initial=0;
    if(name=="#222"){
        WindowsGuid id{};
        memory.read(GuestAddress{arg(0)},id);
        object_name+=windows_guid_text(id);
    }else{
        initial=u32(1);
        if(arg(0) && name=="#422")object_name+=utf8_to_utf16_lossy(read_ansi(memory,arg(0))).substr(0,253);
        if(arg(0) && name=="#423")for(unsigned i=0;i<253;++i){
            const auto unit=static_cast<char16_t>(memory.load(GuestAddress{arg(0)+2*i},16));
            if(!unit)break;
            object_name+=unit;
        }
    }
    GuestScratch scratch(*this,memory,state);
    const auto text=scratch.text(object_name), attributes=scratch.allocate(24+descriptor_size);
    if(!text || !attributes)return done(0);
    all_access_attributes(attributes,attributes+24);
    const auto semaphore=handle(host("kernel32.dll","CreateSemaphoreW",{attributes,initial,INT32_MAX,text}));
    if(semaphore)return done(semaphore);
    return done(host("kernel32.dll","OpenSemaphoreW",{0x100002,0,text}));
}
if(name=="#223" || name=="#224" || name=="#424"){
    GuestScratch scratch(*this,memory,state);
    const auto previous=scratch.allocate(4);
    if(!previous)return done(0);
    const auto semaphore=arg(0);
    memory.store(GuestAddress{previous},32,0);
    (void)host("kernel32.dll","ReleaseSemaphore",{semaphore,1,previous});
    const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{previous},32));
    if(name=="#224")return done(value+1U);
    (void)host("kernel32.dll","WaitForSingleObject",{semaphore,0});
    if(name=="#223")return done(value);
    (void)host("kernel32.dll","WaitForSingleObject",{semaphore,0});
    return done(value-1U);
}
if(name.starts_with("SHGlobalCounter")){
    if(u32(0)>=X64ARM_SHELL_COUNTER_COUNT)return done(0);
    const auto page=memory.load(gui_timing_cell_,pointer_bits);
    const GuestAddress slot{page+X64ARM_SHELL_COUNTER_OFFSET+4*u32(0)};
    if(name=="SHGlobalCounterSetValue")return done(memory.atomic_rmw(slot,32,GuestMemory::AtomicRmw::Exchange,u32(1)));
    const std::uint32_t delta=name=="SHGlobalCounterIncrement"?1U:name=="SHGlobalCounterDecrement"?UINT32_MAX:0U;
    return done(static_cast<std::uint32_t>(memory.atomic_rmw(slot,32,GuestMemory::AtomicRmw::Add,delta))+delta);
}
