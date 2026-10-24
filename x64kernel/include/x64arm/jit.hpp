#pragma once
#include "x64arm/interpreter.hpp"
#include <filesystem>
#include <functional>
#include <span>

namespace x64arm {
// 僅保留明確的編譯錯誤，避免舊呼叫端靜默啟用退役後端。
// 完整舊宣告留於 src/jit/legacy_jit_reference.hpp；SDK 不編入任何 JitEngine 實作。
class JitEngine final {
public:
    JitEngine() = delete;
    explicit JitEngine(std::size_t) = delete;
    JitEngine(std::size_t, std::size_t, DecodeCache* = nullptr) = delete;
    JitEngine(const JitEngine&) = delete;
    JitEngine(JitEngine&&) = delete;
    ~JitEngine() = delete;
    ExecutionResult run(GuestState&, GuestMemory&, RunOptions = {}) = delete;
    std::size_t precompile(const GuestMemory&, std::span<const GuestAddress>, GuestAddress,
                          std::uint64_t, const std::function<bool()>& = {}, bool = true,
                          std::uint32_t = 0x3fU) = delete;
};
}
