#pragma once
#include "x64arm/windows_abi.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <memory>

namespace x64arm {
struct NativeFontFace {
    std::u16string family,full_name,style,postscript_name;
    std::int32_t em{},ascent{},descent{},line_gap{},average_width{},maximum_width{},weight{400};
    std::uint16_t first_character{},last_character{},default_character{},break_character{32};
    std::uint32_t flags{},type{4};std::uint8_t family_class{};bool italic{},fixed_pitch{};
    std::array<std::uint32_t,6> signature{};
    std::array<std::int32_t,10> design_metrics{};
    unsigned simulations{}; // DirectWrite 粗體／斜體模擬，共用輪廓與光柵。
    // 保留確切原生字型及 variation；系統 UI 字型不能用名稱重新開啟。
    std::shared_ptr<const void> native;
    bool operator==(const NativeFontFace&)const=default;
};
struct GuiFont;
// 系統介面的邏輯別名在選取實體 face 前套用；自訂字型的 glyph 身分不受影響。
bool windows_ui_font_alias(std::u16string_view);
std::u16string native_ui_font_family();
struct NativeTextShape;
struct NativeScriptShape {
    std::vector<std::uint16_t> glyphs,clusters,attributes;
    bool missing{};
};
NativeScriptShape native_script_shape(const GuiFont&,std::u16string_view,bool rtl,bool logical,bool unshaped,bool no_glyphs);
struct NativeGdiPlacement {
    std::vector<std::uint16_t> glyphs;
    std::vector<std::uint32_t> clusters;
    std::vector<double> advances;
};
// 已完成雙向分段的單一 run；回傳目前選取 face 的 glyph，不能混入替代字型的索引。
NativeGdiPlacement native_gdi_placement(const GuiFont&,std::u16string_view,bool rtl,std::uint32_t flags);
struct GuiDraw;
struct NativeGlyphRaster;
std::vector<std::int32_t> native_gdi_glyph_advances(const GuiFont&,std::u16string_view);
NativeGlyphRaster native_gdi_glyph_raster(const GuiDraw&);
NativeTextShape native_shape_text(const GuiFont&,std::u16string_view,bool rtl,bool fallback);
std::optional<NativeFontFace> native_resolve_font(const GuiFont&);
std::vector<NativeFontFace> native_font_faces(std::u16string_view family);
struct FontEnumerationRecord {
    std::vector<std::uint8_t> logical,metrics;std::uint32_t type{};
    bool operator==(const FontEnumerationRecord&)const=default;
};
std::vector<FontEnumerationRecord> font_enumeration_records(std::vector<NativeFontFace>,bool wide,std::uint8_t charset,bool all_styles);
struct FontEnumeration {
    std::vector<FontEnumerationRecord> records;std::size_t index{};
    std::uint64_t callback{},parameter{},logical{},metrics{},thread{};
    std::vector<std::uint64_t> scratch;
    bool operator==(const FontEnumeration&)const=default;
};
std::span<const WindowsServiceDescriptor> font_service_descriptors();
}
