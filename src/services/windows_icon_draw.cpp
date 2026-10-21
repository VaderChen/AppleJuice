#include "x64arm/windows_services.hpp"
#include "x64arm/windows_icon.hpp"
#include <algorithm>
#include <limits>

namespace x64arm {
bool WindowsServices::draw_gui_icon(GuestMemory& memory,std::uint64_t hdc,std::uint64_t handle,std::int32_t x,std::int32_t y,std::uint32_t width,std::uint32_t height,std::uint32_t flags,std::uint64_t brush,std::uint32_t step){
    const auto fail=[&](std::uint32_t error){last_error_=error;return false;};
    const auto found=gui_.icons.find(handle);const auto context=gui_.contexts.find(hdc);
    if(found==gui_.icons.end() || context==gui_.contexts.end())return fail(6);
    if(flags&~0x1fU)return fail(87);
    const auto& source_icon=found->second;
    if(!source_icon.animation_sequence.empty() && step>=source_icon.animation_sequence.size())return fail(87);
    const auto& icon=source_icon.animation_sequence.empty()?source_icon:source_icon.animation_frames.at(source_icon.animation_sequence[step]);
    width=width?width:flags&8?32:icon.width;height=height?height:flags&8?32:icon.height;
    if(!width || !height || width>4096 || height>4096 || std::int64_t{x}+width>INT32_MAX || std::int64_t{y}+height>INT32_MAX)return fail(87);
    const auto count=static_cast<std::size_t>(icon.width)*icon.height;
    if(icon.mask_pixels.size()!=count*(icon.monochrome?2U:1U) || (!icon.monochrome && icon.color_pixels.size()!=count))return fail(50);
    if(brush && !gui_.brushes.contains(brush) && !gui_.brush_patterns.contains(brush) && (brush<0xb000 || brush>0xb005) && brush!=0xb012)return fail(6);
    const std::array<std::int32_t,4> destination{x,y,x+static_cast<std::int32_t>(width),y+static_cast<std::int32_t>(height)},source{0,0,static_cast<std::int32_t>(icon.width),static_cast<std::int32_t>(icon.height)};
    if((flags&3)==0)return true;
    auto target=raster_surface(memory,hdc);auto dc=context->second;
    if(!target){
        if(brush || (flags&3)!=3)return fail(50);
        GuiDraw draw;draw.kind=GuiDraw::Kind::Icon;draw.x=x;draw.y=y;draw.right=destination[2];draw.bottom=destination[3];draw.image=icon.image;draw.system_icon=icon.system;
        draw.clipped=dc.clipped;draw.clip=dc.clip;draw.excluded=dc.excluded;
        if(record_metafile_draw(hdc,draw))return true;
        const auto window=hdc>=0x10000000?gui_.windows.find(hdc-0x10000000):gui_.windows.end();if(window==gui_.windows.end())return fail(50);
        window->second.drawing.push_back(std::move(draw));present_gui(window->first);return true;
    }
    try {
        auto output=target;auto rectangle=destination;GuiDc draw_dc=dc;
        if(brush){
            output=make_hardware_raster(width,height);rectangle={0,0,static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)};draw_dc=GuiDc{};
            if(brush!=0xb005){GuiDraw fill;fill.kind=GuiDraw::Kind::Fill;fill.right=rectangle[2];fill.bottom=rectangle[3];
                fill.color=brush==0xb012?dc.dc_brush_color:gui_.brushes.contains(brush)?gui_.brushes.at(brush):brush==0xb000?0xffffffU:brush==0xb001?0xc0c0c0U:brush==0xb002?0x808080U:brush==0xb003?0x404040U:0U;
                fill.hatch=gui_.brush_hatches.contains(brush)?gui_.brush_hatches.at(brush):-1;fill.pattern_opaque=dc.mode==2;fill.pattern_background=dc.background;fill.pattern_color=dc.color;fill.brush_origin={dc.brush_origin[0]-x,dc.brush_origin[1]-y};
                std::shared_ptr<RasterSurface> pattern;if(gui_.brush_patterns.contains(brush)){const auto& bitmap=gui_.brush_patterns.at(brush);fill.pattern_brush=brush;fill.pattern_monochrome=bitmap.bits_per_pixel==1;pattern=make_hardware_raster(bitmap.width,bitmap.height,bitmap.pixels);}
                output->draw(fill,pattern.get());
            }
        }
        if((flags&3)==3 && icon.alpha){
            auto image=make_hardware_raster(icon.width,icon.height);image->upload(icon.color_pixels,true);output->blit(*image,rectangle,source,draw_dc,0x01ff0000U);
        }else {
            std::vector<std::uint32_t> pixels(count);RasterBlit operation;
            if(flags&1){for(std::size_t i=0;i<count;++i)pixels[i]=icon.mask_pixels[i]?0xffffffU:0U;auto mask=make_hardware_raster(icon.width,icon.height,pixels);operation.operation=(flags&2)?0x88U:0xccU;output->blit(*mask,rectangle,source,draw_dc,UINT32_MAX,operation);}
            if(flags&2){if(icon.monochrome)for(std::size_t i=0;i<count;++i)pixels[i]=icon.mask_pixels[count+i]?0xffffffU:0U;else pixels=icon.color_pixels;
                auto image=make_hardware_raster(icon.width,icon.height,pixels);operation.operation=(flags&1)?0x66U:0xccU;output->blit(*image,rectangle,source,draw_dc,UINT32_MAX,operation);}
        }
        if(brush){const std::array<std::int32_t,4> local{0,0,static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)};target->blit(*output,destination,local,dc,UINT32_MAX);}
        if(hdc>=0x10000000 && gui_.windows.contains(hdc-0x10000000))present_gui(hdc-0x10000000);return true;
    }catch(const std::bad_alloc&){return fail(8);}catch(const std::exception&){return fail(50);}
}
}
