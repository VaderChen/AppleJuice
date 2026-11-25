#include "x64arm/windows_services.hpp"
#include "x64arm/windows_palette.hpp"
#include "x64arm/windows_dib_rows.hpp"
#include "gdi_ddb.hpp"
#include "windows_icm_store.hpp"
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
DibFormat dib_format(GuestMemory& memory,std::uint64_t address,std::uint32_t usage,bool input,std::uint32_t capacity=UINT32_MAX,
                     std::span<const std::uint32_t> logical={},bool brush_indices=false){
    if(!address || usage>(brush_indices?2U:1U) || capacity<4)throw DibError{87};
    if(usage && logical.empty() && !brush_indices)throw DibError{50};
    const auto field=[&](unsigned offset,std::uint8_t bits=32){return static_cast<std::uint32_t>(memory.load(GuestAddress{address+offset},bits));};
    DibFormat f;f.header=field(0);const bool core=f.header==12;
    if(!core && f.header!=40 && f.header!=52 && f.header!=56 && f.header!=108 && f.header!=124)throw DibError{50};
    if(f.header>capacity)throw DibError{87};
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
    if(f.compression==3){if(capacity<52)throw DibError{87};for(unsigned n=0;n<3;++n)f.masks[n]=field(40+n*4);f.masks[3]=f.header>=56?field(52):0;
        if(!valid_dib_masks(f.masks,f.bpp))throw DibError{87};}
    f.stride=(f.width*f.bpp+31)/32*4;f.table_offset=f.header+(f.header==40 && f.compression==3?12:0);f.table_entry=usage?2:core?3:4;
    f.table_count=core?0:field(32);if(!f.table_count && f.bpp<=8)f.table_count=1U<<f.bpp;
    if(usage==2){
        f.table_count=0;
        if(f.bpp<=8)for(unsigned index=0;index<(1U<<f.bpp);++index)f.palette.push_back(index);
    }
    if(f.table_count>256 || (f.bpp<=8 && f.table_count>(1U<<f.bpp)))throw DibError{87};
    if(f.table_offset>capacity || f.table_count>(capacity-f.table_offset)/f.table_entry)throw DibError{87};
    if(input && f.bpp<=8)for(unsigned n=0;n<f.table_count;++n){const auto at=f.table_offset+n*f.table_entry;
        if(usage){const auto index=field(at,16);f.palette.push_back(brush_indices?index:index<logical.size()?gui_palette_color(logical[index]):0);continue;}
        f.palette.push_back(field(at+2,8)|(field(at+1,8)<<8)|(field(at,8)<<16));}
    return f;
}
GuiBitmap decode_dib_band(std::span<const std::uint8_t> bytes,const DibFormat& f,std::uint32_t lines){
    GuiBitmap bitmap{f.width,lines,std::vector<std::uint32_t>(static_cast<std::size_t>(f.width)*lines)};
    const DibRowCodec codec(f.bpp,f.masks,f.palette);
    for(std::uint32_t y=0;y<lines;++y){const auto row=static_cast<std::size_t>(f.top?y:lines-y-1)*f.stride;
        if(!codec.decode(std::span(bytes).subspan(row,f.stride),std::span(bitmap.pixels).subspan(static_cast<std::size_t>(y)*f.width,f.width)))throw DibError{87};
    }
    return bitmap;
}
GuiBitmap dib_band(GuestMemory& memory,std::uint64_t address,const DibFormat& f,std::uint32_t lines){
    if(!address)throw DibError{87};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(f.stride)*lines);
    memory.read(GuestAddress{address},bytes);
    return decode_dib_band(bytes,f,lines);
}

