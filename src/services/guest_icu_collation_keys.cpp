#include "icu_collation_key_math.hpp"

using namespace x64arm::icu_collation_key;
extern "C" int32_t ucol_primaryOrder(int32_t value){return static_cast<int32_t>(primary(static_cast<uint32_t>(value)));}
extern "C" int32_t ucol_secondaryOrder(int32_t value){return static_cast<int32_t>(secondary(static_cast<uint32_t>(value)));}
extern "C" int32_t ucol_tertiaryOrder(int32_t value){return static_cast<int32_t>(tertiary(static_cast<uint32_t>(value)));}
extern "C" int32_t ucol_keyHashCode(const uint8_t* key,int32_t length){
    const auto size=key && length>0?static_cast<uint32_t>(length):0U;
    return static_cast<int32_t>(hash(size,[&](uint32_t offset){return key[offset];}));
}
