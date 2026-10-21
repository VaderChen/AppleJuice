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
