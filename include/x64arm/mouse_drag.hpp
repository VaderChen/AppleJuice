#pragma once
#include <array>
#include <cstdint>

namespace x64arm::mouse_drag {
// 邊界用 64 位元計算，保留 POINT／系統門檻的完整有號範圍。
struct Detection {
    enum class Result { Tracking, Dragged, Cancelled } result{Result::Tracking};
    std::array<std::int64_t,4> rectangle{};
    Detection() = default;
    Detection(std::array<std::int32_t,2> start,std::int32_t width,std::int32_t height);
    void move(std::array<std::int32_t,2> point,bool left_down);
    void cancel();
    bool operator==(const Detection&) const = default;
};
}
