// NT、PSAPI 與跨程序 IPC 共用真正的核心觸頁紀錄；wire 每欄固定 64 位元。
std::uint32_t WindowsServices::working_set_watch_status(std::uint32_t error){
    switch(error){
    case 0:return 0;case 5:return 0xc0000022U;case 6:return 0xc0000008U;
    case 8:return 0xc0000017U;case 24:return 0xc0000004U;case 50:return 0xc00000bbU;
    case 87:return 0xc000000dU;case 122:return 0xc0000023U;case 259:return 0x8000001aU;
    case 998:return 0xc0000005U;case 0x10048U:return 0xc0000048U;default:return 0xc0000001U;
    }
}
std::uint32_t WindowsServices::local_working_set_watch(GuestMemory& memory,bool initialize,std::size_t capacity,
        std::vector<std::uint8_t>& wire,std::size_t& required){
    required=0;wire.clear();
    if(initialize){
        if(memory.working_set_watch_enabled())return 0x10048U;
        memory.enable_working_set_watch();return 0;
    }
    // 消費核心佇列前準備好輸出容量，配置失敗仍保留尚未交付的紀錄。
    wire.reserve(std::min<std::size_t>(capacity,4096)*32);
    const auto changes=memory.read_working_set_changes(capacity);required=changes.required;
    using Status=GuestMemory::WorkingSetReadStatus;
    if(changes.status==Status::Disabled)return 31;
    if(changes.status==Status::Busy)return 259;
    if(changes.status==Status::TooSmall)return 122;
    if(changes.entries.empty() && !changes.lost){required=0;return 259;}
    const auto put=[&](std::uint64_t value){for(unsigned n=0;n<8;++n)wire.push_back(static_cast<std::uint8_t>(value>>(n*8)));};
    for(const auto& entry:changes.entries){put(entry.pc);put(entry.address);put(entry.thread);put(entry.flags);}
    put(0);put(changes.lost);put(0);put(0);return 0;
}
std::uint32_t WindowsServices::process_working_set_watch(GuestMemory& memory,std::uint64_t handle,bool initialize,bool extended,
        std::uint64_t buffer,std::uint32_t capacity,std::uint32_t& returned,bool native){
    returned=0;std::uint32_t pid{},mode{};std::uint64_t birth{};
    const auto rights=initialize && native?0x200U:0x400U;
    if(const auto error=process_memory_target(memory,handle,rights,pid,birth))return error;
    const unsigned unit=x86_?4U:8U,stride=unit*(extended?4U:2U);
    constexpr std::size_t maximum=4096,wire_capacity=maximum*32;
    const auto entries=std::min<std::size_t>(capacity/stride,maximum);
    try{
        if(!initialize && capacity)memory.check(GuestAddress{buffer},std::min<std::size_t>(capacity,maximum*stride),Access::Write);
        std::vector<std::uint8_t> wire,output;std::size_t required=0;std::uint32_t error=0;
        if(!initialize){wire.reserve(entries*32);output.reserve(entries*stride);}
        if(pid==process_id())error=local_working_set_watch(memory,initialize,entries,wire,required);
        else {
            std::array<std::uint64_t,5> values{entries,wire_capacity};
            if(initialize)error=remote_process_request(memory,pid,birth,15,12,values,mode);
            else {
#if !defined(_WIN32)
                auto name=(std::filesystem::temp_directory_path()/"x64arm-ws-watch-XXXXXX").string();
                std::vector<char> pattern(name.begin(),name.end());pattern.push_back(0);
                const auto fd=::mkstemp(pattern.data());if(fd<0)return 8;
                struct Close{int fd;~Close(){::close(fd);}} close{fd};
                (void)::unlink(pattern.data());(void)::fcntl(fd,F_SETFD,FD_CLOEXEC);
                if(::ftruncate(fd,wire_capacity))return 8;
                error=remote_process_request(memory,pid,birth,15,13,values,mode,fd);
                if(values[2]>maximum || values[3]>wire_capacity || values[3]%32)return 13;
                required=static_cast<std::size_t>(values[2]);
                if(!error){
                    if(values[3]!=required*32 || required>entries)return 13;
                    wire.resize(static_cast<std::size_t>(values[3]));
                    for(std::size_t done=0;done<wire.size();){
                        const auto count=::pread(fd,wire.data()+done,wire.size()-done,static_cast<off_t>(done));
                        if(count<0 && errno==EINTR)continue;if(count<=0)return 31;done+=static_cast<std::size_t>(count);
                    }
                }
#else
                return 50;
#endif
            }
        }
        returned=static_cast<std::uint32_t>(required*stride);
        if(error || initialize)return error;
        for(std::size_t at=0;at<wire.size();at+=32)
            for(unsigned field=0;field<(extended?4U:2U);++field)
                output.insert(output.end(),wire.begin()+static_cast<std::ptrdiff_t>(at+field*8),
                    wire.begin()+static_cast<std::ptrdiff_t>(at+field*8+unit));
        memory.write(GuestAddress{buffer},output);return 0;
    }catch(const MemoryFault&){return 998;}catch(const std::bad_alloc&){return 8;}catch(const std::system_error&){return 31;}
}
