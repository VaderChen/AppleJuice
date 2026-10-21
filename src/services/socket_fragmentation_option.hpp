#pragma once
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>

namespace x64arm::socket_detail {
struct FragmentationOption { int level{}, option{}; };
inline FragmentationOption fragmentation_option(std::uint32_t level, std::uint32_t option,
                                                int family, int type) {
    // Winsock 的 DWORD 布林選項只適用於訊息協定；不替 TCP 寫入無效的狀態。
    if (option != 14 || type != SOCK_DGRAM) return {};
#if defined(__APPLE__) && defined(IP_DONTFRAG)
    if (level == 0 && family == AF_INET) return {IPPROTO_IP, IP_DONTFRAG};
#endif
#if defined(__APPLE__)
#if defined(IPV6_DONTFRAG)
    if (level == 41 && family == AF_INET6) return {IPPROTO_IPV6, IPV6_DONTFRAG};
#else
    // Darwin SDK 將此公開 ABI 常數放在 RFC 3542 宣告區塊。
    // 不切換整個 translation unit 的 RFC 模式，以免改變既有 ancillary data 常數。
    if (level == 41 && family == AF_INET6) return {IPPROTO_IPV6, 62};
#endif
#endif
    return {};
}
} // namespace x64arm::socket_detail
