#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace x64arm::detail {
// 著色器翻譯結果的跨程序共享記憶體快取：鍵為（階段、bytecode）的 SHA-256，區段以目前執行檔的 LC_UUID 區分，
// 引擎重建即自然失效。記錄只追加、發布後不可變；讀取時核對完整鍵與校驗和，不符視為未命中。
// 預設開啟，X64ARM_SHADER_MEMORY_CACHE=0 關閉（同成品對照）。2026-10-30 電馭判客 720p 同成品交錯各 3 輪：
// 1% low 16.9／15.7／16.9 對關閉 11.4／15.1／11.7，平均 44.94 對 44.29，Benchmark 提早約 8 秒開始；
// 2,160 個實際 DXIL 著色器的 MSL 與未快取逐檔相同。
// X64ARM_SHADER_MEMORY_CACHE_MB 設定區段容量（16～2048，預設 256），只有實際寫入的頁面占用記憶體。
using ShaderCacheKey=std::array<std::uint8_t,32>;
bool shader_memory_cache_enabled() noexcept;
ShaderCacheKey shader_cache_key(std::uint32_t stage,std::span<const std::uint8_t> bytecode) noexcept;
std::optional<std::vector<std::uint8_t>> shader_memory_cache_find(const ShaderCacheKey& key);
void shader_memory_cache_store(const ShaderCacheKey& key,std::span<const std::uint8_t> payload) noexcept;
}
