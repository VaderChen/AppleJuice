#include "x64arm/windows_power.hpp"
namespace x64arm {
std::unique_ptr<NativePowerAssertion> native_power_activity(bool){return {};}
std::unique_ptr<NativePowerAssertion> native_power_assertion(unsigned,std::u16string_view){return {};}
std::optional<std::array<std::uint8_t,12>> native_power_status(){return {};}
std::optional<std::array<std::uint8_t,76>> native_power_capabilities(){return {};}
std::optional<std::array<std::uint8_t,32>> native_power_battery_state(){return {};}
std::optional<std::array<std::uint8_t,16>> native_system_power_information(){return {};}
std::optional<std::vector<std::array<std::uint32_t,6>>> native_processor_power_information(unsigned){return {};}
std::optional<std::uint32_t> native_power_role(){return {};}
std::optional<NativePowerPreferences> native_power_preferences(){return {};}
std::shared_ptr<NativePowerNotifications> native_power_notifications(){return {};}
std::shared_ptr<NativePowerMonitor> make_native_power_monitor(){return {};}
}
