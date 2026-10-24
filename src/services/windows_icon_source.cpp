#include "x64arm/windows_icon.hpp"
#include "x64arm/result.hpp"
#include "x64arm/windows_dib.hpp"
#include "x64arm/riff_chunk.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <stdexcept>

namespace x64arm {
namespace {
std::uint32_t integer(std::span<const std::uint8_t> bytes,std::size_t at,unsigned size){
    if(at>bytes.size() || size>bytes.size()-at)throw std::invalid_argument("圖示資料截斷");
    std::uint32_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint32_t{bytes[at+i]}<<(i*8);return value;
}
void put(std::vector<std::uint8_t>& bytes,std::size_t at,unsigned size,std::uint32_t value){
    for(unsigned i=0;i<size;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));
}
std::uint32_t premultiply(std::uint32_t pixel){
    const auto alpha=pixel>>24;std::uint32_t value=alpha<<24;
    for(unsigned c=0;c<3;++c)value|=(((pixel>>(c*8))&255U)*alpha+127U)/255U<<(c*8);return value;
}
void dimensions(std::uint32_t w,std::uint32_t h){
    if(!w || !h || w>4096 || h>4096 || std::uint64_t{w}*h>16U*1024U*1024U)throw std::invalid_argument("圖示尺寸不合法");
}
}
IconChoice choose_icon(std::span<const std::uint8_t> bytes,bool cursor,bool file,std::uint32_t width,std::uint32_t height,bool mono,bool prefer_larger){
    const auto count=integer(bytes,4,2),entry_size=file?16U:14U;
    if(integer(bytes,0,2) || integer(bytes,2,2)!=(cursor?2U:1U) || !count || count>4096 || bytes.size()<6+count*entry_size)throw std::invalid_argument("圖示群組格式不合法");
    width=width?width:32;height=height?height:32;dimensions(width,height);
    IconChoice best;bool selected=false,best_fits=false;std::uint32_t best_distance=UINT32_MAX,best_depth_distance=UINT32_MAX;const auto wanted_depth=mono?1U:32U;
    for(std::uint32_t i=0;i<count;++i){
        const auto at=6+i*entry_size;
        const auto w=cursor && !file?integer(bytes,at,2):(integer(bytes,at,1)?integer(bytes,at,1):256U);
        const auto h=cursor && !file?integer(bytes,at+2,2)/2:(integer(bytes,at+1,1)?integer(bytes,at+1,1):256U);
        if(!w || !h)continue;
        const auto depth=cursor && file?32U:integer(bytes,at+6,2);
        const bool fits=prefer_larger?w>=width && h>=height:w<=width && h<=height;
        const auto distance=static_cast<std::uint32_t>(std::abs(static_cast<int>(w)-static_cast<int>(width))+std::abs(static_cast<int>(h)-static_cast<int>(height)));
        // 同一解析器依 API 契約選擇放大或縮小來源；尺寸相同時再比色深。
        const auto depth_distance=depth<=wanted_depth?wanted_depth-depth:1024U+depth-wanted_depth;
        if(!selected || (fits && !best_fits) || (fits==best_fits && (distance<best_distance || (distance==best_distance && depth_distance<best_depth_distance)))){
            best={at,w,h,depth,file?UINT32_MAX:integer(bytes,at+12,2)};best_fits=fits;best_distance=distance;best_depth_distance=depth_distance;selected=true;
        }
    }
    if(!selected)throw std::invalid_argument("圖示群組沒有有效尺寸");return best;
}
void finish_icon(GuiIcon& icon){
    dimensions(icon.width,icon.height);const auto count=static_cast<std::size_t>(icon.width)*icon.height;
    if(icon.rgba.size()!=count)throw std::invalid_argument("圖示像素數目不符");
    if(icon.mask_pixels.empty()){icon.mask_pixels.resize(count);for(std::size_t i=0;i<count;++i)icon.mask_pixels[i]=(icon.rgba[i]>>24)==0;}
    if(!icon.monochrome && icon.color_pixels.empty()){icon.color_pixels.resize(count);std::transform(icon.rgba.begin(),icon.rgba.end(),icon.color_pixels.begin(),premultiply);}
    if(icon.is_icon){icon.hotspot_x=icon.width/2;icon.hotspot_y=icon.height/2;}
    // 原生 AppKit／Metal UI 共用 ICO；DrawIconEx 另保留原始 AND/XOR，以支援目的像素反相。
    const auto stride=(icon.width+31)/32*4,payload=40+icon.width*icon.height*4+stride*icon.height;
    icon.image.assign(22+payload,0);put(icon.image,2,2,1);put(icon.image,4,2,1);
    put(icon.image,6,1,icon.width>=256?0:icon.width);put(icon.image,7,1,icon.height>=256?0:icon.height);
    put(icon.image,10,2,1);put(icon.image,12,2,32);put(icon.image,14,4,payload);put(icon.image,18,4,22);
    put(icon.image,22,4,40);put(icon.image,26,4,icon.width);put(icon.image,30,4,icon.height*2);put(icon.image,34,2,1);put(icon.image,36,2,32);
    for(std::uint32_t y=0;y<icon.height;++y)for(std::uint32_t x=0;x<icon.width;++x){
        const auto i=static_cast<std::size_t>(y)*icon.width+x,row=static_cast<std::size_t>(icon.height-1-y);
        const auto pixel=icon.rgba[i];put(icon.image,62+(row*icon.width+x)*4,4,(pixel&0xff00ff00U)|((pixel&255U)<<16)|((pixel>>16)&255U));
        if(icon.mask_pixels[i])icon.image[62+count*4+row*stride+x/8]|=static_cast<std::uint8_t>(0x80U>>(x%8));
    }
}
void resize_icon(GuiIcon& icon,std::uint32_t width,std::uint32_t height,bool mono){
    width=width?width:icon.width;height=height?height:icon.height;dimensions(width,height);
    for(auto& frame:icon.animation_frames)resize_icon(frame,width,height,mono);
    const auto old_width=icon.width,old_height=icon.height;const auto old_count=static_cast<std::size_t>(old_width)*old_height;
    if(width!=old_width || height!=old_height){
        const auto resize=[&]<typename T>(std::vector<T>& pixels,unsigned planes){
            if(pixels.empty())return;std::vector<T> out(static_cast<std::size_t>(width)*height*planes);
            for(unsigned p=0;p<planes;++p)for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x)
                out[static_cast<std::size_t>(p)*width*height+static_cast<std::size_t>(y)*width+x]=pixels.at(p*old_count+static_cast<std::size_t>(y)*old_height/height*old_width+static_cast<std::size_t>(x)*old_width/width);
            pixels=std::move(out);
        };
        resize(icon.rgba,1);resize(icon.color_pixels,1);resize(icon.mask_pixels,icon.monochrome?2:1);
        icon.hotspot_x=static_cast<std::uint32_t>(std::uint64_t{icon.hotspot_x}*width/old_width);icon.hotspot_y=static_cast<std::uint32_t>(std::uint64_t{icon.hotspot_y}*height/old_height);icon.width=width;icon.height=height;
    }
    if(mono && !icon.monochrome){
        const auto count=static_cast<std::size_t>(width)*height;icon.mask_pixels.resize(count*2);
        for(std::size_t i=0;i<count;++i){const auto pixel=icon.rgba[i];const auto white=((pixel&255U)*299+((pixel>>8)&255U)*587+((pixel>>16)&255U)*114)>=128000U;
            icon.mask_pixels[i]=(pixel>>24)<128;icon.mask_pixels[count+i]=icon.mask_pixels[i]?0:static_cast<std::uint8_t>(white);icon.rgba[i]=(white?0xffffffU:0U)|(icon.mask_pixels[i]?0U:0xff000000U);}
        icon.monochrome=true;icon.alpha=false;icon.color_pixels.clear();
    }
    finish_icon(icon);
}
GuiIcon decode_icon_resource(std::span<const std::uint8_t> bytes,bool is_icon,std::uint32_t width,std::uint32_t height,std::uint32_t flags){
    GuiIcon icon;icon.is_icon=is_icon;
    if(!is_icon){icon.hotspot_x=integer(bytes,0,2);icon.hotspot_y=integer(bytes,2,2);bytes=bytes.subspan(4);}
    static constexpr std::array<std::uint8_t,8> png{137,80,78,71,13,10,26,10};
    if(bytes.size()>=8 && std::equal(png.begin(),png.end(),bytes.begin())){
        auto decoded=native_png_icon(bytes);if(!decoded)throw std::invalid_argument("PNG 圖示解碼失敗");
        const auto x=icon.hotspot_x,y=icon.hotspot_y;icon=std::move(*decoded);icon.is_icon=is_icon;icon.hotspot_x=x;icon.hotspot_y=y;
    }else{
        std::vector<std::uint8_t> dib(bytes.begin(),bytes.end());auto header=integer(bytes,0,4);
        if(header==12){
            const auto w=integer(bytes,4,2),h=integer(bytes,6,2),bpp=integer(bytes,10,2),colors=bpp<=8?1U<<bpp:0U;
            if(bytes.size()<12+colors*3)throw std::invalid_argument("CORE 圖示 palette 截斷");
            dib.assign(40+colors*4+bytes.size()-12-colors*3,0);put(dib,0,4,40);put(dib,4,4,w);put(dib,8,4,h);put(dib,12,2,integer(bytes,8,2));put(dib,14,2,bpp);
            for(std::uint32_t i=0;i<colors;++i)std::copy_n(bytes.begin()+12+i*3,3,dib.begin()+40+i*4);
            std::copy(bytes.begin()+12+colors*3,bytes.end(),dib.begin()+40+colors*4);header=40;
        }
        const auto signed_height=std::bit_cast<std::int32_t>(integer(dib,8,4));const auto h=static_cast<std::uint32_t>(signed_height<0?-static_cast<std::int64_t>(signed_height):signed_height);
        const auto w=integer(dib,4,4),bpp=integer(dib,14,2),compression=integer(dib,16,4);if(h%2)throw std::invalid_argument("圖示色彩／遮罩高度不符");dimensions(w,h/2);
        const auto colors=bpp<=8?(integer(dib,32,4)?integer(dib,32,4):1U<<bpp):0U;
        const auto extra=header==40?(compression==3?12U:compression==6?16U:0U):0U;
        const auto start=static_cast<std::size_t>(header)+extra+colors*4;const auto stride=(std::uint64_t{w}*bpp+31)/32*4,mask_stride=(std::uint64_t{w}+31)/32*4;
        const auto half=h/2;const auto count=static_cast<std::size_t>(w)*half;
        if(start>dib.size() || stride*half>dib.size()-start)throw std::invalid_argument("圖示 XOR 像素截斷");
        const auto mask_start=start+stride*half;const bool has_mask=mask_start<=dib.size() && mask_stride*half<=dib.size()-mask_start;
        if(compression==6){if(header==40){dib.erase(dib.begin()+52,dib.begin()+56);}put(dib,16,4,3);}
        put(dib,8,4,std::bit_cast<std::uint32_t>(signed_height<0?-static_cast<std::int32_t>(half):static_cast<std::int32_t>(half)));
        GuiBitmap bitmap;try{bitmap=decode_dib_bitmap(dib);}catch(const HostCallFault& e){throw std::invalid_argument(e.what());}
        icon.width=w;icon.height=half;icon.monochrome=bpp==1;icon.mask_pixels.resize(count*(icon.monochrome?2U:1U));icon.rgba.resize(count);
        if(!icon.monochrome)icon.color_pixels=bitmap.pixels;
        const auto alpha_mask=compression==6 || (compression==3 && header>=56)?integer(bytes,52,4):compression==0 && bpp==32?0xff000000U:0U;
        if(bpp==32 && alpha_mask && (compression==3 || compression==6)){
            const std::array<std::uint32_t,4> masks{integer(bytes,40,4),integer(bytes,44,4),integer(bytes,48,4),alpha_mask};
            if(!valid_dib_masks(masks,32))throw std::invalid_argument("圖示 alpha bitfield 不合法");
        }
        std::vector<std::uint32_t> alpha(count);const auto original=header==40 && compression==6?bytes:std::span<const std::uint8_t>{dib};
        if(bpp==32 && alpha_mask){const auto shift=std::countr_zero(alpha_mask);const auto maximum=alpha_mask>>shift;
            for(std::uint32_t y=0;y<half;++y)for(std::uint32_t x=0;x<w;++x){const auto row=signed_height<0?y:half-1-y;const auto raw=integer(original,start+row*stride+x*4,4);alpha[static_cast<std::size_t>(y)*w+x]=static_cast<std::uint32_t>(std::uint64_t{(raw&alpha_mask)>>shift}*255/maximum);}
            icon.alpha=std::any_of(alpha.begin(),alpha.end(),[](auto a){return a!=0;});
        }
        if(!has_mask && !icon.alpha)throw std::invalid_argument("圖示 AND 遮罩截斷");
        // mask 使用原始資料位置；BI_ALPHABITFIELDS 的額外 alpha mask 已在共用 DIB 解碼前移除。
        const auto mask_bytes=header==40 && compression==6?bytes:std::span<const std::uint8_t>{dib};
        for(std::uint32_t y=0;y<half;++y)for(std::uint32_t x=0;x<w;++x){
            const auto i=static_cast<std::size_t>(y)*w+x;const auto row=signed_height<0?y:half-1-y;
            const auto and_bit=has_mask?((integer(mask_bytes,mask_start+row*mask_stride+x/8,1)>>(7-x%8))&1U):0U;
            icon.mask_pixels[i]=static_cast<std::uint8_t>(and_bit);const auto pixel=bitmap.pixels[i]&0xffffffU;
            if(icon.monochrome)icon.mask_pixels[count+i]=pixel?1:0;
            icon.rgba[i]=pixel|((icon.alpha?alpha[i]:and_bit?0U:255U)<<24);
            if(icon.alpha)icon.color_pixels[i]=premultiply(icon.rgba[i]);
        }
    }
    if(!is_icon && (icon.hotspot_x>=icon.width || icon.hotspot_y>=icon.height))throw std::invalid_argument("游標熱點超出尺寸");
    resize_icon(icon,width?width:(flags&0x40?32:icon.width),height?height:(flags&0x40?32:icon.height),(flags&1)!=0);return icon;
}

