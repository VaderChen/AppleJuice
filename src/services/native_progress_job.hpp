#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <string>
#include <new>
namespace x64arm {
// 原生演算法與客體回呼交替執行。工作執行緒不存取客體記憶體，等待回呼時不運算。
class NativeProgressJob {
    std::mutex mutex_;std::condition_variable changed_;std::thread worker_;
    bool cancelled_{},pending_{},complete_{},report_{};float progress_{};
    std::uint32_t reply_{};
public:
    std::uint32_t result{};std::string error;
    using Progress=std::function<std::uint32_t(float)>;
    NativeProgressJob(bool report,std::function<std::uint32_t(const Progress&)> work):report_(report){
        worker_=std::thread([this,work=std::move(work)]{
            try{result=work([this](float progress){
                std::unique_lock lock(mutex_);if(cancelled_)return 0x80004004U;if(!report_)return 0U;
                progress_=progress;pending_=true;changed_.notify_all();changed_.wait(lock,[this]{return !pending_ || cancelled_;});
                return cancelled_?0x80004004U:reply_;
            });}catch(const std::bad_alloc&){result=0x8007000eU;}catch(const std::exception& e){result=0x80004005U;error=e.what();}catch(...){result=0x80004005U;}
            std::lock_guard lock(mutex_);complete_=true;changed_.notify_all();
        });
    }
    ~NativeProgressJob(){cancel();if(worker_.joinable())worker_.join();}
    void cancel(){std::lock_guard lock(mutex_);cancelled_=true;changed_.notify_all();}
    void reply(std::uint32_t value){std::lock_guard lock(mutex_);reply_=value;pending_=false;changed_.notify_all();}
    bool wait(float& progress){std::unique_lock lock(mutex_);changed_.wait(lock,[this]{return pending_ || complete_;});progress=progress_;return pending_;}
};
}
