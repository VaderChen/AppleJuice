#pragma once
#include "x64arm/host_services.hpp"
#include <coroutine>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace x64arm::service_callback {
// API 的 C++ 區域資料由 coroutine frame 持有，客體 callback 則沿原排程器執行。
// 完成後只重新進入原 API 的純前置解析；consume 交回既有結果，不重做操作。
struct Suspended {};

template<class T> class Task {
public:
    struct promise_type;
    using Handle=std::coroutine_handle<promise_type>;
    struct promise_type {
        std::optional<T> value;
        std::exception_ptr error;
        std::coroutine_handle<> continuation;
        Task get_return_object(){return Task(Handle::from_promise(*this));}
        std::suspend_always initial_suspend() const noexcept{return {};}
        struct Final {
            bool await_ready() const noexcept{return false;}
            std::coroutine_handle<> await_suspend(Handle h) const noexcept {
                return h.promise().continuation?h.promise().continuation:std::noop_coroutine();
            }
            void await_resume() const noexcept{}
        };
        Final final_suspend() const noexcept{return {};}
        template<class V> void return_value(V&& v){value.emplace(std::forward<V>(v));}
        void unhandled_exception() noexcept{error=std::current_exception();}
    };
    Task()=default;
    explicit Task(Handle h):handle(h){}
    Task(Task&& other) noexcept:handle(std::exchange(other.handle,{})){}
    Task& operator=(Task&& other) noexcept {
        if(handle)handle.destroy();handle=std::exchange(other.handle,{});return *this;
    }
    Task(const Task&)=delete;
    ~Task(){if(handle)handle.destroy();}
    bool done() const noexcept{return handle && handle.done();}
    std::coroutine_handle<> address() const noexcept{return handle;}
    T take(){return take(handle);}
    struct Awaiter {
        Handle handle;
        ~Awaiter(){if(handle)handle.destroy();}
        bool await_ready() const noexcept{return handle.done();}
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) noexcept {
            handle.promise().continuation=parent;return handle;
        }
        T await_resume(){return Task::take(handle);}
    };
    Awaiter operator co_await() && noexcept{return {std::exchange(handle,{})};}
private:
    Handle handle{};
    static T take(Handle h){
        if(h.promise().error)std::rethrow_exception(h.promise().error);
        if(!h.done() || !h.promise().value)throw HostCallFault("服務續行沒有完成結果");
        return std::move(*h.promise().value);
    }
};

using Begin=std::function<void(GuestState&,GuestMemory&,std::uint64_t,std::vector<std::uint64_t>,std::uint64_t)>;
struct Context {
    void* service{};
    std::uint64_t id{},thread{};
    std::string_view kind;
    GuestState entry,state;
    GuestMemory* memory{};
    Begin begin;
    std::coroutine_handle<> continuation;
    std::uint64_t callback_value{};
    bool waiting{};
    virtual ~Context()=default;
    virtual bool done() const noexcept=0;
};
template<class T> struct TypedContext final:Context {
    // factory 的 captures 被 coroutine 借用，因此必須比 task 更晚釋放。
    std::function<Task<T>(GuestState&,GuestMemory&)> factory;
    Task<T> task;
    bool done() const noexcept override{return task.done();}
};
inline std::mutex contexts_mutex;
inline std::map<std::uint64_t,std::shared_ptr<Context>> contexts;
inline std::uint64_t next_id=1;
inline thread_local Context* active{};

