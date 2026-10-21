#pragma once
#include <cstdint>
#include <sys/socket.h>
namespace x64arm::socket_detail {
inline int randomize_port_option(std::uint32_t level, std::uint32_t option) {
#if defined(SO_RANDOMPORT)
    if (level == 0xffff && option == 0x3005) return SO_RANDOMPORT;
#else
    (void)level; (void)option;
#endif
    return 0;
}
inline int socket_option_boolean(std::uint32_t value) { return value != 0; }
} // namespace x64arm::socket_detail
