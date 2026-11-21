// 私有 ICU 映像的 freestanding 記憶體與字串原語。
#include <stddef.h>
#include <stdint.h>

namespace {
void copy_forward(unsigned char* destination,const unsigned char* source,size_t size){
    for(size_t i=0;i<size;++i)destination[i]=source[i];
}
}

extern "C" {

void* memcpy(void* destination,const void* source,size_t size){
    copy_forward(static_cast<unsigned char*>(destination),static_cast<const unsigned char*>(source),size);
    return destination;
}

void* memmove(void* destination,const void* source,size_t size){
    auto* output=static_cast<unsigned char*>(destination);
    const auto* input=static_cast<const unsigned char*>(source);
    if(reinterpret_cast<uintptr_t>(output)>reinterpret_cast<uintptr_t>(input))
        while(size){--size;output[size]=input[size];}
    else
        copy_forward(output,input,size);
    return destination;
}

void* memset(void* destination,int value,size_t size){
    auto* output=static_cast<unsigned char*>(destination);
    for(size_t i=0;i<size;++i)output[i]=static_cast<unsigned char>(value);
    return destination;
}

size_t strlen(const char* text){
    const auto* end=text;
    while(*end)++end;
    return static_cast<size_t>(end-text);
}

size_t wcslen(const wchar_t* text){
    const auto* end=text;
    while(*end)++end;
    return static_cast<size_t>(end-text);
}

int32_t uprv_min(int32_t left,int32_t right){
    return left<right?left:right;
}

}
