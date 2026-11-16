#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

// GDI+ PixelFormat 的編解碼（純運算）：顏色一律以非預乘 0xAARRGGBB 交換。
// 格式值的位元：8～15 為每像素位元數，0x10000 索引色、0x20000 GDI 相容、0x40000 含 alpha、0x80000 預乘、0x100000 擴充（16 位元通道）。
namespace x64arm::gdiplus_pixel {
constexpr std::uint32_t indexed1=0x00030101U,indexed4=0x00030402U,indexed8=0x00030803U,gray16=0x00101004U,rgb555=0x00021005U,rgb565=0x00021006U,
    argb1555=0x00061007U,rgb24=0x00021808U,rgb32=0x00022009U,argb32=0x0026200aU,pargb32=0x000e200bU,rgb48=0x0010300cU,argb64=0x0034400dU,pargb64=0x001a400eU;
inline unsigned bits(std::uint32_t format){return (format>>8)&255U;}
inline bool supported(std::uint32_t format){
    switch(format){case indexed1:case indexed4:case indexed8:case gray16:case rgb555:case rgb565:case argb1555:case rgb24:case rgb32:case argb32:case pargb32:case rgb48:case argb64:case pargb64:return true;default:return false;}
}
inline bool indexed(std::uint32_t format){return (format&0x10000U)!=0;}
inline bool alpha(std::uint32_t format){return (format&0x40000U)!=0;}
// 每列位元組數對齊 4。
inline std::int64_t stride(std::uint32_t format,std::uint32_t width){return (static_cast<std::int64_t>(width)*bits(format)+31)/32*4;}
inline std::uint32_t premultiply(std::uint32_t argb){
    const auto a=argb>>24;if(a==255)return argb;
    const auto scale=[&](unsigned shift){return ((((argb>>shift)&255U)*a+127U)/255U)<<shift;};
    return (a<<24)|scale(16)|scale(8)|scale(0);
}
inline std::uint32_t unpremultiply(std::uint32_t pargb){
    const auto a=pargb>>24;if(a==255 || !a)return a?pargb:0;
    const auto scale=[&](unsigned shift){return std::min(255U,(((pargb>>shift)&255U)*255U+a/2U)/a)<<shift;};
    return (a<<24)|scale(16)|scale(8)|scale(0);
}
// 48／64 位元格式依 GDI+ 慣例為線性 gamma、每通道 0～8192；與 8 位元 sRGB 之間以 gamma 2.2 轉換（alpha 為線性）。
inline std::uint16_t wide_channel(std::uint32_t v,bool linear){return static_cast<std::uint16_t>(std::lround((linear?static_cast<double>(v)/255:std::pow(static_cast<double>(v)/255,2.2))*8192));}
inline std::uint32_t narrow_channel(std::uint32_t v,bool linear){
    const auto x=std::clamp(static_cast<double>(v)/8192,0.0,1.0);return static_cast<std::uint32_t>(std::lround((linear?x:std::pow(x,1/2.2))*255));}
// 調色盤最近色（RGB 距離優先，alpha 一併比較）。
inline std::size_t nearest(std::span<const std::uint32_t> palette,std::uint32_t argb){
    std::size_t best=0;std::int64_t best_distance=INT64_MAX;
    for(std::size_t i=0;i<palette.size();++i){std::int64_t d=0;for(unsigned shift=0;shift<32;shift+=8){const auto a=static_cast<std::int64_t>((palette[i]>>shift)&255U)-static_cast<std::int64_t>((argb>>shift)&255U);d+=a*a*(shift==24?4:1);}
        if(d<best_distance){best=i;best_distance=d;if(!d)break;}}
    return best;
}
inline std::uint32_t load(std::span<const std::uint8_t> row,std::size_t at,unsigned bytes){std::uint32_t v=0;for(unsigned i=0;i<bytes;++i)v|=std::uint32_t{row[at+i]}<<(8*i);return v;}
inline void put(std::span<std::uint8_t> row,std::size_t at,unsigned bytes,std::uint64_t v){for(unsigned i=0;i<bytes;++i)row[at+i]=static_cast<std::uint8_t>(v>>(8*i));}
// row 為一整列（或至少涵蓋第 x 個像素）的原始位元組。
inline std::uint32_t decode(std::uint32_t format,std::span<const std::uint8_t> row,std::uint32_t x,std::span<const std::uint32_t> palette){
    const auto entry=[&](std::size_t i)->std::uint32_t{return i<palette.size()?palette[i]:0xff000000U;};
    switch(format){
    case indexed1:return entry((row[x/8]>>(7-x%8))&1U);
    case indexed4:return entry((row[x/2]>>(x%2?0:4))&15U);
    case indexed8:return entry(row[x]);
    case gray16:{const auto v=load(row,x*2,2)>>8;return 0xff000000U|(v<<16)|(v<<8)|v;}
    case rgb555:case argb1555:{const auto v=load(row,x*2,2);const auto c=[&](unsigned s){const auto f=(v>>s)&31U;return (f<<3)|(f>>2);};
        return ((format==argb1555 && !(v&0x8000U))?0U:0xff000000U)|(c(10)<<16)|(c(5)<<8)|c(0);}
    case rgb565:{const auto v=load(row,x*2,2);const auto r=(v>>11)&31U,g=(v>>5)&63U,b=v&31U;return 0xff000000U|(((r<<3)|(r>>2))<<16)|(((g<<2)|(g>>4))<<8)|((b<<3)|(b>>2));}
    case rgb24:return 0xff000000U|load(row,x*3,3);
    case rgb32:return 0xff000000U|load(row,x*4,4);
    case argb32:return load(row,x*4,4);
    case pargb32:return unpremultiply(load(row,x*4,4));
    case rgb48:{std::uint32_t out=0xff000000U;for(unsigned i=0;i<3;++i)out|=narrow_channel(load(row,x*6+i*2,2),false)<<(8*i);return out;}
    case argb64:case pargb64:{
        std::array<double,4> c{};for(unsigned i=0;i<4;++i)c[i]=static_cast<double>(load(row,x*8+i*2,2))/8192;
        if(format==pargb64 && c[3]>0)for(unsigned i=0;i<3;++i)c[i]/=c[3];
        std::uint32_t out=static_cast<std::uint32_t>(std::lround(std::clamp(c[3],0.0,1.0)*255))<<24;
        for(unsigned i=0;i<3;++i)out|=static_cast<std::uint32_t>(std::lround(std::pow(std::clamp(c[i],0.0,1.0),1/2.2)*255))<<(8*i);return out;}
    default:return 0;
    }
}
inline void encode(std::uint32_t format,std::span<std::uint8_t> row,std::uint32_t x,std::uint32_t argb,std::span<const std::uint32_t> palette){
    const auto r=(argb>>16)&255U,g=(argb>>8)&255U,b=argb&255U,a=argb>>24;
    switch(format){
    case indexed1:{const auto i=static_cast<std::uint8_t>(nearest(palette,argb)&1U);auto& byte=row[x/8];const auto bit=static_cast<std::uint8_t>(0x80U>>(x%8));byte=static_cast<std::uint8_t>(i?byte|bit:byte&~static_cast<unsigned>(bit));return;}
    case indexed4:{const auto i=static_cast<std::uint8_t>(nearest(palette,argb)&15U);auto& byte=row[x/2];byte=static_cast<std::uint8_t>(x%2?(byte&0xf0U)|i:(byte&0x0fU)|(static_cast<unsigned>(i)<<4));return;}
    case indexed8:row[x]=static_cast<std::uint8_t>(nearest(palette,argb)&255U);return;
    case gray16:{const auto y=(r*299+g*587+b*114+500)/1000;put(row,x*2,2,y*257);return;}
    case rgb555:case argb1555:put(row,x*2,2,((format==argb1555 && a>=128)?0x8000U:0U)|((r>>3)<<10)|((g>>3)<<5)|(b>>3));return;
    case rgb565:put(row,x*2,2,((r>>3)<<11)|((g>>2)<<5)|(b>>3));return;
    case rgb24:put(row,x*3,3,argb&0xffffffU);return;
    case rgb32:put(row,x*4,4,argb|0xff000000U);return;
    case argb32:put(row,x*4,4,argb);return;
    case pargb32:put(row,x*4,4,premultiply(argb));return;
    case rgb48:for(unsigned i=0;i<3;++i)put(row,x*6+i*2,2,wide_channel((argb>>(8*i))&255U,false));return;
    case argb64:case pargb64:{
        const auto alpha_value=static_cast<double>(a)/255;
        for(unsigned i=0;i<3;++i){auto v=std::pow(static_cast<double>((argb>>(8*i))&255U)/255,2.2);if(format==pargb64)v*=alpha_value;put(row,x*8+i*2,2,static_cast<std::uint64_t>(std::lround(v*8192)));}
        put(row,x*8+6,2,wide_channel(a,true));return;}
    default:return;
    }
}
// 預設調色盤：VGA 16 色；半色調為 16 系統色加 N³ 色（252 為 6×7×6、256 為 8×8×4，不含系統色）。
inline std::vector<std::uint32_t> system_colors(){
    return {0xff000000U,0xff800000U,0xff008000U,0xff808000U,0xff000080U,0xff800080U,0xff008080U,0xffc0c0c0U,
            0xff808080U,0xffff0000U,0xff00ff00U,0xffffff00U,0xff0000ffU,0xffff00ffU,0xff00ffffU,0xffffffffU};
}
inline std::vector<std::uint32_t> halftone(unsigned red,unsigned green,unsigned blue,bool with_system){
    std::vector<std::uint32_t> out=with_system?system_colors():std::vector<std::uint32_t>{};
    const auto level=[](unsigned i,unsigned n){return n>1?(i*255U+(n-1)/2)/(n-1):0U;};
    for(unsigned b=0;b<blue;++b)for(unsigned g=0;g<green;++g)for(unsigned r=0;r<red;++r)out.push_back(0xff000000U|(level(r,red)<<16)|(level(g,green)<<8)|level(b,blue));
    return out;
}
// PaletteType：FixedBW 2、FixedHalftone8 3、27 4、64 5、125 6、216 7、252 8、256 9。
inline std::vector<std::uint32_t> fixed_palette(unsigned type){
    switch(type){
    case 2:return {0xff000000U,0xffffffffU};case 3:return halftone(2,2,2,true);case 4:return halftone(3,3,3,true);case 5:return halftone(4,4,4,true);
    case 6:return halftone(5,5,5,true);case 7:return halftone(6,6,6,true);case 8:return halftone(6,7,6,false);case 9:return halftone(8,8,4,false);default:return {};
    }
}
// 新建索引色點陣圖的調色盤：1bpp 黑白、4bpp VGA 16 色、8bpp 為 216 半色調（含系統色）補滿 256 色。
inline std::vector<std::uint32_t> default_palette(std::uint32_t format){
    if(format==indexed1)return fixed_palette(2);if(format==indexed4)return system_colors();
    if(format==indexed8){auto out=fixed_palette(7);out.resize(256,0xff000000U);return out;}return {};
}
// 最佳化調色盤：以中位數切割把顏色分成 count 群取平均（保留透明色時第 0 色為透明）。
inline std::vector<std::uint32_t> optimal_palette(std::vector<std::uint32_t> colors,std::size_t count,bool transparent){
    std::vector<std::uint32_t> out;if(transparent && count){out.push_back(0);--count;}
    if(colors.empty() || !count)return out;
    std::vector<std::pair<std::size_t,std::size_t>> boxes{{0,colors.size()}};
    const auto channel=[](std::uint32_t c,unsigned k){return (c>>(8*k))&255U;};
    while(boxes.size()<count){
        std::size_t pick=boxes.size();unsigned axis=0,range=0;
        for(std::size_t i=0;i<boxes.size();++i){const auto [b,e]=boxes[i];if(e-b<2)continue;
            for(unsigned k=0;k<3;++k){unsigned low=255,high=0;for(auto j=b;j<e;++j){low=std::min(low,channel(colors[j],k));high=std::max(high,channel(colors[j],k));}
                if(high-low>range){range=high-low;axis=k;pick=i;}}}
        if(pick==boxes.size() || !range)break;
        const auto [b,e]=boxes[pick];std::sort(colors.begin()+static_cast<std::ptrdiff_t>(b),colors.begin()+static_cast<std::ptrdiff_t>(e),[&](auto x,auto y){return channel(x,axis)<channel(y,axis);});
        const auto middle=b+(e-b)/2;boxes[pick]={b,middle};boxes.push_back({middle,e});
    }
    for(const auto& [b,e]:boxes){std::array<std::uint64_t,3> sum{};for(auto j=b;j<e;++j)for(unsigned k=0;k<3;++k)sum[k]+=channel(colors[j],k);
        std::uint32_t c=0xff000000U;for(unsigned k=0;k<3;++k)c|=static_cast<std::uint32_t>(sum[k]/(e-b))<<(8*k);out.push_back(c);}
    return out;
}
}
