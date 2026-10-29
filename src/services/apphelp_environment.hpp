#pragma once
#include "apphelp_cache_store.hpp"
#include <optional>
#include <string_view>

namespace x64arm::apphelp_cache {
// AhcProbeAndReadEnvironment／AslEnvVarQuery：先擷取完整 bytes，再補四個零 bytes。
// 不沿用宿主環境，也不讓非白名單變數覆寫系統環境。
struct Environment {
    static constexpr std::size_t maximum_size=64U*1024U*1024U;
    static constexpr std::array<std::u16string_view,3> names{u"__COMPAT_LAYER",u"__PROCESS_HISTORY",u"LOCALAPPDATA"};
    static constexpr std::array<std::size_t,3> capacities{255,1023,259}; // 包含終止字元。
    std::array<std::optional<std::u16string>,3> values;
    template<class Uppercase>
    static Environment capture(std::span<const std::uint8_t> bytes,Uppercase uppercase) {
        if(bytes.size()>maximum_size)throw Error{quota};
        // 奇數尾 byte 與追加的零合成最後一個 WCHAR；原生不要求來源對齊。
        const auto unit=[&](std::size_t index){
            const auto at=index*2;
            return at>=bytes.size()?char16_t{}:static_cast<char16_t>(bytes[at]|(at+1<bytes.size()?unsigned(bytes[at+1])<<8:0U));
        };
        Environment result;
        for(unsigned field=0;field<names.size();++field){
            const auto name=names[field];std::size_t at=0;
            while(unit(at)){
                const auto begin=at;std::size_t matched=0;
                while(matched<name.size() && unit(at) && uppercase(unit(at))==uppercase(name[matched])){++at;++matched;}
                const bool found=matched==name.size() && unit(at)==u'=';
                if(!found)while(unit(at) && !(unit(at)==u'=' && at!=begin))++at;
                if(unit(at)==u'='){
                    const auto value=at+1;std::size_t consumed=0;
                    // AslEnvVarQuery 對等號後的掃描上限是 0xfffe bytes（含等號）。
                    while(unit(at) && consumed<32767){++at;++consumed;}
                    if(found){
                        const auto length=at-value;
                        if(length<capacities[field]){
                            std::u16string text;text.reserve(length);
                            for(auto n=value;n<at;++n)text.push_back(unit(n));
                            result.values[field]=std::move(text);
                        }
                        break; // 第一筆超長值同樣阻止後續同名值生效。
                    }
                }
                ++at;
            }
        }
        return result;
    }
};
}
