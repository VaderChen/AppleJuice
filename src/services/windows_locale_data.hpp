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
std::optional<std::array<std::uint32_t,3>> windows_locale_digit_substitution(std::uint32_t locale);
// CRT locale 物件與 Win32 NLS 共用名稱、LCID、碼頁及 lead／trail 分類。
std::optional<CrtLocaleCategory> windows_crt_locale(std::u16string_view name);
std::optional<CrtLocaleCategory> windows_crt_codepage(std::uint32_t codepage);
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
}
