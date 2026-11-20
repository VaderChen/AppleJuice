#pragma once
enum ShellPidlQuery { ShellPidlSize, ShellPidlNext, ShellPidlLast };

/* 下一段可以是結尾的兩個零位元組，呼叫端可直接比較其位址。 */
static inline unsigned long long shell_pidl_walk(void *context, unsigned long long list,
    enum ShellPidlQuery query, unsigned (*length)(void *, unsigned long long))
{
    if (!list) return 0;
    unsigned long long current = list, last = list;
    for (;;) {
        unsigned cb = length(context, current);
        if (query == ShellPidlNext) return cb ? current + cb : 0;
        if (!cb) return query == ShellPidlLast ? last : current - list + 2;
        if (cb < 2 || current - list + cb + 2 > 0xffffffffULL) return 0;
        last = current;
        current += cb;
    }
}
