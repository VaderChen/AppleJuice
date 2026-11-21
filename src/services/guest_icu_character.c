#include "guest_icu_character_tables.h"
#include "icu_set_operations.h"

#define ICU_EXPORT(type) __declspec(dllexport) type

enum {
#define ICU_PREDICATE(symbol) symbol##_bit,
#define ICU_BYTE_PROPERTY(symbol, byte, type)
#define ICU_SCALAR(symbol)
#include "icu_character_scalars.inc"
#undef ICU_PREDICATE
#undef ICU_BYTE_PROPERTY
#undef ICU_SCALAR

    icu_predicate_count
};
_Static_assert(icu_predicate_count<=32,"Unicode character predicate table");

static const uint32_t *codepoint_record(int32_t codepoint) {
    const uint32_t selected=(uint32_t)codepoint;
    if(selected>=UINT32_C(0x110000))return icu_invalid_codepoint;
    unsigned first=0,last=sizeof(icu_codepoint_ranges)/sizeof(icu_codepoint_ranges[0]);
    while(last-first>1) {
        const unsigned middle=first+(last-first)/2;
        if((icu_codepoint_ranges[middle]>>ICU_CODEPOINT_RECORD_BITS)<=selected)first=middle;
        else last=middle;
    }
    return icu_codepoint_records[icu_codepoint_ranges[first]&((1U<<ICU_CODEPOINT_RECORD_BITS)-1)];
}

#define ICU_PREDICATE(symbol) \
    ICU_EXPORT(int8_t) symbol(int32_t c) {return (int8_t)((codepoint_record(c)[0]>>symbol##_bit)&1U);}
#define ICU_BYTE_PROPERTY(symbol, byte, type) \
    ICU_EXPORT(type) symbol(int32_t c) {return (type)((codepoint_record(c)[1]>>(8*byte))&UINT32_C(0xff));}
#define ICU_SCALAR(symbol)
#include "icu_character_scalars.inc"
#undef ICU_PREDICATE
#undef ICU_BYTE_PROPERTY
#undef ICU_SCALAR

ICU_EXPORT(int8_t) uset_contains(const IcuSetView* view,int32_t codepoint) {
    return icu_set_find(view,codepoint)!=NULL;
}

ICU_EXPORT(int8_t) uset_containsRange(const IcuSetView* view,int32_t start,int32_t end) {
    const IcuSetRange* range=icu_set_find(view,start);
    return range && end<range->limit;
}

ICU_EXPORT(int32_t) uset_indexOf(const IcuSetView* view,int32_t codepoint) {
    const IcuSetRange* range=icu_set_find(view,codepoint);
    return range?range->index+codepoint-range->start:-1;
}

ICU_EXPORT(int32_t) uset_charAt(const IcuSetView* view,int32_t index) {
    return icu_set_character_at(view,index);
}

ICU_EXPORT(int32_t) uset_getRangeCount(const IcuSetView* view) {return view?view->range_count:0;}
ICU_EXPORT(int32_t) uset_getItemCount(const IcuSetView* view) {return view?view->item_count:0;}
ICU_EXPORT(int32_t) uset_size(const IcuSetView* view) {return view?view->size:0;}
ICU_EXPORT(int8_t) uset_isEmpty(const IcuSetView* view) {return !view || !view->size;}
ICU_EXPORT(int8_t) uset_isFrozen(const IcuSetView* view) {return view && (view->flags&ICU_SET_FROZEN);}
ICU_EXPORT(int8_t) uset_hasStrings(const IcuSetView* view) {return view && (view->flags&ICU_SET_HAS_STRINGS);}
