#include "x64arm/windows_services.hpp"
#include "x64arm/windows_icon.hpp"
#include "x64arm/windows_palette.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
std::span<const WindowsServiceDescriptor> icon_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"comctl32.dll","LoadIconMetric",4},{"comctl32.dll","LoadIconWithScaleDown",5},
        {"user32.dll","LoadCursorFromFileA",1},{"user32.dll","LoadCursorFromFileW",1},
        {"user32.dll","CopyImage",5},
        {"user32.dll","CreateIconFromResource",4},{"user32.dll","CreateIconFromResourceEx",7},
        {"user32.dll","LookupIconIdFromDirectory",2},{"user32.dll","LookupIconIdFromDirectoryEx",5},
        {"user32.dll","PrivateExtractIconsA",8},{"user32.dll","PrivateExtractIconsW",8},
        {"shell32.dll","ExtractIconA",3},{"shell32.dll","ExtractIconW",3},
        {"shell32.dll","ExtractIconExA",5},{"shell32.dll","ExtractIconExW",5},
        {"shell32.dll","SHGetStockIconInfo",3},
        // 系統游標取代（SPI_SETCURSORS 還原）與動畫游標的逐步幀資訊。
        {"user32.dll","SetSystemCursor",2,0,0,0x2},{"user32.dll","GetCursorFrameInfo",5,0,0,0x6},
        {"win32u.dll","NtUserGetCursorFrameInfo",4,0,0,0x2}
    };return table;
}
std::uint64_t WindowsServices::store_gui_icon(GuiIcon icon,std::uint32_t flags){
    icon.shared=(flags&0x8000)!=0;
    if(icon.shared)for(const auto& [handle,existing]:gui_.icons)if(existing.shared && existing==icon)return handle;
    if(gui_.icons.size()>=8192)throw std::bad_alloc{};const auto handle=gui_.next_object++;gui_.icons.emplace(handle,std::move(icon));return handle;
}
std::uint64_t WindowsServices::shell_system_icons(std::uint32_t size){
    auto& handle=size==16?gui_.shell_small_image_list:size==32?gui_.shell_large_image_list:gui_.shell_sized_image_lists[size];
    if(gui_.image_lists.contains(handle))return handle;
    GuiImageList images;images.width=static_cast<std::int32_t>(size);images.height=images.width;images.flags=0x21;
    for(const auto id:{0U,3U,8U}){auto icon=native_stock_icon(id,size);if(!icon)return 0;icon->system=0x10001U+static_cast<std::uint32_t>(images.images.size());images.images.push_back(std::move(*icon));}
    // 其他尺寸的系統影像清單在建立時補上已快取的圖示（沿用 32 像素圖像），使索引與大小清單一致。
    if(size!=16 && size!=32 && gui_.image_lists.contains(gui_.shell_large_image_list)){const auto& source=gui_.image_lists.at(gui_.shell_large_image_list).images;for(std::size_t i=images.images.size();i<source.size();++i)images.images.push_back(source[i]);}
    handle=gui_.next_object++;gui_.image_lists.emplace(handle,std::move(images));return handle;
}

