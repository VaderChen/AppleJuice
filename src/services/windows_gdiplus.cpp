#include "x64arm/windows_services.hpp"
#include "texture_load.hpp"
#include "gdiplus_codecs.hpp"
#include "gdiplus_metafile.hpp"
#include "gdiplus_brush.hpp"
#include "gdiplus_pixels.hpp"
#include "gdiplus_image_attributes.hpp"
#include "gdiplus_effects.hpp"
#include "unicode_text_analysis.hpp"
#include "windows_locale_data.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/native_font_data.hpp"
#include "x64arm/windows_icon.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/windows_gdi_path.hpp"
#include "x64arm/windows_transform.hpp"
#include <algorithm>
#include <memory>
#include <span>
#include <zlib.h>
#include <bit>
#include <cstdlib>
#include <map>
#include <optional>
#include <tuple>

namespace x64arm {
// GDI+ 平面 API：啟動／關閉、記憶體配置、記憶體點陣圖與繪製到 HDC。
// 點陣圖像素保留在客體記憶體（Scan0 由呼叫端提供或由此配置），繪製經共用 raster surface 合成。
// 參考：https://learn.microsoft.com/windows/win32/gdiplus/-gdiplus-flatapi-flat
struct GdiplusState {
    struct Path {std::vector<GuiPathPoint> points;unsigned fill{};bool new_figure{true};};
    // 自訂線端（座標以筆寬為單位，+Y 指向線外）：type 0 為 CustomLineCap、1 為 AdjustableArrowCap。
    struct CustomCap {unsigned type{};Path fill{},stroke{};unsigned base_cap{},start_cap{},end_cap{},join{};double inset{},scale{1};
        double arrow_width{},arrow_height{},middle{};bool filled{true};};
    // 畫筆：fill 為非純色筆刷的複本（nullptr 表示以 color 純色描邊）；dashes 為 DashStyleCustom 的圖樣（筆寬倍數），
    // compound 為複合線的分段位置（0～1），transform 套用到筆尖，alignment 0 置中、1 內縮；線端 0xff 時使用 custom_start／custom_end。
    struct Pen {std::uint32_t color{};double width{1};unsigned unit{},start_cap{},end_cap{},join{},dash{},dash_cap{};double miter{10};std::shared_ptr<const GdiplusBrush> fill;
        std::vector<double> dashes{};double dash_offset{};std::vector<double> compound{};GuiTransform transform{gui_identity};unsigned alignment{};
        std::shared_ptr<const CustomCap> custom_start{},custom_end{};};
    std::map<std::uint64_t,CustomCap> caps;
    // 路徑迭代器：建立時的路徑內容與 subpath／marker／path type 游標（type 範圍限於最近一次 NextSubpath 的 figure）。
    struct PathIterator {std::vector<GuiPathPoint> points{};std::size_t subpath{},marker{},type_begin{},type_end{};};
    std::map<std::uint64_t,PathIterator> iterators;
    // GpMatrix：(m11, m12, m21, m22, dx, dy)，與 GDI 世界變換同一種列向量配置。
    std::map<std::uint64_t,GuiTransform> matrices;
    // Graphics 的世界變換、頁面單位、提示與裁切（裁切以裝置座標區域保存，nullopt 為無限大）。
    struct Graphics {GuiTransform world{1,0,0,1,0,0};unsigned page_unit{1};double page_scale{1};
        unsigned compositing{},quality{},interpolation{},pixel_offset{},text_hint{},text_contrast{4};std::int32_t origin_x{},origin_y{};
        std::optional<GuiRegion> clip;std::uint64_t window{};bool owned_dc{};};
    std::map<std::uint64_t,Graphics> graphics_state;
    std::map<std::uint64_t,std::vector<std::pair<std::uint32_t,Graphics>>> saved_graphics;std::uint32_t next_save{1};
    std::map<std::uint64_t,Path> paths;
    // GpRegion：與 GdipGetRegionData 序列化相同的節點樹；葉節點為矩形 0x10000000、路徑 0x10000001、空 0x10000002、無限大 0x10000003，
    // 內部節點的型別即 CombineMode 1～5。節點不可變，合併與複製共用子樹。
    struct RegionNode {std::uint32_t type{0x10000003U};std::array<double,4> rect{};Path path;std::shared_ptr<const RegionNode> left,right;};
    std::map<std::uint64_t,std::shared_ptr<const RegionNode>> regions;
    std::map<std::uint64_t,Pen> pens;
    std::map<std::uint64_t,GdiplusBrush> brushes;
    std::map<std::uint64_t,unsigned> smoothing;
    // LockBits 期間的暫存區：格式相同時直接指向像素，否則轉換到暫存緩衝並於 UnlockBits 寫回。
    struct Lock {std::uint64_t buffer{};std::uint32_t mode{},format{};std::array<std::int32_t,4> rect{};std::int64_t stride{};bool owned{};};
    // 點陣圖：索引色格式附調色盤；raw 為 ImageFormat GUID 的末位元組（MemoryBMP 0xaa、BMP 0xab、JPEG 0xae、PNG 0xaf、GIF 0xb0、TIFF 0xb1、Icon 0xb5）。
    struct Property {std::uint32_t id{};std::uint16_t type{};std::vector<std::uint8_t> value{};};
    struct Bitmap {std::uint32_t width{},height{},format{};std::int64_t stride{};std::uint64_t scan0{};bool owned{};std::optional<Lock> lock{};
        std::vector<std::uint32_t> palette{};std::uint32_t palette_flags{};double dpi_x{96},dpi_y{96};std::uint8_t raw{0xaa};std::uint32_t image_flags{};std::vector<Property> properties{};};
    std::map<std::uint64_t,Bitmap> bitmaps;
    std::map<std::uint64_t,gdiplus_meta::Image> metafiles;
    // ImageAttributes 與 CachedBitmap（建立時複製成 32bppPARGB 的自有點陣圖）。
    std::map<std::uint64_t,GdiplusImageAttributes> attributes;std::map<std::uint64_t,Bitmap> cached;
    // GDI+ 1.1 影像效果：種類、已檢查的參數（RedEye 另存區域複本）與是否已設定。
    struct Effect {gdiplus_effect::Kind kind{};std::vector<std::uint8_t> parameters;std::vector<std::array<std::int32_t,4>> areas;bool configured{};};
    std::map<std::uint64_t,Effect> effects;
    // 文字：字型家族（名稱與實體 face）、字型（家族＋em 大小＋樣式＋單位）、字型集合（已安裝或私有）、StringFormat。
    struct FontFamily {std::u16string name{};std::vector<NativeFontFace> faces{};};
    struct Font {FontFamily family{};double size{};unsigned style{},unit{};};
    struct FontCollection {bool installed{};std::vector<FontFamily> families{};};
    struct StringFormat {std::uint32_t flags{};std::uint16_t language{},digit_language{};unsigned align{},line_align{},trimming{1},hotkey{},digit_method{};bool typographic{};
        double tab_first{};std::vector<double> tabs{};std::vector<std::pair<std::int32_t,std::int32_t>> ranges{};};
    std::map<std::uint64_t,FontFamily> families;std::map<std::uint64_t,Font> fonts;std::map<std::uint64_t,FontCollection> collections;std::map<std::uint64_t,StringFormat> formats;
    std::uint64_t installed_collection{},generic_default{},generic_typographic{};
    // 字形輪廓快取（設計單位、Y 向上）：鍵為 PostScript 名稱＋模擬旗標＋字形。
    std::map<std::tuple<std::u16string,unsigned,std::uint16_t>,std::vector<GuiPathPoint>> outlines;
    std::map<std::uint64_t,std::uint64_t> graphics; // GpGraphics → HDC。
    // GdipGetImageGraphicsContext：以 32bpp 預乘 DIB section 承接繪圖。dirty 表示 DIB 可能比點陣圖新（繪製過），
    // held 表示 GdipGetDC 借出的 HDC 尚未歸還；讀取點陣圖前寫回、寫入點陣圖後同步到 DIB，刪除 Graphics 時最後寫回一次。
    struct ImageTarget {std::uint64_t bitmap{},dc{},dib{},previous{},bits{};bool dirty{},held{};};
    std::map<std::uint64_t,ImageTarget> image_graphics;
    std::uint64_t next_token{1};
    unsigned startups{};
    // GdipLoadImageFromStream：以 IStream::Read 分段讀入，完成後由共用影像解碼器轉成 32bppARGB。
    struct StreamLoad {std::uint64_t stream{},output{},buffer{};std::vector<std::uint8_t> bytes;bool saving{};std::size_t offset{};std::uint32_t sent{};std::string_view operation{};bool seeking{};};
    std::map<std::uint64_t,StreamLoad> loads;std::uint64_t next_load{1};
};
namespace {
// GpStatus。
constexpr std::uint32_t ok=0,generic_error=1,invalid_parameter=2,out_of_memory=3,object_busy=4,insufficient_buffer=5,not_implemented=6,wrong_state=8,unknown_image_format=13,unsupported_version=17,not_initialized=18;
constexpr std::uint32_t stream_chunk=65536;constexpr std::size_t stream_limit=64U*1024U*1024U;
// 像素格式與預乘轉換共用 gdiplus_pixels.hpp。
constexpr std::uint32_t argb32=gdiplus_pixel::argb32,pargb32=gdiplus_pixel::pargb32,rgb32=gdiplus_pixel::rgb32,rgb24=gdiplus_pixel::rgb24;
using gdiplus_pixel::premultiply;using gdiplus_pixel::unpremultiply;
constexpr std::uint32_t property_not_found=19;
// 依檔頭判斷 ImageFormat（GUID 末位元組）。
std::uint8_t raw_format(std::span<const std::uint8_t> bytes){
    const auto starts=[&](std::initializer_list<std::uint8_t> magic){return bytes.size()>=magic.size() && std::equal(magic.begin(),magic.end(),bytes.begin());};
    if(starts({0x89,'P','N','G'}))return 0xaf;if(starts({0xff,0xd8}))return 0xae;if(starts({'G','I','F'}))return 0xb0;if(starts({'B','M'}))return 0xab;
    if(starts({'I','I',42,0}) || starts({'M','M',0,42}))return 0xb1;if(starts({0,0,1,0}))return 0xb5;return 0xaa;
}
// 以 BGRA 位元組（即小端序 0xAARRGGBB）建立 32bppARGB 點陣圖並寫出 handle；解碼與材質筆刷取回影像共用。
template<class Allocate,class Release>
std::uint32_t store_bitmap(GdiplusState& g,GuestMemory& memory,std::span<const std::uint8_t> pixels,std::uint32_t width,std::uint32_t height,std::uint64_t output,std::uint8_t pointer_bits,Allocate allocate,Release release,std::uint8_t raw=0xaa,std::uint32_t flags=0){
    GdiplusState::Bitmap bitmap{width,height,argb32,static_cast<std::int64_t>(width)*4,0,true};bitmap.raw=raw;bitmap.image_flags=flags;
    memory.check(GuestAddress{output},pointer_bits/8,Access::Write);
    bitmap.scan0=allocate(pixels.size());if(!bitmap.scan0)return out_of_memory;
    std::uint64_t handle=0;
    try{
        memory.write(GuestAddress{bitmap.scan0},pixels);handle=allocate(16U);
        if(!handle){release(bitmap.scan0);return out_of_memory;}
        g.bitmaps.emplace(handle,bitmap);memory.store(GuestAddress{output},pointer_bits,handle);return ok;
    }catch(...){if(handle){g.bitmaps.erase(handle);release(handle);}release(bitmap.scan0);throw;}
}
template<class Allocate,class Release>
std::uint32_t decode_bitmap(GdiplusState& g,GuestMemory& memory,std::span<const std::uint8_t> bytes,std::uint64_t output,std::uint8_t pointer_bits,Allocate allocate,Release release){
    texture_image::Decoded image;
    try{image=texture_image::decode(bytes);}catch(const texture_image::Error&){return unknown_image_format;}
    if(image.desc.format!=28 || image.pixels.empty())return unknown_image_format;
    auto& pixels=image.pixels.front();for(std::size_t i=0;i+3<pixels.size();i+=4)std::swap(pixels[i],pixels[i+2]);
    // 解碼影像：ImageFlagsColorSpaceRGB 0x10、HasRealPixelSize 0x2000；含透明像素時 HasAlpha 0x2。
    bool translucent=false;for(std::size_t i=3;i<pixels.size();i+=4)if(pixels[i]!=255){translucent=true;break;}
    return store_bitmap(g,memory,pixels,image.desc.width,image.desc.height,output,pointer_bits,allocate,release,raw_format(bytes),0x2010U|(translucent?2U:0U));
}
}
std::span<const WindowsServiceDescriptor> gdiplus_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdiplus.dll","GdiplusStartup",3},{"gdiplus.dll","GdiplusShutdown",1},
        {"gdiplus.dll","GdiplusNotificationHook",1},{"gdiplus.dll","GdiplusNotificationUnhook",1},
        {"gdiplus.dll","GdipAlloc",1},{"gdiplus.dll","GdipFree",1},
        {"gdiplus.dll","GdipCreateBitmapFromScan0",6},{"gdiplus.dll","GdipBitmapSetPixel",4},{"gdiplus.dll","GdipBitmapGetPixel",4},
        {"gdiplus.dll","GdipCloneImage",2},{"gdiplus.dll","GdipDisposeImage",1},{"gdiplus.dll","GdipCreateHBITMAPFromBitmap",3},
        {"gdiplus.dll","GdipGetImageWidth",2},{"gdiplus.dll","GdipGetImageHeight",2},{"gdiplus.dll","GdipGetImagePixelFormat",2},
        {"gdiplus.dll","GdipCreateFromHDC",2},{"gdiplus.dll","GdipDeleteGraphics",1},
        {"gdiplus.dll","GdipDrawImageI",4},{"gdiplus.dll","GdipDrawImageRectI",6},
        {"gdiplus.dll","GdipLoadImageFromStream",2},{"gdiplus.dll","GdipLoadImageFromStreamICM",2},
        {"gdiplus.dll","GdipCreateBitmapFromStream",2},{"gdiplus.dll","GdipCreateBitmapFromStreamICM",2},
        {"gdiplus.dll","GdipGetImageEncodersSize",2},{"gdiplus.dll","GdipGetImageEncoders",3},
        {"gdiplus.dll","GdipGetImageDecodersSize",2},{"gdiplus.dll","GdipGetImageDecoders",3},
        {"gdiplus.dll","GdipGetEncoderParameterListSize",3},{"gdiplus.dll","GdipGetEncoderParameterList",4},
        {"gdiplus.dll","GdipSaveImageToFile",4},{"gdiplus.dll","GdipSaveImageToStream",4},
        {"gdiplus.dll","GdipGetMetafileHeaderFromEmf",2},{"gdiplus.dll","GdipGetMetafileHeaderFromWmf",3},
        {"gdiplus.dll","GdipGetMetafileHeaderFromFile",2},{"gdiplus.dll","GdipGetMetafileHeaderFromStream",2},
        {"gdiplus.dll","GdipGetMetafileHeaderFromMetafile",2},{"gdiplus.dll","GdipGetHemfFromMetafile",2},
        {"gdiplus.dll","GdipCreateMetafileFromEmf",3},{"gdiplus.dll","GdipCreateMetafileFromWmf",4},
        {"gdiplus.dll","GdipCreateMetafileFromFile",2},{"gdiplus.dll","GdipCreateMetafileFromWmfFile",3},
        {"gdiplus.dll","GdipCreateMetafileFromStream",2},
        {"gdiplus.dll","GdipCreateStreamOnFile",3},
        {"gdiplus.dll","GdipLoadImageFromFile",2},{"gdiplus.dll","GdipLoadImageFromFileICM",2},
        {"gdiplus.dll","GdipCreateBitmapFromFile",2},{"gdiplus.dll","GdipCreateBitmapFromFileICM",2},
        {"gdiplus.dll","GdipBitmapLockBits",5},{"gdiplus.dll","GdipBitmapUnlockBits",2},
        {"gdiplus.dll","GdipGetImagePaletteSize",2},{"gdiplus.dll","GdipGetImagePalette",3},{"gdiplus.dll","GdipGetImageGraphicsContext",2},
        {"gdiplus.dll","GdipCreateBitmapFromHBITMAP",3},
        {"gdiplus.dll","GdipCreatePath",2},{"gdiplus.dll","GdipDeletePath",1},
        {"gdiplus.dll","GdipAddPathLine",5},{"gdiplus.dll","GdipAddPathArc",7},{"gdiplus.dll","GdipClosePathFigure",1},
        {"gdiplus.dll","GdipCreateSolidFill",2},{"gdiplus.dll","GdipCloneBrush",2},{"gdiplus.dll","GdipDeleteBrush",1},
        {"gdiplus.dll","GdipCreatePen1",4},{"gdiplus.dll","GdipDeletePen",1},
        {"gdiplus.dll","GdipSetPenStartCap",2},{"gdiplus.dll","GdipSetPenEndCap",2},{"gdiplus.dll","GdipSetPenLineJoin",2},
        {"gdiplus.dll","GdipSetSmoothingMode",2},{"gdiplus.dll","GdipGetSmoothingMode",2},
        {"gdiplus.dll","GdipFillPath",3},{"gdiplus.dll","GdipDrawPath",3},
        {"gdiplus.dll","GdipFillRectangle",6},{"gdiplus.dll","GdipDrawRectangle",6},
        {"gdiplus.dll","GdipFillEllipse",6},{"gdiplus.dll","GdipDrawEllipse",6},
        {"gdiplus.dll","GdipFillPolygon",5},{"gdiplus.dll","GdipDrawLine",6},{"gdiplus.dll","GdipDrawLineI",6},
        // 幾何家族（gdiplus_geometry_handlers.inc）。
        {"gdiplus.dll","GdipCreateMatrix",1},{"gdiplus.dll","GdipCreateMatrix2",7},{"gdiplus.dll","GdipCreateMatrix3",3},{"gdiplus.dll","GdipCreateMatrix3I",3},{"gdiplus.dll","GdipCloneMatrix",2},{"gdiplus.dll","GdipDeleteMatrix",1},{"gdiplus.dll","GdipSetMatrixElements",7},{"gdiplus.dll","GdipGetMatrixElements",2},{"gdiplus.dll","GdipMultiplyMatrix",3},{"gdiplus.dll","GdipTranslateMatrix",4},{"gdiplus.dll","GdipScaleMatrix",4},{"gdiplus.dll","GdipShearMatrix",4},{"gdiplus.dll","GdipRotateMatrix",3},{"gdiplus.dll","GdipInvertMatrix",1},{"gdiplus.dll","GdipTransformMatrixPoints",3},{"gdiplus.dll","GdipTransformMatrixPointsI",3},{"gdiplus.dll","GdipVectorTransformMatrixPoints",3},{"gdiplus.dll","GdipVectorTransformMatrixPointsI",3},{"gdiplus.dll","GdipIsMatrixIdentity",2},{"gdiplus.dll","GdipIsMatrixInvertible",2},{"gdiplus.dll","GdipIsMatrixEqual",3},
        {"gdiplus.dll","GdipCreateFromHWND",2},{"gdiplus.dll","GdipCreateFromHWNDICM",2},{"gdiplus.dll","GdipCreateFromHDC2",3},{"gdiplus.dll","GdipGetDC",2},{"gdiplus.dll","GdipReleaseDC",2},{"gdiplus.dll","GdipFlush",2},{"gdiplus.dll","GdipSetWorldTransform",2},{"gdiplus.dll","GdipGetWorldTransform",2},{"gdiplus.dll","GdipResetWorldTransform",1},{"gdiplus.dll","GdipMultiplyWorldTransform",3},{"gdiplus.dll","GdipTranslateWorldTransform",4},{"gdiplus.dll","GdipScaleWorldTransform",4},{"gdiplus.dll","GdipRotateWorldTransform",3},{"gdiplus.dll","GdipSetPageUnit",2},{"gdiplus.dll","GdipGetPageUnit",2},{"gdiplus.dll","GdipSetPageScale",2},{"gdiplus.dll","GdipGetPageScale",2},{"gdiplus.dll","GdipGetDpiX",2},{"gdiplus.dll","GdipGetDpiY",2},{"gdiplus.dll","GdipSetRenderingOrigin",3},{"gdiplus.dll","GdipGetRenderingOrigin",3},
        {"gdiplus.dll","GdipSetCompositingMode",2},{"gdiplus.dll","GdipGetCompositingMode",2},{"gdiplus.dll","GdipSetCompositingQuality",2},{"gdiplus.dll","GdipGetCompositingQuality",2},{"gdiplus.dll","GdipSetInterpolationMode",2},{"gdiplus.dll","GdipGetInterpolationMode",2},{"gdiplus.dll","GdipSetPixelOffsetMode",2},{"gdiplus.dll","GdipGetPixelOffsetMode",2},{"gdiplus.dll","GdipSetTextRenderingHint",2},{"gdiplus.dll","GdipGetTextRenderingHint",2},{"gdiplus.dll","GdipSetTextContrast",2},{"gdiplus.dll","GdipGetTextContrast",2},{"gdiplus.dll","GdipSaveGraphics",2},{"gdiplus.dll","GdipRestoreGraphics",2},{"gdiplus.dll","GdipTransformPoints",5},{"gdiplus.dll","GdipTransformPointsI",5},{"gdiplus.dll","GdipGraphicsClear",2},
        {"gdiplus.dll","GdipSetClipRect",6},{"gdiplus.dll","GdipSetClipRectI",6},{"gdiplus.dll","GdipSetClipPath",3},{"gdiplus.dll","GdipSetClipGraphics",3},{"gdiplus.dll","GdipResetClip",1},{"gdiplus.dll","GdipTranslateClip",3},{"gdiplus.dll","GdipTranslateClipI",3},{"gdiplus.dll","GdipGetClipBounds",2},{"gdiplus.dll","GdipGetClipBoundsI",2},{"gdiplus.dll","GdipGetVisibleClipBounds",2},{"gdiplus.dll","GdipGetVisibleClipBoundsI",2},{"gdiplus.dll","GdipIsClipEmpty",2},{"gdiplus.dll","GdipIsVisibleClipEmpty",2},{"gdiplus.dll","GdipIsVisiblePoint",4},{"gdiplus.dll","GdipIsVisiblePointI",4},{"gdiplus.dll","GdipIsVisibleRect",6},{"gdiplus.dll","GdipIsVisibleRectI",6},
        {"gdiplus.dll","GdipCreatePath2",5},{"gdiplus.dll","GdipCreatePath2I",5},{"gdiplus.dll","GdipClonePath",2},{"gdiplus.dll","GdipResetPath",1},{"gdiplus.dll","GdipStartPathFigure",1},{"gdiplus.dll","GdipClosePathFigures",1},{"gdiplus.dll","GdipGetPointCount",2},{"gdiplus.dll","GdipGetPathFillMode",2},{"gdiplus.dll","GdipSetPathFillMode",2},{"gdiplus.dll","GdipGetPathPoints",3},{"gdiplus.dll","GdipGetPathPointsI",3},{"gdiplus.dll","GdipGetPathTypes",3},{"gdiplus.dll","GdipGetPathLastPoint",2},{"gdiplus.dll","GdipTransformPath",2},{"gdiplus.dll","GdipFlattenPath",3},{"gdiplus.dll","GdipReversePath",1},{"gdiplus.dll","GdipAddPathPath",3},{"gdiplus.dll","GdipGetPathWorldBounds",4},{"gdiplus.dll","GdipGetPathWorldBoundsI",4},{"gdiplus.dll","GdipIsVisiblePathPoint",5},{"gdiplus.dll","GdipIsVisiblePathPointI",5},
        {"gdiplus.dll","GdipAddPathLineI",5},{"gdiplus.dll","GdipAddPathLine2",3},{"gdiplus.dll","GdipAddPathLine2I",3},{"gdiplus.dll","GdipAddPathArcI",7},{"gdiplus.dll","GdipAddPathBezier",9},{"gdiplus.dll","GdipAddPathBezierI",9},{"gdiplus.dll","GdipAddPathBeziers",3},{"gdiplus.dll","GdipAddPathBeziersI",3},{"gdiplus.dll","GdipAddPathRectangle",5},{"gdiplus.dll","GdipAddPathRectangleI",5},{"gdiplus.dll","GdipAddPathRectangles",3},{"gdiplus.dll","GdipAddPathRectanglesI",3},{"gdiplus.dll","GdipAddPathEllipse",5},{"gdiplus.dll","GdipAddPathEllipseI",5},{"gdiplus.dll","GdipAddPathPolygon",3},{"gdiplus.dll","GdipAddPathPolygonI",3},{"gdiplus.dll","GdipAddPathPie",7},{"gdiplus.dll","GdipAddPathPieI",7},{"gdiplus.dll","GdipAddPathCurve",3},{"gdiplus.dll","GdipAddPathCurveI",3},{"gdiplus.dll","GdipAddPathCurve2",4},{"gdiplus.dll","GdipAddPathCurve2I",4},{"gdiplus.dll","GdipAddPathCurve3",6},{"gdiplus.dll","GdipAddPathCurve3I",6},{"gdiplus.dll","GdipAddPathClosedCurve",3},{"gdiplus.dll","GdipAddPathClosedCurveI",3},{"gdiplus.dll","GdipAddPathClosedCurve2",4},{"gdiplus.dll","GdipAddPathClosedCurve2I",4},
        {"gdiplus.dll","GdipDrawRectangleI",6},{"gdiplus.dll","GdipFillRectangleI",6},{"gdiplus.dll","GdipDrawEllipseI",6},{"gdiplus.dll","GdipFillEllipseI",6},{"gdiplus.dll","GdipDrawLines",4},{"gdiplus.dll","GdipDrawLinesI",4},{"gdiplus.dll","GdipDrawRectangles",4},{"gdiplus.dll","GdipDrawRectanglesI",4},{"gdiplus.dll","GdipFillRectangles",4},{"gdiplus.dll","GdipFillRectanglesI",4},{"gdiplus.dll","GdipDrawPolygon",4},{"gdiplus.dll","GdipDrawPolygonI",4},{"gdiplus.dll","GdipFillPolygonI",5},{"gdiplus.dll","GdipFillPolygon2",4},{"gdiplus.dll","GdipFillPolygon2I",4},{"gdiplus.dll","GdipDrawArc",8},{"gdiplus.dll","GdipDrawArcI",8},{"gdiplus.dll","GdipDrawPie",8},{"gdiplus.dll","GdipDrawPieI",8},{"gdiplus.dll","GdipFillPie",8},{"gdiplus.dll","GdipFillPieI",8},{"gdiplus.dll","GdipDrawBezier",10},{"gdiplus.dll","GdipDrawBezierI",10},{"gdiplus.dll","GdipDrawBeziers",4},{"gdiplus.dll","GdipDrawBeziersI",4},
        {"gdiplus.dll","GdipDrawCurve",4},{"gdiplus.dll","GdipDrawCurveI",4},{"gdiplus.dll","GdipDrawCurve2",5},{"gdiplus.dll","GdipDrawCurve2I",5},{"gdiplus.dll","GdipDrawCurve3",7},{"gdiplus.dll","GdipDrawCurve3I",7},{"gdiplus.dll","GdipDrawClosedCurve",4},{"gdiplus.dll","GdipDrawClosedCurveI",4},{"gdiplus.dll","GdipDrawClosedCurve2",5},{"gdiplus.dll","GdipDrawClosedCurve2I",5},{"gdiplus.dll","GdipFillClosedCurve",4},{"gdiplus.dll","GdipFillClosedCurveI",4},{"gdiplus.dll","GdipFillClosedCurve2",6},{"gdiplus.dll","GdipFillClosedCurve2I",6},
        {"gdiplus.dll","GdipCreatePen2",4},{"gdiplus.dll","GdipClonePen",2},{"gdiplus.dll","GdipSetPenWidth",2},{"gdiplus.dll","GdipGetPenWidth",2},{"gdiplus.dll","GdipSetPenMiterLimit",2},{"gdiplus.dll","GdipGetPenMiterLimit",2},{"gdiplus.dll","GdipSetPenColor",2},{"gdiplus.dll","GdipGetPenColor",2},{"gdiplus.dll","GdipSetPenUnit",2},{"gdiplus.dll","GdipGetPenUnit",2},{"gdiplus.dll","GdipGetPenStartCap",2},{"gdiplus.dll","GdipGetPenEndCap",2},{"gdiplus.dll","GdipGetPenLineJoin",2},{"gdiplus.dll","GdipSetPenDashStyle",2},{"gdiplus.dll","GdipGetPenDashStyle",2},{"gdiplus.dll","GdipSetPenDashCap197819",2},{"gdiplus.dll","GdipGetPenDashCap197819",2},{"gdiplus.dll","GdipSetPenLineCap197819",4},{"gdiplus.dll","GdipGetPenFillType",2},{"gdiplus.dll","GdipGetPenBrushFill",2},{"gdiplus.dll","GdipSetPenBrushFill",2},{"gdiplus.dll","GdipGetBrushType",2},{"gdiplus.dll","GdipGetSolidFillColor",2},{"gdiplus.dll","GdipSetSolidFillColor",2},
        // Region 家族（gdiplus_region_handlers.inc）。
        {"gdiplus.dll","GdipCreateRegion",1},{"gdiplus.dll","GdipCreateRegionRect",2},{"gdiplus.dll","GdipCreateRegionRectI",2},{"gdiplus.dll","GdipCreateRegionPath",2},{"gdiplus.dll","GdipCreateRegionRgnData",3},{"gdiplus.dll","GdipCreateRegionHrgn",2},{"gdiplus.dll","GdipCloneRegion",2},{"gdiplus.dll","GdipDeleteRegion",1},{"gdiplus.dll","GdipSetInfinite",1},{"gdiplus.dll","GdipSetEmpty",1},{"gdiplus.dll","GdipCombineRegionRect",3},{"gdiplus.dll","GdipCombineRegionRectI",3},{"gdiplus.dll","GdipCombineRegionPath",3},{"gdiplus.dll","GdipCombineRegionRegion",3},{"gdiplus.dll","GdipTransformRegion",2},{"gdiplus.dll","GdipTranslateRegion",3},{"gdiplus.dll","GdipTranslateRegionI",3},{"gdiplus.dll","GdipGetRegionBounds",3},{"gdiplus.dll","GdipGetRegionBoundsI",3},{"gdiplus.dll","GdipGetRegionDataSize",2},{"gdiplus.dll","GdipGetRegionData",4},{"gdiplus.dll","GdipGetRegionHRgn",3},{"gdiplus.dll","GdipIsEmptyRegion",3},{"gdiplus.dll","GdipIsInfiniteRegion",3},{"gdiplus.dll","GdipIsEqualRegion",4},{"gdiplus.dll","GdipIsVisibleRegionPoint",5},{"gdiplus.dll","GdipIsVisibleRegionPointI",5},{"gdiplus.dll","GdipIsVisibleRegionRect",7},{"gdiplus.dll","GdipIsVisibleRegionRectI",7},{"gdiplus.dll","GdipGetRegionScansCount",3},{"gdiplus.dll","GdipGetRegionScans",4},{"gdiplus.dll","GdipGetRegionScansI",4},{"gdiplus.dll","GdipFillRegion",3},{"gdiplus.dll","GdipSetClipRegion",3},{"gdiplus.dll","GdipSetClipHrgn",3},{"gdiplus.dll","GdipGetClip",2},
        // 筆刷家族（gdiplus_brush_handlers.inc）。
        {"gdiplus.dll","GdipCreateLineBrush",6},{"gdiplus.dll","GdipCreateLineBrushI",6},{"gdiplus.dll","GdipCreateLineBrushFromRect",6},{"gdiplus.dll","GdipCreateLineBrushFromRectI",6},{"gdiplus.dll","GdipCreateLineBrushFromRectWithAngle",7},{"gdiplus.dll","GdipCreateLineBrushFromRectWithAngleI",7},{"gdiplus.dll","GdipSetLineColors",3},{"gdiplus.dll","GdipGetLineColors",2},{"gdiplus.dll","GdipGetLineRect",2},{"gdiplus.dll","GdipGetLineRectI",2},{"gdiplus.dll","GdipCreatePathGradient",4},{"gdiplus.dll","GdipCreatePathGradientI",4},{"gdiplus.dll","GdipCreatePathGradientFromPath",2},{"gdiplus.dll","GdipGetPathGradientCenterColor",2},{"gdiplus.dll","GdipSetPathGradientCenterColor",2},{"gdiplus.dll","GdipGetPathGradientCenterPoint",2},{"gdiplus.dll","GdipGetPathGradientCenterPointI",2},{"gdiplus.dll","GdipSetPathGradientCenterPoint",2},{"gdiplus.dll","GdipSetPathGradientCenterPointI",2},{"gdiplus.dll","GdipGetPathGradientFocusScales",3},{"gdiplus.dll","GdipSetPathGradientFocusScales",3},{"gdiplus.dll","GdipGetPathGradientPointCount",2},{"gdiplus.dll","GdipGetPathGradientSurroundColorCount",2},{"gdiplus.dll","GdipSetPathGradientSurroundColorsWithCount",3},{"gdiplus.dll","GdipGetPathGradientSurroundColorsWithCount",3},{"gdiplus.dll","GdipGetPathGradientRect",2},{"gdiplus.dll","GdipGetPathGradientRectI",2},{"gdiplus.dll","GdipGetPathGradientPath",2},{"gdiplus.dll","GdipSetPathGradientPath",2},{"gdiplus.dll","GdipCreateHatchBrush",4},{"gdiplus.dll","GdipGetHatchStyle",2},{"gdiplus.dll","GdipGetHatchForegroundColor",2},{"gdiplus.dll","GdipGetHatchBackgroundColor",2},{"gdiplus.dll","GdipCreateTexture",3},{"gdiplus.dll","GdipCreateTexture2",7},{"gdiplus.dll","GdipCreateTexture2I",7},{"gdiplus.dll","GdipCreateTextureIA",7},{"gdiplus.dll","GdipCreateTextureIAI",7},{"gdiplus.dll","GdipGetTextureImage",2},{"gdiplus.dll","GdipSetLineTransform",2},{"gdiplus.dll","GdipGetLineTransform",2},{"gdiplus.dll","GdipResetLineTransform",1},{"gdiplus.dll","GdipMultiplyLineTransform",3},{"gdiplus.dll","GdipTranslateLineTransform",4},{"gdiplus.dll","GdipScaleLineTransform",4},{"gdiplus.dll","GdipRotateLineTransform",3},{"gdiplus.dll","GdipSetLineWrapMode",2},{"gdiplus.dll","GdipGetLineWrapMode",2},{"gdiplus.dll","GdipSetPathGradientTransform",2},{"gdiplus.dll","GdipGetPathGradientTransform",2},{"gdiplus.dll","GdipResetPathGradientTransform",1},{"gdiplus.dll","GdipMultiplyPathGradientTransform",3},{"gdiplus.dll","GdipTranslatePathGradientTransform",4},{"gdiplus.dll","GdipScalePathGradientTransform",4},{"gdiplus.dll","GdipRotatePathGradientTransform",3},{"gdiplus.dll","GdipSetPathGradientWrapMode",2},{"gdiplus.dll","GdipGetPathGradientWrapMode",2},{"gdiplus.dll","GdipSetTextureTransform",2},{"gdiplus.dll","GdipGetTextureTransform",2},{"gdiplus.dll","GdipResetTextureTransform",1},{"gdiplus.dll","GdipMultiplyTextureTransform",3},{"gdiplus.dll","GdipTranslateTextureTransform",4},{"gdiplus.dll","GdipScaleTextureTransform",4},{"gdiplus.dll","GdipRotateTextureTransform",3},{"gdiplus.dll","GdipSetTextureWrapMode",2},{"gdiplus.dll","GdipGetTextureWrapMode",2},{"gdiplus.dll","GdipSetLineGammaCorrection",2},{"gdiplus.dll","GdipGetLineGammaCorrection",2},{"gdiplus.dll","GdipSetLineBlend",4},{"gdiplus.dll","GdipGetLineBlend",4},{"gdiplus.dll","GdipGetLineBlendCount",2},{"gdiplus.dll","GdipSetLinePresetBlend",4},{"gdiplus.dll","GdipGetLinePresetBlend",4},{"gdiplus.dll","GdipGetLinePresetBlendCount",2},{"gdiplus.dll","GdipSetLineLinearBlend",3},{"gdiplus.dll","GdipSetLineSigmaBlend",3},{"gdiplus.dll","GdipSetPathGradientGammaCorrection",2},{"gdiplus.dll","GdipGetPathGradientGammaCorrection",2},{"gdiplus.dll","GdipSetPathGradientBlend",4},{"gdiplus.dll","GdipGetPathGradientBlend",4},{"gdiplus.dll","GdipGetPathGradientBlendCount",2},{"gdiplus.dll","GdipSetPathGradientPresetBlend",4},{"gdiplus.dll","GdipGetPathGradientPresetBlend",4},{"gdiplus.dll","GdipGetPathGradientPresetBlendCount",2},{"gdiplus.dll","GdipSetPathGradientLinearBlend",3},{"gdiplus.dll","GdipSetPathGradientSigmaBlend",3},
        // 影像家族（gdiplus_image_handlers.inc）。
        {"gdiplus.dll","GdipGetImageBounds",3},{"gdiplus.dll","GdipGetImageDimension",3},{"gdiplus.dll","GdipGetImageFlags",2},{"gdiplus.dll","GdipGetImageHorizontalResolution",2},{"gdiplus.dll","GdipGetImageVerticalResolution",2},{"gdiplus.dll","GdipBitmapSetResolution",3},{"gdiplus.dll","GdipGetImageRawFormat",2},{"gdiplus.dll","GdipGetImageType",2},{"gdiplus.dll","GdipImageForceValidation",1},{"gdiplus.dll","GdipImageSetAbort",2},{"gdiplus.dll","GdipGraphicsSetAbort",2},{"gdiplus.dll","GdipGetNearestColor",2},{"gdiplus.dll","GdipImageGetFrameDimensionsCount",2},{"gdiplus.dll","GdipImageGetFrameDimensionsList",3},{"gdiplus.dll","GdipImageGetFrameCount",3},{"gdiplus.dll","GdipImageSelectActiveFrame",3},{"gdiplus.dll","GdipSetImagePalette",2},{"gdiplus.dll","GdipInitializePalette",5},{"gdiplus.dll","GdipCreateHalftonePalette",0},{"gdiplus.dll","GdipImageRotateFlip",2},{"gdiplus.dll","GdipCloneBitmapArea",7},{"gdiplus.dll","GdipCloneBitmapAreaI",7},{"gdiplus.dll","GdipBitmapConvertFormat",6},{"gdiplus.dll","GdipBitmapGetHistogramSize",2},{"gdiplus.dll","GdipBitmapGetHistogram",7},{"gdiplus.dll","GdipGetImageThumbnail",6},{"gdiplus.dll","GdipCreateBitmapFromGraphics",4},{"gdiplus.dll","GdipCreateBitmapFromGdiDib",3},{"gdiplus.dll","GdipCreateBitmapFromHICON",2},{"gdiplus.dll","GdipCreateHICONFromBitmap",2},{"gdiplus.dll","GdipCreateBitmapFromResource",3},{"gdiplus.dll","GdipGetPropertyCount",2},{"gdiplus.dll","GdipGetPropertyIdList",3},{"gdiplus.dll","GdipGetPropertyItemSize",3},{"gdiplus.dll","GdipGetPropertyItem",4},{"gdiplus.dll","GdipGetPropertySize",3},{"gdiplus.dll","GdipGetAllPropertyItems",4},{"gdiplus.dll","GdipRemovePropertyItem",2},{"gdiplus.dll","GdipSetPropertyItem",2},
        // DrawImage／ImageAttributes／CachedBitmap（gdiplus_draw_image_handlers.inc）。
        {"gdiplus.dll","GdipCreateEffect",2,0,1},{"gdiplus.dll","GdipDeleteEffect",1},{"gdiplus.dll","GdipGetEffectParameterSize",2},{"gdiplus.dll","GdipSetEffectParameters",3},
        {"gdiplus.dll","GdipGetEffectParameters",3},{"gdiplus.dll","GdipBitmapApplyEffect",6},{"gdiplus.dll","GdipBitmapCreateApplyEffect",9},
        {"gdiplus.dll","GdipCreateImageAttributes",1},{"gdiplus.dll","GdipCloneImageAttributes",2},{"gdiplus.dll","GdipDisposeImageAttributes",1},{"gdiplus.dll","GdipResetImageAttributes",2},{"gdiplus.dll","GdipSetImageAttributesToIdentity",2},{"gdiplus.dll","GdipSetImageAttributesColorMatrix",6},{"gdiplus.dll","GdipSetImageAttributesThreshold",4},{"gdiplus.dll","GdipSetImageAttributesGamma",4},{"gdiplus.dll","GdipSetImageAttributesNoOp",3},{"gdiplus.dll","GdipSetImageAttributesColorKeys",5},{"gdiplus.dll","GdipSetImageAttributesOutputChannel",4},{"gdiplus.dll","GdipSetImageAttributesOutputChannelColorProfile",4},{"gdiplus.dll","GdipSetImageAttributesRemapTable",5},{"gdiplus.dll","GdipSetImageAttributesWrapMode",4},{"gdiplus.dll","GdipSetImageAttributesCachedBackground",2},{"gdiplus.dll","GdipGetImageAttributesAdjustedPalette",3},{"gdiplus.dll","GdipDrawImage",4},{"gdiplus.dll","GdipDrawImageRect",6},{"gdiplus.dll","GdipDrawImagePoints",4},{"gdiplus.dll","GdipDrawImagePointsI",4},{"gdiplus.dll","GdipDrawImagePointRect",9},{"gdiplus.dll","GdipDrawImagePointRectI",9},{"gdiplus.dll","GdipDrawImageRectRect",14},{"gdiplus.dll","GdipDrawImageRectRectI",14},{"gdiplus.dll","GdipDrawImagePointsRect",12},{"gdiplus.dll","GdipDrawImagePointsRectI",12},{"gdiplus.dll","GdipDrawImageFX",7},{"gdiplus.dll","GdipCreateCachedBitmap",3},{"gdiplus.dll","GdipDeleteCachedBitmap",1},{"gdiplus.dll","GdipDrawCachedBitmap",4},
        // 文字家族（gdiplus_text_handlers.inc）。
        {"gdiplus.dll","GdipNewInstalledFontCollection",1},{"gdiplus.dll","GdipNewPrivateFontCollection",1},{"gdiplus.dll","GdipDeletePrivateFontCollection",1},{"gdiplus.dll","GdipPrivateAddFontFile",2},{"gdiplus.dll","GdipPrivateAddMemoryFont",3},{"gdiplus.dll","GdipGetFontCollectionFamilyCount",2},{"gdiplus.dll","GdipGetFontCollectionFamilyList",4},{"gdiplus.dll","GdipCreateFontFamilyFromName",3},{"gdiplus.dll","GdipGetGenericFontFamilySansSerif",1},{"gdiplus.dll","GdipGetGenericFontFamilySerif",1},{"gdiplus.dll","GdipGetGenericFontFamilyMonospace",1},{"gdiplus.dll","GdipCloneFontFamily",2},{"gdiplus.dll","GdipDeleteFontFamily",1},{"gdiplus.dll","GdipGetFamilyName",3},{"gdiplus.dll","GdipIsStyleAvailable",3},{"gdiplus.dll","GdipGetEmHeight",3},{"gdiplus.dll","GdipGetCellAscent",3},{"gdiplus.dll","GdipGetCellDescent",3},{"gdiplus.dll","GdipGetLineSpacing",3},{"gdiplus.dll","GdipCreateFont",5},{"gdiplus.dll","GdipCreateFontFromLogfontW",3},{"gdiplus.dll","GdipCreateFontFromLogfontA",3},{"gdiplus.dll","GdipCreateFontFromDC",2},{"gdiplus.dll","GdipCloneFont",2},{"gdiplus.dll","GdipDeleteFont",1},{"gdiplus.dll","GdipGetFamily",2},{"gdiplus.dll","GdipGetFontSize",2},{"gdiplus.dll","GdipGetFontStyle",2},{"gdiplus.dll","GdipGetFontUnit",2},{"gdiplus.dll","GdipGetFontHeight",3},{"gdiplus.dll","GdipGetFontHeightGivenDPI",3},{"gdiplus.dll","GdipGetLogFontW",3},{"gdiplus.dll","GdipGetLogFontA",3},{"gdiplus.dll","GdipCreateStringFormat",3},{"gdiplus.dll","GdipStringFormatGetGenericDefault",1},{"gdiplus.dll","GdipStringFormatGetGenericTypographic",1},{"gdiplus.dll","GdipCloneStringFormat",2},{"gdiplus.dll","GdipDeleteStringFormat",1},{"gdiplus.dll","GdipSetStringFormatAlign",2},{"gdiplus.dll","GdipGetStringFormatAlign",2},{"gdiplus.dll","GdipSetStringFormatLineAlign",2},{"gdiplus.dll","GdipGetStringFormatLineAlign",2},{"gdiplus.dll","GdipSetStringFormatTrimming",2},{"gdiplus.dll","GdipGetStringFormatTrimming",2},{"gdiplus.dll","GdipSetStringFormatHotkeyPrefix",2},{"gdiplus.dll","GdipGetStringFormatHotkeyPrefix",2},{"gdiplus.dll","GdipSetStringFormatFlags",2},{"gdiplus.dll","GdipGetStringFormatFlags",2},{"gdiplus.dll","GdipSetStringFormatTabStops",4},{"gdiplus.dll","GdipGetStringFormatTabStopCount",2},{"gdiplus.dll","GdipGetStringFormatTabStops",4},{"gdiplus.dll","GdipSetStringFormatDigitSubstitution",3},{"gdiplus.dll","GdipGetStringFormatDigitSubstitution",3},{"gdiplus.dll","GdipSetStringFormatMeasurableCharacterRanges",3},{"gdiplus.dll","GdipGetStringFormatMeasurableCharacterRangeCount",2},{"gdiplus.dll","GdipDrawString",7},{"gdiplus.dll","GdipMeasureString",9},{"gdiplus.dll","GdipMeasureCharacterRanges",8},{"gdiplus.dll","GdipDrawDriverString",8},{"gdiplus.dll","GdipMeasureDriverString",8},{"gdiplus.dll","GdipAddPathString",8},{"gdiplus.dll","GdipAddPathStringI",8},
        // 畫筆進階、線端、路徑迭代器與容器（gdiplus_pen_path_handlers.inc）。
        {"gdiplus.dll","GdipSetPenDashArray",3},{"gdiplus.dll","GdipGetPenDashArray",3},{"gdiplus.dll","GdipGetPenDashCount",2},{"gdiplus.dll","GdipGetPenDashOffset",2},{"gdiplus.dll","GdipSetPenDashOffset",2},{"gdiplus.dll","GdipSetPenCompoundArray",3},{"gdiplus.dll","GdipGetPenCompoundArray",3},{"gdiplus.dll","GdipGetPenCompoundCount",2},{"gdiplus.dll","GdipSetPenTransform",2},{"gdiplus.dll","GdipGetPenTransform",2},{"gdiplus.dll","GdipResetPenTransform",1},{"gdiplus.dll","GdipMultiplyPenTransform",3},{"gdiplus.dll","GdipTranslatePenTransform",4},{"gdiplus.dll","GdipScalePenTransform",4},{"gdiplus.dll","GdipRotatePenTransform",3},{"gdiplus.dll","GdipGetPenMode",2},{"gdiplus.dll","GdipSetPenMode",2},{"gdiplus.dll","GdipSetPenCustomStartCap",2},{"gdiplus.dll","GdipGetPenCustomStartCap",2},{"gdiplus.dll","GdipSetPenCustomEndCap",2},{"gdiplus.dll","GdipGetPenCustomEndCap",2},{"gdiplus.dll","GdipCreateCustomLineCap",5},{"gdiplus.dll","GdipCloneCustomLineCap",2},{"gdiplus.dll","GdipDeleteCustomLineCap",1},{"gdiplus.dll","GdipGetCustomLineCapBaseCap",2},{"gdiplus.dll","GdipSetCustomLineCapBaseCap",2},{"gdiplus.dll","GdipGetCustomLineCapBaseInset",2},{"gdiplus.dll","GdipSetCustomLineCapBaseInset",2},{"gdiplus.dll","GdipGetCustomLineCapStrokeJoin",2},{"gdiplus.dll","GdipSetCustomLineCapStrokeJoin",2},{"gdiplus.dll","GdipGetCustomLineCapStrokeCaps",3},{"gdiplus.dll","GdipSetCustomLineCapStrokeCaps",3},{"gdiplus.dll","GdipGetCustomLineCapType",2},{"gdiplus.dll","GdipGetCustomLineCapWidthScale",2},{"gdiplus.dll","GdipSetCustomLineCapWidthScale",2},{"gdiplus.dll","GdipCreateAdjustableArrowCap",4},{"gdiplus.dll","GdipGetAdjustableArrowCapHeight",2},{"gdiplus.dll","GdipSetAdjustableArrowCapHeight",2},{"gdiplus.dll","GdipGetAdjustableArrowCapWidth",2},{"gdiplus.dll","GdipSetAdjustableArrowCapWidth",2},{"gdiplus.dll","GdipGetAdjustableArrowCapMiddleInset",2},{"gdiplus.dll","GdipSetAdjustableArrowCapMiddleInset",2},{"gdiplus.dll","GdipGetAdjustableArrowCapFillState",2},{"gdiplus.dll","GdipSetAdjustableArrowCapFillState",2},{"gdiplus.dll","GdipCreatePathIter",2},{"gdiplus.dll","GdipDeletePathIter",1},{"gdiplus.dll","GdipPathIterCopyData",6},{"gdiplus.dll","GdipPathIterEnumerate",5},{"gdiplus.dll","GdipPathIterGetCount",2},{"gdiplus.dll","GdipPathIterGetSubpathCount",2},{"gdiplus.dll","GdipPathIterHasCurve",2},{"gdiplus.dll","GdipPathIterIsValid",2},{"gdiplus.dll","GdipPathIterNextMarker",4},{"gdiplus.dll","GdipPathIterNextMarkerPath",3},{"gdiplus.dll","GdipPathIterNextPathType",5},{"gdiplus.dll","GdipPathIterNextSubpath",5},{"gdiplus.dll","GdipPathIterNextSubpathPath",4},{"gdiplus.dll","GdipPathIterRewind",1},{"gdiplus.dll","GdipGetPathData",2},{"gdiplus.dll","GdipSetPathMarker",1},{"gdiplus.dll","GdipClearPathMarkers",1},{"gdiplus.dll","GdipIsOutlineVisiblePathPoint",6},{"gdiplus.dll","GdipIsOutlineVisiblePathPointI",6},{"gdiplus.dll","GdipWidenPath",4},{"gdiplus.dll","GdipWarpPath",10},{"gdiplus.dll","GdipWindingModeOutline",3},{"gdiplus.dll","GdipBeginContainer",5},{"gdiplus.dll","GdipBeginContainer2",2},{"gdiplus.dll","GdipBeginContainerI",5},{"gdiplus.dll","GdipEndContainer",2},{"gdiplus.dll","GdipResetPageTransform",1},
    };return table;
}
#include "gdiplus_metafile_read.inc"
std::optional<std::uint32_t> WindowsServices::invoke_gdiplus(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    if(!gdiplus_)gdiplus_=std::make_shared<GdiplusState>();
    else if(gdiplus_.use_count()>1)gdiplus_=std::make_shared<GdiplusState>(*gdiplus_);
    auto& g=*gdiplus_;
    const auto pointer_bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto u32=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto s32=[&](unsigned n){return std::bit_cast<std::int32_t>(u32(n));};
    // 物件 handle 指向客體堆積中的小區塊，保證唯一且是有效位址；客體只當作不透明指標傳回。
    const auto new_handle=[&]{return allocate_heap(memory,16);};
    const auto release_handle=[&](std::uint64_t handle){(void)free_heap(memory,handle,state);};
    try{
        if(symbol=="GdipAlloc")return finish(allocate_heap(memory,std::max<std::uint64_t>(arg(0),1)));
        if(symbol=="GdipFree"){if(arg(0))(void)free_heap(memory,arg(0),state);return finish(0);}
        if(symbol=="GdiplusStartup"){
            // GdiplusStartupInput：版本、DebugEventCallback、SuppressBackgroundThread、SuppressExternalCodecs。
            if(!arg(0) || !arg(1))return finish(invalid_parameter);
            const auto version=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
            if(version<1 || version>3)return finish(unsupported_version);
            const auto suppress=memory.load(GuestAddress{arg(1)+(x86_?8U:16U)},32)!=0;
            if(suppress){
                // 背景執行緒由呼叫端驅動：輸出 NotificationHook／Unhook 函式指標。
                if(!arg(2))return finish(invalid_parameter);
                memory.store(GuestAddress{arg(2)},pointer_bits,bind_import("gdiplus.dll","GdiplusNotificationHook",true).value);
                memory.store(GuestAddress{arg(2)+(x86_?4U:8U)},pointer_bits,bind_import("gdiplus.dll","GdiplusNotificationUnhook",true).value);
            }
            memory.store(GuestAddress{arg(0)},pointer_bits,g.next_token++);++g.startups;return finish(ok);
        }
        if(symbol=="GdiplusShutdown"){if(g.startups)--g.startups;return finish(0);}
        if(symbol=="GdiplusNotificationHook"){if(!arg(0))return finish(invalid_parameter);memory.store(GuestAddress{arg(0)},pointer_bits,g.next_token++);return finish(ok);}
        if(symbol=="GdiplusNotificationUnhook")return finish(0);
        if(!g.startups)return finish(not_initialized);
        if(const auto target=g.image_graphics.find(arg(0));target!=g.image_graphics.end()){
            if(symbol.starts_with("GdipDraw") || symbol.starts_with("GdipFill") || symbol=="GdipGraphicsClear")target->second.dirty=true;
            else if(symbol=="GdipGetDC")target->second.held=true;else if(symbol=="GdipReleaseDC"){target->second.held=false;target->second.dirty=true;}
        }
#include "gdiplus_metafile_handlers.inc"
#include "gdiplus_vector_handlers.inc"
#include "gdiplus_geometry_handlers.inc"
#include "gdiplus_pen_path_handlers.inc"
#include "gdiplus_region_handlers.inc"
        if(symbol=="GdipGetImageEncodersSize" || symbol=="GdipGetImageDecodersSize"){
            if(!arg(0) || !arg(1))return finish(invalid_parameter);
            memory.check(GuestAddress{arg(0)},4,Access::Write);memory.check(GuestAddress{arg(1)},4,Access::Write);
            memory.store(GuestAddress{arg(0)},32,gdiplus_codec::codecs.size());
            memory.store(GuestAddress{arg(1)},32,gdiplus_codec::information(x86_,0).size());return finish(ok);
        }
        if(symbol=="GdipGetImageEncoders" || symbol=="GdipGetImageDecoders"){
            if(!arg(2) || u32(0)!=gdiplus_codec::codecs.size())return finish(invalid_parameter);
            const auto bytes=gdiplus_codec::information(x86_,arg(2));if(u32(1)!=bytes.size())return finish(invalid_parameter);
            memory.write(GuestAddress{arg(2)},bytes);return finish(ok);
        }
        if(symbol=="GdipCreateStreamOnFile"){
            const bool write=(u32(1)&0x40000000U)!=0, read=(u32(1)&0x80000000U)!=0;
            if(!arg(0) || !arg(2) || (!read && !write))return finish(invalid_parameter);
            // 寫入優先，建立或截短；兩種模式都拒絕其他寫入者。
            const unsigned mode=write?0x1021U:0x20U;
            const auto hr=nested_service(state,memory,"shlwapi.dll","SHCreateStreamOnFileW",{arg(0),mode,arg(2)});
            if(!hr)return finish(ok);
            if(hr==0x8007000eU)return finish(out_of_memory);
            return finish(hr==0x80070057U?invalid_parameter:generic_error);
        }
        if(symbol=="GdipGetMetafileHeaderFromEmf" || symbol=="GdipGetMetafileHeaderFromWmf" ||
           symbol=="GdipCreateMetafileFromEmf" || symbol=="GdipCreateMetafileFromWmf"){
            const bool header=symbol.starts_with("GdipGet"),wmf=symbol.ends_with("Wmf");
            const auto output=arg(service.arguments-1);
            if(!arg(0) || !output)return finish(invalid_parameter);
            if(!header)memory.store(GuestAddress{output},pointer_bits,0);
            std::span<const std::uint8_t> bytes;
            if(wmf){
                const auto found=gui_.windows_metafiles.find(arg(0));if(found==gui_.windows_metafiles.end())return finish(generic_error);
                bytes=found->second;
            }else{
                const auto found=gui_.metafiles.find(arg(0));if(found==gui_.metafiles.end())return finish(generic_error);
                bytes=found->second.bytes;
            }
            return finish(read_gdiplus_image(state,memory,bytes,output,symbol,arg(0),!header && u32(1),wmf?arg(header?1:2):0));
        }
        if(symbol=="GdipLoadImageFromFile" || symbol=="GdipLoadImageFromFileICM" || symbol=="GdipCreateBitmapFromFile" ||
           symbol=="GdipCreateBitmapFromFileICM" || symbol=="GdipGetMetafileHeaderFromFile" ||
           symbol=="GdipCreateMetafileFromFile" || symbol=="GdipCreateMetafileFromWmfFile"){
            const auto output=arg(service.arguments-1);
            if(!arg(0) || !output)return finish(invalid_parameter);
            if(!symbol.starts_with("GdipGet"))memory.store(GuestAddress{output},pointer_bits,0);
            const auto path=read_utf16(memory,GuestAddress{arg(0)});if(path.empty())return finish(invalid_parameter);
            const auto opened=filesystem_.open_reader(utf16_to_utf8(normalized_windows_path(path,runtime_.current_directory)));
            if(opened.error)return finish(symbol.find("Metafile")!=symbol.npos?generic_error:opened.error==2 || opened.error==3?10:opened.error==5?12:7);
            const auto info=opened.file->information();if(info.error)return finish(7);if(info.metadata.size>stream_limit)return finish(out_of_memory);
            std::vector<std::uint8_t> bytes(info.metadata.size);const auto io=opened.file->read(bytes,0);
            if(io.error || io.bytes!=bytes.size())return finish(7);
            return finish(read_gdiplus_image(state,memory,bytes,output,symbol,0,false,symbol.ends_with("WmfFile")?arg(1):0));
        }
        if(symbol=="GdipLoadImageFromStream" || symbol=="GdipLoadImageFromStreamICM" || symbol=="GdipCreateBitmapFromStream" ||
           symbol=="GdipCreateBitmapFromStreamICM" || symbol=="GdipGetMetafileHeaderFromStream" || symbol=="GdipCreateMetafileFromStream"){
            if(!arg(0) || !arg(1))return finish(invalid_parameter);
            if(!symbol.starts_with("GdipGet"))memory.store(GuestAddress{arg(1)},pointer_bits,0);
            const auto table=memory.load(GuestAddress{arg(0)},pointer_bits);
            const auto seek=memory.load(GuestAddress{table+5*(pointer_bits/8U)},pointer_bits);
            if(!seek)return finish(generic_error);
            const auto buffer=allocate_heap(memory,stream_chunk+8);if(!buffer)return finish(out_of_memory);
            const auto id=g.next_load++;
            auto& load=g.loads.emplace(id,GdiplusState::StreamLoad{arg(0),arg(1),buffer,{}}).first->second;
            load.operation=symbol;load.seeking=true;
            // IStream::Seek 的 LARGE_INTEGER 在 x86 佔兩個堆疊欄位。
            GuestCallback rewind{seek,{arg(0),0,0,0}};
            rewind.wide_arguments=1U<<1;
            start_callbacks(state,memory,{std::move(rewind)},Continuation::GdiplusStream,id);return std::nullopt;
        }
        if(symbol=="GdipCreateBitmapFromScan0"){
            const auto width=s32(0),height=s32(1),stride=s32(2);const auto format=u32(3);const auto scan0=arg(4);
            if(!arg(5) || width<=0 || height<=0)return finish(invalid_parameter);
            if(!gdiplus_pixel::supported(format))return finish(invalid_parameter);const auto row_bytes=(static_cast<std::int64_t>(width)*gdiplus_pixel::bits(format)+7)/8;
            GdiplusState::Bitmap bitmap{static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),format,stride,scan0,false};bitmap.palette=gdiplus_pixel::default_palette(format);
            if(!scan0){
                // 自行配置時 stride 對齊 4 bytes 並清零，等同完全透明的黑色（索引色為第 0 色）。
                bitmap.stride=gdiplus_pixel::stride(format,static_cast<std::uint32_t>(width));
                const auto size=static_cast<std::uint64_t>(bitmap.stride)*static_cast<std::uint64_t>(height);
                bitmap.scan0=allocate_heap(memory,size);if(!bitmap.scan0)return finish(out_of_memory);
                memory.fill(GuestAddress{bitmap.scan0},0,static_cast<std::size_t>(size));bitmap.owned=true;
            }else if(std::abs(stride)<row_bytes)return finish(invalid_parameter);
            const auto handle=new_handle();if(!handle){if(bitmap.owned)(void)free_heap(memory,bitmap.scan0,state);return finish(out_of_memory);}
            g.bitmaps[handle]=bitmap;memory.store(GuestAddress{arg(5)},pointer_bits,handle);return finish(ok);
        }
        const auto bitmap_of=[&](std::uint64_t handle)->GdiplusState::Bitmap*{const auto found=g.bitmaps.find(handle);return found==g.bitmaps.end()?nullptr:&found->second;};
        const auto codec_of=[&](std::uint64_t pointer){gdiplus_codec::Guid id{};memory.read(GuestAddress{pointer},id);return gdiplus_codec::find(id);};
        if(symbol=="GdipGetEncoderParameterListSize" || symbol=="GdipGetEncoderParameterList"){
            if(!bitmap_of(arg(0)) || !arg(1))return finish(invalid_parameter);const auto* codec=codec_of(arg(1));if(!codec)return finish(unknown_image_format);
            const bool jpeg=codec->format==1,tiff=codec->format==10;const unsigned header=x86_?4:8,entry=x86_?28:32;
            const auto size=(jpeg || tiff)?header+entry+8:header;
            if(symbol=="GdipGetEncoderParameterListSize"){if(!arg(2))return finish(invalid_parameter);memory.store(GuestAddress{arg(2)},32,size);return finish(ok);}
            if(!arg(3))return finish(invalid_parameter);if(u32(2)<size)return finish(insufficient_buffer);
            const auto out=arg(3);memory.fill(GuestAddress{out},0,size);memory.store(GuestAddress{out},32,jpeg || tiff?1:0);
            if(jpeg || tiff){
                const auto parameter=out+header,values=parameter+entry;memory.write(GuestAddress{parameter},jpeg?gdiplus_codec::quality:gdiplus_codec::compression);
                memory.store(GuestAddress{parameter+16},32,jpeg?1:2);memory.store(GuestAddress{parameter+20},32,jpeg?6:4);
                memory.store(GuestAddress{parameter+24},pointer_bits,values);memory.store(GuestAddress{values},32,jpeg?0:2);memory.store(GuestAddress{values+4},32,jpeg?100:6);
            }
            return finish(ok);
        }
        // 像素以 0xAARRGGBB（非預乘）交換：row 為該列起點，編解碼與調色盤對應在 gdiplus_pixels.hpp；1／4 位元格式以位元組讀改寫。
        const auto row_of=[&](const GdiplusState::Bitmap& b,std::uint32_t y){return b.scan0+static_cast<std::uint64_t>(static_cast<std::int64_t>(y)*b.stride);};
        const auto address_of=[&](const GdiplusState::Bitmap& b,std::uint32_t x,std::uint32_t y){return row_of(b,y)+static_cast<std::uint64_t>(x)*gdiplus_pixel::bits(b.format)/8;};
        const auto pixel_window=[](std::uint32_t format,std::uint32_t x){const auto bits=gdiplus_pixel::bits(format);
            return bits<8?std::pair<std::uint64_t,std::uint32_t>{x/(8/bits),x%(8/bits)}:std::pair<std::uint64_t,std::uint32_t>{static_cast<std::uint64_t>(x)*(bits/8),0};};
        const auto load_pixel=[&](std::uint32_t format,std::uint64_t row,std::uint32_t x,std::span<const std::uint32_t> palette)->std::uint32_t{
            const auto [offset,index]=pixel_window(format,x);std::array<std::uint8_t,8> buffer{};const auto size=std::max(1U,gdiplus_pixel::bits(format)/8);
            memory.read(GuestAddress{row+offset},std::span<std::uint8_t>(buffer.data(),size));return gdiplus_pixel::decode(format,buffer,index,palette);
        };
        const auto store_pixel=[&](std::uint32_t format,std::uint64_t row,std::uint32_t x,std::uint32_t color,std::span<const std::uint32_t> palette){
            const auto [offset,index]=pixel_window(format,x);std::array<std::uint8_t,8> buffer{};const auto size=std::max(1U,gdiplus_pixel::bits(format)/8);
            if(gdiplus_pixel::bits(format)<8)memory.read(GuestAddress{row+offset},std::span<std::uint8_t>(buffer.data(),1));
            gdiplus_pixel::encode(format,buffer,index,color,palette);memory.write(GuestAddress{row+offset},std::span<const std::uint8_t>(buffer.data(),size));
        };
        const auto read_pixel=[&](const GdiplusState::Bitmap& b,std::uint32_t x,std::uint32_t y)->std::uint32_t{return load_pixel(b.format,row_of(b,y),x,b.palette);};
        const auto write_pixel=[&](const GdiplusState::Bitmap& b,std::uint32_t x,std::uint32_t y,std::uint32_t color){store_pixel(b.format,row_of(b,y),x,color,b.palette);};
        // 影像 Graphics 的 DIB 與點陣圖保持同一份像素：copy_back 把 DIB（預乘）寫回點陣圖格式，mirror 把點陣圖矩形寫入 DIB。
        const auto copy_back=[&](GdiplusState::ImageTarget& target){
            auto* b=bitmap_of(target.bitmap);if(!b)return;(void)flush_gdi_batch(memory);sync_raster_bitmap(memory,target.dib);
            for(std::uint32_t y=0;y<b->height;++y)for(std::uint32_t x=0;x<b->width;++x){
                const auto raw=static_cast<std::uint32_t>(memory.load(GuestAddress{target.bits+(static_cast<std::uint64_t>(y)*b->width+x)*4},32));
                write_pixel(*b,x,y,gdiplus_pixel::alpha(b->format)?unpremultiply(raw):raw|0xff000000U);}
            target.dirty=false;
        };
        const auto mirror=[&](std::uint64_t bitmap,const std::array<std::int32_t,4>& rect){
            const auto* b=bitmap_of(bitmap);if(!b)return;
            for(const auto& [handle,target]:g.image_graphics){(void)handle;if(target.bitmap!=bitmap)continue;
                for(auto y=rect[1];y<rect[1]+rect[3];++y)for(auto x=rect[0];x<rect[0]+rect[2];++x)
                    memory.store(GuestAddress{target.bits+(static_cast<std::uint64_t>(y)*b->width+static_cast<std::uint64_t>(x))*4},32,premultiply(read_pixel(*b,static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y))));}
        };
        {
            // 讀取點陣圖像素的呼叫：來源點陣圖的參數位置依 API 而定。
            const auto source=symbol.starts_with("GdipCloneBitmapArea")?arg(5):symbol.starts_with("GdipDrawImage")?arg(1):arg(0);
            if(g.bitmaps.contains(source) && (symbol.starts_with("GdipBitmap") || symbol.starts_with("GdipCloneImage") || symbol.starts_with("GdipCloneBitmapArea") ||
               symbol=="GdipCreateHBITMAPFromBitmap" || symbol=="GdipCreateHICONFromBitmap" || symbol.starts_with("GdipSaveImage") || symbol.starts_with("GdipCreateTexture") ||
               symbol=="GdipGetImageGraphicsContext" || symbol.starts_with("GdipImage") || symbol=="GdipGetImageThumbnail" || symbol.starts_with("GdipDrawImage") || symbol=="GdipCreateCachedBitmap"))
                for(auto& [handle,target]:g.image_graphics){(void)handle;if(target.bitmap==source && (target.dirty || target.held))copy_back(target);}
        }
