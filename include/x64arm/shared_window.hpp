#pragma once
#include <array>
#include <cstdint>

namespace x64arm {
// 僅放跨程序可讀的視窗屬性；客體 procedure、物件指標與客體記憶體不外流。
struct SharedWindowInfo {
    std::uint64_t handle{}, thread{}, parent{}, owner{}, instance{}, id{}, desktop{}, job{};
    std::uint32_t process{}, width{}, height{}, outer_width{}, outer_height{}, style{}, ex_style{}, visible{},
        enabled{}, show{};
    std::int32_t x{}, y{}, client_x{}, client_y{};
    std::uint32_t class_atom{}, active{}, shell_desktop{};
    std::array<char16_t, 256> class_name{};
    std::array<char16_t, 512> title{};
    // 使用原有尾端 4-byte padding，不改變既有程序的欄位位置或共用槽位大小。
    std::uint32_t display_affinity{};
    bool operator==(const SharedWindowInfo &) const = default;
};
struct SharedWindowState;
} // namespace x64arm
