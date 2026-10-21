#pragma once
#include <cstdint>
#include <string_view>

namespace x64arm::entry_runtime_profile {
// 獨立診斷開關，不啟用 startup profile，也不撤銷常駐服務或原生接續。
bool enabled() noexcept;
enum class Kind : unsigned {
    Native, Service, Bridge, Fallback, ResidentRep,
    Prepare, Discover, Compile, Publish, MemoryBacking,
    CompileIndex, CompileCfg, CompileLiveness, CompileEmission, CompileRelocation, CompileMetadata
};
struct Totals;
class Scope {
    Scope* parent_{};
    Totals* totals_{};
    std::uint64_t cpu_{},wall_{},children_cpu_{};
    struct NativeExit {
        std::uint64_t rip,completed,budget;
        unsigned reason;
    } native_exit_;
    bool has_native_exit_{};
    static thread_local Scope* current_;
public:
    Scope(Kind,std::uint64_t entry,std::string_view label={}) noexcept;
    ~Scope();
    // 只在啟用計時時保存原生批次結果；不改配額、排程或任何客體狀態。
    void native_exit(std::uint64_t rip,std::uint64_t completed,std::uint64_t budget,unsigned reason) noexcept {
        if(totals_){native_exit_={rip,completed,budget,reason};has_native_exit_=true;}
    }
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
}
