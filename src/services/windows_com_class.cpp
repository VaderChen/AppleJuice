#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,not_implemented=0x80004001U;
constexpr std::uint32_t not_initialized=0x800401f0U,wrong_thread=0x8001010eU;
constexpr std::uint32_t class_not_registered=0x80040154U,no_interface=0x80004002U,no_memory=0x8007000eU;
constexpr unsigned scratch_size=128;
enum Phase : unsigned { RegisterAddRef,RegisterUndo,HoldRef,Query,ReleaseHold,Create,ReleaseFactory };
enum Kind : unsigned { Registration,GetClass,CreateObject,QueryService,Runnable };
constexpr std::array<std::uint8_t,16> class_factory_iid{1,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
std::uint64_t method(const GuestMemory& memory,std::uint64_t object,unsigned index){
    const auto bits=memory.execution_mode()==ExecutionMode::X86?32U:64U;
    const auto table=memory.load(GuestAddress{object},static_cast<std::uint8_t>(bits));
    if(!table || index>(UINT64_MAX-table)/(bits/8))throw HostCallFault("COM vtable 位址不合法");
    return memory.load(GuestAddress{table+index*(bits/8)},static_cast<std::uint8_t>(bits));
}
}

std::optional<std::uint64_t> WindowsServices::com_apartment(std::uint64_t thread) const {
    const auto found=gui_.com_initializations.find(thread);
    if(found==gui_.com_initializations.end() || found->second<16)return std::nullopt;
    return (found->second&2U)?thread:0;
}

std::vector<GuestCallback> WindowsServices::revoke_com_classes(GuestMemory& memory,std::uint64_t thread,bool callbacks){
    std::vector<GuestCallback> result;
    const auto initialized=gui_.com_initializations.find(thread);
    if(initialized==gui_.com_initializations.end())return result;
    // CoUninitialize 已減少初始化次數，但尚未清除 apartment mode。
    const auto apartment=(initialized->second&2U)?thread:0;
    if(!apartment && std::any_of(gui_.com_initializations.begin(),gui_.com_initializations.end(),
        [&](const auto& entry){return entry.first!=thread && entry.second>=16 && !(entry.second&2U);}))return result;
    for(auto it=runtime_.com_classes.begin();it!=runtime_.com_classes.end();){
        auto& record=it->second;
        if(record.apartment!=apartment){++it;continue;}
        // AddRef 內可重入 CoUninitialize；等 AddRef 返回後再配對 Release。
        if(record.pending && callbacks){record.canceled=true;++it;continue;}
        if(callbacks){
            const auto target=method(memory,record.object,2);
            if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
            result.push_back({target,{record.object}});
        }
        it=runtime_.com_classes.erase(it);
    }
    return result;
}

