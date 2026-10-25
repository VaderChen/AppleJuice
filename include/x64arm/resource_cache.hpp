#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace x64arm {
std::filesystem::path generated_resource_cache_directory();
void preload_generated_resources();
// 在背景執行緒預載，與 PE 載入、GUI 初始化及父程序握手重疊；之後首次取用或 flush 自然等待完成。
void preload_generated_resources_async();
void flush_generated_resources();
struct ResourceCacheStats {
    std::uint64_t stores{},entries{},retained_bytes{};
    std::uint64_t payload_budget_bytes{},payload_reserved_bytes{},peak_payload_reserved_bytes{};
    std::uint64_t store_limit{},reserved_stores{},entry_limit_per_store{},budget_rejections{},store_rejections{},store_evictions{};
    std::uint64_t live_payloads{},live_payload_bytes{},peak_live_payload_bytes{};
    std::uint64_t writer_held_bytes{},peak_writer_held_bytes{},active_loads{};
    std::uint64_t requests{},memory_hits{},disk_hits{},build_calls{},build_failures{},wait_calls{},evictions{},bypasses{};
    std::uint64_t build_ns{},read_ns{},decode_ns{},encode_ns{},write_ns{},write_failures{};
};
// 固定大小的程序彙總；各欄位為非同步取樣，停止作業後才是同一穩定狀態。
// retained_bytes 為索引中的邏輯大小；live_payload_bytes 以 vector capacity 計唯一保留配置，
// 包括 writer 尚未放開的資料，不含回傳 Bytes 複本、壓縮暫存、容器或配置器額外成本。
// 生成資源家族合計最多保留 256 MiB payload／16 個 store，每個 store 最多 1024 筆。
// reserved 在配置之前取得，最後釋放才歸還；不足時不保留副本，不強迫前景等待 writer。
ResourceCacheStats generated_resource_cache_stats() noexcept;
// 可重建的唯讀資源；來源身分由呼叫端提供，資料另以 SHA-256 驗證。
// RAM 以解壓大小限額，磁碟以含檔頭大小限額；最多 entries 筆。
// 啟動預載、命中只讀 RAM；新產物背景批次寫入，正常結束補寫。
// 容量不足時淘汰最久未使用者；快取失敗不影響資源建立。
class ResourceCache {
public:
    struct Result {
        std::vector<std::uint8_t> bytes;bool hit{},stored{},pending{},memory_hit{};std::uint64_t stored_bytes{};
        double build_ms{},read_ms{},decode_ms{},encode_ms{},write_ms{};
    };
    ResourceCache(std::filesystem::path directory,std::uint64_t bytes=256U*1024U*1024U,std::size_t entries=1024,bool compress=true)
        :directory_(std::move(directory)),limit_(bytes),entries_(entries),compress_(compress){}
    Result get_or_create(std::span<const std::uint8_t> identity,const std::function<std::vector<std::uint8_t>()>& create) const;
    void preload() const;
    void flush() const;
private:
    std::filesystem::path directory_;
    std::uint64_t limit_;
    std::size_t entries_;
    bool compress_;
};
}
