#pragma once

#include <unicode/utext.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace x64arm::icu_break {
struct Projection {
    std::u16string units;
    std::vector<std::int32_t> native_offsets;
};
using SharedProjection=std::shared_ptr<const Projection>;

inline const Projection& projection(const UText* text){
    return *static_cast<const Projection*>(text->context);
}

inline std::int64_t U_CALLCONV native_length(UText* text){
    return projection(text).native_offsets.back();
}

inline std::int32_t U_CALLCONV map_index(const UText* text,std::int64_t index){
    const auto& offsets=projection(text).native_offsets;
    const auto upper=std::upper_bound(offsets.begin(),offsets.end(),index);
    auto unit=upper==offsets.begin()?0:static_cast<std::size_t>(upper-offsets.begin()-1);
    // 補充碼點的兩個 UTF-16 code unit 共用同一個原生開始索引。
    if(unit && offsets[unit-1]==offsets[unit])--unit;
    return static_cast<std::int32_t>(unit);
}

inline std::int64_t U_CALLCONV map_offset(const UText* text){
    return projection(text).native_offsets[static_cast<std::size_t>(text->chunkOffset)];
}

inline UBool U_CALLCONV access_chunk(UText* text,std::int64_t index,UBool forward){
    text->chunkOffset=map_index(text,index);
    return forward?(index>=0 && index<text->chunkNativeLimit):(index>0 && index<=text->chunkNativeLimit);
}

inline std::int32_t U_CALLCONV extract(UText* text,std::int64_t start,std::int64_t limit,
                                     UChar* output,std::int32_t capacity,UErrorCode* error){
    if(U_FAILURE(*error))return 0;
    if(capacity<0 || (!output && capacity) || start>limit){
        *error=U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }
    const auto first=map_index(text,start),last=map_index(text,limit);
    const auto length=last-first;
    if(const auto copied=std::min(length,capacity);copied>0)
        std::copy_n(projection(text).units.data()+first,copied,output);
    if(length<capacity)output[length]=0;
    else if(length>capacity)*error=U_BUFFER_OVERFLOW_ERROR;
    else if(length)*error=U_STRING_NOT_TERMINATED_WARNING;
    text->chunkOffset=last;
    return length;
}

inline UText* open_projection(UText*,SharedProjection,UErrorCode*);

inline UText* U_CALLCONV clone(UText* dest,const UText* source,UBool,UErrorCode* error){
    auto* result=open_projection(dest,*static_cast<const SharedProjection*>(source->pExtra),error);
    if(result)result->chunkOffset=source->chunkOffset;
    return result;
}

inline void U_CALLCONV close(UText* text){
    std::destroy_at(static_cast<SharedProjection*>(text->pExtra));
}

inline UText* open_projection(UText* text,SharedProjection buffer,UErrorCode* error){
    static const UTextFuncs funcs{sizeof(UTextFuncs),0,0,0,clone,native_length,access_chunk,extract,
                                 nullptr,nullptr,map_offset,map_index,close,nullptr,nullptr,nullptr};
    text=utext_setup(text,sizeof(SharedProjection),error);
    if(!text || U_FAILURE(*error))return nullptr;
    const auto& shared=*std::construct_at(static_cast<SharedProjection*>(text->pExtra),std::move(buffer));
    text->context=shared.get();
    text->pFuncs=&funcs;
    text->providerProperties=1<<UTEXT_PROVIDER_STABLE_CHUNKS;
    text->chunkNativeStart=0;
    text->chunkNativeLimit=shared->native_offsets.back();
    text->chunkContents=shared->units.data();
    text->chunkLength=static_cast<std::int32_t>(shared->units.size());
    text->nativeIndexingLimit=0;
    text->chunkOffset=0;
    return text;
}
}
