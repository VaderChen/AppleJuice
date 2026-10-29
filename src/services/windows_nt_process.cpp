#include "x64arm/windows_services.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <cerrno>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace x64arm {
namespace {
template<class Files,class Accepts,class Sockets> bool pending_io(std::uint64_t thread,const Files& files,const Accepts& accepts,const Sockets& sockets){
    const auto pending=[&](const auto& items){return std::any_of(items.begin(),items.end(),[&](const auto& item){return item.thread==thread && !item.done;});};
    return pending(files) || pending(accepts) || pending(sockets);
}
std::uint32_t native_status(std::uint32_t error){
    switch(error){case 0:return 0;case 5:return 0xc0000022U;case 6:return 0xc0000008U;case 8:return 0xc0000017U;case 50:return 0xc00000bbU;case 998:return 0xc0000005U;case 121:return 0x102U;default:return 0xc000000dU;}
}
}
std::span<const WindowsServiceDescriptor> nt_process_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","NtQueryInformationProcess",5,0,0,0xa}, {"ntdll.dll","ZwQueryInformationProcess",5,0,0,0xa},
        {"ntdll.dll","NtWow64QueryInformationProcess64",5,0,0,0xa}, {"ntdll.dll","ZwWow64QueryInformationProcess64",5,0,0,0xa},
        {"ntdll.dll","NtSuspendProcess",1}, {"ntdll.dll","ZwSuspendProcess",1},
        {"ntdll.dll","NtResumeProcess",1}, {"ntdll.dll","ZwResumeProcess",1},
        {"ntdll.dll","NtQueryInformationThread",5,0,0,0xa}, {"ntdll.dll","ZwQueryInformationThread",5,0,0,0xa},
        {"ntdll.dll","NtSetInformationThread",4,0,0,0xa}, {"ntdll.dll","ZwSetInformationThread",4,0,0,0xa}
    };return table;
}
std::uint32_t WindowsServices::local_process_information(std::uint32_t operation,std::array<std::uint64_t,5>& values,std::uint32_t& bits,int descriptor){
    bits=x86_?32U:64U;
    if(operation==10 || operation==11){
        // 與 SuspendThread 共用逐執行緒計數；不以程序布林值抹去 CREATE_SUSPENDED 或巢狀暫停。
        // 程序操作逐一處理存活執行緒；已終止或計數達上限者不再增加。
        bool live=false;
        for(auto& [id,thread]:threads_.items){
            (void)id;if(thread.status==ThreadStatus::Exited || thread.terminating)continue;
            live=true;
            if(operation==10){if(thread.suspended<127)++thread.suspended;}
            else if(thread.suspended)--thread.suspended;
        }
        if(!live)return 87;
        threads_.remaining=0;threads_.yield=true;notify_concurrency_changed();return 0;
    }
    const auto base=runtime_.process_background || runtime_.process_priority_class==0x40?4:
        runtime_.process_priority_class==0x4000?6:runtime_.process_priority_class==0x8000?10:runtime_.process_priority_class==0x80?13:8;
    if(operation==0){
        values={259,peb_base_.value,runtime_.process_affinity,static_cast<std::uint64_t>(base),process_parent_id()};return 0;
    }
    if(operation==6 || operation==8){
#if !defined(_WIN32)
        auto path=runtime_.image.path;
        if(operation==6){if(path.size()<3 || path[1]!=u':')return 50;path=windows_device_name(path[0])+path.substr(2);}
        else {const auto slash=path.find_last_of(u"\\/");if(slash!=path.npos)path.erase(0,slash+1);path.resize(std::min<std::size_t>(259,path.size()));}
        const auto bytes=utf16_bytes(path);
        if(bytes.size()>65534)return 8;
        std::size_t done=0;while(done<bytes.size()){const auto count=::pwrite(descriptor,bytes.data()+done,bytes.size()-done,static_cast<off_t>(done));if(count<0 && errno==EINTR)continue;if(count<=0)return 8;done+=static_cast<std::size_t>(count);}
        values[0]=bytes.size();if(operation==8){values[1]=process_parent_id();values[2]=active_thread_count();values[3]=static_cast<std::uint64_t>(base);}return 0;
#else
        (void)descriptor;return 50;
#endif
    }
    if(operation==7){
#if !defined(_WIN32)
        const auto handles=local_process_handles();
        if(handles.size()>65534/4)return 8;
        std::vector<std::uint32_t> items;items.reserve(handles.size());
        for(const auto handle:handles){if(handle>UINT32_MAX)return 50;items.push_back(static_cast<std::uint32_t>(handle));}
        const auto size=items.size()*4;std::size_t done=0;
        while(done<size){const auto count=::pwrite(descriptor,reinterpret_cast<const std::uint8_t*>(items.data())+done,size-done,static_cast<off_t>(done));if(count<0 && errno==EINTR)continue;if(count<=0)return 8;done+=static_cast<std::size_t>(count);}
        values[0]=size;return 0;
#else
        (void)descriptor;return 50;
#endif
    }
    if(operation==12){
#if !defined(_WIN32)
        const auto threads=local_thread_snapshot();if(threads.size()>4096)return 8;
        std::vector<std::uint8_t> bytes;bytes.reserve(threads.size()*12);
        for(const auto& thread:threads)for(const auto value:{thread.id,thread.owner,thread.priority})
            for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>(value>>shift));
        std::size_t done=0;while(done<bytes.size()){
            const auto count=::pwrite(descriptor,bytes.data()+done,bytes.size()-done,static_cast<off_t>(done));
            if(count<0 && errno==EINTR)continue;if(count<=0)return 8;done+=static_cast<std::size_t>(count);
        }
        values[0]=bytes.size();return 0;
#else
        (void)descriptor;return 50;
#endif
    }
    const auto id=values[4]?values[4]:1U;
    if(id>UINT32_MAX)return 87;
    const auto found=threads_.items.find(id);if(found==threads_.items.end())return 6;auto& thread=found->second;
    const auto priority=thread.priority==-15?1:thread.priority==15?15:std::clamp(base+thread.priority,1,15);
    if(operation==1){values={thread.status==ThreadStatus::Exited?thread.exit_code:259U,thread.teb,thread.affinity,static_cast<std::uint64_t>(std::min(priority+static_cast<int>(thread.dynamic_priority_boost),15)),static_cast<std::uint64_t>(priority)};return 0;}
    if(operation==2){values[0]=thread.entry;return 0;}
    if(operation==3){values[0]=pending_io(id,pending_file_io_,runtime_.network.pending_accepts,runtime_.network.pending_socket_io);return 0;}
    if(operation==4){values[0]=thread.hidden_from_debugger?1U:0U;return 0;}
    if(operation==5){if(thread.status==ThreadStatus::Exited)return 87;thread.hidden_from_debugger=true;return 0;}
    return 87;
}
std::optional<std::uint32_t> WindowsServices::invoke_nt_process(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto saved_error=last_error_;
    const auto finish=[&](std::uint32_t status){last_error_=saved_error;call.return_to(state,status);return std::optional<std::uint32_t>{};};
    try{
        const bool suspending=service.symbol=="NtSuspendProcess" || service.symbol=="ZwSuspendProcess";
        if(suspending || service.symbol=="NtResumeProcess" || service.symbol=="ZwResumeProcess"){
            std::uint32_t pid{},bits{};std::uint64_t birth{};std::array<std::uint64_t,5> values{};
            auto error=process_memory_target(memory,call(0),0x800U,pid,birth);
            if(error==6 && !object_type(call(0)).empty())return finish(0xc0000024U);
            const auto operation=suspending?10U:11U;
            if(!error)error=pid==process_id()?local_process_information(operation,values,bits,-1):
                remote_process_request(memory,pid,birth,14,operation,values,bits);
            // 只透過有出生身分核對的客體 IPC；宿主程序持續處理恢復請求。
            const auto status=error==87?0xc000010aU:error==121?0xc00000b5U:native_status(error);
            auto result=finish(status);
            if(!error && suspending && pid==process_id())schedule(state,memory);
            return result;
        }
        const bool query64=service.symbol.ends_with("Wow64QueryInformationProcess64");
        const auto cls=static_cast<std::uint32_t>(call(1));const bool process=query64 || service.symbol.ends_with("Process"),setting=service.symbol.find("SetInformation")!=service.symbol.npos;
        const bool narrow=x86_ && !query64;const auto unit=narrow?4U:8U;
        const auto length=static_cast<std::uint32_t>(call(3));const auto output=call(2);
        if(!setting && call(4))memory.check(GuestAddress{call(4)},4,Access::Write);
        if(query64 && cls!=0 && cls!=20 && cls!=26 && cls!=27 && cls!=58)return finish(0xc0000003U);
        if(query64 && cls==0 && length<48){if(call(4))memory.store(GuestAddress{call(4)},32,48);return finish(0xc0000004U);}
        std::array<std::uint64_t,5> values{};std::uint32_t pid{},bits{};std::uint64_t tid{},birth{};std::uint32_t error=0;
        if(process){error=process_basic_information(memory,call(0),values,pid,bits);if(error)return finish(error==6 && !object_type(call(0)).empty()?0xc0000024U:native_status(error));}
        else {
            tid=thread_id(call(0));
            if(tid){
                if(setting?!(thread_access(call(0))&0x20U):!(thread_access(call(0))&0x840U))return finish(0xc0000022U);
                auto& thread=threads_.items.at(tid);
                const auto base=runtime_.process_background || runtime_.process_priority_class==0x40?4:runtime_.process_priority_class==0x4000?6:runtime_.process_priority_class==0x8000?10:runtime_.process_priority_class==0x80?13:8;
                const auto priority=thread.priority==-15?1:thread.priority==15?15:std::clamp(base+thread.priority,1,15);
                values={thread.status==ThreadStatus::Exited?thread.exit_code:259U,thread.teb,thread.affinity,static_cast<std::uint64_t>(std::min(priority+static_cast<int>(thread.dynamic_priority_boost),15)),static_cast<std::uint64_t>(priority)};pid=process_id();bits=x86_?32U:64U;
            }else {
                std::uint32_t rights{},internal{};
                error=process_thread_target(call(0),pid,rights,&birth,&internal);
                if(error)return finish(error==6 && !object_type(call(0)).empty()?0xc0000024U:native_status(error));
                if(setting?!(rights&0x20U):!(rights&0x840U))return finish(0xc0000022U);
                tid=internal;
            }
            if(setting){
                if(cls!=17)return finish(0xc0000003U);
                if(length)return finish(0xc0000004U);
                if(pid==process_id()){auto& thread=threads_.items.at(tid);if(thread.status==ThreadStatus::Exited)return finish(0xc000004bU);thread.hidden_from_debugger=true;}
                else {values[4]=tid;error=remote_process_request(memory,pid,birth,14,5,values,bits);if(error)return finish(native_status(error));}
                return finish(0);
            }
            if(cls!=0 && cls!=9 && cls!=16 && cls!=17)return finish(0xc0000003U);
            if(pid!=process_id()){values[4]=tid;error=remote_process_request(memory,pid,birth,14,cls==0?1U:cls==9?2U:cls==16?3U:4U,values,bits);if(error)return finish(native_status(error));}
            else if(cls==9)values[0]=threads_.items.at(tid).entry;
            else if(cls==16)values[0]=pending_io(tid,pending_file_io_,runtime_.network.pending_accepts,runtime_.network.pending_socket_io);
            else if(cls==17)values[0]=threads_.items.at(tid).hidden_from_debugger?1U:0U;
        }
        std::vector<std::uint8_t> bytes;
        const auto put=[&](unsigned offset,unsigned width,std::uint64_t value){for(unsigned n=0;n<width;++n)bytes.at(offset+n)=static_cast<std::uint8_t>(value>>(n*8));};
        if(cls==0){
            // 32 位元程序目前沒有原生 64 位元 PEB；不得把 PEB32 位址包成 PEB64。
            if(query64 && bits==32)return finish(0xc00000bbU);
            bytes.resize(process?unit*6U:narrow?28U:48U);put(0,4,values[0]);
            if(narrow && (values[1]>UINT32_MAX || values[2]>UINT32_MAX))return finish(0xc00000bbU);
            put(unit,unit,values[1]);
            if(process){put(unit*2,unit,values[2]);put(unit*3,4,values[3]);put(unit*4,unit,pid);put(unit*5,unit,values[4]);}
            else {put(unit*2,unit,pid);put(unit*3,unit,pid==process_id()?thread_identifier(tid):remote_thread_identifier(pid,birth,static_cast<std::uint32_t>(tid)));put(unit*4,unit,values[2]);put(unit*5,4,values[3]);put(unit*5+4,4,values[4]);}
        }else if(process && (cls==7 || cls==26)){
            bytes.resize(unit);put(0,unit,cls==26 && bits==32?values[1]:0);
        }else if(process && cls==20){
            std::uint32_t count{};error=query_process_handle_count(memory,pid,count);if(error)return finish(native_status(error));bytes.resize(4);put(0,4,count);
        }else if(process && cls==58){
            std::uint32_t target_pid{};error=process_memory_target(memory,call(0),0x400U,target_pid,birth);if(error)return finish(native_status(error));
            if(pid==process_id()){
                const auto handles=local_process_handles();bytes.resize(handles.size()*4);
                for(std::size_t n=0;n<handles.size();++n){if(handles[n]>UINT32_MAX)return finish(0xc00000bbU);put(static_cast<unsigned>(n*4),4,handles[n]);}
            }else{
#if !defined(_WIN32)
                auto name=(std::filesystem::temp_directory_path()/"x64arm-handles-XXXXXX").string();std::vector<char> pattern(name.begin(),name.end());pattern.push_back(0);
                const auto fd=::mkstemp(pattern.data());if(fd<0)return finish(0xc0000017U);struct Close{int fd;~Close(){::close(fd);}} cleanup{fd};(void)::unlink(pattern.data());(void)::fcntl(fd,F_SETFD,FD_CLOEXEC);
                if(::ftruncate(fd,65534))return finish(0xc0000017U);
                error=remote_process_request(memory,pid,birth,14,7,values,bits,fd);if(error)return finish(native_status(error));if(values[0]>65534 || values[0]%4)return finish(0xc000000dU);
                bytes.resize(values[0]);std::size_t done=0;while(done<bytes.size()){const auto count=::pread(fd,bytes.data()+done,bytes.size()-done,static_cast<off_t>(done));if(count<0 && errno==EINTR)continue;if(count<=0)return finish(0xc000000dU);done+=static_cast<std::size_t>(count);}
#else
                return finish(0xc00000bbU);
#endif
            }
        }else if(process && cls==75){bytes.resize(4);put(0,4,1);}
        else if(process && cls==61){bytes.resize(1);}
        else if(process && cls==27){
#if !defined(_WIN32)
            auto name=(std::filesystem::temp_directory_path()/"x64arm-image-XXXXXX").string();std::vector<char> pattern(name.begin(),name.end());pattern.push_back(0);
            const auto fd=::mkstemp(pattern.data());if(fd<0)return finish(0xc0000017U);
            struct Close{int fd;~Close(){::close(fd);}} cleanup{fd};(void)::unlink(pattern.data());(void)::fcntl(fd,F_SETFD,FD_CLOEXEC);
            if(::ftruncate(fd,65534))return finish(0xc0000017U);
            if(pid==process_id())error=local_process_information(6,values,bits,fd);
            else {std::uint32_t target_pid{};error=process_memory_target(memory,call(0),0x400U,target_pid,birth);if(error==5)error=process_memory_target(memory,call(0),0x1000U,target_pid,birth);if(!error)error=remote_process_request(memory,pid,birth,14,6,values,bits,fd);}
            if(error)return finish(native_status(error));if(values[0]<2 || values[0]>65534 || values[0]%2)return finish(0xc000000dU);
            const auto header=narrow?8U:16U;bytes.resize(header+values[0]);
            std::size_t done=0;while(done<values[0]){const auto count=::pread(fd,bytes.data()+header+done,static_cast<std::size_t>(values[0]-done),static_cast<off_t>(done));if(count<0 && errno==EINTR)continue;if(count<=0)return finish(0xc000000dU);done+=static_cast<std::size_t>(count);}
            put(0,2,values[0]-2);put(2,2,values[0]);put(narrow?4U:8U,unit,output+header);
#else
            return finish(0xc00000bbU);
#endif
        }else if(!process){bytes.resize(cls==9?unit:cls==17?1U:4U);put(0,static_cast<unsigned>(bytes.size()),values[0]);}
        else return finish(0xc0000003U);
        if(call(4))memory.store(GuestAddress{call(4)},32,bytes.size());
        if(length<bytes.size())return finish(0xc0000004U);
        memory.write(GuestAddress{output},bytes);return finish(0);
    }catch(const MemoryFault&){return finish(0xc0000005U);}catch(const std::bad_alloc&){return finish(0xc0000017U);}
}
}
