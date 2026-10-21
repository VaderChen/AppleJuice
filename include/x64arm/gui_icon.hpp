#pragma once
#include <cstdint>
#include <vector>
#include <string>
namespace x64arm {
struct GuiIcon {std::vector<std::uint8_t> image{};std::uint32_t width{},height{},system{};std::vector<std::uint32_t> rgba{},color_pixels{};std::vector<std::uint8_t> mask_pixels{};
    std::uint32_t hotspot_x{},hotspot_y{};bool is_icon{true},monochrome{},alpha{},shared{};
    std::vector<GuiIcon> animation_frames{};
    std::vector<std::uint32_t> animation_sequence{},animation_rates{};
    std::uint64_t resource_instance{};std::uint32_t resource_name_id{};std::u16string resource_name{};
    bool operator==(const GuiIcon&) const = default;};
}
