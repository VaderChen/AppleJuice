#pragma once
#include <cstdint>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace x64arm {
enum class ExecutionPolicy : std::uint8_t { Automatic, Efficient, Responsive, Utility, Interactive };
// 客體執行範圍持有設定；原生 worker 可輪替承載不同客體，不能留下前一個客體的 QoS。
class HostExecutionScope {
public:
    explicit HostExecutionScope(bool needs_policy=true) {
#if defined(__APPLE__)
        // 沒有宿主服務時不會套用政策，毋須在每次 run 查詢未使用的 QoS。
        // 有服務的範圍仍在入口擷取原值，保留既有還原契約。
        if(needs_policy)valid_=pthread_get_qos_class_np(pthread_self(),&original_,&original_relative_)==0;
#else
        (void)needs_policy;
#endif
    }
    HostExecutionScope(const HostExecutionScope&)=delete;
    HostExecutionScope& operator=(const HostExecutionScope&)=delete;
    ~HostExecutionScope() { (void)apply(ExecutionPolicy::Automatic); }
    bool apply(ExecutionPolicy policy) {
        if (policy==applied_) return true;
#if defined(__APPLE__)
        const auto qos=policy==ExecutionPolicy::Efficient?QOS_CLASS_BACKGROUND:
            policy==ExecutionPolicy::Responsive?QOS_CLASS_USER_INITIATED:
            policy==ExecutionPolicy::Utility?QOS_CLASS_UTILITY:
            policy==ExecutionPolicy::Interactive?QOS_CLASS_USER_INTERACTIVE:original_;
        // 已退出 QoS 系統的 pthread 無法安全恢復，拒絕變更並保留既有排程契約。
        if (!valid_ || original_==QOS_CLASS_UNSPECIFIED ||
            pthread_set_qos_class_self_np(qos,policy==ExecutionPolicy::Automatic?original_relative_:0)) return false;
        applied_=policy; return true;
#else
        return policy==ExecutionPolicy::Automatic;
#endif
    }
private:
    ExecutionPolicy applied_{ExecutionPolicy::Automatic};
#if defined(__APPLE__)
    qos_class_t original_{}; int original_relative_{}; bool valid_{};
#endif
};
}
