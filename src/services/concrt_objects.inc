// ConcRT 排程器物件模型：Scheduler、ScheduleGroup、Context 物件與 CurrentScheduler／Context／_Scheduler／_Context 的入口；由 windows_nt_native.cpp 引入
// （放在 concrt_scheduler_handlers.inc 之前）。物件是客體記憶體中只有 vtable 指標的區塊，狀態在宿主；vtable 依 MSVC 規則排列：
// 宣告順序，多載函式集中在第一個宣告處並倒序（CreateScheduleGroup(location&) 在 CreateScheduleGroup() 之前，ScheduleTask 亦同）。
// Scheduler：純量刪除解構子、Id、GetNumberOfVirtualProcessors、GetPolicy、Reference、Release、RegisterShutdownEvent、Attach、CreateScheduleGroup×2、ScheduleTask×2、IsAvailableLocation。
// ScheduleGroup：ScheduleTask、Id、Reference、Release、解構子。Context：GetId、GetVirtualProcessorId、GetScheduleGroupId、Unblock、IsSynchronouslyBlocked、解構子。
// 預設排程器（識別碼 1）在第一次需要時建立；CurrentScheduler::Create 與 Scheduler::Attach 推入每執行緒的排程器堆疊，Detach 取出並釋放參考。
// 工作一律交給既有的 TaskThunk／QueueUserWorkItem 執行緒池，排程群組與 location 不改變執行位置。
if(service.family=="internal-concrt-object" || (service.family=="concrt140.dll" && (
   name.starts_with("?Create@CurrentScheduler@") || name.starts_with("?Create@Scheduler@") || name.starts_with("?CreateScheduleGroup@CurrentScheduler@") ||
   name.starts_with("?Detach@CurrentScheduler@") || name.starts_with("?Get@CurrentScheduler@") || name.starts_with("?GetPolicy@CurrentScheduler@") ||
   name.starts_with("?IsAvailableLocation@CurrentScheduler@") || name.starts_with("?RegisterShutdownEvent@CurrentScheduler@") || name.starts_with("?_Get@_CurrentScheduler@") ||
   name.starts_with("?CurrentContext@Context@") || name.starts_with("?Block@Context@") || name.starts_with("?_CurrentContext@_Context@") ||
   name.find("_Scheduler@details@Concurrency@@")!=name.npos || name.starts_with("?_IsSynchronouslyBlocked@_Context@")))){
    const auto thread=threads_.current;
    constexpr std::array<std::uint32_t,10> default_policy{0,UINT32_MAX,1,1,8,0,0,0,1,1};
    if(!concrt_vtables_[0]){
        const std::string suffix=x86_?"32":"64";
        const auto build=[&](const char* prefix,std::initializer_list<const char*> methods){
            // vtable 前一格是 RTTI complete object locator（不提供）。
            const auto storage=runtime_allocate(memory,{},(methods.size()+1)*unit),table=storage+unit;unsigned slot=0;
            for(const auto* method:methods)memory.store(GuestAddress{table+slot++*unit},bits,resolve("internal-concrt-object",std::string(prefix)+method+suffix).value);
            return table;
        };
        concrt_vtables_={build("Sched",{"Delete","Id","Processors","GetPolicy","Reference","Release","RegisterShutdownEvent","Attach","CreateGroupAt","CreateGroup","ScheduleTaskAt","ScheduleTask","IsAvailableLocation"}),
                         build("Group",{"ScheduleTask","Id","Reference","Release","Delete"}),
                         build("Ctx",{"GetId","GetVirtualProcessorId","GetScheduleGroupId","Unblock","IsSynchronouslyBlocked","Delete"})};
    }
    const auto make_object=[&](unsigned kind)->std::uint64_t{const auto object=allocate_heap(memory,unit*2);if(!object)return 0;memory.store(GuestAddress{object},bits,concrt_vtables_[kind]);memory.store(GuestAddress{object+unit},bits,0);return object;};
    const auto create_scheduler=[&](const std::array<std::uint32_t,10>& policy,std::uint32_t id)->std::uint64_t{
        const auto object=make_object(0);if(!object)return 0;ConcrtScheduler scheduler;scheduler.id=id;scheduler.policy=policy;concrt_schedulers_[object]=std::move(scheduler);return object;
    };
    // 目前的排程器：堆疊頂端，否則預設排程器（必要時建立並隱含附加）。
    const auto current=[&]()->std::uint64_t{
        concrt_attached_.insert(thread);
        if(const auto stack=concrt_scheduler_stack_.find(thread);stack!=concrt_scheduler_stack_.end() && !stack->second.empty())return stack->second.back();
        if(!concrt_default_scheduler_ || !concrt_schedulers_.contains(concrt_default_scheduler_))concrt_default_scheduler_=create_scheduler(concrt_default_policy_.value_or(default_policy),1);
        return concrt_default_scheduler_;
    };
    const auto policy_values=[&](std::uint64_t policy){std::array<std::uint32_t,10> values=default_policy;const auto bag=memory.load(GuestAddress{policy},bits);
        if(bag)for(unsigned i=0;i<10;++i)values[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{bag+i*4},32));return values;};
    // 回傳 SchedulerPolicy 值：與 SchedulerPolicy 建構子相同，在 CRT heap 配置 10 個 unsigned 的值袋。
    const auto write_policy=[&](std::uint64_t output,const std::array<std::uint32_t,10>& values)->bool{
        memory.check(GuestAddress{output},unit,Access::Write);const auto bag=allocate_heap(memory,40);if(!bag)return false;
        for(unsigned i=0;i<10;++i)memory.store(GuestAddress{bag+i*4},32,values[i]);memory.store(GuestAddress{output},bits,bag);return true;
    };
    const auto release=[&](std::uint64_t object)->std::uint32_t{
        const auto found=concrt_schedulers_.find(object);if(found==concrt_schedulers_.end())return 0;if(found->second.references)--found->second.references;
        const auto left=found->second.references;if(left)return left;
        // 最後一個參考：觸發登記的關閉事件並釋放物件（預設排程器下次需要時重建）。
        bool signaled=false;for(const auto event:found->second.shutdown_events){const auto item=runtime_.wait_objects.find(event);if(item!=runtime_.wait_objects.end() && item->second.kind==WaitObject::Kind::Event){item->second.set_signal(true);signaled=true;}}
        concrt_schedulers_.erase(found);std::erase_if(concrt_groups_,[&](const auto& group){return group.second[0]==object;});(void)free_heap(memory,object,state);
        if(object==concrt_default_scheduler_)concrt_default_scheduler_=0;if(signaled)(void)resolve_thread_waits(memory,ThreadWakeScope::Objects);return 0;
    };
    const auto processors=[&](std::uint64_t object){const auto found=concrt_schedulers_.find(object);const auto limit=found==concrt_schedulers_.end()?UINT32_MAX:found->second.policy[1];
        const auto active=static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll","GetActiveProcessorCount",{0xffff}));return limit==UINT32_MAX?active:limit;};
    const auto create_group=[&](std::uint64_t scheduler)->std::uint64_t{const auto object=make_object(1);if(!object)return 0;concrt_groups_[object]={scheduler,1,concrt_next_group_++};return object;};
    const auto schedule_task=[&](std::uint64_t proc,std::uint64_t data)->bool{
        if(!proc)return false;const auto block=allocate_heap(memory,unit*2);if(!block)return false;memory.store(GuestAddress{block},bits,proc);memory.store(GuestAddress{block+unit},bits,data);
        if(!nested_service(state,memory,"kernel32.dll","QueueUserWorkItem",{resolve("internal-concrt-scheduler","TaskThunk").value,block,0})){(void)free_heap(memory,block,state);return false;}return true;
    };
    // location：低 28 位元是類型（_System 0、_NumaNode 1、_SchedulingNode 2、_ExecutionResource 3），之後是繫結識別碼；沙箱只有一個 NUMA 節點。
    const auto available=[&](std::uint64_t object,std::uint64_t location)->bool{
        if(!location)return false;const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{location},32))&0x0fffffffU;const auto id=static_cast<std::uint32_t>(memory.load(GuestAddress{location+4},32));
        return type==0 || ((type==1 || type==2) && id==0) || (type==3 && id<processors(object));
    };
    const auto context_of=[&](std::uint64_t owner)->ConcrtContext&{
        auto& context=concrt_contexts_[owner];if(!context.object)context.object=make_object(2);return context;
    };
    const auto context_owner=[&](std::uint64_t object)->std::uint64_t{for(const auto& [owner,context]:concrt_contexts_)if(context.object==object)return owner;return UINT64_MAX;};
    if(service.family=="internal-concrt-object"){
        const auto self=x86_?state.reg(Gpr::Rcx)&UINT32_MAX:call(0);const auto a=[&](unsigned index){return call(index+(x86_?0U:1U));};
        const auto method=std::string_view(name).substr(0,name.size()-2);
        if(method.starts_with("Sched")){
            if(!concrt_schedulers_.contains(self))throw HostCallFault("Scheduler 物件不是 ConcRT 建立的排程器");auto& scheduler=concrt_schedulers_.at(self);
            if(method=="SchedDelete"){concrt_schedulers_.erase(self);if(static_cast<std::uint32_t>(a(0))&1U)(void)free_heap(memory,self,state);if(self==concrt_default_scheduler_)concrt_default_scheduler_=0;return finish(self);}
            if(method=="SchedId")return finish(scheduler.id);
            if(method=="SchedProcessors")return finish(processors(self));
            if(method=="SchedGetPolicy"){if(!write_policy(a(0),scheduler.policy)){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return std::nullopt;}return finish(a(0));}
            if(method=="SchedReference")return finish(++scheduler.references);
            if(method=="SchedRelease")return finish(release(self));
            if(method=="SchedRegisterShutdownEvent"){if(a(0))scheduler.shutdown_events.push_back(a(0));return finish(0);}
            if(method=="SchedAttach"){
                // 已經是目前的排程器時是 improper_scheduler_attach；附加持有一個參考。
                auto& stack=concrt_scheduler_stack_[thread];concrt_attached_.insert(thread);
                if((!stack.empty() && stack.back()==self) || (stack.empty() && self==concrt_default_scheduler_)){throw_msvc_exception(state,memory,service,MsvcExceptionKind::ImproperSchedulerAttach);return std::nullopt;}
                ++scheduler.references;stack.push_back(self);return finish(0);
            }
            if(method=="SchedCreateGroupAt" || method=="SchedCreateGroup")return finish(create_group(self));
            if(method=="SchedScheduleTaskAt" || method=="SchedScheduleTask"){(void)schedule_task(a(0),a(1));return finish(0);}
            if(method=="SchedIsAvailableLocation")return finish(available(self,a(0))?1:0);
        }else if(method.starts_with("Group")){
            const auto found=concrt_groups_.find(self);if(found==concrt_groups_.end())throw HostCallFault("ScheduleGroup 物件不是 ConcRT 建立的群組");
            if(method=="GroupScheduleTask"){(void)schedule_task(a(0),a(1));return finish(0);}
            if(method=="GroupId")return finish(found->second[2]);
            if(method=="GroupReference")return finish(++found->second[1]);
            if(method=="GroupRelease"){if(found->second[1])--found->second[1];const auto left=found->second[1];if(!left){concrt_groups_.erase(found);(void)free_heap(memory,self,state);}return finish(left);}
            if(method=="GroupDelete"){concrt_groups_.erase(found);if(static_cast<std::uint32_t>(a(0))&1U)(void)free_heap(memory,self,state);return finish(self);}
        }else if(method.starts_with("Ctx")){
            const auto owner=context_owner(self);if(owner==UINT64_MAX)throw HostCallFault("Context 物件不是 ConcRT 建立的 Context");auto& context=concrt_contexts_.at(owner);
            if(method=="CtxGetId")return finish(concrt_context_ids_.try_emplace(owner,static_cast<std::uint32_t>(concrt_context_ids_.size())).first->second);
            if(method=="CtxGetVirtualProcessorId")return finish(context.blocked?UINT32_MAX:nested_service(state,memory,"kernel32.dll","GetCurrentProcessorNumber",{})&UINT32_MAX);
            if(method=="CtxGetScheduleGroupId")return finish(0);
            if(method=="CtxIsSynchronouslyBlocked")return finish(context.blocked?1:0);
            if(method=="CtxDelete")return finish(self); // Context 由執行階段擁有，不在此釋放。
            if(method=="CtxUnblock"){
                // 等待中的 Context 立刻喚醒；還沒 Block 時記下，下一次 Block 直接返回。
                if(context.blocked && context.event){const auto item=runtime_.wait_objects.find(context.event);if(item!=runtime_.wait_objects.end()){item->second.set_signal(true);(void)resolve_thread_waits(memory,ThreadWakeScope::Objects);}}
                else context.unblocked=true;
                return finish(0);
            }
        }
        throw HostCallFault("未知的 ConcRT 物件方法："+std::string(name));
    }
    // _Scheduler／_Context 包裝類別（x86 thiscall）。
    if(name.find("_Scheduler@details@Concurrency@@")!=name.npos && !name.starts_with("?_Get@")){
        const auto self=x86_?state.reg(Gpr::Rcx)&UINT32_MAX:call(0);const auto a=[&](unsigned index){return call(index+(x86_?0U:1U));};
        memory.check(GuestAddress{self},unit,Access::Write);
        if(name.starts_with("??0_Scheduler@")){memory.store(GuestAddress{self},bits,a(0));return finish(self);}
        if(name.starts_with("??_F_Scheduler@")){memory.store(GuestAddress{self},bits,0);return finish(self);}
        const auto scheduler=memory.load(GuestAddress{self},bits);
        if(name.starts_with("?_GetScheduler@"))return finish(scheduler);
        if(!concrt_schedulers_.contains(scheduler))throw HostCallFault("_Scheduler 沒有 ConcRT 排程器");
        if(name.starts_with("?_Reference@"))return finish(++concrt_schedulers_.at(scheduler).references);
        return finish(release(scheduler));
    }
    if(name.starts_with("?_IsSynchronouslyBlocked@")){
        const auto self=x86_?state.reg(Gpr::Rcx)&UINT32_MAX:call(0);const auto object=memory.load(GuestAddress{self},bits);const auto owner=context_owner(object);
        return finish(owner!=UINT64_MAX && concrt_contexts_.at(owner).blocked?1:0);
    }
    // 靜態函式（__cdecl）：回傳類別值的函式以第一個參數接收隱藏的回傳指標，並回傳它。
    if(name.starts_with("?Create@Scheduler@") || name.starts_with("?Create@CurrentScheduler@")){
        const auto values=policy_values(call(0));const auto object=create_scheduler(values,concrt_next_scheduler_++);
        if(!object){throw_msvc_exception(state,memory,service,MsvcExceptionKind::SchedulerResourceAllocationError);return std::nullopt;}
        if(name.starts_with("?Create@Scheduler@"))return finish(object);
        concrt_attached_.insert(thread);concrt_scheduler_stack_[thread].push_back(object);return finish(0); // 附加持有建立時的參考
    }
    if(name.starts_with("?Detach@")){
        auto stack=concrt_scheduler_stack_.find(thread);
        if(stack==concrt_scheduler_stack_.end() || stack->second.empty()){throw_msvc_exception(state,memory,service,MsvcExceptionKind::ImproperSchedulerDetach);return std::nullopt;}
        const auto object=stack->second.back();stack->second.pop_back();if(stack->second.empty())concrt_scheduler_stack_.erase(stack);(void)release(object);return finish(0);
    }
    if(name.starts_with("?Get@CurrentScheduler@"))return finish(current());
    if(name.starts_with("?GetPolicy@")){const auto object=current();if(!write_policy(call(0),concrt_schedulers_.at(object).policy)){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return std::nullopt;}return finish(call(0));}
    if(name.starts_with("?CreateScheduleGroup@"))return finish(create_group(current()));
    if(name.starts_with("?RegisterShutdownEvent@")){const auto object=current();if(call(0))concrt_schedulers_.at(object).shutdown_events.push_back(call(0));return finish(0);}
    if(name.starts_with("?IsAvailableLocation@"))return finish(available(current(),call(0))?1:0);
    if(name.starts_with("?_Get@_CurrentScheduler@")){memory.check(GuestAddress{call(0)},unit,Access::Write);memory.store(GuestAddress{call(0)},bits,current());return finish(call(0));}
    if(name.starts_with("?CurrentContext@")){concrt_attached_.insert(thread);return finish(context_of(thread).object);}
    if(name.starts_with("?_CurrentContext@")){concrt_attached_.insert(thread);memory.check(GuestAddress{call(0)},unit,Access::Write);memory.store(GuestAddress{call(0)},bits,context_of(thread).object);return finish(call(0));}
    if(name.starts_with("?Block@")){
        // 先前已 Unblock 時直接返回；否則在這個 Context 的自動重設事件上等待 Unblock。
        concrt_attached_.insert(thread);auto& context=context_of(thread);if(context.unblocked){context.unblocked=false;return finish(0);}
        return nested_continuation(state,memory,"ConcRT Context::Block",[this,service,thread](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall frame(s,m,service);
            if(!concrt_contexts_[thread].event){const auto event=co_await service_callback::call_value(resolve("kernel32.dll","CreateEventW").value,{0,0,0,0});concrt_contexts_[thread].event=x86_?event&UINT32_MAX:event;}
            if(concrt_contexts_[thread].unblocked){concrt_contexts_[thread].unblocked=false;frame.return_to(s,0);co_return std::optional<std::uint32_t>{};}
            concrt_contexts_[thread].blocked=true;const auto event=concrt_contexts_[thread].event;
            (void)co_await service_callback::call(resolve("kernel32.dll","WaitForSingleObject").value,{event,UINT32_MAX});
            concrt_contexts_[thread].blocked=false;frame.return_to(s,0);co_return std::optional<std::uint32_t>{};
        });
    }
}
