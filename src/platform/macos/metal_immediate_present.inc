// 非同步呈現只適用 interval=0：GPU 仍產生每一幀，螢幕取得 drawable 時採最新快照。
// 三個有額度的快照保留 GPU／呈現讀取生命週期，不讓 CoreAnimation 的 drawable 池阻塞客體。
class MetalImmediatePresenter {
    enum class Use { Free, Reserved, Pending, Reading };
    struct Slot {
        id<MTLTexture> texture;
        id<MTLCommandBuffer> ready;
        std::shared_ptr<GpuMemoryReservation> lease;
        std::uint64_t sequence{};
        Use use{Use::Free};
    };
    id<MTLCommandQueue> queue_;
    CAMetalLayer* layer_;
    std::array<Slot,3> slots_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<unsigned> pending_;
    std::thread worker_;
    bool stopping_{};
    std::string failure_;
    std::uint64_t submitted_{},displayed_{},coalesced_{},last_displayed_{};
    unsigned width_{},height_{};

    void run() noexcept {
        try {
            for(;;){
                {
                    std::unique_lock lock(mutex_);
                    wake_.wait(lock,[&]{return stopping_ || pending_.has_value();});
                    if(!pending_)return;
                }
                @autoreleasepool {
                    // 等待 drawable 時不持有 mutex；客體可以繼續更新最新快照。
                    auto drawable=[layer_ nextDrawable];
                    if(!drawable)throw std::runtime_error("Metal 非同步 drawable 無法取得");
                    unsigned index=0;std::uint64_t sequence=0;id<MTLCommandBuffer> ready;
                    {
                        std::unique_lock lock(mutex_);
                        wake_.wait(lock,[&]{return stopping_ || pending_.has_value();});
                        if(!pending_)return;
                        index=*pending_;pending_.reset();slots_[index].use=Use::Reading;
                        ready=slots_[index].ready;
                        sequence=slots_[index].sequence;
                    }
                    // ready 已 commit，且包含 producer 的 GPU 交接及畫面快照複製。
                    // 此等待發生於呈現執行緒；客體下一幀不用等螢幕更新。
                    [ready waitUntilCompleted];
                    if(ready.status==MTLCommandBufferStatusError)throw std::runtime_error("Metal 呈現快照執行失敗");
                    auto command=[queue_ commandBuffer];
                    auto encoder=[command blitCommandEncoder];
                    if(!command || !encoder)throw std::runtime_error("Metal 呈現複製命令建立失敗");
                    MetalEncoderScope scope{encoder};
                    [encoder copyFromTexture:slots_[index].texture sourceSlice:0 sourceLevel:0
                              sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width_,height_,1)
                                 toTexture:drawable.texture destinationSlice:0 destinationLevel:0
                         destinationOrigin:MTLOriginMake(0,0,0)];
                    scope.end();[command presentDrawable:drawable];track_gpu_commands(command);
                    [command commit];[command waitUntilCompleted];
                    if(command.status==MTLCommandBufferStatusError)throw std::runtime_error("Metal 非同步呈現執行失敗");
                    {
                        std::lock_guard lock(mutex_);
                        slots_[index].ready=nil;slots_[index].use=Use::Free;++displayed_;last_displayed_=sequence;
                    }
                }
            }
        }catch(const std::exception& error){
            std::lock_guard lock(mutex_);failure_=error.what();stopping_=true;pending_.reset();wake_.notify_all();
        }catch(...){
            std::lock_guard lock(mutex_);failure_="Metal 非同步呈現發生未知錯誤";stopping_=true;pending_.reset();wake_.notify_all();
        }
    }
public:
    MetalImmediatePresenter(id<MTLDevice> device,CAMetalLayer* layer,unsigned width,unsigned height)
        :queue_([device newCommandQueue]),layer_(layer),width_(width),height_(height){
        if(!queue_)throw std::bad_alloc{};
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                      width:width height:height mipmapped:NO];
        descriptor.storageMode=MTLStorageModePrivate;descriptor.usage=MTLTextureUsageShaderRead;
        for(auto& slot:slots_){
            slot.lease=reserve_gpu_memory(static_cast<std::size_t>(width)*height*4);
            slot.texture=[device newTextureWithDescriptor:descriptor];
            if(!slot.texture)throw std::bad_alloc{};
        }
        worker_=std::thread([this]{run();});
    }
    ~MetalImmediatePresenter(){finish();}
    unsigned prepare(){
        std::lock_guard lock(mutex_);
        if(!failure_.empty())throw std::runtime_error(failure_);
        if(stopping_)throw std::runtime_error("Metal 呈現佇列已停止");
        if(pending_){
            const auto index=*pending_;pending_.reset();slots_[index].ready=nil;
            slots_[index].use=Use::Reserved;++coalesced_;return index;
        }
        for(unsigned i=0;i<slots_.size();++i)if(slots_[i].use==Use::Free){slots_[i].use=Use::Reserved;return i;}
        throw std::runtime_error("Metal 呈現快照生命週期不一致");
    }
    id<MTLTexture> texture(unsigned index)const{return slots_[index].texture;}
    void publish(unsigned index,id<MTLCommandBuffer> ready){
        std::lock_guard lock(mutex_);
        // 即使呈現執行緒先失敗，也保留已提交的最後寫入，finish 才能等到 GPU 釋放快照。
        slots_[index].ready=ready;
        if(!failure_.empty())throw std::runtime_error(failure_);
        slots_[index].use=Use::Pending;slots_[index].sequence=++submitted_;pending_=index;wake_.notify_one();
    }
    void finish() noexcept {
        {
            std::lock_guard lock(mutex_);stopping_=true;wake_.notify_all();
        }
        if(worker_.joinable())worker_.join();
        for(const auto& slot:slots_)if(slot.ready)[slot.ready waitUntilCompleted];
    }
    void report()const{
        // 僅於 finish／join 完成後讀取，避免和工作執行緒交錯。
        std::fprintf(stderr,"[metal-immediate-present] submitted=%llu displayed=%llu coalesced=%llu last=%llu failed=%u\n",
                     static_cast<unsigned long long>(submitted_),static_cast<unsigned long long>(displayed_),
                     static_cast<unsigned long long>(coalesced_),static_cast<unsigned long long>(last_displayed_),failure_.empty()?0U:1U);
        if(!failure_.empty())std::fprintf(stderr,"[metal-immediate-present-error] %s\n",failure_.c_str());
    }
};
