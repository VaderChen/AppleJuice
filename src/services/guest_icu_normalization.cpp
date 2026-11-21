#include "guest_icu_normalization_tables.hpp"
#include <unicode/unorm2.h>

namespace {
using namespace x64arm::icu_normalization;
const GuestNormalizer& view(const UNormalizer2* normalizer){
    return *reinterpret_cast<const GuestNormalizer*>(normalizer);
}

uint64_t record(uint32_t code){
    if(code>0x10ffff)return invalid_code;
    unsigned lower=0,upper=sizeof(ranges)/sizeof(*ranges);
    while(lower+1<upper){
        const auto middle=(lower+upper)/2;
        if((ranges[middle]>>7)<=code)lower=middle;
        else upper=middle;
    }
    return records[ranges[lower]&127];
}

uint32_t properties(const GuestNormalizer& data,UChar32 code){
    return (record(static_cast<uint32_t>(code))>>(8+3*data.profile))&7;
}

template<auto Function,GuestQuery query,class... Args>
auto gateway(const UNormalizer2* normalizer,Args... args){
    return reinterpret_cast<decltype(Function)>(view(normalizer).gateway[query])(normalizer,args...);
}
}

extern "C" int32_t unorm2_composePair(const UNormalizer2* normalizer,UChar32 a,UChar32 b){
    if(!normalizer)return -1;
    const auto profile=view(normalizer).profile;
    if(profile>=profile_count)
        return gateway<unorm2_composePair,query_unorm2_composePair>(normalizer,a,b);
    const auto first=static_cast<uint32_t>(a),second=static_cast<uint32_t>(b);
    if(first-0x1100<19 && second-0x1161<21)return 0xac00+((first-0x1100)*21+(second-0x1161))*28;
    const auto syllable=first-0xac00,trail=second-0x11a7;
    if(syllable<11172 && syllable%28==0 && trail>0 && trail<28)return a+trail;
    if(first>0x10ffff || second>0x10ffff)return -1;
    unsigned lower=0,upper=sizeof(compositions)/sizeof(*compositions);
    while(lower<upper){
        const auto middle=(lower+upper)/2;
        const auto& item=compositions[middle];
        if(item.first<first || (item.first==first && item.second<second))lower=middle+1;
        else upper=middle;
    }
    for(unsigned index=lower;index<sizeof(compositions)/sizeof(*compositions);++index){
        const auto& item=compositions[index];
        if(item.first!=first || item.second!=second)break;
        if(item.profiles&(1U<<profile))return item.result;
    }
    return -1;
}

extern "C" uint8_t unorm2_getCombiningClass(const UNormalizer2* normalizer,UChar32 code){
    if(!normalizer)return 0;
    if(view(normalizer).profile>=profile_count)
        return gateway<unorm2_getCombiningClass,query_unorm2_getCombiningClass>(normalizer,code);
    return static_cast<uint8_t>(record(static_cast<uint32_t>(code)));
}

#define NORM_PROPERTY(name,bit) \
extern "C" UBool name(const UNormalizer2* normalizer,UChar32 code){ \
    if(!normalizer)return false; \
    const auto& data=view(normalizer); \
    if(data.profile>=profile_count)return gateway<name,query_##name>(normalizer,code); \
    return (properties(data,code)&bit)!=0; \
}
NORM_PROPERTY(unorm2_hasBoundaryAfter,2)
NORM_PROPERTY(unorm2_hasBoundaryBefore,1)
NORM_PROPERTY(unorm2_isInert,4)
#undef NORM_PROPERTY