namespace {
GuiIcon single_icon_file(std::span<const std::uint8_t> bytes,bool cursor,std::uint32_t width,std::uint32_t height,std::uint32_t flags,bool prefer_larger){
    const auto type=integer(bytes,2,2);
    if(integer(bytes,0,2) || (type!=1 && type!=2))throw std::invalid_argument("不是 ICO／CUR 檔案");
    const auto requested_width=width?width:(flags&0x40)?32U:(integer(bytes,6,1)?integer(bytes,6,1):256U);
    const auto requested_height=height?height:(flags&0x40)?32U:(integer(bytes,7,1)?integer(bytes,7,1):256U);
    const auto choice=choose_icon(bytes,type==2,true,requested_width,requested_height,(flags&1)!=0,prefer_larger);
    const auto length=integer(bytes,choice.offset+8,4),offset=integer(bytes,choice.offset+12,4);
    if(length>16U*1024U*1024U || offset>bytes.size() || length>bytes.size()-offset)throw std::invalid_argument("圖示檔像素截斷");
    auto payload=bytes.subspan(offset,length);GuiIcon icon;
    if(type==2){
        std::vector<std::uint8_t> resource(4+payload.size());put(resource,0,2,integer(bytes,choice.offset+4,2));put(resource,2,2,integer(bytes,choice.offset+6,2));
        std::copy(payload.begin(),payload.end(),resource.begin()+4);icon=decode_icon_resource(resource,false,width,height,flags);
    }else icon=decode_icon_resource(payload,true,width,height,flags);
    icon.is_icon=!cursor;finish_icon(icon);return icon;
}
}
GuiIcon decode_icon_file(std::span<const std::uint8_t> bytes,bool cursor,std::uint32_t width,std::uint32_t height,std::uint32_t flags,bool prefer_larger){
    if(bytes.size()>64U*1024U*1024U)throw std::invalid_argument("圖示檔超過容量限制");
    if(bytes.size()<12 || integer(bytes,0,4)!=0x46464952)return single_icon_file(bytes,cursor,width,height,flags,prefer_larger);
    const auto end=std::uint64_t{integer(bytes,4,4)}+8;
    if(integer(bytes,8,4)!=0x4e4f4341 || end<12 || end>bytes.size())throw std::invalid_argument("ANI RIFF 格式不合法");
    std::span<const std::uint8_t> header,rates,sequence;std::vector<std::span<const std::uint8_t>> frames;
    const auto chunks=[&](std::size_t begin,std::size_t limit,auto consume){
        for(auto at=begin;at<limit;){
            const auto item=riff::chunk(bytes.subspan(at),at,limit);
            consume(item.id,static_cast<std::size_t>(item.data),bytes.subspan(static_cast<std::size_t>(item.data),item.size));at=static_cast<std::size_t>(item.next);
        }
    };
    chunks(12,static_cast<std::size_t>(end),[&](std::uint32_t tag,std::size_t at,std::span<const std::uint8_t> chunk){
        const auto unique=[&](auto& target){if(!target.empty())throw std::invalid_argument("ANI 重複控制區塊");target=chunk;};
        if(tag==0x68696e61)unique(header);else if(tag==0x65746172)unique(rates);else if(tag==0x20716573)unique(sequence);
        else if(tag==0x5453494c && chunk.size()>=4 && integer(chunk,0,4)==0x6d617266){
            chunks(at+4,at+chunk.size(),[&](std::uint32_t frame_tag,std::size_t,std::span<const std::uint8_t> frame){
                if(frame_tag!=0x6e6f6369)return;if(frames.size()>=1024)throw std::invalid_argument("ANI 影格數量過多");frames.push_back(frame);
            });
        }
    });
    if(header.size()<36 || integer(header,0,4)!=36)throw std::invalid_argument("ANI 缺少有效 anih");
    const auto frame_count=integer(header,4,4),steps=integer(header,8,4),default_rate=integer(header,28,4),attributes=integer(header,32,4);
    if(!frame_count || frame_count!=frames.size() || !steps || steps>65536 || !(attributes&1U) || (attributes&~3U) || integer(header,12,4)>4096 || integer(header,16,4)>4096)
        throw std::invalid_argument("ANI 影格／步驟格式不合法");
    if((attributes&2U)?sequence.size()<static_cast<std::size_t>(steps)*4:steps!=frame_count)throw std::invalid_argument("ANI 播放順序截斷");
    if(!rates.empty() && rates.size()<static_cast<std::size_t>(steps)*4)throw std::invalid_argument("ANI 播放速率截斷");
    std::vector<GuiIcon> decoded;std::vector<std::uint32_t> order,durations;std::size_t allocation=0;
    for(const auto frame:frames){
        auto icon=single_icon_file(frame,cursor,width,height,flags,prefer_larger);
        allocation+=icon.image.size()+icon.rgba.size()*4+icon.color_pixels.size()*4+icon.mask_pixels.size();
        if(allocation>64U*1024U*1024U)throw std::bad_alloc{};decoded.push_back(std::move(icon));
    }
    for(std::uint32_t i=0;i<steps;++i){
        const auto index=(attributes&2U)?integer(sequence,static_cast<std::size_t>(i)*4,4):i;
        if(index>=frame_count)throw std::invalid_argument("ANI 播放索引超出影格");order.push_back(index);
        const auto rate=rates.empty()?default_rate:integer(rates,static_cast<std::size_t>(i)*4,4);durations.push_back(rate?rate:6U);
    }
    auto icon=decoded[order.front()];icon.animation_frames=std::move(decoded);icon.animation_sequence=std::move(order);icon.animation_rates=std::move(durations);return icon;
}
std::uint32_t icon_animation_step(const GuiIcon& icon,std::uint64_t elapsed_ticks){
    if(icon.animation_sequence.empty())return 0;
    std::uint64_t period=0;for(const auto rate:icon.animation_rates)period+=rate;
    if(!period)return 0;
    // 每個 jiffy 為 1/60 秒；拆分餘數，避免長時間執行時乘法溢位或逐幀追趕。
    auto elapsed=((elapsed_ticks/10000000U)%period*60U+(elapsed_ticks%10000000U)*60U/10000000U)%period;
    for(std::uint32_t i=0;i<icon.animation_rates.size();++i){if(elapsed<icon.animation_rates[i])return i;elapsed-=icon.animation_rates[i];}return 0;
}

