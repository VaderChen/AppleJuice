#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <vector>
namespace x64arm {
using ObjectSecurityFactory=std::function<std::vector<std::uint8_t>()>;
std::vector<std::uint8_t> shared_object_security(int descriptor,bool thread=false);
void set_shared_object_security(int descriptor,const std::vector<std::uint8_t>& security,bool thread=false);
struct SharedEventStorage;
class SharedEvent {
    std::shared_ptr<SharedEventStorage> storage_;
public:
    explicit SharedEvent(std::shared_ptr<SharedEventStorage> storage):storage_(std::move(storage)){}
    bool manual_reset() const noexcept;
    bool signaled() const noexcept;
    std::uint32_t count() const noexcept;
    std::uint32_t maximum() const noexcept;
    bool same_object(const SharedEvent& other) const noexcept;
    std::uint32_t release(std::uint32_t increment,std::uint32_t* previous=nullptr,bool saturate=false);
    void set(bool value);
    void consume_locked() noexcept;
    int lock_descriptor() const noexcept;
    int descriptor() const noexcept;
};
struct SharedEventLock {
    int descriptor{-1};
    explicit SharedEventLock(int value=-1);
    ~SharedEventLock();
    SharedEventLock(const SharedEventLock&)=delete;
    SharedEventLock& operator=(const SharedEventLock&)=delete;
};
struct OpenSharedEvent {std::shared_ptr<SharedEvent> event;std::uint32_t error{};bool existed{};};
OpenSharedEvent inherit_shared_event(const std::string& session,int descriptor);
OpenSharedEvent open_shared_event(const std::string& session,std::u16string name,bool create,bool manual,bool initial,const ObjectSecurityFactory& security={});
// 計數訊號沿用 event 的命名空間、鎖、權限、生命週期與 SharedWake；獨立簽章防止物件類型混用。
OpenSharedEvent open_shared_semaphore(const std::string& session,std::u16string name,bool create,std::uint32_t initial,std::uint32_t maximum,const ObjectSecurityFactory& security={});
OpenSharedEvent inherit_shared_semaphore(const std::string& session,int descriptor);
struct SharedMutexStorage;
// 擁有者為宿主 PID（即客體 PID）與該程序的執行緒鍵；recursion 為 0 表示未持有。
struct SharedMutexState {std::uint64_t process{},thread{};std::uint32_t recursion{};bool abandoned{};};
class SharedMutex {
    std::shared_ptr<SharedMutexStorage> storage_;
public:
    explicit SharedMutex(std::shared_ptr<SharedMutexStorage> storage):storage_(std::move(storage)){}
    bool available_locked(std::uint64_t thread);
    bool acquire_locked(std::uint64_t thread); // 回傳是否承接 abandoned ownership。
    std::uint32_t release_locked(std::uint64_t thread);
    SharedMutexState state_locked();
    void abandon(std::uint64_t thread);
    bool owned() const noexcept;
    void detach_handles(); // 保留執行緒 ownership lease，但最後的 handle 不再計入物件 references。
    int descriptor() const noexcept;
    int lock_descriptor() const noexcept;
};
struct OpenSharedMutex {std::shared_ptr<SharedMutex> mutex;std::uint32_t error{};bool existed{};};
OpenSharedMutex open_shared_mutex(const std::string& session,std::u16string name,bool create,std::uint64_t owner=0,std::uint32_t recursion=1,const ObjectSecurityFactory& security={});
OpenSharedMutex inherit_shared_mutex(const std::string& session,int descriptor);
}
