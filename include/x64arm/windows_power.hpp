#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <memory>
#include <string_view>
#include <vector>
namespace x64arm {
class NativePowerAssertion {
public:
    virtual ~NativePowerAssertion() = default;
};
// 0: 顯示器、1: 系統、3: 執行；無法建立實際宿主請求時回空值。
std::unique_ptr<NativePowerAssertion> native_power_assertion(unsigned type,std::u16string_view reason);
// 一次性活動宣告會由系統閒置計時器到期；與持續 assertion 分開保存。
std::unique_ptr<NativePowerAssertion> native_power_activity(bool display);
std::optional<std::array<std::uint8_t,12>> native_power_status();
// Windows 的固定寬度電源資訊；不把 macOS 百分比當成電池容量或虛構 CPU 時脈。
std::optional<std::array<std::uint8_t,76>> native_power_capabilities();
std::optional<std::array<std::uint8_t,32>> native_power_battery_state();
std::optional<std::array<std::uint8_t,16>> native_system_power_information();
std::optional<std::vector<std::array<std::uint32_t,6>>> native_processor_power_information(unsigned count);
// ACPI 角色不可得時，依實際內建電池採用 Windows 的 mobile／desktop 備援規則。
std::optional<std::uint32_t> native_power_role();
struct NativePowerSourcePreferences {
    bool present{};
    // 順序為螢幕、系統、磁碟閒置逾時，macOS 的單位為分鐘。
    std::array<std::optional<std::int64_t>,3> minutes;
    std::array<bool,3> invalid{};
};
struct NativePowerPreferences {
    // AC 與 DC；DC 使用內建電池，沒有內建電池設定時使用 UPS 設定。
    std::array<NativePowerSourcePreferences,2> sources;
};
std::optional<NativePowerPreferences> native_power_preferences();
struct NativePowerEvent {
    std::uint32_t type{};
    std::array<std::uint8_t,16> setting{};
    std::vector<std::uint8_t> data;
};
// 宿主只產生資料；Windows ABI、客體記憶體及回呼由服務協調執行緒處理。
class NativePowerNotifications {
public:
    virtual ~NativePowerNotifications() = default;
    virtual std::vector<NativePowerEvent> poll() = 0;
    virtual std::optional<std::vector<std::uint8_t>> current_setting(const std::array<std::uint8_t,16>&) = 0;
};
std::shared_ptr<NativePowerNotifications> native_power_notifications();
struct NativePowerNotificationSnapshot {
    // Windows 的 PoAc／PoDc／PoHot；無可靠來源時不送出虛構值。
    std::optional<std::uint32_t> source,battery,saving,lid,display;
};
class NativePowerMonitor {
public:
    virtual ~NativePowerMonitor() = default;
    virtual NativePowerNotificationSnapshot sample() = 0;
};
std::shared_ptr<NativePowerMonitor> make_native_power_monitor();
}
