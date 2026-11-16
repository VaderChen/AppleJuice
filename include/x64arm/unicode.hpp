#pragma once
#include "x64arm/memory.hpp"
#include <string>
#include <optional>

namespace x64arm {
char16_t unicode_case_unit(char16_t character,bool upper);
std::u16string utf8_to_utf16_lossy(std::string_view text);
std::optional<std::u16string> normalize_utf16(std::u16string_view text,bool composed);
std::u16string cp1252_to_utf16(std::string_view text);
std::string utf16_to_cp1252(std::u16string_view text,char replacement,bool& used_default);
std::u16string utf8_to_utf16(std::string_view text);
std::string utf16_to_utf8(std::u16string_view text);
// Windows 的 UTF-16→UTF-8（ACP／OEMCP＝65001）轉換：孤立代理字元換成 U+FFFD，不拋例外；完整的代理字元對保持為一個碼位。
std::string utf16_to_utf8_lossy(std::u16string_view text);
// 同上規則就地清理 UTF-16（孤立代理字元→U+FFFD），供需要先清理再計算長度或位移的呼叫端共用。
void replace_lone_surrogates(std::u16string& text);
std::u16string read_utf16(const GuestMemory& memory, GuestAddress address, std::size_t limit = 32767);
std::u16string read_utf16_counted(const GuestMemory& memory, GuestAddress address, std::size_t count);
std::vector<std::uint8_t> utf16_bytes(std::u16string_view text);
}
