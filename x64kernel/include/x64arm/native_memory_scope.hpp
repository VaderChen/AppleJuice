#pragma once
#include <cstdint>
#include "x64arm/jit_recovery.hpp"
namespace x64arm {
class JitEngine;
class JitMemory;
class GuestMemory;
class NativeMemoryScope {
public:
    // 停用恢復時編譯器可直接看見空路徑，毋須呼叫建構／解構函式或初始化故障槽。
    NativeMemoryScope(const JitEngine& engine,const GuestMemory& memory,bool active):active_(active) {
        if(active_)enter(engine,memory);
    }
    NativeMemoryScope(const JitMemory& arena,const GuestMemory& memory,bool active):active_(active) {
        if(active_)enter(arena,memory);
    }
    ~NativeMemoryScope(){if(active_)leave();}
    static std::uint64_t fault_count() noexcept;
    static std::uintptr_t take_fault_pc() noexcept;
    NativeMemoryScope(const NativeMemoryScope&)=delete;
    NativeMemoryScope& operator=(const NativeMemoryScope&)=delete;
private:
    using Lookup=std::optional<JitRecoveryLocation>(*)(const void*,std::uintptr_t) noexcept;
    void enter(const JitEngine&,const GuestMemory&);
    void enter(const JitMemory&,const GuestMemory&);
    void enter(const void*,Lookup,const GuestMemory&);
    void leave() noexcept;
    const void* previous_context_;
    Lookup previous_lookup_;
    const GuestMemory* previous_memory_;
    bool active_;
};
}
