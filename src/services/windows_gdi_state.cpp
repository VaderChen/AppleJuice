#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include <algorithm>
#include <bit>
#include "x64arm/windows_transform.hpp"
#include "gui_paint_region.hpp"
#include "gui_cursor_bounds.hpp"
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
        {"win32u.dll","NtGdiComputeXformCoefficients",1},{"win32u.dll","NtGdiModifyWorldTransform",3,0,0,0x4},
        {"gdi32.dll","LPtoDP",3},{"gdi32.dll","DPtoLP",3},
        {"gdi32.dll","SaveDC",1},{"gdi32.dll","RestoreDC",2},{"gdi32.dll","GetCurrentObject",2},{"win32u.dll","NtGdiGetDCObject",2,0,0,2},{"gdi32.dll","GetObjectType",1},
        {"gdi32.dll","SelectClipRgn",2},{"gdi32.dll","ExtSelectClipRgn",3},{"gdi32.dll","GetClipRgn",2},{"gdi32.dll","GetClipBox",2},
        {"gdi32.dll","IntersectClipRect",5},{"gdi32.dll","ExcludeClipRect",5},{"gdi32.dll","OffsetClipRgn",3},
        {"gdi32.dll","PtVisible",3},{"gdi32.dll","RectVisible",2},{"gdi32.dll","GetMetaRgn",2},{"gdi32.dll","SetMetaRgn",1},{"gdi32.dll","GetRandomRgn",3},{"win32u.dll","NtGdiGetRandomRgn",3}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_gdi_state(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool native_region=service.symbol=="NtGdiGetRandomRgn";
    const auto name=native_region?std::string_view{"GetRandomRgn"}:service.symbol;
    const auto finish=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto failure=[&](std::uint32_t error){last_error_=error;return finish(name=="GetClipRgn" || name=="GetRandomRgn" || name.ends_with("Color")?UINT32_MAX:0);};
    const auto integer=[&](unsigned i){return static_cast<std::int32_t>(arg(i));};
    const auto read_transform=[&](std::uint64_t at){GuiTransform m{};for(unsigned i=0;i<6;++i)m[i]=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{at+i*4},32)));return m;};
    const auto write_transform=[&](std::uint64_t at,const GuiTransform& m){memory.check(GuestAddress{at},24,Access::Write);for(unsigned i=0;i<6;++i)memory.store(GuestAddress{at+i*4},32,std::bit_cast<std::uint32_t>(static_cast<float>(m[i])));};
    if(name=="CombineTransform"){
        if(!arg(0) || !arg(1) || !arg(2))return failure(87);
        try{
            const auto combined=gui_multiply(read_transform(arg(1)),read_transform(arg(2)));
            if(!gui_valid_transform(combined))return failure(87);
            write_transform(arg(0),combined);
            return finish(1);
        }catch(const MemoryFault&){return failure(998);}
    }
    const auto window=arg(0)>=0x10000000?gui_.windows.find(arg(0)-0x10000000):gui_.windows.end();
    const auto remote=window==gui_.windows.end() && arg(0)>=0x10000000 && gui_.dc_objects.contains(arg(0))?
        shared_gui_window(arg(0)-0x10000000):std::optional<SharedWindowInfo>{};
    const bool known=gui_.contexts.contains(arg(0)) || gui_.dc_objects.contains(arg(0)) || window!=gui_.windows.end();
    if(name=="GetObjectType"){
        const auto h=arg(0);if(gui_.metafiles.contains(h))return finish(13);if(gui_.windows_metafiles.contains(h))return finish(9);if(gui_.emf_recorders.contains(h))return finish(gui_.emf_recorders.at(h).wmf?4:12); // OBJ_METADC／OBJ_ENHMETADC
        if(known)return finish(is_printer_dc(h) || h==0x10000000 || remote || window!=gui_.windows.end() || (gui_.contexts.contains(h) && gui_.contexts.at(h).display)?3:10);
        if(gui_.bitmaps.contains(h) || h==0xb015)return finish(7);
        if(gui_.regions.contains(h))return finish(8);
        if(gui_.fonts.contains(h) || (h>=0xb00a && h<=0xb00e) || h==0xb010 || h==0xb011)return finish(6);
        if(gui_.brushes.contains(h) || (h>=0xb000 && h<=0xb005) || h==0xb012)return finish(2);
        if(const auto pen=gui_.pen_definitions.find(h);pen!=gui_.pen_definitions.end() && pen->second.extended)return finish(11);
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
        else if(remote){width=remote->width;height=remote->height;}
        else if(const auto bitmap=gui_.bitmaps.find(dc.bitmap);bitmap!=gui_.bitmaps.end()){width=bitmap->second.width;height=bitmap->second.height;}
        else if(dc.display || arg(0)==0x10000000U)return GuiRegion{gui_virtual_screen_bounds(gui_monitors())};
        return width && height?GuiRegion{{0,0,static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)}}:GuiRegion{};
    };
    const auto effective=[&](const GuiDc& value,bool bounded){
        auto region=gui_dc_clip(value);
        if(!region)return visible();return bounded?gui_region_combine(*region,visible(),1):*region;
    };
    const auto update=[&](GuiDc& value){
        gui_refresh_dc_clip(value);
    };
    const auto write_rect=[&](std::uint64_t at,const GuiRect& rect){memory.check(GuestAddress{at},16,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{at+i*4},32,static_cast<std::uint32_t>(rect[i]));};
    const auto device_dimensions=[&]{
        if(dc.virtual_resolution[0]>0)return dc.virtual_resolution;
        // 使用 GetDeviceCaps 的同一份顯示器或印表機尺寸。前兩欄是像素，後兩欄是毫米。
        const auto cap=[&](unsigned index){
            return static_cast<std::int32_t>(nested_service(state,memory,"gdi32.dll","GetDeviceCaps",{arg(0),index}));
        };
        return std::array{cap(8),cap(10),cap(4),cap(6)};
    };
    // 等向映射的兩軸必須代表相同實體長度。虛擬裝置可有非正方形像素。
    const auto adjust_isotropic=[&]{
        if(dc.map_mode!=7)return;
        const auto dimensions=device_dimensions();
        if(std::any_of(dimensions.begin(),dimensions.end(),[](auto n){return n<=0;}))return;
        std::array<double,2> length{};
        for(unsigned axis=0;axis<2;++axis){
            const double pixel=static_cast<double>(dimensions[axis+2])/dimensions[axis];
            length[axis]=std::abs(static_cast<double>(dc.viewport_extent[axis])/dc.window_extent[axis])*pixel;
        }
        if(length[0]==length[1])return;
        const unsigned axis=length[0]>length[1]?0:1;
        const auto extent=dc.viewport_extent[axis];
        const auto scaled=std::round(extent*(length[1-axis]/length[axis]));
        dc.viewport_extent[axis]=scaled!=0.0?static_cast<std::int32_t>(scaled):(extent<0?-1:1);
    };
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
        if(name=="GetTransform"){
            if(!arg(2))return failure(87);
            std::optional<GuiTransform> matrix;
            switch(integer(1)){
            case 0x203:matrix=dc.world;break;
            case 0x304:matrix=gui_page_transform(dc);break;
            case 0x204:matrix=gui_device_transform(dc);break;
            case 0x402:matrix=gui_inverse(gui_device_transform(dc));break;
            default:return failure(87);
            }
            if(!matrix || !gui_valid_transform(*matrix))return failure(87);
            write_transform(arg(2),*matrix);
            return finish(1);
        }
        if(name=="NtGdiComputeXformCoefficients"){
            adjust_isotropic();
            // 其餘係數直接由 DC 的世界矩陣及目前範圍推導，不另存一份快取。
            return finish(gui_valid_transform(gui_device_transform(dc)));
        }
        if(name=="SetWorldTransform" || name=="ModifyWorldTransform" || name=="NtGdiModifyWorldTransform"){
            const auto mode=name=="SetWorldTransform"?4U:static_cast<unsigned>(arg(2));
            const bool native=name=="NtGdiModifyWorldTransform";
            if(dc.graphics_mode!=2 && (!native || mode==4))return failure(87);
            if(mode<1 || mode>4 || (mode!=1 && !arg(1)))return failure(87);
            if(native && mode==1 && arg(1))memory.check(GuestAddress{arg(1)},24,Access::Read);
            auto changed=mode==1?gui_identity:read_transform(arg(1));
            if(mode==2)changed=gui_multiply(changed,dc.world);
            if(mode==3)changed=gui_multiply(dc.world,changed);
            if(!gui_valid_transform(changed) || ((!native || mode==4) && !gui_inverse(changed)))return failure(87);
            dc.world=changed;
            return finish(1);
        }
        if(name=="GetMapMode")return finish(dc.map_mode);
        if(name=="SetMapMode"){
            const auto mode=static_cast<unsigned>(arg(1));
            if(mode<1 || mode>8)return failure(87);
            const auto previous=dc.map_mode;
            if(mode==1)dc.window_extent=dc.viewport_extent={1,1};
            else if(mode<=7 && !(mode==7 && previous==7)){
                constexpr std::array<int,8> units{0,1,254,2540,100,1000,1440,254};
                const auto dimensions=device_dimensions();
                if(std::any_of(dimensions.begin(),dimensions.end(),[](auto n){return n<=0;}))return failure(87);
                std::array<std::int32_t,2> extent{};
                for(unsigned axis=0;axis<2;++axis){
                    const auto value=(static_cast<std::int64_t>(dimensions[axis+2])*units[mode]*10+127)/254;
                    if(value==0 || value>INT32_MAX)return failure(87);
                    extent[axis]=static_cast<std::int32_t>(value);
                }
                dc.window_extent=extent;
                dc.viewport_extent={dimensions[0],-dimensions[1]};
            }
            dc.map_mode=mode;
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
            if(output){
                memory.check(GuestAddress{output},8,Access::Write);
                for(unsigned axis=0;axis<2;++axis)
                    memory.store(GuestAddress{output+axis*4},32,static_cast<std::uint32_t>(value[axis]));
            }
            if(get || (extent && dc.map_mode<7))return finish(1);
            auto changed=value;
            for(unsigned axis=0;axis<2;++axis){
                std::int64_t next=integer(1+axis);
                if(scale){
                    const auto numerator=integer(1+axis*2),denominator=integer(2+axis*2);
                    if(!numerator || !denominator)return failure(87);
                    next=static_cast<std::int64_t>(value[axis])*numerator/denominator;
                    if(!next)next=1;
                }else if(offset)next+=value[axis];
                if(next<INT32_MIN || next>INT32_MAX || (extent && !next))return failure(87);
                changed[axis]=static_cast<std::int32_t>(next);
            }
            value=changed;
            if(extent)adjust_isotropic();
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
            auto restored=saved[static_cast<std::size_t>(index)];restored.copy_count=dc.copy_count;restored.paint_clip=dc.paint_clip;restored.acquisition_clip=dc.acquisition_clip;restored.acquisition_region=dc.acquisition_region;update(restored);dc=std::move(restored);saved.resize(static_cast<std::size_t>(index));
            if(window!=gui_.windows.end()){window->second.text_color=dc.color;window->second.background_mode=dc.mode;}
            gui_.dc_objects[arg(0)]=dc.font;return finish(1);
        }
        if(name=="GetCurrentObject" || name=="NtGdiGetDCObject"){
            unsigned type=static_cast<unsigned>(arg(1));
            if(name=="NtGdiGetDCObject"){
                // NT 使用 GDI handle 的型別位元；Win32 入口使用 OBJ_*。
                switch(type){
                case 0x300000:case 0x500000:type=1;break;
                case 0x100000:type=2;break;
                case 0x080000:type=5;break;
                case 0x0a0000:type=6;break;
                case 0x050000:type=7;break;
                case 0x090000:type=14;break;
                default:return failure(87);
                }
            }
            switch(type){
            case 1:case 11:return finish(dc.pen);
            case 2:return finish(dc.brush);
            case 5:return finish(dc.palette);
            case 6:return finish(dc.font);
            case 7:return finish(dc.bitmap?dc.bitmap:0xb015);
            case 14:return finish(nested_service(state,memory,"gdi32.dll","GetColorSpace",{arg(0)}));
            default:return failure(87);
            }
        }
        if(name=="GetClipRgn" || name=="GetMetaRgn" || name=="GetRandomRgn"){
            const auto target=gui_.regions.find(arg(1));if(target==gui_.regions.end())return failure(6);
            if(name=="GetRandomRgn"){
                const auto selector=integer(2);
                if(native_region && selector>=1 && selector<=3){
                    const auto requested=selector==1?dc.application_clip:selector==2?dc.meta_clip:gui_dc_clip(dc,false);
                    if(!requested)return finish(0);
                    target->second=*requested;
                    return finish(1);
                }
                if(selector!=4)return failure(87);
                auto region=visible();
                if(dc.acquisition_clip)region=gui_region_combine(region,*dc.acquisition_clip,1);
                if(dc.paint_clip)region=gui_region_combine(region,*dc.paint_clip,1);
                if(window!=gui_.windows.end() || remote){const auto origin=remote?std::array<std::int64_t,2>{remote->client_x,remote->client_y}:gui_client_origin(gui_,window->first);for(auto& r:region)for(unsigned i=0;i<4;++i){const auto value=static_cast<std::int64_t>(r[i])+origin[i%2];if(value<INT32_MIN || value>INT32_MAX)return failure(87);r[i]=static_cast<std::int32_t>(value);}}
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
        if(name=="ExcludeUpdateRgn"){
            const auto target=gui_.windows.find(arg(1));if(target==gui_.windows.end())return failure(1400);
            auto region=gui_update_region(target->second);
            if(window!=gui_.windows.end() && window->first!=target->first){
                const auto source_origin=gui_client_origin(gui_,target->first),dc_origin=gui_client_origin(gui_,window->first);
                for(auto& r:region)for(unsigned i=0;i<4;++i){const auto value=static_cast<std::int64_t>(r[i])+source_origin[i%2]-dc_origin[i%2];if(value<INT32_MIN || value>INT32_MAX)return failure(87);r[i]=static_cast<std::int32_t>(value);}
            }
            changed.application_clip=gui_region_combine(dc.application_clip.value_or(visible()),region,4);
        }
        else if(name=="SetMetaRgn"){changed.meta_clip=gui_dc_clip(dc,false);changed.application_clip.reset();}
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
    }catch(const MemoryFault&){
        if(name=="RectVisible" || name=="GetClipBox")return finish(0);
        if(name=="GetTransform" || name.ends_with("WorldTransform") || name.ends_with("ExtEx"))return failure(998);
        throw;
    }catch(const std::bad_alloc&){return failure(8);}catch(const std::length_error&){return failure(8);}
}
}
