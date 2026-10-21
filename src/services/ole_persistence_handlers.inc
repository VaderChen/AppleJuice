// 四個公開入口共用相同的 guest IStream／IPersistStream 呼叫流程；不解析特定物件 payload。
namespace ole_persistence {
constexpr unsigned scratch_size=128;
enum Kind : unsigned { Save,Load,ReadClass,WriteClass };
enum Phase : unsigned { GetClass,ReadGuid,WriteGuid,SaveObject,CreateObject,LoadObject,QueryObject,ReleasePersist,ReleaseResult };
constexpr std::array<std::uint8_t,16> persist_iid{9,1,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
std::uint64_t method(const GuestMemory& memory,std::uint64_t object,unsigned index){
    const auto bits=static_cast<std::uint8_t>(memory.execution_mode()==ExecutionMode::X86?32:64);
    if(!object)throw HostCallFault("OLE 持續化介面為空");
    const auto table=memory.load(GuestAddress{object},bits);
    if(!table || index>(UINT64_MAX-table)/(bits/8))throw HostCallFault("OLE 持續化 vtable 不合法");
    return memory.load(GuestAddress{table+index*(bits/8)},bits);
}
}

std::optional<std::uint32_t> WindowsServices::invoke_ole_persistence(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using namespace ole_persistence;
    const WindowsCall arg(state,memory,service);
    const auto done=[&](std::uint32_t status){arg.return_to(state,status);return std::optional<std::uint32_t>{};};
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto name=service.symbol;
    const unsigned kind=name=="OleSaveToStream"?Save:name=="OleLoadFromStream"?Load:name=="ReadClassStm"?ReadClass:WriteClass;
    if(kind==Load){if(!arg(2))return done(0x80004003U);memory.store(GuestAddress{arg(2)},bits,0);}
    if(kind==Save && !arg(0))return done(0x80040007U); // OLE_E_BLANK
    if(!arg(0) || !arg(1))return done(0x80070057U);
    if(kind==Load || kind==WriteClass)memory.check(GuestAddress{arg(1)},16,Access::Read);
    if(kind==ReadClass)memory.check(GuestAddress{arg(1)},16,Access::Write);
    const auto scratch=allocate_heap(memory,scratch_size);
    if(!scratch)return done(0x8007000eU);
    try{
        memory.write(GuestAddress{scratch},std::array<std::uint8_t,scratch_size>{});
        memory.store(GuestAddress{scratch+4},32,kind);
        memory.store(GuestAddress{scratch+16},64,kind==Save?arg(1):arg(0));
        memory.store(GuestAddress{scratch+24},64,kind==Save?arg(0):0);
        memory.store(GuestAddress{scratch+32},64,kind==Load?arg(2):kind==ReadClass?arg(1):0);
        memory.write(GuestAddress{scratch+96},persist_iid);
        if(kind==Load){std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{arg(1)},iid);memory.write(GuestAddress{scratch+80},iid);}
        if(kind==WriteClass){std::array<std::uint8_t,16> clsid{};memory.read(GuestAddress{arg(1)},clsid);memory.write(GuestAddress{scratch+64},clsid);}
        const auto object=arg(0);
        const auto phase=kind==Save?GetClass:kind==WriteClass?WriteGuid:ReadGuid;
        const auto target=method(memory,object,kind==WriteClass?4:3);
        if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        memory.store(GuestAddress{scratch},32,phase);
        const std::vector<std::uint64_t> args=kind==Save?std::vector<std::uint64_t>{object,scratch+64}:
            std::vector<std::uint64_t>{object,scratch+64,16,scratch+56};
        start_callbacks(state,memory,{{target,args}},Continuation::OlePersistence,scratch);
    }catch(const std::bad_alloc&){(void)free_heap(memory,scratch,state);return done(0x8007000eU);}
    catch(...){(void)free_heap(memory,scratch,state);throw;}
    return std::nullopt;
}

