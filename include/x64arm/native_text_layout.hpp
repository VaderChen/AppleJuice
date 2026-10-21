#pragma once
#include "x64arm/windows_fonts.hpp"
#include "x64arm/native_font_data.hpp"
namespace x64arm {
// CoreText 排版結果以 DIP、UTF-16 位置與真正的字形資料跨越平台邊界。
struct NativeLayoutSpan {std::uint32_t start{},length{};NativeFontFace face;float size{};std::u16string locale{};};
struct NativeLayoutOptions {
    float width{},height{},pixels_per_dip{1},tab{48},line_spacing{},baseline{};
    unsigned alignment{},paragraph{},wrapping{},reading{},spacing{},measuring{};
};
struct NativeLayoutLine {
    std::uint32_t start{},length{},trailing{},newline{};float x{},top{},width{},width_trailing{},height{},baseline{};
};
struct NativeLayoutCluster {
    std::uint32_t start{},length{},line{};float x{},width{};bool whitespace{},newline{},rtl{},wrap{};
};
struct NativeLayoutRun {
    NativeFontFace face;NativeGlyphRun glyphs;std::uint32_t start{},length{},line{};
    std::vector<std::uint16_t> clusters;float width{};
};
struct NativeTextLayout {
    std::vector<NativeLayoutLine> lines;std::vector<NativeLayoutCluster> clusters;std::vector<NativeLayoutRun> runs;
    float left{},top{},width{},width_trailing{},height{},minimum{};
    std::array<float,4> overhang{};unsigned bidi_depth{1};
};
NativeTextLayout native_text_layout(std::u16string_view,std::span<const NativeLayoutSpan>,const NativeLayoutOptions&);
}
