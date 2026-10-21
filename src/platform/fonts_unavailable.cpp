#include "x64arm/windows_fonts.hpp"
#include "x64arm/native_font_data.hpp"
#include "x64arm/native_text_layout.hpp"
#include <stdexcept>
#include "x64arm/windows_gui.hpp"
namespace x64arm {
std::vector<std::array<double,2>> native_polygon_triangles(const std::vector<std::vector<std::array<double,2>>>&){throw std::runtime_error("此平台沒有多邊形細分後端");}
std::u16string native_ui_font_family(){return u"Segoe UI";}
NativeTextLayout native_text_layout(std::u16string_view,std::span<const NativeLayoutSpan>,const NativeLayoutOptions&){throw std::runtime_error("此平台尚未提供文字排版後端");}

NativeGdiGlyph native_gdi_glyph(const NativeFontFace&,std::uint16_t,const std::array<double,4>&,unsigned){throw std::domain_error("本平台沒有 GDI 字形後端");}
std::vector<NativeFontFace> native_load_font_faces(std::span<const std::uint8_t>){return {};}
NativeScriptShape native_script_shape(const GuiFont&,std::u16string_view,bool,bool,bool,bool){throw std::domain_error("本平台沒有原生文字成形後端");}
NativeGdiPlacement native_gdi_placement(const GuiFont&,std::u16string_view,bool,std::uint32_t){throw std::domain_error("本平台沒有原生 GDI 字形定位後端");}
std::vector<std::int32_t> native_gdi_glyph_advances(const GuiFont&,std::u16string_view){throw std::domain_error("本平台沒有原生 GDI 字形量測後端");}
NativeGlyphRaster native_gdi_glyph_raster(const GuiDraw&){throw std::domain_error("本平台沒有原生 GDI 字形繪圖後端");}
NativeTextShape native_shape_text(const GuiFont&,std::u16string_view,bool,bool){throw std::runtime_error("此平台尚未提供文字分析後端");}
NativeGlyphRaster native_font_glyph_raster(const NativeFontFace&,const NativeGlyphRun&){throw std::runtime_error("此平台尚未提供原生字形光柵化");}
std::vector<NativeGlyphMetrics> native_font_glyph_metrics(const NativeFontFace&,std::span<const std::uint16_t>){throw std::runtime_error("此平台尚未提供原生字形量測");}
std::vector<std::uint8_t> native_font_file(const NativeFontFace&){throw std::runtime_error("此平台尚未提供原生字型檔案");}
std::vector<std::uint8_t> native_font_table(const NativeFontFace&,std::uint32_t){throw std::runtime_error("此平台尚未提供原生字型資料");}
std::vector<NativeFontName> native_font_names(const NativeFontFace&,std::uint16_t){throw std::runtime_error("此平台尚未提供原生字型資料");}
std::vector<std::uint16_t> native_font_glyph_indices(const NativeFontFace&,std::span<const std::uint32_t>){throw std::runtime_error("此平台尚未提供原生字形查詢");}
std::optional<NativeFontFace> native_resolve_font(const GuiFont&){return {};}
std::vector<NativeFontFace> native_font_faces(std::u16string_view){throw std::runtime_error("此平台尚未提供原生字型列舉");}
}
