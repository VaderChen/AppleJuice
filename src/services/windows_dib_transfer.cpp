#include "x64arm/windows_services.hpp"
#include "x64arm/windows_palette.hpp"
#include "x64arm/windows_dib_rows.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
namespace {
struct DibError {std::uint32_t code;};
struct DibFormat {
    std::uint32_t header{},width{},height{},bpp{},compression{},stride{},table_offset{},table_count{},table_entry{4};
    bool top{};
    std::array<std::uint32_t,4> masks{};
    std::vector<std::uint32_t> palette;
};
DibFormat dib_format(GuestMemory& memory,std::uint64_t address,std::uint32_t usage,bool input){
    if(!address || usage>1)throw DibError{87};if(usage)throw DibError{50};
    const auto field=[&](unsigned offset,std::uint8_t bits=32){return static_cast<std::uint32_t>(memory.load(GuestAddress{address+offset},bits));};
    DibFormat f;f.header=field(0);const bool core=f.header==12;
    if(!core && f.header!=40 && f.header!=52 && f.header!=56 && f.header!=108 && f.header!=124)throw DibError{50};
    memory.check(GuestAddress{address},f.header,Access::Read);
    const auto width=core?static_cast<std::int32_t>(field(4,16)):std::bit_cast<std::int32_t>(field(4));
    const auto height=core?static_cast<std::int32_t>(field(6,16)):std::bit_cast<std::int32_t>(field(8));
    if(width<=0 || width>8192 || !height || height==INT32_MIN || field(core?8:12,16)!=1)throw DibError{87};
    f.width=static_cast<std::uint32_t>(width);f.top=height<0;f.height=static_cast<std::uint32_t>(f.top?-height:height);
    if(f.height>8192 || std::uint64_t{f.width}*f.height>16*1024*1024)throw DibError{8};
    f.bpp=field(core?10:14,16);f.compression=core?0:field(16);
    if(f.bpp!=1 && f.bpp!=4 && f.bpp!=8 && f.bpp!=16 && f.bpp!=24 && f.bpp!=32)throw DibError{50};
    if(f.compression!=0 && f.compression!=3)throw DibError{50};
    if(f.compression==3 && f.bpp!=16 && f.bpp!=32)throw DibError{87};
    f.masks=f.bpp==16?std::array<std::uint32_t,4>{0x7c00,0x3e0,0x1f,0}:std::array<std::uint32_t,4>{0xff0000,0xff00,0xff,f.bpp==32?0xff000000U:0U};
    if(f.compression==3){for(unsigned n=0;n<3;++n)f.masks[n]=field(40+n*4);f.masks[3]=f.header>=56?field(52):0;
        if(!valid_dib_masks(f.masks,f.bpp))throw DibError{87};}
    f.stride=(f.width*f.bpp+31)/32*4;f.table_offset=f.header+(f.header==40 && f.compression==3?12:0);f.table_entry=core?3:4;
    f.table_count=core?0:field(32);if(!f.table_count && f.bpp<=8)f.table_count=1U<<f.bpp;
    if(f.table_count>256 || (f.bpp<=8 && f.table_count>(1U<<f.bpp)))throw DibError{87};
    if(input && f.bpp<=8)for(unsigned n=0;n<f.table_count;++n){const auto at=f.table_offset+n*f.table_entry;
        f.palette.push_back(field(at+2,8)|(field(at+1,8)<<8)|(field(at,8)<<16));}
    return f;
}
GuiBitmap dib_band(GuestMemory& memory,std::uint64_t address,const DibFormat& f,std::uint32_t lines){
    if(!address)throw DibError{87};std::vector<std::uint8_t> bytes(static_cast<std::size_t>(f.stride)*lines);memory.read(GuestAddress{address},bytes);
    GuiBitmap bitmap{f.width,lines,std::vector<std::uint32_t>(static_cast<std::size_t>(f.width)*lines)};
    const DibRowCodec codec(f.bpp,f.masks,f.palette);
    for(std::uint32_t y=0;y<lines;++y){const auto row=static_cast<std::size_t>(f.top?y:lines-y-1)*f.stride;
        if(!codec.decode(std::span(bytes).subspan(row,f.stride),std::span(bitmap.pixels).subspan(static_cast<std::size_t>(y)*f.width,f.width)))throw DibError{87};
    }
    return bitmap;
}
std::int32_t coordinate(std::int64_t value){if(value<INT32_MIN || value>INT32_MAX)throw DibError{87};return static_cast<std::int32_t>(value);}
}
std::span<const WindowsServiceDescriptor> dib_transfer_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","GetDIBits",7},{"gdi32.dll","SetDIBits",7},
        {"gdi32.dll","StretchDIBits",13},{"gdi32.dll","SetDIBitsToDevice",12}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_dib_transfer(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};const auto si=[&](unsigned n){return std::bit_cast<std::int32_t>(d(n));};std::uint64_t result=0;
    try {
        const bool get=symbol=="GetDIBits",set=symbol=="SetDIBits";
        if(get || set){
            const auto found=gui_.bitmaps.find(arg(1));if(found==gui_.bitmaps.end())throw DibError{6};auto& bitmap=found->second;
            if(get && !gui_.contexts.contains(arg(0)) && !gui_.dc_objects.contains(arg(0)))throw DibError{6};
            if(std::any_of(gui_.contexts.begin(),gui_.contexts.end(),[&](const auto& item){return item.second.bitmap==arg(1);}))throw DibError{87};
            if(!arg(5) || d(6)>1)throw DibError{87};if(d(6))throw DibError{50};
            const auto header=memory.load(GuestAddress{arg(5)},32);const bool core=header==12;
            if(get && !arg(4) && (header==12 || header==40 || header==108 || header==124) && !memory.load(GuestAddress{arg(5)+(core?10:14)},16)){
                // bit count 為零時只查結構，不寫入呼叫端未配置的 palette。
                memory.check(GuestAddress{arg(5)},header,Access::Write);memory.write(GuestAddress{arg(5)},std::vector<std::uint8_t>(static_cast<std::size_t>(header)));
                const auto bpp=bitmap.bits_per_pixel;memory.store(GuestAddress{arg(5)},32,header);
                memory.store(GuestAddress{arg(5)+4},core?16:32,bitmap.width);memory.store(GuestAddress{arg(5)+(core?6:8)},core?16:32,bitmap.height);
                memory.store(GuestAddress{arg(5)+(core?8:12)},16,1);memory.store(GuestAddress{arg(5)+(core?10:14)},16,bpp);
                if(!core)memory.store(GuestAddress{arg(5)+20},32,((bitmap.width*bpp+31)/32*4)*bitmap.height);result=1;
            }else{
                auto f=dib_format(memory,arg(5),d(6),set);
                if(f.width!=bitmap.width || f.height!=bitmap.height)throw DibError{87};
                const auto start=d(2),lines=start<f.height?std::min(d(3),f.height-start):0;
                if(get){
                    if(f.bpp<=8){f.palette=f.bpp==bitmap.bits_per_pixel && !bitmap.dib_palette.empty()?bitmap.dib_palette:gui_dib_palette(f.bpp);
                        f.table_count=static_cast<std::uint32_t>(f.palette.size());}
                    if(!core)memory.check(GuestAddress{arg(5)+20},20,Access::Write);
                    if(!f.palette.empty())memory.check(GuestAddress{arg(5)+f.table_offset},f.palette.size()*f.table_entry,Access::Write);
                    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(f.stride)*lines);
                    const DibRowCodec codec(f.bpp,f.masks,f.palette);
                    if(arg(4) && lines){memory.check(GuestAddress{arg(4)},bytes.size(),Access::Write);sync_raster_bitmap(memory,arg(1));
                        for(unsigned row=0;row<lines;++row){const auto y=f.top?start+row:f.height-start-row-1;
                            if(!codec.encode(std::span(bitmap.pixels).subspan(static_cast<std::size_t>(y)*f.width,f.width),std::span(bytes).subspan(static_cast<std::size_t>(row)*f.stride,f.stride)))throw DibError{87};
                        }
                        memory.write(GuestAddress{arg(4)},bytes);
                    }
                    if(!core){memory.store(GuestAddress{arg(5)+20},32,f.stride*f.height);memory.store(GuestAddress{arg(5)+32},32,f.bpp<=8?f.table_count:0);}
                    for(unsigned n=0;n<f.palette.size();++n){const auto color=f.palette[n];const auto at=arg(5)+f.table_offset+n*f.table_entry;
                        for(unsigned c=0;c<f.table_entry;++c)memory.store(GuestAddress{at+c},8,c<3?(color>>((2-c)*8))&255:0);}
                    result=arg(4)?lines:1;
                }else if(lines){
                    const auto band=dib_band(memory,arg(4),f,lines);sync_raster_bitmap(memory,arg(1));const auto first=f.top?start:f.height-start-lines;
                    std::copy(band.pixels.begin(),band.pixels.end(),bitmap.pixels.begin()+static_cast<std::ptrdiff_t>(first*f.width));
                    // 寫入時解除共享資源快取；下一次 Metal 使用重新上傳，DIB section 同步客體 backing。
                    rasters_.erase(arg(1));raster_shared_.erase(arg(1));bitmap.resource_id=0;bitmap.raw_alpha=true;export_dib_bitmap(memory,arg(1));result=lines;
                }
            }
        }else{
            auto dc=gui_.contexts.find(arg(0));if(dc==gui_.contexts.end())throw DibError{6};
            if(gui_.emf_recorders.contains(arg(0)))throw DibError{50};
            const bool stretch=symbol=="StretchDIBits";
            const auto f=dib_format(memory,arg(10),d(11),true);const auto start=stretch?0:d(7),lines=stretch?f.height:start<f.height?std::min(d(8),f.height-start):0;
            const auto width=si(3),height=si(4),sw=stretch?si(7):width,sh=stretch?si(8):height;
            if(!stretch && (width<0 || height<0))throw DibError{87};
            if(width && height && sw && sh && lines){
                const auto band=dib_band(memory,arg(9),f,lines);
                const auto sx=si(5),sy=si(6);std::int64_t y0=f.top?sy:static_cast<std::int64_t>(f.height)-sy-sh,y1=y0+sh;
                const auto band_start=f.top?start:f.height-start-lines;
                auto dx=static_cast<std::int64_t>(si(1)),dy=static_cast<std::int64_t>(si(2));auto dh=static_cast<std::int64_t>(height);
                if(!stretch){const auto top=std::max<std::int64_t>(y0,band_start),bottom=std::min<std::int64_t>(y1,band_start+lines);dy+=top-y0;y0=top;y1=bottom;dh=bottom-top;}
                if(stretch || dh>0){
                    const auto mode=stretch?d(12):0x00cc0020U;if(mode&0x40000000U)throw DibError{50};
                    RasterBlit options;options.operation=(mode>>16)&255U;const bool pattern_used=((options.operation^(options.operation>>4))&15U)!=0;
                    const bool source_used=((options.operation^(options.operation>>2))&0x33U)!=0;
                    const auto handle=dc->second.brush;auto& brush=options.brush;
                    brush.color=handle==0xb012?dc->second.dc_brush_color:gui_.brushes.contains(handle)?gui_.brushes.at(handle):handle==0xb000?0xffffff:handle==0xb001?0xc0c0c0:handle==0xb002?0x808080:handle==0xb003?0x404040:0;
                    brush.hatch=gui_.brush_hatches.contains(handle)?gui_.brush_hatches.at(handle):-1;brush.pattern_opaque=dc->second.mode==2;brush.pattern_background=dc->second.background;brush.pattern_color=dc->second.color;brush.brush_origin=dc->second.brush_origin;
                    gui_resolve_draw_colors(gui_,dc->second,brush);
                    const auto source_end=static_cast<std::int64_t>(sx)+sw;
                    const auto copied_rows=std::min<std::int64_t>(std::max(y0,y1),band_start+lines)-std::max<std::int64_t>(std::min(y0,y1),band_start);
                    const bool source_visible=copied_rows>0 && std::min<std::int64_t>(std::max<std::int64_t>(sx,source_end),f.width)>std::max<std::int64_t>(std::min<std::int64_t>(sx,source_end),0);
                    if((!pattern_used || handle!=0xb005) && (!source_used || source_visible)){
                        const GuiRect dest{coordinate(dx),coordinate(dy),coordinate(dx+width),coordinate(dy+dh)};
                        const GuiRect source{sx,coordinate(y0-band_start),coordinate(static_cast<std::int64_t>(sx)+sw),coordinate(y1-band_start)};
                        const auto dst=raster_surface(memory,arg(0));if(!dst)throw DibError{50};const auto src=make_hardware_raster(band.width,band.height,band.pixels);
                        std::shared_ptr<RasterSurface> pattern;if(pattern_used && gui_.brush_patterns.contains(handle)){const auto& image=gui_.brush_patterns.at(handle);brush.pattern_monochrome=image.bits_per_pixel==1;pattern=make_hardware_raster(image.width,image.height,image.pixels);}
                        auto target=dc->second;if(!stretch)target.stretch_mode=3;
                        dst->blit(*src,dest,source,target,UINT32_MAX,options,pattern.get());
                        result=stretch?static_cast<std::uint32_t>(source_used?(sh<0?-copied_rows:copied_rows):sh):static_cast<std::uint32_t>(dh);
                    }
                }
            }
        }
    }catch(const DibError& error){last_error_=error.code;
        if((symbol=="StretchDIBits" || symbol=="SetDIBitsToDevice") && error.code==50 && arg(10)){
            try{const auto compression=memory.load(GuestAddress{arg(10)+16},32);if(compression==4 || compression==5)result=UINT32_MAX;}catch(const MemoryFault&){}
        }
    }catch(const MemoryFault&){last_error_=998;}catch(const std::bad_alloc&){last_error_=8;}
    arg.return_to(state,result);return std::nullopt;
}
}
