// D3D11 立即 context 的 Metal 命令執行緒：繪製、運算、寫入、複製與清除依序排入專用宿主執行緒，客體執行緒
// 不必等 Metal 編碼完成即可繼續處理下一批狀態與客體程式碼。其餘方法（讀回、建立資源與著色器、fence、
// 時間戳、呈現與交接、提交）先等佇列清空再於呼叫端執行緒同步執行，因此同一時間只有一個執行緒操作底層
// 後端，後端本身不需要額外的鎖。背景執行緒的例外延到下一次同步時在呼叫端拋出。
// graphics_*_retained 只保留參照：呼叫端保證參照內容在下一次 graphics_sync() 前不變（D3D11 繪製狀態快取）。
// 依序完成時設定的 fence 中繼旗標：命令執行緒處理到 fence 時才掛到當下 command buffer 的完成回呼。
struct MetalFenceRelays {
    std::mutex mutex;bool fired{};std::vector<std::shared_ptr<std::atomic<bool>>> items;
    void fire(){std::lock_guard lock(mutex);fired=true;for(auto& item:items)item->store(true,std::memory_order_release);items.clear();}
    void attach(std::shared_ptr<std::atomic<bool>> relay){std::lock_guard lock(mutex);if(fired)relay->store(true,std::memory_order_release);else items.push_back(std::move(relay));}
};
void metal_fence_relay(NativeOpenGlContext&,std::shared_ptr<std::atomic<bool>>,bool flush);
class MetalCommandThread final:public NativeOpenGlContext {
public:
    explicit MetalCommandThread(std::shared_ptr<NativeOpenGlContext> inner):inner_(std::move(inner)) {
        worker_=std::thread([this]{run();});
    }
    ~MetalCommandThread() override {
        stop_requested_.store(true,std::memory_order_release);
        {std::lock_guard lock(mutex_);stopping_=true;}
        cv_.notify_all();worker_.join();
    }
    NativeOpenGlContext& inner() noexcept {return *inner_;}
    std::uint64_t graphics_ticket()const override {return queued_.load(std::memory_order_acquire);}
    void graphics_sync() override {graphics_sync_until(queued_.load(std::memory_order_acquire));}
    void graphics_sync_until(std::uint64_t ticket) override {
        if(on_worker())return;
        const auto target=std::min(ticket,queued_.load(std::memory_order_acquire));
        if(completed_.load(std::memory_order_acquire)<target){
            // 統計：需要等待命令執行緒的同步次數與等待時間（隨 statistics 輸出，供判斷客體端是否被編碼速度卡住）。
            const auto wait_begin=std::chrono::steady_clock::now();
            struct WaitStat {std::atomic<std::uint64_t>& count;std::atomic<std::uint64_t>& ns;std::chrono::steady_clock::time_point begin;
                ~WaitStat(){count.fetch_add(1,std::memory_order_relaxed);
                    ns.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count()),std::memory_order_relaxed);}
            } wait_stat{sync_waits_,sync_wait_ns_,wait_begin};
            // 繪製編碼通常數微秒內完成：先短暫自旋，避免每次同步都經 futex。
            for(unsigned i=0;i<20000 && completed_.load(std::memory_order_acquire)<target;++i)
#if defined(__aarch64__)
                __builtin_arm_yield();
#else
                ;
#endif
            if(completed_.load(std::memory_order_acquire)<target){
                std::unique_lock lock(mutex_);waiting_=true;
                idle_cv_.wait(lock,[&]{return completed_.load(std::memory_order_acquire)>=target;});
                waiting_=false;
            }
        }
        if(failed_.load(std::memory_order_acquire)){
            std::exception_ptr error;{std::lock_guard lock(mutex_);error=std::exchange(error_,nullptr);failed_.store(false,std::memory_order_release);}
            if(error)std::rethrow_exception(error);
        }
    }
    // 非同步：只依呼叫當下的參數，資料在排入時複製。
    void graphics_write(const GraphicsView& view,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image) override {
        post([this,view,bytes=std::vector<std::uint8_t>(data.begin(),data.end()),row,image]{inner_->graphics_write(view,bytes,row,image);});
    }
    void graphics_write_discard(const GraphicsView& view,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image) override {
        post([this,view,bytes=std::vector<std::uint8_t>(data.begin(),data.end()),row,image]{inner_->graphics_write_discard(view,bytes,row,image);});
    }
    void graphics_write_no_overwrite(const GraphicsView& view,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image) override {
        post([this,view,bytes=std::vector<std::uint8_t>(data.begin(),data.end()),row,image]{inner_->graphics_write_no_overwrite(view,bytes,row,image);});
    }
    void graphics_write_region(const GraphicsView& view,std::span<const std::uint8_t> data,std::uint32_t row,std::uint32_t image,std::array<std::uint32_t,6> region) override {
        post([this,view,bytes=std::vector<std::uint8_t>(data.begin(),data.end()),row,image,region]{inner_->graphics_write_region(view,bytes,row,image,region);});
    }
    void graphics_copy(const GraphicsView& target,const GraphicsView& source,std::array<std::uint32_t,3> offset,std::array<std::uint32_t,6> box) override {
        post([this,target,source,offset,box]{inner_->graphics_copy(target,source,offset,box);});
    }
    void graphics_resolve(const GraphicsView& target,const GraphicsView& source,std::uint32_t format) override {
        post([this,target,source,format]{inner_->graphics_resolve(target,source,format);});
    }
    void graphics_clear(const GraphicsView& view,std::array<float,4> color,std::uint32_t flags) override {
        post([this,view,color,flags]{inner_->graphics_clear(view,color,flags);});
    }
    void graphics_mips(const GraphicsView& view) override {post([this,view]{inner_->graphics_mips(view);});}
    void graphics_draw(const GraphicsDraw& draw) override {
        auto copy=std::make_shared<const GraphicsDraw>(draw);post([this,copy]{inner_->graphics_draw(*copy);});
    }
    void graphics_draw_retained(const GraphicsDraw& draw) override {post([this,&draw]{inner_->graphics_draw(draw);});}
    void graphics_dispatch(const GraphicsStage& stage,std::array<std::uint32_t,3> groups) override {
        auto copy=std::make_shared<const GraphicsStage>(stage);post([this,copy,groups]{inner_->graphics_dispatch(*copy,groups);});
    }
    void graphics_dispatch_retained(const GraphicsStage& stage,std::array<std::uint32_t,3> groups) override {
        post([this,&stage,groups]{inner_->graphics_dispatch(stage,groups);});
    }
    void graphics_dispatch_indirect(const GraphicsStage& stage,const std::shared_ptr<GraphicsResource>& arguments,std::uint32_t offset) override {
        auto copy=std::make_shared<const GraphicsStage>(stage);post([this,copy,arguments,offset]{inner_->graphics_dispatch_indirect(*copy,arguments,offset);});
    }
    // 其餘方法：先等佇列清空，再同步轉交。
    bool fixed_vertex_supported()const noexcept override {return inner_->fixed_vertex_supported();}
    void graphics_fixed_targets(const GraphicsView& color,const GraphicsView& depth) override {graphics_sync();inner_->graphics_fixed_targets(color,depth);}
    bool graphics_fixed_texture(const GraphicsView& view) override {graphics_sync();return inner_->graphics_fixed_texture(view);}
    void graphics_fixed_stencil(const std::array<std::uint32_t,12>& state) override {graphics_sync();inner_->graphics_fixed_stencil(state);}
    void graphics_fixed_draw(const FixedGraphicsDraw& draw) override {graphics_sync();inner_->graphics_fixed_draw(draw);}
    std::shared_ptr<GraphicsResource> graphics_resource(const GraphicsResourceDesc& desc) override {graphics_sync();return inner_->graphics_resource(desc);}
    bool graphics_samples_supported(std::uint32_t format,std::uint32_t samples) override {graphics_sync();return inner_->graphics_samples_supported(format,samples);}
    bool graphics_vertex_format_supported(std::uint32_t format) override {graphics_sync();return inner_->graphics_vertex_format_supported(format);}
    std::shared_ptr<GraphicsShader> graphics_shader(std::uint32_t stage,std::span<const std::uint8_t> bytes) override {graphics_sync();return inner_->graphics_shader(stage,bytes);}
    std::shared_ptr<GraphicsShader> graphics_compute_source(const GraphicsModernShader& shader) override {graphics_sync();return inner_->graphics_compute_source(shader);}
    std::shared_ptr<GraphicsSampler> graphics_sampler(std::span<const std::uint8_t> desc) override {graphics_sync();return inner_->graphics_sampler(desc);}
    bool graphics_timestamp_supported()const override {return inner_->graphics_timestamp_supported();}
    GraphicsTimestamp graphics_timestamp() override {graphics_sync();return inner_->graphics_timestamp();}
    std::shared_ptr<const GraphicsTimestampFuture> graphics_timestamp_future() override {graphics_sync();return inner_->graphics_timestamp_future();}
    std::vector<std::uint8_t> graphics_read(const GraphicsView& view) override {graphics_sync();return inner_->graphics_read(view);}
    bool graphics_descriptor(const GraphicsView& view,std::uint32_t type,std::array<std::uint64_t,2>& value,std::shared_ptr<void>& keep) override {graphics_sync();return inner_->graphics_descriptor(view,type,value,keep);}
    // 已完成範圍的 Shared 緩衝不必等命令執行緒排空；其他資源仍同步後讀回。
    std::vector<std::uint8_t> graphics_read_completed(const GraphicsView& view) override {
        if(metal_completed_readable(view.resource))return inner_->graphics_read_completed(view);
        graphics_sync();return inner_->graphics_read(view);
    }
    bool graphics_clear_unordered(const GraphicsView& view,std::array<std::uint32_t,4> value,bool floating) override {graphics_sync();return inner_->graphics_clear_unordered(view,value,floating);}
    void graphics_export_frame(const GraphicsView& view,void* destination,std::size_t size,std::uint32_t pitch,std::shared_ptr<void> owner) override {
        graphics_sync();inner_->graphics_export_frame(view,destination,size,pitch,std::move(owner));
    }
    void graphics_present(const GraphicsView& view) override {graphics_sync();inner_->graphics_present(view);}
    void graphics_handoff(NativeGraphics& consumer,const GraphicsView& view) override {
        graphics_sync();NativeGraphics* target=&consumer;
        if(auto* proxy=dynamic_cast<MetalCommandThread*>(target)){proxy->graphics_sync();target=&proxy->inner();}
        inner_->graphics_handoff(*target,view);
    }
    // 查詢 End 每幀上百次：不等佇列清空，回傳中繼旗標，由命令執行緒依序掛到當下的 command buffer。
    std::shared_ptr<const std::atomic<bool>> graphics_fence() override {
        auto relay=std::make_shared<std::atomic<bool>>(false);post([this,relay]{metal_fence_relay(*inner_,relay,true);});return relay;
    }
    std::shared_ptr<const std::atomic<bool>> graphics_fence_pending() override {
        auto relay=std::make_shared<std::atomic<bool>>(false);post([this,relay]{metal_fence_relay(*inner_,relay,false);});return relay;
    }
    void graphics_flush(bool wait) override {graphics_sync();inner_->graphics_flush(wait);}
    OpenGlShareResult share_objects(NativeOpenGlContext& source) override {
        graphics_sync();NativeOpenGlContext* target=&source;
        if(auto* proxy=dynamic_cast<MetalCommandThread*>(target)){proxy->graphics_sync();target=&proxy->inner();}
        return inner_->share_objects(*target);
    }
    bool programmable()const noexcept override {return inner_->programmable();}
    std::shared_ptr<OpenGlBuffer> buffer_object(std::uint32_t name) override {graphics_sync();return inner_->buffer_object(name);}
    std::shared_ptr<OpenGlBuffer> bound_buffer(std::uint32_t target) override {graphics_sync();return inner_->bound_buffer(target);}
    void bind() override {graphics_sync();inner_->bind();}
    void drawable(void* view,std::uint32_t width,std::uint32_t height) override {graphics_sync();inner_->drawable(view,width,height);}
    bool swap() override {graphics_sync();return inner_->swap();}
    bool swap_interval(std::int32_t interval) override {graphics_sync();return inner_->swap_interval(interval);}
    std::uint64_t execute(GlOperation operation,std::span<const std::uint64_t> args,std::span<const std::uint8_t> data,std::span<std::uint8_t> output) override {
        graphics_sync();return inner_->execute(operation,args,data,output);
    }
    void array(unsigned index,std::int32_t size,std::uint32_t type,std::span<const std::uint8_t> bytes,bool enabled) override {graphics_sync();inner_->array(index,size,type,bytes,enabled);}
    std::string string(std::uint32_t name) override {graphics_sync();return inner_->string(name);}
    OpenGlRendererInfo info()const override {const_cast<MetalCommandThread*>(this)->graphics_sync();return inner_->info();}
    std::string statistics()const override {
        const_cast<MetalCommandThread*>(this)->graphics_sync();
        // 內層字串以換行結尾；欄位接在同一行，換行放回最後。
        auto text=inner_->statistics();while(!text.empty() && (text.back()=='\n' || text.back()==' '))text.pop_back();
        return text+" metal_command_sync_waits="+std::to_string(sync_waits_.load(std::memory_order_relaxed))+
            " metal_command_sync_wait_ns="+std::to_string(sync_wait_ns_.load(std::memory_order_relaxed))+"\n";
    }
    std::string capture(const std::filesystem::path& path) override {graphics_sync();return inner_->capture(path);}
    GraphicsView graphics_drawable(bool depth) override {graphics_sync();return inner_->graphics_drawable(depth);}
