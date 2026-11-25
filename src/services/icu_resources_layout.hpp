#pragma once
#include <stdint.h>
#include <unicode/ures.h>

namespace x64arm::icu_resources {
struct alignas(8) View {
    static constexpr uint32_t signature=0x31534552;
    uint64_t key{};
    int32_t size{};
    int32_t type{URES_NONE};
    uint32_t has_next{};
    uint32_t magic{signature};
};
static_assert(sizeof(View)==24);

struct Utf8Window {
    int32_t size{};
    int32_t offset{};
};

constexpr Utf8Window utf8_window(int32_t length,int32_t capacity,bool force){
    if(capacity<length)return {};
    const auto bound=3*int64_t(length)+1;
    if(capacity<=bound)return {capacity,0};
    const auto maximum=static_cast<int32_t>(bound);
    return {maximum,force?0:capacity-maximum};
}
}