std::uint64_t WindowsServices::shell_image_list(std::uint32_t size){
    {const auto& existing=size==16?gui_.shell_small_image_list:size==32?gui_.shell_large_image_list:gui_.shell_sized_image_lists[size];if(gui_.image_lists.contains(existing))return existing;}
    if(const auto native=shell_system_icons(size))return native;
    GuiImageList images;images.width=static_cast<std::int32_t>(size);images.height=images.width;images.flags=0x21;
    for(std::uint32_t i=0;i<3;++i)images.images.push_back(GuiIcon{{},size,size,0x10001U+i});if(size!=16 && size!=32 && gui_.image_lists.contains(gui_.shell_large_image_list)){const auto& source=gui_.image_lists.at(gui_.shell_large_image_list).images;for(std::size_t i=images.images.size();i<source.size();++i)images.images.push_back(source[i]);}
    auto& handle=size==16?gui_.shell_small_image_list:size==32?gui_.shell_large_image_list:gui_.shell_sized_image_lists[size];
    handle=gui_.next_object++;gui_.image_lists.emplace(handle,std::move(images));return handle;
}
namespace {
std::size_t stock_index(std::int32_t index){
    const auto ids=stock_icon_ids();if(index>=0)return static_cast<std::size_t>(index);
    const auto resource=-static_cast<std::int64_t>(index);if(resource<0x4000 || resource>INT64_C(0x4000)+UINT32_MAX)return ids.size();
    const auto id=static_cast<std::uint32_t>(resource-0x4000);const auto found=std::find(ids.begin(),ids.end(),id);return static_cast<std::size_t>(found-ids.begin());
}
void decorate_stock(GuiIcon& icon,std::uint32_t flags){
    if(flags&0x8000){auto arrow=native_stock_icon(29,std::max(8U,icon.width/2));if(!arrow)throw std::invalid_argument("原生連結圖示不可用");
        for(std::uint32_t y=0;y<arrow->height;++y)for(std::uint32_t x=0;x<arrow->width;++x){
            const auto src=arrow->rgba[static_cast<std::size_t>(y)*arrow->width+x];auto& dst=icon.rgba[static_cast<std::size_t>(icon.height-arrow->height+y)*icon.width+x];
            const auto sa=src>>24,da=dst>>24,a=sa+(da*(255-sa)+127)/255;std::uint32_t out=a<<24;
            for(unsigned c=0;c<3;++c){const auto s=(src>>(c*8))&255U,d=(dst>>(c*8))&255U;out|=(a?(s*sa*255+d*da*(255-sa)+a*127)/(a*255):0U)<<(c*8);}dst=out;
        }
    }
    if(flags&0x10000)for(auto& pixel:icon.rgba){const auto r=pixel&255U,g=(pixel>>8)&255U,b=(pixel>>16)&255U;pixel=(pixel&0xff000000U)|((r+40)/2)|(((g+100)/2)<<8)|(((b+220)/2)<<16);}
    icon.color_pixels.clear();icon.mask_pixels.clear();finish_icon(icon);
}
}
std::optional<std::uint32_t> WindowsServices::invoke_icon_api(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};const auto unit=x86_?4U:8U;
    std::uint64_t result=0;std::vector<std::uint64_t> allocated;
    const bool metric=symbol=="LoadIconMetric",scaled=metric || symbol=="LoadIconWithScaleDown";
    try {
        if(scaled){
            const auto finish=[&](std::uint32_t status){arg.return_to(state,status);return std::optional<std::uint32_t>{};};
            const auto output=arg(metric?3:4);if(!output)return finish(0x80070057U);
            memory.store(GuestAddress{output},static_cast<std::uint8_t>(unit*8),0);
            if(!arg(1) || (metric && d(2)>1))return finish(0x80070057U);
            const auto width=metric?system_metric(memory,d(2)?11:49):d(2),height=metric?system_metric(memory,d(2)?12:50):d(3);
            if(width>4096 || height>4096)return finish(0x80070057U);
            const auto standard=[](std::uint32_t size){return size==16 || size==32 || size==48 || size==256;};
            const bool prefer_larger=metric || !(standard(width) && standard(height));
            // 名稱／序號、ICO／ANI 與模組資源共用既有解碼、縮放及 HICON 生命週期。
            std::uint64_t handle=0;
            if(!arg(0) && arg(1)>65535){
                const auto file=filesystem_.open(utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)})));
                if(file.error)last_error_=file.error==2 || file.error==3?1813:file.error;
                else handle=store_gui_icon(decode_icon_file(*file.bytes,false,width,height,0x40U,prefer_larger));
            }else handle=load_gui_icon(memory,arg(0),arg(1),true,width,height,false,false,false,0,prefer_larger);
            if(!handle)return finish(0x80070000U|(last_error_?last_error_:1813U));
            memory.store(GuestAddress{output},static_cast<std::uint8_t>(unit*8),handle);
            return finish(0);
        }else if(symbol=="SetSystemCursor"){
            // 以 hcur 的內容取代 OCR_* 系統游標，並依文件以 DestroyCursor 銷毀 hcur；目前正使用該系統游標時立即更新。
            const auto id=d(1);const auto found=gui_.icons.find(arg(0));
            if(found==gui_.icons.end() || found->second.is_icon){last_error_=1402;result=0;}
            else if(!gui_system_cursor_id(id)){last_error_=87;result=0;}
            else {
                auto copy=found->second;copy.shared=false;copy.system=0;copy.is_icon=false;gui_.system_cursors.insert_or_assign(id,std::move(copy));
                release_cursor_frames(0xd000U+id);
                (void)nested_service(state,memory,"user32.dll","DestroyCursor",{arg(0)});
                if(gui_.cursor==0xd000U+id)present_gui_cursor();
                result=1;
            }
        }else if(symbol=="GetCursorFrameInfo" || symbol=="NtUserGetCursorFrameInfo"){
            const unsigned index=symbol=="GetCursorFrameInfo"?2:1;
            const auto step=d(index);
            const auto rate_out=arg(index+1),steps_out=arg(index+2);
            if(!rate_out || !steps_out){arg.return_to(state,0);return std::nullopt;}
            GuiIcon system;
            const auto known=gui_.icons.find(arg(0));
            const auto* icon=known!=gui_.icons.end()?&known->second:resolve_gui_cursor(arg(0),system);
            if(!icon)last_error_=1402;
            else if(icon->animation_sequence.empty() || step<icon->animation_sequence.size()){
                memory.check(GuestAddress{rate_out},4,Access::Write);
                memory.check(GuestAddress{steps_out},4,Access::Write);
                if(icon->animation_frames.size()<=1){
                    result=arg(0);
                    memory.store(GuestAddress{rate_out},32,0);
                    memory.store(GuestAddress{steps_out},32,1);
                }else {
                    const auto steps=static_cast<std::uint32_t>(icon->animation_sequence.size());
                    memory.store(GuestAddress{rate_out},32,icon->animation_rates.at(step));
                    memory.store(GuestAddress{steps_out},32,steps==1?UINT32_MAX:steps);
                    const auto frame=icon->animation_sequence[step];const auto key=std::pair{arg(0),frame};
                    if(const auto cached=gui_.cursor_frames.find(key);cached!=gui_.cursor_frames.end() && gui_.icons.contains(cached->second))result=cached->second;
                    else {
                        if(gui_.icons.size()>=8192)throw std::bad_alloc{};
                        auto copy=icon->animation_frames.at(frame);copy.shared=true;copy.is_icon=icon->is_icon;
                        result=gui_.next_object++;gui_.icons.emplace(result,std::move(copy));gui_.cursor_frames.insert_or_assign(key,result);
                    }
                }
            }
        }else if(symbol=="CopyImage"){
            const auto type=d(1),flags=d(4),requested_width=d(2),requested_height=d(3);
            const auto finish=[&](std::uint64_t handle){arg.return_to(state,handle);return std::optional<std::uint32_t>{};};
            const auto fail=[&](std::uint32_t error){last_error_=error;return finish(0);};
            if(type>2 || requested_width>16384 || requested_height>16384 || (flags&~0x604dU) || (type && (flags&0x2000U)))return fail(87);
            const bool monochrome=(flags&1)!=0;
            if(!type){
                const auto source=gui_.bitmaps.find(arg(0));if(source==gui_.bitmaps.end())return fail(6);
                const auto width=requested_width?requested_width:source->second.width,height=requested_height?requested_height:source->second.height;
                if(!width || !height || std::uint64_t{width}*height>64U*1024U*1024U)return fail(8);
                if((flags&4) && width==source->second.width && height==source->second.height && (!monochrome || source->second.bits_per_pixel==1) && (!(flags&0x2000U) || source->second.dib_address))return finish(arg(0));
                sync_raster_bitmap(memory,arg(0));const auto original=source->second;
                GuiBitmap copy{width,height,std::vector<std::uint32_t>(static_cast<std::size_t>(width)*height)};
                copy.bits_per_pixel=monochrome?1:original.bits_per_pixel;copy.dib_masks=original.dib_masks;
                copy.dib_compression=monochrome?0:original.dib_compression;copy.dib_palette=monochrome?gui_dib_palette(1):original.dib_palette;copy.raw_alpha=original.raw_alpha && !monochrome;
                if(copy.bits_per_pixel<=8 && copy.dib_palette.empty())copy.dib_palette=gui_dib_palette(copy.bits_per_pixel);
                for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x){auto pixel=original.pixels[static_cast<std::size_t>(std::uint64_t{y}*original.height/height)*original.width+static_cast<std::size_t>(std::uint64_t{x}*original.width/width)];
                    if(monochrome)pixel=((pixel&255U)*299+((pixel>>8)&255U)*587+((pixel>>16)&255U)*114>=128000U)?0xffffffffU:0xff000000U;
                    copy.pixels[static_cast<std::size_t>(y)*width+x]=pixel;}
                if(flags&0x2000U){
                    result=create_dib_bitmap(memory,state,width,static_cast<std::int32_t>(height),copy.bits_per_pixel);if(!result)return finish(0);
                    auto& destination=gui_.bitmaps.at(result);destination.pixels=std::move(copy.pixels);destination.dib_masks=copy.dib_masks;destination.dib_compression=copy.dib_compression;destination.dib_palette=std::move(copy.dib_palette);
                    try{export_dib_bitmap(memory,result);}catch(...){(void)free_heap(memory,destination.dib_address,state);gui_.bitmaps.erase(result);throw;}
                }else {copy.stride=(width*copy.bits_per_pixel+15)/16*2;const auto handle=gui_.next_object;gui_.bitmaps.emplace(handle,std::move(copy));++gui_.next_object;result=handle;}
            }else {
                const auto source=gui_.icons.find(arg(0));std::optional<GuiIcon> system;
                if(source==gui_.icons.end() && type==2)system=resolve_system_cursor(gui_,arg(0));
                if(source==gui_.icons.end() && !system)return fail(type==1?1402:1401);
                const auto original=source!=gui_.icons.end()?source->second:*system;if(original.is_icon!=(type==1))return fail(87);
                const auto width=requested_width?requested_width:(flags&0x40U)?system_metric(memory,type==1?11:13):original.width;
                const auto height=requested_height?requested_height:(flags&0x40U)?system_metric(memory,type==1?12:14):original.height;
                if(width>4096 || height>4096)return fail(87);
                if((flags&4) && width==original.width && height==original.height && (!monochrome || original.monochrome))return finish(arg(0));
                if((flags&0x4000U) && original.resource_instance && (original.resource_name_id || !original.resource_name.empty())){
                    std::uint64_t name=original.resource_name_id,buffer=0;
                    if(!original.resource_name.empty()){const auto bytes=utf16_bytes(original.resource_name);buffer=allocate_heap(memory,bytes.size());if(!buffer)return fail(8);
                        try{memory.write(GuestAddress{buffer},bytes);}catch(...){(void)free_heap(memory,buffer,state);throw;}name=buffer;}
                    try{result=load_gui_icon(memory,original.resource_instance,name,true,width,height,true,false,type==2,monochrome?1:0);}catch(...){if(buffer)(void)free_heap(memory,buffer,state);throw;}
                    if(buffer)(void)free_heap(memory,buffer,state);if(!result)return finish(0);
                }else {auto copy=original;copy.shared=false;copy.system=0;resize_icon(copy,width,height,monochrome);result=store_gui_icon(std::move(copy));}
            }
            if(flags&8U){start_callbacks(state,memory,{{resolve(type?"user32.dll":"gdi32.dll",type==1?"DestroyIcon":type==2?"DestroyCursor":"DeleteObject").value,{arg(0)}}},Continuation::Return,result);return std::nullopt;}
            return finish(result);
        }else if(symbol.starts_with("LoadCursor") || symbol.starts_with("LoadImage") || symbol.starts_with("LoadIcon")){
            const bool image=symbol.starts_with("LoadImage"),from_cursor_file=symbol.starts_with("LoadCursorFromFile");
            const auto type=image?d(2):symbol.starts_with("LoadIcon")?1U:2U;
            if(image && !type)return invoke_resource(state,memory,service);
            const auto flags=image?d(5):from_cursor_file?0x50U:0x8040U;
            const auto name=arg(from_cursor_file?0:1),instance=from_cursor_file?0:arg(0);
            const auto width=image?d(3):0U,height=image?d(4):0U;
            if((type!=1 && type!=2) || !name || width>4096 || height>4096 || (flags&~0x80d1U))last_error_=87;
            else if(flags&0x10U){
                if(instance || name<=65535)last_error_=87;
                else {
                    const auto path=symbol.ends_with('W')?read_utf16(memory,GuestAddress{name}):utf8_to_utf16_lossy(read_ansi(memory,name));
                    const auto file=filesystem_.open(utf16_to_utf8(path));
                    if(file.error)last_error_=file.error;
                    else if(from_cursor_file && file.bytes->size()>=4 && (*file.bytes)[0]==0 && (*file.bytes)[1]==0 && (*file.bytes)[2]==1)last_error_=193;
                    else result=store_gui_icon(decode_icon_file(*file.bytes,type==2,width,height,flags),flags&~0x8000U);
                }
            }else if(image && !instance && name<=65535 && !(flags&0x8000U))last_error_=1814;
            else result=load_gui_icon(memory,instance,name,symbol.ends_with('W'),width,height,!(flags&0x40U),false,type==2,flags);
        }else if(symbol.starts_with("CreateIconFromResource")){
            const bool ex=symbol.ends_with("Ex");const auto flags=ex?d(6):0U;
            if(!arg(0) || (arg(0)&3) || !d(1) || d(1)>64U*1024U*1024U || d(3)<0x20000 || d(3)>0x30000 || (flags&~0x8041U) || (ex && (d(4)>4096 || d(5)>4096)))last_error_=87;
            else {std::vector<std::uint8_t> bytes(d(1));memory.read(GuestAddress{arg(0)},bytes);result=store_gui_icon(decode_icon_resource(bytes,d(2)!=0,ex?d(4):0,ex?d(5):0,flags),flags);}
        }else if(symbol.starts_with("LookupIconIdFromDirectory")){
            const bool ex=symbol.ends_with("Ex");const auto flags=ex?d(4):0U;
            if(!arg(0) || (flags&~1U) || (ex && (d(2)>4096 || d(3)>4096)))last_error_=87;
            else {const auto count=memory.load(GuestAddress{arg(0)+4},16);if(!count || count>4096)last_error_=87;
                else {std::vector<std::uint8_t> bytes(static_cast<std::size_t>(6+count*14));memory.read(GuestAddress{arg(0)},bytes);result=choose_icon(bytes,!d(1),false,ex?d(2):32,ex?d(3):32,(flags&1)!=0).id;}}
        }else if(symbol=="SHGetStockIconInfo"){
            const auto flags=d(1),size=x86_?536U:544U;
            if(!stock_icon_valid(d(0)) || (flags&~0x1c105U) || !arg(2) || memory.load(GuestAddress{arg(2)},32)!=size)result=0x80070057U;
            else {
                memory.check(GuestAddress{arg(2)},size,Access::Write);const auto width=flags&1?16U:flags&4?48U:32U;
                const auto sys_width=flags&1?16U:32U;auto icon=native_stock_icon(d(0),width);
                if(!icon)result=0x80004001U;
                else {
                    if(flags&(0x8000|0x10000))decorate_stock(*icon,flags);
                    std::uint64_t list=0;const auto index=3+d(0);
                    if(flags&0x4000){list=shell_system_icons(sys_width);if(!list)throw std::invalid_argument("原生 Shell 圖示不可用");auto& images=gui_.image_lists.at(list).images;
                        if(images.size()<=index)images.resize(index+1);if(images[index].rgba.empty()){auto sys=native_stock_icon(d(0),sys_width);if(!sys)throw std::invalid_argument("原生 Shell 圖示不可用");images[index]=std::move(*sys);}}
                    const auto handle=flags&0x100?store_gui_icon(std::move(*icon)):0;
                    memory.store(GuestAddress{arg(2)+(x86_?4:8)},static_cast<std::uint8_t>(unit*8),handle);
                    memory.store(GuestAddress{arg(2)+(x86_?8:16)},32,flags&0x4000?index:UINT32_MAX);
                    memory.store(GuestAddress{arg(2)+(x86_?12:20)},32,static_cast<std::uint32_t>(-static_cast<std::int32_t>(0x4000+d(0))));
                    memory.write(GuestAddress{arg(2)+(x86_?16:24)},utf16_bytes(stock_icon_path));result=0;
                }
            }
        }else{
            const bool private_api=symbol.starts_with("PrivateExtract"),simple=symbol=="ExtractIconA" || symbol=="ExtractIconW",wide=symbol.ends_with('W');
            const auto path_pointer=arg(simple?1:0);const auto index=std::bit_cast<std::int32_t>(d(simple?2:1));
            const auto large=private_api?arg(4):simple?0:arg(2),small=private_api?0:simple?0:arg(3),ids=private_api?arg(5):0;
            const auto count=simple?1U:d(private_api?6:4),flags=private_api?d(7):0U;
            const bool query=simple?index==-1:private_api?!large:index==-1 && !large && !small;
            if(!path_pointer || (!query && count>4096) || (flags&~0x8041U)){last_error_=87;result=private_api || simple?0:UINT32_MAX;}
            else {
                const auto path=wide?read_utf16(memory,GuestAddress{path_pointer}):utf8_to_utf16_lossy(read_ansi(memory,path_pointer));const bool builtin=is_stock_icon_path(path);
                if(!query && !simple){for(const auto out:{large,small})if(out){memory.check(GuestAddress{out},static_cast<std::size_t>(count)*unit,Access::Write);for(std::uint32_t i=0;i<count;++i)memory.store(GuestAddress{out+i*unit},static_cast<std::uint8_t>(unit*8),0);}if(ids){memory.check(GuestAddress{ids},static_cast<std::size_t>(count)*4,Access::Write);for(std::uint32_t i=0;i<count;++i)memory.store(GuestAddress{ids+i*4},32,UINT32_MAX);}}
                const auto file=builtin?FileSnapshot{}:filesystem_.open(utf16_to_utf8(path));
                if(!builtin && file.error){last_error_=file.error;result=simple || (private_api && query)?0:UINT32_MAX;}
                else {
                    if(simple && !builtin){const auto& bytes=*file.bytes;const bool mz=bytes.size()>=2 && bytes[0]=='M' && bytes[1]=='Z';const bool icon_file=bytes.size()>=6 && bytes[0]==0 && bytes[1]==0 && (bytes[2]==1 || bytes[2]==2) && bytes[3]==0;
                        if(!mz && !icon_file){arg.return_to(state,1);return std::nullopt;}}
                    std::optional<IconSource> source;if(!builtin)source.emplace(*file.bytes);const auto total=builtin?stock_icon_ids().size():source->count(),first=builtin?stock_index(index):source->index(index);
                    if(query)result=total;
                    else {
                        const auto extract=[&](std::size_t i,std::uint32_t w,std::uint32_t h,std::uint32_t& id){
                            if(!builtin)return source->extract(i,w,h,flags,&id);id=0x4000+stock_icon_ids()[i];auto icon=native_stock_icon(stock_icon_ids()[i],w?w:32);if(!icon)throw std::invalid_argument("原生 Shell 圖示不可用");resize_icon(*icon,w,h?h:w,(flags&1)!=0);return std::move(*icon);
                        };
                        std::vector<std::tuple<GuiIcon,std::uint64_t,std::uint32_t>> pending;std::size_t bytes=0;std::uint32_t written=0;
                        const auto append=[&](std::size_t i,std::uint32_t w,std::uint32_t h,std::uint64_t output){std::uint32_t id=UINT32_MAX;auto icon=extract(i,w,h,id);icon.is_icon=true;finish_icon(icon);bytes+=icon.rgba.size()*4+icon.color_pixels.size()*4+icon.mask_pixels.size()+icon.image.size();if(bytes>64U*1024U*1024U)throw std::bad_alloc{};pending.emplace_back(std::move(icon),output,id);};
                        for(std::size_t i=first;i<total && written<count;++i){
                            if(simple){append(i,32,32,0);++written;break;}
                            else if(private_api){if(large){append(i,d(2)&0xffff,d(3)&0xffff,large+written*unit);++written;if((d(2)>>16) && (d(3)>>16) && written<count){append(i,d(2)>>16,d(3)>>16,large+written*unit);++written;}}else break;}
                            else {if(large)append(i,32,32,large+written*unit);if(small)append(i,16,16,small+written*unit);if(!large && !small)break;++written;}
                        }
                        allocated.reserve(pending.size());for(auto& [icon,out,id]:pending){(void)out;(void)id;const auto handle=store_gui_icon(std::move(icon),flags);allocated.push_back(handle);}
                        for(std::size_t i=0;i<pending.size();++i){const auto& [icon,out,id]=pending[i];(void)icon;if(out)memory.store(GuestAddress{out},static_cast<std::uint8_t>(unit*8),allocated[i]);if(ids)memory.store(GuestAddress{ids+i*4},32,id);}
                        result=simple?(allocated.empty()?0:allocated.front()):written;
                    }
                }
            }
        }
    }catch(const std::invalid_argument&){last_error_=193;result=scaled || symbol=="SHGetStockIconInfo"?0x8007000dU:symbol.starts_with("ExtractIconEx")?UINT32_MAX:0;}
    catch(const std::bad_alloc&){for(const auto handle:allocated){const auto icon=gui_.icons.find(handle);if(icon!=gui_.icons.end() && !icon->second.shared)gui_.icons.erase(icon);}last_error_=8;result=scaled || symbol=="SHGetStockIconInfo"?0x8007000eU:0;}
    arg.return_to(state,result);return std::nullopt;
}
// shell32 圖示家族：DuplicateIcon、ExtractAssociatedIcon(Ex)、FreeIconList、SHDefExtractIcon、Shell_GetImageLists、Shell_GetCachedImageIndex、
// SHUpdateImage／SHHandleUpdateImage、SHGetIconOverlayIndex、SHLoadNonloadedIconOverlayIdentifiers、SHCreateFileExtractIconW（IExtractIconW）。
// 圖示一律經同一個 IconSource／原生內建圖示路徑取得，與 ExtractIcon／PrivateExtractIcons 一致；A 版字串依 ACP（UTF-8）轉換。
std::span<const WindowsServiceDescriptor> shell_icon_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shell32.dll","DuplicateIcon",2},{"shell32.dll","ExtractAssociatedIconA",3},{"shell32.dll","ExtractAssociatedIconW",3},
        {"shell32.dll","ExtractAssociatedIconExA",4},{"shell32.dll","ExtractAssociatedIconExW",4},{"shell32.dll","FreeIconList",1,0,0,0x1},
        {"shell32.dll","SHDefExtractIconA",6,0,0,0x26},{"shell32.dll","SHDefExtractIconW",6,0,0,0x26},{"shell32.dll","Shell_GetImageLists",2},
        {"shell32.dll","Shell_GetCachedImageIndexA",3,0,0,0x6},{"shell32.dll","Shell_GetCachedImageIndexW",3,0,0,0x6},
        {"shell32.dll","SHUpdateImageA",4,0,0,0xe},{"shell32.dll","SHUpdateImageW",4,0,0,0xe},{"shell32.dll","SHHandleUpdateImage",1},
        {"shell32.dll","SHGetIconOverlayIndexA",2,0,0,0x2},{"shell32.dll","SHGetIconOverlayIndexW",2,0,0,0x2},{"shell32.dll","SHLoadNonloadedIconOverlayIdentifiers",0},
        {"shell32.dll","SHCreateFileExtractIconW",4,0,0,0x2},
        {"internal-extract-icon","QueryInterface",3},{"internal-extract-icon","AddRef",1},{"internal-extract-icon","Release",1},
        {"internal-extract-icon","GetIconLocation",6,0,0,0xa},{"internal-extract-icon","Extract",6,0,0,0x24},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_shell_icons(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool narrow=name.ends_with('A');
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto read=[&](std::uint64_t at)->std::u16string{if(!at)return {};return narrow?utf8_to_utf16_lossy(read_ansi(memory,at,32768)):read_utf16(memory,GuestAddress{at},32768);};
    const auto write=[&](std::uint64_t at,const std::u16string& text,std::size_t capacity)->bool{
        if(narrow){const auto bytes=utf16_to_utf8_lossy(text);if(bytes.size()>=capacity)return false;std::vector<std::uint8_t> data(bytes.begin(),bytes.end());data.push_back(0);memory.write(GuestAddress{at},data);return true;}
        if(text.size()>=capacity)return false;memory.write(GuestAddress{at},utf16_bytes(text));return true;};
    constexpr std::array<std::uint8_t,16> unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},extract_iid{0xfa,0x14,0x02,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    // 由檔案（或內建圖示路徑）取指定索引、指定大小的圖示；負索引為資源 ID。找不到回 nullopt，檔案無法讀取另回報 file_error。
    std::uint32_t file_error=0;
    const auto load=[&](const std::u16string& path,std::int32_t index,std::uint32_t size)->std::optional<GuiIcon>{
        file_error=0;
        try{
            if(is_stock_icon_path(path)){const auto ids=stock_icon_ids();const auto at=stock_index(index);if(at>=ids.size())return std::nullopt;
                auto icon=native_stock_icon(ids[at],size);if(!icon)return std::nullopt;resize_icon(*icon,size,size);icon->is_icon=true;finish_icon(*icon);return icon;}
            const auto file=filesystem_.open(utf16_to_utf8(path));if(file.error){file_error=file.error;return std::nullopt;}
            IconSource source(*file.bytes);const auto at=source.index(index);if(at>=source.count())return std::nullopt;
            auto icon=source.extract(at,size,size);icon.is_icon=true;finish_icon(icon);return icon;
        }catch(const std::invalid_argument&){file_error=193;return std::nullopt;}};
    const auto expand=[&](const std::u16string& text)->std::u16string{
        GuestScratch temporary(*this,memory,state);const auto source=temporary.text(text),buffer=temporary.allocate(2*32768);if(!source || !buffer)return text;
        const auto length=static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll","ExpandEnvironmentStringsW",{source,buffer,32768}));
        return length && length<=32768?read_utf16(memory,GuestAddress{buffer},32767):text;};
    // DefaultIcon 等 "路徑,索引" 文字：去引號、%1 代表檔案本身、展開環境變數。
    const auto location=[&](std::u16string text,const std::u16string& file)->std::pair<std::u16string,std::int32_t>{
        std::int32_t index=0;if(const auto comma=text.find_last_of(u',');comma!=text.npos){std::size_t at=comma+1;while(at<text.size() && text[at]==u' ')++at;bool negative=false;if(at<text.size() && text[at]==u'-'){negative=true;++at;}
            std::int64_t value=0;for(;at<text.size() && text[at]>=u'0' && text[at]<=u'9';++at)value=std::min<std::int64_t>(value*10+(text[at]-u'0'),INT32_MAX);index=static_cast<std::int32_t>(negative?-value:value);text.resize(comma);}
        while(!text.empty() && text.back()==u' ')text.pop_back();if(text.size()>=2 && text.front()==u'"' && text.back()==u'"')text=text.substr(1,text.size()-2);
        if(text==u"%1")return {file,index};return {expand(text),index};};
    const auto extension_of=[](const std::u16string& path){const auto slash=path.find_last_of(u"\\/");const auto dot=path.rfind(u'.');if(dot==path.npos || (slash!=path.npos && dot<slash))return std::u16string{};
        auto out=path.substr(dot);for(auto& c:out)c=unicode_case_unit(c,false);return out;};
    // 檔案的圖示位置：資料夾與無關聯檔案用內建圖示；.exe／.ico／.cur 用檔案本身；其他依關聯的 DefaultIcon，最後退回執行檔圖示。flags 為 GIL_*。
    const auto icon_location=[&](const std::u16string& path,std::uint32_t attributes,std::uint32_t& flags)->std::pair<std::u16string,std::int32_t>{
        flags=0;if(attributes&0x10U)return {std::u16string(stock_icon_path),-(0x4000+3)};
        const auto extension=extension_of(path);if(extension==u".exe" || extension==u".ico" || extension==u".cur"){flags=2;return {path,0};} // GIL_PERINSTANCE
        if(!extension.empty()){std::u16string value;
            if(!query_association(0,15,extension,u"",value)){flags=4;return location(value,path);} // GIL_PERCLASS
            if(!query_association(0,2,extension,u"",value)){flags=4;return {value,0};}}
        flags=4;return {std::u16string(stock_icon_path),-(0x4000+0)};};
    try{
        if(name=="DuplicateIcon")return done(nested_service(state,memory,"user32.dll","CopyIcon",{arg(1)}));
        if(name=="FreeIconList")return done(0); // 16 位元相容入口：Win32 沒有可釋放的清單
        if(name=="Shell_GetImageLists"){if(arg(0))memory.store(GuestAddress{arg(0)},bits,shell_image_list(32));if(arg(1))memory.store(GuestAddress{arg(1)},bits,shell_image_list(16));return done(1);}
        if(name=="SHLoadNonloadedIconOverlayIdentifiers")return done(0); // 沒有登錄的覆疊識別處理常式時沒有需要載入的項目
        if(name.starts_with("SHGetIconOverlayIndex")){
            // 內建覆疊：IDO_SHGIOI_SHARE 1、LINK 2、SLOWFILE 3、DEFAULT 4；自訂圖示路徑沒有登錄的覆疊識別處理常式，回 -1。
            if(!arg(0)){switch(d(1)){case 0x0fffffffU:return done(1);case 0x0ffffffeU:return done(2);case 0x0ffffffdU:return done(3);case 0x0ffffffcU:return done(4);default:return done(UINT32_MAX);}}
            return done(UINT32_MAX);
        }
        if(name.starts_with("ExtractAssociatedIcon")){
            // (HINSTANCE, 路徑 輸入輸出 MAX_PATH, WORD* 索引 輸入輸出[, WORD* 資源 ID])：先取檔案本身的圖示，其次關聯的 DefaultIcon／執行檔，最後為內建文件圖示；改寫路徑與索引為實際來源。
            const bool ex=name.starts_with("ExtractAssociatedIconEx");if(!arg(1) || !arg(2))return done(0);const auto path=read(arg(1));
            auto index=static_cast<std::int32_t>(static_cast<std::int16_t>(memory.load(GuestAddress{arg(2)},16)));
            auto icon=load(path,index,32);std::u16string source=path;
            if(!icon){std::uint32_t flags=0;const auto [where,at]=icon_location(path,0,flags);icon=load(where,at,32);source=where;index=at;}
            if(!icon){source=std::u16string(stock_icon_path);index=-(0x4000+0);icon=load(source,index,32);}
            if(!icon)return done(0);
            if(source!=path)(void)write(arg(1),source,260);memory.store(GuestAddress{arg(2)},16,static_cast<std::uint16_t>(index));
            if(ex && arg(3))memory.store(GuestAddress{arg(3)},16,static_cast<std::uint16_t>(index<0?-index:index));
            return done(store_gui_icon(std::move(*icon)));
        }
        const auto extract_pair=[&](const std::u16string& file,std::int32_t index,std::uint64_t large,std::uint64_t small,std::uint32_t sizes)->std::uint32_t{
            // SHDefExtractIcon／IExtractIcon::Extract：LOWORD 為大圖示、HIWORD 為小圖示的像素大小（0 時為 32／16）。檔案無法讀取回 E_FAIL，沒有該圖示回 S_FALSE。
            if(large)memory.store(GuestAddress{large},bits,0);if(small)memory.store(GuestAddress{small},bits,0);
            const auto large_size=(sizes&0xffffU)?sizes&0xffffU:32U,small_size=(sizes>>16)?sizes>>16:16U;
            std::optional<GuiIcon> big,little;if(large)big=load(file,index,large_size);if(file_error)return 0x80004005U;if(small)little=load(file,index,small_size);if(file_error)return 0x80004005U;
            if((large && !big) || (small && !little))return 1;
            if(big)memory.store(GuestAddress{large},bits,store_gui_icon(std::move(*big)));if(little)memory.store(GuestAddress{small},bits,store_gui_icon(std::move(*little)));return 0;};
        if(name.starts_with("SHDefExtractIcon")){if(!arg(0))return done(0x80070057U);return done(extract_pair(read(arg(0)),static_cast<std::int32_t>(d(1)),arg(3),arg(4),d(5)));}
        if(name.starts_with("Shell_GetCachedImageIndex")){
            // 把圖示加入系統大小影像清單並回傳索引（相同路徑與索引只加入一次）；取不到圖示回 -1。
            auto key=read(arg(0));if(key.empty())return done(UINT32_MAX);const auto path=key;for(auto& c:key)c=unicode_case_unit(c,false);
            const auto index=static_cast<std::int32_t>(d(1));key+=u"|"+utf8_to_utf16_lossy(std::to_string(index));
            if(const auto found=gui_.shell_cached_icons.find(key);found!=gui_.shell_cached_icons.end())return done(found->second);
            auto big=load(path,index,32),little=load(path,index,16);if(!big || !little)return done(UINT32_MAX);
            auto& large_images=gui_.image_lists.at(shell_image_list(32)).images;auto& small_images=gui_.image_lists.at(shell_image_list(16)).images;
            const auto slot=static_cast<std::uint32_t>(std::max(large_images.size(),small_images.size()));
            // 其他尺寸的系統影像清單（SHGetImageList）同步加入同一個索引。
            for(const auto& [size,handle]:gui_.shell_sized_image_lists){if(!gui_.image_lists.contains(handle))continue;auto& sized=gui_.image_lists.at(handle).images;sized.resize(slot+1);
                if(auto icon=load(path,index,size))sized[slot]=std::move(*icon);else sized[slot]=*big;}
            large_images.resize(slot+1);small_images.resize(slot+1);large_images[slot]=std::move(*big);small_images[slot]=std::move(*little);
            gui_.shell_cached_icons.emplace(std::move(key),slot);return done(slot);
        }
        if(name.starts_with("SHUpdateImage")){
            // SHChangeUpdateImageIDList（1 位元組對齊）：cb、iIconIndex、iCurIndex、uFlags、dwProcessID、szName[MAX_PATH]、結尾 0；以 SHCNE_UPDATEIMAGE／SHCNF_IDLIST 通知。
            GuestScratch temporary(*this,memory,state);const auto list=temporary.allocate(540);if(!list)return done(0);std::vector<std::uint8_t> bytes(540);
            const auto put=[&](std::size_t at,std::uint32_t value,unsigned size){for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
            put(0,538,2);put(2,d(1),4);put(6,d(3),4);put(10,d(2),4);put(14,static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll","GetCurrentProcessId",{})),4);
            auto file=read(arg(0));if(file.size()>259)file.resize(259);for(std::size_t i=0;i<file.size();++i)put(18+i*2,file[i],2);
            memory.write(GuestAddress{list},bytes);(void)nested_service(state,memory,"shell32.dll","SHChangeNotify",{0x8000,0,list,0});return done(0);
        }
        if(name=="SHHandleUpdateImage"){
            // 同程序的通知直接回 iCurIndex；其他程序依名稱與圖示索引加入快取後回傳索引。
            if(!arg(0) || memory.load(GuestAddress{arg(0)},16)<538)return done(UINT32_MAX);
            const auto field=[&](unsigned at){return static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+at},32));};
            if(field(14)==static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll","GetCurrentProcessId",{})))return done(field(6));
            GuestScratch temporary(*this,memory,state);std::u16string file;for(unsigned i=0;i<259;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{arg(0)+18+i*2},16));if(!c)break;file.push_back(c);}
            const auto text=temporary.text(file);if(!text)return done(UINT32_MAX);
            return done(nested_service(state,memory,"shell32.dll","Shell_GetCachedImageIndexW",{text,field(2),field(10)})&UINT32_MAX);
        }
        if(name=="SHCreateFileExtractIconW"){
            // (檔案, FILE_ATTRIBUTE_*, riid, ppv)：不要求檔案存在，依名稱與屬性決定圖示。
            if(!arg(3))return done(0x80004003U);memory.store(GuestAddress{arg(3)},bits,0);if(!arg(0) || !arg(2))return done(0x80070057U);
            std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{arg(2)},iid);if(iid!=unknown && iid!=extract_iid)return done(0x80004002U);
            if(gui_.shell_extract_icons.size()>=4096)return done(0x8007000eU);
            if(!gui_.shell_extract_icon_vtable){const auto table=runtime_allocate(memory,{},5*unit);unsigned i=0;
                for(const auto& entry:shell_icon_service_descriptors())if(entry.family=="internal-extract-icon")memory.store(GuestAddress{table+unit*i++},bits,resolve(entry.family,entry.symbol).value);gui_.shell_extract_icon_vtable=table;}
            const auto object=allocate_heap(memory,unit);if(!object)return done(0x8007000eU);memory.store(GuestAddress{object},bits,gui_.shell_extract_icon_vtable);
            gui_.shell_extract_icons.emplace(object,GuiState::ShellExtractIcon{read(arg(0)),d(1)});memory.store(GuestAddress{arg(3)},bits,object);return done(0);
        }
        if(service.family=="internal-extract-icon"){
            const auto found=gui_.shell_extract_icons.find(arg(0));if(found==gui_.shell_extract_icons.end())return done(0x80010108U);auto& object=found->second;
            if(name=="AddRef")return done(++object.references);
            if(name=="Release"){if(--object.references)return done(object.references);gui_.shell_extract_icons.erase(found);(void)free_heap(memory,arg(0),state);return done(0);}
            if(name=="QueryInterface"){if(!arg(2))return done(0x80004003U);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(0x80004003U);
                std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{arg(1)},iid);if(iid!=unknown && iid!=extract_iid)return done(0x80004002U);++object.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return done(0);}
            if(name=="GetIconLocation"){
                // (uFlags, 圖示檔 輸出, cch, 索引*, GIL_* 旗標*)
                if(!arg(2) || !d(3) || !arg(4) || !arg(5))return done(0x80070057U);std::uint32_t flags=0;const auto [where,at]=icon_location(object.path,object.attributes,flags);
                if(!write(arg(2),where,d(3)))return done(0x8007007aU);memory.store(GuestAddress{arg(4)},32,static_cast<std::uint32_t>(at));memory.store(GuestAddress{arg(5)},32,flags);return done(0);
            }
            if(name=="Extract"){if(!arg(1))return done(0x80070057U);return done(extract_pair(read_utf16(memory,GuestAddress{arg(1)},32768),static_cast<std::int32_t>(d(2)),arg(3),arg(4),d(5)));}
        }
    }catch(const std::bad_alloc&){return done(0);}
    throw HostCallFault("未知的 shell32 圖示服務");
}
}
