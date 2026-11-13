// 畫面內插的呈現：生成畫面與真實畫面各占一個螢幕更新週期，由專用執行緒依序取得 drawable 並送出。
// 繪製執行緒只把兩張畫面複製到這裡的快照就返回，不等 drawable：CoreAnimation 的 drawable 池只有三個，
// 用過的 drawable 要過一個更新週期才回到池中，繪製執行緒自己取第二個 drawable 時會被擋住，等待時間吃掉下一幀的運算，
// 垂直同步下真實幀率會由每兩個週期一張掉到每三個週期一張（1080p 實測約一到兩成的幀）。
// 兩組快照：一組正在送出、一組可寫入；兩組都被占用時 prepare 等待（背壓），真實幀率因此不會超過更新率的一半。
class MetalInterpolationPresenter {
    enum class Use { Free, Reserved, Queued, Presenting };
    struct Slot {
        id<MTLTexture> generated,real;
        id<MTLCommandBuffer> ready;
        std::shared_ptr<GpuMemoryReservation> lease;
        macos_detail::MetalFrameTiming timing;
        bool has_generated{};
        Use use{Use::Free};
    };
    id<MTLCommandQueue> queue_;
    CAMetalLayer* layer_;
    std::array<Slot,2> slots_;
    std::mutex mutex_;
    std::condition_variable wake_,freed_;
    std::deque<unsigned> pending_;
    std::thread worker_;
    bool stopping_{};
    std::string failure_;
    unsigned width_{},height_{};

    // 複製一張快照到新的 drawable 並送出；回傳已提交的命令緩衝（快照在它完成前不可改寫）。
    id<MTLCommandBuffer> present(id<MTLTexture> source,const macos_detail::MetalFrameTiming& timing,bool real){
        auto drawable=[layer_ nextDrawable];
        if(!drawable)throw std::runtime_error("Metal 畫面內插 drawable 無法取得");
        auto command=[queue_ commandBuffer];auto encoder=[command blitCommandEncoder];
        if(!command || !encoder)throw std::runtime_error("Metal 畫面內插呈現命令建立失敗");
        MetalEncoderScope scope{encoder};
        [encoder copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width_,height_,1)
                       toTexture:drawable.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        scope.end();
        if(real)timing.present(drawable);
        else if(timing.recorder){
            // 生成畫面不屬於客體提交，另記「generated」事件（frame 為其後真實畫面的序號）；presentedTime 為 0 表示未上屏。
            const auto record=timing;
            [drawable addPresentedHandler:^(id<MTLDrawable> displayed){
                record.recorder->frame("generated",record.surface,record.frame,record.width,record.height,0,0,displayed.presentedTime);
            }];
        }
        [command presentDrawable:drawable];track_gpu_commands(command);[command commit];
        return command;
    }
    void run() noexcept {
        try {
            for(;;){
                unsigned index=0;id<MTLCommandBuffer> ready;macos_detail::MetalFrameTiming timing;bool has_generated=false;
                {
                    std::unique_lock lock(mutex_);
                    wake_.wait(lock,[&]{return stopping_ || !pending_.empty();});
                    if(pending_.empty())return;
                    index=pending_.front();pending_.pop_front();auto& slot=slots_[index];
                    slot.use=Use::Presenting;ready=slot.ready;timing=slot.timing;has_generated=slot.has_generated;
                }
                @autoreleasepool {
                    // ready 已提交，內含這一幀的繪製、內插與兩張快照的複製；等待發生在呈現執行緒，繪製執行緒已開始下一幀。
                    [ready waitUntilCompleted];
                    if(ready.status==MTLCommandBufferStatusError && !metal_innocent_victim(ready))throw std::runtime_error("Metal 畫面內插快照執行失敗");
                    id<MTLCommandBuffer> first=has_generated?present(slots_[index].generated,timing,false):nil;
                    id<MTLCommandBuffer> second=present(slots_[index].real,timing,true);
                    if(first)[first waitUntilCompleted];
                    [second waitUntilCompleted];
                    if((first && first.status==MTLCommandBufferStatusError && !metal_innocent_victim(first)) ||
                       (second.status==MTLCommandBufferStatusError && !metal_innocent_victim(second)))throw std::runtime_error("Metal 畫面內插呈現執行失敗");
                }
                {
                    std::lock_guard lock(mutex_);
                    slots_[index].ready=nil;slots_[index].use=Use::Free;freed_.notify_all();
                }
            }
        }catch(const std::exception& error){
            std::lock_guard lock(mutex_);failure_=error.what();stopping_=true;pending_.clear();wake_.notify_all();freed_.notify_all();
        }catch(...){
            std::lock_guard lock(mutex_);failure_="Metal 畫面內插呈現發生未知錯誤";stopping_=true;pending_.clear();wake_.notify_all();freed_.notify_all();
        }
    }
public:
    MetalInterpolationPresenter(id<MTLDevice> device,CAMetalLayer* layer,unsigned width,unsigned height,MTLPixelFormat format)
        :queue_([device newCommandQueue]),layer_(layer),width_(width),height_(height){
        if(!queue_)throw std::bad_alloc{};
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
        descriptor.storageMode=MTLStorageModePrivate;descriptor.usage=MTLTextureUsageShaderRead;
        for(auto& slot:slots_){
            slot.lease=reserve_gpu_memory(static_cast<std::size_t>(width)*height*8);
            slot.generated=[device newTextureWithDescriptor:descriptor];slot.real=[device newTextureWithDescriptor:descriptor];
            if(!slot.generated || !slot.real)throw std::bad_alloc{};
        }
        worker_=std::thread([this]{run();});
    }
    ~MetalInterpolationPresenter(){finish();}
    unsigned width()const noexcept{return width_;}
    unsigned height()const noexcept{return height_;}
    // 有空的快照組可寫入（不必等待）。
    bool ready(){
        std::lock_guard lock(mutex_);
        return stopping_ || std::any_of(slots_.begin(),slots_.end(),[](const auto& slot){return slot.use==Use::Free;});
    }
    // 取得可寫入的快照組；兩組都在送出時等待其中一組完成。
    unsigned prepare(){
        std::unique_lock lock(mutex_);
        for(;;){
            if(!failure_.empty())throw std::runtime_error(failure_);
            if(stopping_)throw std::runtime_error("Metal 畫面內插呈現佇列已停止");
            for(unsigned i=0;i<slots_.size();++i)if(slots_[i].use==Use::Free){slots_[i].use=Use::Reserved;return i;}
            freed_.wait(lock);
        }
    }
    id<MTLTexture> generated(unsigned index)const{return slots_[index].generated;}
    id<MTLTexture> real(unsigned index)const{return slots_[index].real;}
    void publish(unsigned index,id<MTLCommandBuffer> ready,const macos_detail::MetalFrameTiming& timing,bool has_generated){
        std::lock_guard lock(mutex_);
        // 即使呈現執行緒先失敗，也保留已提交的最後寫入，finish 才能等到 GPU 釋放快照。
        slots_[index].ready=ready;slots_[index].timing=timing;slots_[index].has_generated=has_generated;
        if(!failure_.empty())throw std::runtime_error(failure_);
        slots_[index].use=Use::Queued;pending_.push_back(index);wake_.notify_one();
    }
    void finish() noexcept {
        {
            std::lock_guard lock(mutex_);stopping_=true;wake_.notify_all();freed_.notify_all();
        }
        if(worker_.joinable())worker_.join();
        for(const auto& slot:slots_)if(slot.ready)[slot.ready waitUntilCompleted];
    }
};