std::uint32_t IconSource::get(std::size_t at,unsigned size)const{return integer(bytes_,at,size);}
std::span<const std::uint8_t> IconSource::slice(std::size_t at,std::size_t size)const{
    if(at>bytes_.size() || size>bytes_.size()-at)throw std::invalid_argument("圖示來源越界");return bytes_.subspan(at,size);
}
std::size_t IconSource::offset(std::uint32_t rva,std::size_t size)const{
    for(unsigned i=0;i<section_count_;++i){const auto at=sections_+i*40;const auto base=get(at+12,4),length=get(at+16,4);
        if(rva>=base && rva-base<=length && size<=length-(rva-base))return static_cast<std::size_t>(std::uint64_t{get(at+20,4)}+rva-base);}
    throw std::invalid_argument("PE 圖示 RVA 超出 section");
}
std::size_t IconSource::directory(std::uint32_t relative,std::size_t size)const{
    if(relative>resource_size_ || size>resource_size_-relative || relative>UINT32_MAX-resource_rva_)throw std::invalid_argument("PE 圖示目錄越界");const auto at=offset(resource_rva_+relative,size);(void)slice(at,size);return at;
}
std::vector<IconSource::Resource> IconSource::entries(std::uint32_t relative)const{
    const auto at=directory(relative,16);const auto count=get(at+12,2)+get(at+14,2);if(count>4096)throw std::invalid_argument("PE 圖示目錄過大");(void)directory(relative,16+count*8);
    std::vector<Resource> result;for(std::uint32_t i=0;i<count;++i)result.push_back({get(at+16+i*8,4),get(at+20+i*8,4),0});return result;
}
std::span<const std::uint8_t> IconSource::data(const Resource& resource,unsigned depth)const{
    if(ne_)return slice(resource.offset,resource.size);if(depth>4)throw std::invalid_argument("PE 圖示目錄循環");
    if(resource.offset&0x80000000U){const auto children=entries(resource.offset&0x7fffffffU);if(children.empty())throw std::invalid_argument("PE 圖示語言目錄為空");return data(children.front(),depth+1);}
    const auto at=directory(resource.offset,16);const auto size=get(at+4,4);if(size>16*1024*1024)throw std::invalid_argument("圖示影像過大");return slice(offset(get(at,4),size),size);
}
IconSource::IconSource(std::span<const std::uint8_t> bytes):bytes_(bytes){
    if(bytes_.size()>=12 && get(0,4)==0x46464952 && get(8,4)==0x4e4f4341){
        const auto end=std::uint64_t{get(4,4)}+8;if(end>bytes_.size())throw std::invalid_argument("ANI RIFF 截斷");bool found=false;
        for(std::size_t at=12;at<end;){const auto item=riff::chunk(bytes_.subspan(at),at,end);
            if(item.id==riff::list_id && item.size>=4 && get(item.data,4)==0x6d617266){
                for(auto frame=item.data+4;frame<item.data+item.size;){const auto child=riff::chunk(bytes_.subspan(static_cast<std::size_t>(frame)),frame,item.data+item.size);
                    if(child.id==0x6e6f6369){bytes_=slice(static_cast<std::size_t>(child.data),child.size);found=true;break;}frame=child.next;}
            }
            if(found)break;at=static_cast<std::size_t>(item.next);
        }
        if(!found)throw std::invalid_argument("ANI 沒有圖示 frame");
    }
    if(bytes_.size()>=6 && get(0,2)==0 && (get(2,2)==1 || get(2,2)==2)){
        file_=true;cursor_=get(2,2)==2;const auto count=get(4,2);if(!count || count>4096)throw std::invalid_argument("ICO/CUR 項目數目不合法");(void)slice(6,count*16);groups_.push_back({0,0,0});return;
    }
    if(get(0,2)==0x4d42){bitmap_=true;groups_.push_back({0,0,0});return;}
    if(get(0,2)!=0x5a4d)throw std::invalid_argument("不是可擷取圖示的來源");const auto pe=get(60,4);
    if(get(pe,2)==0x454e){
        ne_=true;const auto table=static_cast<std::size_t>(pe)+get(pe+0x24,2),end=static_cast<std::size_t>(pe)+get(pe+0x26,2);const auto shift=get(table,2);
        if(shift>16 || table+2>end || end>bytes_.size())throw std::invalid_argument("NE 圖示資源表不合法");
        for(std::size_t at=table+2;at+2<=end;){const auto type=get(at,2);if(!type)break;if(at+8>end)throw std::invalid_argument("NE 類型表截斷");const auto count=get(at+2,2);
            if(count>4096 || at+8+count*12>end)throw std::invalid_argument("NE 圖示項目截斷");
            for(std::uint32_t i=0;i<count;++i){const auto item=at+8+i*12;Resource entry{get(item+6,2),get(item,2)<<shift,get(item+2,2)<<shift};if(entry.id&0x8000)entry.id&=0x7fff;else entry.id|=0x80000000U;(void)slice(entry.offset,entry.size);
                if(type==0x8003)icons_.push_back(entry);else if(type==0x800e)groups_.push_back(entry);}
            at+=8+count*12;
        }return;
    }
    if(get(pe,4)!=0x4550)throw std::invalid_argument("PE signature 不合法");section_count_=get(pe+6,2);const auto optional=static_cast<std::size_t>(pe)+24;const auto size=get(pe+20,2),magic=get(optional,2);
    if(!section_count_ || section_count_>96 || (magic!=0x10b && magic!=0x20b))throw std::invalid_argument("PE optional header 不合法");
    const auto directories=magic==0x20b?112U:96U;if(size<directories+24 || get(optional+directories-4,4)<3)throw std::invalid_argument("PE 缺少 resource directory");
    (void)slice(optional,size);sections_=optional+size;(void)slice(sections_,section_count_*40);resource_rva_=get(optional+directories+16,4);resource_size_=get(optional+directories+20,4);if(!resource_rva_ || !resource_size_)return;
    for(const auto entry:entries(0))if(entry.id==3 || entry.id==14){if(!(entry.offset&0x80000000U))throw std::invalid_argument("PE 圖示類型不是目錄");auto children=entries(entry.offset&0x7fffffffU);if(entry.id==3)icons_=std::move(children);else groups_=std::move(children);}
}
std::size_t IconSource::index(std::int32_t requested)const{
    if(requested>=0)return static_cast<std::size_t>(requested);const auto id=static_cast<std::uint64_t>(-static_cast<std::int64_t>(requested));
    for(std::size_t i=0;i<groups_.size();++i)if(!(groups_[i].id&0x80000000U) && groups_[i].id==id)return i;return groups_.size();
}
GuiIcon IconSource::extract(std::size_t index,std::uint32_t width,std::uint32_t height,std::uint32_t flags,std::uint32_t* id)const{
    if(index>=groups_.size())throw std::invalid_argument("圖示索引不存在");if(id)*id=UINT32_MAX;
    if(bitmap_){
        const auto start=get(10,4);auto dib=slice(14,bytes_.size()-14);const auto header=integer(dib,0,4),bpp=integer(dib,14,2),compression=integer(dib,16,4);
        const auto colors=bpp<=8?(integer(dib,32,4)?integer(dib,32,4):1U<<bpp):0U,packed=header+(header==40 && compression==3?12U:0U)+colors*4;
        if(start<14+packed || start>bytes_.size())throw std::invalid_argument("BMP 像素位置不合法");std::vector<std::uint8_t> packed_dib(dib.begin(),dib.begin()+packed);packed_dib.insert(packed_dib.end(),bytes_.begin()+start,bytes_.end());
        GuiBitmap bitmap;try{bitmap=decode_dib_bitmap(packed_dib);}catch(const HostCallFault& e){throw std::invalid_argument(e.what());}GuiIcon icon;icon.width=bitmap.width;icon.height=bitmap.height;icon.color_pixels=bitmap.pixels;icon.rgba=bitmap.pixels;
        for(auto& pixel:icon.rgba)pixel|=0xff000000U;resize_icon(icon,width,height?height:width,(flags&1)!=0);return icon;
    }
    const auto group=file_?bytes_:data(groups_[index]);const auto choice=choose_icon(group,cursor_,file_,width,height?height:width,(flags&1)!=0);
    std::span<const std::uint8_t> payload;
    if(file_){const auto length=integer(group,choice.offset+8,4);if(length>16*1024*1024)throw std::invalid_argument("圖示影像過大");payload=slice(integer(group,choice.offset+12,4),length);}
    else {const auto found=std::find_if(icons_.begin(),icons_.end(),[&](auto r){return r.id==choice.id;});if(found==icons_.end())throw std::invalid_argument("群組參照不存在的 RT_ICON");payload=data(*found);if(id)*id=choice.id;}
    if(cursor_){std::vector<std::uint8_t> resource(4+payload.size());put(resource,0,2,integer(group,choice.offset+4,2));put(resource,2,2,integer(group,choice.offset+6,2));std::copy(payload.begin(),payload.end(),resource.begin()+4);return decode_icon_resource(resource,false,width,height?height:width,flags);}
    return decode_icon_resource(payload,true,width,height?height:width,flags);
}
std::span<const std::uint32_t> stock_icon_ids(){
    static constexpr std::uint32_t ids[]{0,1,2,3,4,5,6,7,8,9,10,11,12,13,15,16,17,22,23,28,29,30,31,32,40,47,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,132,133,134,135,136,137,138,139,140};return ids;
}
bool stock_icon_valid(std::uint32_t id){const auto ids=stock_icon_ids();return std::binary_search(ids.begin(),ids.end(),id);}
bool is_stock_icon_path(std::u16string_view path){
    if(path.size()!=stock_icon_path.size())return false;for(std::size_t i=0;i<path.size();++i){auto c=path[i],expected=stock_icon_path[i];if(c==u'/')c=u'\\';if(c>=u'A' && c<=u'Z')c+=32;if(expected>=u'A' && expected<=u'Z')expected+=32;if(c!=expected)return false;}return true;
}
}
