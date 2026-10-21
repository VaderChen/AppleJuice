#include "x64arm/windows_gdi_path.hpp"
#include "x64arm/windows_transform.hpp"
#include <cmath>
#include <functional>
#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#endif
namespace x64arm {
namespace {
void append(std::vector<GuiPathPoint>& points,double x,double y,unsigned type){
    if(!std::isfinite(x) || !std::isfinite(y) || std::abs(x)>=(1<<26) || std::abs(y)>=(1<<26))throw std::invalid_argument("GDI path 座標超出範圍");
    if(points.size()>=65536)throw std::length_error("GDI path 頂點超出上限");points.push_back({x,y,static_cast<std::uint8_t>(type)});
}
#if defined(__APPLE__)
struct PathOwner{CGPathRef value;~PathOwner(){if(value)CGPathRelease(value);}};
CGPathRef path(const std::vector<GuiPathPoint>& points){
    auto result=CGPathCreateMutable();if(!result)throw std::bad_alloc();
    for(std::size_t i=0;i<points.size();++i){const auto& p=points[i];
        if((p.type&6)==6)CGPathMoveToPoint(result,nullptr,p.x,p.y);
        else if((p.type&6)==2)CGPathAddLineToPoint(result,nullptr,p.x,p.y);
        else if((p.type&6)==4 && i+2<points.size()){const auto& q=points[i+1];const auto& r=points[i+2];CGPathAddCurveToPoint(result,nullptr,p.x,p.y,q.x,q.y,r.x,r.y);i+=2;}
        else {CGPathRelease(result);throw std::invalid_argument("GDI path 頂點類型不合法");}
        if(points[i].type&1)CGPathCloseSubpath(result);
    }return result;
}
CGAffineTransform affine(const GuiTransform& t){return CGAffineTransformMake(t[0],t[1],t[2],t[3],t[4],t[5]);}
#endif
}
std::vector<GuiPathPoint> gui_flatten_path(const std::vector<GuiPathPoint>& input){
    std::vector<GuiPathPoint> output;GuiPathPoint current{},start{};
    const auto middle=[](const GuiPathPoint& a,const GuiPathPoint& b){return GuiPathPoint{(a.x+b.x)/2,(a.y+b.y)/2,2};};
    std::function<void(GuiPathPoint,GuiPathPoint,GuiPathPoint,GuiPathPoint,unsigned)> curve;
    curve=[&](auto a,auto b,auto c,auto d,unsigned depth){
        const auto distance=[&](const auto& p){const double dx=d.x-a.x,dy=d.y-a.y,length=dx*dx+dy*dy;if(length==0)return std::hypot(p.x-a.x,p.y-a.y);const double t=std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/length,0.0,1.0);return std::hypot(p.x-a.x-t*dx,p.y-a.y-t*dy);};
        if(std::max(distance(b),distance(c))<=0.125){append(output,d.x,d.y,2);return;}
        if(depth>=24)throw std::length_error("GDI Bezier 細分超過上限");
        const auto ab=middle(a,b),bc=middle(b,c),cd=middle(c,d),abc=middle(ab,bc),bcd=middle(bc,cd),split=middle(abc,bcd);
        curve(a,ab,abc,split,depth+1);curve(split,bcd,cd,d,depth+1);
    };
    for(std::size_t i=0;i<input.size();++i){auto p=input[i];const auto type=p.type&6;
        if(type==6){append(output,p.x,p.y,6);start=p;}
        else if(type==2)append(output,p.x,p.y,2);
        else if(type==4 && i+2<input.size()){curve(current,p,input[i+1],input[i+2],0);p=input[i+2];i+=2;}
        else throw std::invalid_argument("GDI path 不合法");current=p;
        if(p.type&1){output.back().type|=1;current=start;}
    }return output;
}
GuiRegion native_path_region(const std::vector<GuiPathPoint>& input,unsigned fill_mode){
#if defined(__APPLE__)
    if(input.empty())return {};PathOwner source{path(input)};const auto box=CGPathGetPathBoundingBox(source.value);if(CGRectIsNull(box) || CGRectIsEmpty(box))return {};
    const double l=std::floor(CGRectGetMinX(box)),t=std::floor(CGRectGetMinY(box)),r=std::ceil(CGRectGetMaxX(box)),b=std::ceil(CGRectGetMaxY(box));
    if(l<=-(1<<26) || t<=-(1<<26) || r>=(1<<26) || b>=(1<<26))throw std::invalid_argument("GDI region 座標超出範圍");
    const auto width=static_cast<std::size_t>(r-l),height=static_cast<std::size_t>(b-t);
    if(!width || !height)return {};if(width>32768 || height>32768 || width*height>64*1024*1024)throw std::length_error("GDI path 遮罩超過上限");
    std::vector<std::uint8_t> mask(width*height);auto space=CGColorSpaceCreateDeviceGray();auto context=CGBitmapContextCreate(mask.data(),width,height,8,width,space,kCGImageAlphaNone);CGColorSpaceRelease(space);if(!context)throw std::bad_alloc();
    CGContextSetShouldAntialias(context,false);CGContextTranslateCTM(context,-l,-t);CGContextSetGrayFillColor(context,1,1);CGContextAddPath(context,source.value);CGContextDrawPath(context,fill_mode==1?kCGPathEOFill:kCGPathFill);CGContextRelease(context);
    GuiRegion output;std::vector<std::array<int,2>> previous;std::size_t previous_start=0;
    for(std::size_t y=0;y<height;++y){std::vector<std::array<int,2>> spans;
        for(std::size_t x=0;x<width;){while(x<width && !mask[(height-1-y)*width+x])++x;const auto first=x;while(x<width && mask[(height-1-y)*width+x])++x;if(first<x)spans.push_back({static_cast<int>(l)+static_cast<int>(first),static_cast<int>(l)+static_cast<int>(x)});}
        if(!spans.empty() && spans==previous){for(std::size_t i=previous_start;i<output.size();++i)output[i][3]=static_cast<int>(t)+static_cast<int>(y)+1;}
        else {previous_start=output.size();for(const auto& span:spans)output.push_back({span[0],static_cast<int>(t)+static_cast<int>(y),span[1],static_cast<int>(t)+static_cast<int>(y)+1});}
        previous=std::move(spans);if(output.size()>4096)throw std::length_error("GDI region 超過矩形上限");
    }return output;
#else
    (void)input;(void)fill_mode;throw std::domain_error("本平台無原生 path 後端");
#endif
}
std::vector<GuiPathPoint> native_stroke_path(const std::vector<GuiPathPoint>& input,const GuiTransform& transform,double width,unsigned style,double miter){
#if defined(__APPLE__)
    if(input.empty() || width<=0)return {};const auto inverse=gui_inverse(transform);if(!inverse)throw std::invalid_argument("GDI stroke 矩陣不可逆");
    PathOwner device{path(input)};auto inv=affine(*inverse),matrix=affine(transform);PathOwner logical{CGPathCreateCopyByTransformingPath(device.value,&inv)};if(!logical.value)throw std::bad_alloc();
    const auto cap=(style&0xf00)==0x100?kCGLineCapSquare:(style&0xf00)==0x200?kCGLineCapButt:kCGLineCapRound;
    const auto join=(style&0xf000)==0x1000?kCGLineJoinBevel:(style&0xf000)==0x2000?kCGLineJoinMiter:kCGLineJoinRound;
    PathOwner stroke{CGPathCreateCopyByStrokingPath(logical.value,nullptr,width,cap,join,miter)};if(!stroke.value)throw std::bad_alloc();
    PathOwner mapped{CGPathCreateCopyByTransformingPath(stroke.value,&matrix)};if(!mapped.value)throw std::bad_alloc();
    struct Collector{std::vector<GuiPathPoint> points;std::exception_ptr error;};Collector collector;
    CGPathApply(mapped.value,&collector,[](void* opaque,const CGPathElement* element){auto& c=*static_cast<Collector*>(opaque);if(c.error)return;try{
        const auto p=element->points;
        if(element->type==kCGPathElementMoveToPoint)append(c.points,p[0].x,p[0].y,6);
        else if(element->type==kCGPathElementAddLineToPoint)append(c.points,p[0].x,p[0].y,2);
        else if(element->type==kCGPathElementAddCurveToPoint)for(unsigned i=0;i<3;++i)append(c.points,p[i].x,p[i].y,4);
        else if(element->type==kCGPathElementCloseSubpath){if(!c.points.empty())c.points.back().type|=1;}
        else throw std::domain_error("CoreGraphics stroke 回傳未支援的曲線");
    }catch(...){c.error=std::current_exception();}});
    if(collector.error)std::rethrow_exception(collector.error);return gui_flatten_path(collector.points);
#else
    (void)input;(void)transform;(void)width;(void)style;(void)miter;throw std::domain_error("本平台無原生 stroke 後端");
#endif
}
}
