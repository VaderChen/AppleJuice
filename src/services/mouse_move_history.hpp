#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace x64arm {
struct SharedWindowState;
using MouseMoveSample=std::array<std::int64_t,4>;
void append_shared_mouse_move(SharedWindowState&,const MouseMoveSample&);
std::vector<MouseMoveSample> shared_mouse_moves(SharedWindowState&);
}
