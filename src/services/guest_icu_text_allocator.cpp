// 客體 ICU 使用 Windows 相容層既有的程序 heap。
#include <stddef.h>
#include <stdint.h>

extern "C" {
__declspec(dllimport) void* __stdcall GetProcessHeap();
__declspec(dllimport) void* __stdcall HeapAlloc(void*,uint32_t,size_t);
__declspec(dllimport) int __stdcall HeapFree(void*,uint32_t,void*);
__declspec(dllimport) void* __stdcall HeapReAlloc(void*,uint32_t,void*,size_t);

void* uprv_malloc(size_t size){
    return HeapAlloc(GetProcessHeap(),0,size);
}

void* uprv_calloc(size_t count,size_t size){
    if(size && count>SIZE_MAX/size)return nullptr;
    constexpr uint32_t heap_zero_memory=8;
    return HeapAlloc(GetProcessHeap(),heap_zero_memory,count*size);
}

void uprv_free(void* address){
    if(address)HeapFree(GetProcessHeap(),0,address);
}

void* uprv_realloc(void* address,size_t size){
    if(!address)return uprv_malloc(size);
    if(!size){uprv_free(address);return nullptr;}
    return HeapReAlloc(GetProcessHeap(),0,address,size);
}
}
