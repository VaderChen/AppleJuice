#pragma once

/* A 版保留 UTF-8 位元組位移。只有後接非斜線字元的分隔符號才開始新的檔名。 */
static inline unsigned long long shell_file_name(void *ctx, unsigned long long path, unsigned unit,
    int strip, unsigned (*read_unit)(void *, unsigned long long, unsigned),
    void (*write_unit)(void *, unsigned long long, unsigned, unsigned))
{
    if (!path) return 0;
    unsigned long long name = path, end = path;
    int previous_separator = 0;
    for (;; end += unit) {
        unsigned c = read_unit(ctx, end, unit);
        if (!c) break;
        int slash = c == '/' || c == '\\';
        if (previous_separator && !slash) name = end;
        previous_separator = slash || c == ':';
    }
    if (!strip) return name;
    if (name != path) {
        for (unsigned long long p = name; p <= end; p += unit)
            write_unit(ctx, path + (p - name), unit, read_unit(ctx, p, unit));
    }
    return 0;
}
