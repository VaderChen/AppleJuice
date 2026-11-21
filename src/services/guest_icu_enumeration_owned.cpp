#include <unicode/uenum.h>
#include <uenumimp.h>
#include <stddef.h>
#include <stdint.h>

extern "C" void* uprv_malloc(size_t);
extern "C" void uprv_free(void*);
extern "C" void* memcpy(void*,const void*,size_t);

namespace {
void close_owned(UEnumeration* item){
    uprv_free(item->context);
    uprv_free(item);
}
}

// 宿主 provider 傳遞 NUL 分隔的快照；列舉器與快照皆使用客體程序 heap。
extern "C" UEnumeration* x64arm_uenum_open_owned(const char* values,uint32_t bytes,int32_t count,UErrorCode* status){
    if(!status || U_FAILURE(*status))return nullptr;
    if(count<0 || (bytes && !values) || static_cast<uint32_t>(count)>bytes ||
       static_cast<uint32_t>(count)>(SIZE_MAX-bytes)/sizeof(char*)){
        *status=U_ILLEGAL_ARGUMENT_ERROR;
        return nullptr;
    }
    const auto pointer_bytes=static_cast<size_t>(count)*sizeof(char*);
    auto** entries=static_cast<const char**>(uprv_malloc(pointer_bytes+bytes));
    if(!entries){*status=U_MEMORY_ALLOCATION_ERROR;return nullptr;}
    auto* text=reinterpret_cast<char*>(entries)+pointer_bytes;
    if(bytes)memcpy(text,values,bytes);
    unsigned offset=0;
    for(int32_t index=0;index<count;++index){
        if(offset>=bytes){uprv_free(entries);*status=U_ILLEGAL_ARGUMENT_ERROR;return nullptr;}
        entries[index]=text+offset;
        while(offset<bytes && text[offset])++offset;
        if(offset==bytes){uprv_free(entries);*status=U_ILLEGAL_ARGUMENT_ERROR;return nullptr;}
        ++offset;
    }
    auto* result=uenum_openCharStringsEnumeration(entries,count,status);
    if(!result){uprv_free(entries);return nullptr;}
    result->close=close_owned;
    return result;
}
