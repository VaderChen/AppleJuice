#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <deque>

namespace x64arm {
namespace {
enum class PoolKind {Work,Timer,Wait,Simple};
struct PoolObject {
    PoolKind kind{};std::uint64_t owner{},callback{},context{},finalizer{},target{},deadline{UINT64_MAX},absolute_due{};
    std::deque<std::uint32_t> pending;std::uint32_t period{},outstanding{},associated{};
    bool armed{},set{},closed{},long_running{};
};
struct PoolInstance {
    std::uint64_t object{},worker{};bool associated{true},long_running{},finalized{};
    std::vector<GuestCallback> cleanup;
};
struct PoolDefinition {std::uint32_t minimum{},maximum{64};bool closed{};};
std::uint64_t add_ticks(std::uint64_t a,std::uint64_t b){return b>UINT64_MAX-a?UINT64_MAX:a+b;}
}
struct ThreadpoolState {
    std::map<std::uint64_t,PoolDefinition> pools;
    std::map<std::uint64_t,std::uint64_t> worker_owners;
    std::map<std::uint64_t,PoolObject> objects;
    std::map<std::uint64_t,PoolInstance> instances;
    std::uint64_t next_pool{0x110000},next_object{0xe0000},next_instance{0xf0000};
};
bool WindowsServices::threadpool_worker_reserved(std::uint64_t worker) const {
    return threadpool_ && threadpool_->worker_owners.contains(worker);
}
void WindowsServices::collect_threadpools(){
    if(!threadpool_)return;auto& pool=*threadpool_;
    std::erase_if(pool.worker_owners,[&](const auto& entry){return threads_.items.at(entry.first).status==ThreadStatus::Exited;});
    for(auto it=pool.pools.begin();it!=pool.pools.end();){
        const auto owner=it->first;const auto& definition=it->second;
        if(definition.closed && std::none_of(pool.objects.begin(),pool.objects.end(),[&](const auto& entry){return entry.second.owner==owner;})){
            // 沒有繫結物件及執行中回呼後，才將保留 worker 歸還共用閒置池。
            std::erase_if(pool.worker_owners,[&](const auto& entry){return entry.second==owner;});it=pool.pools.erase(it);
        }else {
            auto count=std::count_if(pool.worker_owners.begin(),pool.worker_owners.end(),[&](const auto& entry){return entry.second==owner;});
            std::erase_if(pool.worker_owners,[&](const auto& entry){
                if(entry.second!=owner || count<=definition.maximum || wait_workers_.at(entry.first))return false;
                --count;return true;
            });++it;
        }
    }
}
std::uint64_t WindowsServices::acquire_threadpool_worker(const GuestState& state,GuestMemory& memory,std::uint64_t owner){
    const auto stack=std::max(UINT64_C(65536),runtime_.image.stack_size);
    if(!owner)return acquire_wait_worker(state,memory,stack);
    auto& pool=*threadpool_;const auto& definition=pool.pools.at(owner);
    unsigned count=0,busy=0;std::uint64_t idle=0;
    for(const auto& [worker,id]:pool.worker_owners)if(id==owner){++count;if(wait_workers_.at(worker))++busy;else idle=worker;}
    // 調低上限不終止既有回呼；完成前暫停派送新的回呼。
    if(busy>=definition.maximum)return 0;
    if(idle)return idle;
    if(count>=definition.maximum)return 0;
    const auto worker=acquire_wait_worker(state,memory,stack);if(!worker)return 0;
    pool.worker_owners.emplace(worker,owner);wait_workers_[worker]=0;
    auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::RegisteredWait;
    return worker;
}
bool WindowsServices::threadpool_callbacks_pending(std::uint64_t handle) const {
    if(!threadpool_)return false;const auto found=threadpool_->objects.find(handle);
    return found!=threadpool_->objects.end() && (!found->second.pending.empty() || found->second.associated);
}
std::uint64_t WindowsServices::threadpool_next_deadline() const {
    std::uint64_t next=UINT64_MAX;if(!threadpool_)return next;
    // 單一池額滿不能遮住其他池的 timer／wait deadline；佇列額滿者待回呼釋出後再重試。
    for(const auto& [handle,object]:threadpool_->objects){(void)handle;if(object.armed && !object.closed && object.pending.size()<65536)next=std::min(next,object.absolute_due?runtime_.system_deadline(object.absolute_due):object.deadline);}
    return next;
}
void WindowsServices::release_threadpool_instance(std::uint64_t handle){
    if(!threadpool_)return;auto& pool=*threadpool_;const auto instance=pool.instances.find(handle);if(instance==pool.instances.end())return;
    const auto object=pool.objects.find(instance->second.object);
    if(object!=pool.objects.end()){
        auto& value=object->second;if(value.outstanding)--value.outstanding;if(instance->second.associated && value.associated)--value.associated;
        if(value.closed && !value.outstanding && value.pending.empty())pool.objects.erase(object);
    }
    pool.instances.erase(instance);
    collect_threadpools();
}
bool WindowsServices::cleanup_threadpool_callback(GuestState& state,GuestMemory& memory,std::uint64_t handle){
    auto& pool=*threadpool_;auto& instance=pool.instances.at(handle);const auto& object=pool.objects.at(instance.object);
    if(!instance.finalized){
        instance.finalized=true;
        if(object.finalizer){start_callbacks(state,memory,{{object.finalizer,{handle,object.context}}},Continuation::RegisteredWait);return true;}
    }
    auto callbacks=std::move(instance.cleanup);instance.cleanup.clear();
    if(callbacks.empty())return false;
    start_callbacks(state,memory,std::move(callbacks),Continuation::RegisteredWait);return true;
}
void WindowsServices::poll_threadpool(GuestMemory& memory){
    if(!threadpool_)return;collect_threadpools();auto& pool=*threadpool_;const auto saved_error=last_error_;
    for(auto& [handle,object]:pool.objects){
        (void)handle;if(!object.armed || object.closed)continue;
        if(object.absolute_due)object.deadline=runtime_.system_deadline(object.absolute_due);
        bool signaled=false;
        if(object.kind==PoolKind::Wait){
            const auto target=runtime_.wait_objects.find(object.target);
            SharedEventLock lock(target!=runtime_.wait_objects.end() && target->second.shared_event?target->second.shared_event->lock_descriptor():-1);
            if(target!=runtime_.wait_objects.end())signaled=runtime_.wait_state(target->second).signaled();
            else if(const auto id=thread_id(object.target))signaled=threads_.items.at(id).status==ThreadStatus::Exited;
            else if(const auto exited=process_signaled(object.target))signaled=*exited;
            if(!signaled && object.deadline>runtime_.clock_ticks)continue;
            if(object.pending.size()>=65536)continue;
            object.pending.push_back(signaled?0U:258U);object.armed=false;object.absolute_due=0;
            if(signaled && target!=runtime_.wait_objects.end())runtime_.wait_state(target->second).consume_signal();
        }else if(object.deadline<=runtime_.clock_ticks){
            if(object.pending.size()>=65536)continue;
            object.pending.push_back(0);
            object.absolute_due=0;
            if(object.period)object.deadline=add_ticks(object.deadline,std::uint64_t{object.period}*10000);
            else object.armed=false;
        }
    }
    for(auto& [handle,object]:pool.objects){
        while(!object.pending.empty()){
            if(!object.owner && registered_waits_.size()>=64)break;
            const auto worker=acquire_threadpool_worker(threads_.items.at(threads_.current).context,memory,object.owner);
            if(!worker)break;
            const auto instance_id=pool.next_instance++;PoolInstance instance;instance.object=handle;instance.worker=worker;instance.long_running=object.long_running;
            const auto result=object.pending.front();RegisteredWait work;work.worker=worker;work.pool_instance=instance_id;work.armed=false;work.ready=true;work.closing=true;
            auto arguments=std::vector<std::uint64_t>{instance_id,object.context};
            if(object.kind!=PoolKind::Simple)arguments.push_back(handle);
            if(object.kind==PoolKind::Wait)arguments.push_back(result);
            work.callback_batch.push_back({object.callback,std::move(arguments)});
            pool.instances.emplace(instance_id,std::move(instance));
            const auto registration=next_registered_wait_++;registered_waits_.emplace(registration,std::move(work));wait_workers_[worker]=registration;
            object.pending.pop_front();++object.outstanding;++object.associated;
            auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};
        }
    }
    last_error_=saved_error;
}
std::optional<std::uint32_t> WindowsServices::invoke_threadpool(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t result=0,std::uint32_t error=0){if(error)last_error_=error;arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    if(!threadpool_)threadpool_=std::make_shared<ThreadpoolState>();auto& pool=*threadpool_;
    const auto ptr=[&](std::uint64_t at){return memory.load(GuestAddress{at},x86_?32:64);};
    const auto deadline=[&](std::uint64_t at){
        if(!at)return std::pair<std::uint64_t,std::uint64_t>{UINT64_MAX,0};const auto value=memory.load(GuestAddress{at},64);
        if(value>>63)return std::pair<std::uint64_t,std::uint64_t>{add_ticks(runtime_.clock_ticks,UINT64_C(0)-value),0};
        return std::pair<std::uint64_t,std::uint64_t>{runtime_.system_deadline(value),value};
    };
    if(name=="CreateThreadpool"){
        if(arg(0))return finish(0,87);if(pool.pools.size()>=4096)return finish(0,8);
        const auto handle=pool.next_pool++;pool.pools.emplace(handle,PoolDefinition{});return finish(handle);
    }
    if(name=="CloseThreadpool" || name=="SetThreadpoolThreadMaximum" || name=="SetThreadpoolThreadMinimum"){
        const auto found=pool.pools.find(arg(0));
        if(found==pool.pools.end() || found->second.closed){if(name=="SetThreadpoolThreadMinimum")return finish(0,87);throw HostCallFault("無效的自訂 Threadpool");}
        auto& definition=found->second;
        if(name=="CloseThreadpool"){definition.closed=true;collect_threadpools();return finish();}
        const auto count=static_cast<std::uint32_t>(arg(1));
        if(name=="SetThreadpoolThreadMaximum"){
            definition.maximum=std::max(1U,count);definition.minimum=std::min(definition.minimum,definition.maximum);collect_threadpools();return finish();
        }
        // 預留實際客體 worker；配置失敗時保留舊設定，不以成功回覆掩蓋資源不足。
        if(count>64)return finish(0,8);
        std::vector<std::uint64_t> added;
        auto allocated=std::count_if(pool.worker_owners.begin(),pool.worker_owners.end(),[&](const auto& entry){return entry.second==arg(0);});
        while(allocated<count){
            const auto worker=acquire_wait_worker(state,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));
            if(!worker){for(auto id:added)pool.worker_owners.erase(id);return finish(0,last_error_?last_error_:8);}
            pool.worker_owners.emplace(worker,arg(0));wait_workers_[worker]=0;added.push_back(worker);++allocated;
            auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::RegisteredWait;
        }
        definition.minimum=count;definition.maximum=std::max(definition.maximum,count);return finish(1);
    }
    if(name.starts_with("Create") || name=="TrySubmitThreadpoolCallback"){
        if(!arg(0))return finish(0,87);if(!handles(GuestAddress{arg(0)}))memory.check(GuestAddress{arg(0)},1,Access::Execute);
        if(pool.objects.size()>=4096)return finish(0,8);
        PoolObject object;object.kind=name=="CreateThreadpoolTimer"?PoolKind::Timer:name=="CreateThreadpoolWait"?PoolKind::Wait:name=="TrySubmitThreadpoolCallback"?PoolKind::Simple:PoolKind::Work;
        object.callback=arg(0);object.context=arg(1);
        if(arg(2)){
            const auto env=arg(2),version=memory.load(GuestAddress{env},32);const unsigned unit=x86_?4:8;
            if(version!=1 && version!=3)return finish(0,87);
            const unsigned flags_at=7*unit;memory.check(GuestAddress{env},version==1?flags_at+4:x86_?40U:72U,Access::Read);
            object.owner=ptr(env+unit);
            if(object.owner){const auto owner=pool.pools.find(object.owner);if(owner==pool.pools.end() || owner->second.closed)return finish(0,87);}
            if(ptr(env+2*unit) || ptr(env+3*unit) || ptr(env+4*unit) || ptr(env+5*unit))return finish(0,50);
            const auto flags=memory.load(GuestAddress{env+flags_at},32);if(flags&~3U)return finish(0,87);object.long_running=bool(flags&1U);
            if(flags&2U)return finish(0,50); // 持續指定同一 worker 與 cleanup group 尚未提供。
            object.finalizer=ptr(env+6*unit);if(object.finalizer && !handles(GuestAddress{object.finalizer}))memory.check(GuestAddress{object.finalizer},1,Access::Execute);
            if(version==3 && (memory.load(GuestAddress{env+flags_at+4},32)!=1 || memory.load(GuestAddress{env+flags_at+8},32)!=(x86_?40U:72U)))return finish(0,50);
        }
        const auto handle=pool.next_object++;
        if(object.kind==PoolKind::Simple){object.pending.push_back(0);object.closed=true;}
        pool.objects.emplace(handle,std::move(object));return finish(name=="TrySubmitThreadpoolCallback"?1U:handle);
    }
    if(name.ends_with("WhenCallbackReturns") || name=="CallbackMayRunLong" || name=="DisassociateCurrentThreadFromCallback"){
        const auto found=pool.instances.find(arg(0));if(found==pool.instances.end() || found->second.worker!=threads_.current)throw HostCallFault("Threadpool callback instance 不屬於目前回呼");
        auto& instance=found->second;auto& object=pool.objects.at(instance.object);
        if(name=="DisassociateCurrentThreadFromCallback"){
            if(instance.associated){instance.associated=false;if(object.associated)--object.associated;}return finish();
        }
        if(name=="CallbackMayRunLong"){
            if(instance.long_running)return finish(0);instance.long_running=true;
            if(!object.owner && registered_waits_.size()>=64)return finish(0);
            const auto worker=acquire_threadpool_worker(state,memory,object.owner);
            if(!worker)return finish(0);
            wait_workers_[worker]=0;auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::RegisteredWait;
            return finish(1);
        }
        if(instance.cleanup.size()>=64)throw HostCallFault("Threadpool 延後清理數量超過上限");
        std::string_view api;std::vector<std::uint64_t> arguments{arg(1)};
        if(name=="SetEventWhenCallbackReturns")api="SetEvent";
        else if(name=="ReleaseSemaphoreWhenCallbackReturns"){api="ReleaseSemaphore";arguments.push_back(arg(2));arguments.push_back(0);}
        else if(name=="ReleaseMutexWhenCallbackReturns")api="ReleaseMutex";
        else if(name=="LeaveCriticalSectionWhenCallbackReturns")api="LeaveCriticalSection";
        else api="FreeLibrary";
        instance.cleanup.push_back({resolve("kernel32.dll",api).value,std::move(arguments)});return finish();
    }
    const auto found=pool.objects.find(arg(0));if(found==pool.objects.end() || found->second.closed)throw HostCallFault("無效的 Threadpool 物件");
    auto& object=found->second;const auto expected=name.find("Timer")!=std::string_view::npos?PoolKind::Timer:name.find("WaitCallbacks")!=std::string_view::npos || name.ends_with("Wait")?PoolKind::Wait:PoolKind::Work;
    if(object.kind!=expected)throw HostCallFault("Threadpool API 與物件類型不符");
    if(name=="SubmitThreadpoolWork"){
        if(object.pending.size()>=65536)throw HostCallFault("Threadpool 工作佇列容量不足");object.pending.push_back(0);return finish();
    }
    if(name=="SetThreadpoolTimer"){
        const auto due=deadline(arg(1));object.set=arg(1)!=0;object.armed=object.set;object.deadline=due.first;object.absolute_due=due.second;object.period=static_cast<std::uint32_t>(arg(2));return finish();
    }
    if(name=="IsThreadpoolTimerSet")return finish(object.set?1U:0U);
    if(name=="SetThreadpoolWait"){
        if(arg(1)){
            const auto target=runtime_.wait_objects.find(arg(1));
            if(target!=runtime_.wait_objects.end()){
                if(target->second.kind==WaitObject::Kind::Mutex || !(target->second.access&0x100000))throw HostCallFault("Threadpool wait 目標類型或權限不支援");
            }else if(const auto id=thread_id(arg(1))){(void)id;if(!(thread_access(arg(1))&0x100000))throw HostCallFault("Threadpool wait 執行緒權限不足");}
            else if(!process_signaled(arg(1)).has_value())throw HostCallFault("Threadpool wait 目標無效");
        }
        const auto due=arg(1)?deadline(arg(2)):std::pair<std::uint64_t,std::uint64_t>{UINT64_MAX,0};object.target=arg(1);object.armed=arg(1)!=0;object.deadline=due.first;object.absolute_due=due.second;return finish();
    }
    if(name.starts_with("WaitForThreadpool")){
        auto& caller=threads_.items.at(threads_.current);
        if(caller.wait.kind!=ThreadWaitKind::ThreadpoolCompletion && arg(1)){
            object.pending.clear();std::vector<std::uint64_t> cancel;
            for(const auto& [id,wait]:registered_waits_)if(wait.pool_instance && !wait.active && pool.instances.at(wait.pool_instance).object==arg(0))cancel.push_back(id);
            for(const auto id:cancel)release_registered_wait(id);
        }
        if(!threadpool_callbacks_pending(arg(0))){caller.wait={};return finish();}
        caller.status=ThreadStatus::Waiting;caller.wait={};caller.wait.kind=ThreadWaitKind::ThreadpoolCompletion;caller.wait.handles={arg(0)};
        schedule(state,memory);return std::nullopt;
    }
    if(name.starts_with("CloseThreadpool")){
        object.closed=true;object.armed=false;if(!object.outstanding && object.pending.empty())pool.objects.erase(found);collect_threadpools();return finish();
    }
    throw HostCallFault("未知的 Threadpool API");
}
}
