#pragma once
#include "crt_secure_fill.h"
#include <string_view>

namespace x64arm::crt_secure_set {
struct Entry {
    std::string_view name;
    CrtFillLimit limit;
    unsigned arguments;
    bool multibyte;
};
inline constexpr Entry entries[]{
    {"_strset_s", CrtFillAll, 3, false},          {"_mbsset_s", CrtFillAll, 3, true},
    {"_mbsset_s_l", CrtFillAll, 4, true},         {"_mbsnset_s", CrtFillCharacters, 4, true},
    {"_mbsnset_s_l", CrtFillCharacters, 5, true}, {"_mbsnbset_s", CrtFillBytes, 4, true},
    {"_mbsnbset_s_l", CrtFillBytes, 5, true},
};
inline constexpr const Entry* lookup(std::string_view name) {
    if (!name.starts_with("_mbs") && name != "_strset_s") return nullptr;
    for (const auto& entry : entries)
        if (entry.name == name) return &entry;
    return nullptr;
}
} // namespace x64arm::crt_secure_set
