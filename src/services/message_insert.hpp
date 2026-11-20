#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace x64arm {
class GuestMemory;
std::u16string render_message_insert(GuestMemory&,std::u16string_view,bool,std::span<const std::uint64_t>);
}
