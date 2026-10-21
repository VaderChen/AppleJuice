#pragma once
#include "x64arm/decoder.hpp"
#include "x64arm/result.hpp"
#include "x64arm/host_services.hpp"
#include <exception>
#include <unordered_map>
#include <utility>
namespace x64arm {
inline constexpr GuestAddress return_sentinel{0x00007fffffffff00ULL};
struct RunOptions {
    std::uint64_t max_instructions{100000};
    GuestAddress sentinel{return_sentinel};
    HostServices* services{};
    // JIT 內部逐步回退不需產生預算診斷；對外停止仍由 run 的邊界產生。
    bool diagnose_budget{true};
    // 只於指令／服務邊界觀察，用於同執行緒原生模態回呼的合作返回。
    HostServices::NestedStop cooperative_stop{};
};
class NestedExecutionStop : public std::exception {
public:
    ExecutionResult result;
    explicit NestedExecutionStop(ExecutionResult value):result(std::move(value)){}
};
class NestedExecutionScope {
    HostServices* services_{};
    HostServices::NestedRunner previous_;
public:
    template<class Runner>
    NestedExecutionScope(HostServices* services,Runner&& runner):services_(services) {
        // 沒有服務時不建立型別抹除的 callback；否則函式參數的隱含轉換
        // 會在進入這個判斷前複製 closure，甚至為每次 CPU run 配置記憶體。
        if(services_)previous_=services_->exchange_nested_runner(std::forward<Runner>(runner));
    }
    ~NestedExecutionScope(){if(services_)services_->exchange_nested_runner(std::move(previous_));}
    NestedExecutionScope(const NestedExecutionScope&)=delete;
};
// 共用已退休 CPU trap 的派送；JIT／AOT 不回退直譯器重做指令。
std::optional<ExecutionResult> dispatch_pending_cpu_trap(GuestState&,GuestMemory&,HostServices*);
// 共用一個服務邊界的退休、排程與例外契約；呼叫端已套用執行政策並確認入口。
ExecutionResult invoke_host_service(GuestState&,GuestMemory&,HostServices&);
// 常駐 helper 已呼叫 invoke 但須由宿主迴圈收尾：重新拋出原例外，或以原結束碼完成同一退休契約。
// 延後收尾仍以原始服務 RIP 診斷未處理的例外；GuestState 保留 invoke 已完成的修改。
ExecutionResult complete_host_service(GuestState&,GuestMemory&,HostServices&,std::exception_ptr,std::optional<std::uint32_t>,GuestAddress);
class Interpreter {
public:
    // shared_cache 由呼叫端保持存活，僅同一協調執行緒依序切換後端時共用。
    explicit Interpreter(bool cache_decodes=true,DecodeCache* shared_cache=nullptr)
        :local_cache_(cache_decodes),shared_cache_(cache_decodes?shared_cache:nullptr){}
    ExecutionResult run(GuestState& state, GuestMemory& memory, RunOptions options = {}) const;
    using DecodeCacheStats=DecodeCache::Stats;
    DecodeCache& decode_cache() const noexcept {return shared_cache_?*shared_cache_:local_cache_;}
    const DecodeCacheStats& decode_cache_stats() const noexcept {return decode_cache().stats();}
    std::size_t decoded_entries() const noexcept {return decode_cache().size();}
    static constexpr std::size_t decode_cache_capacity=DecodeCache::default_capacity;
private:
    friend class JitEngine;
    // 原生後端的直譯語義橋接只借用單條 execute，不經 run 的服務、預算與例外派送。
    friend struct InterpreterBridgeAccess;
    static bool fast_scalar_form(const Instruction& instruction);
    static bool execute_fast_scalar(const Instruction& instruction, GuestState& state, GuestMemory& memory);
    mutable DecodeCache local_cache_;
    DecodeCache* shared_cache_{};
    static void execute(const Instruction& instruction, GuestState& state, GuestMemory& memory,HostServices* services);
};
}
