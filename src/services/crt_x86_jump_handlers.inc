namespace crt_x86_jump {
constexpr std::uint32_t signature=0x56433230U;
constexpr std::array registers{Gpr::Rbp,Gpr::Rbx,Gpr::Rdi,Gpr::Rsi,Gpr::Rsp};
GuestState restore(const GuestMemory& memory,std::uint64_t buffer,const GuestState& current,std::uint32_t value){
    memory.check(GuestAddress{buffer},64,Access::Read);
    auto restored=current;
    for(unsigned i=0;i<registers.size();++i)restored.reg(registers[i])=memory.load(GuestAddress{buffer+i*4},32);
    restored.rip=GuestAddress{memory.load(GuestAddress{buffer+20},32)};restored.reg(Gpr::Rax)=value?value:1;
    memory.check(restored.rip,1,Access::Execute);
    const auto low=memory.load(GuestAddress{current.fs_base+8},32),high=memory.load(GuestAddress{current.fs_base+4},32),sp=restored.reg(Gpr::Rsp);
    if((sp&3U) || sp<low || sp>high || high-sp<4)throw HostCallFault("Win32 longjmp 目標不在目前執行緒堆疊內");
    memory.check(GuestAddress{sp},4,Access::Read);return restored;
}
}

std::optional<std::uint32_t> WindowsServices::invoke_x86_jump(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const auto buffer=arg(0);
    if(symbol.find("setjmp")!=std::string_view::npos){
        memory.check(GuestAddress{buffer},64,Access::Write);
        auto caller=state;arg.return_to(caller,0);
        std::array<std::uint8_t,64> bytes{};const auto put=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
        for(unsigned i=0;i<crt_x86_jump::registers.size();++i)put(i*4,static_cast<std::uint32_t>(caller.reg(crt_x86_jump::registers[i])));
        put(20,static_cast<std::uint32_t>(caller.rip.value));
        const auto registration=static_cast<std::uint32_t>(memory.load(GuestAddress{state.fs_base},32));put(24,registration);
        std::uint32_t level=UINT32_MAX;
        if(registration!=UINT32_MAX){
            const auto handler=memory.load(GuestAddress{registration+4},32);
            // 引擎的 nested-exception guard 是 8-byte record，沒有 scope／TryLevel。
            if(handler!=callback_return_.value+0x90)level=static_cast<std::uint32_t>(memory.load(GuestAddress{registration+12},32));
        }
        if(symbol=="_setjmp3"){
            put(32,crt_x86_jump::signature);
            const auto count=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1)));
            if(count>0)put(36,static_cast<std::uint32_t>(arg(2)));
            if(count>1)level=static_cast<std::uint32_t>(arg(3));
            for(int i=2;i<count && i<8;++i)put(40+static_cast<unsigned>(i-2)*4,static_cast<std::uint32_t>(arg(static_cast<unsigned>(i+2))));
        }
        put(28,level);memory.write(GuestAddress{buffer},bytes);arg.return_to(state,0);return std::nullopt;
    }
    const auto restored=crt_x86_jump::restore(memory,buffer,state,static_cast<std::uint32_t>(arg(1)));
    if(const auto* image=module_image(restored.rip.value,true);image && image->guard_long_jump &&
       !std::binary_search(image->guard_long_jump_targets.begin(),image->guard_long_jump_targets.end(),static_cast<std::uint32_t>(restored.rip.value-image->base)))return 0xc0000409U;
    const auto registration=static_cast<std::uint32_t>(memory.load(GuestAddress{buffer+24},32));
    const auto current=static_cast<std::uint32_t>(memory.load(GuestAddress{state.fs_base},32));
    if(current!=registration){
        auto caller=state;arg.return_to(caller,0);
        const std::array<std::uint64_t,1> parameters{buffer};begin_exception(caller,memory,0x80000026U,2,parameters);
        auto& dispatch=exceptions_.stack.back();dispatch.stage=ExceptionStage::Unwind;dispatch.x86_target=registration;
        dispatch.flags|=2;memory.store(GuestAddress{dispatch.record+4},32,dispatch.flags);
        dispatch.target_context=restored;dispatch.restore_jump=true;dispatch.x86_jump_buffer=buffer;
        state=caller;return std::nullopt;
    }
    return finish_x86_longjmp(state,memory,buffer,restored);
}

std::optional<std::uint32_t> WindowsServices::finish_x86_longjmp(GuestState& state,GuestMemory& memory,std::uint64_t buffer,const GuestState& restored){
    const auto registration=memory.load(GuestAddress{buffer+24},32);
    const bool modern=memory.load(GuestAddress{buffer+32},32)==crt_x86_jump::signature;
    const auto unwind=modern?memory.load(GuestAddress{buffer+36},32):0;
    GuestCallback call;
    if(unwind)call={unwind,{buffer}};
    else if(registration!=UINT32_MAX && memory.load(GuestAddress{registration+4},32)!=callback_return_.value+0x90)
        call={resolve("crt","_local_unwind2").value,{registration,memory.load(GuestAddress{buffer+28},32)}};
    if(runtime_.crt_jump_operations.size()>=128)throw HostCallFault("Win32 longjmp 續接操作超過上限");
    auto token=runtime_.next_crt_jump_operation++;while(!token || runtime_.crt_jump_operations.contains(token))token=runtime_.next_crt_jump_operation++;
    runtime_.crt_jump_operations.emplace(token,CrtJumpOperation{restored,buffer,static_cast<std::uint32_t>(registration)});
    if(!call.target)return resume_x86_longjmp(state,memory,token);
    try{start_callbacks(state,memory,{std::move(call)},Continuation::CrtLongjmp,token);}
    catch(...){runtime_.crt_jump_operations.erase(token);throw;}return std::nullopt;
}

std::optional<std::uint32_t> WindowsServices::resume_x86_longjmp(GuestState& state,GuestMemory& memory,std::uint64_t token){
    const auto found=runtime_.crt_jump_operations.find(token);if(found==runtime_.crt_jump_operations.end())throw HostCallFault("Win32 longjmp 續接狀態不存在");
    const auto operation=found->second;runtime_.crt_jump_operations.erase(found);
    const auto restored=operation.restored;
    // 丟棄被跳躍跨過的 callback；局部 finally 必須在這一步之前完成。
    while(!runtime_.callbacks.empty() && restored.reg(Gpr::Rsp)>=((runtime_.callbacks.back().saved.reg(Gpr::Rsp)-256)&~UINT64_C(3))){
        const auto depth=runtime_.callbacks.back().exception_depth;
        if(depth>exceptions_.stack.size())throw HostCallFault("Win32 longjmp callback 例外深度不合法");
        for(std::size_t i=exceptions_.stack.size();i>depth;--i)leave_x86_exception_handler(memory,exceptions_.stack[i-1]);
        discard_callbacks(runtime_.callbacks.size()-1,state,memory);exceptions_.stack.resize(depth);
    }
    memory.store(GuestAddress{restored.fs_base},32,operation.registration);
    state=restored;return std::nullopt;
}
