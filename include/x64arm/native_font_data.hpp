#pragma once
#include <cstdint>
#include <array>
#include <span>
#include <vector>
#include <string>
namespace x64arm {
// 系統彩色／CJK 字型可超過 64 MiB；所有檔案、回呼與原生載入共用容量契約。
inline constexpr std::size_t native_font_file_limit=256U*1024U*1024U;
inline constexpr std::size_t native_font_collection_limit=512U*1024U*1024U;
struct NativeFontFace;
// 使用非零繞行規則將帶孔多邊形分割成三角形；每三個點代表一個三角形。
std::vector<std::array<double,2>> native_polygon_triangles(const std::vector<std::vector<std::array<double,2>>>&);
struct NativeOutlineSegment {unsigned kind{};std::array<std::array<double,2>,3> points{};};
struct NativeOutlineContour {std::array<double,2> start{};std::vector<NativeOutlineSegment> segments;};
struct NativeGdiGlyph {
    std::array<std::int32_t,4> bounds{}; // 左、下、右、上；Y 向上。
    std::array<double,2> advance{};
    std::vector<NativeOutlineContour> contours;
    std::vector<std::uint8_t> alpha; // top-down、單通道、沒有 stride padding。
};
NativeGdiGlyph native_gdi_glyph(const NativeFontFace&,std::uint16_t,const std::array<double,4>&,unsigned raster);

std::vector<NativeFontFace> native_load_font_faces(std::span<const std::uint8_t>);
// DWRITE_GLYPH_METRICS 欄位順序；全部以設計單位表示。
struct NativeGlyphRun {
    float em{},pixels_per_dip{1},baseline_x{},baseline_y{};
    std::array<float,6> transform{1,0,0,1,0,0};
    std::vector<std::uint16_t> indices;
    std::vector<float> advances;
    std::vector<std::array<float,2>> offsets;
    bool sideways{},rtl{},aliased{};
    unsigned measuring{};
};
struct NativeGlyphRaster {
    std::array<std::int32_t,4> bounds{};
    unsigned channels{1};
    std::vector<std::uint8_t> alpha;
};
NativeGlyphRaster native_font_glyph_raster(const NativeFontFace&,const NativeGlyphRun&);
using NativeGlyphMetrics=std::array<std::int32_t,7>;
std::vector<NativeGlyphMetrics> native_font_glyph_metrics(const NativeFontFace&,std::span<const std::uint16_t>);
// 將目前 face 的 CoreText tables 匯出為單一 OpenType sfnt，保留真實字型資料。
std::vector<std::uint8_t> native_font_file(const NativeFontFace&);
// OpenType tag 採檔案中的 big-endian 四字元值。
std::vector<std::uint8_t> native_font_table(const NativeFontFace&,std::uint32_t tag);
struct NativeFontName {std::u16string locale,value;};
std::vector<NativeFontName> native_font_names(const NativeFontFace&,std::uint16_t name_id);
std::vector<std::uint16_t> native_font_glyph_indices(const NativeFontFace&,std::span<const std::uint32_t> codepoints);
}
