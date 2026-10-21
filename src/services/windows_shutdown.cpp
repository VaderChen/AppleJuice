#include "x64arm/windows_services.hpp"
#include "x64arm/machine_shutdown.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/security_access.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <new>
#include <stdexcept>

namespace x64arm {
void WindowsServices::prepare_machine_shutdown(){
    if(machine_shutdown_ || !runtime_.network.external_io)return;
    machine_shutdown_=MachineShutdown::open(mapping_session(),runtime_.host_clock);
}
void WindowsServices::poll_machine_shutdown(){
    if(!machine_shutdown_)return;
    if(!runtime_.shutdown_exit_code){
        const auto phase=machine_shutdown_->phase();
        if(phase!=ShutdownPhase::Countdown && phase!=ShutdownPhase::Committed)return;
        if(phase==ShutdownPhase::Countdown){
            const auto now=runtime_.machine_clock?runtime_.machine_clock->synchronize(runtime_.clock_ticks):runtime_.clock_ticks;
            if(!machine_shutdown_->poll(now))return;
        }
        runtime_.shutdown_exit_code=0;
    }
    record_job_exit(*runtime_.forced_exit_code());
    redirect_process_termination(*runtime_.forced_exit_code());
}
std::uint64_t WindowsServices::machine_shutdown_deadline()const{
    return machine_shutdown_?machine_shutdown_->deadline():UINT64_MAX;
}
std::optional<std::uint32_t> WindowsServices::invoke_shutdown(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
    const auto symbol=service.symbol;
    const bool exit_windows=symbol=="ExitWindowsEx",modern=symbol=="InitiateShutdownA" || symbol=="InitiateShutdownW";
    const bool aborting=symbol.starts_with("AbortSystemShutdown"),wide=symbol.ends_with('W');
    const auto dword=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto finish=[&](std::uint32_t error)->std::optional<std::uint32_t>{
        if(!modern && error)last_error_=error;
        arg.return_to(state,modern?error:error?0:1);return std::nullopt;
    };
    try {
        if(exit_windows && (job_ui_restrictions()&0x80U))return finish(5);
        const auto text=[&](unsigned index,std::size_t limit){
            if(!arg(index))return std::u16string{};
            if(wide)return read_utf16(memory,GuestAddress{arg(index)},limit);
            // Windows 環境字串與 GetACP 沿用目前共用 UTF-8 ACP。
            try{return utf8_to_utf16(read_ansi(memory,arg(index),limit));}
            catch(const HostCallFault&){throw SecurityDescriptorError{87};}
        };
        if(!exit_windows){
            auto machine=text(0,255);
            if(machine.starts_with(u"\\\\")){
                machine.erase(0,2);if(machine.empty())return finish(1210);
            }
            for(auto& unit:machine)unit=unicode_case_unit(unit,false);
            if(machine.find_first_of(u"\\/:*?\"<>|")!=machine.npos)return finish(1210);
            if(!machine.empty() && machine!=u"x64arm" && machine!=u"localhost" && machine!=u".")return finish(53);
        }
        ShutdownRequest request;bool force=false,override_grace=false,unsupported=false;
        if(!aborting){
            if(exit_windows){
                const auto flags=dword(0),type=flags&0x4bU;
                if((flags&~0x0040005fU) || (type && (type&(type-1))) || ((flags&0x00400000U) && type!=1))return finish(87);
                if(!type)return finish(50); // 登出須依虛擬登入工作階段另行協商。
                request.reason=dword(1);
                request.action=type==1?ShutdownAction::Halt:ShutdownAction::PowerOff;
                force=flags&4;
                unsupported=type==2 || type==0x40 || (flags&0x00400000U);
            }else {
                request.message=text(1,MachineShutdown::max_message);
                request.seconds=dword(2);
                if(request.seconds>MachineShutdown::max_timeout)return finish(87);
                if(modern){
                    const auto flags=dword(3),type=flags&0x1cU;
                    override_grace=flags&0x20U;
                    if((flags&~0x3ffffU) || (type && (type&(type-1))) || (override_grace && request.seconds) ||
                       (request.seconds && !(flags&2U)))return finish(87);
                    request.reason=dword(4);force=(flags&3U)==3U;
                    request.action=type==0x10?ShutdownAction::Halt:ShutdownAction::PowerOff;
                    unsupported=!type || type==4 || (flags&~0x3bU);
                }else {
                    force=dword(3)!=0;
                    unsupported=dword(4)!=0;
                    request.reason=symbol.starts_with("InitiateSystemShutdownEx")?dword(5):0x80070000U;
                }
            }
        }
        refresh_file_security_context();
        if(!runtime_.token_objects.at(effective_token_id()).has_privilege(19,true))return finish(1314);
        if(!runtime_.network.external_io)return finish(50);
        if(!aborting && (unsupported || !force || request.seconds))return finish(50);
        // 尚無倒數對話與 WM_QUERYENDSESSION／console shutdown 協商；
        // 公開入口只接受立即強制關機，避免先回成功再遺失這些契約。
        prepare_machine_clock();prepare_machine_shutdown();
        if(!machine_shutdown_)return finish(50);
        const auto now=runtime_.machine_clock?runtime_.machine_clock->synchronize(runtime_.clock_ticks):runtime_.clock_ticks;
        if(aborting)return finish(machine_shutdown_->abort(now));
        request.origin={process_id(),process_identity_birth()};
        return finish(machine_shutdown_->initiate(request,now,override_grace,modern));
    }catch(const SecurityDescriptorError& error){return finish(error.code);}
    catch(const MemoryFault&){return finish(998);}
    catch(const std::invalid_argument&){return finish(87);}
    catch(const std::bad_alloc&){return finish(1450);}
    catch(const HostCallFault&){return finish(31);}
}
}