std::optional<std::uint32_t> WindowsServices::invoke_com_class(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto done=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(name=="OleRun" || name=="OleIsRunning" || name=="OleLockRunning" || name=="OleSetContainedObject"){
        const unsigned slot=name=="OleRun"?4:name=="OleIsRunning"?5:name=="OleLockRunning"?6:7;
        if(!arg(0))return done(slot==5?0:invalid);
        const auto target=method(memory,arg(0),0);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        const auto scratch=allocate_heap(memory,scratch_size);if(!scratch)return done(slot==5?0:no_memory);
        constexpr std::array<std::uint8_t,16> runnable_iid{0x26,1,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
        memory.write(GuestAddress{scratch},std::array<std::uint8_t,scratch_size>{});
        memory.store(GuestAddress{scratch},32,Query);memory.store(GuestAddress{scratch+8},32,Runnable);
        memory.store(GuestAddress{scratch+24},32,slot);
        if(slot>=6)memory.store(GuestAddress{scratch+32},32,arg(1));
        if(slot==6)memory.store(GuestAddress{scratch+40},32,arg(2));
        memory.write(GuestAddress{scratch+96},runnable_iid);
        try{start_callbacks(state,memory,{{target,{arg(0),scratch+96,scratch+56}}},Continuation::ComOperation,scratch);}
        catch(...){discard_com_operation(state,memory,scratch);throw;}
        return std::nullopt;
    }
    if(name=="IUnknown_QueryService"){
        if(!arg(3))return done(pointer_error);memory.store(GuestAddress{arg(3)},x86_?32:64,0);
        if(!arg(0))return done(0x80004005U);if(!arg(1) || !arg(2))return done(invalid);
        memory.check(GuestAddress{arg(1)},16,Access::Read);memory.check(GuestAddress{arg(2)},16,Access::Read);
        const auto target=method(memory,arg(0),0);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        const auto scratch=allocate_heap(memory,scratch_size);if(!scratch)return done(no_memory);
        constexpr std::array<std::uint8_t,16> provider_iid{0xc1,0x40,0x51,0x6d,0x36,0x74,0xce,0x11,0x80,0x34,0,0xaa,0,0x60,9,0xfa};
        memory.write(GuestAddress{scratch},std::array<std::uint8_t,scratch_size>{});
        memory.store(GuestAddress{scratch},32,Query);memory.store(GuestAddress{scratch+8},32,QueryService);
        memory.store(GuestAddress{scratch+16},64,arg(3));memory.store(GuestAddress{scratch+24},64,arg(2));memory.store(GuestAddress{scratch+32},64,arg(1));
        memory.write(GuestAddress{scratch+96},provider_iid);
        try{start_callbacks(state,memory,{{target,{arg(0),scratch+96,scratch+56}}},Continuation::ComOperation,scratch);}
        catch(...){discard_com_operation(state,memory,scratch);throw;}
        return std::nullopt;
    }
    if(name=="CoAddRefServerProcess")return done(++runtime_.com_server_references);
    if(name=="CoReleaseServerProcess"){
        if(runtime_.com_server_references)--runtime_.com_server_references;
        if(!runtime_.com_server_references)runtime_.com_classes_suspended=true;
        return done(runtime_.com_server_references);
    }
    if(name=="CoSuspendClassObjects"){runtime_.com_classes_suspended=true;return done(0);}
    if(name=="CoResumeClassObjects"){
        runtime_.com_classes_suspended=false;
        for(auto& [cookie,record]:runtime_.com_classes){(void)cookie;record.suspended=false;}
        return done(0);
    }
    const auto apartment=com_apartment(threads_.current);
    if(!apartment)return done(not_initialized);
    if(name=="CoRevokeClassObject"){
        const auto found=runtime_.com_classes.find(static_cast<std::uint32_t>(arg(0)));
        if(found==runtime_.com_classes.end() || found->second.pending)return done(invalid);
        if(found->second.apartment!=*apartment)return done(wrong_thread);
        const auto object=found->second.object,target=method(memory,object,2);
        if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        runtime_.com_classes.erase(found);
        start_callbacks(state,memory,{{target,{object}}},Continuation::Return,0);return std::nullopt;
    }
    if(!arg(4))return done(pointer_error);
    memory.store(GuestAddress{arg(4)},32,0);
    if(!arg(0) || !arg(1))return done(invalid);
    const auto context=static_cast<std::uint32_t>(arg(2)),flags=static_cast<std::uint32_t>(arg(3));
    if((context!=1U && context!=4U && context!=5U) || (flags&~31U) || (flags&3U)==3U ||
       (!(flags&3U) && context!=4U))return done(invalid);
    if(flags&8U)return done(not_implemented); // Surrogate 需要獨立 DLL host／RPC。
    if(runtime_.com_classes.size()>=4096)return done(no_memory);
    std::array<std::uint8_t,16> clsid{};memory.read(GuestAddress{arg(0)},clsid);
    const auto object=arg(1),target=method(memory,object,1);
    for(const auto slot:{0U,1U,2U}){
        const auto function=method(memory,object,slot);
        if(!handles(GuestAddress{function}))memory.check(GuestAddress{function},1,Access::Execute);
    }
    const auto scratch=allocate_heap(memory,scratch_size);if(!scratch)return done(no_memory);
    memory.write(GuestAddress{scratch},std::array<std::uint8_t,scratch_size>{});
    auto cookie=runtime_.next_com_class;
    while(!cookie || runtime_.com_classes.contains(cookie))++cookie;
    runtime_.next_com_class=cookie+1;
    // MULTIPLEUSE local server 同時提供本程序 INPROC；MULTI_SEPARATE 保留各 context。
    const auto effective=context|((flags&3U)==1U && (context&4U)?1U:0U);
    try {
        runtime_.com_classes.emplace(cookie,ComClassRegistration{clsid,object,*apartment,effective,flags,true,false,(flags&4U)!=0,false,false});
        memory.store(GuestAddress{scratch},32,RegisterAddRef);
        memory.store(GuestAddress{scratch+8},32,Registration);
        memory.store(GuestAddress{scratch+12},32,cookie);
        memory.store(GuestAddress{scratch+16},64,arg(4));
        memory.store(GuestAddress{scratch+40},64,object);
        start_callbacks(state,memory,{{target,{object}}},Continuation::ComOperation,scratch);
    } catch(...) {discard_com_operation(state,memory,scratch);throw;}
    return std::nullopt;
}

bool WindowsServices::begin_registered_com(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool create=service.symbol=="CoCreateInstance";
    const auto done=[&](std::uint32_t value){arg.return_to(state,value);return true;};
    if(!arg(4))return done(pointer_error);
    memory.store(GuestAddress{arg(4)},x86_?32:64,0);
    if(!arg(0) || !arg(3))return done(invalid);
    std::array<std::uint8_t,16> clsid{};memory.read(GuestAddress{arg(0)},clsid);
    memory.check(GuestAddress{arg(3)},16,Access::Read);
    const bool registered=std::any_of(runtime_.com_classes.begin(),runtime_.com_classes.end(),[&](const auto& pair){return pair.second.clsid==clsid;});
    if(!registered)return begin_inproc_com(state,memory,service); // 登錄 DLL 與內建工廠仍共用一般 COM 啟用流程。
    const auto apartment=com_apartment(threads_.current);if(!apartment)return done(not_initialized);
    const auto context=static_cast<std::uint32_t>(arg(create?2:1));
    if(!context)return done(invalid);
    if(context&~0x17U)return done(not_implemented);
    if(!create && arg(2))return done(not_implemented); // COSERVERINFO／遠端 activation 不在本程序註冊表中。
    std::uint32_t cookie=0;bool foreign=false;
    // 優先使用本程序 in-process context；全程序 suspend 阻擋 local-server activation。
    for(const auto requested:{1U,2U,4U}){
        if(!(context&requested))continue;
        for(const auto& [key,record]:runtime_.com_classes){
            if(record.clsid!=clsid || !(record.context&requested) || record.pending || record.canceled ||
               record.suspended || record.reserved || record.used || (requested==4U && runtime_.com_classes_suspended))continue;
            if(record.apartment!=*apartment && !(record.flags&16U)){foreign=true;continue;}
            cookie=key;break;
        }
        if(cookie)break;
    }
    if(!cookie)return done(foreign?wrong_thread:class_not_registered);
    const auto object=runtime_.com_classes.at(cookie).object,target=method(memory,object,1);
    if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
    const auto scratch=allocate_heap(memory,scratch_size);if(!scratch)return done(no_memory);
    memory.write(GuestAddress{scratch},std::array<std::uint8_t,scratch_size>{});
    memory.store(GuestAddress{scratch},32,HoldRef);
    memory.store(GuestAddress{scratch+8},32,create?CreateObject:GetClass);
    memory.store(GuestAddress{scratch+12},32,cookie);
    memory.store(GuestAddress{scratch+16},64,arg(4));
    memory.store(GuestAddress{scratch+24},64,arg(3));
    memory.store(GuestAddress{scratch+32},64,create?arg(1):0);
    memory.store(GuestAddress{scratch+40},64,object);
    memory.write(GuestAddress{scratch+96},class_factory_iid);
    if(!(runtime_.com_classes.at(cookie).flags&3U))runtime_.com_classes.at(cookie).reserved=true;
    try{start_callbacks(state,memory,{{target,{object}}},Continuation::ComOperation,scratch);}
    catch(...){discard_com_operation(state,memory,scratch);throw;}
    return true;
}

std::optional<std::uint32_t> WindowsServices::resume_com_operation(GuestState& state,GuestMemory& memory,std::uint64_t scratch,std::uint32_t result){
    const auto load=[&](unsigned offset,unsigned bits=64){return memory.load(GuestAddress{scratch+offset},static_cast<std::uint8_t>(bits));};
    const auto store=[&](unsigned offset,std::uint64_t value,unsigned bits=64){memory.store(GuestAddress{scratch+offset},static_cast<std::uint8_t>(bits),value);};
    const auto phase=static_cast<Phase>(load(0,32));const auto kind=static_cast<Kind>(load(8,32));
    const auto cookie=static_cast<std::uint32_t>(load(12,32)),bits=x86_?32U:64U;
    const auto callback=[&](std::uint64_t object,unsigned slot,std::vector<std::uint64_t> arguments,Phase next){
        const auto target=method(memory,object,slot);
        if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        store(0,next,32);start_callbacks(state,memory,{{target,std::move(arguments)}},Continuation::ComOperation,scratch);
        return std::optional<std::uint32_t>{};
    };
    const auto finish=[&](std::uint32_t status,std::uint64_t value=0){
        if(kind!=Registration && kind!=Runnable)memory.store(GuestAddress{load(16)},static_cast<std::uint8_t>(bits),status&0x80000000U?0:value);
        discard_com_operation(state,memory,scratch);complete_call(state,memory,status);return std::optional<std::uint32_t>{};
    };
    try {
        if(kind==Runnable){
            const auto slot=static_cast<unsigned>(load(24,32));
            if(phase==Query){
                // 使用真正的 QueryInterface／方法／Release；callback 可重入，
                // 原始 HRESULT／BOOL 保存在各自的 continuation，不受 Release 覆蓋。
                if(result&0x80000000U)return finish(slot==5?1U:0U);
                const auto object=load(56,bits);if(!object)return finish(slot==5?0:pointer_error);
                std::vector<std::uint64_t> arguments{object};
                if(slot==4)arguments.push_back(0);
                if(slot>=6)arguments.push_back(load(32,32));
                if(slot==6)arguments.push_back(load(40,32));
                return callback(object,slot,std::move(arguments),Create);
            }
            if(phase==Create){store(4,result,32);return callback(load(56,bits),2,{load(56,bits)},ReleaseFactory);}
            if(phase==ReleaseFactory){store(56,0);return finish(static_cast<std::uint32_t>(load(4,32)));}
            throw HostCallFault("OLE 執行狀態 continuation 階段不合法");
        }
        if(phase==RegisterAddRef){
            const auto found=runtime_.com_classes.find(cookie);
            if(found==runtime_.com_classes.end() || found->second.canceled){
                if(found!=runtime_.com_classes.end())runtime_.com_classes.erase(found);
                return callback(load(40),2,{load(40)},RegisterUndo);
            }
            found->second.pending=false;
            memory.store(GuestAddress{load(16)},32,cookie);
            (void)free_heap(memory,scratch,state);complete_call(state,memory,0);return std::nullopt;
        }
        if(phase==RegisterUndo)return finish(not_initialized);
        if(phase==HoldRef)return callback(load(40),0,{load(40),kind==CreateObject?scratch+96:load(24),scratch+56},Query);
        if(phase==Query){
            if(!(result&0x80000000U) && !load(56,bits))result=no_interface;
            if(kind==QueryService){
                if(result&0x80000000U)return finish(0x80004005U);
                const auto provider=load(56,bits);
                return callback(provider,3,{provider,load(32),load(24),scratch+64},Create);
            }
            store(4,result,32);
            if(const auto found=runtime_.com_classes.find(cookie);found!=runtime_.com_classes.end() && found->second.reserved){
                found->second.reserved=false;
                if(!(result&0x80000000U))found->second.used=true;
            }
            return callback(load(40),2,{load(40)},ReleaseHold);
        }
        if(phase==ReleaseHold){
            store(40,0);
            const auto status=static_cast<std::uint32_t>(load(4,32));const auto factory=load(56,bits);
            if(status&0x80000000U)return finish(status);
            if(kind==GetClass)return finish(status,factory);
            return callback(factory,3,{factory,load(32),load(24),scratch+64},Create);
        }
        if(phase==Create){
            if(!(result&0x80000000U) && !load(64,bits))result=no_interface;
            store(4,result,32);return callback(load(56,bits),2,{load(56,bits)},ReleaseFactory);
        }
        if(phase==ReleaseFactory){store(56,0);return finish(static_cast<std::uint32_t>(load(4,32)),load(64,bits));}
        throw HostCallFault("COM activation continuation 階段不合法");
    } catch(...) {discard_com_operation(state,memory,scratch);throw;}
}

void WindowsServices::discard_com_operation(GuestState& state,GuestMemory& memory,std::uint64_t scratch){
    if(!memory.mapped(GuestAddress{scratch}))return;
    const auto cookie=static_cast<std::uint32_t>(memory.load(GuestAddress{scratch+12},32));
    if(const auto found=runtime_.com_classes.find(cookie);found!=runtime_.com_classes.end()){
        if(memory.load(GuestAddress{scratch+8},32)==Registration && found->second.pending)runtime_.com_classes.erase(found);
        else found->second.reserved=false;
    }
    // SEH／強制終止跨越 callback 時不能再次進入已離開的 guest frame。
    (void)free_heap(memory,scratch,state);
}
}
