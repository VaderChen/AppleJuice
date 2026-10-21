#include "x64arm/windows_services.hpp"
#include "x64arm/windows_gdi_path.hpp"
#include "x64arm/windows_transform.hpp"
#include <bit>
#include <cmath>
namespace x64arm {
std::span<const WindowsServiceDescriptor> gdi_path_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","ExtCreatePen",5},{"gdi32.dll","CreatePenIndirect",1},
        {"gdi32.dll","BeginPath",1},{"gdi32.dll","EndPath",1},{"gdi32.dll","AbortPath",1},{"gdi32.dll","CloseFigure",1},
        {"gdi32.dll","FillPath",1},{"gdi32.dll","StrokePath",1},{"gdi32.dll","StrokeAndFillPath",1},{"gdi32.dll","FlattenPath",1},{"gdi32.dll","WidenPath",1},
        {"gdi32.dll","GetPath",4},{"gdi32.dll","PathToRegion",1},{"gdi32.dll","SelectClipPath",2},
        {"gdi32.dll","SetPolyFillMode",2},{"gdi32.dll","GetPolyFillMode",1},{"gdi32.dll","SetArcDirection",2},{"gdi32.dll","GetArcDirection",1},
        {"gdi32.dll","SetMiterLimit",3},{"gdi32.dll","GetMiterLimit",2},
        {"gdi32.dll","PolyBezier",3},{"gdi32.dll","PolyBezierTo",3},{"gdi32.dll","PolylineTo",3},{"gdi32.dll","Polygon",3},{"gdi32.dll","PolyPolygon",4},{"gdi32.dll","PolyPolyline",4},
        {"gdi32.dll","RoundRect",7},{"gdi32.dll","CreatePolygonRgn",3},{"gdi32.dll","CreatePolyPolygonRgn",4},{"gdi32.dll","ExtCreateRegion",3}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_gdi_path(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](unsigned error){last_error_=error;return done(name=="GetPath"?UINT32_MAX:0);};
    const auto integer=[&](unsigned i){return static_cast<std::int32_t>(arg(i));};
    const auto point=[&](std::uint64_t address){return std::array{static_cast<std::int32_t>(memory.load(GuestAddress{address},32)),static_cast<std::int32_t>(memory.load(GuestAddress{address+4},32))};};
    const auto add=[](GuiPath& p,const GuiTransform& transform,double x,double y,unsigned type){
        const auto v=gui_point(transform,x,y);if(!std::isfinite(v[0]) || !std::isfinite(v[1]) || std::abs(v[0])>=(1<<26) || std::abs(v[1])>=(1<<26))throw std::invalid_argument("GDI path 座標不合法");
        if(p.points.size()>=65536)throw std::length_error("GDI path 超過上限");p.points.push_back({v[0],v[1],static_cast<std::uint8_t>(type)});p.new_figure=false;
    };
    const auto polygons=[&](GuiPath& p,const GuiTransform& transform,std::uint64_t points,const std::vector<unsigned>& counts,bool close){
        std::size_t total=0;for(auto count:counts){if(count<2 || count>65536 || total+count>65536)throw std::invalid_argument("多邊形頂點數不合法");total+=count;}
        if(total && !points)throw std::invalid_argument("多邊形頂點為空");memory.check(GuestAddress{points},total*8,Access::Read);std::size_t offset=0;
        for(auto count:counts){for(unsigned i=0;i<count;++i){const auto v=point(points+(offset+i)*8);add(p,transform,v[0],v[1],i?2:6);}if(close)p.points.back().type|=1;p.new_figure=true;offset+=count;}
    };
    const auto counts=[&](std::uint64_t address,int count){if(count<1 || count>4096 || !address)throw std::invalid_argument("多邊形群組數不合法");std::vector<unsigned> result;for(int i=0;i<count;++i)result.push_back(static_cast<unsigned>(memory.load(GuestAddress{address+static_cast<unsigned>(i)*4},32)));return result;};
    try {
        if(name=="ExtCreateRegion"){
            const auto bytes=static_cast<std::uint32_t>(arg(1));const auto address=arg(2);
            if(!address || bytes<32)return fail(87);
            memory.check(GuestAddress{address},32,Access::Read);
            const auto field=[&](unsigned offset){return static_cast<std::uint32_t>(memory.load(GuestAddress{address+offset},32));};
            const auto count=field(8),payload=field(12);
            if(field(0)!=32 || field(4)!=1 || count>(bytes-32)/16 || (payload && (payload<count*16ULL || payload>bytes-32)))return fail(87);
            if(count>65536 || gui_.regions.size()>=65536)return fail(8);
            memory.check(GuestAddress{address+32},static_cast<std::size_t>(count)*16,Access::Read);
            GuiTransform transform=gui_identity;
            if(arg(0)){memory.check(GuestAddress{arg(0)},24,Access::Read);for(unsigned i=0;i<6;++i)
                transform[i]=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+i*4},32)));}
            if(!gui_valid_transform(transform))return fail(87);
            GuiRegion rectangles;
            for(unsigned i=0;i<count;++i){GuiRect rect{};for(unsigned c=0;c<4;++c)rect[c]=static_cast<std::int32_t>(field(32+i*16+c*4));
                if(!gui_region_coordinates({rect}) || rect[0]>rect[2] || rect[1]>rect[3])return fail(87);
                if(gui_rect_empty(rect))continue;
                const auto bounds=gui_transform_bounds(transform,rect);if(!gui_region_coordinates({bounds}))return fail(87);
                auto transformed=gui_transform_region_rect(transform,rect);
                if(rectangles.size()+transformed.size()>65536)return fail(8);
                rectangles.insert(rectangles.end(),transformed.begin(),transformed.end());
            }
            // 與 CombineRgn 共用水平帶正規化，支援重疊、洞與旋轉後的矩形群組。
            auto region=gui_region_combine(rectangles,{},2);const auto handle=gui_.next_object;
            gui_.regions.emplace(handle,std::move(region));++gui_.next_object;return done(handle);
        }
        if(name=="ExtCreatePen" || name=="CreatePenIndirect"){
            const bool extended=name=="ExtCreatePen";if(!(extended?arg(2):arg(0)))return fail(87);
            if(extended)memory.check(GuestAddress{arg(2)},x86_?12:16,Access::Read);else memory.check(GuestAddress{arg(0)},16,Access::Read);
            const auto style=extended?static_cast<unsigned>(arg(0)):static_cast<unsigned>(memory.load(GuestAddress{arg(0)},32));
            const auto width=extended?arg(1):static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{arg(0)+4},32)))));
            const auto brush=extended?static_cast<unsigned>(memory.load(GuestAddress{arg(2)},32)):0U;
            const auto color=static_cast<unsigned>(memory.load(GuestAddress{extended?arg(2)+4:arg(0)+12},32));
            if(width>32768 || (style&~0x133ffU) || (style&0xf00)>0x200 || (style&0xf000)>0x2000 || (!extended && (style&~15U)))return fail(87);
            if((style&15)!=0 && (style&15)!=5)return fail(50);if(extended && (arg(3) || arg(4)))return fail(87);
            if(extended && !(style&0x10000) && (width!=1 || brush!=0 || (style&0xff00)))return fail(87);
            if(brush>1 || color&0xff000000U)return fail(50);
            const auto handle=gui_.next_object++;gui_.pens[handle]=color;gui_.pen_widths[handle]=(style&15)==5 || brush==1?0:static_cast<int>(std::max<std::uint64_t>(1,width));
            if(extended)gui_.extended_pens[handle]={style,static_cast<unsigned>(width),brush};return done(handle);
        }
        if(name=="CreatePolygonRgn" || name=="CreatePolyPolygonRgn"){
            const bool multi=name=="CreatePolyPolygonRgn";const auto mode=arg(multi?3:2);if(mode<1 || mode>2)return fail(87);GuiPath p;
            polygons(p,gui_identity,arg(0),multi?counts(arg(1),integer(2)):std::vector<unsigned>{static_cast<unsigned>(arg(1))},true);
            auto region=native_path_region(p.points,static_cast<unsigned>(mode));const auto handle=gui_.next_object++;gui_.regions.emplace(handle,std::move(region));return done(handle);
        }
        const auto window=arg(0)>=0x10000000?gui_.windows.find(arg(0)-0x10000000):gui_.windows.end();
        if(!gui_.contexts.contains(arg(0)) && !gui_.dc_objects.contains(arg(0)) && window==gui_.windows.end())return fail(6);
        auto& dc=gui_.contexts[arg(0)];const auto transform=gui_device_transform(dc);
        if(name=="GetPolyFillMode")return done(dc.poly_fill_mode);if(name=="GetArcDirection")return done(dc.arc_direction);
        if(name=="SetPolyFillMode" || name=="SetArcDirection"){if(arg(1)<1 || arg(1)>2)return fail(87);auto& field=name=="SetPolyFillMode"?dc.poly_fill_mode:dc.arc_direction;const auto old=field;field=static_cast<unsigned>(arg(1));return done(old);}
        if(name=="SetMiterLimit" || name=="GetMiterLimit"){
            const bool set=name=="SetMiterLimit";float value=dc.miter_limit;
            if(set){std::uint32_t raw=static_cast<unsigned>(arg(1));if(!x86_){raw=0;for(unsigned i=0;i<4;++i)raw|=static_cast<unsigned>(state.xmm[1][i])<<(i*8);}value=std::bit_cast<float>(raw);if(!std::isfinite(value) || value<1)return fail(87);}
            const auto output=arg(set?2:1);if(!set && !output)return fail(87);if(output)memory.store(GuestAddress{output},32,std::bit_cast<unsigned>(dc.miter_limit));if(set)dc.miter_limit=value;return done(1);
        }
        if(name=="BeginPath"){dc.path={1,true,{}};return done(1);}if(name=="AbortPath"){dc.path={};return done(1);}
        if(name=="EndPath"){if(dc.path.state!=1)return fail(1003);dc.path.state=2;return done(1);}
        if(name=="CloseFigure"){if(dc.path.state!=1)return fail(1003);if(!dc.path.points.empty() && !dc.path.new_figure)dc.path.points.back().type|=1;dc.path.new_figure=true;return done(1);}
        const auto closed=name=="GetPath" || name=="FlattenPath" || name=="WidenPath" || name=="FillPath" || name=="StrokePath" || name=="StrokeAndFillPath" || name=="PathToRegion" || name=="SelectClipPath";
        if(closed && dc.path.state!=2)return fail(1003);
        if(name=="GetPath"){
            const auto size=dc.path.points.size();if(integer(3)<0 || (arg(3) && (arg(3)<size || !arg(1) || !arg(2))))return fail(87);if(!arg(3))return done(size);
            const auto inverse=gui_inverse(transform);if(!inverse)return fail(87);std::vector<std::array<int,2>> result;result.reserve(size);
            for(const auto& p:dc.path.points){const auto v=gui_integer_point(*inverse,p.x,p.y);if(!v)return fail(87);result.push_back(*v);}
            memory.check(GuestAddress{arg(1)},size*8,Access::Write);memory.check(GuestAddress{arg(2)},size,Access::Write);
            for(std::size_t i=0;i<size;++i){for(unsigned j=0;j<2;++j)memory.store(GuestAddress{arg(1)+i*8+j*4},32,static_cast<unsigned>(result[i][j]));memory.store(GuestAddress{arg(2)+i},8,dc.path.points[i].type);}return done(size);
        }
        if(name=="FlattenPath"){dc.path.points=gui_flatten_path(dc.path.points);return done(1);}
        const auto pen_width=dc.pen==0xb008?0:gui_.pen_widths.contains(dc.pen)?gui_.pen_widths.at(dc.pen):1;
        const auto extended=gui_.extended_pens.find(dc.pen);const bool geometric=extended!=gui_.extended_pens.end() && (extended->second.style&0x10000);
        const auto stroke=[&](const std::vector<GuiPathPoint>& p){return native_stroke_path(p,geometric || (extended==gui_.extended_pens.end() && pen_width>1)?transform:gui_identity,pen_width,extended==gui_.extended_pens.end()?0:extended->second.style,dc.miter_limit);};
        if(name=="WidenPath"){if(!geometric && pen_width<=1)return fail(1003);dc.path.points=stroke(dc.path.points);return done(1);}
        auto working=closed?dc.path:dc.path.state==1?dc.path:GuiPath{1,true,{}};bool fill=name=="FillPath" || name=="StrokeAndFillPath",outline=name=="StrokePath" || name=="StrokeAndFillPath";
        bool update_position=false;int final_x=dc.x,final_y=dc.y;
        if(!closed){
            if(name=="MoveToEx"){
                if(arg(3))memory.check(GuestAddress{arg(3)},8,Access::Write);add(working,transform,integer(1),integer(2),6);
                if(arg(3)){memory.store(GuestAddress{arg(3)},32,static_cast<unsigned>(dc.x));memory.store(GuestAddress{arg(3)+4},32,static_cast<unsigned>(dc.y));}dc.x=integer(1);dc.y=integer(2);dc.path=std::move(working);return done(1);
            }
            if(name=="LineTo"){if(working.new_figure)add(working,transform,dc.x,dc.y,6);add(working,transform,integer(1),integer(2),2);final_x=integer(1);final_y=integer(2);update_position=true;outline=true;}
            else if(name=="Polyline" || name=="Polygon" || name=="PolyPolygon" || name=="PolyPolyline"){
                const bool multi=name=="PolyPolygon" || name=="PolyPolyline";fill=name=="Polygon" || name=="PolyPolygon";outline=true;
                polygons(working,transform,arg(1),multi?counts(arg(2),integer(3)):std::vector<unsigned>{static_cast<unsigned>(arg(2))},fill);
            }else if(name=="PolyBezier" || name=="PolyBezierTo" || name=="PolylineTo"){
                const bool to=name!="PolyBezier",curve=name!="PolylineTo";const auto count=integer(2);
                if(count<(to?1:4) || count>65536 || (curve && (count-(to?0:1))%3) || !arg(1))return fail(87);memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(count)*8,Access::Read);
                if(to && working.new_figure)add(working,transform,dc.x,dc.y,6);
                for(int i=0;i<count;++i){const auto p=point(arg(1)+static_cast<unsigned>(i)*8);add(working,transform,p[0],p[1],!to && i==0?6:curve?4:2);final_x=p[0];final_y=p[1];}update_position=to;if(!to)working.new_figure=true;outline=true;
            }else if(name=="RoundRect"){
                // 圓角以四段三次 Bezier 近似四分之一橢圓；邊角寬高與 Windows 一樣限制在矩形內。
                const double l=std::min(integer(1),integer(3)),r=std::max(integer(1),integer(3)),t=std::min(integer(2),integer(4)),b=std::max(integer(2),integer(4));
                const double rx=std::min(std::abs(static_cast<double>(integer(5))),r-l)/2,ry=std::min(std::abs(static_cast<double>(integer(6))),b-t)/2,k=0.5522847498307936;
                if(l!=r && t!=b){
                    struct Node {double x,y;unsigned type;};
                    const std::array<Node,16> nodes{{{r,t+ry,6},{r,t+ry-k*ry,4},{r-rx+k*rx,t,4},{r-rx,t,4},{l+rx,t,2},{l+rx-k*rx,t,4},{l,t+ry-k*ry,4},{l,t+ry,4},
                        {l,b-ry,2},{l,b-ry+k*ry,4},{l+rx-k*rx,b,4},{l+rx,b,4},{r-rx,b,2},{r-rx+k*rx,b,4},{r,b-ry+k*ry,4},{r,b-ry,4}}};
                    // 預設 AD_COUNTERCLOCKWISE；順時針時沿同一輪廓反向，每段沿用原段型與反序控制點。
                    add(working,transform,nodes[0].x,nodes[0].y,6);
                    if(dc.arc_direction==1)for(std::size_t i=1;i<nodes.size();++i)add(working,transform,nodes[i].x,nodes[i].y,nodes[i].type);
                    else {add(working,transform,nodes.back().x,nodes.back().y,2);for(std::size_t i=nodes.size()-1;i>0;--i)add(working,transform,nodes[i-1].x,nodes[i-1].y,nodes[i].type);}
                    working.points.back().type|=1;working.new_figure=true;}fill=true;outline=true;
            }else if(name=="Rectangle" || name=="Ellipse"){
                const double l=std::min(integer(1),integer(3)),r=std::max(integer(1),integer(3)),t=std::min(integer(2),integer(4)),b=std::max(integer(2),integer(4));
                if(l!=r && t!=b){if(name=="Rectangle"){add(working,transform,l,t,6);add(working,transform,r,t,2);add(working,transform,r,b,2);add(working,transform,l,b,2);}
                    else {const double x=(l+r)/2,y=(t+b)/2,rx=(r-l)/2,ry=(b-t)/2,k=0.5522847498307936,sign=dc.arc_direction==1?-1:1;
                        add(working,transform,x+rx,y,6);for(unsigned i=0;i<4;++i){const double a=sign*static_cast<double>(i)*1.5707963267948966,z=a+sign*1.5707963267948966;
                            add(working,transform,x+rx*(std::cos(a)-sign*k*std::sin(a)),y+ry*(std::sin(a)+sign*k*std::cos(a)),4);
                            add(working,transform,x+rx*(std::cos(z)+sign*k*std::sin(z)),y+ry*(std::sin(z)-sign*k*std::cos(z)),4);add(working,transform,x+rx*std::cos(z),y+ry*std::sin(z),4);}}
                    working.points.back().type|=1;working.new_figure=true;}fill=true;outline=true;
            }else return fail(50);
            if(dc.path.state==1){dc.path=std::move(working);if(update_position){dc.x=final_x;dc.y=final_y;}return done(1);}
        }
        if(name=="StrokeAndFillPath"){for(std::size_t i=0;i<working.points.size();++i)if(i+1==working.points.size() || (working.points[i+1].type&6)==6)working.points[i].type|=1;}
        if(name=="PathToRegion" || name=="SelectClipPath"){
            auto region=native_path_region(working.points,dc.poly_fill_mode);
            if(name=="PathToRegion"){const auto handle=gui_.next_object++;gui_.regions.emplace(handle,std::move(region));dc.path={};return done(handle);}
            if(arg(1)<1 || arg(1)>5)return fail(87);
            GuiRegion old;if(dc.application_clip)old=*dc.application_clip;else {int width=1440,height=900;if(const auto bitmap=gui_.bitmaps.find(dc.bitmap);bitmap!=gui_.bitmaps.end()){width=static_cast<int>(bitmap->second.width);height=static_cast<int>(bitmap->second.height);}else if(window!=gui_.windows.end()){width=static_cast<int>(window->second.width);height=static_cast<int>(window->second.height);}old={{0,0,width,height}};}
            auto applied=arg(1)==5?region:gui_region_combine(old,region,static_cast<unsigned>(arg(1)));auto effective=dc.meta_clip?gui_region_combine(applied,*dc.meta_clip,1):applied;
            const auto bounds=gui_region_bounds(effective);auto excluded=gui_region_combine(GuiRegion{bounds},effective,4);dc.application_clip=std::move(applied);dc.clipped=true;dc.clip=bounds;dc.excluded=std::move(excluded);dc.path={};return done(1);
        }
        GuiRegion interior,edge;if(fill && dc.brush!=0xb005)interior=native_path_region(working.points,dc.poly_fill_mode);if(outline && pen_width)edge=native_path_region(stroke(working.points),2);
        if(!interior.empty() && !edge.empty())interior=gui_region_combine(interior,edge,4);
        const auto clip=[&](GuiRegion region){if(dc.clipped)region=gui_region_combine(region,GuiRegion{dc.clip},1);if(!dc.excluded.empty())region=gui_region_combine(region,dc.excluded,4);return region;};interior=clip(std::move(interior));edge=clip(std::move(edge));
        const auto paint=[&](const GuiRegion& region,bool pen){
            GuiDraw draw;draw.kind=GuiDraw::Kind::Fill;draw.device_coordinates=true;draw.rop2=dc.rop2;
            if(pen)draw.color=dc.pen==0xb013?dc.dc_pen_color:gui_.pens.contains(dc.pen)?gui_.pens.at(dc.pen):dc.pen==0xb006?0xffffff:0;
            else {draw.color=dc.brush==0xb012?dc.dc_brush_color:gui_.brushes.contains(dc.brush)?gui_.brushes.at(dc.brush):dc.brush==0xb000?0xffffff:dc.brush==0xb001?0xc0c0c0:dc.brush==0xb002?0x808080:dc.brush==0xb003?0x404040:0;draw.hatch=gui_.brush_hatches.contains(dc.brush)?gui_.brush_hatches.at(dc.brush):-1;draw.pattern_opaque=dc.mode==2;draw.pattern_background=dc.background;draw.pattern_color=dc.color;draw.brush_origin=dc.brush_origin;if(gui_.brush_patterns.contains(dc.brush)){draw.pattern_brush=dc.brush;draw.pattern_monochrome=gui_.brush_patterns.at(dc.brush).bits_per_pixel==1;}}
            for(const auto& rect:region){draw.x=rect[0];draw.y=rect[1];draw.right=rect[2];draw.bottom=rect[3];if(!raster_draw(memory,arg(0),draw))throw std::domain_error("此 DC 尚無 path 繪圖後端");}
        };
        paint(interior,false);paint(edge,true);if(closed)dc.path={};if(update_position){dc.x=final_x;dc.y=final_y;}return done(1);
    }catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::domain_error&){return fail(50);}catch(const std::invalid_argument&){return fail(87);}
}
}
