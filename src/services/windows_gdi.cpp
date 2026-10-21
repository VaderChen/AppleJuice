#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/gpu_memory.hpp"
#include "x64arm/windows_dib_rows.hpp"
#include "x64arm/windows_transform.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_userdraw.hpp"
#include "x64arm/windows_palette.hpp"
#include "x64arm/windows_fonts.hpp"
#include <unicode/utf8.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
namespace x64arm {
namespace {
void logical_clip(GuiDraw& draw,const GuiDc& dc,const GuiRect& rect){
    auto region=gui_transform_region_rect(gui_device_transform(dc),rect);
    if(dc.clipped)region=gui_region_combine(region,GuiRegion{dc.clip},1);
    if(!dc.excluded.empty())region=gui_region_combine(region,dc.excluded,4);
    draw.clipped=true;draw.clip=gui_region_bounds(region);draw.excluded=gui_region_combine(GuiRegion{draw.clip},region,4);
}
}
GuiFont gui_font(const GuiState& gui, std::uint64_t handle) {
    GuiFont font;
    const auto found = gui.fonts.find(handle);
    if (found == gui.fonts.end()) {
        if (handle == 0xb00a || handle == 0xb00b || handle == 0xb010) font.pitch_family = 1;
        return font;
    }
    const auto& bytes = found->second;
    if (bytes.size() != 60 && bytes.size() != 92) return font;
    const auto integer = [&](unsigned offset) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t{bytes[offset + i]} << (8 * i);
        return std::bit_cast<std::int32_t>(value);
    };
    font.height = integer(0); font.width = integer(4); font.weight = integer(16);
    font.italic = bytes[20] != 0; font.underline = bytes[21] != 0; font.strikeout = bytes[22] != 0;
    font.charset = bytes[23]; font.pitch_family = bytes[27];
    if (bytes.size() == 92) {
        for (unsigned i = 28; i < 92; i += 2) {
            const auto value = static_cast<char16_t>(bytes[i] | (std::uint16_t{bytes[i + 1]} << 8));
            if (!value) break;
            font.face.push_back(value);
        }
    } else {
        std::string face;
        for (unsigned i = 28; i < 60 && bytes[i]; ++i) face.push_back(static_cast<char>(bytes[i]));
        // 本虛擬 Windows 的 GetACP 固定為 UTF-8，A/W 字型名稱與其餘字串 API 使用相同約定。
        font.face = utf8_to_utf16_lossy(face);
    }
    if(const auto native=gui.font_natives.find(handle);native!=gui.font_natives.end())font.native=native->second;
    else{
        const auto same=[](std::u16string_view a,std::u16string_view b){return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](char16_t x,char16_t y){if(x>=u'A' && x<=u'Z')x=static_cast<char16_t>(x+32);if(y>=u'A' && y<=u'Z')y=static_cast<char16_t>(y+32);return x==y;});};
        std::int64_t best=INT64_MAX;
        for(auto resource=gui.memory_fonts.rbegin();resource!=gui.memory_fonts.rend();++resource)for(const auto& face:resource->second){
            if(!same(font.face,face.family) && !same(font.face,face.full_name) && !same(font.face,face.postscript_name))continue;
            const auto distance=std::abs(static_cast<std::int64_t>(font.weight)-face.weight)+(font.italic==face.italic?0:1000);if(distance<best){best=distance;font.native=face.native;}
        }
    }
    return font;
}
GuiFontMetrics fallback_font_metrics(const GuiFont& font) {
    // Headless 後端使用一致的比例模型；原生後端回報實際選用字型的量測結果。
    const auto requested = std::clamp<std::int64_t>(font.height ? std::abs(static_cast<std::int64_t>(font.height)) : 13, 1, 512);
    GuiFontMetrics metrics;
    metrics.height = static_cast<std::int32_t>(font.height > 0 ? requested : (requested * 16 + 12) / 13);
    metrics.ascent = std::max(1, metrics.height * 13 / 16); metrics.descent = metrics.height - metrics.ascent;
    metrics.average_width = font.width ? static_cast<std::int32_t>(std::clamp<std::int64_t>(std::abs(static_cast<std::int64_t>(font.width)), 1, 512))
        : std::max(1, metrics.height * 7 / 16);
    metrics.fixed_pitch = (font.pitch_family & 3) == 1 || (font.pitch_family & 0xf0) == 0x30;
    metrics.maximum_width = metrics.fixed_pitch ? metrics.average_width : metrics.average_width + 1;
    return metrics;
}
std::uint64_t WindowsServices::create_dib_bitmap(GuestMemory& memory,const GuestState& state,std::uint32_t width,std::int32_t height,std::uint16_t bpp,std::uint64_t section,std::uint32_t offset){
    const auto rows=static_cast<std::uint32_t>(height<0?-height:height),stride=((width*bpp+31)/32)*4;
    std::uint64_t view=0;const auto size=static_cast<std::uint64_t>(stride)*rows;
    const auto address=section?map_dib_section(memory,section,offset,size,view):allocate_heap(memory,size);if(!address){if(!section)last_error_=8;return 0;}
    try {
        GuiBitmap bitmap{width,rows,std::vector<std::uint32_t>(static_cast<std::size_t>(width)*rows,bpp==24?0xff000000U:0U)};
        if(bpp==16)bitmap.dib_masks={0x7c00,0x3e0,0x1f,0};else if(bpp==24)bitmap.dib_masks[3]=0;
        bitmap.dib_view=view;bitmap.dib_section=section;bitmap.dib_offset=section?offset:0;
        bitmap.stride=stride;bitmap.bits_per_pixel=bpp;bitmap.signed_height=height;bitmap.dib_address=address;
        bitmap.dib_palette=gui_dib_palette(bpp);
        const auto handle=gui_.next_object;gui_.bitmaps.emplace(handle,std::move(bitmap));++gui_.next_object;return handle;
    }catch(...){if(view)(void)unmap_file_view(memory,view,true);else (void)free_heap(memory,address,state);throw;}
}
bool WindowsServices::import_dib_bitmap(GuestMemory& memory,std::uint64_t handle){
    const auto found=gui_.bitmaps.find(handle);if(found==gui_.bitmaps.end() || !found->second.dib_address)return false;
    auto& bitmap=found->second;dib_scratch_.resize(static_cast<std::size_t>(bitmap.stride)*bitmap.height);
    memory.read(GuestAddress{bitmap.dib_address},dib_scratch_);bool changed=false;
    const DibRowCodec codec(bitmap.bits_per_pixel,bitmap.dib_masks,bitmap.dib_palette);
    for(std::uint32_t y=0;y<bitmap.height;++y){const auto row=bitmap.signed_height<0?y:bitmap.height-1-y;
        if(!codec.decode(std::span(dib_scratch_).subspan(static_cast<std::size_t>(row)*bitmap.stride,bitmap.stride),std::span(bitmap.pixels).subspan(static_cast<std::size_t>(y)*bitmap.width,bitmap.width),&changed))throw HostCallFault("DIB 掃描列長度不符");
    }
    bitmap.dib_upload_pending|=changed;return changed;
}
void WindowsServices::export_dib_bitmap(GuestMemory& memory,std::uint64_t handle){
    auto& bitmap=gui_.bitmaps.at(handle);if(!bitmap.dib_address)return;
    // padding 不是像素，不應因 GDI 畫圖而破壞程式放在其中的值。
    dib_scratch_.resize(static_cast<std::size_t>(bitmap.stride)*bitmap.height);memory.read(GuestAddress{bitmap.dib_address},dib_scratch_);
    const DibRowCodec codec(bitmap.bits_per_pixel,bitmap.dib_masks,bitmap.dib_palette);
    for(std::uint32_t y=0;y<bitmap.height;++y){const auto row=bitmap.signed_height<0?y:bitmap.height-1-y;
        if(!codec.encode(std::span(bitmap.pixels).subspan(static_cast<std::size_t>(y)*bitmap.width,bitmap.width),std::span(dib_scratch_).subspan(static_cast<std::size_t>(row)*bitmap.stride,bitmap.stride),true))throw HostCallFault("DIB 掃描列長度不符");
    }
    memory.write(GuestAddress{bitmap.dib_address},dib_scratch_);bitmap.dib_upload_pending=false;
    if(bitmap.bits_per_pixel<=8)for(std::uint32_t y=0;y<bitmap.height;++y){const auto row=bitmap.signed_height<0?y:bitmap.height-1-y;
        if(!codec.decode(std::span(dib_scratch_).subspan(static_cast<std::size_t>(row)*bitmap.stride,bitmap.stride),std::span(bitmap.pixels).subspan(static_cast<std::size_t>(y)*bitmap.width,bitmap.width),&bitmap.dib_upload_pending))throw HostCallFault("DIB 色表索引不合法");}
}
std::shared_ptr<RasterSurface> WindowsServices::raster_surface(GuestMemory& memory,std::uint64_t hdc,bool writing){
    const auto dc=gui_.contexts.find(hdc);if(dc==gui_.contexts.end())return {};
    if(writing && !printer_drawing_enabled(hdc) && !prepare_legacy_printer_page(memory,hdc))return {};
    import_dib_bitmap(memory,dc->second.bitmap);
    // 控制項也可持有離屏繪製表面；headless 不應忽略已建立的真實像素。
    if(!native_gui_){const auto found=rasters_.find(dc->second.bitmap?dc->second.bitmap:hdc);return found==rasters_.end()?std::shared_ptr<RasterSurface>{}:found->second;}
    const auto window=hdc>=0x10000000?gui_.windows.find(hdc-0x10000000):gui_.windows.end();
    std::uint64_t key=hdc,resource=0;std::uint32_t width=0,height=0;std::span<const std::uint32_t> pixels;void* view=nullptr;
    if(window!=gui_.windows.end()){width=window->second.width;height=window->second.height;view=native_gui_->graphics_view(window->first);}
    else {const auto bitmap=gui_.bitmaps.find(dc->second.bitmap);if(bitmap==gui_.bitmaps.end())return {};key=dc->second.bitmap;width=bitmap->second.width;height=bitmap->second.height;pixels=bitmap->second.pixels;resource=bitmap->second.resource_id;}
    if(!width || !height)return {};
    const auto found=rasters_.find(key);
    auto surface=found==rasters_.end()?std::shared_ptr<RasterSurface>{}:found->second;
    bool shared=raster_shared_.contains(key);
    if(!surface && resource){
        auto cached=raster_templates_.find(resource);
        if(cached==raster_templates_.end()){
            std::uint64_t bytes=0;for(const auto& [id,item]:raster_templates_){(void)id;bytes+=std::uint64_t{item->width()}*item->height()*4;}
            if(raster_templates_.size()>=32 || bytes+std::uint64_t{width}*height*4>gpu_memory_stats().limit/4)raster_templates_.clear();
            cached=raster_templates_.emplace(resource,make_hardware_raster(width,height,pixels)).first;
        }
        surface=cached->second;shared=true;
    }
    const bool created=!surface || surface->width()!=width || surface->height()!=height;
    if(created){surface=make_hardware_raster(width,height,pixels,view);if(window!=gui_.windows.end())native_gui_->graphics_surface(window->first,surface);}
    if(writing && shared){
        auto independent=make_hardware_raster(width,height);GuiDc copy_dc;const std::array<std::int32_t,4> rect{0,0,static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)};independent->blit(*surface,rect,rect,copy_dc,UINT32_MAX);surface=std::move(independent);shared=false;
    }
    if(window==gui_.windows.end()){auto& bitmap=gui_.bitmaps.at(key);if((bitmap.dib_address && (created || bitmap.dib_upload_pending)) || (bitmap.raw_alpha && created)){surface->set_dib_alpha(bitmap.dib_address && bitmap.bits_per_pixel==32);surface->upload(bitmap.pixels,true);bitmap.dib_upload_pending=false;}}
    if(shared)raster_shared_.insert(key);else raster_shared_.erase(key);
    rasters_[key]=surface;return surface;
}
void WindowsServices::sync_raster_bitmap(GuestMemory& memory,std::uint64_t handle){
    import_dib_bitmap(memory,handle);
    const auto surface=rasters_.find(handle),unused=rasters_.end();
    if(surface!=unused && !raster_shared_.contains(handle) && gui_.bitmaps.contains(handle)){
        auto& bitmap=gui_.bitmaps.at(handle);if(bitmap.dib_address && bitmap.dib_upload_pending){surface->second->upload(bitmap.pixels,true);bitmap.dib_upload_pending=false;}
        bitmap.pixels=surface->second->snapshot().pixels;bitmap.resource_id=0;export_dib_bitmap(memory,handle);
    }
}
bool WindowsServices::raster_draw(GuestMemory& memory,std::uint64_t dc,GuiDraw& draw){
    gui_resolve_draw_colors(gui_,gui_.contexts.at(dc),draw);
    if(!draw.device_coordinates)draw.transform=gui_device_transform(gui_.contexts.at(dc));
    accumulate_dc_bounds(gui_.contexts.at(dc),draw);
    if(record_metafile_draw(dc,draw))return true;
    const auto surface=raster_surface(memory,dc);if(!surface)return false;
    std::shared_ptr<RasterSurface> pattern;
    if(draw.pattern_brush){const auto& bitmap=gui_.brush_patterns.at(draw.pattern_brush);const auto cached=rasters_.find(draw.pattern_brush);
        if(cached==rasters_.end()){pattern=make_hardware_raster(bitmap.width,bitmap.height,bitmap.pixels);rasters_.emplace(draw.pattern_brush,pattern);}else pattern=cached->second;
    }
    surface->draw(draw,pattern.get());if(native_gui_ && draw.kind==GuiDraw::Kind::Text && dc>=0x10000000)native_gui_->graphics_text(dc-0x10000000,draw);return true;
}
std::span<const WindowsServiceDescriptor> gdi_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","GrayStringA",9},{"user32.dll","GrayStringW",9},
        {"user32.dll","DrawStateA",10},{"user32.dll","DrawStateW",10},
        {"user32.dll","DrawTextW",5},{"user32.dll","DrawTextA",5},{"user32.dll","DrawTextExW",6},{"user32.dll","DrawTextExA",6},
        {"user32.dll","TabbedTextOutW",8},{"user32.dll","TabbedTextOutA",8},{"user32.dll","GetTabbedTextExtentW",5},{"user32.dll","GetTabbedTextExtentA",5},
        {"user32.dll","FillRect",3},{"user32.dll","FrameRect",3},{"user32.dll","InvertRect",2},{"user32.dll","DrawFocusRect",2},{"user32.dll","DrawEdge",4},{"user32.dll","DrawFrameControl",4},{"gdi32.dll","GdiFlush",0},{"gdi32.dll","BitBlt",9},{"gdi32.dll","StretchBlt",11},{"msimg32.dll","AlphaBlend",11},{"gdi32.dll","GdiAlphaBlend",11},{"msimg32.dll","TransparentBlt",11},{"gdi32.dll","PatBlt",6},{"gdi32.dll","CreateHatchBrush",2},{"gdi32.dll","Ellipse",5},
        {"user32.dll","DrawIcon",4},{"user32.dll","DrawIconEx",9},
        {"gdi32.dll","SetStretchBltMode",2},{"gdi32.dll","GetStretchBltMode",1},{"gdi32.dll","SetBrushOrgEx",4},{"gdi32.dll","GetBrushOrgEx",2},{"gdi32.dll","CreatePatternBrush",1},
        {"gdi32.dll","GetBitmapBits",3},{"gdi32.dll","SetBitmapBits",3},{"gdi32.dll","CreateBrushIndirect",1},
        {"gdi32.dll","GetDeviceGammaRamp",2},{"gdi32.dll","SetDeviceGammaRamp",2},
        {"gdi32.dll","SelectPalette",3},{"gdi32.dll","RealizePalette",1},{"gdi32.dll","CreatePalette",1},
        {"gdi32.dll","GetPaletteEntries",4},{"gdi32.dll","SetPaletteEntries",4},{"gdi32.dll","ResizePalette",2},
        {"gdi32.dll","GetDIBColorTable",4},{"gdi32.dll","SetDIBColorTable",4},
        {"gdi32.dll","CreateHalftonePalette",1},{"gdi32.dll","GetSystemPaletteEntries",4},
        {"gdi32.dll","AnimatePalette",4},{"gdi32.dll","GetNearestPaletteIndex",2},{"gdi32.dll","GetNearestColor",2},
        {"gdi32.dll","UnrealizeObject",1},{"gdi32.dll","UpdateColors",1},
        {"gdi32.dll","GetBkMode",1},{"gdi32.dll","GetBkColor",1},{"gdi32.dll","GetTextColor",1},{"gdi32.dll","GetTextAlign",1},
        {"gdi32.dll","CreateBitmap",5},{"gdi32.dll","CreateDIBSection",6},{"gdi32.dll","TranslateCharsetInfo",3},{"gdi32.dll","GetICMProfileA",3},{"gdi32.dll","GetICMProfileW",3},{"gdi32.dll","CreateDCA",4},{"gdi32.dll","CreateDCW",4},{"gdi32.dll","CreateICA",4},{"gdi32.dll","CreateICW",4},{"gdi32.dll","CreateCompatibleDC",1},{"gdi32.dll","DeleteDC",1},{"gdi32.dll","CreateCompatibleBitmap",3},
        {"gdi32.dll","SelectObject",2},{"gdi32.dll","DeleteObject",1},{"gdi32.dll","SetTextAlign",2},{"gdi32.dll","SetTextColor",2},{"gdi32.dll","SetBkColor",2},{"gdi32.dll","SetBkMode",2},
        {"msimg32.dll","GradientFill",6,0,0,52},{"gdi32.dll","GdiGradientFill",6,0,0,52},
        {"gdi32.dll","ExtTextOutA",8},{"gdi32.dll","ExtTextOutW",8},{"gdi32.dll","TextOutA",5},{"gdi32.dll","TextOutW",5},
        {"gdi32.dll","GetPixel",3},{"gdi32.dll","SetPixel",4},{"gdi32.dll","MoveToEx",4},{"gdi32.dll","LineTo",3},{"gdi32.dll","Polyline",3},
        {"gdi32.dll","CreatePen",3},{"gdi32.dll","Rectangle",5}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_gdi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};const auto si=[&](unsigned i){return std::bit_cast<std::int32_t>(d(i));};std::uint64_t result=0;
    try {
    const auto window=arg(0)>=0x10000000?gui_.windows.find(arg(0)-0x10000000):gui_.windows.end();
    const bool known_dc=arg(0) && (gui_.contexts.contains(arg(0)) || gui_.dc_objects.contains(arg(0)) || window!=gui_.windows.end());
#include "gdi_palette_handlers.inc"
    const bool uses_dc=symbol!="GdiFlush" && symbol!="CreateBrushIndirect" && symbol!="CreateHatchBrush" && symbol!="CreatePatternBrush" && symbol!="CreateBitmap" && symbol!="CreateDIBSection" && symbol!="CreatePen" && symbol!="DeleteObject" && symbol!="TranslateCharsetInfo" && symbol!="CreateDCA" && symbol!="CreateDCW" && symbol!="CreateICA" && symbol!="CreateICW" && symbol!="CreateCompatibleDC" && symbol!="DeleteDC" && symbol!="GetBitmapBits" && symbol!="SetBitmapBits";
    // 非 HDC 參數、未知 handle 與 DeleteDC 使用局部狀態，不能讓查詢製造有效 DC。
    GuiDc unused_dc;auto existing_dc=gui_.contexts.find(arg(0));
    if(uses_dc && known_dc && existing_dc==gui_.contexts.end())existing_dc=gui_.contexts.try_emplace(arg(0)).first;
    auto& dc=uses_dc && known_dc?existing_dc->second:unused_dc;
    if(window!=gui_.windows.end() && !gui_.dc_objects.contains(arg(0))) {dc.color=window->second.text_color;dc.mode=window->second.background_mode;}
    const bool text_calculation=symbol.starts_with("DrawText") && (d(4)&0x400U) && (symbol.starts_with("DrawTextEx") || !(d(4)&0x80U));
    if(is_printer_dc(arg(0)) && !printer_drawing_enabled(arg(0)) && (symbol=="FillRect" || symbol=="FrameRect" || symbol=="InvertRect" || symbol=="DrawFocusRect" || symbol=="DrawEdge" || symbol=="DrawFrameControl" || symbol=="SetPixel" || symbol=="LineTo" || symbol=="Polyline" || symbol=="Rectangle" || symbol=="Ellipse" || symbol.find("Blt")!=symbol.npos || symbol.find("Blend")!=symbol.npos || symbol.starts_with("TextOut") || symbol.starts_with("ExtTextOut") || (symbol.starts_with("DrawText") && !text_calculation) || symbol.starts_with("TabbedTextOut") || symbol.starts_with("DrawIcon")) && !prepare_legacy_printer_page(memory,arg(0))){arg.return_to(state,symbol=="SetPixel"?UINT32_MAX:0);return std::nullopt;}
    if(gui_.extended_pens.contains(dc.pen) && (symbol=="LineTo" || symbol=="Polyline" || symbol=="Rectangle" || symbol=="Ellipse"))return invoke_gdi_path(state,memory,service);
    if(dc.path.state==1){
        if(symbol=="MoveToEx" || symbol=="LineTo" || symbol=="Polyline" || symbol=="Rectangle" || symbol=="Ellipse")return invoke_gdi_path(state,memory,service);
        if(symbol.starts_with("TextOut") || symbol.starts_with("ExtTextOut") || (symbol.starts_with("DrawText") && !text_calculation) || symbol.starts_with("TabbedTextOut")){last_error_=50;arg.return_to(state,0);return std::nullopt;}
    }
    if(symbol=="GrayStringA" || symbol=="GrayStringW")return invoke_gray_string(state,memory,service);
    if(symbol=="DrawStateA" || symbol=="DrawStateW")return invoke_draw_state(state,memory,service);
    if(symbol.starts_with("DrawText") || symbol.starts_with("TabbedTextOut") || symbol.starts_with("GetTabbedTextExtent"))return invoke_gdi_text(state,memory,service);
    if(symbol=="GetDIBColorTable" || symbol=="SetDIBColorTable"){
        const bool set=symbol=="SetDIBColorTable";const auto selected=gui_.bitmaps.find(dc.bitmap);
        if(!known_dc)last_error_=6;
        else if(selected==gui_.bitmaps.end() || !selected->second.dib_address || selected->second.bits_per_pixel>8)last_error_=87;
        else {auto& bitmap=selected->second;const auto start=d(1);
            if(start<bitmap.dib_palette.size()){
                const auto count=std::min<std::size_t>(d(2),bitmap.dib_palette.size()-start);
                if(count && !arg(3))last_error_=87;
                else if(count){
                    memory.check(GuestAddress{arg(3)},count*4,set?Access::Read:Access::Write);
                    if(set){
                        std::vector<std::uint32_t> colors;colors.reserve(count);
                        for(std::size_t i=0;i<count;++i){const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+i*4},32));
                            colors.push_back(((value>>16)&255)|(value&0xff00)|((value&255)<<16));}
                        // 先完成舊色表的繪圖，再以相同索引 backing 重新解碼；色表變動不改寫索引。
                        sync_raster_bitmap(memory,dc.bitmap);
                        std::copy(colors.begin(),colors.end(),bitmap.dib_palette.begin()+start);
                        import_dib_bitmap(memory,dc.bitmap);bitmap.dib_upload_pending=true;bitmap.resource_id=0;
                        rasters_.erase(dc.bitmap);raster_shared_.erase(dc.bitmap);
                    }else for(std::size_t i=0;i<count;++i){const auto value=bitmap.dib_palette[start+i];
                        memory.store(GuestAddress{arg(3)+i*4},32,((value>>16)&255)|(value&0xff00)|((value&255)<<16));}
                    result=count;
                }
            }
        }
    }else if(symbol=="GetDeviceGammaRamp" || symbol=="SetDeviceGammaRamp"){
        if(!known_dc)last_error_=6;
        else if(!arg(1))last_error_=87;
        else if(is_printer_dc(arg(0)) || (!dc.display && !gui_.dc_objects.contains(arg(0)) && window==gui_.windows.end()) || !native_gui_)last_error_=50;
        else {
            if(!runtime_.network.external_io)throw HostCallFault("原生 gamma ramp 不支援 deterministic compare");
            const bool set=symbol=="SetDeviceGammaRamp";memory.check(GuestAddress{arg(1)},1536,set?Access::Read:Access::Write);
            if(set){
                std::array<std::uint16_t,768> ramp{};bool readable=true;
                for(unsigned c=0;c<3;++c)for(unsigned i=0;i<256;++i){const auto at=c*256+i;ramp[at]=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(1)+2ULL*at},16));
                    // Windows 亦會忽略可能令整個畫面不可讀的曲線；採用單調且偏差不超過半量程的子集。
                    const auto delta=static_cast<int>(ramp[at])-static_cast<int>(i*257);if(std::abs(delta)>32768 || (i && ramp[at]<ramp[at-1]))readable=false;
                }
                if(!readable)result=1; // 已辨識的 Windows 保護行為：不套用、不回報 API 失敗。
                else if(native_gui_->set_gamma_ramp(ramp))result=1;else last_error_=50;
            }else if(const auto ramp=native_gui_->gamma_ramp()){
                for(unsigned i=0;i<768;++i)memory.store(GuestAddress{arg(1)+2ULL*i},16,(*ramp)[i]);result=1;
            }else last_error_=50;
        }
    }else if(symbol=="GetICMProfileA" || symbol=="GetICMProfileW"){
        if(!known_dc)last_error_=6;else if(!arg(1))last_error_=87;
        else {memory.check(GuestAddress{arg(1)},4,Access::Write);last_error_=2015;} // 虛擬顯示裝置目前沒有關聯的 ICC 檔。
    }
    else if(symbol=="GdiFlush"){for(auto& [key,surface]:rasters_){surface->flush(true);if(gui_.bitmaps.contains(key) && gui_.bitmaps.at(key).dib_address)sync_raster_bitmap(memory,key);}result=1;}
    else if(symbol=="CreateBrushIndirect"){
        if(!arg(0))last_error_=87;
        else {
            memory.check(GuestAddress{arg(0)},x86_?12:16,Access::Read);
            const auto style=memory.load(GuestAddress{arg(0)},32),color=memory.load(GuestAddress{arg(0)+4},32),hatch=memory.load(GuestAddress{arg(0)+8},x86_?32:64);
            const auto forward=[&](std::string_view target,std::vector<std::uint64_t> arguments){start_callbacks(state,memory,{{resolve("gdi32.dll",target).value,std::move(arguments)}},Continuation::ReturnCallback);};
            if(style==0){forward("CreateSolidBrush",{color});return std::nullopt;}
            if(style==1)result=0xb005;
            else if(style==2){forward("CreateHatchBrush",{hatch,color});return std::nullopt;}
            else if(style==3 || style==7){forward("CreatePatternBrush",{hatch});return std::nullopt;}
            else last_error_=style==5 || style==6 || style==8?50:87;
        }
    }
    else if(symbol=="CreateHatchBrush"){if(d(0)>5)last_error_=87;else{result=gui_.next_object++;gui_.brushes[result]=d(1);gui_.brush_hatches[result]=si(0);}}
    else if(symbol=="CreatePatternBrush"){
        const auto bitmap=gui_.bitmaps.find(arg(0));if(bitmap==gui_.bitmaps.end())last_error_=6;
        else {sync_raster_bitmap(memory,arg(0));GuiBitmap copy{bitmap->second.width,bitmap->second.height,bitmap->second.pixels};copy.bits_per_pixel=bitmap->second.bits_per_pixel;
            const auto handle=gui_.next_object;gui_.brush_patterns.emplace(handle,std::move(copy));
            try{gui_.brushes.emplace(handle,0);}catch(...){gui_.brush_patterns.erase(handle);throw;}
            result=handle;++gui_.next_object;
        }
    }
    else if(symbol=="GetBitmapBits" || symbol=="SetBitmapBits"){
        const bool set=symbol=="SetBitmapBits";const auto found=gui_.bitmaps.find(arg(0));
        if(found==gui_.bitmaps.end())last_error_=6;
        else {
            auto& bitmap=found->second;const auto bpp=bitmap.bits_per_pixel;
            if(bpp!=1 && bpp!=4 && bpp!=8 && bpp!=16 && bpp!=24 && bpp!=32)last_error_=50;
            else {
                const auto pixel_bytes=(bitmap.width*bpp+7)/8;
                const auto stride=(bitmap.width*bpp+15)/16*2;const auto size=static_cast<std::size_t>(stride)*bitmap.height;
                const auto count=!set && si(1)<0?size:std::min<std::size_t>(d(1),size);
                if(!set && !arg(2))result=size;
                else if(set && !arg(2))last_error_=87;
                else if(count){
                    memory.check(GuestAddress{arg(2)},count,set?Access::Read:Access::Write);sync_raster_bitmap(memory,arg(0));
                    dib_scratch_.assign(size,0);std::vector<std::uint8_t> backing;
                    const auto palette=bitmap.dib_palette.empty()?gui_dib_palette(bpp):bitmap.dib_palette;
                    const DibRowCodec codec(bpp,bitmap.dib_masks,palette);
                    if(bitmap.dib_address){backing.resize(static_cast<std::size_t>(bitmap.stride)*bitmap.height);memory.read(GuestAddress{bitmap.dib_address},backing);
                        for(std::uint32_t y=0;y<bitmap.height;++y){const auto row=bitmap.signed_height<0?y:bitmap.height-1-y;
                            std::copy_n(backing.begin()+static_cast<std::ptrdiff_t>(row*bitmap.stride),pixel_bytes,dib_scratch_.begin()+static_cast<std::ptrdiff_t>(y*stride));}}
                    else for(std::uint32_t y=0;y<bitmap.height;++y)
                        if(!codec.encode(std::span(bitmap.pixels).subspan(static_cast<std::size_t>(y)*bitmap.width,bitmap.width),std::span(dib_scratch_).subspan(static_cast<std::size_t>(y)*stride,stride)))throw HostCallFault("Bitmap 掃描列長度不符");
                    if(!set){memory.write(GuestAddress{arg(2)},std::span(dib_scratch_).first(count));result=count;}
                    else {
                        memory.read(GuestAddress{arg(2)},std::span(dib_scratch_).first(count));auto pixels=bitmap.pixels;
                        for(std::uint32_t y=0;y<bitmap.height;++y)
                            if(!codec.decode(std::span(dib_scratch_).subspan(static_cast<std::size_t>(y)*stride,stride),std::span(pixels).subspan(static_cast<std::size_t>(y)*bitmap.width,bitmap.width)))throw HostCallFault("Bitmap 掃描列或色表索引不合法");
                        if(bitmap.dib_address){memory.check(GuestAddress{bitmap.dib_address},backing.size(),Access::Write);
                            for(std::uint32_t y=0;y<bitmap.height;++y){const auto row=bitmap.signed_height<0?y:bitmap.height-1-y;
                                // WORD 與 DWORD 行距的 padding 沒有像素對應，保留 DIB 原資料。
                                std::copy_n(dib_scratch_.begin()+static_cast<std::ptrdiff_t>(y*stride),pixel_bytes,backing.begin()+static_cast<std::ptrdiff_t>(row*bitmap.stride));}
                            memory.write(GuestAddress{bitmap.dib_address},backing);}
                        if(const auto cached=rasters_.find(arg(0));cached!=rasters_.end()){
                            if(raster_shared_.contains(arg(0))){auto replacement=make_hardware_raster(bitmap.width,bitmap.height,pixels);replacement->upload(pixels,true);cached->second=std::move(replacement);raster_shared_.erase(arg(0));}
                            else cached->second->upload(pixels,true);
                        }
                        bitmap.pixels=std::move(pixels);bitmap.resource_id=0;bitmap.raw_alpha=bpp==32;bitmap.dib_upload_pending=false;result=count;
                    }
                }
            }
        }
    }
    else if(symbol=="SetStretchBltMode" || symbol=="GetStretchBltMode"){
        if(!known_dc)last_error_=6;else if(symbol=="SetStretchBltMode" && (d(1)<1 || d(1)>4))last_error_=87;
        else {result=dc.stretch_mode;if(symbol=="SetStretchBltMode")dc.stretch_mode=d(1);}
    }
    else if(symbol=="SetBrushOrgEx" || symbol=="GetBrushOrgEx"){
        const bool set=symbol=="SetBrushOrgEx";const auto pointer=arg(set?3:1);
        if(!known_dc)last_error_=6;else if(!set && !pointer)last_error_=87;
        else {if(pointer){memory.check(GuestAddress{pointer},8,Access::Write);for(unsigned i=0;i<2;++i)memory.store(GuestAddress{pointer+i*4},32,static_cast<std::uint32_t>(dc.brush_origin[i]));}
            if(set)dc.brush_origin={si(1),si(2)};result=1;
        }
    }
