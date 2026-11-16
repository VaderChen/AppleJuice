#pragma once
// 宿主 IPv4／IPv6 協定統計（macOS sysctl net.inet*.stats），依 Windows MIB 欄位順序回傳 32 位元計數。
// 由服務層補上介面數、位址數、路由數與連線數；宿主不提供（如一般使用者讀不到的 TCP 計數）時為 0。
#include <array>
#include <cstdint>
#include <optional>
namespace x64arm::host_network_statistics {
// MIB_IPSTATS 的 23 個欄位；最後三個（介面、位址、路由數）由呼叫端填入。
std::optional<std::array<std::uint32_t,23>> ip(bool ipv6);
// MIB_TCPSTATS 的 15 個欄位；CurrEstab（8）與 NumConns（14）由呼叫端依 TCP 表填入。
std::optional<std::array<std::uint32_t,15>> tcp();
// MIB_UDPSTATS 的 5 個欄位；NumAddrs（4）由呼叫端依 UDP 表填入。
std::optional<std::array<std::uint32_t,5>> udp();
struct Icmp {std::uint32_t in_messages{},in_errors{},out_messages{},out_errors{};std::array<std::uint32_t,256> in{},out{};};
std::optional<Icmp> icmp(bool ipv6);
}
