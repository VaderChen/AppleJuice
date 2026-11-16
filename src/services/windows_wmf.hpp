#pragma once
// Windows 中繼檔（WMF，MS-WMF）共用格式工具與中繼檔播放狀態：
// 錄製端沿用 EMF 錄製 DC 的 GuiDraw 串流再編成 WMF 記錄；播放端依 PlayMetaFileRecord 契約呼叫既有 GDI 服務。
#include "x64arm/windows_metafile.hpp"
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace x64arm {
inline constexpr std::size_t windows_metafile_limit=64U*1024U*1024U;
// 所有欄位為 little-endian；記錄大小以 WORD 計，參數自記錄第 6 位元組起。
inline std::uint16_t wmf_word(std::span<const std::uint8_t> bytes,std::size_t at){
    if(at>bytes.size() || 2>bytes.size()-at)throw std::invalid_argument("WMF 欄位越界");
    return static_cast<std::uint16_t>(bytes[at]|static_cast<unsigned>(bytes[at+1])<<8);
}
inline std::uint32_t wmf_dword(std::span<const std::uint8_t> bytes,std::size_t at){return wmf_word(bytes,at)|static_cast<std::uint32_t>(wmf_word(bytes,at+2))<<16;}
// METAHEADER：Type 1（記憶體）或 2（磁碟）、HeaderSize 9、Version 0x100／0x300、Size（WORD 數）不超過資料長度。
bool valid_windows_metafile(std::span<const std::uint8_t> bytes);
std::size_t windows_metafile_size(std::span<const std::uint8_t> bytes);
// 依序列出標頭之後的記錄（位移、位元組數），以 EOF（RecordFunction 0）結尾；格式錯誤擲 invalid_argument。
std::vector<std::pair<std::size_t,std::size_t>> wmf_records(std::span<const std::uint8_t> bytes);
// 錄製：wmf_begin 建立標頭佔位，wmf_draw 以 output（裝置→輸出邏輯單位）把一筆 GuiDraw 編成 WMF 記錄，wmf_finish 補 EOF 與標頭。
// WMF 沒有世界座標矩陣，旋轉／斜切、字形索引、垂直字距與圖樣筆刷擲 domain_error；座標超出 16 位元擲 range_error。
void wmf_begin(GuiMetafile& meta,bool disk);
void wmf_draw(GuiMetafile& meta,const GuiDraw& draw,const GuiTransform& output=gui_identity);
void wmf_record(GuiMetafile& meta,std::uint16_t function,std::span<const std::uint16_t> parameters);
void wmf_finish(GuiMetafile& meta);
// META_ESCAPE_ENHANCED_METAFILE：EMF 依 8192 位元組分段內嵌於 MFCOMMENT；取回時核對段數、總長與 checksum。
void wmf_embed_emf(GuiMetafile& meta,std::span<const std::uint8_t> emf);
std::optional<std::vector<std::uint8_t>> wmf_embedded_emf(std::span<const std::uint8_t> wmf);

// EnumEnhMetaFile 與 EnumMetaFile 的進行中列舉；以客體配置（handle table 起點）為鍵，供回呼中的 Play*Record 與收尾使用。
struct MetafileState {
    struct Enumeration {EmfDecoder decoder;std::uint64_t dc{};unsigned count{};GuiTransform mapping{gui_identity};GuiDc original;std::vector<GuiDc> saved;std::map<unsigned,std::uint64_t> objects;};
    struct WindowsEnumeration {std::uint64_t dc{};unsigned handles{};std::array<std::uint64_t,4> selected{};};
    std::map<std::uint64_t,Enumeration> enumerations;
    std::map<std::uint64_t,WindowsEnumeration> windows_enumerations;
};
}
