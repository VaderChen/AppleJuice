#include "x64arm/windows_services.hpp"
#include "windows_loader_lock.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
struct NativeError {std::uint32_t status;};
std::uint32_t nt_status(std::uint32_t error){
    switch(error){case 0:return 0;case 2:return 0xc0000034U;case 3:return 0xc000003aU;case 5:return 0xc0000022U;case 6:return 0xc0000008U;
    case 8:return 0xc0000017U;case 24:return 0xc0000004U;case 32:return 0xc0000043U;case 33:return 0xc0000054U;case 38:return 0xc0000011U;
    case 50:return 0xc00000bbU;case 80:case 183:return 0xc0000035U;case 87:return 0xc000000dU;case 109:return 0xc000014bU;
    case 112:return 0xc000007fU;case 123:return 0xc0000033U;case 234:return 0x80000005U;case 267:return 0xc0000103U;
    case 288:return 0xc0000046U;case 298:return 0xc0000047U;case 995:return 0xc0000120U;case 997:return 0x103U;case 998:return 0xc0000005U;
    case 1008:return 0xc000007cU;case 1314:return 0xc0000061U;case 1346:return 0xc00000a5U;case 1347:return 0xc00000a4U;default:return 0xc0000001U;}
}
struct ObjectAttributes {std::u16string name;std::uint64_t root{},security{};std::uint32_t flags{};};
std::u16string native_string(const GuestMemory& memory,std::uint64_t address,bool x86){
    const auto length=memory.load(GuestAddress{address},16),maximum=memory.load(GuestAddress{address+2},16),buffer=memory.load(GuestAddress{address+(x86?4U:8U)},x86?32:64);
    if(length%2 || length>maximum || (length && !buffer))throw NativeError{0xc000000dU};
    std::u16string result;for(std::uint64_t n=0;n<length;n+=2)result.push_back(static_cast<char16_t>(memory.load(GuestAddress{buffer+n},16)));return result;
}
ObjectAttributes object_attributes(const GuestMemory& memory,std::uint64_t address,bool x86,bool optional=false){
    if(!address){if(optional)return {};throw NativeError{0xc000000dU};}
    if(memory.load(GuestAddress{address},32)!=(x86?24U:48U))throw NativeError{0xc000000dU};
    const auto unit=x86?4U:8U;const auto pointer=[&](unsigned n){return memory.load(GuestAddress{address+n*unit},x86?32:64);};
    ObjectAttributes result;result.root=pointer(1);result.flags=static_cast<std::uint32_t>(pointer(3));result.security=pointer(4);
    // 核心模式 handle 與自訂 SQOS 不能冒充已支援的使用者模式物件。
    if((result.flags&~0x1c2U) || pointer(5))throw NativeError{0xc000000dU};
    if(pointer(2))result.name=native_string(memory,pointer(2),x86);return result;
}
}
std::span<const WindowsServiceDescriptor> nt_native_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","NtCreateFile",11,0,0,0x5e2},{"ntdll.dll","NtOpenFile",6,0,0,0x32},
        {"ntdll.dll","NtQueryAttributesFile",2},{"ntdll.dll","NtQueryFullAttributesFile",2},
        {"ntdll.dll","NtReadFile",9,0,0,0x40},{"ntdll.dll","NtWriteFile",9,0,0,0x40},
        {"ntdll.dll","NtOpenProcess",4,0,0,0x2},{"ntdll.dll","NtOpenThread",4,0,0,0x2},
        {"ntdll.dll","NtOpenProcessToken",3,0,0,0x2},{"ntdll.dll","NtOpenProcessTokenEx",4,0,0,0x6},
        {"ntdll.dll","NtOpenThreadToken",4,0,0,0x6},{"ntdll.dll","NtOpenThreadTokenEx",5,0,0,0xe},
        {"ntdll.dll","NtDuplicateObject",7,0,0,0x70},{"ntdll.dll","NtWaitForSingleObject",3,0,0,0x2},{"ntdll.dll","NtQueryMutant",5,0,0,0xa},{"ntdll.dll","NtSignalAndWaitForSingleObject",4,0,0,0x4},
        {"ntdll.dll","NtCreateSection",7,0,0,0x32},{"ntdll.dll","NtOpenSection",3,0,0,0x2},{"ntdll.dll","NtMapViewOfSection",10,0,0,0x380},
        {"ntdll.dll","RtlCreateHeap",6,0,0,0x1},{"ntdll.dll","RtlDestroyHeap",1},
        {"ntdll.dll","RtlRunOnceExecuteOnce",4},
        // MSVC ABI 包裝沿用此處的非同步服務轉接，底層同步物件仍由 Win32 共用流程持有。
        {"msvcp140.dll","_Mtx_init_in_situ",2,0,0,0,true},
        {"msvcp140.dll","_Mtx_destroy_in_situ",1,0,0,0,true},
        {"msvcp140.dll","_Mtx_lock",1,0,0,0,true},
        {"msvcp140.dll","_Mtx_unlock",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_init_in_situ",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_destroy_in_situ",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_wait",2,0,0,0,true},
        {"msvcp140.dll","_Cnd_signal",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_broadcast",1,0,0,0,true},
        {"msvcp140.dll","_Thrd_yield",0,0,0,0,true},
        {"msvcp140.dll","??0_Lockit@std@@QEAA@H@Z",2},
        {"msvcp140.dll","??0_Lockit@std@@QEAA@XZ",1},
        {"msvcp140.dll","??1_Lockit@std@@QEAA@XZ",1},
        {"msvcp140.dll","??0_Lockit@std@@QAE@H@Z",1},
        {"msvcp140.dll","??0_Lockit@std@@QAE@XZ",0},
        {"msvcp140.dll","??1_Lockit@std@@QAE@XZ",0},
        {"internal-msvcp","GlobalLocksInit",3},
        {"msvcp140.dll","?_Execute_once@std@@YAHAEAUonce_flag@1@P6AHPEAX1PEAPEAX@Z1@Z",3,0,0,0,true},
        {"msvcp140.dll","?_Execute_once@std@@YAHAAUonce_flag@1@P6GHPAX1PAPAX@Z1@Z",3,0,0,0,true},
        {"ntdll.dll","RtlAnsiStringToUnicodeString",3,0,0,0x4},{"ntdll.dll","RtlCompareUnicodeString",3,0,0,0x4},{"ntdll.dll","RtlEqualUnicodeString",3,0,0,0x4},
        {"ntdll.dll","RtlFreeUnicodeString",1},{"ntdll.dll","RtlGetLastNtStatus",0}
        ,{"ntdll.dll","LdrLockLoaderLock",3,0,0,1},{"ntdll.dll","LdrUnlockLoaderLock",2,0,0,1}
        ,{"ntdll.dll","LdrLoadDll",4},{"ntdll.dll","LdrGetDllHandle",4},{"ntdll.dll","LdrGetProcedureAddress",4,0,0,0x4},{"ntdll.dll","LdrUnloadDll",1}
        ,{"ntdll.dll","RtlAreLongPathsEnabled",0}
    };return table;
}
std::map<std::uint64_t,std::uint32_t> WindowsServices::builtin_system_calls() const {
    std::map<std::uint64_t,std::uint32_t> result;const auto table=runtime_service_descriptors();
    for(std::size_t i=0;i<table.size();++i)if(table[i].family=="ntdll.dll" && (table[i].symbol.starts_with("Nt") || table[i].symbol.starts_with("Zw"))){
        const auto target=resolve("ntdll.dll",table[i].symbol).value;
        result.try_emplace(target,static_cast<std::uint32_t>((target-stub_base_.value)/16+1));
    }
    return result;
}
std::optional<std::uint32_t> WindowsServices::finish_system_call(GuestState& state,GuestMemory& memory){
    auto& frames=threads_.items.at(threads_.current).system_calls;if(frames.empty())throw HostCallFault("NT 系統呼叫返回缺少框架");
    const auto& frame=frames.back();if(state.reg(Gpr::Rsp)!=frame.expected_stack)throw HostCallFault("NT 系統呼叫返回堆疊不符");
    if(x86_)memory.write(GuestAddress{frame.stack},frame.saved_memory);else memory.store(GuestAddress{frame.stack},64,frame.caller);
    state.reg(Gpr::Rsp)=frame.resume_stack;state.reg(Gpr::Rcx)=frame.resume_rcx;state.reg(Gpr::Rdx)=frame.resume_rdx;
    state.reg(Gpr::R11)=frame.resume_r11;state.rflags=frame.flags;state.rip=GuestAddress{frame.resume};frames.pop_back();return std::nullopt;
}
bool WindowsServices::dispatch_system_call(GuestState& state,GuestMemory& memory,SystemCallEntry entry){
    if((entry==SystemCallEntry::Syscall && x86_) || (entry==SystemCallEntry::Sysenter && !x86_))return false;
    GuestThread::SystemCallFrame frame;frame.resume=state.rip.value;frame.flags=state.rflags;
    frame.resume_stack=state.reg(Gpr::Rsp);frame.resume_rcx=state.reg(Gpr::Rcx);
    frame.resume_rdx=state.reg(Gpr::Rdx);frame.resume_r11=state.reg(Gpr::R11);
    if(entry==SystemCallEntry::Sysenter){
        frame.resume=callback_return_.value+0xf0;frame.resume_stack=static_cast<std::uint32_t>(state.reg(Gpr::Rdx));
        frame.resume_rcx=frame.resume_stack;frame.resume_rdx=frame.resume;
    }
    const auto fail=[&](std::uint32_t status){
        state.reg(Gpr::Rax)=status;state.rip=GuestAddress{frame.resume};state.reg(Gpr::Rsp)=frame.resume_stack;
        state.reg(Gpr::Rcx)=frame.resume_rcx;state.reg(Gpr::Rdx)=frame.resume_rdx;return true;
    };
    const auto id=static_cast<std::uint32_t>(state.reg(Gpr::Rax));const auto index=id?static_cast<std::size_t>(id-1):SIZE_MAX;
    const auto target=stub_base_.value+index*16;const auto calls=builtin_system_calls();
    if(!id || !calls.contains(target))return fail(0xc000001cU);
    auto& frames=threads_.items.at(threads_.current).system_calls;
    if(x86_){
        const WindowsServiceDescriptor* descriptor=nullptr;
        for(const auto& candidate:runtime_service_descriptors())
            if(candidate.family=="ntdll.dll" && resolve(candidate.family,candidate.symbol).value==target){descriptor=&candidate;break;}
        if(!descriptor)throw HostCallFault("NT 系統呼叫描述元缺失");
        const auto slots=descriptor->arguments+static_cast<unsigned>(std::popcount(descriptor->wide_arguments))+
            3U*static_cast<unsigned>(std::popcount(descriptor->aggregate_arguments));
        const auto size=(std::uint64_t{slots}+1)*4;
        // SYSENTER 不保存／使用原 ESP；Windows 由 EDX 提供使用者返回框架。
        const auto sp=static_cast<std::uint32_t>(frame.resume_stack);
        const auto arguments=std::uint64_t{static_cast<std::uint32_t>(state.reg(Gpr::Rdx))}+(entry==SystemCallEntry::Sysenter?8U:0U);
        if(sp<size || arguments>UINT64_C(0x100000000)-slots*4U)return fail(0xc0000005U);
        frame.stack=(sp-size)&~UINT64_C(15);frame.expected_stack=frame.stack+4+(descriptor->caller_cleanup?0:slots*4U);
        try{
            std::vector<std::uint8_t> values(slots*4U);memory.read(GuestAddress{arguments},values);
            frame.saved_memory.resize(size);memory.read(GuestAddress{frame.stack},frame.saved_memory);
            memory.check(GuestAddress{frame.stack},size,Access::Write);
            // 一次複製參數，回呼與等待沿用既有 NT API；返回時恢復暫借的使用者堆疊。
            std::vector<std::uint8_t> outgoing(size);const auto callback=callback_return_.value+0xe0;
            for(unsigned n=0;n<4;++n)outgoing[n]=static_cast<std::uint8_t>(callback>>(n*8));
            std::copy(values.begin(),values.end(),outgoing.begin()+4);
            memory.write(GuestAddress{frame.stack},outgoing);
        }catch(const MemoryFault&){return fail(0xc0000005U);}
        frames.push_back(std::move(frame));
        state.reg(Gpr::Rsp)=frames.back().stack;
    }else{
        frame.stack=state.reg(Gpr::Rsp);frame.expected_stack=frame.stack+8;
        memory.check(GuestAddress{frame.stack},8,Access::Write);frame.caller=memory.load(GuestAddress{frame.stack},64);
        frames.push_back(frame);memory.store(GuestAddress{frame.stack},64,callback_return_.value+0xe0);
        state.reg(Gpr::Rcx)=state.reg(Gpr::R10);
    }
    state.rip=GuestAddress{target};return true;
}
bool WindowsServices::software_interrupt_enabled(std::uint8_t vector) const noexcept {
    return vector==3 || vector==4 || (vector>=0x29 && vector<=0x2e);
}
std::optional<ExecutionResult> WindowsServices::dispatch_software_interrupt(GuestState& state,GuestMemory& memory,std::uint8_t vector,unsigned length){
    if(vector==0x2e){if(dispatch_system_call(state,memory,SystemCallEntry::Interrupt))return std::nullopt;}
    if(vector==0x29){
        // Fast-fail 是第二次機會、不可繼續的程序失敗，不能進入一般 SEH／VEH。
        state.rip.value=(state.rip.value-length)&(x86_?UINT32_MAX:UINT64_MAX);
        (void)emit_debug_event(state,memory,debugger::Exception{0xc0000409U,1,0,state.rip.value,{static_cast<std::uint32_t>(state.reg(Gpr::Rcx))},false});
        record_job_exit(0xc0000409U);(void)emit_debug_event(state,memory,debugger::ExitProcess{0xc0000409U});
        ExecutionResult result;result.reason=StopReason::Exited;result.exit_code=0xc0000409U;result.rip=state.rip;return result;
    }
    if(vector==0x2c){
        state.rip.value=(state.rip.value-length)&(x86_?UINT32_MAX:UINT64_MAX);
        if(dispatch_cpu_exception(state,memory,0xc0000420U))return std::nullopt;
        ExecutionResult result;result.reason=StopReason::ArithmeticFault;result.rip=state.rip;result.message="未處理的 Windows assertion trap";return result;
    }
    return HostServices::dispatch_software_interrupt(state,memory,vector,length);
}
std::optional<std::uint32_t> WindowsServices::invoke_nt_native(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto name=service.symbol;const auto saved_error=last_error_;const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint64_t status){if(name.starts_with("Nt"))threads_.items.at(threads_.current).last_native_status=static_cast<std::uint32_t>(status);last_error_=saved_error;call.return_to(state,status);return std::optional<std::uint32_t>{};};
    NativeOperation op;op.symbol=name;op.saved_error=saved_error;for(unsigned i=0;i<service.arguments;++i)op.arguments[i]=call(i);
    const auto start=[&](std::string_view family,std::string_view symbol,std::vector<std::uint64_t> arguments){
        const auto id=next_native_operation_++;native_operations_.emplace(id,op);start_callbacks(state,memory,{{resolve(family,symbol).value,std::move(arguments)}},Continuation::NativeOperation,id);return std::optional<std::uint32_t>{};
    };
    const auto scratch=[&](std::size_t size){op.scratch=allocate_heap(memory,size);if(!op.scratch)throw NativeError{0xc0000017U};memory.write(GuestAddress{op.scratch},std::vector<std::uint8_t>(size));return op.scratch;};
    const auto security=[&](const ObjectAttributes& attr,std::uint64_t at){memory.store(GuestAddress{at},32,x86_?12U:24U);memory.store(GuestAddress{at+unit},bits,attr.security);memory.store(GuestAddress{at+unit*2},32,(attr.flags&2U)!=0);return at;};
    const auto path=[&](ObjectAttributes attr){
        auto value=std::move(attr.name);if(value.find(u'\0')!=value.npos || value.empty())throw NativeError{0xc0000033U};
        if(attr.root){const auto file=files_.find(attr.root);if(file==files_.end())throw NativeError{object_type(attr.root).empty()?0xc0000008U:0xc0000024U};if(!(file->second.metadata.attributes&0x10U))throw NativeError{0xc0000024U};if(value.front()==u'\\' || value.find(u':')!=value.npos)throw NativeError{0xc0000033U};value=utf8_to_utf16(file->second.path)+u"\\"+value;}
        else if(value.starts_with(u"\\??\\"))value.erase(0,4);
        else if(value.starts_with(u"\\DosDevices\\"))value.erase(0,12);
        else {bool matched=false;for(char16_t drive=u'A';drive<=u'Z';++drive){const auto device=windows_device_name(drive);if(value.starts_with(device+u"\\")){value=std::u16string{drive,u':'}+value.substr(device.size());matched=true;break;}}if(!matched && (value.size()<3 || value[1]!=u':'))throw NativeError{0xc000003bU};}
        return value;
    };
    try{
#include "msvcp_sync_handlers.inc"
        if(name=="NtQueryMutant"){
            // MUTANT_BASIC_INFORMATION：CurrentCount=1-遞迴次數、OwnedByCaller、AbandonedState；MUTANT_OWNER_INFORMATION：擁有者 CLIENT_ID。
            const auto handle=runtime_.wait_objects.find(call(0));
            if(handle==runtime_.wait_objects.end())return finish(object_type(call(0)).empty()?0xc0000008U:0xc0000024U);
            if(handle->second.kind!=WaitObject::Kind::Mutex)return finish(0xc0000024U);
            if(!(handle->second.access&1U))return finish(0xc0000022U);
            const auto info=static_cast<std::uint32_t>(call(1)),length=static_cast<std::uint32_t>(call(3));
            if(info>1)return finish(0xc0000003U);const auto required=info?2U*unit:8U;if(length!=required)return finish(0xc0000004U);
            const auto& mutex=runtime_.mutex_objects.at(handle->second.mutex);
            std::uint64_t process=0,thread=0;std::uint32_t recursion=0;bool abandoned=mutex.abandoned,caller=false;
            if(mutex.shared){SharedEventLock lock(mutex.shared->lock_descriptor());const auto value=mutex.shared->state_locked();
                // 其他程序持有時只能取得該程序的執行緒鍵，與本程序公開 TID 的轉換無法跨程序重建。
                const bool local=mutex.shared->owned();process=value.process;recursion=value.recursion;abandoned=value.abandoned;caller=local && value.thread==threads_.current;thread=local?thread_identifier(value.thread):static_cast<std::uint32_t>(value.thread);}
            else if(mutex.recursion){process=process_id();recursion=mutex.recursion;caller=mutex.owner==threads_.current;thread=thread_identifier(mutex.owner);}
            memory.check(GuestAddress{call(2)},required,Access::Write);
            if(!info){memory.store(GuestAddress{call(2)},32,static_cast<std::uint32_t>(1-static_cast<std::int64_t>(recursion)));memory.store(GuestAddress{call(2)+4},8,caller);memory.store(GuestAddress{call(2)+5},8,abandoned);memory.store(GuestAddress{call(2)+6},16,0);}
            else {memory.store(GuestAddress{call(2)},bits,recursion?process:0);memory.store(GuestAddress{call(2)+unit},bits,recursion?thread:0);}
            if(call(4))memory.store(GuestAddress{call(4)},32,required);
            return finish(0);
        }
        if(name=="RtlRunOnceExecuteOnce"){
            memory.check(GuestAddress{call(0)},unit,Access::Write);
            if(call(3))memory.check(GuestAddress{call(3)},unit,Access::Write);
            // 使用相同 INIT_ONCE 狀態與回呼／排程流程，避免 NT 與 Win32 分裂。
            last_error_=0;
            return start("kernel32.dll","InitOnceExecuteOnce",{call(0),call(1),call(2),call(3)});
        }
        if(name=="RtlAreLongPathsEnabled")return finish(0); // 預設登錄 LongPathsEnabled=0。
        if(name=="LdrGetDllHandle"){
            // (PathToFile, Flags, ModuleFileName, *ModuleHandle)：只查詢已載入模組，不載入。
            if(!call(2) || !call(3))return finish(0xc000000dU);
            const auto handle=find_module(native_string(memory,call(2),x86_));
            if(!handle)return finish(0xc0000135U);
            memory.check(GuestAddress{call(3)},unit,Access::Write);memory.store(GuestAddress{call(3)},bits,handle);return finish(0);
        }
        if(name=="LdrLoadDll"){
            // (PathToFile, Flags, ModuleFileName, *ModuleHandle)：以 LoadLibraryExW 流程載入（含 DllMain 排程），完成後寫回 handle。
            if(!call(2) || !call(3))return finish(0xc000000dU);
            const auto module_name=native_string(memory,call(2),x86_);if(module_name.empty())return finish(0xc000000dU);
            memory.check(GuestAddress{call(3)},unit,Access::Write);
            const auto encoded=utf16_bytes(module_name);const auto buffer=scratch(encoded.size()+2);memory.write(GuestAddress{buffer},encoded);
            return start("kernel32.dll","LoadLibraryExW",{buffer,0,0});
        }
        if(name=="LdrGetProcedureAddress"){
            // (ModuleHandle, PANSI_STRING Name, Ordinal, *Address)：名稱優先，否則依序數。
            if(!call(0) || !call(3))return finish(0xc000000dU);
            memory.check(GuestAddress{call(3)},unit,Access::Write);
            std::uint64_t lookup=call(2)&0xffffU;
            if(call(1)){
                const auto length=memory.load(GuestAddress{call(1)},16),source=memory.load(GuestAddress{call(1)+unit},bits);
                if(length && !source)return finish(0xc000000dU);
                std::vector<std::uint8_t> bytes;for(std::uint64_t n=0;n<length;++n)bytes.push_back(static_cast<std::uint8_t>(memory.load(GuestAddress{source+n},8)));
                lookup=scratch(bytes.size()+1);memory.write(GuestAddress{lookup},bytes);
            }
            return start("kernel32.dll","GetProcAddress",{call(0),lookup});
        }
        if(name=="LdrUnloadDll"){if(!call(0))return finish(0xc000000dU);return start("kernel32.dll","FreeLibrary",{call(0)});}
        if(name=="LdrLockLoaderLock" || name=="LdrUnlockLoaderLock"){
            const auto flags=static_cast<std::uint32_t>(call(0));
            const auto loader_error=[&](std::uint32_t status){
                if(flags&1U){auto caller=state;call.return_to(caller,status);begin_exception(caller,memory,status,0,{});state=caller;return std::optional<std::uint32_t>{};}
                return finish(status);
            };
            if(name=="LdrUnlockLoaderLock"){
                if(flags&~1U)return loader_error(0xc00000efU);
                if(!call(1))return finish(0);
                const auto cookie=runtime_.loader_lock_cookies.find(call(1));
                if(cookie==runtime_.loader_lock_cookies.end() || cookie->second!=threads_.current || runtime_.loader_owner!=threads_.current || !runtime_.loader_depth)return loader_error(0xc00000f0U);
                runtime_.loader_lock_cookies.erase(cookie);release_loader_notification();
                publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);return finish(0);
            }
            if(call(1))memory.store(GuestAddress{call(1)},32,0);if(call(2))memory.store(GuestAddress{call(2)},bits,0);
            if(flags&~3U)return loader_error(0xc00000efU);
            if((flags&2U) && !call(1))return loader_error(0xc00000f0U);
            if(!call(2))return loader_error(0xc00000f1U);
            if(runtime_.loader_owner && runtime_.loader_owner!=threads_.current){
                if(flags&2U){memory.store(GuestAddress{call(1)},32,2);return finish(0);}
                (void)await_loader(state,memory);return std::nullopt;
            }
            if(runtime_.loader_depth==UINT32_MAX || runtime_.loader_lock_cookies.size()>=8192)return loader_error(0xc0000017U);
            auto cookie=runtime_.next_loader_lock_cookie++;while(!cookie || runtime_.loader_lock_cookies.contains(cookie))cookie=runtime_.next_loader_lock_cookie++;
            runtime_.loader_lock_cookies.emplace(cookie,threads_.current);runtime_.loader_owner=threads_.current;++runtime_.loader_depth;
            publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);
            if(call(1))memory.store(GuestAddress{call(1)},32,1);memory.store(GuestAddress{call(2)},bits,cookie);return finish(0);
        }
        if(name=="RtlGetLastNtStatus")return finish(threads_.items.at(threads_.current).last_native_status);
        if(name=="RtlCompareUnicodeString" || name=="RtlEqualUnicodeString"){
            auto a=native_string(memory,call(0),x86_),b=native_string(memory,call(1),x86_);if(call(2))for(auto* value:{&a,&b})for(auto& c:*value)c=unicode_case_unit(c,true);
            const auto count=std::min(a.size(),b.size());std::int32_t result=0;for(std::size_t i=0;i<count;++i)if(a[i]!=b[i]){result=static_cast<std::int32_t>(a[i])-b[i];break;}if(!result)result=static_cast<std::int32_t>(a.size()*2)-static_cast<std::int32_t>(b.size()*2);
            return finish(name=="RtlEqualUnicodeString"?result==0:static_cast<std::uint32_t>(result));
        }
        if(name=="RtlAnsiStringToUnicodeString"){
            const auto length=memory.load(GuestAddress{call(1)},16),maximum=memory.load(GuestAddress{call(1)+2},16),source=memory.load(GuestAddress{call(1)+unit},bits);if(length>maximum || (length && !source))return finish(0xc000000dU);
            std::string bytes;for(std::uint64_t n=0;n<length;++n)bytes.push_back(static_cast<char>(memory.load(GuestAddress{source+n},8)));const auto converted=utf8_to_utf16(bytes);if(converted.size()>32766)return finish(0xc000000dU);
            const auto encoded=utf16_bytes(converted);memory.check(GuestAddress{call(0)},unit*2,Access::Write);auto destination=memory.load(GuestAddress{call(0)+unit},bits);
            if(call(2)){destination=allocate_heap(memory,encoded.size());if(!destination)return finish(0xc0000017U);memory.store(GuestAddress{call(0)+2},16,encoded.size());memory.store(GuestAddress{call(0)+unit},bits,destination);}
            else if(memory.load(GuestAddress{call(0)+2},16)<encoded.size())return finish(0x80000005U);
            memory.write(GuestAddress{destination},encoded);memory.store(GuestAddress{call(0)},16,converted.size()*2);return finish(0);
        }
        if(name=="RtlFreeUnicodeString"){const auto buffer=memory.load(GuestAddress{call(0)+unit},bits);if(buffer)(void)free_heap(memory,buffer,state);memory.write(GuestAddress{call(0)},std::vector<std::uint8_t>(unit*2));return finish(0);}
        if(name=="RtlCreateHeap"){if(call(1) || call(4) || call(5) || (call(0)&~1U))return finish(0);return start("kernel32.dll","HeapCreate",{call(0),call(3),call(2)});}
        if(name=="RtlDestroyHeap")return start("kernel32.dll","HeapDestroy",{call(0)});
        if(name=="NtMapViewOfSection")return invoke_nt_section(state,memory,service);
        if(name=="NtQueryAttributesFile" || name=="NtQueryFullAttributesFile"){
            const auto file=path(object_attributes(memory,call(0),x86_));const auto info=filesystem_.information(utf16_to_utf8(file));if(info.error)return finish(nt_status(info.error));
            const auto error=filesystem_.security_access(utf16_to_utf8(file),0x80U);if(error)return finish(nt_status(error));
            const auto& m=info.metadata;const bool full=name=="NtQueryFullAttributesFile";memory.write(GuestAddress{call(1)},std::vector<std::uint8_t>(full?56U:40U));
            for(const auto& [offset,value]:std::array<std::pair<unsigned,std::uint64_t>,4>{{{0,m.created},{8,m.accessed},{16,m.modified},{24,m.changed}}})memory.store(GuestAddress{call(1)+offset},64,value);
            if(full){memory.store(GuestAddress{call(1)+32},64,m.allocation_size);memory.store(GuestAddress{call(1)+40},64,m.size);}memory.store(GuestAddress{call(1)+(full?48:32)},32,m.attributes);return finish(0);
        }
        if(name=="NtCreateFile" || name=="NtOpenFile"){
            const bool create=name=="NtCreateFile";const auto attr=object_attributes(memory,call(2),x86_);const auto file=path(attr);const auto options=static_cast<std::uint32_t>(call(create?8:5)),disposition=create?static_cast<std::uint32_t>(call(7)):1U;
            memory.check(GuestAddress{call(0)},unit,Access::Write);memory.check(GuestAddress{call(3)},unit*2,Access::Write);
            if(disposition>5 || (options&~0x0020507fU) || (options&0x30U)==0x30U || ((options&0x30U) && !(call(1)&0x100000U)) || ((options&1U) && (options&0x40U)))return finish(0xc000000dU);
            if(create && (call(9) || call(10)))return finish(0xc00000bbU);
            const auto info=filesystem_.information(utf16_to_utf8(file));const bool existed=!info.error;if(info.error && info.error!=2 && info.error!=3)return finish(nt_status(info.error));
            if(existed && (options&0x40U) && (info.metadata.attributes&0x10U))return finish(0xc00000baU);
            if(existed && (options&1U) && !(info.metadata.attributes&0x10U))return finish(0xc0000103U);
            auto flags=create?static_cast<std::uint32_t>(call(5)):0x80U;if(!flags)flags=0x80U;
            if(options&2U)flags|=0x80000000U;if(options&4U)flags|=0x08000000U;if(options&8U)flags|=0x20000000U;
            if(!(options&0x30U))flags|=0x40000000U;if(options&0x1000U)flags|=0x04000000U;if(options&0x4000U)flags|=0x02000000U;if(options&0x200000U)flags|=0x00200000U;
            const auto at=scratch(128+file.size()*2+2);memory.write(GuestAddress{at+128},utf16_bytes(file));security(attr,at);
            unsigned win_disposition=disposition==0 || disposition==5?2U:disposition==1?3U:disposition==2?1U:disposition==3?4U:5U;
            if(options&1U){if(disposition!=1 && disposition!=2 && disposition!=3)throw NativeError{0xc000000dU};if(existed && disposition==2)throw NativeError{0xc0000035U};if(!existed && disposition==1)throw NativeError{0xc0000034U};if(!existed){const auto error=filesystem_.create_directory(utf16_to_utf8(file),[&]{return file_creation_descriptor(memory,at);});if(error)throw NativeError{nt_status(error)};}flags|=0x02000000U;win_disposition=3;}
            op.information=existed?(disposition==0?0U:disposition==4 || disposition==5?3U:1U):2U;op.attributes=options;
            if(create && call(4) && op.information!=1){op.allocation=memory.load(GuestAddress{call(4)},64);if(op.allocation>INT64_MAX)throw NativeError{0xc000000dU};}
            return start("kernel32.dll","CreateFileW",{at+128,call(1),call(create?6:4),at,win_disposition,flags,0});
        }
        if(name=="NtOpenProcess" || name=="NtOpenThread"){
            memory.check(GuestAddress{call(0)},unit,Access::Write);const auto attr=object_attributes(memory,call(2),x86_);if(!attr.name.empty() || attr.root || attr.security || !call(3))return finish(0xc000000dU);
            const auto pid=memory.load(GuestAddress{call(3)},bits),tid=memory.load(GuestAddress{call(3)+unit},bits);if(pid>UINT32_MAX || tid>UINT32_MAX)return finish(0xc000000bU);
            if(name=="NtOpenThread" && pid && pid!=process_id())return finish(0xc00000bbU);
            return start("kernel32.dll",name=="NtOpenProcess"?"OpenProcess":"OpenThread",{call(1),(attr.flags&2U)!=0,name=="NtOpenProcess"?pid:tid});
        }
        if(name.starts_with("NtOpen") && name.find("Token")!=name.npos){
            const bool process=name.starts_with("NtOpenProcess"),extended=name.ends_with("Ex");const auto attrs=extended?static_cast<std::uint32_t>(call(process?2:3)):0U;
            if(attrs&~2U)return finish(0xc000000dU);const auto output=call(process?(extended?3:2):(extended?4:3));memory.check(GuestAddress{output},unit,Access::Write);op.attributes=attrs;
            return start("advapi32.dll",process?"OpenProcessToken":"OpenThreadToken",process?std::vector<std::uint64_t>{call(0),call(1),output}:std::vector<std::uint64_t>{call(0),call(1),call(2),output});
        }
        if(name=="NtDuplicateObject"){if(call(5)&~2U)return finish(0xc000000dU);if(call(3))memory.check(GuestAddress{call(3)},unit,Access::Write);return start("kernel32.dll","DuplicateHandle",{call(0),call(1),call(2),call(3),call(4),(call(5)&2U)!=0,call(6)});}
        if(name=="NtWaitForSingleObject" || name=="NtSignalAndWaitForSingleObject"){
            const bool signal=name.starts_with("NtSignal");const auto timeout=call(signal?3:2);std::uint64_t ticks=UINT64_MAX;
            if(timeout){const auto value=memory.load(GuestAddress{timeout},64);ticks=value>>63?(~value+1):value>runtime_.system_filetime()?value-runtime_.system_filetime():0;}
            // 共用等待 API 的排程及 APC 行為；保留 NT 的 100 ns 截止時間供排程器使用。
            op.information=ticks==UINT64_MAX?UINT64_MAX:ticks>UINT64_MAX-runtime_.clock_ticks?UINT64_MAX:runtime_.clock_ticks+ticks;
            const auto millis=ticks==UINT64_MAX?UINT32_MAX:std::min<std::uint64_t>(0xfffffffeU,ticks/10000+(ticks%10000!=0));
            return start("kernel32.dll",signal?"SignalObjectAndWait":"WaitForSingleObjectEx",signal?std::vector<std::uint64_t>{call(0),call(1),millis,call(2)}:std::vector<std::uint64_t>{call(0),millis,call(1)});
        }
        if(name=="NtCreateSection" || name=="NtOpenSection"){
            const bool create=name=="NtCreateSection";memory.check(GuestAddress{call(0)},unit,Access::Write);const auto attr=object_attributes(memory,call(2),x86_,create);if(attr.root)return finish(0xc00000bbU);
            auto section=attr.name;if(section.starts_with(u"\\BaseNamedObjects\\"))section.erase(0,18);else if(!section.empty() && section.front()==u'\\')return finish(0xc000003bU);
            if(call(1)&~0xf001fU)return finish(0xc0000022U);
            if(create && call(5)!=0x8000000U && call(5)!=0)return finish(0xc00000bbU);
            const auto at=scratch(128+section.size()*2+2);security(attr,at);if(!section.empty())memory.write(GuestAddress{at+128},utf16_bytes(section));op.attributes=static_cast<std::uint32_t>(call(1));
            if(create){const auto size=call(3)?memory.load(GuestAddress{call(3)},64):0;if(size>INT64_MAX)throw NativeError{0xc000000dU};return start("kernel32.dll","CreateFileMappingW",{call(6)?call(6):(x86_?UINT32_MAX:UINT64_MAX),at,call(4)|0x8000000U,size>>32,static_cast<std::uint32_t>(size),section.empty()?0:at+128});}
            if(section.empty())throw NativeError{0xc0000033U};return start("kernel32.dll","OpenFileMappingW",{call(1),(attr.flags&2U)!=0,at+128});
        }
        if(name=="NtReadFile" || name=="NtWriteFile"){
            memory.check(GuestAddress{call(4)},unit*2,Access::Write);const auto file=files_.find(call(0));if(file==files_.end())return finish(0xc0000008U);
            const bool sync=(file->second.mode&0x30U)!=0,writing=name=="NtWriteFile";if(call(8))return finish(0xc00000bbU);
            if(call(1) && (!runtime_.wait_objects.contains(call(1)) || runtime_.wait_objects.at(call(1)).kind!=WaitObject::Kind::Event))return finish(0xc0000008U);
            if(call(2) && file->second.completion_port)return finish(0xc000000dU);
            auto offset=call(7)?memory.load(GuestAddress{call(7)},64):UINT64_MAX-1;if(offset==UINT64_MAX-1){if(!sync && !file->second.pipe && !file->second.serial && !file->second.hid)return finish(0xc000000dU);offset=file->second.pipe || file->second.serial || file->second.hid?0:static_cast<std::uint64_t>(runtime_.win_file_positions.at(file->second.object));}
            if(offset>INT64_MAX && !(writing && offset==UINT64_MAX))return finish(0xc000000dU);
            const auto at=scratch(128);memory.store(GuestAddress{at+64+unit*2},64,offset);memory.store(GuestAddress{at+64+(x86_?16U:24U)},bits,call(1));
            return start("kernel32.dll",writing?"WriteFile":"ReadFile",{call(0),call(5),call(6),at+32,at+64});
        }
        throw HostCallFault("未知 NT 原生服務");
    }catch(const NativeError& error){if(op.scratch)(void)free_heap(memory,op.scratch,state);return finish(error.status);}
    catch(const MemoryFault&){if(op.scratch)(void)free_heap(memory,op.scratch,state);return finish(0xc0000005U);}
    catch(const std::bad_alloc&){if(op.scratch)(void)free_heap(memory,op.scratch,state);return finish(0xc0000017U);}
}
std::optional<std::uint32_t> WindowsServices::resume_nt_native(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    auto op=native_operations_.at(id);native_operations_.erase(id);const auto& a=op.arguments;const auto name=op.symbol;const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);auto status=nt_status(last_error_);bool keep_scratch=false;
    if(name.find("_Lockit@std@@")!=name.npos){
        if(!op.information){
            if(!value)throw HostCallFault("STL 共用鎖初始化失敗");
            op.information=1;const auto token=next_native_operation_++;native_operations_.emplace(token,op);
            const auto lock=runtime_.msvcp_global_locks+unit+op.attributes*(x86_?24U:40U);
            const bool release=name.starts_with("??1");
            start_callbacks(state,memory,{{resolve("kernel32.dll",release?"LeaveCriticalSection":"EnterCriticalSection").value,{lock}}},Continuation::NativeOperation,token);return std::nullopt;
        }
        last_error_=op.saved_error;complete_call(state,memory,name.starts_with("??0")?a[0]:0);return std::nullopt;
    }
    if(name.starts_with("_Mtx_") || name.starts_with("_Cnd_") || name=="_Thrd_yield" || name.starts_with("?_Execute_once@std@@")){
        if(name=="_Mtx_lock" || name=="_Cnd_wait"){
            const auto address=a[name=="_Cnd_wait"?1:0],owner=address+(x86_?40U:72U);
            memory.store(GuestAddress{owner},32,thread_identifier(threads_.current));
            memory.store(GuestAddress{owner+4},32,memory.load(GuestAddress{owner+4},32)+1);
        }
        // once 的回呼失敗及 LastError 完整沿用 InitOnceExecuteOnce。
        if(!name.starts_with("?_Execute_once@std@@"))last_error_=op.saved_error;
        complete_call(state,memory,name.starts_with("?_Execute_once@std@@")?value:0);
        return std::nullopt;
    }
    if(name=="RtlCreateHeap"){if(op.scratch)(void)free_heap(memory,op.scratch,state);last_error_=op.saved_error;complete_call(state,memory,value);return std::nullopt;}
    if(name=="RtlDestroyHeap"){last_error_=op.saved_error;complete_call(state,memory,value?0:a[0]);return std::nullopt;}
    if(name=="RtlRunOnceExecuteOnce"){
        status=value?0:last_error_?nt_status(last_error_):0xc0000001U;
        last_error_=op.saved_error;complete_call(state,memory,status);return std::nullopt;
    }
    if(name=="NtCreateFile" || name=="NtOpenFile"){
        const bool good=value!=(x86_?UINT32_MAX:UINT64_MAX) && value!=UINT64_MAX;status=good?0:status;
        if(good){if(const auto file=files_.find(value);file!=files_.end()){file->second.mode=op.attributes&0x30U;if(op.allocation && file->second.native)status=nt_status(file->second.native->allocate(op.allocation));}
            if(!status)memory.store(GuestAddress{a[0]},bits,value);else {files_.erase(value);runtime_.win_file_positions.erase(value);runtime_.handle_flags.erase(value);}}
        memory.store(GuestAddress{a[3]},bits,status);memory.store(GuestAddress{a[3]+unit},bits,status?0:op.information);
    }else if(name=="NtOpenProcess" || name=="NtOpenThread"){status=value?0:status;if(value)memory.store(GuestAddress{a[0]},bits,value);}
    else if(name.find("Token")!=name.npos){status=value?0:status;if(value && (op.attributes&2U)){const bool process=name.starts_with("NtOpenProcess"),extended=name.ends_with("Ex");const auto output=a[process?(extended?3:2):(extended?4:3)];runtime_.handle_flags[memory.load(GuestAddress{output},bits)]|=1U;}}
    else if(name=="NtCreateSection" || name=="NtOpenSection"){status=value?native_section_access(value,op.attributes):status;if(!status)memory.store(GuestAddress{a[0]},bits,value);else if(value)(void)close_mapping(value);}
    else if(name=="NtWaitForSingleObject" || name=="NtSignalAndWaitForSingleObject")status=value==UINT32_MAX?status:static_cast<std::uint32_t>(value);
    else if(name=="NtReadFile" || name=="NtWriteFile"){
        status=value?0:status;const auto bytes=memory.load(GuestAddress{op.scratch+64+unit},bits);memory.store(GuestAddress{a[4]},bits,status);memory.store(GuestAddress{a[4]+unit},bits,bytes);
        if(status==0x103U){const auto pending=std::find_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& io){return io.overlapped==op.scratch+64 && !io.done;});
            if(pending==pending_file_io_.end())throw HostCallFault("NT 待命 I/O 缺少共用要求");pending->native_iosb=a[4];pending->native_context=a[3];pending->native_scratch=op.scratch;pending->routine=a[2];keep_scratch=true;
        }else if(a[2] && (!status || status==0xc0000011U || status==0x80000005U)){threads_.items.at(threads_.current).apcs.push_back({a[2],{a[3],a[4],0}});threads_.yield=true;}
    }else if(name=="LdrLoadDll"){status=value?0:(last_error_==126?0xc0000135U:last_error_==193?0xc000007bU:status);if(value)memory.store(GuestAddress{a[3]},bits,value);}
    else if(name=="LdrGetProcedureAddress"){status=value?0:0xc0000139U;if(value)memory.store(GuestAddress{a[3]},bits,value);}
    else if(name=="LdrUnloadDll")status=value?0:0xc0000135U;
    else status=value?0:status;
    if(op.scratch && !keep_scratch)(void)free_heap(memory,op.scratch,state);last_error_=op.saved_error;threads_.items.at(threads_.current).last_native_status=status;complete_call(state,memory,status);return std::nullopt;
}
}
