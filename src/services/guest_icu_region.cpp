#include "icu_region_layout.hpp"
#include <unicode/uregion.h>

using namespace x64arm::icu_region;

extern "C" const View x64arm_region_catalog[count]={
#define ICU_REGION_LAYOUT(...)
#define ICU_REGION_ENTRY(code,numeric,type,parent,...) {code,numeric,parent,type,0,0,{__VA_ARGS__}},
#define ICU_REGION_CODE(...)
#define ICU_REGION_NUMBER(...)
#include "guest_icu_region_catalog.inc"
#undef ICU_REGION_NUMBER
#undef ICU_REGION_CODE
#undef ICU_REGION_ENTRY
#undef ICU_REGION_LAYOUT
};

namespace {
struct IndexEntry {uint32_t key;uint16_t index;};

constexpr IndexEntry code_index[code_count]={
#define ICU_REGION_LAYOUT(...)
#define ICU_REGION_ENTRY(...)
#define ICU_REGION_CODE(key,index) {key,index},
#define ICU_REGION_NUMBER(...)
#include "guest_icu_region_catalog.inc"
#undef ICU_REGION_NUMBER
#undef ICU_REGION_CODE
#undef ICU_REGION_ENTRY
#undef ICU_REGION_LAYOUT
};

constexpr IndexEntry numeric_index[numeric_count]={
#define ICU_REGION_LAYOUT(...)
#define ICU_REGION_ENTRY(...)
#define ICU_REGION_CODE(...)
#define ICU_REGION_NUMBER(key,index) {key,index},
#include "guest_icu_region_catalog.inc"
#undef ICU_REGION_NUMBER
#undef ICU_REGION_CODE
#undef ICU_REGION_ENTRY
#undef ICU_REGION_LAYOUT
};

const View* checked(const URegion* region){
    const auto address=reinterpret_cast<uintptr_t>(region);
    const auto base=reinterpret_cast<uintptr_t>(x64arm_region_catalog);
    if(address<base || address-base>=sizeof(x64arm_region_catalog) || (address-base)%sizeof(View))return nullptr;
    return reinterpret_cast<const View*>(region);
}

const URegion* handle(uint16_t index){
    return index<count?reinterpret_cast<const URegion*>(x64arm_region_catalog+index):nullptr;
}

template<size_t N>
const URegion* lookup(const IndexEntry (&entries)[N],uint32_t key,UErrorCode* status){
    size_t low=0,high=N;
    while(low<high){
        const auto middle=low+(high-low)/2U;
        if(entries[middle].key<key)low=middle+1;
        else high=middle;
    }
    if(low<N && entries[low].key==key)return handle(entries[low].index);
    *status=U_ILLEGAL_ARGUMENT_ERROR;
    return nullptr;
}
}

extern "C" UBool uregion_areEqual(const URegion* left,const URegion* right){return left==right;}

extern "C" UBool uregion_contains(const URegion* outer,const URegion* inner){
    const auto* first=checked(outer);
    const auto* second=checked(inner);
    if(!first || !second)return false;
    const auto index=static_cast<size_t>(second-x64arm_region_catalog);
    return (first->descendants[index/64U]>>(index%64U))&1U;
}

extern "C" const URegion* uregion_getContainingRegion(const URegion* region){
    const auto* entry=checked(region);
    return entry?handle(entry->parent):nullptr;
}

extern "C" const URegion* uregion_getContainingRegionOfType(const URegion* region,URegionType type){
    const auto* entry=checked(region);
    for(auto parent=entry?entry->parent:no_region;parent<count;parent=x64arm_region_catalog[parent].parent)
        if(x64arm_region_catalog[parent].type==type)return handle(parent);
    return nullptr;
}

extern "C" int32_t uregion_getNumericCode(const URegion* region){
    const auto* entry=checked(region);
    return entry?entry->numeric:-1;
}

extern "C" const char* uregion_getRegionCode(const URegion* region){
    const auto* entry=checked(region);
    return entry?entry->code:nullptr;
}

extern "C" const URegion* uregion_getRegionFromCode(const char* code,UErrorCode* status){
    if(!status || U_FAILURE(*status))return nullptr;
    uint32_t key=0;
    if(code){
        for(unsigned n=0;n<3;++n){
            const auto character=static_cast<unsigned char>(code[n]);
            if(!character)return lookup(code_index,key,status);
            key=(key<<8U)|character;
        }
        if(!code[3])return lookup(code_index,key,status);
    }
    *status=U_ILLEGAL_ARGUMENT_ERROR;
    return nullptr;
}

extern "C" const URegion* uregion_getRegionFromNumericCode(int32_t numeric,UErrorCode* status){
    if(!status || U_FAILURE(*status))return nullptr;
    return lookup(numeric_index,static_cast<uint32_t>(numeric),status);
}

extern "C" URegionType uregion_getType(const URegion* region){
    const auto* entry=checked(region);
    return entry?static_cast<URegionType>(entry->type):URGN_UNKNOWN;
}
