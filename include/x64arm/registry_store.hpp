#pragma once
#include "x64arm/registry.hpp"
#include <filesystem>
#include <array>
#include <functional>
namespace x64arm {
// Runner 必須執行 Work 一次，等待其完全停止後才返回或拋出；不得保存 Work。
// 只有原始 I/O 進入 Runner，RegistryState 與快取的解析／發布仍在呼叫執行緒。
class RegistryIoScope {
public:
    using Work=std::function<void()>;
    using Runner=std::function<void(const Work&)>;
    explicit RegistryIoScope(Runner);
    ~RegistryIoScope();
    RegistryIoScope(const RegistryIoScope&)=delete;
    RegistryIoScope& operator=(const RegistryIoScope&)=delete;
private:
    Runner runner_;
    RegistryIoScope* previous_{};
    friend void run_registry_io(const Work&);
};
void run_registry_io(const RegistryIoScope::Work&);
// 只供持有同一份 RegistryState 的服務使用；持久資料的變更仍由交易鎖序列化。
struct RegistryStoreCache {
    static constexpr std::size_t byte_limit=256U*1024U;
    const RegistryState* owner{};
    std::filesystem::path path;
    std::array<std::uint64_t,7> stamp{};
    std::uint64_t hits{},loads{};
    bool valid{};
    // 只保留一份小型已驗證資料；可寫交易失效的是解析狀態，不是磁碟快照。
    std::filesystem::path byte_path;
    std::array<std::uint64_t,7> byte_stamp{};
    std::vector<std::uint8_t> bytes;
    std::uint64_t byte_hits{},disk_reads{};
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
