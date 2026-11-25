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
    size_t prefix{};
    void* allocation()const{return data?reinterpret_cast<uint8_t*>(data)-prefix:nullptr;}
    ~SnapshotBuffer(){uprv_free(allocation());}
    CodePoint* points()const{return reinterpret_cast<CodePoint*>(data+1);}

    bool reserve(size_t desired){
        if(desired>(SIZE_MAX-prefix-sizeof(Snapshot))/sizeof(CodePoint))return false;
        auto* bytes=static_cast<uint8_t*>(uprv_malloc(prefix+sizeof(Snapshot)+desired*sizeof(CodePoint)));
        if(!bytes)return false;
        auto* replacement=reinterpret_cast<Snapshot*>(bytes+prefix);
        *replacement=data?*data:Snapshot{};
        auto* next=reinterpret_cast<CodePoint*>(replacement+1);
        for(uint32_t i=0;i<replacement->count;++i)next[i]=points()[i];
        uprv_free(allocation());
        data=replacement;
        capacity=desired;
        return true;
    }
};

bool valid_source(const UText* source,UErrorCode* error){
    if(!error || U_FAILURE(*error))return false;
    if(source && source->magic==UTEXT_MAGIC && source->pFuncs && source->pFuncs->clone)return true;
    *error=U_ILLEGAL_ARGUMENT_ERROR;
    return false;
}

// 掃描專用的淺複本。需要保留提供者時，在快照前記錄客體複本的位址。
void* project(UText* text, bool retain,UErrorCode* error){
    const auto length=utext_nativeLength(text);
    if(length<0){
        *error=U_INDEX_OUTOFBOUNDS_ERROR;
        return nullptr;
    }
    const auto limit=static_cast<uint32_t>(length>INT32_MAX?INT32_MAX:length);
    utext_setNativeIndex(text,0);
    SnapshotBuffer buffer;
    buffer.prefix=retain?sizeof(uint64_t):0;
    if(!buffer.reserve(limit<64?limit:64)){
        *error=U_MEMORY_ALLOCATION_ERROR;
        return nullptr;
    }
    buffer.data->native_length=length;
    int64_t start=0;
    if(utext_getNativeIndex(text)!=0){
        *error=U_INVALID_FORMAT_ERROR;
        return nullptr;
    }
    while(start<length){
        const auto code=utext_next32(text);
        const auto next=utext_getNativeIndex(text);
        if(code<0 || code>0x10ffff || next<=start || next>length){
            *error=U_INVALID_FORMAT_ERROR;
            return nullptr;
        }
        if(buffer.data->count==buffer.capacity){
            if(buffer.capacity==limit){
                *error=U_INDEX_OUTOFBOUNDS_ERROR;
                return nullptr;
            }
            const auto size=buffer.capacity<=limit/2?buffer.capacity*2:limit;
            if(!buffer.reserve(size)){
                *error=U_MEMORY_ALLOCATION_ERROR;
                return nullptr;
            }
        }
        buffer.points()[buffer.data->count++]={start,static_cast<uint32_t>(code),0};
        start=next;
    }
    auto* result=buffer.allocation();
    if(retain)*static_cast<uint64_t*>(result)=reinterpret_cast<uintptr_t>(text);
    buffer.data=nullptr;
    return result;
}

}

extern "C" void* x64arm_utext_project(const UText* source,UErrorCode* error){
    if(!valid_source(source,error))return nullptr;
    TextClone clone;
    if(!utext_clone(&clone.value,source,false,true,error) || U_FAILURE(*error))return nullptr;
    return project(&clone.value,false,error);
}

extern "C" void* x64arm_utext_retain_project(const UText* source,UErrorCode* error){
    if(!valid_source(source,error))return nullptr;
    auto* clone=utext_clone(nullptr,source,false,true,error);
    if(clone && U_SUCCESS(*error)){
        const auto index=utext_getNativeIndex(clone);
        auto* result=project(clone,true,error);
        if(result){
            utext_setNativeIndex(clone,index);
            return result;
        }
    }
    utext_close(clone);
    return nullptr;
}

// 客體 UText 自行持有 UTF-16。淺複製會連同 pExtra 複製並重定位文字指標。
extern "C" UText* x64arm_utext_own_utf16(const UChar* source,int32_t length,UErrorCode* error){
    if(!error || U_FAILURE(*error))return nullptr;
    if(length<0 || (!source && length)){*error=U_ILLEGAL_ARGUMENT_ERROR;return nullptr;}
    const auto count=static_cast<uint64_t>(length)+1;
    if(count>SIZE_MAX/sizeof(UChar)){*error=U_MEMORY_ALLOCATION_ERROR;return nullptr;}
    const auto bytes=static_cast<size_t>(count)*sizeof(UChar);
    // utext_setup 的附加空間含結構與對齊，以帶號 32 位元計算。
    // 超出單塊配置時，改由 UChars 提供者接管獨立的文字儲存區。
    if(bytes>INT32_MAX-sizeof(UText)-64){
        auto* units=static_cast<UChar*>(uprv_malloc(bytes));
        if(!units){*error=U_MEMORY_ALLOCATION_ERROR;return nullptr;}
        auto* view=utext_openUChars(nullptr,units,length,error);
        if(!view){uprv_free(units);return nullptr;}
        for(int32_t i=0;i<length;++i)units[i]=source[i];
        units[length]=0;
        view->providerProperties|=1<<UTEXT_PROVIDER_OWNS_TEXT;
        return view;
    }
    TextClone prototype;
    if(!utext_openUChars(&prototype.value,source,length,error))return nullptr;
    auto* view=utext_setup(nullptr,static_cast<int32_t>(bytes),error);
    if(!view)return nullptr;
    view->pFuncs=prototype.value.pFuncs;
    auto* units=static_cast<UChar*>(view->pExtra);
    if(!utext_openUChars(view,units,length,error)){utext_close(view);return nullptr;}
    for(int32_t i=0;i<length;++i)units[i]=source[i];
    units[length]=0;
    return view;
}
