#pragma once
#import <QuartzCore/QuartzCore.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

namespace x64arm::macos_detail {
// 可選的實際呈現量測；每個程序分檔，回呼只保留 recorder，不保留視窗或 surface。
class GuiTiming final {
    enum class Kind : std::uint8_t { Submitted,Completed,Presented,Input };
    struct Event {
        Kind kind{};
        std::uint32_t width{},height{},message{};
        std::uint64_t surface{},frame{},hwnd{};
        double request{},submit{},display{},gpu_start{},gpu_end{},host{},wall{};
    };
    struct Slot {std::atomic<bool> ready{};Event event;};
    // 僅診斷啟用時配置；不能以環境設定放大。背景 I/O 不持有 producer 所需的鎖。
    static constexpr std::uint64_t capacity=4096;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);
    std::array<Slot,capacity> slots_;
    std::FILE* file_{};
    std::thread writer_;
    std::atomic<std::uint64_t> head_{},tail_{},wake_{},overflow_{},contention_{},invalid_{},errors_{};
    std::atomic<bool> stopping_{},available_{};
    std::atomic<std::uint64_t> next_surface_{1};
    std::uint64_t written_{};
    static double unix_time() {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    void enqueue(Event event) noexcept {
        if(!available_.load(std::memory_order_relaxed))return;
        auto position=head_.load(std::memory_order_relaxed);
        for(unsigned attempt=0;attempt<16;++attempt){
            if(position-tail_.load(std::memory_order_acquire)>=capacity){
                const auto current=head_.load(std::memory_order_acquire);
                if(current!=position){position=current;continue;}
                overflow_.fetch_add(1,std::memory_order_relaxed);return;
            }
            if(!head_.compare_exchange_weak(position,position+1,std::memory_order_acq_rel,std::memory_order_relaxed))continue;
            // 先保留票號再取時鐘。空佇列 checkpoint 的截止時間不包含尚未保留的新事件。
            event.host=CACurrentMediaTime();event.wall=unix_time();
            auto& slot=slots_[position%capacity];slot.event=event;
            slot.ready.store(true,std::memory_order_release);
            wake_.fetch_add(1,std::memory_order_release);wake_.notify_one();return;
        }
        contention_.fetch_add(1,std::memory_order_relaxed);
    }
    bool emit(const Event& e,std::uint64_t sequence) noexcept {
        if(errors_.load(std::memory_order_relaxed))return false;
        int result{};
        if(e.kind==Kind::Input){
            result=std::fprintf(file_,"{\"event\":\"input_queue\",\"sequence\":%llu,\"pid\":%d,\"hwnd\":%llu,"
                "\"message\":%u,\"event_time\":%.9f,\"unix_time\":%.9f}\n",
                static_cast<unsigned long long>(sequence),::getpid(),static_cast<unsigned long long>(e.hwnd),e.message,e.host,e.wall);
        }else{
            constexpr const char* names[]={"submitted","completed","presented"};
            result=std::fprintf(file_,"{\"event\":\"%s\",\"sequence\":%llu,\"pid\":%d,\"surface\":%llu,\"frame\":%llu,"
                "\"width\":%u,\"height\":%u,\"request_time\":%.9f,\"submit_time\":%.9f,"
                "\"event_time\":%.9f,\"presented_time\":%.9f,\"gpu_start\":%.9f,"
                "\"gpu_end\":%.9f,\"unix_time\":%.9f}\n",names[static_cast<unsigned>(e.kind)],
                static_cast<unsigned long long>(sequence),::getpid(),static_cast<unsigned long long>(e.surface),
                static_cast<unsigned long long>(e.frame),e.width,e.height,e.request,e.submit,e.host,e.display,e.gpu_start,e.gpu_end,e.wall);
        }
        if(result<0){
            if(!errors_.fetch_add(1,std::memory_order_relaxed))std::fputs("GUI 呈現診斷寫入失敗；量測不能宣稱完整。\n",stderr);
            return false;
        }
        ++written_;return true;
    }
    void marker(const char* event,double host,double wall) noexcept {
        const auto overflow=overflow_.load(std::memory_order_relaxed),contention=contention_.load(std::memory_order_relaxed),invalid=invalid_.load(std::memory_order_relaxed);
        if(std::fprintf(file_,"{\"event\":\"%s\",\"recording_schema\":1,\"pid\":%d,\"capacity_events\":%llu,"
            "\"storage_bytes\":%zu,\"accepted\":%llu,\"written\":%llu,\"dropped\":%llu,"
            "\"overflow\":%llu,\"contention\":%llu,\"invalid\":%llu,\"write_errors\":%llu,"
            "\"event_time\":%.9f,\"unix_time\":%.9f}\n",event,::getpid(),static_cast<unsigned long long>(capacity),sizeof(slots_),
            static_cast<unsigned long long>(head_.load(std::memory_order_acquire)),static_cast<unsigned long long>(written_),
            static_cast<unsigned long long>(overflow+contention+invalid),static_cast<unsigned long long>(overflow),
            static_cast<unsigned long long>(contention),static_cast<unsigned long long>(invalid),
            static_cast<unsigned long long>(errors_.load(std::memory_order_relaxed)),host,wall)<0 || std::fflush(file_)){
            if(!errors_.fetch_add(1,std::memory_order_relaxed))std::fputs("GUI 呈現診斷寫入失敗；量測不能宣稱完整。\n",stderr);
        }
    }
    void write_loop() noexcept {
        marker("timing_begin",CACurrentMediaTime(),unix_time());
        auto next=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        for(;;){
            const auto epoch=wake_.load(std::memory_order_acquire);
            auto position=tail_.load(std::memory_order_relaxed);bool consumed=false;
            while(slots_[position%capacity].ready.load(std::memory_order_acquire)){
                auto& slot=slots_[position%capacity];const auto event=slot.event;
                slot.ready.store(false,std::memory_order_release);
                tail_.store(++position,std::memory_order_release);
                emit(event,position);consumed=true;
            }
            const auto host=CACurrentMediaTime(),wall=unix_time();
            const bool empty=position==head_.load(std::memory_order_acquire);
            if(stopping_.load(std::memory_order_acquire) && empty){marker("timing_end",host,wall);return;}
            if(empty && std::chrono::steady_clock::now()>=next){
                // 每秒於空佇列發布完整前綴。檔案 I/O 即使停頓也只佔用 writer。
                marker("timing_checkpoint",host,wall);next=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            }
            if(!consumed)wake_.wait(epoch,std::memory_order_acquire);
        }
    }
public:
    struct Statistics {std::uint64_t capacity_events,storage_bytes,accepted,consumed,overflow,contention,invalid,write_errors;};
    Statistics statistics()const noexcept {
        return {capacity,sizeof(slots_),head_.load(std::memory_order_acquire),tail_.load(std::memory_order_acquire),
            overflow_.load(std::memory_order_acquire),contention_.load(std::memory_order_acquire),
            invalid_.load(std::memory_order_acquire),errors_.load(std::memory_order_acquire)};
    }
    explicit GuiTiming(const char* prefix) {
        const auto path=std::string(prefix)+"-"+std::to_string(::getpid())+".jsonl";
        file_=std::fopen(path.c_str(),"a");
        if(!file_)std::fprintf(stderr,"GUI 呈現量測檔無法開啟：%s\n",path.c_str());
        else{
            (void)::fcntl(::fileno(file_),F_SETFD,FD_CLOEXEC);
            try{writer_=std::thread([this]{write_loop();});available_.store(true,std::memory_order_release);}
            catch(...){std::fclose(file_);file_=nullptr;}
        }
    }
    ~GuiTiming(){
        available_.store(false,std::memory_order_release);stopping_.store(true,std::memory_order_release);
        wake_.fetch_add(1,std::memory_order_release);wake_.notify_one();
        if(writer_.joinable())writer_.join();
        if(file_ && std::fclose(file_))std::fputs("GUI 呈現診斷關閉失敗。\n",stderr);
    }
    bool valid()const{return available_.load(std::memory_order_acquire);}
    std::uint64_t surface(){return next_surface_.fetch_add(1,std::memory_order_relaxed);}
    void frame(const char* event,std::uint64_t surface,std::uint64_t frame,
               std::uint32_t width,std::uint32_t height,double request,double submit,
               double display=0,double gpu_start=0,double gpu_end=0) noexcept {
        Event value;
        if(event && !std::strcmp(event,"submitted"))value.kind=Kind::Submitted;
        else if(event && !std::strcmp(event,"completed"))value.kind=Kind::Completed;
        else if(event && !std::strcmp(event,"presented"))value.kind=Kind::Presented;
        else{invalid_.fetch_add(1,std::memory_order_relaxed);return;}
        value.surface=surface;value.frame=frame;value.width=width;value.height=height;
        value.request=request;value.submit=submit;value.display=display;value.gpu_start=gpu_start;value.gpu_end=gpu_end;
        enqueue(value);
    }
    void input(std::uint64_t hwnd,std::uint32_t message) noexcept {
        Event value;value.kind=Kind::Input;value.hwnd=hwnd;value.message=message;enqueue(value);
    }
};
inline const std::shared_ptr<GuiTiming>& gui_timing() {
    static const auto recorder=[]()->std::shared_ptr<GuiTiming>{
        const auto* prefix=std::getenv("X64ARM_GUI_TIMING_PREFIX");
        if(!prefix || !*prefix)return {};
        auto result=std::make_shared<GuiTiming>(prefix);
        return result->valid()?result:nullptr;
    }();
    return recorder;
}
}
