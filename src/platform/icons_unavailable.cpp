#include "x64arm/windows_icon.hpp"
namespace x64arm {
std::optional<GuiIcon> native_png_icon(std::span<const std::uint8_t>){return {};}
std::optional<GuiIcon> native_stock_icon(std::uint32_t,std::uint32_t){return {};}
std::optional<GuiIcon> native_system_cursor(std::uint32_t){return {};}
}
