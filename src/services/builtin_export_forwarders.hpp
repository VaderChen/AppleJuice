#pragma once

#include "builtin_crt_forwarded_exports.hpp"

namespace x64arm::detail {

#include "builtin_d3dx10_forwarded_names.inc"

inline constexpr std::span<const ForwardedExports> builtin_forwarder_tables[]{d3dx10_forwarders,crt_forwarders};

inline std::span<const ForwardedExports> builtin_forwarders(std::string_view source){
    for(const auto table:builtin_forwarder_tables){
        const auto range=std::ranges::equal_range(table,source,{},&ForwardedExports::source);
        if(!range.empty())return {range.begin(),range.end()};
    }
    return {};
}

}
