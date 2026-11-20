#pragma once

enum ShellPathQuery { ShellRelative, ShellUNC, ShellServer, ShellShare, ShellUNCServerAddress };
static inline unsigned long long shell_unc_query(void *ctx, unsigned long long path, unsigned unit,
    enum ShellPathQuery query, unsigned (*read_unit)(void *, unsigned long long, unsigned))
{
    if (!path) return query == ShellRelative;
    unsigned first = read_unit(ctx,path,unit);
    if (query == ShellRelative)
        return !first || (first != '\\' && read_unit(ctx,path+unit,unit) != ':');
    if (first != '\\' || read_unit(ctx,path+unit,unit) != '\\') return 0;
    if (query == ShellUNC) return 1;

    unsigned long long server = path + 2 * unit;
    unsigned c = read_unit(ctx,server,unit);
    if (query == ShellUNCServerAddress) {
        if (c == '.' && read_unit(ctx,server+unit,unit) == '\\') return 0;
        if (c != '?') return server;
        const char suffix[] = "\\UNC\\";
        for (unsigned i = 0; i < sizeof(suffix)-1; ++i) {
            unsigned value = read_unit(ctx,server+(i+1)*unit,unit);
            if (value >= 'a' && value <= 'z') value -= 'a'-'A';
            if (value != (unsigned char)suffix[i]) return 0;
        }
        return path+8*unit;
    }
    unsigned long long p = server;
    while (c && c != '\\') {
        p += unit;
        c = read_unit(ctx,p,unit);
    }
    if (query == ShellServer) return !c;
    if (!c || p == server || (p == server+unit && read_unit(ctx,server,unit) == '?')) return 0;
    unsigned long long share = p+unit;
    for (p = share; ; p += unit) {
        c = read_unit(ctx,p,unit);
        if (!c) return p != share;
        if (c == '\\') return 0;
    }
}
