// 每個服務實例只有一個工作執行緒及一個工作格位；沒有逐次增長的工作佇列。
// worker 只執行 RegistryStore 提供的私有 I/O 工作，不操作客體記憶體或 live RegistryState。
struct WindowsServices::RegistryIoWorker {
    std::mutex mutex;
    std::condition_variable changed;
    std::function<void()> pending;
    std::shared_ptr<SharedWake> wake;
    std::exception_ptr failure;
    std::atomic<bool> done{true};
    bool stopping{};
    std::thread worker;
    RegistryIoWorker():worker([this]{
        std::unique_lock lock(mutex);
        for(;;){
            changed.wait(lock,[&]{return stopping || bool(pending);});
            if(!pending && stopping)return;
            auto work=std::exchange(pending,{});auto notification=wake;lock.unlock();
            std::exception_ptr error;try{work();}catch(...){error=std::current_exception();}
            lock.lock();failure=std::move(error);
            done.store(true,std::memory_order_release);
            changed.notify_all();lock.unlock();
            if(notification)notification->notify();
            lock.lock();
        }
    }){}
    ~RegistryIoWorker(){
        {std::lock_guard lock(mutex);stopping=true;}
        changed.notify_all();worker.join();
    }
    void submit(const std::function<void()>& work,std::shared_ptr<SharedWake> notification){
        std::lock_guard lock(mutex);
        if(!done.load(std::memory_order_acquire) || pending)throw HostCallFault("登錄 I/O 格位仍在使用");
        pending=work;wake=std::move(notification);failure={};
        done.store(false,std::memory_order_release);changed.notify_one();
    }
    void drain(){std::unique_lock lock(mutex);changed.wait(lock,[&]{return done.load(std::memory_order_acquire);});}
};

bool WindowsServices::registry_io_ready() const {
    return registry_io_worker_ && registry_io_worker_->done.load(std::memory_order_acquire);
}

void WindowsServices::await_registry_io(GuestState& state,GuestMemory& memory,const std::function<void()>& work){
    const auto trace=[&](const char* phase){
        static const bool enabled=std::getenv("X64ARM_TRACE_REGISTRY_IO")!=nullptr;
        if(!enabled)return;
        static std::atomic<unsigned> emitted{};
        auto count=emitted.load(std::memory_order_relaxed);
        while(count<64 && !emitted.compare_exchange_weak(count,count+1,std::memory_order_relaxed)){}
        // 正常流程至多保留 64 筆；重入只在終止點輸出，不累積逐筆永久歷史。
        if(count>=64 && !registry_io_busy_)return;
        if(count>=64 && std::string_view(phase)!="enter")return;
        std::fprintf(stderr,"[registry-io] phase=%s pid=%llu current=%llu owner=%llu busy=%u ready=%u saved=%u rip=%llx callbacks=%zu\n",
            phase,static_cast<unsigned long long>(process_id()),static_cast<unsigned long long>(threads_.current),
            static_cast<unsigned long long>(registry_io_owner_),registry_io_busy_?1U:0U,registry_io_ready()?1U:0U,
            threads_.context_saved?1U:0U,static_cast<unsigned long long>(state.rip.value),runtime_.callbacks.size());
    };
    trace("enter");
    if(registry_io_busy_)throw HostCallFault("登錄 I/O 不得在未完成的交易內重入");
    // 初始化、沒有客體排程器的獨立單元測試仍使用原本的同步契約。
    if(!has_nested_runner() || threads_.items.empty() || threads_.context_saved ||
       threads_.items.at(threads_.current).status!=ThreadStatus::Runnable){work();return;}
    if(!registry_io_worker_){
        try{registry_io_worker_=std::make_shared<RegistryIoWorker>();}
        catch(const std::system_error&){throw std::bad_alloc();}
    }
    const auto saved=state;const auto owner=threads_.current;
    const auto saved_error=last_error_;const auto saved_wait=threads_.items.at(owner).wait;
    auto worker=registry_io_worker_;
    process_io::Counters io_counters;
    worker->submit([&]{process_io::Scope accounting(io_counters);work();},mapping_wake());
    registry_io_busy_=true;registry_io_owner_=owner;
    struct Finish {
        WindowsServices& self;std::shared_ptr<RegistryIoWorker> worker;process_io::Counters& counters;
        ~Finish(){
            // 例外／停止也先等私有 I/O 完成，才讓其借用的 C++ 區域資料離開作用域。
            worker->drain();process_io::record(counters);
            self.registry_io_busy_=false;self.registry_io_owner_=0;
            self.notify_concurrency_changed();
        }
    } finish{*this,worker,io_counters};
    auto& thread=threads_.items.at(owner);thread.status=ThreadStatus::Waiting;thread.wait={};
    thread.wait.kind=ThreadWaitKind::RegistryIo;
    // 此階段可能已切到其他 guest；不能讓基礎設施例外落入外層 Reg API
    // 的可恢復錯誤分支，以免在另一個 guest 的堆疊上返回登錄結果。
    const auto stop=[&](StopReason reason,const char* message,const MemoryFault* fault=nullptr){
        ExecutionResult result;result.reason=reason;result.rip=state.rip;
        if(fault){result.fault_address=fault->address;result.fault_access=fault->access;result.fault_size=fault->size;}
        // OOM 時診斷字串也可能無法配置；保留停止原因及 fault 欄位即可。
        try{result.message=message;}catch(const std::bad_alloc&){}
        throw NestedExecutionStop(std::move(result));
    };
    ExecutionResult nested;
    try{
        trace("park");
        schedule(state,memory);
        trace("scheduled");
        nested=run_nested(state,memory,[&]{return threads_.current==owner;});
        trace("resumed");
    }catch(const NestedExecutionStop&){throw;}
    catch(const MemoryFault& fault){stop(StopReason::MemoryFault,fault.what(),&fault);}
    catch(const std::bad_alloc&){stop(StopReason::InternalError,"登錄 I/O 等待期間記憶體不足");}
    catch(const HostCallFault& error){stop(StopReason::UnsupportedHostCall,error.what());}
    catch(const std::exception& error){stop(StopReason::InternalError,error.what());}
    catch(...){stop(StopReason::InternalError,"登錄 I/O 等待期間發生未知例外");}
    if(nested.reason!=StopReason::Returned)throw NestedExecutionStop(std::move(nested));
    if(const auto code=runtime_.forced_exit_code()){
        ExecutionResult stopped;stopped.reason=StopReason::Exited;stopped.exit_code=*code;stopped.rip=state.rip;
        throw NestedExecutionStop(std::move(stopped));
    }
    // Nested runner 的正常 sentinel 也回 Returned；必須保留原結束原因，不能改成服務故障。
    if(threads_.current!=owner)throw NestedExecutionStop(std::move(nested));
    if(!registry_io_ready())
        throw HostCallFault("登錄 I/O 等待期間客體執行提前停止："+nested.message);
    const auto elapsed=state.tsc;state=saved;state.tsc=elapsed;last_error_=saved_error;
    threads_.items.at(owner).wait=saved_wait;
    if(worker->failure)std::rethrow_exception(worker->failure);
}
