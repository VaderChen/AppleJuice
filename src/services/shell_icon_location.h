#pragma once
#include "shell_quote_text.h"

static inline void shell_remove_blanks(void *ctx, ShellQuoteAddress path, unsigned unit,
                                     ShellQuoteRead read_unit, ShellQuoteWrite write_unit)
{
    if (!path) return;
    ShellQuoteAddress src = path, dst = path, end = path;
    unsigned c = read_unit(ctx, src, unit);
    if (!c) return;
    while (c == ' ') {
        src += unit;
        c = read_unit(ctx, src, unit);
    }
    while (c) {
        write_unit(ctx, dst, unit, c);
        dst += unit;
        if (c != ' ') end = dst;
        src += unit;
        c = read_unit(ctx, src, unit);
    }
    write_unit(ctx, end, unit, 0);
}

static inline unsigned shell_parse_icon_location(void *ctx, ShellQuoteAddress path, unsigned unit,
                                                 ShellQuoteRead read_unit, ShellQuoteWrite write_unit)
{
    if (!path) return 0;
    ShellQuoteAddress p = path;
    unsigned c, index = 0;
    while ((c = read_unit(ctx, p, unit)) && c != ',') p += unit;
    if (c == ',') {
        write_unit(ctx, p, unit, 0);
        p += unit;
        c = read_unit(ctx, p, unit);
        int negative = c == '-';
        if (negative) {
            p += unit;
            c = read_unit(ctx, p, unit);
        }
        while (c >= '0' && c <= '9') {
            index = index * 10u + (c - '0');
            p += unit;
            c = read_unit(ctx, p, unit);
        }
        if (negative) index = 0u - index;
    }
    /* 參考實作 先去除成對引號，再刪除路徑前後的 ASCII 空格。 */
    shell_quote_text(ctx, path, unit, ShellUnquoteSpaces, read_unit, write_unit);
    shell_remove_blanks(ctx, path, unit, read_unit, write_unit);
    return index;
}
