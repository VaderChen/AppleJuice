#pragma once
#include <stdint.h>
#include <unicode/uldnames.h>

namespace x64arm::icu_display_names {
inline constexpr int32_t context_count=UDISPCTX_TYPE_SUBSTITUTE_HANDLING+1;

// 以四個公開類別正規化選項。同類別後出現的有效選項覆蓋前項。
struct Contexts {
    UDisplayContext items[context_count]{UDISPCTX_STANDARD_NAMES,UDISPCTX_CAPITALIZATION_NONE,
                                        UDISPCTX_LENGTH_FULL,UDISPCTX_SUBSTITUTE};
    constexpr void apply(int32_t input){
        if(input<0)return;
        const auto type=input>>8;
        constexpr int32_t maxima[]{UDISPCTX_DIALECT_NAMES,UDISPCTX_CAPITALIZATION_FOR_STANDALONE,
                                  UDISPCTX_LENGTH_SHORT,UDISPCTX_NO_SUBSTITUTE};
        if(type<context_count && input<=maxima[type])items[type]=static_cast<UDisplayContext>(input);
    }
    constexpr UDisplayContext get(int32_t type)const{
        return type>=0 && type<context_count?items[type]:UDISPCTX_STANDARD_NAMES;
    }
};

struct View {
    static constexpr uint32_t signature=0x314e444c;
    uint32_t magic{signature};
    Contexts contexts;
};
static_assert(sizeof(UDisplayContext)==4 && sizeof(View)==20 && alignof(View)==4);
}