// ICM 查詢輸出 BITMAPINFO 與由下往上的獨立 DIB；只請求大小時無須轉碼。
void query_brush_dib(GuestMemory& memory,const GuiBitmap& pattern,std::uint32_t usage,
                     std::uint64_t info,std::uint64_t bits,std::uint64_t size_out,std::uint64_t usage_out){
    const auto bpp=pattern.bits_per_pixel;
    const auto stride=(pattern.width*bpp+31)/32*4;
    const auto size=stride*pattern.height;
    const auto palette=bpp<=8 && pattern.dib_palette.empty()?gui_dib_palette(bpp):pattern.dib_palette;
    const auto count=usage==2?0U:static_cast<unsigned>(palette.size());
    const bool fields=pattern.dib_compression==3;
    const auto table=fields?52U:40U;
    if(bits && size_out && memory.load(GuestAddress{size_out},32)<size){
        memory.store(GuestAddress{size_out},32,size);throw DibError{122};
    }
    if(info)memory.check(GuestAddress{info},table+count*(usage?2:4),Access::Write);
    if(bits)memory.check(GuestAddress{bits},size,Access::Write);
    if(size_out)memory.check(GuestAddress{size_out},4,Access::Write);
    if(usage_out)memory.check(GuestAddress{usage_out},4,Access::Write);
    std::vector<std::uint8_t> rows;
    if(bits){
        rows.resize(size);
        const DibRowCodec codec(bpp,pattern.dib_masks,palette);
        for(unsigned y=0;y<pattern.height;++y){
            auto row=std::span(rows).subspan(static_cast<std::size_t>(y)*stride,stride);
            if(pattern.dib_address){
                const auto source=pattern.signed_height<0?pattern.height-y-1:y;
                memory.read(GuestAddress{pattern.dib_address+static_cast<std::size_t>(source)*stride},row);
            }else if(!codec.encode(std::span(pattern.pixels).subspan(static_cast<std::size_t>(pattern.height-y-1)*pattern.width,pattern.width),row))throw DibError{87};
        }
    }
    if(info){
        std::vector<std::uint8_t> header(table+count*(usage?2:4));
        const auto put=[&](unsigned at,unsigned value,unsigned length=4){
            for(unsigned i=0;i<length;++i)header[at+i]=static_cast<std::uint8_t>(value>>(8*i));
        };
        put(0,40);put(4,pattern.width);put(8,pattern.height);put(12,1,2);put(14,bpp,2);
        put(16,pattern.dib_compression);put(20,size);put(32,count);
        if(fields)for(unsigned i=0;i<3;++i)put(40+4*i,pattern.dib_masks[i]);
        for(unsigned i=0;i<count;++i){
            const auto color=palette[i];
            if(usage)put(table+2*i,color,2);
            else put(table+4*i,((color&255U)<<16)|(color&0xff00U)|((color>>16)&255U));
        }
        memory.write(GuestAddress{info},header);
    }
    if(bits)memory.write(GuestAddress{bits},rows);
    if(size_out)memory.store(GuestAddress{size_out},32,size);
    if(usage_out)memory.store(GuestAddress{usage_out},32,usage);
}
std::int32_t coordinate(std::int64_t value){if(value<INT32_MIN || value>INT32_MAX)throw DibError{87};return static_cast<std::int32_t>(value);}
}
std::span<const WindowsServiceDescriptor> dib_transfer_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","CreateDIBitmap",6},{"win32u.dll","NtGdiCreateDIBitmapInternal",11,0,0,0x3ce},
        {"win32u.dll","NtGdiGetDIBitsInternal",9,0,0,0x1cc},{"win32u.dll","NtGdiStretchDIBitsInternal",16,0,0,0x79fe},
        {"win32u.dll","NtGdiSetDIBitsToDeviceInternal",16,0,0,0x79fe},
        {"gdi32.dll","GetDIBits",7},{"gdi32.dll","SetDIBits",7},
        {"gdi32.dll","StretchDIBits",13},{"gdi32.dll","SetDIBitsToDevice",12},
        {"win32u.dll","NtGdiCreateDIBBrush",6,0,0,0x1e},{"win32u.dll","NtGdiIcmBrushInfo",8,0,0,0x80},
        {"gdi32.dll","CreateDIBPatternBrushPt",2,0,0,2},{"gdi32.dll","CreateDIBPatternBrush",2,0,0,2}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_dib_transfer(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);auto symbol=service.symbol;
    const bool native=service.family=="win32u.dll";
    if(symbol=="NtGdiGetDIBitsInternal")symbol="GetDIBits";
    if(symbol=="NtGdiStretchDIBitsInternal")symbol="StretchDIBits";
    if(symbol=="NtGdiSetDIBitsToDeviceInternal")symbol="SetDIBitsToDevice";
    const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};const auto si=[&](unsigned n){return std::bit_cast<std::int32_t>(d(n));};std::uint64_t result=0;
    try {
        const auto logical_palette=[&]{
            const auto dc=gui_.contexts.find(arg(0));
            return gui_palette_entries(gui_,dc==gui_.contexts.end()?0xb00f:dc->second.palette);
        };
        if(symbol=="NtGdiCreateDIBitmapInternal" || symbol=="CreateDIBitmap"){
            const auto init=d(native?3:2),usage=d(native?6:5);
            const auto info=arg(native?5:4),bits=arg(native?4:3);
            const auto dc=gui_.contexts.find(arg(0));
            const bool device=arg(0)>=0x10000000 && gui_.windows.contains(arg(0)-0x10000000);
            if(arg(0) && !device && dc==gui_.contexts.end())throw DibError{6};
            if((init&~6U) || usage>1)throw DibError{87};
            std::int32_t width{},signed_height{};
            if(native){width=si(1);signed_height=si(2);}
            else{
                const auto header=arg(1);if(!header)throw DibError{87};
                const auto size=memory.load(GuestAddress{header},32);
                if(size!=12 && size<40)throw DibError{87};
                const std::uint8_t field_bits=size==12?16:32;
                width=static_cast<std::int32_t>(memory.load(GuestAddress{header+4},field_bits));
                signed_height=static_cast<std::int32_t>(memory.load(GuestAddress{header+(size==12?6:8)},field_bits));
            }
            if(width<0 || signed_height==INT32_MIN)throw DibError{87};
            const bool empty=!width || !signed_height;
            if(native && empty)throw DibError{87};
            const auto columns=empty?1U:static_cast<unsigned>(width);
            const auto rows=empty?1U:static_cast<unsigned>(std::abs(signed_height));
            if(!gdi_bitmap_dimensions_fit(columns,rows))throw DibError{8};
            const bool initialize=!empty && (init&4U) && bits;
            const bool use_format=!empty && (init&2U);
            DibFormat format;
            if(initialize || use_format){
                format=dib_format(memory,info,usage,true,native?d(7):UINT32_MAX,logical_palette());
                if(format.width!=columns || format.height!=rows)throw DibError{87};
                if(initialize && native && std::uint64_t{format.stride}*rows>d(8))throw DibError{87};
            }
            const auto selected=dc==gui_.contexts.end()?gui_.bitmaps.end():gui_.bitmaps.find(dc->second.bitmap);
            const bool display=device || (dc!=gui_.contexts.end() && (dc->second.display || is_printer_dc(arg(0))));
            const auto depth=empty?1U:use_format?format.bpp:selected!=gui_.bitmaps.end()?selected->second.bits_per_pixel:display?32U:1U;
            auto bitmap=gdi_ddb(columns,rows,static_cast<std::uint16_t>(depth));
            if(use_format){
                bitmap.dib_masks=format.masks;bitmap.dib_palette=format.palette;
            }else if(!empty && selected!=gui_.bitmaps.end()){
                bitmap.dib_masks=selected->second.dib_masks;bitmap.dib_palette=selected->second.dib_palette;
            }
            if(initialize){
                auto source=dib_band(memory,bits,format,rows);
                bitmap.pixels=std::move(source.pixels);
                // DDB 的像素須可由目的地色深表示；沿用位元遮罩及調色盤的列轉碼。
                if(depth<=16){
                    const DibRowCodec codec(depth,bitmap.dib_masks,bitmap.dib_palette);
                    std::vector<std::uint8_t> row(bitmap.stride);
                    for(unsigned y=0;y<rows;++y){
                        auto pixels=std::span(bitmap.pixels).subspan(static_cast<std::size_t>(y)*columns,columns);
                        if(!codec.encode(pixels,row) || !codec.decode(row,pixels))throw DibError{87};
                    }
                }
            }
            gui_.bitmaps.emplace(gui_.next_object,std::move(bitmap));arg.return_to(state,gui_.next_object++);
            return std::nullopt;
        }
        if(symbol=="NtGdiIcmBrushInfo"){
            const auto pattern=gui_.brush_patterns.find(arg(1));
            if(pattern!=gui_.brush_patterns.end() && d(7)<=1){
                const auto dib=gui_.brush_dib_patterns.find(arg(1));
                const auto usage=dib==gui_.brush_dib_patterns.end()?0U:static_cast<unsigned>(dib->second[0]);
                if(arg(6))memory.check(GuestAddress{arg(6)},4,Access::Write);
                if(!d(7)){
                    query_brush_dib(memory,pattern->second,usage,arg(2),arg(3),arg(4),arg(5));
                    if(arg(6))memory.store(GuestAddress{arg(6)},32,gdi_translated_brush(arg(1),arg(0))?1:0);
                }else{
                    const auto dc=gui_.contexts.find(arg(0));
                    if(dc==gui_.contexts.end())throw DibError{6};
                    if(!arg(3) || !arg(4))throw DibError{87};
                    if(arg(5))memory.check(GuestAddress{arg(5)},4,Access::Write);
                    const auto format=dib_format(memory,arg(2),0,true);
                    if(format.width!=pattern->second.width || format.height!=pattern->second.height)throw DibError{87};
                    const auto length=format.stride*format.height;
                    if(memory.load(GuestAddress{arg(4)},32)<length)throw DibError{122};
                    auto converted=dib_band(memory,arg(3),format,format.height);
                    auto surface=make_hardware_raster(format.width,format.height,converted.pixels);
                    icm_profiles().brush_translations.insert_or_assign({arg(1),arg(0)},
                        IcmProfiles::BrushTranslation{dc->second.color_space,dc->second.icm_profile,std::move(surface)});
                    if(arg(5))memory.store(GuestAddress{arg(5)},32,usage);
                    if(arg(6))memory.store(GuestAddress{arg(6)},32,1);
                }
                result=1;
            }
            arg.return_to(state,result);return std::nullopt;
        }
        if(symbol=="NtGdiCreateDIBBrush" || symbol=="CreateDIBPatternBrushPt" || symbol=="CreateDIBPatternBrush"){
            const bool global=symbol=="CreateDIBPatternBrush";auto address=arg(0);
            if(global){address=nested_service(state,memory,"kernel32.dll","GlobalLock",{arg(0)});if(!address)throw DibError{87};}
            struct Unlock{WindowsServices& self;GuestState& s;GuestMemory& m;std::uint64_t handle;~Unlock(){if(handle)(void)self.nested_service(s,m,"kernel32.dll","GlobalUnlock",{handle});}} unlock{*this,state,memory,global?arg(0):0};
            const auto usage=d(1);
            if(!native && usage>1)throw DibError{87};
            const auto capacity=native?d(2):UINT32_MAX;
            const auto format=dib_format(memory,address,usage,true,capacity,{},true);
            const auto offset=format.table_offset+format.table_count*format.table_entry;
            const auto length=static_cast<std::size_t>(format.stride)*format.height;
            if(length>capacity-offset)throw DibError{87};
            std::vector<std::uint8_t> bytes(length);
            memory.read(GuestAddress{address+offset},bytes);
            auto pattern=decode_dib_band(bytes,format,format.height);
            pattern.bits_per_pixel=static_cast<std::uint16_t>(format.bpp);pattern.stride=format.stride;
            pattern.signed_height=format.top?-static_cast<int>(format.height):static_cast<int>(format.height);
            pattern.dib_palette=format.palette;pattern.dib_compression=format.compression;pattern.dib_masks=format.masks;
            result=create_pattern_brush(state,memory,std::move(pattern),bytes,native?arg(5):arg(0),usage);
            arg.return_to(state,result);return std::nullopt;
        }
        const bool get=symbol=="GetDIBits",set=symbol=="SetDIBits";
        if(get || set){
            const auto found=gui_.bitmaps.find(arg(1));if(found==gui_.bitmaps.end())throw DibError{6};auto& bitmap=found->second;
            if(get && !gui_.contexts.contains(arg(0)) && !gui_.dc_objects.contains(arg(0)))throw DibError{6};
            // SetDIBits 仍要求點陣圖未選入 DC。GetDIBits 的文件也這麼寫，但 Windows 實際上照常讀回（舊程式常在點陣圖選入記憶體 DC、
            // 畫完文字後直接 GetDIBits 取回圖層；三國志 VIII 的動態文字因此全部空白），讀取不設此限。
            if(set && std::any_of(gui_.contexts.begin(),gui_.contexts.end(),[&](const auto& item){return item.second.bitmap==arg(1);}))throw DibError{87};
            const auto info_limit=native && d(8)?d(8):UINT32_MAX;
            const auto bits_limit=native && d(7)?d(7):UINT32_MAX;
            if(!arg(5) || d(6)>1 || info_limit<4)throw DibError{87};
            const auto header=memory.load(GuestAddress{arg(5)},32);const bool core=header==12;
            if(header>info_limit)throw DibError{87};
            if(get && !arg(4) && (header==12 || header==40 || header==52 || header==56 || header==108 || header==124) && !memory.load(GuestAddress{arg(5)+(core?10:14)},16)){
                // bit count 為零時只查結構，不寫入呼叫端未配置的 palette。
                memory.check(GuestAddress{arg(5)},header,Access::Write);memory.write(GuestAddress{arg(5)},std::vector<std::uint8_t>(static_cast<std::size_t>(header)));
                const auto bpp=bitmap.bits_per_pixel;memory.store(GuestAddress{arg(5)},32,header);
                memory.store(GuestAddress{arg(5)+4},core?16:32,bitmap.width);memory.store(GuestAddress{arg(5)+(core?6:8)},core?16:32,bitmap.height);
                memory.store(GuestAddress{arg(5)+(core?8:12)},16,1);memory.store(GuestAddress{arg(5)+(core?10:14)},16,bpp);
                if(!core)memory.store(GuestAddress{arg(5)+20},32,((bitmap.width*bpp+31)/32*4)*bitmap.height);result=1;
            }else{
                auto f=dib_format(memory,arg(5),d(6),set,info_limit,logical_palette());
                if(f.width!=bitmap.width || f.height!=bitmap.height)throw DibError{87};
                const auto start=d(2),lines=start<f.height?std::min(d(3),f.height-start):0;
                if(get){
                    if(f.bpp<=8){
                        if(d(6)){
                            for(auto entry:logical_palette())f.palette.push_back(gui_palette_color(entry));
                        }else f.palette=f.bpp==bitmap.bits_per_pixel && !bitmap.dib_palette.empty()?bitmap.dib_palette:gui_dib_palette(f.bpp);
                        f.palette.resize(std::min<std::size_t>(f.palette.size(),f.table_count));
                        f.table_count=static_cast<std::uint32_t>(f.palette.size());}
                    if(!core)memory.check(GuestAddress{arg(5)+20},20,Access::Write);
                    if(!f.palette.empty())memory.check(GuestAddress{arg(5)+f.table_offset},f.palette.size()*f.table_entry,Access::Write);
                    if(arg(4) && std::uint64_t{f.stride}*lines>bits_limit)throw DibError{87};
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
                        if(d(6)){memory.store(GuestAddress{at},16,n);continue;}
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
            const auto info_limit=native && d(13)?d(13):UINT32_MAX;
            const auto bits_limit=native && d(stretch?14:12)?d(stretch?14:12):UINT32_MAX;
            const auto f=dib_format(memory,arg(10),d(11),true,info_limit,logical_palette());const auto start=stretch?0:d(7),lines=stretch?f.height:start<f.height?std::min(d(8),f.height-start):0;
            const auto width=si(3),height=si(4),sw=stretch?si(7):width,sh=stretch?si(8):height;
            if(!stretch && (width<0 || height<0))throw DibError{87};
            if(width && height && sw && sh && lines){
                if(std::uint64_t{f.stride}*lines>bits_limit)throw DibError{87};
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
                        const auto src=make_hardware_raster(band.width,band.height,band.pixels);
                        const auto pattern=pattern_used?gdi_brush_raster(handle,arg(0),brush):nullptr;
                        auto target=dc->second;
                        if(!stretch){
                            target.stretch_mode=3;
                            if(native && !d(14)){
                                target.world=gui_identity;target.map_mode=1;target.viewport_origin={};target.window_origin={};
                                target.window_extent={1,1};target.viewport_extent={1,1};
                            }
                        }
                        if(!blit_raster(memory,arg(0),src.get(),dest,source,target,UINT32_MAX,options,pattern.get()))throw DibError{last_error_};
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
