#include "x64arm/windows_services.hpp"
#include "x64arm/shared_job.hpp"
#include "x64arm/shared_event.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/unicode.hpp"
#include "monotonic_clock.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#if !defined(_WIN32)
#include <unistd.h>
#endif
namespace x64arm {
struct JobState {
    struct Handle {std::shared_ptr<SharedJob> object;std::uint32_t access{};};
    std::map<std::uint64_t,Handle> handles;
    std::shared_ptr<SharedJob> current;
    bool die_on_unhandled_exception{};
    struct Association {std::shared_ptr<SharedJob> observer;std::uint64_t port{},key{},cursor{},generation{},revision{};};
    std::map<std::uint64_t,Association> associations;
    // 宿主時鐘下，排程器與記憶體服務的共享 Job 輪詢依硬體時間限制頻率；
    // 本程序自己的 Job API 需要最新狀態時設定 immediate。
    std::uint64_t next_poll{},current_revision{};bool immediate{};
};
std::optional<WindowsServices::ObjectMetadata> WindowsServices::job_object_metadata(std::uint64_t handle)const{
    if(!jobs_)return {};const auto at=jobs_->handles.find(handle);if(at==jobs_->handles.end())return {};const auto identity=at->second.object->snapshot().identity;
    ObjectMetadata out;out.access=at->second.access;out.identity=identity;
    for(const auto& [h,item]:jobs_->handles){(void)h;if(item.object->snapshot().identity==identity)++out.handles;}
    out.pointers=out.handles;if(jobs_->current && jobs_->current->snapshot().identity==identity)++out.pointers;
    for(const auto& [h,item]:jobs_->associations){(void)h;if(item.observer->snapshot().identity==identity)++out.pointers;}return out;
}
bool WindowsServices::job_handle(std::uint64_t handle)const{return jobs_ && jobs_->handles.contains(handle);}
bool WindowsServices::export_job_handle(std::uint64_t handle,InheritedHandle& item)const{
    if(!job_handle(handle))return false;const auto& source=jobs_->handles.at(handle);item.type=4;item.access=source.access;item.descriptor=source.object->descriptor();item.lifetime=source.object;return true;
}
void WindowsServices::import_job_handle(const InheritedHandle& item){
    const auto opened=import_shared_job(mapping_session(),item.descriptor,true);
#if !defined(_WIN32)
    ::close(item.descriptor);
#endif
    if(opened.error)throw HostCallFault("無法匯入 Job handle："+std::to_string(opened.error));
    if(!jobs_)jobs_=std::make_shared<JobState>();jobs_->handles.emplace(item.handle,JobState::Handle{opened.job,item.access});
}
bool WindowsServices::close_job(std::uint64_t handle){return jobs_ && jobs_->handles.erase(handle);}
std::size_t WindowsServices::job_handle_count()const{return jobs_?jobs_->handles.size():0;}
void WindowsServices::append_job_handles(std::vector<std::uint64_t>& handles)const{if(jobs_)for(const auto& [handle,item]:jobs_->handles){(void)item;handles.push_back(handle);}}
std::u16string WindowsServices::job_object_name(std::uint64_t handle)const{return job_handle(handle)?jobs_->handles.at(handle).object->object_name():std::u16string{};}
std::uint32_t WindowsServices::job_security_location(std::uint64_t handle,std::uint32_t needed,int& native,int& lock)const{
    if(!jobs_ || !jobs_->handles.contains(handle))return 6;const auto& item=jobs_->handles.at(handle);if((item.access&needed)!=needed)return 5;
    native=item.object->descriptor();lock=item.object->lock_descriptor();return 0;
}
std::uint64_t WindowsServices::current_job_identity()const{return jobs_ && jobs_->current?jobs_->current->snapshot().identity:0;}
std::uint32_t WindowsServices::import_process_job(int descriptor){
    if(jobs_ && jobs_->current)return 50;
    const auto imported=import_shared_job(mapping_session(),descriptor,false);if(imported.error)return imported.error;
    const auto error=imported.job->join();if(error){if(error==1816)runtime_.job_exit_code=error;return error;}
    if(!jobs_)jobs_=std::make_shared<JobState>();jobs_->current=imported.job;
    jobs_->die_on_unhandled_exception=bool(imported.job->snapshot().flags&0x400U);
    if(jobs_->die_on_unhandled_exception)runtime_.error_mode|=2U; // SEM_NOGPFAULTERRORBOX：真正套用程序錯誤模式。
    for(const auto& [handle,window]:gui_.windows){(void)window;publish_gui_window(handle);}
    return 0;
}
int WindowsServices::inherited_job_descriptor(std::uint32_t flags,std::uint32_t& error)const{
    error=0;if(!jobs_ || !jobs_->current)return -1;const auto snapshot=jobs_->current->snapshot();
    if(snapshot.flags&0x1000U)return -1;
    if(flags&0x01000000U){if(snapshot.flags&0x800U)return -1;error=5;return -1;}
    return jobs_->current->descriptor();
}
int WindowsServices::startup_job_descriptor(std::span<const std::uint64_t> handles,int inherited,std::uint32_t& error)const{
    error=0;std::shared_ptr<SharedJob> selected;
    for(const auto handle:handles){
        if(!jobs_ || !jobs_->handles.contains(handle)){error=6;return -1;}
        const auto& item=jobs_->handles.at(handle);if(!(item.access&1U)){error=5;return -1;}
        if(selected){error=50;return -1;} // 尚無巢狀 Job，不能丟棄其餘限制。
        selected=item.object;
    }
    if(!selected){error=87;return -1;}
    if(inherited>=0 && jobs_->current->snapshot().identity!=selected->snapshot().identity){error=50;return -1;}
    return selected->descriptor();
}
std::uint32_t WindowsServices::job_ui_restrictions()const{return jobs_ && jobs_->current?jobs_->current->snapshot().ui_restrictions:0;}
std::shared_ptr<SharedJob> WindowsServices::job_atom_table()const{return (job_ui_restrictions()&0x20U)?jobs_->current:nullptr;}
bool WindowsServices::allowed_gui_window(const SharedWindowInfo& window)const{
    if(window.process==process_id() || !jobs_ || !jobs_->current)return true;
    const auto limits=jobs_->current->snapshot();return !(limits.ui_restrictions&1U) || window.job==limits.identity || jobs_->current->user_handle_granted(window.handle,window.process);
}
void WindowsServices::record_job_exit(std::uint32_t code){if(jobs_ && jobs_->current)jobs_->current->record_exit_code(code);}
void WindowsServices::poll_job_termination(GuestMemory& memory){
    // 共享 Job 輪詢需跨程序鎖定並逐一核對成員存活。限制、終止、成員與通知變更會遞增共享版本，
    // 不加鎖比對到變更時立即完整輪詢；版本未變時，存活核對與記憶體記帳每 5 ms 一次即可，
    // 避免 JIT 每個時間片都付出這筆成本。virtual 時鐘維持每次輪詢以保持可重現。
    constexpr std::uint64_t job_poll_interval=50000;
    bool throttled=false;
    if(jobs_ && runtime_.host_clock && !jobs_->immediate){
        bool changed=jobs_->current && jobs_->current->revision()!=jobs_->current_revision;
        for(const auto& [handle,association]:jobs_->associations){(void)handle;if(!changed && association.observer->revision()!=association.revision)changed=true;}
        const auto now=detail::monotonic_ticks();
        if(!changed && now<jobs_->next_poll)throttled=true;else jobs_->next_poll=now+job_poll_interval;
    }
    if(jobs_)jobs_->immediate=false;
    if(jobs_ && !throttled)for(auto it=jobs_->associations.begin();it!=jobs_->associations.end();){
        auto& association=it->second;association.revision=association.observer->revision();
        const auto notifications=association.observer->notifications_since(association.cursor,association.generation);
        if(!notifications){it=jobs_->associations.erase(it);continue;}
        if(const auto port=runtime_.completion_ports.find(association.port);port!=runtime_.completion_ports.end())for(const auto& notification:*notifications)port->second.packets.push_back({association.key,notification.pid,0,notification.message});
        ++it;
    }
    if(jobs_ && jobs_->current && !throttled){jobs_->current_revision=jobs_->current->revision();const auto usage=memory.usage();const auto limits=jobs_->current->poll_member(usage.private_commit,usage.peak_private_commit);const bool die=bool(limits.flags&0x400U);
        if(die && !jobs_->die_on_unhandled_exception)runtime_.error_mode|=2U;
        jobs_->die_on_unhandled_exception=die;
        memory.set_private_commit_limit(limits.process_memory_limit?limits.process_memory_limit:UINT64_MAX);
        if(!runtime_.job_exit_code)runtime_.job_exit_code=limits.termination_code;}
    if(!runtime_.job_exit_code)return;
    record_job_exit(*runtime_.job_exit_code);
    // 終止不執行 DLL/FLS 清理；由既有服務退出入口將客體碼送回父程序。
    redirect_process_termination(*runtime_.job_exit_code);
}
void WindowsServices::redirect_process_termination(std::uint32_t code){
    // 強制結束共用相同入口；退出碼由 invoke 的 forced_exit_code 提供。
    for(auto& [id,thread]:threads_.items){
        (void)id;if(thread.status==ThreadStatus::Exited)continue;
        thread.context.rip=process_exit_retry_;thread.context.reg(Gpr::Rcx)=code;
        thread.status=ThreadStatus::Runnable;thread.suspended=0;thread.wait={};
    }
    threads_.yield=true;
}
std::optional<std::uint32_t> WindowsServices::invoke_job(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    // 本程序的 Job API 可能改變限制、成員或終止狀態（handle 物件未必與目前 Job 是同一實例）；
    // 結束後下一次輪詢不受節流，讓緊接的配置、例外或等待看到最新狀態。
    struct ImmediatePoll {std::shared_ptr<JobState>& jobs;~ImmediatePoll(){if(jobs)jobs->immediate=true;}} immediate_poll{jobs_};
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;std::uint64_t result=0;
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};const std::uint8_t bits=x86_?32:64;
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    if(!runtime_.network.external_io && (symbol.starts_with("Create") || symbol.starts_with("Open") || symbol=="AssignProcessToJobObject"))throw HostCallFault("共用 Job 不支援 deterministic compare");
    if(!jobs_)jobs_=std::make_shared<JobState>();
    const auto access_mask=[](std::uint32_t access){const auto generic=access;access&=0x0fffffffU;if(generic&0x80000000U)access|=0x20004;if(generic&0x40000000U)access|=0x2000b;if(generic&0x20000000U)access|=0x120000;if(generic&0x10000000U)access=0x1f003f;return access;};
    try{
    if(symbol=="UserHandleGrantAccess"){
        const auto job=jobs_->handles.find(arg(1));const auto window=shared_gui_window(arg(0));
        if(job==jobs_->handles.end())fail(6);else if(!(job->second.access&2U))fail(5);else if(!window)fail(1400);
        else if(current_job_identity()==job->second.object->snapshot().identity || window->job==job->second.object->snapshot().identity)fail(5);
        else {const auto error=job->second.object->grant_user_handle(window->handle,window->process,arg(2)!=0);if(error)fail(error);else result=1;}
    }else if(symbol.starts_with("CreateJobObject") || symbol.starts_with("OpenJobObject")){
        const bool create=symbol.starts_with("Create");const auto nameptr=arg(create?1:2);const auto rights=create?0x1f003fU:access_mask(u32(0));
        const auto security=create?synchronization_descriptor(memory,arg(0),false):SynchronizationDescriptor{arg(1)!=0,{}};
        if(rights&~0x21f003fU)fail(5);else if(jobs_->handles.size()>=1024)fail(8);else if(!nameptr && !create)fail(87);
        else {
            auto name=nameptr?(symbol.ends_with('W')?read_utf16(memory,GuestAddress{nameptr}):utf8_to_utf16(read_ansi(memory,nameptr))):std::u16string{};
            if(!nameptr){const auto text="x64arm-job-"+std::to_string(process_id())+"-"+std::to_string(runtime_.next_wait_handle);name.assign(text.begin(),text.end());}
            const auto opened=open_shared_job(mapping_session(),std::move(name),create,[&]{return security.bytes;});
            if(opened.error)fail(opened.error);else {SharedEventLock lock(opened.job->lock_descriptor());const auto descriptor=shared_object_security(opened.job->descriptor());if(descriptor.empty())throw SecurityDescriptorError{1338};const auto granted=create && !opened.existed?rights:object_security_access(descriptor,rights,{0x20004U,0x2000bU,0x120000U,0x1f003fU});result=runtime_.allocate_wait_handle();jobs_->handles.emplace(result,JobState::Handle{opened.job,granted});runtime_.handle_flags[result]=security.inherit?1:0;if(create)last_error_=opened.existed?183:0;}
        }
    }else if(symbol=="DuplicateHandle"){
        const auto source=jobs_->handles.find(arg(1));
        if(source==jobs_->handles.end() || !current_process_handle(arg(0),0x40) || !current_process_handle(arg(2),0x40))fail(6);
        else if(!arg(3) || (u32(6)&~3U))fail(87);
        else if((u32(6)&1U) && (runtime_.handle_flags[arg(1)]&2U))fail(5);
        else if(!(u32(6)&2U) && (u32(4)&~source->second.access))fail(5);
        else {memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);const auto handle=runtime_.allocate_wait_handle();auto copy=source->second;if(!(u32(6)&2U))copy.access=u32(4);jobs_->handles.emplace(handle,std::move(copy));runtime_.handle_flags[handle]=arg(5)?1:0;memory.store(GuestAddress{arg(3)},bits,handle);if(u32(6)&1U){jobs_->handles.erase(source);runtime_.handle_flags.erase(arg(1));}result=1;}
    }else if(symbol=="IsProcessInJob"){
        auto pid=process_handle_pid(arg(0),0x400);if(!pid)pid=process_handle_pid(arg(0),0x1000);
        if(!pid)fail(process_object_type(arg(0))==u"Process"?5U:6U);
        else if(arg(1) && !job_handle(arg(1)))fail(6);
        else if(arg(1) && !(jobs_->handles.at(arg(1)).access&4U))fail(5);
        else if(!arg(2))fail(87);
        else {memory.check(GuestAddress{arg(2)},4,Access::Write);std::uint64_t identity=0;const auto error=remote_process_job(memory,pid,-1,identity);if(error)fail(error);else{memory.store(GuestAddress{arg(2)},32,identity && (!arg(1) || identity==jobs_->handles.at(arg(1)).object->snapshot().identity));result=1;}}
    }else {
        std::shared_ptr<SharedJob> object;std::uint32_t rights=0;
        if(symbol=="QueryInformationJobObject" && !arg(0)){object=jobs_->current;rights=4;}else if(const auto found=jobs_->handles.find(arg(0));found!=jobs_->handles.end()){object=found->second.object;rights=found->second.access;}
        if(!object)fail(6);
        else if(symbol=="AssignProcessToJobObject"){
            const auto pid=process_handle_pid(arg(1),0x101);
            if(!(rights&1U))fail(5);else if(!pid)fail(process_object_type(arg(1))==u"Process"?5U:6U);
            else {std::uint64_t identity=0;const auto error=remote_process_job(memory,pid,object->descriptor(),identity);if(error)fail(error);else result=1;}
        }else if(symbol=="TerminateJobObject"){
            if(!(rights&8U))fail(5);else{object->terminate(u32(1));jobs_->immediate=true;poll_job_termination(memory);if(runtime_.job_exit_code)return *runtime_.job_exit_code;result=1;}
        }else if(symbol=="SetInformationJobObject" || symbol=="QueryInformationJobObject"){
            const bool setting=symbol.starts_with("Set");const auto kind=u32(1);if(jobs_->current==object){jobs_->immediate=true;poll_job_termination(memory);}const auto snapshot=object->snapshot();const unsigned basic=x86_?48:64,extended=x86_?112:144;
            if(!(rights&(setting?2U:4U)))fail(5);
            else if(kind==7){
                if(!setting || !arg(2))fail(87);
                else if(u32(3)!=(x86_?8U:16U))fail(24);
                else {
                    memory.check(GuestAddress{arg(2)},x86_?8:16,Access::Read);
                    const auto key=memory.load(GuestAddress{arg(2)},bits),port=memory.load(GuestAddress{arg(2)+(x86_?4:8)},bits);
                    if(port && !runtime_.completion_ports.contains(port))fail(6);
                    else {
                        // 觀察者不持有 Job owner reference，不能延後 KILL_ON_JOB_CLOSE。
                        const auto observer=port?import_shared_job(mapping_session(),object->descriptor(),false):OpenSharedJob{};
                        JobSnapshot associated;const auto error=observer.error?observer.error:object->associate_completion_port(port,key,associated);
                        if(error)fail(error);
                        else {
                            jobs_->associations.erase(snapshot.identity);
                            if(port){jobs_->associations.emplace(snapshot.identity,JobState::Association{observer.job,port,key,associated.notification_sequence,associated.completion_generation});for(const auto pid:associated.processes)runtime_.completion_ports.at(port).packets.push_back({key,pid,0,6});}
                            result=1;
                        }
                    }
                }
            }
            else if(kind!=2 && kind!=3 && kind!=4 && kind!=9)fail(50);
            else if(setting && kind==3)fail(87);
            else if(!arg(2))fail(87);
            else {
                const auto size=kind==2?basic:kind==9?extended:kind==4?4U:8U+snapshot.processes.size()*(x86_?4:8);
                if(!setting && arg(4))memory.store(GuestAddress{arg(4)},32,size);
                if(!setting && kind==3 && u32(3)>=8 && u32(3)<size){
                    const auto copied=std::min<std::size_t>((u32(3)-8)/(x86_?4:8),snapshot.processes.size());memory.check(GuestAddress{arg(2)},8+copied*(x86_?4:8),Access::Write);
                    memory.store(GuestAddress{arg(2)},32,snapshot.processes.size());memory.store(GuestAddress{arg(2)+4},32,copied);for(std::size_t i=0;i<copied;++i)memory.store(GuestAddress{arg(2)+8+i*(x86_?4:8)},bits,snapshot.processes[i]);fail(234);
                }else if(u32(3)<size)fail(24);
                else if(setting){
                    memory.check(GuestAddress{arg(2)},size,Access::Read);
                    if(std::getenv("X64ARM_TRACE_PROCESS_PROTOCOL")){const auto detail=memory.load(GuestAddress{arg(2)+(kind==4?0:16)},32);std::fprintf(stderr,"job_set_information pid=%u kind=%u size=%u flags=%x\n",process_id(),kind,u32(3),static_cast<unsigned>(detail));if(kind==9)std::fprintf(stderr,"job_process_memory_limit pid=%u bytes=%llu\n",process_id(),static_cast<unsigned long long>(memory.load(GuestAddress{arg(2)+(x86_?96:112)},bits)));}
                    if(kind==4){const auto error=object->set_ui_restrictions(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32)));if(error)fail(error);else result=1;}
                    else {const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+16},32));const auto limit=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+(x86_?28:40)},32));const auto memory_limit=kind==9?memory.load(GuestAddress{arg(2)+(x86_?96:112)},bits):0;const auto error=kind==2 && (flags&0x3d00U)?87U:object->set_limits(flags,limit,memory_limit);if(error)fail(error);else {if(jobs_->current==object){jobs_->immediate=true;poll_job_termination(memory);}result=1;}}
                }else {
                    memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(size));
                    if(kind==2 || kind==9){memory.store(GuestAddress{arg(2)+16},32,snapshot.flags);memory.store(GuestAddress{arg(2)+(x86_?28:40)},32,snapshot.active_limit);if(kind==9){memory.store(GuestAddress{arg(2)+(x86_?96:112)},bits,snapshot.process_memory_limit);memory.store(GuestAddress{arg(2)+(x86_?104:128)},bits,snapshot.peak_process_memory);memory.store(GuestAddress{arg(2)+(x86_?108:136)},bits,snapshot.peak_job_memory);}}
                    else if(kind==4)memory.store(GuestAddress{arg(2)},32,snapshot.ui_restrictions);
                    else if(kind==3){memory.store(GuestAddress{arg(2)},32,snapshot.processes.size());memory.store(GuestAddress{arg(2)+4},32,snapshot.processes.size());for(std::size_t i=0;i<snapshot.processes.size();++i)memory.store(GuestAddress{arg(2)+8+i*(x86_?4:8)},bits,snapshot.processes[i]);}
                    result=1;
                }
            }
        }else throw HostCallFault("未知的 Job 服務");
    }
    }catch(const SecurityDescriptorError& error){fail(error.code);}catch(const MemoryFault&){fail(998);}catch(const std::bad_alloc&){fail(8);}
    arg.return_to(state,result);return std::nullopt;
}
}
