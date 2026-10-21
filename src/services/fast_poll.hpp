#pragma once
#include <sys/select.h>
#include <sys/time.h>
#include <algorithm>

// 零逾時的就緒查詢。實測宿主（macOS 27／M4 Pro）上，kevent 取回事件與 poll 即使逾時為 0，
// 每次也要約 5–13 µs；select 只需約 0.2 µs（同條件的非阻塞 recv 約 0.2 µs）。
// 排程器在每個時間片都可能查詢外部通道，因此這類空輪詢改走 select；只有 fd 超出 FD_SETSIZE 時才退回原路徑。
namespace x64arm::fast_poll {
// 1＝可讀、0＝目前不可讀、-1＝無法判定（fd 無效、超出 FD_SETSIZE 或被訊號打斷），呼叫端須改走原本路徑。
inline int readable_now(int fd) noexcept {
    if (fd < 0 || fd >= FD_SETSIZE)
        return -1;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval zero{};
    const int ready = ::select(fd + 1, &set, nullptr, nullptr, &zero);
    return ready < 0 ? -1 : ready > 0 ? 1 : 0;
}
}
