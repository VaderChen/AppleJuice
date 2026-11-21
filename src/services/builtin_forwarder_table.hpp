#pragma once
#include <algorithm>
#include <span>
#include <string_view>

namespace x64arm::detail {

struct ForwardedExportName {
    std::string_view name;
    unsigned architectures; // bit 0 為 x86，bit 1 為 x64。
    std::string_view target_name{};

    std::string_view destination() const {return target_name.empty()?name:target_name;}
};

struct ForwardedExports {
    std::string_view source;
    std::string_view target;
    std::span<const ForwardedExportName> symbols;

    std::string_view destination(std::string_view symbol,bool x86) const {
        const auto matches=std::ranges::equal_range(symbols,symbol,{},&ForwardedExportName::name);
        const auto selected=std::ranges::find_if(matches,[&](const auto& entry){
            return (entry.architectures&(x86?1U:2U))!=0;
        });
        return selected!=matches.end()?selected->destination():std::string_view{};
    }
};

}
