#include "x64arm/process_debugger.hpp"
#include "x64arm/process_identity.hpp"
#include <unistd.h>

namespace x64arm::process_debugger {
Result query(std::uint32_t pid) {
    const auto result = process_identity::query(pid);
    return {result.error, result.traced, result.identity.birth, result.exited};
}
Result self(GuestMemory &memory, GuestAddress peb, bool refresh) {
    if (!refresh)
        return {0, memory.load(GuestAddress{peb.value + 2}, 8) != 0, 0};
    const auto result = query(static_cast<std::uint32_t>(::getpid()));
    if (!result.error) {
        // 匯入盤點／純 API 測試可能尚未建立 PEB；只更新可寫的實際 PEB。
        try {
            memory.store(GuestAddress{peb.value + 2}, 8, result.present);
        } catch (const MemoryFault &) {
        }
    }
    return result;
}
} // namespace x64arm::process_debugger
