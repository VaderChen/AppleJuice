#pragma once
#include "native_callback_job.hpp"
#include "service_callback_task.hpp"
#include <unicode/uregex.h>

namespace x64arm::icu_regex {
struct GuestHandler {
    std::uint64_t function{}, context{};
};
struct Handlers {
    GuestHandler match, progress;
    bool enabled() const { return match.function || progress.function; }
};

struct CallbackRequest {
    GuestHandler handler;
    std::uint64_t argument;
    bool wide;
};

class CallbackBridge {
public:
    CallbackBridge(URegularExpression* regex, Handlers handlers, UErrorCode& status)
        : regex_(regex), handlers_(handlers) {
        if (handlers_.match.function) uregex_setMatchCallback(regex_, match, this, &status);
        if (handlers_.progress.function) uregex_setFindProgressCallback(regex_, progress, this, &status);
    }
    CallbackBridge(const CallbackBridge&) = delete;
    ~CallbackBridge() {
        UErrorCode status = U_ZERO_ERROR;
        if (handlers_.match.function) uregex_setMatchCallback(regex_, nullptr, nullptr, &status);
        if (handlers_.progress.function) uregex_setFindProgressCallback(regex_, nullptr, nullptr, &status);
    }

    template<class Work>
    service_callback::Task<std::int32_t> execute(Work& operation) {
        std::int32_t result{};
        // frame 清除時先喚醒並 join 工作執行緒，再釋放結果與上層的 matcher。
        NativeCallbackJob<CallbackRequest, 0> job;
        work_ = &job;
        job.start([&] { result = operation(); });
        while (const auto request = job.wait()) {
            GuestCallback guest{request->handler.function, {request->handler.context, request->argument}};
            guest.wide_arguments = request->wide ? 2U : 0U;
            const auto raw = co_await service_callback::call_native(std::move(guest));
            job.reply((raw & 0xffU) != 0);
        }
        co_return result;
    }

private:
    static UBool U_CALLCONV match(const void* context, std::int32_t steps) {
        auto& self = *static_cast<const CallbackBridge*>(context);
        return self.work_->call({self.handlers_.match, static_cast<std::uint32_t>(steps), false}) != 0;
    }
    static UBool U_CALLCONV progress(const void* context, std::int64_t position) {
        auto& self = *static_cast<const CallbackBridge*>(context);
        return self.work_->call({self.handlers_.progress, static_cast<std::uint64_t>(position), true}) != 0;
    }
    URegularExpression* regex_;
    Handlers handlers_;
    NativeCallbackJob<CallbackRequest, 0>* work_{};
};
}
