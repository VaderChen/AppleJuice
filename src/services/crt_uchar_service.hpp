#pragma once
#include "crt_mbcs.hpp"
#include "crt_uchar_exports.hpp"
#include "x64arm/windows_abi.hpp"

namespace x64arm {
inline void invoke_crt_uchar(const WindowsCall& arg, GuestState& state, GuestMemory& memory,
                             CrtUcharFunction function, std::uint64_t implicit_states, std::uint64_t errno_cell) {
    const bool decode = function == CrtMbrtoc16 || function == CrtMbrtoc32;
    const bool utf16 = function == CrtMbrtoc16 || function == CrtC16rtomb;
    const auto destination = arg(0), input = arg(1), state_arg = arg(decode ? 3 : 2);
    const GuestAddress storage{state_arg ? state_arg : implicit_states + function * sizeof(CrtUcharState)};
    CrtUcharState conversion{};
    memory.read(storage, conversion.bytes);
    memory.check(storage, sizeof(conversion), Access::Write);
    int result;
    if (decode) {
        unsigned value = 0;
        result = crt_uchar_decode(&conversion, utf16, &memory, crt_mbc_guest_byte, input, arg(2), &value);
        if (destination && input && (result >= 0 || result == CrtUcharPending))
            memory.store(GuestAddress{destination}, utf16 ? 16 : 32, value);
    } else {
        std::array<unsigned char, 4> bytes{};
        const auto value = utf16 ? static_cast<std::uint16_t>(input) : static_cast<std::uint32_t>(input);
        result = crt_uchar_encode(&conversion, utf16, destination ? value : 0, bytes.data());
        if (destination && result > 0)
            memory.write(GuestAddress{destination}, std::span(bytes).first(static_cast<std::size_t>(result)));
    }
    memory.write(storage, conversion.bytes);
    if (result == CrtUcharInvalid) memory.store(GuestAddress{errno_cell}, 32, 42);
    arg.return_to(state, static_cast<std::uint64_t>(result));
}
} // namespace x64arm
