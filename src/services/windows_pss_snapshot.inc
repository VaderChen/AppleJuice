const WindowsCall pss_call(state,memory,service);const auto saved_error=last_error_;
const auto finish=[&](unsigned error){last_error_=saved_error;pss_call.return_to(state,error);return std::optional<std::uint32_t>{};};
const auto dispose=[&](ProcessState::PssSnapshot& snapshot){
    if(snapshot.vm){std::array<std::uint64_t,5> values{snapshot.storage,0,0x8000U};const auto error=local_memory_operation(memory,3,values,-1,state.reg(Gpr::Rsp));if(error)return error;}
    else if(snapshot.storage && !free_heap(memory,snapshot.storage,state))return 6U;
    snapshot.storage=0;return 0U;
};
const auto storage=[&](ProcessState::PssSnapshot& snapshot) -> unsigned{
    snapshot.vm=(snapshot.flags&0x20000000U)!=0;
    if(snapshot.vm){std::array<std::uint64_t,5> values{0,snapshot.process.size(),0x3000U,4};const auto error=local_memory_operation(memory,2,values,-1);if(error)return error;snapshot.storage=values[0];}
    else snapshot.storage=allocate_heap(memory,snapshot.process.size());
    if(!snapshot.storage)return 8;memory.write(GuestAddress{snapshot.storage},snapshot.process);return 0;
};
try{
    if(!processes_)processes_=std::make_shared<ProcessState>();
    if(service.symbol=="PssCaptureSnapshot"){
        if(!pss_call(3))return finish(87);memory.check(GuestAddress{pss_call(3)},x86_?4U:8U,Access::Write);
        const auto flags=static_cast<std::uint32_t>(pss_call(1));
        // NONE 的完整程序資料與 descriptor 配置；其他表格／VA clone 必須另有實際 capture 後端。
        if(flags&~0xe0000000U)return finish(flags&0x03004402U?87U:50U);
        const auto started=std::chrono::steady_clock::now();
        ProcessState::PssSnapshot snapshot;snapshot.flags=flags;
        std::array<std::uint64_t,5> basic{};std::uint32_t pid{},bits{};
        if(const auto error=process_basic_information(memory,pss_call(0),basic,pid,bits))return finish(error);
        if(pid==process_id()){
            if(const auto error=collect_pss_process_data(memory,snapshot.process))return finish(error);
        }else{
#if !defined(_WIN32)
            std::uint32_t target{};std::uint64_t birth{};auto error=process_memory_target(memory,pss_call(0),0x400U,target,birth);
            if(error==5)error=process_memory_target(memory,pss_call(0),0x1000U,target,birth);if(error)return finish(error);
            auto name=(std::filesystem::temp_directory_path()/"x64arm-pss-XXXXXX").string();std::vector<char> path(name.begin(),name.end());path.push_back(0);
            const auto fd=::mkstemp(path.data());if(fd<0)return finish(8);struct Close{int fd;~Close(){::close(fd);}} close{fd};(void)::unlink(path.data());(void)::fcntl(fd,F_SETFD,FD_CLOEXEC);
            if(::ftruncate(fd,65534))return finish(8);std::array<std::uint64_t,5> values{};unsigned mode{};
            error=remote_process_request(memory,target,birth,14,9,values,mode,fd);if(error)return finish(error);
            if(values[0]<210 || values[0]>728 || values[0]%2)return finish(13);snapshot.process.resize(values[0]);std::size_t done=0;
            while(done<snapshot.process.size()){const auto count=::pread(fd,snapshot.process.data()+done,snapshot.process.size()-done,static_cast<off_t>(done));if(count<0 && errno==EINTR)continue;if(count<=0)return finish(13);done+=static_cast<std::size_t>(count);}
#else
            return finish(50);
#endif
        }
        if(const auto error=storage(snapshot))return finish(error);
        snapshot.performance[1]=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count());
        const auto handle=runtime_.allocate_wait_handle();try{processes_->pss_snapshots.emplace(handle,std::move(snapshot));}catch(...){(void)dispose(snapshot);throw;}
        memory.store(GuestAddress{pss_call(3)},x86_?32:64,handle);return finish(0);
    }
    if(service.symbol=="PssFreeSnapshot"){
        if(!current_process_handle(pss_call(0),0x58U))return finish(current_process_handle(pss_call(0))?5U:6U);
        const auto found=processes_->pss_snapshots.find(pss_call(1));if(found==processes_->pss_snapshots.end())return finish(6);
        if(const auto error=dispose(found->second))return finish(error);processes_->pss_snapshots.erase(found);return finish(0);
    }
    if(service.symbol=="PssDuplicateSnapshot"){
        if(!current_process_handle(pss_call(0),0x50U) || !current_process_handle(pss_call(2),0x68U))return finish(5);
        const auto found=processes_->pss_snapshots.find(pss_call(1));if(found==processes_->pss_snapshots.end())return finish(6);
        if(!pss_call(3) || (pss_call(4)&~1U))return finish(87);memory.check(GuestAddress{pss_call(3)},x86_?4U:8U,Access::Write);
        auto copy=found->second;copy.storage=0;if(const auto error=storage(copy))return finish(error);
        const auto handle=runtime_.allocate_wait_handle();try{processes_->pss_snapshots.emplace(handle,std::move(copy));}catch(...){(void)dispose(copy);throw;}
        if(pss_call(4)&1U){if(const auto error=dispose(found->second)){(void)dispose(processes_->pss_snapshots.at(handle));processes_->pss_snapshots.erase(handle);return finish(error);}processes_->pss_snapshots.erase(found);}
        memory.store(GuestAddress{pss_call(3)},x86_?32:64,handle);return finish(0);
    }
    const auto found=processes_->pss_snapshots.find(pss_call(0));if(found==processes_->pss_snapshots.end())return finish(6);
    const auto cls=static_cast<std::uint32_t>(pss_call(1));if(cls>7)return finish(87);
    if(cls!=0 && cls!=7)return finish(1168);
    const auto& snapshot=found->second;if(cls==7 && !(snapshot.flags&0x40000000U))return finish(1168);
    const unsigned size=cls==7?96U:x86_?636U:704U;if(pss_call(3)<size)return finish(24);if(!pss_call(2))return finish(87);
    std::vector<std::uint8_t> bytes(size);const auto put=[&](unsigned at,unsigned width,std::uint64_t value){for(unsigned b=0;b<width;++b)bytes.at(at+b)=static_cast<std::uint8_t>(value>>(b*8));};
    if(cls==7){for(unsigned i=0;i<12;++i)put(i*8,8,snapshot.performance[i]);}
    else{
        const auto get=[&](unsigned index){std::uint64_t value=0;for(unsigned b=0;b<8;++b)value|=std::uint64_t(snapshot.process.at(index*8+b))<<(b*8);return value;};
        const unsigned unit=x86_?4U:8U;unsigned at=0;
        const auto field=[&](unsigned index,unsigned width){at=(at+width-1)&~(width-1);put(at,width,get(index));at+=width;};
        field(0,4);field(1,unit);field(2,unit);for(unsigned i=3;i<=6;++i)field(i,4);
        // FILETIME 的結構對齊是 DWORD；ULONGLONG 不代替這個 ABI 契約。
        for(unsigned i=7;i<=10;++i){put(at,8,get(i));at+=8;}
        field(11,4);field(12,unit);field(13,unit);field(14,4);for(unsigned i=15;i<=23;++i)field(i,unit);field(24,4);
        const auto count=snapshot.process.size()-208;if(count>520 || count<2 || snapshot.process[snapshot.process.size()-1] || snapshot.process[snapshot.process.size()-2])return finish(13);
        std::copy_n(snapshot.process.begin()+208,count,bytes.begin()+at);
    }
    memory.write(GuestAddress{pss_call(2)},bytes);return finish(0);
}catch(const MemoryFault&){return finish(998);}catch(const std::bad_alloc&){return finish(8);}
