#pragma once
#import <QuartzCore/QuartzCore.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <fcntl.h>
#include <unistd.h>

namespace x64arm::macos_detail {
// 可選的實際呈現量測；每個程序分檔，回呼只保留 recorder，不保留視窗或 surface。
class GuiTiming final {
    std::FILE* file_{};
    std::mutex mutex_;
    std::atomic<std::uint64_t> next_surface_{1};
    static double unix_time() {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
public:
    explicit GuiTiming(const char* prefix) {
        const auto path=std::string(prefix)+"-"+std::to_string(::getpid())+".jsonl";
        file_=std::fopen(path.c_str(),"a");
        if(!file_)std::fprintf(stderr,"GUI 呈現量測檔無法開啟：%s\n",path.c_str());
        else (void)::fcntl(::fileno(file_),F_SETFD,FD_CLOEXEC);
    }
    ~GuiTiming(){if(file_)std::fclose(file_);}
    bool valid()const{return file_!=nullptr;}
    std::uint64_t surface(){return next_surface_.fetch_add(1,std::memory_order_relaxed);}
    void frame(const char* event,std::uint64_t surface,std::uint64_t frame,
               std::uint32_t width,std::uint32_t height,double request,double submit,
               double display=0,double gpu_start=0,double gpu_end=0) {
        const auto now=CACurrentMediaTime(),wall=unix_time();
        std::lock_guard lock(mutex_);
        std::fprintf(file_,"{\"event\":\"%s\",\"pid\":%d,\"surface\":%llu,\"frame\":%llu,"
                     "\"width\":%u,\"height\":%u,\"request_time\":%.9f,\"submit_time\":%.9f,"
                     "\"event_time\":%.9f,\"presented_time\":%.9f,\"gpu_start\":%.9f,"
                     "\"gpu_end\":%.9f,\"unix_time\":%.6f}\n",
                     event,::getpid(),static_cast<unsigned long long>(surface),
                     static_cast<unsigned long long>(frame),width,height,request,submit,
                     now,display,gpu_start,gpu_end,wall);
        std::fflush(file_);
    }
    void input(std::uint64_t hwnd,std::uint32_t message) {
        const auto now=CACurrentMediaTime(),wall=unix_time();
        std::lock_guard lock(mutex_);
        std::fprintf(file_,"{\"event\":\"input_queue\",\"pid\":%d,\"hwnd\":%llu,"
                     "\"message\":%u,\"event_time\":%.9f,\"unix_time\":%.6f}\n",
                     ::getpid(),static_cast<unsigned long long>(hwnd),message,now,wall);
        std::fflush(file_);
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
