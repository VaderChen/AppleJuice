#pragma once
#include "shell_quote_text.h"

/* Shell32 序號 58 在 NT 使用 UTF-16。len 是可複製的碼元數，終止字元另計。 */
static inline unsigned shell_parse_field(void *ctx, ShellQuoteAddress src, unsigned field,
    ShellQuoteAddress dst, unsigned len, ShellQuoteRead read_unit, ShellQuoteWrite write_unit)
{
    if (!src || !dst || !len) return 0;
    unsigned ch = read_unit(ctx, src, 2);
    if (!ch) return 0;
    while (field > 1) {
        if (!ch) return 0;
        if (ch == ',') --field;
        src += 2;
        ch = read_unit(ctx, src, 2);
    }
    while (len && ch && ch != ',') {
        write_unit(ctx, dst, 2, ch);
        dst += 2;
        src += 2;
        --len;
        if (len) ch = read_unit(ctx, src, 2);
    }
    write_unit(ctx, dst, 2, 0);
    return 1;
}
