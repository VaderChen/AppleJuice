#pragma once
#include <stddef.h>
#include <stdint.h>

/* USet 對客體仍為 opaque。查詢使用宿主 ICU 發布的區間與累計碼點數。 */
typedef struct IcuSetRange {
    int32_t start;
    int32_t limit;
    int32_t index;
} IcuSetRange;

typedef struct IcuSetView {
    uint64_t ranges;
    int32_t range_count;
    int32_t item_count;
    int32_t size;
    uint32_t flags;
} IcuSetView;

enum {ICU_SET_FROZEN=1, ICU_SET_HAS_STRINGS=2};

#ifdef __cplusplus
static_assert(sizeof(IcuSetView)==24 && offsetof(IcuSetView,flags)==20);
static_assert(sizeof(IcuSetRange)==12);
#else
_Static_assert(sizeof(IcuSetView)==24 && offsetof(IcuSetView,flags)==20,"USet view ABI");
_Static_assert(sizeof(IcuSetRange)==12,"USet range ABI");
#endif

static inline const IcuSetRange* icu_set_find(const IcuSetView* view,int32_t codepoint) {
    if(!view || (uint32_t)codepoint>UINT32_C(0x10ffff))return NULL;
    const IcuSetRange* ranges=(const IcuSetRange*)(uintptr_t)view->ranges;
    int32_t first=0,last=view->range_count;
    while(first<last){
        const int32_t middle=first+(last-first)/2;
        const IcuSetRange* range=ranges+middle;
        if(codepoint<range->start)last=middle;
        else if(codepoint>=range->limit)first=middle+1;
        else return range;
    }
    return NULL;
}

static inline int32_t icu_set_character_at(const IcuSetView* view,int32_t index) {
    if(!view || index<0)return -1;
    const IcuSetRange* ranges=(const IcuSetRange*)(uintptr_t)view->ranges;
    int32_t first=0,last=view->range_count;
    while(first<last){
        const int32_t middle=first+(last-first)/2;
        const IcuSetRange* range=ranges+middle;
        if(index<range->index)last=middle;
        else {
            const int32_t offset=index-range->index;
            if(offset<range->limit-range->start)return range->start+offset;
            first=middle+1;
        }
    }
    return -1;
}
