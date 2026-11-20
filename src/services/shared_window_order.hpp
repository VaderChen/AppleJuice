#pragma once
#include <cstdint>
#include <vector>
namespace x64arm {
struct SharedWindowState;
std::vector<std::uint64_t> snapshot_window_order(SharedWindowState*,const std::vector<std::uint64_t>& local);
// after 為 0 時置頂，1 時置底，其餘值為前一個視窗。由共享視窗表鎖序列化。
void publish_window_order(SharedWindowState*,std::uint64_t window,std::uint64_t after);
}
