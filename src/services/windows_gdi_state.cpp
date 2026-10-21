#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include <algorithm>
#include <bit>
#include "x64arm/windows_transform.hpp"
namespace x64arm {
std::span<const WindowsServiceDescriptor> gdi_state_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","SetROP2",2},{"gdi32.dll","GetROP2",1},
        {"gdi32.dll","SetDCPenColor",2},{"gdi32.dll","GetDCPenColor",1},{"gdi32.dll","SetDCBrushColor",2},{"gdi32.dll","GetDCBrushColor",1},
        {"gdi32.dll","SetGraphicsMode",2},{"gdi32.dll","GetGraphicsMode",1},{"gdi32.dll","SetLayout",2},{"gdi32.dll","GetLayout",1},{"gdi32.dll","SetMapMode",2},{"gdi32.dll","GetMapMode",1},
        {"gdi32.dll","SetWorldTransform",2},{"gdi32.dll","GetWorldTransform",2},{"gdi32.dll","ModifyWorldTransform",3},{"gdi32.dll","CombineTransform",3},
        {"gdi32.dll","SetViewportOrgEx",4},{"gdi32.dll","GetViewportOrgEx",2},{"gdi32.dll","OffsetViewportOrgEx",4},
        {"gdi32.dll","SetWindowOrgEx",4},{"gdi32.dll","GetWindowOrgEx",2},{"gdi32.dll","OffsetWindowOrgEx",4},
        {"gdi32.dll","SetViewportExtEx",4},{"gdi32.dll","GetViewportExtEx",2},{"gdi32.dll","ScaleViewportExtEx",6},
        {"gdi32.dll","SetWindowExtEx",4},{"gdi32.dll","GetWindowExtEx",2},{"gdi32.dll","ScaleWindowExtEx",6},
        {"gdi32.dll","LPtoDP",3},{"gdi32.dll","DPtoLP",3},
        {"gdi32.dll","SaveDC",1},{"gdi32.dll","RestoreDC",2},{"gdi32.dll","GetCurrentObject",2},{"gdi32.dll","GetObjectType",1},
        {"gdi32.dll","SelectClipRgn",2},{"gdi32.dll","ExtSelectClipRgn",3},{"gdi32.dll","GetClipRgn",2},{"gdi32.dll","GetClipBox",2},
        {"gdi32.dll","IntersectClipRect",5},{"gdi32.dll","ExcludeClipRect",5},{"gdi32.dll","OffsetClipRgn",3},
        {"gdi32.dll","PtVisible",3},{"gdi32.dll","RectVisible",2},{"gdi32.dll","GetMetaRgn",2},{"gdi32.dll","SetMetaRgn",1},{"gdi32.dll","GetRandomRgn",3}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_gdi_state(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto failure=[&](std::uint32_t error){last_error_=error;return finish(name=="GetClipRgn" || name=="GetRandomRgn" || name.ends_with("Color")?UINT32_MAX:0);};
    const auto integer=[&](unsigned i){return static_cast<std::int32_t>(arg(i));};
    const auto read_transform=[&](std::uint64_t at){GuiTransform m{};for(unsigned i=0;i<6;++i)m[i]=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{at+i*4},32)));return m;};
    const auto write_transform=[&](std::uint64_t at,const GuiTransform& m){memory.check(GuestAddress{at},24,Access::Write);for(unsigned i=0;i<6;++i)memory.store(GuestAddress{at+i*4},32,std::bit_cast<std::uint32_t>(static_cast<float>(m[i])));};
    if(name=="CombineTransform"){if(!arg(0) || !arg(1) || !arg(2))return failure(87);const auto combined=gui_multiply(read_transform(arg(1)),read_transform(arg(2)));if(!gui_valid_transform(combined))return failure(87);write_transform(arg(0),combined);return finish(1);}
    const auto window=arg(0)>=0x10000000?gui_.windows.find(arg(0)-0x10000000):gui_.windows.end();
    const bool known=gui_.contexts.contains(arg(0)) || gui_.dc_objects.contains(arg(0)) || window!=gui_.windows.end();
    if(name=="GetObjectType"){
        const auto h=arg(0);if(gui_.metafiles.contains(h))return finish(13);if(gui_.windows_metafiles.contains(h))return finish(9);if(gui_.emf_recorders.contains(h))return finish(12);
        if(known)return finish(is_printer_dc(h) || window!=gui_.windows.end() || (gui_.contexts.contains(h) && gui_.contexts.at(h).display)?3:10);
        if(gui_.bitmaps.contains(h) || h==0xb015)return finish(7);
        if(gui_.regions.contains(h))return finish(8);
        if(gui_.fonts.contains(h) || (h>=0xb00a && h<=0xb00e) || h==0xb010 || h==0xb011)return finish(6);
        if(gui_.brushes.contains(h) || (h>=0xb000 && h<=0xb005) || h==0xb012)return finish(2);
        if(gui_.extended_pens.contains(h))return finish(11);
        if(gui_.pens.contains(h) || (h>=0xb006 && h<=0xb008) || h==0xb013)return finish(1);
        if(h==0xb00f || gui_.palettes.contains(h))return finish(5);return failure(6);
    }
    if(name=="GetLayout" || name=="SetLayout"){
        // LAYOUT_BITMAPORIENTATIONPRESERVED 只在鏡像版面生效；LAYOUT_RTL 鏡像尚未接入裝置座標轉換，明確回報不支援而不假裝成功。
        constexpr std::uint64_t gdi_error=UINT32_MAX;
        if(!known){last_error_=6;return finish(gdi_error);}
        auto& value=gui_.contexts[arg(0)];if(name=="GetLayout")return finish(value.layout);
        const auto layout=static_cast<std::uint32_t>(arg(1));if(layout&~9U){last_error_=87;return finish(gdi_error);}if(layout&1U){last_error_=50;return finish(gdi_error);}
        const auto previous=value.layout;value.layout=layout;return finish(previous);
    }
    if(!known)return failure(6);auto& dc=gui_.contexts[arg(0)];
    const auto visible=[&]{
        std::uint32_t width=1,height=1;
        if(window!=gui_.windows.end()){width=window->second.width;height=window->second.height;}
        else if(const auto bitmap=gui_.bitmaps.find(dc.bitmap);bitmap!=gui_.bitmaps.end()){width=bitmap->second.width;height=bitmap->second.height;}
        else if(dc.display){width=1440;height=900;}
        return width && height?GuiRegion{{0,0,static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)}}:GuiRegion{};
    };
    const auto effective=[&](const GuiDc& value,bool bounded){
        std::optional<GuiRegion> region=value.application_clip;
        if(value.meta_clip)region=region?gui_region_combine(*region,*value.meta_clip,1):value.meta_clip;
        if(!region)return visible();return bounded?gui_region_combine(*region,visible(),1):*region;
    };
    const auto update=[&](GuiDc& value){
        value.clipped=value.application_clip.has_value() || value.meta_clip.has_value();value.excluded.clear();
        if(value.clipped){const auto region=effective(value,false);value.clip=gui_region_bounds(region);value.excluded=gui_region_combine(GuiRegion{value.clip},region,4);}
    };
    const auto write_rect=[&](std::uint64_t at,const GuiRect& rect){memory.check(GuestAddress{at},16,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{at+i*4},32,static_cast<std::uint32_t>(rect[i]));};
    try{
        if(name=="GetROP2")return finish(dc.rop2);
        if(name=="SetROP2"){if(arg(1)<1 || arg(1)>16)return failure(87);const auto old=dc.rop2;dc.rop2=static_cast<unsigned>(arg(1));return finish(old);}
        if(name=="GetDCPenColor" || name=="GetDCBrushColor" || name=="SetDCPenColor" || name=="SetDCBrushColor"){
            auto& color=name.find("Pen")!=name.npos?dc.dc_pen_color:dc.dc_brush_color;const auto old=color;
            if(name.starts_with("Set")){if(arg(1)&0xff000000U)return failure(50);color=static_cast<unsigned>(arg(1));}return finish(old);
        }
        if(name=="GetGraphicsMode")return finish(dc.graphics_mode);
        if(name=="SetGraphicsMode"){
            if(arg(1)!=1 && arg(1)!=2)return failure(87);if(arg(1)==1 && dc.world!=gui_identity)return failure(87);const auto old=dc.graphics_mode;dc.graphics_mode=static_cast<unsigned>(arg(1));return finish(old);
        }
        if(name=="GetWorldTransform"){if(!arg(1))return failure(87);write_transform(arg(1),dc.world);return finish(1);}
        if(name=="SetWorldTransform" || name=="ModifyWorldTransform"){
            if(dc.graphics_mode!=2)return failure(87);const auto mode=name=="SetWorldTransform"?4U:static_cast<unsigned>(arg(2));if(mode<1 || mode>4 || (mode!=1 && !arg(1)))return failure(87);
            auto changed=mode==1?gui_identity:read_transform(arg(1));if(mode==2)changed=gui_multiply(changed,dc.world);else if(mode==3)changed=gui_multiply(dc.world,changed);
            if(!gui_valid_transform(changed) || !gui_inverse(changed))return failure(87);dc.world=changed;return finish(1);
        }
        if(name=="GetMapMode")return finish(dc.map_mode);
        if(name=="SetMapMode"){
            if(arg(1)<1 || arg(1)>8)return failure(87);const auto previous=dc.map_mode;dc.map_mode=static_cast<unsigned>(arg(1));
            if(dc.map_mode==1)dc.window_extent=dc.viewport_extent={1,1};else if(dc.map_mode<=6){constexpr std::array<int,7> units{0,1,254,2540,100,1000,1440};dc.window_extent={units[dc.map_mode],units[dc.map_mode]};dc.viewport_extent={96,-96};}
            return finish(previous);
        }
        if(name=="LPtoDP" || name=="DPtoLP"){
            const auto count=integer(2);if(count<0 || count>1048576 || (count && !arg(1)))return failure(87);auto transform=gui_device_transform(dc);if(name=="DPtoLP"){const auto inverse=gui_inverse(transform);if(!inverse)return failure(87);transform=*inverse;}
            if(count)memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(count)*8,Access::Write);std::vector<std::array<std::int32_t,2>> points(static_cast<std::size_t>(count));
            for(unsigned i=0;i<points.size();++i){const auto point=gui_integer_point(transform,static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+i*8},32)),static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+i*8+4},32)));if(!point)return failure(87);points[i]=*point;}
            for(unsigned i=0;i<points.size();++i)for(unsigned j=0;j<2;++j)memory.store(GuestAddress{arg(1)+i*8+j*4},32,static_cast<std::uint32_t>(points[i][j]));return finish(1);
        }
        if(name.find("Viewport")!=name.npos || name.find("WindowOrg")!=name.npos || name.find("WindowExt")!=name.npos){
            const bool viewport=name.find("Viewport")!=name.npos,extent=name.find("ExtEx")!=name.npos,get=name.starts_with("Get"),scale=name.starts_with("Scale"),offset=name.starts_with("Offset");
            auto& value=extent?(viewport?dc.viewport_extent:dc.window_extent):(viewport?dc.viewport_origin:dc.window_origin);const auto output=arg(get?1:scale?5:3);if(get && !output)return failure(87);
            if(output)memory.check(GuestAddress{output},8,Access::Write);auto changed=value;
            if(!get && !(extent && dc.map_mode<7)){
                for(unsigned i=0;i<2;++i){double v=integer(1+i);if(scale){const auto denominator=integer(2+i*2);if(!denominator)return failure(87);v=std::round(static_cast<double>(value[i])*integer(1+i*2)/denominator);}else if(offset)v+=value[i];
                    if(v<INT32_MIN || v>INT32_MAX || (extent && v==0))return failure(87);changed[i]=static_cast<std::int32_t>(v);}
            }
            if(output)for(unsigned i=0;i<2;++i)memory.store(GuestAddress{output+i*4},32,static_cast<std::uint32_t>(value[i]));if(!get)value=changed;
            if(!get && extent && dc.map_mode==7){const double x=std::abs(static_cast<double>(dc.viewport_extent[0])/dc.window_extent[0]),y=std::abs(static_cast<double>(dc.viewport_extent[1])/dc.window_extent[1]);const auto factor=std::min(x,y);
                for(unsigned i=0;i<2;++i)dc.viewport_extent[i]=static_cast<std::int32_t>(std::copysign(std::max(1.0,std::round(std::abs(static_cast<double>(dc.window_extent[i]))*factor)),dc.viewport_extent[i]));}
            return finish(1);
        }
        if(name=="SaveDC"){
            auto& saved=gui_.saved_contexts[arg(0)];if(saved.size()>=4096)return failure(8);saved.push_back(dc);return finish(saved.size());
        }
        if(name=="RestoreDC"){
            const auto found=gui_.saved_contexts.find(arg(0));const auto level=integer(1);
            if(!level || found==gui_.saved_contexts.end())return failure(87);auto& saved=found->second;
            const auto index=level>0?static_cast<std::int64_t>(level)-1:static_cast<std::int64_t>(saved.size())+level;
            if(index<0 || static_cast<std::uint64_t>(index)>=saved.size())return failure(87);
            const auto restored=saved[static_cast<std::size_t>(index)];dc=restored;saved.resize(static_cast<std::size_t>(index));
            if(window!=gui_.windows.end()){window->second.text_color=dc.color;window->second.background_mode=dc.mode;}
            gui_.dc_objects[arg(0)]=dc.font;return finish(1);
        }
        if(name=="GetCurrentObject"){
            switch(arg(1)){case 1:return finish(dc.pen);case 2:return finish(dc.brush);case 5:return finish(dc.palette);case 6:return finish(dc.font);case 7:return finish(dc.bitmap?dc.bitmap:0xb015);default:return failure(87);}
        }
        if(name=="GetClipRgn" || name=="GetMetaRgn" || name=="GetRandomRgn"){
            const auto target=gui_.regions.find(arg(1));if(target==gui_.regions.end())return failure(6);
            if(name=="GetRandomRgn"){
                if(arg(2)!=4)return failure(87);auto region=visible();
                if(window!=gui_.windows.end()){const auto origin=gui_client_origin(gui_,window->first);for(auto& r:region)for(unsigned i=0;i<4;++i){const auto value=static_cast<std::int64_t>(r[i])+origin[i%2];if(value<INT32_MIN || value>INT32_MAX)return failure(87);r[i]=static_cast<std::int32_t>(value);}}
                target->second=std::move(region);return finish(1);
            }
            const auto& region=name=="GetClipRgn"?dc.application_clip:dc.meta_clip;
            if(!region)return finish(0);target->second=*region;return finish(1);
        }
        if(name=="GetClipBox"){const auto region=effective(dc,true);const auto inverse=gui_inverse(gui_device_transform(dc));if(!inverse)return failure(87);write_rect(arg(1),gui_transform_bounds(*inverse,gui_region_bounds(region)));return finish(gui_region_kind(region));}
        if(name=="PtVisible" || name=="RectVisible"){
            const auto region=effective(dc,true);
            if(name=="PtVisible"){const auto p=gui_integer_point(gui_device_transform(dc),integer(1),integer(2));if(!p)return failure(87);return finish(std::any_of(region.begin(),region.end(),[&](const auto& r){return (*p)[0]>=r[0] && (*p)[0]<r[2] && (*p)[1]>=r[1] && (*p)[1]<r[3];}));}
            GuiRect r{};for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+i*4},32));
            return finish(!gui_rect_empty(r) && !gui_region_combine(region,gui_transform_region_rect(gui_device_transform(dc),r),1).empty());
        }
        GuiDc changed=dc;
        if(name=="SetMetaRgn"){changed.meta_clip=effective(dc,false);changed.application_clip.reset();}
        else if(name=="SelectClipRgn" || name=="ExtSelectClipRgn"){
            const auto mode=name=="SelectClipRgn"?5:static_cast<unsigned>(arg(2));
            if(mode<1 || mode>5 || (!arg(1) && mode!=5))return failure(87);
            if(!arg(1))changed.application_clip.reset();
            else {const auto input=gui_.regions.find(arg(1));if(input==gui_.regions.end())return failure(6);
                changed.application_clip=mode==5?input->second:gui_region_combine(dc.application_clip.value_or(visible()),input->second,mode);}
        }else if(name=="OffsetClipRgn"){
            if(!dc.application_clip)return finish(2);auto region=*dc.application_clip;auto transform=gui_device_transform(dc);transform[4]=transform[5]=0;const auto delta=gui_integer_point(transform,integer(1),integer(2));if(!delta)return failure(87);
            for(auto& r:region)for(unsigned i=0;i<4;++i){const auto value=static_cast<std::int64_t>(r[i])+(*delta)[i%2];if(value<-(1<<26) || value>=(1<<26))return failure(87);r[i]=static_cast<std::int32_t>(value);}changed.application_clip=std::move(region);
        }else {
            const GuiRect rectangle{integer(1),integer(2),integer(3),integer(4)};GuiRegion region=gui_transform_region_rect(gui_device_transform(dc),rectangle);
            changed.application_clip=gui_region_combine(dc.application_clip.value_or(visible()),region,name=="IntersectClipRect"?1:4);
        }
        update(changed);const auto kind=gui_region_kind(effective(changed,false));dc=std::move(changed);return finish(kind);
    }catch(const std::bad_alloc&){return failure(8);}catch(const std::length_error&){return failure(8);}
}
}
