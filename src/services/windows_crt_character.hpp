#pragma once
namespace x64arm {
struct WindowsServiceDescriptor;
struct CrtCharacterOperation;
const CrtCharacterOperation* crt_character_operation(const WindowsServiceDescriptor&);
bool crt_character_requested() noexcept;
// 寬字元的 CRT 分類位元（_UPPER 1、_LOWER 2、_DIGIT 4、_SPACE 8、_PUNCT 0x10、_CONTROL 0x20、_BLANK 0x40、_HEX 0x80、_ALPHA 0x100），以 Unicode 屬性判斷。
unsigned windows_crt_unicode_mask(char16_t);
}
