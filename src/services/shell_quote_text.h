#pragma once

/* 讀寫以客體碼元為單位。相同演算法供常駐客體入口及宿主慢路徑使用。 */
typedef unsigned long long ShellQuoteAddress;
typedef unsigned (*ShellQuoteRead)(void *, ShellQuoteAddress, unsigned);
typedef void (*ShellQuoteWrite)(void *, ShellQuoteAddress, unsigned, unsigned);
enum ShellQuoteAction { ShellExtractQuoted, ShellQuoteSpaces, ShellUnquoteSpaces, ShellFindArguments, ShellRemoveArguments };

static inline ShellQuoteAddress shell_quote_text(void *context, ShellQuoteAddress start, unsigned unit,
                                                enum ShellQuoteAction action, ShellQuoteRead read_unit,
                                                ShellQuoteWrite write_unit)
{
    if (action == ShellFindArguments || action == ShellRemoveArguments) {
        if (!start) return 0;
        ShellQuoteAddress p = start;
        int quoted = 0;
        for (;; p += unit) {
            unsigned c = read_unit(context, p, unit);
            if (!c) break;
            if (c == ' ' && !quoted) {
                p += unit;
                break;
            }
            if (c == '"') quoted = !quoted;
        }
        if (action == ShellFindArguments) return p;
        /* 未閉合引號內的尾端空白也會移除一個碼元。 */
        if (p > start && read_unit(context, p - unit, unit) == ' ')
            write_unit(context, p - unit, unit, 0);
        return 0;
    }
    if (action == ShellExtractQuoted) {
        if (read_unit(context, start, unit) == '"') {
            for (ShellQuoteAddress p = start + unit; ; p += unit) {
                unsigned c = read_unit(context, p, unit);
                if (!c) break;
                write_unit(context, p - unit, unit, c == '"' ? 0 : c);
                if (c == '"') break;
            }
        }
        return start;
    }
    if (!start) return 0;
    if (action == ShellUnquoteSpaces) {
        if (read_unit(context, start, unit) != '"') return 0;
        ShellQuoteAddress last = start;
        while (read_unit(context, last + unit, unit)) last += unit;
        if (read_unit(context, last, unit) != '"') return 0;
        write_unit(context, last, unit, 0);
        for (ShellQuoteAddress p = start; p < last; p += unit)
            write_unit(context, p, unit, read_unit(context, p + unit, unit));
        return 1;
    }
    unsigned count = 0, space = 0;
    for (; count < 257; ++count) {
        unsigned c = read_unit(context, start + count * unit, unit);
        if (!c) break;
        if (c == ' ') space = 1;
    }
    if (!space || count == 257) return 0;
    write_unit(context, start + (count + 2) * unit, unit, 0);
    write_unit(context, start + (count + 1) * unit, unit, '"');
    for (; count; --count)
        write_unit(context, start + count * unit, unit, read_unit(context, start + (count - 1) * unit, unit));
    write_unit(context, start, unit, '"');
    return 1;
}
