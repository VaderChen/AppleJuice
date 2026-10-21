// 原生資源及 CFRunLoop 全由同一工作執行緒建立、使用、釋放。
// 背景 QoS 是小核心排程提示，不是假定固定 CPU 編號的 affinity。
class InputWorker {
public:
    struct Snapshot {
        std::vector<Device> devices;
        std::vector<Gamepad> gamepads;
        std::vector<PointerDevice> pointers;
        bool observes_wheel{};
    };
private:
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{};
    std::deque<std::function<void(NativeInput&)>> commands_;
    std::vector<Event> events_;
    std::vector<Report> reports_;
    std::vector<PointerSample> pointers_;
    std::shared_ptr<const Snapshot> snapshot_;
    std::thread thread_;

    template<class T> static void append(std::vector<T>& target,std::vector<T> source,std::size_t limit){
        // 與原生佇列相同的有界 FIFO；保留已排隊事件的先後順序。
        const auto n=std::min(source.size(),limit-target.size());
        target.insert(target.end(),std::make_move_iterator(source.begin()),std::make_move_iterator(source.begin()+static_cast<typename std::vector<T>::difference_type>(n)));
    }
    void publish(NativeInput& input){
        auto events=input.poll();auto reports=input.reports();auto pointers=input.pointer_samples();
        auto next=std::make_shared<Snapshot>();next->devices=input.devices();next->gamepads=input.gamepads();
        next->pointers=input.pointer_devices();next->observes_wheel=input.observes_wheel();
        std::lock_guard lock(mutex_);
        append(events_,std::move(events),65536);append(reports_,std::move(reports),10000);append(pointers_,std::move(pointers),65536);
        snapshot_=std::move(next);
    }
public:
    InputWorker(){
        std::promise<void> ready;auto result=ready.get_future();
        thread_=std::thread([this,ready=std::move(ready)]()mutable{
            pthread_setname_np("x64arm.input.background");
            @autoreleasepool {
                std::unique_ptr<NativeInput> input;
                // 初始化由前景 API 同步等待；先沿用呼叫端 QoS 完成 HID／framework 載入，避免背景 I/O 節流造成優先權反轉。
                try{input=std::make_unique<NativeInput>();publish(*input);const auto error=pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND,0);if(error)throw std::system_error(error,std::generic_category(),"無法設定輸入執行緒背景 QoS");ready.set_value();}
                catch(...){ready.set_exception(std::current_exception());return;}
                auto next=std::chrono::steady_clock::now();
                for(;;){
                    std::deque<std::function<void(NativeInput&)>> commands;
                    {
                        std::unique_lock lock(mutex_);
                        wake_.wait_until(lock,next,[&]{return stopping_ || !commands_.empty();});
                        if(stopping_)break;
                        commands.swap(commands_);
                    }
                    @autoreleasepool {
                        for(auto& command:commands)command(*input);
                        const auto now=std::chrono::steady_clock::now();
                        if(now>=next){publish(*input);next=now+std::chrono::milliseconds(4);}
                    }
                }
                input.reset();
            }
        });
        try{result.get();}catch(...){thread_.join();throw;}
    }
    ~InputWorker(){
        {std::lock_guard lock(mutex_);stopping_=true;}
        wake_.notify_one();thread_.join();
    }
    template<class F> auto call(F&& f){
        using R=std::invoke_result_t<F,NativeInput&>;
        auto task=std::make_shared<std::packaged_task<R(NativeInput&)>>(std::forward<F>(f));
        auto result=task->get_future();
        {std::lock_guard lock(mutex_);commands_.emplace_back([task](NativeInput& input){(*task)(input);});}
        wake_.notify_one();return result.get();
    }
    std::shared_ptr<const Snapshot> snapshot()const{std::lock_guard lock(mutex_);return snapshot_;}
    std::vector<Event> poll(){std::lock_guard lock(mutex_);std::vector<Event> out;out.swap(events_);return out;}
    std::vector<Report> reports(){std::lock_guard lock(mutex_);std::vector<Report> out;out.swap(reports_);return out;}
    std::vector<PointerSample> pointers(){std::lock_guard lock(mutex_);std::vector<PointerSample> out;out.swap(pointers_);return out;}
};
// Effect 可比 Backend 長壽；共享工作執行緒，最後一個 effect 釋放後才停止。
class WorkerEffect final:public Effect {
    std::shared_ptr<InputWorker> worker_;
    std::shared_ptr<Effect> effect_;
public:
    WorkerEffect(std::shared_ptr<InputWorker> worker,std::shared_ptr<Effect> effect):worker_(std::move(worker)),effect_(std::move(effect)){}
    ~WorkerEffect()override{worker_->call([&](NativeInput&){effect_.reset();});}
    std::uint32_t set(const EffectParameters& p,std::uint32_t flags)override{return worker_->call([&](NativeInput&){return effect_->set(p,flags);});}
    std::uint32_t start(std::uint32_t n,std::uint32_t flags)override{return worker_->call([&](NativeInput&){return effect_->start(n,flags);});}
    std::uint32_t stop()override{return worker_->call([&](NativeInput&){return effect_->stop();});}
    std::uint32_t download()override{return worker_->call([&](NativeInput&){return effect_->download();});}
    std::uint32_t unload()override{return worker_->call([&](NativeInput&){return effect_->unload();});}
    std::uint32_t status(std::uint32_t& out)override{return worker_->call([&](NativeInput&){return effect_->status(out);});}
    std::uint32_t escape(std::uint32_t command,std::span<const std::uint8_t> in,std::vector<std::uint8_t>& out)override{return worker_->call([&](NativeInput&){return effect_->escape(command,in,out);});}
};
class WorkerInput final:public Backend {
    std::shared_ptr<InputWorker> worker_=std::make_shared<InputWorker>();
public:
    std::vector<Device> devices()override{return worker_->snapshot()->devices;}
    std::vector<Event> poll()override{return worker_->poll();}
    std::vector<Report> reports()override{return worker_->reports();}
    std::vector<Gamepad> gamepads()override{return worker_->snapshot()->gamepads;}
    std::vector<PointerDevice> pointer_devices()override{return worker_->snapshot()->pointers;}
    std::vector<PointerSample> pointer_samples()override{return worker_->pointers();}
    bool observes_wheel()const override{return worker_->snapshot()->observes_wheel;}
    bool available(const Guid& id)override{const auto state=worker_->snapshot();for(const auto& d:state->devices)if(d.instance==id)return d.attached;return false;}
    std::uint32_t rumble(const Guid& id,std::uint16_t left,std::uint16_t right)override{return worker_->call([&](NativeInput& input){return input.rumble(id,left,right);});}
    std::uint32_t set_report(const Guid& id,unsigned type,std::span<const std::uint8_t> bytes)override{return worker_->call([&](NativeInput& input){return input.set_report(id,type,bytes);});}
    std::uint32_t get_report(const Guid& id,unsigned type,std::span<std::uint8_t> bytes)override{return worker_->call([&](NativeInput& input){return input.get_report(id,type,bytes);});}
    std::uint32_t cooperative(const Guid& id,bool exclusive)override{return worker_->call([&](NativeInput& input){return input.cooperative(id,exclusive);});}
    void mouse_wheel(std::int32_t value)override{worker_->call([&](NativeInput& input){input.mouse_wheel(value);});}
    void horizontal_wheel(std::int32_t value)override{worker_->call([&](NativeInput& input){input.horizontal_wheel(value);});}
    void cursor_warped()override{worker_->call([&](NativeInput& input){input.cursor_warped();});}
    // AppKit 對話框留在 UI 呼叫端，不移到背景 run loop。
    std::uint32_t control_panel(const Guid& id)override{return NativeInput::control_panel_ui(id);}
    std::uint32_t configure(std::vector<Action>& actions,bool edit)override{return NativeInput::configure_ui(actions,edit);}
    std::uint32_t ff_property(const Guid& id,unsigned property,std::uint32_t& data,bool set)override{return worker_->call([&](NativeInput& input){return input.ff_property(id,property,data,set);});}
    std::uint32_t ff_state(const Guid& id,std::uint32_t& out)override{return worker_->call([&](NativeInput& input){return input.ff_state(id,out);});}
    std::uint32_t ff_command(const Guid& id,std::uint32_t command)override{return worker_->call([&](NativeInput& input){return input.ff_command(id,command);});}
    std::uint32_t escape(const Guid& id,std::uint32_t command,std::span<const std::uint8_t> in,std::vector<std::uint8_t>& out)override{return worker_->call([&](NativeInput& input){return input.escape(id,command,in,out);});}
    std::shared_ptr<Effect> effect(const Guid& id,unsigned type,const EffectParameters* p,std::uint32_t& error)override{
        auto native=worker_->call([&](NativeInput& input){return input.effect(id,type,p,error);});
        return native?std::make_shared<WorkerEffect>(worker_,std::move(native)):nullptr;
    }
};
