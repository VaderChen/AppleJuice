#pragma once
// 色彩管理（ICM）共用儲存與 ICC 工具：gdi32 的 ICM 函式與 mscms.dll 共用。
// 設定檔檔案放在沙箱色彩目錄；已安裝清單、裝置關聯（有序，第一個為預設）、標準色彩空間、CMM 與設定經一般登錄服務存於
// HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ICM。標準 sRGB 設定檔不使用微軟檔案，依 IEC 61966-2.1 參數自行產生。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/color_transform.hpp"
#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace x64arm {
inline constexpr std::u16string_view icm_color_directory=u"C:\\Windows\\System32\\spool\\drivers\\color";
inline constexpr std::u16string_view icm_display_device=u"\\\\.\\DISPLAY1";
inline constexpr std::u16string_view icm_srgb_file=u"sRGB Color Space Profile.icm";
inline constexpr std::uint32_t icm_error_invalid_profile=2011,icm_error_not_associated=2015,icm_error_profile_not_found=2016,icm_error_invalid_cmm=2010;
inline constexpr std::uint32_t lcs_srgb=0x73524742,lcs_windows_color_space=0x57696e20,lcs_calibrated_rgb=0;
std::u16string icm_lower(std::u16string text);
std::u16string icm_file_part(std::u16string_view path);
// 只有檔名時位於色彩目錄。
std::u16string icm_profile_path(std::u16string_view name);

// ICC 設定檔的標頭與 tag 模型：共用同一份資料的 tag（reference）指向同一個資料區塊。
struct IccProfile {
    std::array<std::uint8_t,128> header{};
    std::vector<std::pair<std::uint32_t,std::size_t>> tags; // 簽章、資料索引（依 tag table 順序）
    std::vector<std::vector<std::uint8_t>> data;
    const std::vector<std::uint8_t>* find(std::uint32_t tag) const;
    std::size_t references(std::size_t index) const;
};
std::uint32_t icc_u32(std::span<const std::uint8_t> bytes,std::size_t at);
void icc_put32(std::span<std::uint8_t> bytes,std::size_t at,std::uint32_t value);
// 標頭大小（big-endian）、'acsp' 與 tag table 都在範圍內才可解析；格式錯誤回 nullopt。
std::optional<IccProfile> icc_parse(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> icc_build(const IccProfile& profile);
IccProfile icc_empty();
// 矩陣／TRC RGB 顯示設定檔（ICC v2.1）：PCS 為 D50 XYZ；gamma 為 0 時使用 sRGB 曲線。
struct IccRgbParameters {std::array<std::array<double,3>,3> colorants{};std::array<double,3> white{0.9642,1.0,0.8249};std::array<double,3> gamma{};std::uint32_t intent{};std::string description;};
std::vector<std::uint8_t> icc_rgb_profile(const IccRgbParameters& parameters);
IccRgbParameters icc_srgb_parameters();

struct WindowsServices::IcmStore {
    WindowsServices& self;GuestState& state;GuestMemory& memory;GuestScratch scratch;
    IcmStore(WindowsServices& services,GuestState& guest_state,GuestMemory& guest_memory):self(services),state(guest_state),memory(guest_memory),scratch(services,guest_memory,guest_state){}
    std::uint64_t text(std::u16string_view value);
    // HKLM\...\ICM 之下的登錄值；子鍵不存在或值不存在時回 nullopt。
    std::optional<std::u16string> read_string(std::u16string_view child,std::u16string_view name);
    std::optional<std::vector<std::u16string>> read_list(std::u16string_view child,std::u16string_view name);
    // HKLM 之下任意路徑的 REG_MULTI_SZ（列印排存的 CopyFiles 清單等）。
    std::optional<std::vector<std::u16string>> read_machine_list(std::u16string_view path,std::u16string_view name);
    std::optional<std::uint32_t> read_number(std::u16string_view child,std::u16string_view name);
    bool write_string(std::u16string_view child,std::u16string_view name,std::u16string_view value);
    bool write_list(std::u16string_view child,std::u16string_view name,const std::vector<std::u16string>& values);
    bool remove(std::u16string_view child,std::u16string_view name);
    std::vector<std::u16string> names(std::u16string_view child);
    bool exists(std::u16string_view path);
    std::optional<std::vector<std::uint8_t>> read_profile(std::u16string_view path); // 讀檔並核對 ICC 標頭
    // 安裝：驗證後複製到色彩目錄（已在目錄內則不複製）並登錄；解除安裝同時移除所有裝置關聯。
    std::vector<std::u16string> registered();
    bool installed(std::u16string_view file);
    bool install(std::u16string_view source);
    bool uninstall(std::u16string_view name,bool remove_file);
    std::vector<std::u16string> associations(std::u16string_view device);
    bool associate(std::u16string_view file,std::u16string_view device,bool first);
    bool disassociate(std::u16string_view file,std::u16string_view device);
    // 標準色彩空間（LCS_sRGB／LCS_WINDOWS_COLOR_SPACE 預設為自行產生的 sRGB 設定檔）與裝置預設設定檔（第一個關聯，沒有時為 sRGB）。
    std::optional<std::u16string> standard_profile(std::uint32_t space);
    bool set_standard_profile(std::uint32_t space,std::u16string_view path);
    std::optional<std::u16string> device_default(std::u16string_view device);
    std::optional<std::u16string> cmm(std::uint32_t id);
    bool register_cmm(std::uint32_t id,std::u16string_view module);
    bool unregister_cmm(std::uint32_t id);
private:
    std::uint64_t key(std::u16string_view child);
    std::uint32_t advapi(std::string_view symbol,std::initializer_list<std::uint64_t> arguments);
    std::uint64_t root() const;
};
// mscms 的已開啟設定檔（HPROFILE）；可寫入的磁碟設定檔在 CloseColorProfile 時寫回。
struct WindowsServices::IcmProfiles {
    struct Profile {IccProfile model;std::u16string path;bool writable{},dirty{};};
    // 色彩轉換（HTRANSFORM）：輸入與輸出的通道數與色彩空間簽章取自設定檔序列兩端。
    struct Transform {std::shared_ptr<NativeColorTransform> native;unsigned input{},output{};std::uint32_t input_space{},output_space{};};
    std::map<std::uint64_t,Profile> profiles;std::map<std::uint64_t,Transform> transforms;std::uint64_t next{0x1000};
};
// 色彩空間簽章的通道數（'GRAY' 1、'RGB '／'XYZ '／'Lab ' 等 3、'CMYK' 4、'2CLR'～'FCLR' 2～15）；不認得時回 0。
unsigned icc_space_channels(std::uint32_t space);
}
