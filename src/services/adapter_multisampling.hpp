#pragma once
#include <cstdint>
#include <optional>

namespace x64arm {
// 位元 n 表示預設繪圖裝置支援 n 個樣本。裝置已移除時回傳空值。
std::optional<std::uint32_t> adapter_multisample_mask(std::uint32_t format);
}
