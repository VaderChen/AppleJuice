#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace x64arm {
// 程序的 ANSI 碼頁（GetACP／CP_ACP）。預設 65001；程序啟動時依主程式的清單 activeCodePage 與版本資源語言決定：
// 繁體中文→950、簡體中文→936、日文→932、韓文→949，其餘語言維持 65001。真實 Windows 的 ACP 來自系統地區設定，
// 為這些語言製作的舊程式只在對應地區設定下正確，因此以程式本身標示的語言代替系統地區。
std::uint32_t process_ansi_codepage() noexcept;
void set_process_ansi_codepage(std::uint32_t page) noexcept;
inline bool process_ansi_is_utf8() noexcept {return process_ansi_codepage()==65001;}
// CP_ACP(0)／CP_OEMCP(1)／CP_THREAD_ACP(3) 解析為程序碼頁（OEM 碼頁與 ANSI 相同）；其他值原樣回傳。
inline std::uint32_t resolve_ansi_alias(std::uint32_t page) noexcept {return page==0 || page==1 || page==3?process_ansi_codepage():page;}
// 版本資源語言（LANGID）對應的 ANSI 碼頁；0 表示沿用預設。
std::uint32_t ansi_codepage_for_language(std::uint16_t language) noexcept;
// 依 VS_VERSIONINFO 的 VarFileInfo\Translation（沒有時用資源目錄的語言）取得產品語言；找不到回傳 0。
std::uint16_t version_resource_language(std::span<const std::uint8_t> version,std::uint16_t directory_language) noexcept;

// 程序 ANSI 位元組與 UTF-16／UTF-8 互轉。ACP 為 65001 時與 utf8_to_utf16／utf16_to_utf8 完全相同；
// 其他碼頁無法對應的位元組換成 U+FFFD／'?'（與 MultiByteToWideChar／WideCharToMultiByte 不帶嚴格旗標的結果相同）。
std::u16string ansi_to_utf16(std::string_view bytes);
// 同上，但 ACP 為 65001 時沿用 utf8_to_utf16_lossy（無效序列換成 U+FFFD，不拋例外）。
std::u16string ansi_to_utf16_lossy(std::string_view bytes);
std::string ansi_to_utf8(std::string_view bytes);
std::string utf16_to_ansi(std::u16string_view text);
std::string utf8_to_ansi(std::string_view text);
// 不超過 maximum 位元組、且不切斷多位元組字元的前綴長度（UTF-8 跳過延續位元組，DBCS 依前導位元組逐字計算）。
std::size_t ansi_prefix_length(std::string_view text,std::size_t maximum) noexcept;
// 程序碼頁的 DBCS 前導位元組（UTF-8 一律 false）；CharNextA、IsDBCSLeadByte 與 CRT 多位元組表共用。
bool ansi_lead_byte(std::uint8_t byte) noexcept;
}
