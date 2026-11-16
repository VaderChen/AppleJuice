#pragma once
#include "x64arm/windows_transform.hpp"
#include "x64arm/windows_gdi_path.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

// GDI+ 筆刷的資料與逐像素著色（純運算）：BrushType Solid 0、Hatch 1、Texture 2、PathGradient 3、LinearGradient 4；
// WrapMode Tile 0、TileFlipX 1、TileFlipY 2、TileFlipXY 3、Clamp 4。顏色一律為非預乘 0xAARRGGBB。
namespace x64arm {
struct GdiplusBrush {
    unsigned type{};std::uint32_t color{};
    // 陰影線：前景為 color、背景與樣式。
    std::uint32_t back{};unsigned hatch{};
    // 漸層與材質共用：筆刷變換（筆刷空間→世界）、外圍模式、gamma 校正。
    GuiTransform transform{gui_identity};unsigned wrap{};bool gamma{};
    // Blend：只有一點時為線性內插；InterpolationColors 未設定時為空。
    std::vector<double> factors{1},positions{1};
    std::vector<std::uint32_t> preset_colors;std::vector<double> preset_positions;
    // 線性漸層：矩形與兩端色；start→end 決定漸層方向與長度（t 為投影比例）。
    std::array<double,4> rect{};std::array<std::uint32_t,2> colors{};std::array<double,2> start{},end{};
    // 路徑漸層：攤平後的邊界、原始點數、中心色與中心點、邊界色（不足時沿用最後一色）、焦點比例。
    std::vector<GuiPathPoint> path;std::size_t points{};std::uint32_t center_color{0xffffffffU};std::array<double,2> center{};
    std::vector<std::uint32_t> surround{0xffffffffU};std::array<double,2> focus{};
    // 材質：寬高與像素。
    std::uint32_t width{},height{};std::vector<std::uint32_t> pixels;
};
using GdiplusShader=std::function<std::uint32_t(double,double)>;

// 線性漸層的方向：矩形四角在方向向量上的投影最小者為 t=0、最大者為 t=1；
// 可縮放角度先在單位正方形中取方向，再隨矩形長寬伸縮（45 度即為對角線漸層）。
inline void gdiplus_line_direction(GdiplusBrush& brush,double degrees,bool scalable){
    const auto& r=brush.rect;if(!(r[2]>0) || !(r[3]>0))throw std::invalid_argument("GDI+ 漸層矩形無效");
    const auto radians=degrees*3.14159265358979323846/180;
    const std::array<double,2> d{std::cos(radians)/(scalable?r[2]:1),std::sin(radians)/(scalable?r[3]:1)};
    const std::array<std::array<double,2>,4> corners{{{r[0],r[1]},{r[0]+r[2],r[1]},{r[0],r[1]+r[3]},{r[0]+r[2],r[1]+r[3]}}};
    double low=std::numeric_limits<double>::infinity(),high=-low;std::array<double,2> origin{};
    for(const auto& c:corners){const auto p=c[0]*d[0]+c[1]*d[1];if(p<low){low=p;origin=c;}high=std::max(high,p);}
    // 梯度向量 G＝d／(high−low)；end＝start＋G／|G|² 使投影比例恰為 t。
    const std::array<double,2> gradient{d[0]/(high-low),d[1]/(high-low)};const auto length=gradient[0]*gradient[0]+gradient[1]*gradient[1];
    brush.start=origin;brush.end={origin[0]+gradient[0]/length,origin[1]+gradient[1]/length};
}
// 兩點建立的線性漸層矩形：取外接矩形，水平或垂直時另一邊取同長並置中。
inline std::array<double,4> gdiplus_line_rect(const std::array<double,2>& a,const std::array<double,2>& b){
    double x=std::min(a[0],b[0]),y=std::min(a[1],b[1]),w=std::abs(b[0]-a[0]),h=std::abs(b[1]-a[1]);
    if(w==0 && h==0)throw std::invalid_argument("GDI+ 漸層兩點相同");
    if(w==0){w=h;x-=w/2;}if(h==0){h=w;y-=h/2;}return {x,y,w,h};
}
inline double gdiplus_wrap_position(double t,unsigned wrap,bool flip){
    if(wrap==4)return std::clamp(t,0.0,1.0);const auto whole=std::floor(t);auto f=t-whole;
    if(flip && (static_cast<std::int64_t>(whole)&1))f=1-f;return f;
}
inline std::uint32_t gdiplus_lerp(std::uint32_t a,std::uint32_t b,double f,bool gamma){
    f=std::clamp(f,0.0,1.0);std::uint32_t out=0;
    for(unsigned shift=0;shift<32;shift+=8){
        double x=((a>>shift)&255U)/255.0,y=((b>>shift)&255U)/255.0;const bool linear=gamma && shift<24;
        if(linear){x=std::pow(x,2.2);y=std::pow(y,2.2);}auto v=x+(y-x)*f;if(linear)v=std::pow(v,1/2.2);
        out|=static_cast<std::uint32_t>(std::lround(std::clamp(v,0.0,1.0)*255))<<shift;
    }
    return out;
}
inline double gdiplus_piecewise(const std::vector<double>& positions,const std::vector<double>& values,double p){
    if(positions.size()<2)return p;
    for(std::size_t i=1;i<positions.size();++i)if(p<=positions[i] || i+1==positions.size()){
        const auto span=positions[i]-positions[i-1];const auto f=span>0?(p-positions[i-1])/span:1;return values[i-1]+(values[i]-values[i-1])*std::clamp(f,0.0,1.0);}
    return values.back();
}
// 漸層位置 p（線性：起點 0→終點 1；路徑：邊界 0→中心 1）的顏色：InterpolationColors 優先，否則依 Blend 比例在兩色間內插。
inline std::uint32_t gdiplus_gradient(const GdiplusBrush& brush,double p,std::uint32_t from,std::uint32_t to){
    if(brush.preset_colors.size()>=2){
        const auto& at=brush.preset_positions;for(std::size_t i=1;i<at.size();++i)if(p<=at[i] || i+1==at.size()){
            const auto span=at[i]-at[i-1];return gdiplus_lerp(brush.preset_colors[i-1],brush.preset_colors[i],span>0?(p-at[i-1])/span:1,brush.gamma);}
    }
    return gdiplus_lerp(from,to,gdiplus_piecewise(brush.positions,brush.factors,p),brush.gamma);
}
// Blend 形狀：三角（focus 處為 scale）與鐘形（以常態累積分布近似，兩側各 16 段）。
inline void gdiplus_triangular_blend(GdiplusBrush& brush,double focus,double scale){
    if(focus<0 || focus>1 || scale<0 || scale>1)throw std::invalid_argument("GDI+ blend 參數無效");
    if(focus==0){brush.positions={0,1};brush.factors={scale,0};}
    else if(focus==1){brush.positions={0,1};brush.factors={0,scale};}
    else {brush.positions={0,focus,1};brush.factors={0,scale,0};}
    brush.preset_colors.clear();brush.preset_positions.clear();
}
inline void gdiplus_bell_blend(GdiplusBrush& brush,double focus,double scale){
    if(focus<0 || focus>1 || scale<0 || scale>1)throw std::invalid_argument("GDI+ blend 參數無效");
    constexpr unsigned steps=16;const auto curve=[](double u){const auto e=std::erf((u-0.5)*4),low=std::erf(-2.0),high=std::erf(2.0);return (e-low)/(high-low);};
    brush.positions.clear();brush.factors.clear();
    if(focus>0)for(unsigned i=0;i<steps;++i){const auto u=static_cast<double>(i)/steps;brush.positions.push_back(u*focus);brush.factors.push_back(scale*curve(u));}
    brush.positions.push_back(focus);brush.factors.push_back(scale);
    if(focus<1)for(unsigned i=1;i<=steps;++i){const auto u=static_cast<double>(i)/steps;brush.positions.push_back(focus+u*(1-focus));brush.factors.push_back(scale*curve(1-u));}
    if(brush.positions.front()>0){brush.positions.insert(brush.positions.begin(),0);brush.factors.insert(brush.factors.begin(),brush.factors.front());}
    brush.preset_colors.clear();brush.preset_positions.clear();
}
// 陰影線 8×8 圖樣：依 HatchStyle 名稱描述的線條、點與網格以程式產生；百分比樣式採 8×8 有序抖動。
inline bool gdiplus_hatch_bit(unsigned style,unsigned x,unsigned y){
    const auto bayer=[](unsigned px,unsigned py){unsigned v=0;for(unsigned bit=0;bit<3;++bit){const auto a=(px>>bit)&1U,b=(py>>bit)&1U;v|=(((a^b)<<1)|b)<<(2*(2-bit));}return v;};
    const auto down=(x+8-y)&7U,up=(x+y)&7U;
    static constexpr unsigned percent[]{3,6,13,16,19,26,32,38,45,48,51,58};
    switch(style){
    case 0:return y==0;case 1:return x==0;case 2:return down==0;case 3:return up==7;case 4:return x==0 || y==0;case 5:return down==0 || up==7;
    case 18:return (down&3U)==0;case 19:return (up&3U)==3;case 20:return (down&3U)<2;case 21:return (up&3U)>=2;case 22:return down<3;case 23:return up>=5;
    case 24:return (x&3U)==0;case 25:return (y&3U)==0;case 26:return (x&1U)==0;case 27:return (y&1U)==0;case 28:return (x&3U)<2;case 29:return (y&3U)<2;
    case 30:return (down&3U)==0 && y<4;case 31:return (up&3U)==3 && y<4;case 32:return (y==0 && x<4) || (y==4 && x>=4);case 33:return (x==0 && y<4) || (x==4 && y>=4);
    case 34:return x==((y*5U)&7U);case 35:{const auto bx=x/2,by=y/2;return bx==((by*3U+1)&3U);}
    case 36:{const auto phase=x&7U;return (y&3U)==(phase<4?phase:7-phase);}
    case 37:{static constexpr unsigned wave[]{0,0,1,2,2,1,0,0};return (y&3U)==wave[x]%4 && (y<4);}
    case 38:return up==7 || (down==4 && ((x&4U)!=0));case 39:return (y&3U)==0 || (y<4?x==0:x==4);
    case 40:return ((x+y)&3U)==0 || (((x+8-y)&7U)==4 && ((x&2U)!=0));case 41:return y<2 || ((x&3U)<2 && (y&1U)==0);
    case 42:return (x==2 && y==1) || (x==3 && y==2) || (x==6 && y==5) || (x==5 && y==6);case 43:return (y==0 && (x&1U)==0) || (x==0 && (y&1U)==0);
    case 44:return (down==0 || up==0) && (x&1U)==0;case 45:return y==0 || (y<4?x==y:x==11-y);case 46:return ((x+y)&1U)==0 && ((y&1U)==0 || (x&3U)==0);
    case 47:{const auto dx=static_cast<int>(x)-3,dy=static_cast<int>(y)-3;return dx*dx+dy*dy<=9 && !(dx<=-1 && dy<=-1);}
    case 48:return (x&3U)==0 || (y&3U)==0;case 49:return (((x/2)+(y/2))&1U)==0;case 50:return (((x/4)+(y/4))&1U)==0;
    case 51:return down==4 || up==3;case 52:{const auto dx=std::abs(static_cast<int>(x)*2-7),dy=std::abs(static_cast<int>(y)*2-7);return dx+dy<=8;}
    default:if(style>=6 && style<=17)return bayer(x,y)<percent[style-6];return false;
    }
}
// 射線（中心→q）與多邊形邊界的最近交點：回傳（射線參數 s, 邊界起點索引, 邊上比例）；q 在交點處時 s 為 1。
inline std::optional<std::array<double,3>> gdiplus_ray_hit(const std::vector<GuiPathPoint>& path,const std::array<double,2>& c,double dx,double dy,std::array<double,2> scale){
    std::optional<std::array<double,3>> best;std::size_t begin=0;
    const auto vertex=[&](std::size_t i){return std::array<double,2>{c[0]+(path[i].x-c[0])*scale[0],c[1]+(path[i].y-c[1])*scale[1]};};
    for(std::size_t i=0;i<path.size();++i){
        if((path[i].type&6)==6)begin=i;
        const bool last=i+1==path.size() || (path[i+1].type&6)==6;const auto j=last?begin:i+1;if(j==i)continue;
        const auto a=vertex(i),b=vertex(j);const auto ex=b[0]-a[0],ey=b[1]-a[1],den=dx*ey-dy*ex;if(den==0)continue;
        const auto wx=a[0]-c[0],wy=a[1]-c[1];const auto s=(wx*ey-wy*ex)/den,u=(wx*dy-wy*dx)/den;
        if(s>1e-12 && u>=-1e-12 && u<=1+1e-12 && (!best || s<(*best)[0]))best=std::array<double,3>{s,static_cast<double>(i),std::clamp(u,0.0,1.0)};
    }
    return best;
}
inline std::optional<std::uint32_t> gdiplus_path_gradient(const GdiplusBrush& brush,double x,double y){
    const auto& c=brush.center;const auto dx=x-c[0],dy=y-c[1];
    if(dx==0 && dy==0)return gdiplus_gradient(brush,1,brush.center_color,brush.center_color);
    const auto hit=gdiplus_ray_hit(brush.path,c,dx,dy,{1,1});if(!hit || (*hit)[0]<1-1e-9)return std::nullopt;
    const auto vertex=static_cast<std::size_t>((*hit)[1]);std::size_t next=vertex+1,begin=vertex;
    while(begin>0 && (brush.path[begin].type&6)!=6)--begin;if(next>=brush.path.size() || (brush.path[next].type&6)==6)next=begin;
    // 邊界色依原始點索引；有曲線攤平時按比例對應。
    const auto surround=[&](std::size_t i){std::size_t index=i;if(brush.points && brush.path.size()!=brush.points)index=i*brush.points/brush.path.size();
        return brush.surround[std::min(index,brush.surround.size()-1)];};
    const auto edge=gdiplus_lerp(surround(vertex),surround(next),(*hit)[2],false);
    // 焦點比例：內縮邊界以內為中心色，其外重新正規化。
    auto ratio=1/(*hit)[0];
    if(brush.focus[0]>0 || brush.focus[1]>0){const auto inner=gdiplus_ray_hit(brush.path,c,dx,dy,brush.focus);
        if(inner){const auto outer=(*hit)[0],focus=(*inner)[0];if(focus>=1)return brush.center_color;ratio=(1-focus)/(outer-focus);}}
    return gdiplus_gradient(brush,1-std::clamp(ratio,0.0,1.0),edge,brush.center_color);
}
inline std::uint32_t gdiplus_texture(const GdiplusBrush& brush,double u,double v){
    if(!brush.width || !brush.height)return 0;
    const auto w=static_cast<std::int64_t>(brush.width),h=static_cast<std::int64_t>(brush.height);
    const auto ix=static_cast<std::int64_t>(std::floor(u)),iy=static_cast<std::int64_t>(std::floor(v));
    if(brush.wrap==4 && (ix<0 || iy<0 || ix>=w || iy>=h))return 0;
    const auto tile_x=ix>=0?ix/w:-((-ix-1)/w)-1,tile_y=iy>=0?iy/h:-((-iy-1)/h)-1;auto px=ix-tile_x*w,py=iy-tile_y*h;
    if((brush.wrap==1 || brush.wrap==3) && (tile_x&1))px=w-1-px;if((brush.wrap==2 || brush.wrap==3) && (tile_y&1))py=h-1-py;
    return brush.pixels[static_cast<std::size_t>(py*w+px)];
}
// 裝置像素中心→顏色；陰影線對齊裝置像素與 Graphics 的繪製原點，其餘依（筆刷變換·世界→裝置）的反變換回到筆刷空間。
inline GdiplusShader gdiplus_shader(const GdiplusBrush& brush,const GuiTransform& world_to_device,std::int32_t origin_x,std::int32_t origin_y){
    if(brush.type==0)return [color=brush.color](double,double){return color;};
    if(brush.type==1)return [brush,origin_x,origin_y](double x,double y){
        const auto px=static_cast<unsigned>((static_cast<std::int64_t>(std::floor(x))-origin_x)&7),py=static_cast<unsigned>((static_cast<std::int64_t>(std::floor(y))-origin_y)&7);
        return gdiplus_hatch_bit(brush.hatch,px,py)?brush.color:brush.back;};
    const auto inverse=gui_inverse(gui_multiply(brush.transform,world_to_device));if(!inverse)return [](double,double){return 0U;};
    return [brush,m=*inverse](double x,double y)->std::uint32_t{
        const auto p=gui_point(m,x,y);
        if(brush.type==2)return gdiplus_texture(brush,p[0],p[1]);
        if(brush.type==3)return gdiplus_path_gradient(brush,p[0],p[1]).value_or(0U);
        const auto dx=brush.end[0]-brush.start[0],dy=brush.end[1]-brush.start[1];
        const auto t=((p[0]-brush.start[0])*dx+(p[1]-brush.start[1])*dy)/(dx*dx+dy*dy);
        return gdiplus_gradient(brush,gdiplus_wrap_position(t,brush.wrap,brush.wrap==1 || brush.wrap==3),brush.colors[0],brush.colors[1]);
    };
}
}
