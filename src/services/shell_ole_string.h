#pragma once
#include "shell_quote_text.h"

/* NT 的 OLE 與 Shell 字串都是 UTF-16。容量包含 NUL，回傳值只計算內容。
   容量為零時 lstrcpynW 保留目的地，後續 lstrlenW 仍量出原有長度。 */
static inline unsigned shell_ole_string(void *ctx, ShellQuoteAddress dst, unsigned capacity,
    ShellQuoteAddress src, ShellQuoteRead read_unit, ShellQuoteWrite write_unit)
{
    if (!dst) return 0;
    unsigned length = 0;
    if (!capacity) {
        while (read_unit(ctx, dst + 2ULL * length, 2)) ++length;
        return length;
    }
    while (length < capacity - 1) {
        if (!src) return 0;
        unsigned ch = read_unit(ctx, src + 2ULL * length, 2);
        if (!ch) break;
        write_unit(ctx, dst + 2ULL * length, 2, ch);
        ++length;
    }
    write_unit(ctx, dst + 2ULL * length, 2, 0);
    return length;
}
