#include "guest_icu_currency_tables.hpp"
#include <unicode/ucurr.h>

namespace {
using namespace x64arm::icu_currency;

const CurrencyData* lookup(const UChar* currency,bool numeric){
    if(!currency)return nullptr;
    unsigned key=0;
    for(unsigned i=0;i<3;++i){
        auto letter=static_cast<unsigned>(currency[i]);
        if(numeric && letter-'a'<26)letter-='a'-'A';
        if(letter-'A'>=26)return nullptr;
        key=key*26+letter-'A';
    }
    if(numeric && currency[3])return nullptr;
    unsigned lower=0,upper=sizeof(currencies)/sizeof(*currencies);
    while(lower<upper){
        const auto middle=(lower+upper)/2;
        if(currencies[middle].key<key)lower=middle+1;
        else upper=middle;
    }
    return lower<sizeof(currencies)/sizeof(*currencies) && currencies[lower].key==key?&currencies[lower]:nullptr;
}
}

extern "C" int32_t ucurr_getNumericCode(const UChar* currency){
    const auto* item=lookup(currency,true);
    return item?item->numeric:0;
}

extern "C" int32_t ucurr_getDefaultFractionDigitsForUsage(const UChar* currency,UCurrencyUsage usage,UErrorCode* error){
    if(!error || U_FAILURE(*error))return 0;
    if(usage!=UCURR_USAGE_STANDARD && usage!=UCURR_USAGE_CASH){
        *error=U_UNSUPPORTED_ERROR;
        return 0;
    }
    if(!currency || !*currency){*error=U_ILLEGAL_ARGUMENT_ERROR;return 2;}
    const auto* item=lookup(currency,false);
    if(!item)return 2;
    return usage==UCURR_USAGE_STANDARD?item->standard_digits:item->cash_digits;
}

extern "C" int32_t ucurr_getDefaultFractionDigits(const UChar* currency,UErrorCode* error){
    return ucurr_getDefaultFractionDigitsForUsage(currency,UCURR_USAGE_STANDARD,error);
}

extern "C" double ucurr_getRoundingIncrementForUsage(const UChar* currency,UCurrencyUsage usage,UErrorCode* error){
    if(!error || U_FAILURE(*error))return 0.0;
    if(!currency || !*currency){*error=U_ILLEGAL_ARGUMENT_ERROR;return 0.0;}
    if(usage!=UCURR_USAGE_STANDARD && usage!=UCURR_USAGE_CASH){
        *error=U_UNSUPPORTED_ERROR;
        return 0.0;
    }
    const auto* item=lookup(currency,false);
    if(!item)return 0.0;
    return usage==UCURR_USAGE_STANDARD?item->standard_increment:item->cash_increment;
}

extern "C" double ucurr_getRoundingIncrement(const UChar* currency,UErrorCode* error){
    return ucurr_getRoundingIncrementForUsage(currency,UCURR_USAGE_STANDARD,error);
}