private:
    static bool& on_worker() noexcept {static thread_local bool value=false;return value;}
    template<class Task> void post(Task&& task) {
        if(on_worker()){task();return;}
        bool wake=false;
        {std::lock_guard lock(mutex_);queue_.emplace_back(std::forward<Task>(task));queued_.store(++submitted_,std::memory_order_release);wake=sleeping_;}
        if(wake)cv_.notify_one();
    }
    void run() {
        on_worker()=true;
#if defined(__APPLE__)
        pthread_setname_np("x64arm.metal.commands");
#endif
        for(;;){
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                // 同成品診斷對照專用；正常執行不輪詢空佇列。
                if(legacy_idle_spin_ && queue_.empty()){
                    lock.unlock();
                    for(unsigned i=0;i<200000 && queued_.load(std::memory_order_acquire)==taken_ && !stop_requested_.load(std::memory_order_acquire);++i)
#if defined(__aarch64__)
                        __builtin_arm_yield();
#else
                        ;
#endif
                    lock.lock();
                }
                // 有時限的閒置自旋：繪製之間客體工作通常只有數微秒到十幾微秒，佇列一空就睡會讓下一次 post 付出
                // notify 系統呼叫（DQ3 主執行緒約 2%），命令執行緒也要再等喚醒延遲。只看已提交序號、不取鎖；
                // 逾時仍回到下方的條件變數等待。X64ARM_METAL_COMMAND_SPIN_US=0 關閉（預設 20 µs）。
                if(idle_spin_ns_ && !legacy_idle_spin_ && queue_.empty() && !stopping_){
                    lock.unlock();
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::nanoseconds(idle_spin_ns_);
                    for(unsigned i=1;queued_.load(std::memory_order_acquire)==taken_ && !stop_requested_.load(std::memory_order_acquire);++i){
#if defined(__aarch64__)
                        __builtin_arm_yield();
#endif
                        if(!(i&63U) && std::chrono::steady_clock::now()>=deadline)break;
                    }
                    lock.lock();
                }
                // 佇列檢查、睡眠標記與 post 的喚醒判定共用 mutex，避免遺失通知。
                // 空佇列直接等待；停止時仍依序完成已排入的命令。
                while(queue_.empty() && !stopping_){sleeping_=true;cv_.wait(lock);sleeping_=false;}
                if(queue_.empty())return;
                task=std::move(queue_.front());queue_.pop_front();++taken_;
            }
            try{task();}
            catch(...){std::lock_guard lock(mutex_);if(!error_)error_=std::current_exception();failed_.store(true,std::memory_order_release);}
            task=nullptr;
            completed_.fetch_add(1,std::memory_order_acq_rel);
            {std::lock_guard lock(mutex_);if(waiting_)idle_cv_.notify_all();}
        }
    }
    std::shared_ptr<NativeOpenGlContext> inner_;
    std::mutex mutex_;std::condition_variable cv_,idle_cv_;
    std::deque<std::function<void()>> queue_;
    const bool legacy_idle_spin_=[] {const auto* value=std::getenv("X64ARM_METAL_LEGACY_IDLE_SPIN");return value && std::string_view(value)=="1";}();
    const std::int64_t idle_spin_ns_=[] {const auto* value=std::getenv("X64ARM_METAL_COMMAND_SPIN_US");
        if(!value || !*value)return std::int64_t{20000};const auto us=std::strtoll(value,nullptr,10);return us<0?std::int64_t{0}:std::min<std::int64_t>(us,1000)*1000;}();
    std::uint64_t submitted_{},taken_{};
    std::atomic<std::uint64_t> completed_{},queued_{},sync_waits_{},sync_wait_ns_{};
    std::atomic<bool> failed_{},stop_requested_{};
    bool stopping_{},sleeping_{},waiting_{};
    std::exception_ptr error_;
    std::thread worker_;
};
