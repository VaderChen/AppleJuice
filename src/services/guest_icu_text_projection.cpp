#include "icu_text_projection_layout.hpp"
#include <stddef.h>
#include <unicode/utext.h>

extern "C" void* uprv_malloc(size_t);
extern "C" void uprv_free(void*);

namespace {
using x64arm::icu_text::CodePoint;
using x64arm::icu_text::Snapshot;

struct TextClone {
    UText value=UTEXT_INITIALIZER;
    ~TextClone(){utext_close(&value);}
};
struct SnapshotBuffer {
    Snapshot* data{};
    size_t capacity{};
    ~SnapshotBuffer(){uprv_free(data);}
    CodePoint* points()const{return reinterpret_cast<CodePoint*>(data+1);}

    bool reserve(size_t desired){
        if(desired>(SIZE_MAX-sizeof(Snapshot))/sizeof(CodePoint))return false;
        auto* replacement=static_cast<Snapshot*>(uprv_malloc(sizeof(Snapshot)+desired*sizeof(CodePoint)));
        if(!replacement)return false;
        *replacement=data?*data:Snapshot{};
        auto* next=reinterpret_cast<CodePoint*>(replacement+1);
        for(uint32_t i=0;i<replacement->count;++i)next[i]=points()[i];
        uprv_free(data);
        data=replacement;
        capacity=desired;
        return true;
    }
};
}

// 以淺複製保留原迭代位置，並在客體內執行原 provider 與 UTF-8 索引轉換。
extern "C" Snapshot* x64arm_utext_project(const UText* source,UErrorCode* error){
    if(!error || U_FAILURE(*error))return nullptr;
    if(!source || source->magic!=UTEXT_MAGIC || !source->pFuncs || !source->pFuncs->clone){
        *error=U_ILLEGAL_ARGUMENT_ERROR;
        return nullptr;
    }
    TextClone clone;
    if(!utext_clone(&clone.value,source,false,true,error) || U_FAILURE(*error))return nullptr;
    const auto length=utext_nativeLength(&clone.value);
    if(length<0 || length>INT32_MAX){
        *error=U_INDEX_OUTOFBOUNDS_ERROR;
        return nullptr;
    }
    const auto end=static_cast<uint32_t>(length);
    utext_setNativeIndex(&clone.value,0);
    SnapshotBuffer buffer;
    if(!buffer.reserve(end<64?end:64)){
        *error=U_MEMORY_ALLOCATION_ERROR;
        return nullptr;
    }
    buffer.data->native_length=end;
    int64_t start=0;
    if(utext_getNativeIndex(&clone.value)!=0){
        *error=U_INVALID_FORMAT_ERROR;
        return nullptr;
    }
    while(start<length){
        const auto code=utext_next32(&clone.value);
        const auto next=utext_getNativeIndex(&clone.value);
        if(code<0 || code>0x10ffff || next<=start || next>length){
            *error=U_INVALID_FORMAT_ERROR;
            return nullptr;
        }
        if(buffer.data->count==buffer.capacity){
            const auto size=buffer.capacity<=end/2?buffer.capacity*2:end;
            if(!buffer.reserve(size)){
                *error=U_MEMORY_ALLOCATION_ERROR;
                return nullptr;
            }
        }
        buffer.points()[buffer.data->count++]={static_cast<uint32_t>(start),static_cast<uint32_t>(code)};
        start=next;
    }
    auto* result=buffer.data;
    buffer.data=nullptr;
    return result;
}
