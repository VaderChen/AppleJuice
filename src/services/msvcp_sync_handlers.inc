// Microsoft STL v14 的 ABI 轉接；鎖、等待與回呼都交由既有 Win32 實作。
if(service.family=="internal-msvcp" && name=="GlobalLocksInit"){
    if(call(1)!=runtime_.msvcp_global_locks)throw HostCallFault("STL 共用鎖初始化參數無效");
    std::vector<GuestCallback> callbacks;
    for(unsigned i=1;i<8;++i)callbacks.push_back({resolve("kernel32.dll","InitializeCriticalSection").value,{call(1)+unit+i*(x86_?24U:40U)}});
    start_callbacks(state,memory,std::move(callbacks),Continuation::Return,1);return std::nullopt;
}
if(name.find("_Lockit@std@@")!=name.npos){
    const bool object=name.find("_Lockit@std@@")!=name.npos,constructor=name.starts_with("??0");
    const auto address=object?(x86_?state.reg(Gpr::Rcx)&UINT32_MAX:call(0)):0;
    std::uint32_t kind=0;
    if(object){
        memory.check(GuestAddress{address},4,constructor?Access::Write:Access::Read);
        if(constructor){kind=name.find("@H@Z")!=name.npos?static_cast<std::uint32_t>(call(x86_?0:1)):0;memory.store(GuestAddress{address},32,kind);}
        else kind=static_cast<std::uint32_t>(memory.load(GuestAddress{address},32));
        if(kind&0x80000000U)throw HostCallFault("STL 鎖種類不可為負數");
        if(kind>=8)return finish(constructor?address:0);
    }
    op.arguments[0]=address;op.attributes=kind;
    // locale 鎖已有 CRT 共用模型，直接轉接，不重新初始化或改變既有狀態。
    if(!kind){op.information=1;return start("crt",constructor?"_lock_locales":"_unlock_locales",{});}
    if(!runtime_.msvcp_global_locks){
        const auto size=unit+8*(x86_?24U:40U);const auto storage=runtime_allocate(memory,{},size);
        memory.write(GuestAddress{storage},std::vector<std::uint8_t>(size));runtime_.msvcp_global_locks=storage;
    }
    return start("kernel32.dll","InitOnceExecuteOnce",{runtime_.msvcp_global_locks,resolve("internal-msvcp","GlobalLocksInit").value,runtime_.msvcp_global_locks,0});
}
if(service.family=="msvcp140.dll"){
    if(name=="_Thrd_yield")return start("kernel32.dll","SwitchToThread",{});
    if(name.starts_with("?_Execute_once@std@@"))
        return start("kernel32.dll","InitOnceExecuteOnce",{call(0),call(1),call(2),0});
    const bool mutex=name.starts_with("_Mtx_");
    const auto address=call(0),owner=address+(x86_?40U:72U),lock=address+unit*2;
    memory.check(GuestAddress{address},mutex?(x86_?48U:80U):(x86_?40U:72U),Access::Write);
    if(name.ends_with("init_in_situ")){
        if(mutex){
            memory.store(GuestAddress{address},32,call(1));
            memory.store(GuestAddress{address+unit},bits,0);
            memory.store(GuestAddress{owner},32,UINT32_MAX);
            memory.store(GuestAddress{owner+4},32,0);
        }else memory.store(GuestAddress{address},bits,0);
        return start("kernel32.dll",mutex?"InitializeSRWLock":"InitializeConditionVariable",
                     {mutex?lock:address+unit});
    }
    if(name.ends_with("destroy_in_situ")){
        if(mutex){
            if(memory.load(GuestAddress{owner+4},32))throw HostCallFault("不可銷毀持有中的 MSVC mutex");
            runtime_.slim_locks.erase(lock);
        }
        // 條件變數沒有獨立配置；STL 的原位解構器本身不修改儲存區。
        return finish(0);
    }
    if(mutex){
        const auto count=memory.load(GuestAddress{owner+4},32);
        const bool owned=memory.load(GuestAddress{owner},32)==thread_identifier(threads_.current);
        if(name=="_Mtx_lock"){
            if(owned){
                const auto type=memory.load(GuestAddress{address},32);
                if((type&~0x100U)!=1 && !(type&0x100U))return finish(3);
                if(count==INT32_MAX)return finish(3);
                memory.store(GuestAddress{owner+4},32,count+1);return finish(0);
            }
            return start("kernel32.dll","AcquireSRWLockExclusive",{lock});
        }
        if(!owned || !count)throw HostCallFault("執行緒未持有 MSVC mutex");
        memory.store(GuestAddress{owner+4},32,count-1);
        if(count>1)return finish(0);
        memory.store(GuestAddress{owner},32,UINT32_MAX);
        return start("kernel32.dll","ReleaseSRWLockExclusive",{lock});
    }
    if(name=="_Cnd_wait"){
        const auto mtx=call(1),mtx_owner=mtx+(x86_?40U:72U);
        memory.check(GuestAddress{mtx},x86_?48U:80U,Access::Write);
        if(memory.load(GuestAddress{mtx_owner},32)!=thread_identifier(threads_.current) || memory.load(GuestAddress{mtx_owner+4},32)!=1)
            throw HostCallFault("MSVC 條件變數必須搭配只取得一次的 mutex");
        memory.store(GuestAddress{mtx_owner},32,UINT32_MAX);
        memory.store(GuestAddress{mtx_owner+4},32,0);
        return start("kernel32.dll","SleepConditionVariableSRW",{address+unit,mtx+unit*2,UINT32_MAX,0});
    }
    return start("kernel32.dll",name=="_Cnd_signal"?"WakeConditionVariable":"WakeAllConditionVariable",{address+unit});
}
