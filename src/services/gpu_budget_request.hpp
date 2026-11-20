#pragma once
#include <cstdint>

namespace x64arm::gpu_budget_request {
// 程序資訊通道第 14 類請求。接收者只存取自己的 GPU 配額。
inline constexpr std::uint32_t query=16,change=17;
}
