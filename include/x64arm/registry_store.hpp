#pragma once
#include "x64arm/registry.hpp"
#include <filesystem>
#include <array>
namespace x64arm {
// 只供持有同一份 RegistryState 的服務使用；持久資料的變更仍由交易鎖序列化。
struct RegistryStoreCache {
    const RegistryState* owner{};
    std::filesystem::path path;
    std::array<std::uint64_t,7> stamp{};
    std::uint64_t hits{},loads{};
    bool valid{};
};
// 一次 Win32 登錄操作持有跨程序鎖；只有成功的變更才原子取代資料檔。
class RegistryTransaction {
    RegistryState& state_;
    std::filesystem::path path_;
    int lock_{-1};
    RegistryStoreCache* cache_{};
    bool readonly_{};
public:
    std::uint32_t error{};
    RegistryTransaction(RegistryState&,const std::filesystem::path&,RegistryStoreCache* = nullptr,bool readonly = false);
    ~RegistryTransaction();
    RegistryTransaction(const RegistryTransaction&)=delete;
    RegistryTransaction& operator=(const RegistryTransaction&)=delete;
    std::uint32_t commit();
    // RegFlushKey／NtFlushKey 的同步耐久化邊界；不重寫或更改登錄值。
    std::uint32_t flush();
    // 可寫交易最後沒有改變持久狀態（只配置程序內 handle）時呼叫：結束時保留快取，下一次不必重新解析登錄檔。
    void unchanged() noexcept {readonly_=true;}
};
}