#include "windows_gdi_gradient.inc"
    else if(symbol=="BitBlt" || symbol=="StretchBlt" || (symbol=="AlphaBlend" || symbol=="GdiAlphaBlend") || symbol=="TransparentBlt" || symbol=="PatBlt"){
        const bool pat=symbol=="PatBlt",stretch=symbol!="BitBlt" && !pat,alpha=(symbol=="AlphaBlend" || symbol=="GdiAlphaBlend"),transparent=symbol=="TransparentBlt";
        const auto mode=d(pat?5:stretch?10:8),rop=alpha || transparent?0xccU:(mode>>16)&255U;
        const bool reads_source=((rop^(rop>>2))&0x33U)!=0,reads_pattern=((rop^(rop>>4))&15U)!=0;
        const auto sx=pat?0:si(6),sy=pat?0:si(7),sw=stretch?si(8):si(3),sh=stretch?si(9):si(4);
        const auto end=[](std::int32_t a,std::int32_t b){const auto v=static_cast<std::int64_t>(a)+b;if(v<INT32_MIN || v>INT32_MAX)throw HostCallFault("GDI 座標溢位");return static_cast<std::int32_t>(v);};
        if(!known_dc || (reads_source && !gui_.contexts.contains(arg(5))))last_error_=6;
        else if((pat && reads_source) || (alpha && (mode&0xff00ffffU & ~0x01000000U)) || ((alpha || transparent) && (si(3)<=0 || si(4)<=0 || sw<=0 || sh<=0)))last_error_=87;
        else if(!alpha && !transparent && (mode&0x40000000U))last_error_=50; // CAPTUREBLT 需要視窗合成後端。
        else if(!si(3) || !si(4) || (reads_source && (!sw || !sh)))result=1;
        else if(reads_pattern && dc.brush==0xb005)result=1;
        else {
            const auto dst=raster_surface(memory,arg(0)),src=reads_source?raster_surface(memory,arg(5),false):dst;
            if(!dst || !src)last_error_=50;
            else {
                RasterBlit options;options.operation=rop;options.transparent=transparent?mode&0xffffffU:UINT32_MAX;auto& brush=options.brush;
                const auto handle=dc.brush;brush.color=handle==0xb012?dc.dc_brush_color:gui_.brushes.contains(handle)?gui_.brushes.at(handle):handle==0xb000?0xffffff:handle==0xb001?0xc0c0c0:handle==0xb002?0x808080:handle==0xb003?0x404040:0;
                brush.hatch=gui_.brush_hatches.contains(handle)?gui_.brush_hatches.at(handle):-1;brush.pattern_opaque=dc.mode==2;brush.pattern_background=dc.background;brush.pattern_color=dc.color;brush.brush_origin=dc.brush_origin;
                gui_resolve_draw_colors(gui_,dc,brush);
                std::shared_ptr<RasterSurface> pattern;
                if(reads_pattern && gui_.brush_patterns.contains(handle)){const auto& bitmap=gui_.brush_patterns.at(handle);brush.pattern_monochrome=bitmap.bits_per_pixel==1;
                    const auto cached=rasters_.find(handle);if(cached==rasters_.end()){pattern=make_hardware_raster(bitmap.width,bitmap.height,bitmap.pixels);rasters_.emplace(handle,pattern);}else pattern=cached->second;
                }
                auto target_dc=dc;if(!stretch || alpha || (transparent && dc.stretch_mode<3))target_dc.stretch_mode=3;
                GuiRect source_rect{sx,sy,end(sx,sw),end(sy,sh)};bool valid=true;
                if(reads_source){const auto transform=gui_device_transform(gui_.contexts.at(arg(5)));if(transform[1]!=0 || transform[2]!=0){last_error_=87;valid=false;}
                    else {const auto a=gui_integer_point(transform,source_rect[0],source_rect[1]),b=gui_integer_point(transform,source_rect[2],source_rect[3]);if(!a || !b){last_error_=87;valid=false;}else source_rect={(*a)[0],(*a)[1],(*b)[0],(*b)[1]};}}
                if(valid){dst->blit(*src,{si(1),si(2),end(si(1),si(3)),end(si(2),si(4))},source_rect,target_dc,alpha?mode:UINT32_MAX,options,pattern.get());result=1;}
            }
        }
    }
    else if(symbol=="FillRect" || symbol=="FrameRect" || symbol=="InvertRect" || symbol=="DrawFocusRect" || symbol=="DrawEdge" || symbol=="DrawFrameControl"){
        if(!known_dc)last_error_=6;else if(!arg(1))last_error_=87;
        else if((symbol=="DrawFocusRect" || symbol=="DrawFrameControl") && dc.map_mode!=1)last_error_=50;
        else {
            GuiRect rect{};for(unsigned i=0;i<4;++i)rect[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+i*4},32));const auto width=static_cast<std::int64_t>(rect[2])-rect[0],height=static_cast<std::int64_t>(rect[3])-rect[1];const auto draw_bounds=rect;
            if(width<=0 || height<=0)result=1;
            else if(width>16384 || height>16384 || rect[0]<INT32_MIN+32768 || rect[1]<INT32_MIN+32768 || rect[2]>INT32_MAX-32768 || rect[3]>INT32_MAX-32768)last_error_=87;
            else {
                std::vector<GuiDraw> draws;std::uint32_t error=0;bool adjust=false;
                const auto box=[&](int left,int top,int right,int bottom,const GuiDraw& style){if(right<=left || bottom<=top)return;auto draw=style;draw.x=left;draw.y=top;draw.right=right;draw.bottom=bottom;draws.push_back(std::move(draw));};
                if(symbol=="DrawEdge"){error=gui_draw_edge(rect,d(2),d(3),draws);adjust=d(3)&0x2000;}
                else if(symbol=="DrawFrameControl"){
                    if(d(2)==4 && (d(3)&0x41f)==0x410 && width*height>65536)error=8;
                    else error=gui_frame_control(rect,d(2),d(3),draws);adjust=d(2)==4 && (d(3)&31)==16 && (d(3)&0x2000);
                }else {
                    GuiDraw style;style.kind=GuiDraw::Kind::Fill;
                    if(symbol=="FillRect" || symbol=="FrameRect"){
                        const auto brush=arg(2);const bool system=symbol=="FillRect" && brush>=1 && brush<=31;
                        if(brush==0xb005){result=1;arg.return_to(state,result);return std::nullopt;}
                        if(!system && !gui_.brushes.contains(brush) && !(brush>=0xb000 && brush<=0xb004) && brush!=0xb012)error=6;
                        else {constexpr std::array<unsigned,5> stocks{0xffffff,0xc0c0c0,0x808080,0x404040,0};style.color=system?gui_system_color(static_cast<unsigned>(brush-1)):brush==0xb012?dc.dc_brush_color:gui_.brushes.contains(brush)?gui_.brushes.at(brush):stocks[brush-0xb000];style.hatch=gui_.brush_hatches.contains(brush)?gui_.brush_hatches.at(brush):-1;style.pattern_background=dc.background;style.pattern_opaque=dc.mode==2;style.brush_origin=dc.brush_origin;style.pattern_color=dc.color;if(gui_.brush_patterns.contains(brush)){style.pattern_brush=brush;style.pattern_monochrome=gui_.brush_patterns.at(brush).bits_per_pixel==1;}}
                    }else {style.rop2=symbol=="InvertRect"?6:7;style.color=0xffffff;}
                    if(!error){
                        if(symbol=="FrameRect"){box(rect[0],rect[1],rect[2],rect[1]+1,style);if(height>1)box(rect[0],rect[3]-1,rect[2],rect[3],style);box(rect[0],rect[1]+1,rect[0]+1,rect[3]-1,style);if(width>1)box(rect[2]-1,rect[1]+1,rect[2],rect[3]-1,style);}
                        else if(symbol=="DrawFocusRect"){
                            for(int x=rect[0];x<rect[2];++x){if(((x^rect[1])&1)==0)box(x,rect[1],x+1,rect[1]+1,style);if(height>1 && ((x^(rect[3]-1))&1)==0)box(x,rect[3]-1,x+1,rect[3],style);}
                            for(int y=rect[1]+1;y<rect[3]-1;++y){if(((rect[0]^y)&1)==0)box(rect[0],y,rect[0]+1,y+1,style);if(width>1 && (((rect[2]-1)^y)&1)==0)box(rect[2]-1,y,rect[2],y+1,style);}
                        }else box(rect[0],rect[1],rect[2],rect[3],style);
                    }
                }
                if(error)last_error_=error;
                else {if(adjust)memory.check(GuestAddress{arg(1)},16,Access::Write);result=1;for(auto& draw:draws){draw.clipped=dc.clipped;draw.clip=dc.clip;draw.excluded=dc.excluded;if(symbol=="DrawFrameControl")logical_clip(draw,dc,draw_bounds);if(!raster_draw(memory,arg(0),draw)){if(draw.pattern_brush || window==gui_.windows.end()){last_error_=50;result=0;break;}window->second.drawing.push_back(draw);}}if(result && adjust)for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(1)+i*4},32,static_cast<std::uint32_t>(rect[i]));if(result && window!=gui_.windows.end())present_gui(window->first);}
            }
        }
    }
    else if(symbol=="DrawIcon" || symbol=="DrawIconEx"){
        const bool ex=symbol=="DrawIconEx";result=draw_gui_icon(memory,arg(0),arg(3),si(1),si(2),ex?d(4):0,ex?d(5):0,ex?d(8):15,ex?arg(7):0,ex?d(6):0);
    }
    else if(symbol=="GetBkMode")result=dc.mode;
    else if(symbol=="GetBkColor")result=dc.background;
    else if(symbol=="GetTextColor")result=dc.color;
    else if(symbol=="GetTextAlign")result=dc.alignment;
    else if(symbol=="CreateDIBSection"){
        if(!arg(3)){last_error_=87;}
        else {
            memory.store(GuestAddress{arg(3)},x86_?32:64,0);
            if(!arg(1) || d(2)>1){last_error_=87;}
            else {
                memory.check(GuestAddress{arg(1)},40,Access::Read);
                const auto field=[&](unsigned offset,std::uint8_t bits=32){return static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+offset},bits));};
                const auto width=std::bit_cast<std::int32_t>(field(4)),height=std::bit_cast<std::int32_t>(field(8));const auto bpp=field(14,16),header=field(0),compression=field(16);
                std::array<std::uint32_t,4> masks=bpp==16?std::array<std::uint32_t,4>{0x7c00,0x3e0,0x1f,0}:std::array<std::uint32_t,4>{0xff0000,0xff00,0xff,bpp==32?0xff000000U:0U};
                const bool supported=(header==40 || header==52 || header==56 || header==108 || header==124) && (bpp==1 || bpp==4 || bpp==8 || bpp==16 || bpp==24 || bpp==32) && (compression==0 || (compression==3 && (bpp==16 || bpp==32)));
                if(supported){memory.check(GuestAddress{arg(1)},header,Access::Read);if(compression==3){for(unsigned i=0;i<3;++i)masks[i]=field(40+i*4);masks[3]=header>=56?field(52):0;}}
                if(!supported)last_error_=50;
                else if(bpp>8 && !valid_dib_masks(masks,bpp))last_error_=87;
                else if(width<=0 || !height || height==INT32_MIN || field(12,16)!=1)last_error_=87;
                else {
                    const auto rows=static_cast<std::uint32_t>(height<0?-height:height),columns=static_cast<std::uint32_t>(width);
                    if(columns>16384 || rows>16384 || std::uint64_t{columns}*rows>64*1024*1024)last_error_=8;
                    else if(bpp<=8 && field(32)>(1U<<bpp))last_error_=87;
                    else if(d(2)==1 && bpp<=8 && !known_dc)last_error_=6;
                    else {
                        std::vector<std::uint32_t> palette;
                        if(bpp<=8){const auto count=field(32)?field(32):1U<<bpp;palette.reserve(count);
                            const auto logical=gui_palette_entries(gui_,gui_.contexts.contains(arg(0))?gui_.contexts.at(arg(0)).palette:0xb00f);
                            memory.check(GuestAddress{arg(1)+header},static_cast<std::size_t>(count)*(d(2)?2:4),Access::Read);
                            for(unsigned i=0;i<count;++i){if(d(2)){const auto index=field(header+i*2,16);palette.push_back(index<logical.size()?gui_palette_color(logical[index]):0);}
                                else {const auto value=field(header+i*4);palette.push_back(((value>>16)&255)|(value&0xff00)|((value&255)<<16));}}
                        }
                        result=create_dib_bitmap(memory,state,columns,height,static_cast<std::uint16_t>(bpp),arg(4),d(5));
                        if(result){auto& bitmap=gui_.bitmaps.at(result);bitmap.dib_compression=compression;bitmap.dib_masks=masks;bitmap.dib_palette=std::move(palette);import_dib_bitmap(memory,result);memory.store(GuestAddress{arg(3)},x86_?32:64,bitmap.dib_address);}
                    }
                }
            }
        }
    }
    else if(symbol=="CreateBitmap"){
        const auto bpp=d(3);if(si(0)<0 || si(1)<0 || d(0)>4096 || d(1)>4096 || d(2)!=1 || (bpp!=1 && bpp!=16 && bpp!=24 && bpp!=32)){last_error_=87;}
        else {const auto width=d(0) && d(1)?d(0):1U,height=d(0) && d(1)?d(1):1U,depth=d(0) && d(1)?bpp:1U;
            GuiBitmap bitmap{width,height,std::vector<std::uint32_t>(static_cast<std::size_t>(width)*height)};bitmap.bits_per_pixel=static_cast<std::uint16_t>(depth);bitmap.stride=(width*depth+15)/16*2;
            if(depth==16)bitmap.dib_masks={0x7c00,0x3e0,0x1f,0};else bitmap.dib_masks[3]=0;
            if(arg(4) && d(0) && d(1)){dib_scratch_.resize(static_cast<std::size_t>(bitmap.stride)*height);memory.read(GuestAddress{arg(4)},dib_scratch_);
                if(depth==1){for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)bitmap.pixels[static_cast<std::size_t>(y)*width+x]=(dib_scratch_[static_cast<std::size_t>(y)*bitmap.stride+x/8]&(0x80U>>(x%8)))?0xffffff:0;}
                else {const DibRowCodec codec(depth,bitmap.dib_masks);for(unsigned y=0;y<height;++y)if(!codec.decode(std::span(dib_scratch_).subspan(static_cast<std::size_t>(y)*bitmap.stride,bitmap.stride),std::span(bitmap.pixels).subspan(static_cast<std::size_t>(y)*width,width)))throw HostCallFault("DDB 掃描列長度不符");}
            }
            result=gui_.next_object++;gui_.bitmaps.emplace(result,std::move(bitmap));}
    }
    else if(symbol=="TranslateCharsetInfo"){
        const auto input=d(2)==1?d(0):d(2)==2?d(0):UINT32_MAX;
        if(input==0 || input==1 || input==65001 || input==1252){memory.write(GuestAddress{arg(1)},std::array<std::uint8_t,32>{});memory.store(GuestAddress{arg(1)+4},32,input==65001?65001:1252);result=1;}else last_error_=87;
    }else if(symbol=="CreateDCA" || symbol=="CreateDCW" || symbol=="CreateICA" || symbol=="CreateICW"){
        const auto text=[&](std::uint64_t pointer){auto value=pointer?(symbol.ends_with('W')?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16(read_ansi(memory,pointer))):std::u16string{};for(auto& c:value)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return value;};
        const auto driver=text(arg(0)),device=text(arg(1));
        if((driver.empty() || driver==u"winspool") && device==u"win64 pdf")result=create_printer_dc(memory,arg(3),symbol.ends_with('W'),symbol.starts_with("CreateIC"));
        else if(driver.empty() && device.empty())last_error_=87;
        else if((driver.empty() || driver==u"display" || driver==u"\\\\.\\display1") && (device.empty() || device==u"\\\\.\\display1")){
            result=gui_.next_object++;gui_.contexts[result]={};gui_.contexts[result].display=true;
        }else last_error_=50;
    }else if(symbol=="CreateCompatibleDC"){if(arg(0) && !known_dc)last_error_=6;else{result=gui_.next_object++;gui_.contexts[result]={};}}
    else if(symbol=="CreateCompatibleBitmap"){
        if(si(1)>0 && si(2)>0 && d(1)<=4096 && d(2)<=4096){
            const auto selected=gui_.bitmaps.find(dc.bitmap);
            if(selected!=gui_.bitmaps.end() && selected->second.dib_address){result=create_dib_bitmap(memory,state,d(1),si(2),selected->second.bits_per_pixel);if(result){gui_.bitmaps.at(result).dib_masks=selected->second.dib_masks;gui_.bitmaps.at(result).dib_compression=selected->second.dib_compression;gui_.bitmaps.at(result).dib_palette=selected->second.dib_palette;import_dib_bitmap(memory,result);}}
            else {result=gui_.next_object++;gui_.bitmaps[result]={d(1),d(2),std::vector<std::uint32_t>(static_cast<std::size_t>(d(1))*d(2))};}
        }else last_error_=87;
    }else if(symbol=="DeleteDC"){delete_printer_dc(arg(0));gui_.emf_recorders.erase(arg(0));result=gui_.contexts.erase(arg(0))?1:0;gui_.dc_objects.erase(arg(0));gui_.saved_contexts.erase(arg(0));}
    else if(symbol=="DeleteObject"){
        const auto bitmap=gui_.bitmaps.find(arg(0));
        const auto selected=[&](const GuiDc& context){return context.bitmap==arg(0) || context.font==arg(0) || context.pen==arg(0) || context.brush==arg(0) || context.palette==arg(0);};
        const bool in_use=std::any_of(gui_.contexts.begin(),gui_.contexts.end(),[&](const auto& entry){return selected(entry.second);}) || std::any_of(gui_.saved_contexts.begin(),gui_.saved_contexts.end(),[&](const auto& entry){return std::any_of(entry.second.begin(),entry.second.end(),selected);});
        if(in_use){last_error_=170;}
        else {rasters_.erase(arg(0));raster_shared_.erase(arg(0));gui_.pen_widths.erase(arg(0));gui_.extended_pens.erase(arg(0));gui_.brush_hatches.erase(arg(0));gui_.brush_patterns.erase(arg(0));
            if(bitmap!=gui_.bitmaps.end() && bitmap->second.dib_address){if(bitmap->second.dib_view)(void)unmap_file_view(memory,bitmap->second.dib_view,true);else (void)free_heap(memory,bitmap->second.dib_address,state);}
            result=((gui_.font_natives.erase(arg(0)),gui_.fonts.erase(arg(0))) || gui_.bitmaps.erase(arg(0)) || gui_.brushes.erase(arg(0)) || gui_.pens.erase(arg(0)) || gui_.regions.erase(arg(0)) || gui_.palettes.erase(arg(0)))?1:0;
        }
    }
    else if(symbol=="SelectObject"){
        if(!known_dc)last_error_=6;
        else if(is_printer_dc(arg(0)) && (gui_.bitmaps.contains(arg(1)) || arg(1)==0xb015))last_error_=50;
        else if(gui_.bitmaps.contains(arg(1)) || arg(1)==0xb015){
            if(arg(1)!=0xb015 && std::any_of(gui_.contexts.begin(),gui_.contexts.end(),[&](const auto& entry){return entry.first!=arg(0) && entry.second.bitmap==arg(1);})){last_error_=170;}
            else {result=dc.bitmap?dc.bitmap:0xb015;dc.bitmap=arg(1);}
        }
        else if(gui_.fonts.contains(arg(1)) || (arg(1)>=0xb00a && arg(1)<=0xb011 && arg(1)!=0xb00f)){result=dc.font;dc.font=arg(1);gui_.dc_objects[arg(0)]=arg(1);}
        else if(gui_.pens.contains(arg(1)) || arg(1)==0xb013 || (arg(1)>=0xb006 && arg(1)<=0xb008)){result=dc.pen;dc.pen=arg(1);}
        else if(gui_.brushes.contains(arg(1)) || arg(1)==0xb012 || (arg(1)>=0xb000 && arg(1)<=0xb005)){result=dc.brush;dc.brush=arg(1);}
        else last_error_=6;
    }else if(symbol=="SetTextColor"){result=dc.color;dc.color=d(1);if(window!=gui_.windows.end())window->second.text_color=d(1);}
    else if(symbol=="SetBkColor"){result=dc.background;dc.background=d(1);}
    else if(symbol=="SetBkMode"){result=dc.mode;if(d(1)==1 || d(1)==2)dc.mode=d(1);else result=0;if(window!=gui_.windows.end())window->second.background_mode=dc.mode;}
    else if(symbol=="SetTextAlign"){result=dc.alignment;dc.alignment=d(1);}
    else if(symbol=="GetPixel" || symbol=="SetPixel"){
        const auto bitmap=gui_.bitmaps.find(dc.bitmap);result=UINT32_MAX;const auto pixel_color=symbol=="SetPixel"?gui_resolve_color(gui_,dc,d(3)):0;
        const auto point=gui_integer_point(gui_device_transform(dc),si(1),si(2));if(!point){last_error_=87;arg.return_to(state,result);return std::nullopt;}const auto px=(*point)[0],py=(*point)[1];const bool clipped=(dc.clipped && (px<dc.clip[0] || px>=dc.clip[2] || py<dc.clip[1] || py>=dc.clip[3])) || std::any_of(dc.excluded.begin(),dc.excluded.end(),[&](const auto& r){return px>=r[0] && px<r[2] && py>=r[1] && py<r[3];});
        if(clipped){arg.return_to(state,result);return std::nullopt;}
        if(const auto surface=raster_surface(memory,arg(0),symbol=="SetPixel");surface && px>=0 && py>=0 && static_cast<unsigned>(px)<surface->width() && static_cast<unsigned>(py)<surface->height()){
            if(symbol=="SetPixel"){
                GuiDraw draw;draw.kind=GuiDraw::Kind::Fill;draw.x=px;draw.y=py;draw.right=draw.x+1;draw.bottom=draw.y+1;draw.color=pixel_color;draw.clipped=dc.clipped;draw.clip=dc.clip;draw.excluded=dc.excluded;surface->draw(draw);
                // 原生 raster 是 BGRA8 UNORM，已解析的 RGB 可精確表示。
                // 無須為回傳同一顏色而逐點等待 GPU；GetPixel／GdiFlush 仍保留讀回屏障。
                static const bool readback=[] {const auto v=std::getenv("X64ARM_GDI_PIXEL_READBACK");return v && std::string_view(v)=="1";}();
                result=readback?surface->pixel(static_cast<unsigned>(px),static_cast<unsigned>(py)):pixel_color;
            }else result=surface->pixel(static_cast<unsigned>(px),static_cast<unsigned>(py));
        }else if(window!=gui_.windows.end() && !window->second.layered_pixels.empty() && px>=0 && py>=0 && static_cast<unsigned>(px)<window->second.width && static_cast<unsigned>(py)<window->second.height){auto& pixel=window->second.layered_pixels[static_cast<std::size_t>(py)*window->second.width+static_cast<unsigned>(px)];if(symbol=="SetPixel")pixel=0xff000000U|pixel_color;result=pixel&0xffffffU;
        }else if(bitmap!=gui_.bitmaps.end() && px>=0 && py>=0 && static_cast<unsigned>(px)<bitmap->second.width && static_cast<unsigned>(py)<bitmap->second.height){auto& pixel=bitmap->second.pixels[static_cast<std::size_t>(py)*bitmap->second.width+static_cast<unsigned>(px)];if(symbol=="SetPixel"){pixel=pixel_color;export_dib_bitmap(memory,dc.bitmap);}result=pixel&0xffffff;}
    }else if(symbol.starts_with("ExtTextOut") || symbol.starts_with("TextOut")){
        const bool ext=symbol.starts_with("Ext"),wide=symbol.ends_with('W');const auto count=d(ext?6:4),options=ext?d(3):0;
        const auto fail_text=[&](unsigned error){last_error_=error;arg.return_to(state,0);return std::optional<std::uint32_t>{};};
        if(count>(ext?8192U:65536U) || (options&~0x2096U))return fail_text(87);
        // Microsoft 的 A 版契約：GLYPH_INDEX 成功但不繪圖，也不錄入 EMF。
        if(!wide && (options&16U)){arg.return_to(state,1);return std::nullopt;}
        try {
        GuiDraw draw;draw.kind=GuiDraw::Kind::Text;draw.x=(dc.alignment&1)?dc.x:si(1);draw.y=(dc.alignment&1)?dc.y:si(2);draw.color=gui_resolve_color(gui_,dc,dc.color);const auto pointer=arg(ext?5:3);
        draw.glyph_indices=(options&16U)!=0;draw.rtl_reading=!draw.glyph_indices && ((options&128U) || (dc.alignment&256U));
        if(count && !pointer)return fail_text(87);memory.check(GuestAddress{pointer},count*(wide?2U:1U),Access::Read);
        std::string narrow;
        if(wide)for(std::uint64_t i=0;i<count;++i)draw.text.push_back(static_cast<char16_t>(memory.load(GuestAddress{pointer+i*2},16)));
        else {narrow.resize(count);for(std::uint64_t i=0;i<count;++i)narrow[static_cast<std::size_t>(i)]=static_cast<char>(memory.load(GuestAddress{pointer+i},8));draw.text=utf8_to_utf16_lossy(narrow);}
        draw.font=gui_font(gui_,dc.font);
        const auto metrics=native_gui_?native_gui_->font_metrics(draw.font).value_or(fallback_font_metrics(draw.font)):fallback_font_metrics(draw.font);
        if(metrics.fixed_pitch && !draw.glyph_indices)draw.cell_width=static_cast<std::uint32_t>(metrics.average_width);
        const auto checked=[](std::int64_t value){if(value<INT32_MIN || value>INT32_MAX)throw std::invalid_argument("GDI 文字座標溢位");return static_cast<std::int32_t>(value);};
        if(ext && arg(7)){
            const bool pdy=(options&8192U)!=0;const auto stride=pdy?8U:4U;memory.check(GuestAddress{arg(7)},static_cast<std::size_t>(count)*stride,Access::Read);
            const auto advance=[&](unsigned i,unsigned axis){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(7)+i*stride+axis*4},32)));};
            // ETO_PDY 使用字型向上的垂直軸，GuiDraw 的邏輯 Y 向下。
            if(wide)for(unsigned i=0;i<count;++i){draw.advances.push_back(advance(i,0));if(pdy)draw.advances_y.push_back(checked(-static_cast<std::int64_t>(advance(i,1))));}
            else for(int at=0;at<static_cast<int>(count);){const auto begin=at;UChar32 cp;U8_NEXT_OR_FFFD(narrow.data(),at,static_cast<int>(count),cp);std::int64_t x=0,y=0;
                for(int i=begin;i<at;++i){x+=advance(static_cast<unsigned>(i),0);if(pdy)y+=advance(static_cast<unsigned>(i),1);}draw.advances.push_back(checked(x));if(pdy){checked(y);draw.advances_y.push_back(checked(-y));}
                if(cp>0xffff){draw.advances.push_back(0);if(pdy)draw.advances_y.push_back(0);}}
        }
        if(draw.glyph_indices){const auto widths=native_gdi_glyph_advances(draw.font,draw.text);if(draw.advances.empty())draw.advances=widths;}
        const auto fallback_width=static_cast<std::int32_t>(draw.text.size())*metrics.average_width;
        std::int32_t text_width=0,text_height=0;if(draw.advances.empty())text_width=native_gui_?native_gui_->text_width(draw.font,draw.text).value_or(fallback_width):fallback_width;
        else {std::int64_t x=0,y=0;for(std::size_t i=0;i<draw.advances.size();++i){x+=draw.advances[i];if(!draw.advances_y.empty())y+=draw.advances_y[i];checked(x);checked(y);}text_width=checked(x);text_height=checked(y);}
        const auto reference_x=draw.x,reference_y=draw.y;
        if((dc.alignment&6)==2){draw.x=checked(static_cast<std::int64_t>(draw.x)-text_width);draw.y=checked(static_cast<std::int64_t>(draw.y)-text_height);}
        else if((dc.alignment&6)==6){draw.x=checked(static_cast<std::int64_t>(draw.x)-text_width/2);draw.y=checked(static_cast<std::int64_t>(draw.y)-text_height/2);}
        if((dc.alignment&24)==24)draw.y=checked(static_cast<std::int64_t>(draw.y)-metrics.ascent);else if(dc.alignment&8)draw.y=checked(static_cast<std::int64_t>(draw.y)-metrics.height);
        checked(static_cast<std::int64_t>(draw.x)+text_width);checked(static_cast<std::int64_t>(draw.y)+metrics.height);
        const auto next_x=checked(static_cast<std::int64_t>(reference_x)+((dc.alignment&6)==6?0:(dc.alignment&2)?-static_cast<std::int64_t>(text_width):text_width));
        const auto next_y=checked(static_cast<std::int64_t>(reference_y)+((dc.alignment&6)==6?0:(dc.alignment&2)?-static_cast<std::int64_t>(text_height):text_height));
        draw.clipped=dc.clipped;draw.clip=dc.clip;draw.excluded=dc.excluded;draw.transform=gui_device_transform(dc);if(ext && (options&4) && arg(4)){GuiRect rect{};for(unsigned i=0;i<4;++i)rect[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+i*4},32)));logical_clip(draw,dc,rect);}
        const auto rectangle=[&](GuiDraw& fill){fill.kind=GuiDraw::Kind::Fill;fill.color=gui_resolve_color(gui_,dc,dc.background);fill.x=draw.x;fill.y=draw.y;fill.right=draw.x+text_width;fill.bottom=draw.y+metrics.height;if(ext && arg(4) && (options&2)){fill.x=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)},32)));fill.y=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+4},32)));fill.right=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+8},32)));fill.bottom=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+12},32)));}};
        if(gui_.emf_recorders.contains(arg(0))){
            if(dc.mode==2 || (options&2)){GuiDraw fill;rectangle(fill);fill.clipped=draw.clipped;fill.clip=draw.clip;fill.excluded=draw.excluded;fill.transform=draw.transform;record_metafile_draw(arg(0),fill);}
            if(!draw.text.empty())record_metafile_draw(arg(0),draw);result=1;
        }else if(const auto surface=raster_surface(memory,arg(0))){
            if(dc.mode==2 || (options&2)){GuiDraw fill;rectangle(fill);fill.clipped=draw.clipped;fill.clip=draw.clip;fill.excluded=draw.excluded;fill.transform=gui_device_transform(dc);surface->draw(fill);}
            if(!draw.text.empty()){draw.transform=gui_device_transform(dc);surface->draw(draw);}
            // 保留有界的文字診斷，畫面本身存在 GPU，不重播歷史圖元。
            if(window!=gui_.windows.end()){auto& history=window->second.drawing;if(history.size()>=256)history.erase(history.begin(),history.begin()+128);history.push_back(draw);if(!draw.glyph_indices)native_gui_->graphics_text(window->first,draw);}result=1;
        }else if(window!=gui_.windows.end()){
            if(window->second.drawing.size()>65536)throw HostCallFault("GDI 繪圖命令超出上限");
            if(dc.mode==2 || (options&2)){GuiDraw fill;rectangle(fill);fill.clipped=draw.clipped;fill.clip=draw.clip;fill.excluded=draw.excluded;fill.transform=gui_device_transform(dc);window->second.drawing.push_back(fill);}
            if(!draw.text.empty())window->second.drawing.push_back(draw);present_gui(window->first);result=1;
        }else if(gui_.bitmaps.contains(dc.bitmap)){
            if(draw.glyph_indices || std::any_of(draw.text.begin(),draw.text.end(),[](char16_t c){return c!=u' ';}))return fail_text(50);
            auto& bitmap=gui_.bitmaps.at(dc.bitmap);GuiDraw fill;rectangle(fill);
            for(std::uint32_t y=0;y<bitmap.height;++y)for(std::uint32_t x=0;x<bitmap.width;++x)if(static_cast<std::int32_t>(x)>=fill.x && static_cast<std::int32_t>(x)<fill.right && static_cast<std::int32_t>(y)>=fill.y && static_cast<std::int32_t>(y)<fill.bottom){auto& pixel=bitmap.pixels[static_cast<std::size_t>(y)*bitmap.width+x];if(dc.mode==2 || (options&2))pixel=gui_resolve_color(gui_,dc,dc.background);if(draw.font.underline && static_cast<std::int32_t>(y)==draw.y+metrics.ascent+1)pixel=gui_resolve_color(gui_,dc,dc.color);}
            export_dib_bitmap(memory,dc.bitmap);result=1;
        }else last_error_=6;
        if(result && (dc.alignment&1)){dc.x=next_x;dc.y=next_y;}
        }catch(const std::bad_alloc&){return fail_text(8);}catch(const std::invalid_argument&){return fail_text(87);}catch(const std::domain_error&){return fail_text(50);}
    }else if(symbol=="CreatePen"){if((d(0)&15)!=0 && (d(0)&15)!=5)last_error_=50;else{result=gui_.next_object++;gui_.pens[result]=d(2);gui_.pen_widths[result]=(d(0)&15)==5?0:std::max(1,si(1));}}
    else if(symbol=="MoveToEx"){if(arg(3)){memory.store(GuestAddress{arg(3)},32,static_cast<std::uint32_t>(dc.x));memory.store(GuestAddress{arg(3)+4},32,static_cast<std::uint32_t>(dc.y));}dc.x=si(1);dc.y=si(2);result=1;}
    else if(symbol=="Polyline"){
        const auto count=si(2);
        if(count<2 || count>65536)last_error_=87;
        else if(window==gui_.windows.end() && !raster_surface(memory,arg(0)))last_error_=dc.bitmap?50:6;
        else if(window!=gui_.windows.end() && !native_gui_ && window->second.drawing.size()+static_cast<std::size_t>(count-1)>65536)last_error_=8;
        else {
            memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(count)*8,Access::Read);
            const auto coordinate=[&](std::uint64_t offset){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+offset},32)));};
            // POINT 在 x64 仍由兩個 LONG 組成；Polyline 不讀取或改動 DC 的 current position。
            auto x=coordinate(0),y=coordinate(4);
            for(std::int32_t i=1;i<count;++i){const auto offset=static_cast<std::uint64_t>(i)*8;const auto next_x=coordinate(offset),next_y=coordinate(offset+4);
                if(dc.pen!=0xb008){GuiDraw draw;draw.kind=GuiDraw::Kind::Line;draw.rop2=dc.rop2;draw.x=x;draw.y=y;draw.right=next_x;draw.bottom=next_y;
                    draw.color=dc.pen==0xb013?dc.dc_pen_color:gui_.pens.contains(dc.pen)?gui_.pens.at(dc.pen):dc.pen==0xb006?0xffffff:0;
                    draw.clipped=dc.clipped;draw.clip=dc.clip;draw.excluded=dc.excluded;draw.pen_width=gui_.pen_widths.contains(dc.pen)?gui_.pen_widths.at(dc.pen):1;if(!raster_draw(memory,arg(0),draw))window->second.drawing.push_back(std::move(draw));}
                x=next_x;y=next_y;
            }
            if(window!=gui_.windows.end() && !native_gui_)present_gui(window->first);result=1;
        }
    }
    else if(symbol=="LineTo" || symbol=="Rectangle" || symbol=="Ellipse"){
        GuiDraw draw;draw.rop2=dc.rop2;draw.kind=symbol=="LineTo"?GuiDraw::Kind::Line:symbol=="Ellipse"?GuiDraw::Kind::Ellipse:GuiDraw::Kind::Rectangle;
        draw.clipped=dc.clipped;draw.clip=dc.clip;draw.excluded=dc.excluded;draw.color=dc.pen==0xb013?dc.dc_pen_color:gui_.pens.contains(dc.pen)?gui_.pens.at(dc.pen):dc.pen==0xb006?0xffffff:0;
        draw.pen_width=dc.pen==0xb008?0:gui_.pen_widths.contains(dc.pen)?gui_.pen_widths.at(dc.pen):1;
        draw.x=symbol=="LineTo"?dc.x:si(1);draw.y=symbol=="LineTo"?dc.y:si(2);draw.right=symbol=="LineTo"?si(1):si(3);draw.bottom=symbol=="LineTo"?si(2):si(4);
        draw.filled=symbol!="LineTo" && dc.brush!=0xb005;draw.fill_color=dc.brush==0xb012?dc.dc_brush_color:gui_.brushes.contains(dc.brush)?gui_.brushes.at(dc.brush):dc.brush==0xb000?0xffffff:dc.brush==0xb001?0xc0c0c0:dc.brush==0xb002?0x808080:dc.brush==0xb003?0x404040:0;
        draw.hatch=gui_.brush_hatches.contains(dc.brush)?gui_.brush_hatches.at(dc.brush):-1;draw.pattern_background=dc.background;draw.pattern_opaque=dc.mode==2;
        draw.brush_origin=dc.brush_origin;draw.pattern_color=dc.color;if(draw.filled && gui_.brush_patterns.contains(dc.brush)){draw.pattern_brush=dc.brush;draw.pattern_monochrome=gui_.brush_patterns.at(dc.brush).bits_per_pixel==1;}
        if(raster_draw(memory,arg(0),draw))result=1;
        else if(draw.pattern_brush)last_error_=50;
        else if(window!=gui_.windows.end()){
            if(draw.filled){auto fill=draw;fill.kind=GuiDraw::Kind::Fill;fill.color=draw.fill_color;window->second.drawing.push_back(fill);}if(draw.pen_width)window->second.drawing.push_back(draw);present_gui(window->first);result=1;
        }else last_error_=6;
        if(symbol=="LineTo"){dc.x=si(1);dc.y=si(2);}
    }
    else throw HostCallFault("尚未實作 GDI 服務");
    }catch(const std::bad_alloc&){last_error_=8;result=(symbol=="GetPixel" || symbol=="SetPixel")?UINT32_MAX:0;}
    arg.return_to(state,result);return std::nullopt;
}
}