inline void advance(Context& context,std::coroutine_handle<> continuation){
    struct Scope {Context* previous;~Scope(){active=previous;}} scope{active};
    active=&context;context.waiting=false;continuation.resume();
    if(!context.done() && !context.waiting)throw HostCallFault("服務續行缺少客體等待點");
}
inline void erase(std::uint64_t id){
    std::shared_ptr<Context> released;
    {std::lock_guard lock(contexts_mutex);const auto it=contexts.find(id);if(it!=contexts.end()){released=std::move(it->second);contexts.erase(it);}}
}
inline void clear(void* service){
    // 在鎖外銷毀 coroutine，讓來源 scratch 與額度各自按 RAII 歸還。
    std::vector<std::shared_ptr<Context>> released;
    {std::lock_guard lock(contexts_mutex);for(auto it=contexts.begin();it!=contexts.end();)
        if(it->second->service==service){released.push_back(std::move(it->second));it=contexts.erase(it);}else ++it;}
}
inline void cancel(void* service,std::uint64_t id){
    {std::lock_guard lock(contexts_mutex);const auto it=contexts.find(id);
     if(it==contexts.end() || it->second->service!=service)throw HostCallFault("服務續行取消的擁有者不符");}
    erase(id);
}

template<class Result> struct BasicCall {
    std::uint64_t target;
    std::vector<std::uint64_t> arguments;
    Context* context{};
    GuestState saved;
    bool await_ready() const noexcept{return false;}
    void await_suspend(std::coroutine_handle<> continuation){
        if(!active || active->waiting)throw HostCallFault("客體回呼沒有對應服務續行");
        context=active;saved=context->state;
        context->begin(context->state,*context->memory,target,std::move(arguments),context->id);
        context->continuation=continuation;context->waiting=true;
    }
    Result await_resume(){
        const auto elapsed=context->state.tsc;
        const auto value=context->callback_value;
        context->state=saved;context->state.tsc=elapsed;return static_cast<Result>(value);
    }
};
using Call=BasicCall<std::uint32_t>;
inline Call call(std::uint64_t target,std::vector<std::uint64_t> arguments){return {target,std::move(arguments),nullptr,{}};}
inline BasicCall<std::uint64_t> call_value(std::uint64_t target,std::vector<std::uint64_t> arguments){return {target,std::move(arguments),nullptr,{}};}

template<class T,class Factory>
T invoke(void* service,std::uint64_t thread,GuestState& state,GuestMemory& memory,
         std::string_view kind,Begin begin,Factory factory){
    std::shared_ptr<TypedContext<T>> context;
    {
        std::lock_guard lock(contexts_mutex);
        std::size_t count=0;
        for(const auto& [id,candidate]:contexts){
            (void)id;if(candidate->service!=service)continue;++count;
            if(candidate->thread==thread && candidate->kind==kind && candidate->entry.rip==state.rip &&
               candidate->entry.reg(Gpr::Rsp)==state.reg(Gpr::Rsp) && candidate->done()){
                context=std::dynamic_pointer_cast<TypedContext<T>>(candidate);
                if(!context)throw HostCallFault("服務續行結果型別不符");break;
            }
        }
        if(!context){
            if(count>=1024 || !next_id)throw std::bad_alloc();
            context=std::make_shared<TypedContext<T>>();context->service=service;context->thread=thread;
            context->id=next_id++;context->kind=kind;context->entry=context->state=state;context->memory=&memory;
            context->begin=std::move(begin);context->factory=std::move(factory);
            context->task=context->factory(context->state,memory);contexts.emplace(context->id,context);
        }
    }
    if(!context->done()){
        try{advance(*context,context->task.address());}
        catch(...){erase(context->id);throw;}
    }
    state=context->state;
    if(!context->done())throw Suspended{};
    erase(context->id);return context->task.take();
}

inline void resume(void* service,std::uint64_t thread,std::uint64_t id,GuestState& state,std::uint64_t value){
    std::shared_ptr<Context> context;
    {std::lock_guard lock(contexts_mutex);const auto it=contexts.find(id);
     if(it==contexts.end() || it->second->service!=service || it->second->thread!=thread){
         throw HostCallFault("服務續行返回的擁有者不符");
     }
     context=it->second;}
    if(!context->waiting || context->done())throw HostCallFault("服務續行重複返回");
    context->state=state;context->callback_value=value;
    advance(*context,context->continuation);
    // 尚有下一個 Native 呼叫時直接進入它；完成時讓原 API 取回一次結果。
    const auto elapsed=context->state.tsc;
    state=context->done()?context->entry:context->state;state.tsc=elapsed;
}
} // namespace x64arm::service_callback
