#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_userdraw.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
namespace {
std::int32_t signed_value(std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));}
bool gray_brush(const GuiState& gui,std::uint64_t handle){return gui.brushes.contains(handle) || gui.brush_patterns.contains(handle) || (handle>=0xb000 && handle<=0xb005) || handle==0xb012;}
}
void WindowsServices::discard_gray_string(std::uint64_t id,GuestMemory& memory,const GuestState& state){
    const auto found=gui_.gray_strings.find(id);if(found==gui_.gray_strings.end())return;
    const auto operation=found->second;gui_.gray_strings.erase(found);
    gui_.contexts.erase(operation.temporary_dc);gui_.dc_objects.erase(operation.temporary_dc);gui_.saved_contexts.erase(operation.temporary_dc);
    // callback 借用的字型與應用程式位圖仍由客體持有，只釋放本次配置。
    gui_.bitmaps.erase(operation.bitmap);rasters_.erase(operation.bitmap);raster_shared_.erase(operation.bitmap);rasters_.erase(operation.temporary_dc);
    if(operation.source_dc){gui_.contexts.erase(operation.source_dc);gui_.dc_objects.erase(operation.source_dc);gui_.saved_contexts.erase(operation.source_dc);rasters_.erase(operation.source_dc);}
    if(operation.scratch)(void)free_heap(memory,operation.scratch,state);
}
std::optional<std::uint32_t> WindowsServices::invoke_gray_string(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool wide=service.symbol.ends_with('W');
    const auto fail=[&](std::uint32_t error){last_error_=error;arg.return_to(state,0);return std::optional<std::uint32_t>{};};
    const auto context=gui_.contexts.find(arg(0));if(context==gui_.contexts.end())return fail(6);
    const auto brush=arg(1)?arg(1):UINT64_C(0xb004);if(!gray_brush(gui_,brush))return fail(6);
    auto count=signed_value(arg(4)),width=signed_value(arg(7)),height=signed_value(arg(8));
    if(count < -1 || count>65536 || (!arg(2) && count<0) || width<0 || height<0)return fail(87);
    std::u16string text;
    if(!count){
        if(!arg(3))return fail(87);
        if(wide){text=read_utf16(memory,GuestAddress{arg(3)});if(text.size()>65536)return fail(87);count=static_cast<std::int32_t>(text.size());}
        else {const auto bytes=read_ansi(memory,arg(3));if(bytes.size()>65536)return fail(87);count=static_cast<std::int32_t>(bytes.size());text=utf8_to_utf16_lossy(bytes);}
    }else if((!width || !height) && count>=0){
        if(count && !arg(3))return fail(87);
        memory.check(GuestAddress{arg(3)},static_cast<std::size_t>(count)*(wide?2U:1U),Access::Read);
        if(wide)for(int i=0;i<count;++i)text.push_back(static_cast<char16_t>(memory.load(GuestAddress{arg(3)+static_cast<std::uint64_t>(i)*2},16)));
        else {std::string bytes(static_cast<std::size_t>(count),'\0');for(int i=0;i<count;++i)bytes[static_cast<std::size_t>(i)]=static_cast<char>(memory.load(GuestAddress{arg(3)+static_cast<std::uint64_t>(i)},8));text=utf8_to_utf16_lossy(bytes);}
    }
    if(!width || !height){
        if(count<0)return fail(87);const auto font=gui_font(gui_,context->second.font);
        const auto metrics=native_gui_?native_gui_->font_metrics(font).value_or(fallback_font_metrics(font)):fallback_font_metrics(font);
        if(!width)width=native_gui_?native_gui_->text_width(font,text).value_or(static_cast<std::int32_t>(text.size())*metrics.average_width):static_cast<std::int32_t>(text.size())*metrics.average_width;
        if(!height)height=metrics.height;
    }
    const auto x=signed_value(arg(5)),y=signed_value(arg(6));
    if(width<=0 || height<=0 || width>4096 || height>4096 || std::int64_t{x}+width>INT32_MAX || std::int64_t{y}+height>INT32_MAX)return fail(87);
    if(gui_.gray_strings.size()>=256 || gui_.contexts.size()>=8192 || gui_.bitmaps.size()>=4096)return fail(8);
    // 保留原 HDC 的狀態；OutputProc 接到獨立的記憶體 DC，以白色畫在黑色單色遮罩。
    const auto id=gui_.next_object++,temporary=gui_.next_object++,bitmap=gui_.next_object++;
    GuiGrayString operation{arg(0),temporary,bitmap,brush,threads_.current,count,x,y,width,height};
    try {
        gui_.gray_strings.emplace(id,operation);
        GuiBitmap image{static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),std::vector<std::uint32_t>(static_cast<std::size_t>(width)*static_cast<unsigned>(height))};image.bits_per_pixel=1;
        gui_.bitmaps.emplace(bitmap,std::move(image));GuiDc dc;dc.bitmap=bitmap;dc.font=context->second.font;dc.color=0xffffff;dc.background=0;dc.brush=0xb004;
        gui_.contexts.emplace(temporary,dc);
        if(!raster_surface(memory,arg(0)) || !raster_surface(memory,temporary)){discard_gray_string(id,memory,state);return fail(50);}
        if(arg(2))start_callbacks(state,memory,{{arg(2),{temporary,arg(3),static_cast<std::uint32_t>(count)}}},Continuation::GuiGrayString,id);
        else start_callbacks(state,memory,{{resolve("gdi32.dll",wide?"TextOutW":"TextOutA").value,{temporary,0,0,arg(3),static_cast<std::uint32_t>(count)}}},Continuation::GuiGrayString,id);
        return std::nullopt;
    }catch(...){discard_gray_string(id,memory,state);throw;}
}
std::optional<std::uint32_t> WindowsServices::resume_gray_string(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t value){
    const auto found=gui_.gray_strings.find(frame.value);if(found==gui_.gray_strings.end()){last_error_=6;complete_call(state,memory,0);return std::nullopt;}
    if(found->second.draw_state)return resume_draw_state(state,memory,frame,value);
    const auto operation=found->second;std::uint64_t result=static_cast<std::uint32_t>(value)!=0;
    try {
        const auto context=gui_.contexts.find(operation.dc);const auto source=raster_surface(memory,operation.temporary_dc,false);
        const auto target=context==gui_.contexts.end()?std::shared_ptr<RasterSurface>{}:raster_surface(memory,operation.dc);
        if(!source || !target || !gray_brush(gui_,operation.brush)){last_error_=6;result=0;}
        else if(operation.brush!=0xb005){
            const auto image=source->snapshot();const auto width=static_cast<unsigned>(operation.width),height=static_cast<unsigned>(operation.height);
            std::vector<std::uint32_t> mask(static_cast<std::size_t>(width)*height);
            // Mono 遮罩關閉抗鋸齒灰階，55／AA 交錯保留一半墨點。-1／FALSE 保留未灰化影像。
            const bool gray=operation.count!=-1 || static_cast<std::uint32_t>(value)!=0;
            for(unsigned y=0;y<std::min(height,image.height);++y)for(unsigned x=0;x<std::min(width,image.width);++x){
                const auto pixel=image.pixels[static_cast<std::size_t>(y)*image.width+x];
                const auto luminance=(pixel&255U)*77U+((pixel>>8)&255U)*150U+((pixel>>16)&255U)*29U;
                if(luminance>=32768U && (!gray || ((x+y)&1U)==0))mask[static_cast<std::size_t>(y)*width+x]=0xffffff;
            }
            auto stencil=make_hardware_raster(width,height,mask);GuiDc dc=context->second;
            // GrayString 的矩形使用裝置單位，但裁切仍採 HDC 的實際裝置區域。
            dc.world=gui_identity;dc.map_mode=1;dc.window_origin={};dc.viewport_origin={};dc.window_extent={1,1};dc.viewport_extent={1,1};
            RasterBlit options;options.operation=0xe2;auto& brush=options.brush;const auto handle=operation.brush;
            brush.color=handle==0xb012?dc.dc_brush_color:gui_.brushes.contains(handle)?gui_.brushes.at(handle):handle==0xb000?0xffffffU:handle==0xb001?0xc0c0c0U:handle==0xb002?0x808080U:handle==0xb003?0x404040U:0U;
            brush.hatch=gui_.brush_hatches.contains(handle)?gui_.brush_hatches.at(handle):-1;brush.pattern_opaque=dc.mode==2;brush.pattern_background=dc.background;brush.pattern_color=dc.color;brush.brush_origin=dc.brush_origin;
            std::shared_ptr<RasterSurface> pattern;
            if(gui_.brush_patterns.contains(handle)){const auto& bitmap=gui_.brush_patterns.at(handle);brush.pattern_brush=handle;brush.pattern_monochrome=bitmap.bits_per_pixel==1;pattern=make_hardware_raster(bitmap.width,bitmap.height,bitmap.pixels);}
            const std::array<std::int32_t,4> destination{operation.x,operation.y,operation.x+operation.width,operation.y+operation.height},rectangle{0,0,operation.width,operation.height};
            target->blit(*stencil,destination,rectangle,dc,UINT32_MAX,options,pattern.get());
            if(operation.dc>=0x10000000 && gui_.windows.contains(operation.dc-0x10000000))present_gui(operation.dc-0x10000000);
        }
    }catch(const std::bad_alloc&){last_error_=8;result=0;}catch(const MemoryFault&){discard_gray_string(frame.value,memory,state);throw;}catch(const std::exception&){last_error_=50;result=0;}
    discard_gray_string(frame.value,memory,state);complete_call(state,memory,result);return std::nullopt;
}
#include "windows_draw_state.inc"
}
