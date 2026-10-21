#pragma once
#include "x64arm/mouse_drag.hpp"
#include <cstdint>
namespace x64arm {
struct GuiDragDetection {
    enum class Stage { Acquire, Read, Dispatch, Release, RepostQuit } stage{Stage::Acquire};
    mouse_drag::Detection detection;
    std::uint64_t window{},thread{},message_buffer{},capture_generation{};
    std::uint32_t quit_code{};
    bool quit{};
    bool operator==(const GuiDragDetection&) const = default;
};
}