#include "gdiplus_brush_handlers.inc"
#include "gdiplus_image_handlers.inc"
#include "gdiplus_draw_image_handlers.inc"
#include "gdiplus_effect_handlers.inc"
#include "gdiplus_text_handlers.inc"
        if(symbol=="GdipCreateHBITMAPFromBitmap"){
            const auto* b=bitmap_of(arg(0));if(!b || !arg(1))return finish(invalid_parameter);if(b->lock)return finish(object_busy);
            memory.check(GuestAddress{arg(1)},pointer_bits/8,Access::Write);memory.store(GuestAddress{arg(1)},pointer_bits,0);
            const auto header=allocate_heap(memory,56);if(!header)return finish(out_of_memory);
            memory.fill(GuestAddress{header},0,56);memory.store(GuestAddress{header},32,40);memory.store(GuestAddress{header+4},32,b->width);
            memory.store(GuestAddress{header+8},32,static_cast<std::uint32_t>(-static_cast<std::int32_t>(b->height)));
            memory.store(GuestAddress{header+12},16,1);memory.store(GuestAddress{header+14},16,32);
            const auto dib=nested_service(state,memory,"gdi32.dll","CreateDIBSection",{0,header,0,header+48,0,0});
            const auto bits=memory.load(GuestAddress{header+48},pointer_bits);release_handle(header);
            if(!dib || !bits)return finish(out_of_memory);
            try {
                const auto background=u32(2);
                for(std::uint32_t y=0;y<b->height;++y)for(std::uint32_t x=0;x<b->width;++x){
                    const auto color=read_pixel(*b,x,y),alpha=color>>24;std::uint32_t pixel=0xff000000U;
                    for(unsigned shift:{0U,8U,16U})pixel|=(((((color>>shift)&255U)*alpha+((background>>shift)&255U)*(255U-alpha)+127U)/255U)<<shift);
                    memory.store(GuestAddress{bits+(static_cast<std::uint64_t>(y)*b->width+x)*4},32,pixel);
                }
                sync_raster_bitmap(memory,dib);
                memory.store(GuestAddress{arg(1)},pointer_bits,dib);return finish(ok);
            }catch(...){(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{dib});throw;}
        }
        if(symbol=="GdipSaveImageToFile" || symbol=="GdipSaveImageToStream"){
            const auto* b=bitmap_of(arg(0));if(!b || !arg(1) || !arg(2))return finish(invalid_parameter);if(b->lock)return finish(object_busy);
            const auto* codec=codec_of(arg(2));if(!codec)return finish(unknown_image_format);
            texture_image::EncodeOptions options;
            if(codec->format==1)options.quality=.75;
            if(arg(3)){
                const auto count=memory.load(GuestAddress{arg(3)},32);if(count>64)return finish(invalid_parameter);
                bool quality_seen=false,compression_seen=false;
                for(unsigned i=0;i<count;++i){
                    const auto at=arg(3)+(x86_?4U:8U)+i*(x86_?28U:32U);gdiplus_codec::Guid id{};memory.read(GuestAddress{at},id);
                    const auto number=memory.load(GuestAddress{at+16},32),type=memory.load(GuestAddress{at+20},32),value=memory.load(GuestAddress{at+24},pointer_bits);
                    if(number!=1 || type!=4 || !value)return finish(invalid_parameter);const auto scalar=memory.load(GuestAddress{value},32);
                    if(id==gdiplus_codec::quality && codec->format==1){if(quality_seen || scalar>100)return finish(invalid_parameter);quality_seen=true;options.quality=static_cast<double>(scalar)/100;}
                    else if(id==gdiplus_codec::compression && codec->format==10){if(compression_seen || (scalar!=2 && scalar!=6))return finish(invalid_parameter);compression_seen=true;options.tiff_compression=scalar==2?5:1;}
                    else return finish(not_implemented);
                }
            }
            const auto count=std::uint64_t{b->width}*b->height;if(count>stream_limit/4)return finish(out_of_memory);
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(count)*4);
            for(unsigned y=0;y<b->height;++y)for(unsigned x=0;x<b->width;++x){const auto color=read_pixel(*b,x,y);const auto at=(std::size_t{y}*b->width+x)*4;for(unsigned n=0;n<4;++n)pixels[at+n]=static_cast<std::uint8_t>(color>>(n*8));}
            GraphicsResourceDesc description;description.dimension=3;description.width=b->width;description.height=b->height;description.format=87;
            auto bytes=texture_image::encode(description,codec->format,[&](unsigned){return std::move(pixels);},options);
            if(symbol=="GdipSaveImageToFile"){
                const auto path=read_utf16(memory,GuestAddress{arg(1)});if(path.empty())return finish(invalid_parameter);
                const auto opened=filesystem_.open_host(utf16_to_utf8(normalized_windows_path(path,runtime_.current_directory)),0x40000000U,2,0x80,0);
                if(opened.error)return finish(opened.error==5?12:7);const auto written=opened.file->write(bytes,0);
                return finish(written.error || written.bytes!=bytes.size()?7:ok);
            }
            const auto table=memory.load(GuestAddress{arg(1)},pointer_bits),write=memory.load(GuestAddress{table+(x86_?16U:32U)},pointer_bits);
            if(!write)return finish(invalid_parameter);
            const auto buffer=allocate_heap(memory,stream_chunk+8);if(!buffer)return finish(out_of_memory);
            const auto id=g.next_load++;auto& operation=g.loads.emplace(id,GdiplusState::StreamLoad{arg(1),0,buffer,std::move(bytes),true}).first->second;
            operation.sent=static_cast<std::uint32_t>(std::min<std::size_t>(stream_chunk,operation.bytes.size()));
            memory.write(GuestAddress{buffer},std::span(operation.bytes).first(operation.sent));memory.store(GuestAddress{buffer+stream_chunk},32,0);
            start_callbacks(state,memory,{{write,{operation.stream,buffer,operation.sent,buffer+stream_chunk}}},Continuation::GdiplusStream,id);return std::nullopt;
        }
        if(symbol=="GdipBitmapLockBits"){
            auto* b=bitmap_of(arg(0));const auto data=arg(4);if(!b || !data)return finish(invalid_parameter);if(b->lock)return finish(wrong_state);
            std::array<std::int32_t,4> rect{0,0,static_cast<std::int32_t>(b->width),static_cast<std::int32_t>(b->height)};
            if(arg(1))for(unsigned i=0;i<4;++i)rect[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+i*4},32));
            const auto mode=u32(2),format=u32(3);
            if(rect[0]<0 || rect[1]<0 || rect[2]<=0 || rect[3]<=0 || static_cast<std::int64_t>(rect[0])+rect[2]>b->width || static_cast<std::int64_t>(rect[1])+rect[3]>b->height || !(mode&3U) || (mode&~7U))return finish(invalid_parameter);
            // 轉成索引色只允許與點陣圖同格式（共用其調色盤）。
            if(!gdiplus_pixel::supported(format) || (gdiplus_pixel::indexed(format) && format!=b->format))return finish(invalid_parameter);
            const auto row_bytes=(static_cast<std::int64_t>(rect[2])*gdiplus_pixel::bits(format)+7)/8;
            const unsigned data_size=x86_?24U:32U;memory.check(GuestAddress{data},data_size,Access::Write);
            GdiplusState::Lock lock{0,mode,format,rect,0,false};
            if(mode&4U){lock.stride=static_cast<std::int32_t>(memory.load(GuestAddress{data+8},32));lock.buffer=memory.load(GuestAddress{data+16},pointer_bits);
                if(!lock.buffer || std::abs(lock.stride)<row_bytes)return finish(invalid_parameter);}
            const bool aligned=(static_cast<std::int64_t>(rect[0])*gdiplus_pixel::bits(format))%8==0;
            if(mode&4U){}
            else if(format==b->format && aligned){lock.buffer=address_of(*b,static_cast<std::uint32_t>(rect[0]),static_cast<std::uint32_t>(rect[1]));lock.stride=b->stride;}
            else {lock.stride=gdiplus_pixel::stride(format,static_cast<std::uint32_t>(rect[2]));lock.buffer=allocate_heap(memory,static_cast<std::uint64_t>(lock.stride)*static_cast<std::uint32_t>(rect[3]));if(!lock.buffer)return finish(out_of_memory);lock.owned=true;}
            const bool direct=!(mode&4U) && format==b->format && aligned;
            if((mode&1U) && !direct)
                for(std::int32_t y=0;y<rect[3];++y)for(std::int32_t x=0;x<rect[2];++x)
                    store_pixel(format,lock.buffer+static_cast<std::uint64_t>(y*lock.stride),static_cast<std::uint32_t>(x),read_pixel(*b,static_cast<std::uint32_t>(rect[0]+x),static_cast<std::uint32_t>(rect[1]+y)),b->palette);
            memory.store(GuestAddress{data},32,static_cast<std::uint32_t>(rect[2]));memory.store(GuestAddress{data+4},32,static_cast<std::uint32_t>(rect[3]));
            memory.store(GuestAddress{data+8},32,static_cast<std::uint32_t>(lock.stride));memory.store(GuestAddress{data+12},32,format);
            memory.store(GuestAddress{data+16},pointer_bits,lock.buffer);memory.store(GuestAddress{data+(x86_?20U:24U)},pointer_bits,0);
            b->lock=lock;return finish(ok);
        }
        if(symbol=="GdipBitmapUnlockBits"){
            auto* b=bitmap_of(arg(0));if(!b || !arg(1))return finish(invalid_parameter);if(!b->lock)return finish(wrong_state);
            const auto lock=*b->lock;b->lock.reset();const bool direct=!lock.owned && !(lock.mode&4U);
            if((lock.mode&2U) && !direct)
                for(std::int32_t y=0;y<lock.rect[3];++y)for(std::int32_t x=0;x<lock.rect[2];++x)
                    write_pixel(*b,static_cast<std::uint32_t>(lock.rect[0]+x),static_cast<std::uint32_t>(lock.rect[1]+y),load_pixel(lock.format,lock.buffer+static_cast<std::uint64_t>(y*lock.stride),static_cast<std::uint32_t>(x),b->palette));
            if(lock.mode&2U)mirror(arg(0),{lock.rect[0],lock.rect[1],lock.rect[2],lock.rect[3]});
            if(lock.owned)(void)free_heap(memory,lock.buffer,state);return finish(ok);
        }
        if(symbol=="GdipGetImagePaletteSize" || symbol=="GdipGetImagePalette"){
            // ColorPalette：Flags、Count、Entries；沒有調色盤時大小為 12（Count 0）。
            const auto* b=bitmap_of(arg(0));if(!b || !arg(1))return finish(invalid_parameter);
            const auto size=static_cast<std::uint32_t>(8+4*std::max<std::size_t>(1,b->palette.size()));
            if(symbol=="GdipGetImagePaletteSize"){memory.store(GuestAddress{arg(1)},32,size);return finish(ok);}
            if(s32(2)<static_cast<std::int32_t>(b->palette.empty()?8:size))return finish(insufficient_buffer);
            memory.store(GuestAddress{arg(1)},32,b->palette_flags);memory.store(GuestAddress{arg(1)+4},32,b->palette.size());
            for(std::size_t i=0;i<b->palette.size();++i)memory.store(GuestAddress{arg(1)+8+i*4},32,b->palette[i]);return finish(ok);
        }
        if(symbol=="GdipGetImageGraphicsContext"){
            auto* b=bitmap_of(arg(0));if(!b || !arg(1))return finish(invalid_parameter);if(b->lock)return finish(object_busy);
            if(gdiplus_pixel::indexed(b->format) || b->format==gdiplus_pixel::gray16)return finish(out_of_memory);
            const auto dc=nested_service(state,memory,"gdi32.dll","CreateCompatibleDC",{0});if(!dc)return finish(out_of_memory);
            const auto header=allocate_heap(memory,48+8);if(!header){(void)nested_service(state,memory,"gdi32.dll","DeleteDC",{dc});return finish(out_of_memory);}
            memory.fill(GuestAddress{header},0,56);memory.store(GuestAddress{header},32,40);memory.store(GuestAddress{header+4},32,b->width);memory.store(GuestAddress{header+8},32,static_cast<std::uint32_t>(-static_cast<std::int32_t>(b->height)));
            memory.store(GuestAddress{header+12},16,1);memory.store(GuestAddress{header+14},16,32);
            const auto dib=nested_service(state,memory,"gdi32.dll","CreateDIBSection",{dc,header,0,header+48,0,0});const auto bits=memory.load(GuestAddress{header+48},pointer_bits);(void)free_heap(memory,header,state);
            if(!dib || !bits){(void)nested_service(state,memory,"gdi32.dll","DeleteDC",{dc});return finish(out_of_memory);}
            for(std::uint32_t y=0;y<b->height;++y)for(std::uint32_t x=0;x<b->width;++x)memory.store(GuestAddress{bits+(static_cast<std::uint64_t>(y)*b->width+x)*4},32,premultiply(read_pixel(*b,x,y)));
            const auto previous=nested_service(state,memory,"gdi32.dll","SelectObject",{dc,dib});
            const auto handle=new_handle();if(!handle){(void)nested_service(state,memory,"gdi32.dll","SelectObject",{dc,previous});(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{dib});(void)nested_service(state,memory,"gdi32.dll","DeleteDC",{dc});return finish(out_of_memory);}
            g.graphics[handle]=dc;g.image_graphics[handle]={arg(0),dc,dib,previous,bits};memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);
        }
        if(symbol=="GdipBitmapSetPixel" || symbol=="GdipBitmapGetPixel"){
            auto* b=bitmap_of(arg(0));if(!b)return finish(invalid_parameter);
            const auto x=s32(1),y=s32(2);
            if(x<0 || y<0 || static_cast<std::uint32_t>(x)>=b->width || static_cast<std::uint32_t>(y)>=b->height)return finish(invalid_parameter);
            if(symbol=="GdipBitmapGetPixel"){if(!arg(3))return finish(invalid_parameter);memory.store(GuestAddress{arg(3)},32,read_pixel(*b,static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y)));return finish(ok);}
            // 索引色點陣圖不能逐點設定顏色（與 Windows 相同）。
            if(gdiplus_pixel::indexed(b->format))return finish(invalid_parameter);
            write_pixel(*b,static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y),u32(3));mirror(arg(0),{x,y,1,1});return finish(ok);
        }
        if(symbol=="GdipGetImageWidth" || symbol=="GdipGetImageHeight" || symbol=="GdipGetImagePixelFormat"){
            const auto* b=bitmap_of(arg(0));if(!b || !arg(1))return finish(invalid_parameter);
            memory.store(GuestAddress{arg(1)},32,symbol=="GdipGetImageWidth"?b->width:symbol=="GdipGetImageHeight"?b->height:b->format);return finish(ok);
        }
        if(symbol=="GdipCloneImage"){
            const auto* b=bitmap_of(arg(0));if(!b || !arg(1))return finish(invalid_parameter);
            GdiplusState::Bitmap copy=*b;copy.lock.reset();copy.stride=gdiplus_pixel::stride(b->format,b->width);
            const auto size=static_cast<std::uint64_t>(copy.stride)*b->height;
            copy.scan0=allocate_heap(memory,size);if(!copy.scan0)return finish(out_of_memory);copy.owned=true;
            std::vector<std::uint8_t> row(static_cast<std::size_t>((static_cast<std::uint64_t>(b->width)*gdiplus_pixel::bits(b->format)+7)/8));
            for(std::uint32_t y=0;y<b->height;++y){memory.read(GuestAddress{address_of(*b,0,y)},row);memory.write(GuestAddress{copy.scan0+static_cast<std::uint64_t>(copy.stride)*y},row);}
            const auto handle=new_handle();if(!handle){(void)free_heap(memory,copy.scan0,state);return finish(out_of_memory);}
            g.bitmaps[handle]=copy;memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);
        }
        if(symbol=="GdipDisposeImage"){
            const auto found=g.bitmaps.find(arg(0));if(found==g.bitmaps.end())return finish(invalid_parameter);
            if(found->second.owned)(void)free_heap(memory,found->second.scan0,state);
            g.bitmaps.erase(found);release_handle(arg(0));return finish(ok);
        }
        if(symbol=="GdipCreateFromHDC"){
            if(!arg(1) || !gui_.contexts.contains(arg(0)))return finish(invalid_parameter);
            const auto handle=new_handle();if(!handle)return finish(out_of_memory);
            g.graphics[handle]=arg(0);memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);
        }
        if(symbol=="GdipDeleteGraphics"){
            if(const auto target=g.image_graphics.find(arg(0));target!=g.image_graphics.end()){
                // 先讓 GPU raster 寫回 DIB backing，再把預乘像素轉回點陣圖本身的格式。
                auto image=target->second;g.image_graphics.erase(target);copy_back(image);
                (void)nested_service(state,memory,"gdi32.dll","SelectObject",{image.dc,image.previous});(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{image.dib});(void)nested_service(state,memory,"gdi32.dll","DeleteDC",{image.dc});
            }
            if(!g.graphics.contains(arg(0)))return finish(invalid_parameter);
            if(const auto info=g.graphics_state.find(arg(0));info!=g.graphics_state.end() && info->second.owned_dc)
                (void)nested_service(state,memory,"user32.dll","ReleaseDC",{info->second.window,g.graphics.at(arg(0))});
            g.graphics.erase(arg(0));g.graphics_state.erase(arg(0));g.saved_graphics.erase(arg(0));
            g.smoothing.erase(arg(0));
            release_handle(arg(0));return finish(ok);
        }
    }catch(const MemoryFault&){return finish(invalid_parameter);}catch(const std::bad_alloc&){return finish(out_of_memory);}
    catch(const texture_image::Error&){return finish(generic_error);}catch(const std::invalid_argument&){return finish(invalid_parameter);}catch(const std::length_error&){return finish(out_of_memory);}
    return finish(not_implemented);
}
std::optional<std::uint32_t> WindowsServices::resume_gdiplus_stream(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    if(!gdiplus_){complete_call(state,memory,generic_error);return std::nullopt;}
    if(gdiplus_.use_count()>1)gdiplus_=std::make_shared<GdiplusState>(*gdiplus_);
    auto& g=*gdiplus_;const auto found=g.loads.find(id);if(found==g.loads.end()){complete_call(state,memory,generic_error);return std::nullopt;}
    auto& load=found->second;const auto pointer_bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto finish=[&](std::uint32_t status){(void)free_heap(memory,load.buffer,state);g.loads.erase(found);complete_call(state,memory,status);return std::optional<std::uint32_t>{};};
    try{
        const auto status=static_cast<std::uint32_t>(value);if(status&0x80000000U)return finish(generic_error);
        const auto count=load.seeking?0U:static_cast<std::uint32_t>(memory.load(GuestAddress{load.buffer+stream_chunk},32));
        if(load.saving){
            if(count!=load.sent)return finish(generic_error);load.offset+=count;if(load.offset==load.bytes.size())return finish(ok);
            load.sent=static_cast<std::uint32_t>(std::min<std::size_t>(stream_chunk,load.bytes.size()-load.offset));
            memory.write(GuestAddress{load.buffer},std::span(load.bytes).subspan(load.offset,load.sent));memory.store(GuestAddress{load.buffer+stream_chunk},32,0);
            const auto table=memory.load(GuestAddress{load.stream},pointer_bits),write=memory.load(GuestAddress{table+(x86_?16U:32U)},pointer_bits);
            start_callbacks(state,memory,{{write,{load.stream,load.buffer,load.sent,load.buffer+stream_chunk}}},Continuation::GdiplusStream,id);return std::nullopt;
        }
        if(count>stream_chunk || load.bytes.size()+count>stream_limit)return finish(count>stream_chunk?generic_error:out_of_memory);
        if(count){const auto at=load.bytes.size();load.bytes.resize(at+count);memory.read(GuestAddress{load.buffer},std::span(load.bytes).subspan(at));}
        if(load.seeking || (count && status==0)){
            load.seeking=false;
            const auto table=memory.load(GuestAddress{load.stream},pointer_bits),read=memory.load(GuestAddress{table+(x86_?12U:24U)},pointer_bits);
            memory.store(GuestAddress{load.buffer+stream_chunk},32,0);
            start_callbacks(state,memory,{{read,{load.stream,load.buffer,stream_chunk,load.buffer+stream_chunk}}},Continuation::GdiplusStream,id);return std::nullopt;
        }
        return finish(read_gdiplus_image(state,memory,load.bytes,load.output,load.operation));
    }catch(const MemoryFault&){return finish(invalid_parameter);}catch(const std::bad_alloc&){return finish(out_of_memory);}
}
void WindowsServices::discard_gdiplus_stream(GuestState& state,GuestMemory& memory,std::uint64_t id){
    if(!gdiplus_)return;const auto found=gdiplus_->loads.find(id);if(found==gdiplus_->loads.end())return;
    (void)free_heap(memory,found->second.buffer,state);gdiplus_->loads.erase(found);
}
}
