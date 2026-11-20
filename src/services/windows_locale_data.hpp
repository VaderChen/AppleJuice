#pragma once
#include <string_view>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include <ctime>
#include "x64arm/windows_runtime.hpp"
namespace x64arm {
// 與 IsValidLocaleName 使用同一份 Windows 名稱／宿主地區對照。
bool windows_locale_name_valid(std::u16string_view name);
// 與語言群組列舉共用已安裝的 NLS 目錄；bit 0 對應 LGRPID 1。
std::uint32_t windows_nls_language_group_mask();
// Shell 與 NLS 使用相同的語系排序；回傳負值、零或正值。
bool windows_nls_contains(std::u16string text,std::u16string pattern,std::uint32_t id,std::uint32_t flags,bool prefix);
bool windows_regex_search(std::u16string_view text,std::u16string_view pattern);
std::optional<int> windows_nls_compare(std::u16string_view left,std::u16string_view right,
                                     std::uint32_t locale,std::uint32_t flags);
std::optional<std::array<std::uint32_t,3>> windows_locale_digit_substitution(std::uint32_t locale);
// CRT locale 物件與 Win32 NLS 共用名稱、LCID、碼頁及 lead／trail 分類。
std::optional<CrtLocaleCategory> windows_crt_locale(std::u16string_view name);
std::optional<CrtLocaleCategory> windows_nls_codepage(std::uint32_t codepage);
std::optional<CrtLocaleCategory> windows_crt_codepage(std::uint32_t codepage);
// 依 LCID 建立 CRT locale 類別（VC8～VC10 的 STL 以 LCID 識別 locale）；0 即 C locale。
std::optional<CrtLocaleCategory> windows_crt_locale_id(std::uint32_t lcid);
// GetStringTypeW 的單一字元分類（CT_CTYPE1／2／3），STL 的 _Getwctype 共用。
std::uint16_t windows_nls_character_type(char16_t value,unsigned type);
struct CrtNumericData {std::u16string decimal{u"."},thousands;std::string grouping;};
CrtNumericData windows_crt_numeric(const CrtLocaleCategory&);
struct CrtTimeData {
    // __crt_lc_time_data：43 個 narrow 與 wide 欄位，以及末尾的 wide locale 名稱。
    std::array<std::u16string,44> fields;
    std::uint32_t calendar{1};
    bool classic{};
};
CrtTimeData windows_crt_time_data(const CrtLocaleCategory&);
std::optional<std::u16string> windows_crt_time_calendar(const CrtTimeData&,const std::tm&,unsigned field);
struct CrtCharacterTable {std::array<std::uint16_t,257> masks{};std::array<std::uint8_t,256> lower{},upper{};};
const CrtCharacterTable& windows_crt_character_table(const CrtLocaleCategory&);
char16_t windows_crt_map_character(char16_t,const CrtLocaleCategory&,bool upper);
struct CrtDecodedCharacter {std::uint32_t character{};std::size_t bytes{};enum class Status{Complete,Partial,Invalid} status{Status::Invalid};};
CrtDecodedCharacter windows_crt_decode(std::span<const std::uint8_t>,std::uint32_t page);
std::optional<std::vector<std::uint8_t>> windows_crt_encode(std::u16string_view,std::uint32_t page);
// Win32 轉換沿用 CRT 的碼頁選擇；另保留替代字元及嚴格錯誤契約。
std::optional<std::u16string> windows_nls_decode(std::string_view,std::uint32_t page,bool strict);
struct NlsEncodedString {std::vector<std::uint8_t> bytes;bool used_default{};};
std::optional<NlsEncodedString> windows_nls_encode(std::u16string_view,std::uint32_t page,
                                                bool no_best_fit,std::string_view replacement,bool strict=false);
}
