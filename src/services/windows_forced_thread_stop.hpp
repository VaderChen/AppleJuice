#pragma once
#include "x64arm/windows_services.hpp"

namespace x64arm::detail {
// 共用強制停止準備；實際資源回收仍由既有 force_finish_thread 排程入口完成。
template<class Thread>
bool prepare_forced_thread_stop(Thread& thread, std::uint32_t code, GuestAddress continuation) {
    if (thread.status == ThreadStatus::Exited || thread.forced_stop)
        return false;
    thread.forced_stop = thread.terminating = true;
    thread.exit_code = code;
    thread.apcs.clear();
    thread.wait = {};
    thread.suspended = 0;
    thread.status = ThreadStatus::Runnable;
    thread.context.pending_cpu_trap = 0;
    thread.context.rip = continuation;
    return true;
}
}
