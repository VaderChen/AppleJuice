/* CRT 線性搜尋的兩種比較器都以客體 cdecl 呼叫。有效輸入留在客體，
   驗證失敗時透過同一個 CRT 更新 errno 並執行目前的無效參數處理器。 */
typedef __SIZE_TYPE__ Size;
typedef int (__cdecl *Compare)(const void*, const void*);
typedef int (__cdecl *CompareWithContext)(void*, const void*, const void*);

__declspec(dllimport) void* __cdecl memcpy(void*, const void*, Size);
__declspec(dllimport) int __cdecl _set_errno(int);
__declspec(dllimport) void __cdecl _invalid_parameter_noinfo(void);

static void* search(const void* key, const void* base, unsigned* count, Size width,
                    Compare plain, CompareWithContext contextual, void* context, int append) {
    if (!key || !count || (!base && (append || *count)) || !width || (!plain && !contextual)) {
        _set_errno(22);
        _invalid_parameter_noinfo();
        return 0;
    }
    unsigned remaining = *count;
    unsigned char* element = (unsigned char*)base;
    while (remaining--) {
        const int order = contextual ? contextual(context, key, element) : plain(key, element);
        if (!order) return element;
        element += width;
    }
    if (!append) return 0;
    memcpy(element, key, width);
    ++*count;
    return element;
}

#define ENTRY __declspec(dllexport)
ENTRY void* __cdecl _lfind(const void* key, const void* base, unsigned* count, unsigned width, Compare cmp) {
    return search(key, base, count, width, cmp, 0, 0, 0);
}
ENTRY void* __cdecl _lsearch(const void* key, void* base, unsigned* count, unsigned width, Compare cmp) {
    return search(key, base, count, width, cmp, 0, 0, 1);
}
ENTRY void* __cdecl _lfind_s(const void* key, const void* base, unsigned* count, Size width, CompareWithContext cmp, void* context) {
    return search(key, base, count, width, 0, cmp, context, 0);
}
ENTRY void* __cdecl _lsearch_s(const void* key, void* base, unsigned* count, Size width, CompareWithContext cmp, void* context) {
    return search(key, base, count, width, 0, cmp, context, 1);
}
