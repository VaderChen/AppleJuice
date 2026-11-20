#pragma once
enum ShellExtensionAction { ShellFindExtension, ShellGetExtension, ShellRemoveExtension, ShellAddExtension, ShellRenameExtension };

/* A 版保留原始 ACP 位元組位移，W 版以 UTF-16 碼元為單位。 */
static inline unsigned long long shell_extension_text(void *ctx, unsigned long long path,
    unsigned long long ext, unsigned unit, enum ShellExtensionAction action,
    unsigned (*read_unit)(void *, unsigned long long, unsigned),
    void (*write_unit)(void *, unsigned long long, unsigned, unsigned))
{
    if (!path && action != ShellGetExtension) return 0;
    unsigned long long dot = 0, end = path;
    for (;; end += unit) {
        unsigned c = read_unit(ctx, end, unit);
        if (!c) break;
        if (c == '.' ) dot = end;
        else if (c == ' ' || c == '\\') dot = 0;
    }
    unsigned long long at = dot ? dot : end;
    if (action == ShellFindExtension) return at;
    if (action == ShellGetExtension) return dot ? dot + unit : end;
    if (action == ShellRemoveExtension) {
        if (dot) write_unit(ctx, dot, unit, 0);
        return 0;
    }
    if ((action == ShellAddExtension && dot) || (action == ShellRenameExtension && !ext)) return 0;
    unsigned long long prefix = (at - path) / unit;
    if (prefix >= 260) return 0;
    /* 沿用本專案既有 PathAddExtension 的空副檔名預設值。 */
    const int use_default = action == ShellAddExtension && (!ext || !read_unit(ctx, ext, unit));
    unsigned short suffix[260];
    for (unsigned i = 0; i < 260 - prefix; ++i) {
        unsigned c = use_default ? (unsigned char)".exe"[i] : read_unit(ctx, ext + i * unit, unit);
        suffix[i] = (unsigned short)c;
        if (!c) {
            for (unsigned j = 0; j <= i; ++j) write_unit(ctx, at + j * unit, unit, suffix[j]);
            return 1;
        }
    }
    return 0;
}

/* NT 的 PathIsExe 以 PathGetExtensionW 決定副檔名位置。 */
static inline unsigned shell_is_exe(void *ctx, unsigned long long path,
    unsigned (*read_unit)(void *, unsigned long long, unsigned))
{
    unsigned long long ext = shell_extension_text(ctx, path, 0, 2, ShellGetExtension, read_unit, 0);
    static const char suffixes[][4] = {"exe", "com", "pif", "cmd", "bat", "scf", "scr"};
    char folded[4];
    for (unsigned i = 0; i < 4; ++i) {
        unsigned c = read_unit(ctx, ext + 2 * i, 2);
        if (i == 3) {
            if (c) return 0;
        } else if (!c || c > 127) return 0;
        folded[i] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    for (unsigned i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i)
        if (folded[0] == suffixes[i][0] && folded[1] == suffixes[i][1] &&
            folded[2] == suffixes[i][2]) return 1;
    return 0;
}