std::optional<std::uint32_t> WindowsServices::resume_ole_persistence(GuestState& state,GuestMemory& memory,std::uint64_t scratch,std::uint32_t status){
    using namespace ole_persistence;
    const auto kind=static_cast<unsigned>(memory.load(GuestAddress{scratch+4},32));
    const auto phase=static_cast<unsigned>(memory.load(GuestAddress{scratch},32));
    const auto stream=memory.load(GuestAddress{scratch+16},64);
    const auto original=memory.load(GuestAddress{scratch+24},64);
    const auto output=memory.load(GuestAddress{scratch+32},64);
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto callback=[&](unsigned next,std::uint64_t target,std::vector<std::uint64_t> args){
        if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        memory.store(GuestAddress{scratch},32,next);
        start_callbacks(state,memory,{{target,std::move(args)}},Continuation::OlePersistence,scratch);
        return std::optional<std::uint32_t>{};
    };
    const auto call=[&](unsigned next,std::uint64_t object,unsigned slot,std::vector<std::uint64_t> args){
        return callback(next,method(memory,object,slot),std::move(args));
    };
    const auto finish=[&](std::uint32_t result){
        if(!(result&0x80000000U) && kind==Load){
            const auto object=memory.load(GuestAddress{scratch+40},bits);
            memory.store(GuestAddress{output},bits,object);
            memory.store(GuestAddress{scratch+40},bits,0); // 將 QueryInterface 的一份引用交給呼叫者。
        }
        (void)free_heap(memory,scratch,state);complete_call(state,memory,result);
        return std::optional<std::uint32_t>{};
    };
    const auto cleanup=[&](std::uint32_t result){
        memory.store(GuestAddress{scratch+8},32,result);
        const auto persist=memory.load(GuestAddress{scratch+48},bits);
        if(persist){
            const auto target=method(memory,persist,2);
            memory.store(GuestAddress{scratch+48},bits,0);
            return callback(ReleasePersist,target,{persist});
        }
        const auto object=memory.load(GuestAddress{scratch+40},bits);
        if((result&0x80000000U) && object){
            const auto target=method(memory,object,2);
            memory.store(GuestAddress{scratch+40},bits,0);
            return callback(ReleaseResult,target,{object});
        }
        return finish(result);
    };
    try{
        if(phase==ReleasePersist || phase==ReleaseResult)
            return cleanup(static_cast<std::uint32_t>(memory.load(GuestAddress{scratch+8},32)));
        if(status&0x80000000U)return cleanup(status);
        if(phase==GetClass){memory.store(GuestAddress{scratch+56},32,0);return call(WriteGuid,stream,4,{stream,scratch+64,16,scratch+56});}
        if(phase==WriteGuid){
            if(memory.load(GuestAddress{scratch+56},32)!=16)return cleanup(0x8003001dU);
            if(kind==WriteClass)return finish(0);
            return call(SaveObject,original,6,{original,stream,1});
        }
        if(phase==SaveObject)return finish(status);
        if(phase==ReadGuid){
            if(memory.load(GuestAddress{scratch+56},32)!=16)return cleanup(0x8003001eU);
            if(kind==ReadClass){memory.move(GuestAddress{output},GuestAddress{scratch+64},16);return finish(0);}
            std::array<std::uint8_t,16> clsid{};memory.read(GuestAddress{scratch+64},clsid);
            if(std::all_of(clsid.begin(),clsid.end(),[](auto byte){return !byte;}))return finish(0);
            // 使用既有 CoCreateInstance 與客體 class factory，不在宿主實例化 COM 類別。
            return callback(CreateObject,resolve("ole32.dll","CoCreateInstance").value,{scratch+64,0,1,scratch+96,scratch+48});
        }
        if(phase==CreateObject){
            const auto persist=memory.load(GuestAddress{scratch+48},bits);
            if(!persist)return cleanup(0x80004002U);
            return call(LoadObject,persist,5,{persist,stream});
        }
        if(phase==LoadObject){const auto persist=memory.load(GuestAddress{scratch+48},bits);return call(QueryObject,persist,0,{persist,scratch+80,scratch+40});}
        if(phase==QueryObject){if(!memory.load(GuestAddress{scratch+40},bits))return cleanup(0x80004002U);return cleanup(0);}
        throw HostCallFault("OLE 持續化 callback 階段不合法");
    }catch(...){(void)free_heap(memory,scratch,state);throw;}
}
