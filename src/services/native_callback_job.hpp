#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace x64arm {
// 原生運算在客體回呼期間等待；結束續行時先喚醒並收回工作執行緒。
template<class Request, std::int32_t Cancelled>
class NativeCallbackJob {
public:
    NativeCallbackJob()=default;
    NativeCallbackJob(const NativeCallbackJob&)=delete;
    NativeCallbackJob& operator=(const NativeCallbackJob&)=delete;
    ~NativeCallbackJob(){
        {
            std::lock_guard lock(mutex_);
            cancelled_=true;
        }
        changed_.notify_all();
        if(worker_.joinable())worker_.join();
    }

    template<class Work>
    void start(Work work){
        worker_=std::thread([this,work=std::move(work)]() mutable {
            try{work();}
            catch(...){failure_=std::current_exception();}
            {
                std::lock_guard lock(mutex_);
                complete_=true;
            }
            changed_.notify_all();
        });
    }

    std::optional<Request> wait(){
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[this]{return request_ || complete_;});
        if(complete_ && failure_)std::rethrow_exception(failure_);
        return request_;
    }

    void reply(std::int32_t value){
        {
            std::lock_guard lock(mutex_);
            reply_=value;
            request_.reset();
        }
        changed_.notify_all();
    }

    std::int32_t call(Request request){
        std::unique_lock lock(mutex_);
        if(cancelled_)return Cancelled;
        request_=request;
        changed_.notify_all();
        changed_.wait(lock,[this]{return !request_ || cancelled_;});
        return cancelled_?Cancelled:reply_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Request> request_;
    std::int32_t reply_{};
    std::exception_ptr failure_;
    bool complete_{},cancelled_{};
    std::thread worker_;
};

}
