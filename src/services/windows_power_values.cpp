#include "x64arm/windows_power_values.hpp"

namespace x64arm::power {
Value timeout_value(const NativePowerPreferences& preferences,bool dc,std::size_t setting){
    if(setting>=settings.size())return {2,0};
    const auto& source=preferences.sources[dc?1:0];
    if(!source.present)return {2,0};
    if(source.invalid[setting])return {13,0};
    const auto value=source.minutes[setting];
    if(!value)return {2,0};
    if(*value<0)return {13,0};
    if(*value>UINT32_MAX/60U)return {534,0};
    return {0,static_cast<std::uint32_t>(*value)*60U};
}
}
