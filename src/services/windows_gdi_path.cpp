#include "gui_paint_region.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_gdi_path.hpp"
#include "x64arm/windows_transform.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
namespace x64arm {
std::span<const WindowsServiceDescriptor> gdi_path_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","CreatePen",3,0,0,7},{"win32u.dll","NtGdiCreatePen",4,0,0,7},
        {"win32u.dll","NtGdiExtCreatePen",11,0,0,0x34f},{"gdi32.dll","ExtCreatePen",5},{"gdi32.dll","CreatePenIndirect",1},
        {"gdi32.dll","BeginPath",1},{"gdi32.dll","EndPath",1},{"gdi32.dll","AbortPath",1},{"gdi32.dll","CloseFigure",1},
        {"gdi32.dll","FillPath",1},{"gdi32.dll","StrokePath",1},{"gdi32.dll","StrokeAndFillPath",1},{"gdi32.dll","FlattenPath",1},{"gdi32.dll","WidenPath",1},
        {"gdi32.dll","GetPath",4},{"gdi32.dll","PathToRegion",1},{"gdi32.dll","SelectClipPath",2},
        {"gdi32.dll","SetPolyFillMode",2},{"gdi32.dll","GetPolyFillMode",1},{"gdi32.dll","SetArcDirection",2},{"gdi32.dll","GetArcDirection",1},
        {"gdi32.dll","SetMiterLimit",3},{"gdi32.dll","GetMiterLimit",2},
        {"win32u.dll","NtGdiArcInternal",10,0,0,0x3fd},{"win32u.dll","NtGdiAngleArc",6,0,0,0x3e},
        {"win32u.dll","NtGdiSetMiterLimit",3,0,0,0x2},{"win32u.dll","NtGdiGetMiterLimit",2},
        {"win32u.dll","NtGdiPolyDraw",4,0,0,0x8},
        {"win32u.dll","NtGdiPolyPolyDraw",5,0,0,0x18},
        {"gdi32.dll","PolyBezier",3},{"gdi32.dll","PolyBezierTo",3},{"gdi32.dll","PolylineTo",3},{"gdi32.dll","Polygon",3},{"gdi32.dll","PolyPolygon",4},{"gdi32.dll","PolyPolyline",4},
        {"gdi32.dll","Arc",9},{"gdi32.dll","ArcTo",9},{"gdi32.dll","Chord",9},{"gdi32.dll","Pie",9},{"gdi32.dll","AngleArc",6},{"gdi32.dll","GetCurrentPositionEx",2},
        {"gdi32.dll","RoundRect",7},{"gdi32.dll","CreatePolygonRgn",3},{"gdi32.dll","CreatePolyPolygonRgn",4},{"gdi32.dll","ExtCreateRegion",3}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_gdi_path(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    auto name=service.symbol;
    const bool native_poly=name=="NtGdiPolyPolyDraw";
    const bool native_arc=name=="NtGdiArcInternal";
    const bool native_miter=name=="NtGdiSetMiterLimit";
    std::array<std::uint64_t,4> poly_args{};
    const auto arg=[&](unsigned index){return native_poly?poly_args.at(index):call(index+static_cast<unsigned>(native_arc));};
    const auto done=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
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
        if(native_arc){
            constexpr std::array<std::string_view,4> arcs{"Arc","ArcTo","Chord","Pie"};
            const auto kind=call(0);
            if(kind>=arcs.size())return fail(87);
            name=arcs[kind];
        }else if(name=="NtGdiAngleArc")name="AngleArc";
        else if(native_miter)name="SetMiterLimit";
        else if(name=="NtGdiGetMiterLimit")name="GetMiterLimit";
        else if(name=="NtGdiPolyDraw")name="PolyDraw";
        if(native_poly){
            constexpr std::array<std::string_view,6> operations{
                "PolyPolygon","PolyPolyline","PolyBezier","PolyBezierTo","PolylineTo","CreatePolyPolygonRgn"};
            const auto operation=call(4);
            if(operation==0 || operation>operations.size() || !call(2))return fail(87);
            name=operations[operation-1];
            if(operation==6){
                // 此模式第一個位置傳入填滿規則，沒有 DC。
                poly_args={call(1),call(2),call(3),static_cast<std::uint32_t>(call(0))};
            }else{
                poly_args={call(0),call(1),call(2),call(3)};
                if(operation>=3){
                    if(call(3)!=1)return fail(87);
                    poly_args[2]=memory.load(GuestAddress{call(2)},32);
                }
            }
        }
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
        if(name=="CreatePen" || name=="NtGdiCreatePen" || name=="CreatePenIndirect" ||
           name=="ExtCreatePen" || name=="NtGdiExtCreatePen"){
            const bool indirect=name=="CreatePenIndirect";
            const bool native=name=="NtGdiExtCreatePen";
            const bool extended=native || name=="ExtCreatePen";
            GuiState::PenDefinition pen;
            pen.extended=extended && !(native && arg(9));
            std::uint32_t color;
            std::uint64_t hatch=0,styles=0;
            if(indirect){
                if(!arg(0))return fail(87);
                memory.check(GuestAddress{arg(0)},16,Access::Read);
                pen.style=static_cast<unsigned>(memory.load(GuestAddress{arg(0)},32));
                pen.width=static_cast<unsigned>(memory.load(GuestAddress{arg(0)+4},32));
                color=static_cast<unsigned>(memory.load(GuestAddress{arg(0)+12},32));
            }else{
                pen.style=static_cast<unsigned>(arg(0));pen.width=static_cast<unsigned>(arg(1));
                if(native){
                    pen.brush_style=static_cast<unsigned>(arg(2));color=static_cast<unsigned>(arg(3));
                    pen.client_hatch=arg(4);hatch=arg(5);pen.count=static_cast<unsigned>(arg(6));styles=arg(7);
                }else if(extended){
                    if(!arg(2))return fail(87);
                    memory.check(GuestAddress{arg(2)},x86_?12:16,Access::Read);
                    pen.brush_style=static_cast<unsigned>(memory.load(GuestAddress{arg(2)},32));
                    color=static_cast<unsigned>(memory.load(GuestAddress{arg(2)+4},32));
                    pen.client_hatch=hatch=memory.load(GuestAddress{arg(2)+8},x86_?32:64);
                    pen.count=static_cast<unsigned>(arg(3));styles=arg(4);
                }else color=static_cast<unsigned>(arg(2));
            }
            if(!extended && pen.style>6){
                if(name=="NtGdiCreatePen")return fail(87);
                pen.style=0;
            }
            const auto line=pen.style&15;
            const bool geometric=(pen.style&0x10000)!=0;
            if(pen.count>pen.entries.size() || ((pen.count || styles) && line!=7))return fail(87);
            if(line==5 || (geometric && pen.brush_style==1))return done(0xb008);
            pen.width=static_cast<unsigned>(std::abs(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(pen.width))));
            if(pen.width>32768 || (pen.style&~0x1330fU) || (pen.style&0xf00)>0x200 ||
               (pen.style&0xf000)>0x2000 || line>8 || (line==8 && geometric))return fail(87);
            if(pen.extended && !geometric && (pen.width!=1 || pen.brush_style!=0 || line==6))return fail(87);
            if(line==7){
                if(!styles || !pen.count)return fail(87);
                memory.read(GuestAddress{styles},std::span(reinterpret_cast<std::uint8_t*>(pen.entries.data()),pen.count*4U));
                if(geometric){
                    const auto entries=std::span(pen.entries).first(pen.count);
                    if(std::ranges::all_of(entries,[](auto v){return v==0;}) ||
                       std::ranges::any_of(entries,[](auto v){return v>INT32_MAX;}))return fail(87);
                }
            }
            switch(pen.brush_style){
            case 0:break;
            case 2:
                if(hatch>=12)return fail(87);
                if(hatch>=6)pen.brush_style=0;
                else pen.pattern_brush=nested_service(state,memory,"gdi32.dll","CreateHatchBrush",{hatch,color});
                break;
            case 3:
                pen.pattern_brush=nested_service(state,memory,"gdi32.dll","CreatePatternBrush",{hatch});color=0;break;
            case 5:case 6:
                if(native)pen.pattern_brush=nested_service(state,memory,"win32u.dll","NtGdiCreateDIBBrush",{hatch,color,arg(8),0,1,pen.client_hatch});
                else pen.pattern_brush=nested_service(state,memory,"gdi32.dll",pen.brush_style==5?"CreateDIBPatternBrush":"CreateDIBPatternBrushPt",{hatch,color});
                pen.brush_style=6;color=0;break;
            default:return fail(87);
            }
            if(pen.brush_style && !pen.pattern_brush)return done(0);
            const auto handle=gui_.next_object;
            try{
                gui_.pen_definitions.emplace(handle,pen);gui_.pens.emplace(handle,color);
                gui_.pen_widths.emplace(handle,static_cast<int>(std::max(1U,pen.width)));
            }catch(...){
                gui_.pen_definitions.erase(handle);gui_.pens.erase(handle);gui_.pen_widths.erase(handle);
                if(pen.pattern_brush)(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{pen.pattern_brush});
                throw;
            }
            ++gui_.next_object;return done(handle);
        }
        if(name=="CreatePolygonRgn" || name=="CreatePolyPolygonRgn"){
            const bool multi=name=="CreatePolyPolygonRgn";const auto mode=arg(multi?3:2);if(mode<1 || mode>2)return fail(87);GuiPath p;
            polygons(p,gui_identity,arg(0),multi?counts(arg(1),integer(2)):std::vector<unsigned>{static_cast<unsigned>(arg(1))},true);
            if(gui_.regions.size()>=65536)return fail(8);
            auto region=native_path_region(p.points,static_cast<unsigned>(mode));
            const auto handle=gui_.next_object;
            gui_.regions.emplace(handle,std::move(region));++gui_.next_object;return done(handle);
        }
        const auto window=arg(0)>=0x10000000?gui_.windows.find(arg(0)-0x10000000):gui_.windows.end();
        if(!gui_.contexts.contains(arg(0)) && !gui_.dc_objects.contains(arg(0)) && window==gui_.windows.end())return fail(6);
        auto& dc=gui_.contexts[arg(0)];const auto transform=gui_device_transform(dc);
        if(name=="GetCurrentPositionEx"){if(!arg(1))return fail(87);memory.check(GuestAddress{arg(1)},8,Access::Write);memory.store(GuestAddress{arg(1)},32,static_cast<unsigned>(dc.x));memory.store(GuestAddress{arg(1)+4},32,static_cast<unsigned>(dc.y));return done(1);}
        if(name=="GetPolyFillMode")return done(dc.poly_fill_mode);if(name=="GetArcDirection")return done(dc.arc_direction);
        if(name=="SetPolyFillMode" || name=="SetArcDirection"){if(arg(1)<1 || arg(1)>2)return fail(87);auto& field=name=="SetPolyFillMode"?dc.poly_fill_mode:dc.arc_direction;const auto old=field;field=static_cast<unsigned>(arg(1));return done(old);}
        if(name=="SetMiterLimit" || name=="GetMiterLimit"){
            const bool set=name=="SetMiterLimit";float value=dc.miter_limit;
            if(set){std::uint32_t raw=static_cast<unsigned>(arg(1));if(!x86_ && !native_miter){raw=0;for(unsigned i=0;i<4;++i)raw|=static_cast<unsigned>(state.xmm[1][i])<<(i*8);}value=std::bit_cast<float>(raw);if(!std::isfinite(value) || value<1)return fail(87);}
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
        const auto definition=gui_.pen_definitions.find(dc.pen);
        const GuiState::PenDefinition stock{};
        const auto& pen=definition==gui_.pen_definitions.end()?stock:definition->second;
        const bool geometric=(pen.style&0x10000)!=0;
        const auto line=pen.style&15U;
        const auto stroke_transform=geometric || (!pen.extended && pen_width>1)?transform:gui_identity;
        std::vector<double> dash;
        if(pen.extended || pen_width<=1){
            switch(line){
            case 1:dash={6,2};break;case 2:case 8:dash={1,1};break;
            case 3:dash={3,2,1,2};break;case 4:dash={3,1,1,1,1,1};break;
            case 7:dash.assign(pen.entries.begin(),pen.entries.begin()+pen.count);break;
            }
            if(geometric && line!=7)for(auto& length:dash)length*=pen_width;
        }
        // Cosmetic 筆的虛線單位為裝置像素，端點不應蓋住隔壁的空白像素。
        const auto stroke_style=(!geometric && !dash.empty())?(pen.style&~0xf00U)|0x200U:pen.style;
        const auto stroke=[&](const std::vector<GuiPathPoint>& p){
            return native_stroke_path(p,stroke_transform,pen_width,stroke_style,dc.miter_limit,dash);
        };
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
            }else if(name=="PolyDraw"){
                const auto count=static_cast<unsigned>(arg(3));
                if(count>65536)return fail(87);
                if(!count)return done(1);
                memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(count)*8,Access::Read);
                std::vector<std::uint8_t> types(count);
                memory.read(GuestAddress{arg(2)},types);
                std::optional<std::array<int,2>> figure_start;
                if(working.new_figure)figure_start=std::array{dc.x,dc.y};
                // 曲線控制點、閉合旗標與目前位置在整筆成功後一併提交。
                for(unsigned i=0;i<count;){
                    const unsigned type=types[i]&~1U;
                    if(types[i]==6){
                        const auto move=point(arg(1)+i*8);
                        figure_start=move;
                        final_x=move[0];final_y=move[1];
                        // 只有後續線段開始時才把移筆位置寫進路徑。
                        working.new_figure=true;
                        ++i;
                        continue;
                    }
                    if(type!=2 && type!=4)return fail(87);
                    const unsigned length=type==4?3U:1U;
                    if(length==3 && (count-i<3 || types[i]!=4 || types[i+1]!=4 || (types[i+2]&~1U)!=4))return fail(87);
                    if(working.new_figure)add(working,transform,final_x,final_y,6);
                    for(unsigned k=0;k<length;++k,++i){
                        const auto p=point(arg(1)+i*8);
                        add(working,transform,p[0],p[1],types[i]);
                        final_x=p[0];final_y=p[1];
                    }
                    if(types[i-1]&1){
                        if(!figure_start){
                            // 此段可能延續先前的 MoveToEx。路徑保存裝置座標，DC 目前位置使用邏輯座標。
                            const auto move=std::find_if(working.points.rbegin(),working.points.rend(),
                                [](const GuiPathPoint& p){return p.type==6;});
                            const auto inverse=gui_inverse(transform);
                            if(move==working.points.rend() || !inverse)return fail(87);
                            figure_start=gui_integer_point(*inverse,move->x,move->y);
                            if(!figure_start)return fail(87);
                        }
                        working.new_figure=true;
                        final_x=(*figure_start)[0];final_y=(*figure_start)[1];
                    }
                }
                update_position=true;outline=true;
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
            }else if(name=="Arc" || name=="ArcTo" || name=="Chord" || name=="Pie" || name=="AngleArc"){
                // 橢圓弧以每段不超過 90 度的三次 Bezier 近似。Arc／ArcTo／Chord／Pie 依 SetArcDirection（預設逆時針）從起點射線走到終點射線，
                // 兩條射線重合時畫完整橢圓；AngleArc 依掃角正負決定方向、超過 360 度時保留額外圈數。ArcTo／AngleArc 先從目前位置連線到弧起點並把目前位置移到弧終點。
                constexpr double tau=6.283185307179586;const bool angle=name=="AngleArc";double cx=0,cy=0,rx=0,ry=0,start=0,sweep=0;
                if(angle){
                    const auto radius=static_cast<std::int32_t>(arg(3));const auto a=std::bit_cast<float>(static_cast<std::uint32_t>(arg(4))),s=std::bit_cast<float>(static_cast<std::uint32_t>(arg(5)));
                    if(radius<0 || !std::isfinite(a) || !std::isfinite(s))return fail(87);
                    cx=integer(1);cy=integer(2);rx=ry=radius;start=std::remainder(static_cast<double>(a),360.0)*tau/360;sweep=static_cast<double>(s)*tau/360;
                }else{
                    const double l=std::min(integer(1),integer(3)),r=std::max(integer(1),integer(3)),t=std::min(integer(2),integer(4)),b=std::max(integer(2),integer(4));
                    if(l==r || t==b)return done(1);
                    cx=(l+r)/2;cy=(t+b)/2;rx=(r-l)/2;ry=(b-t)/2;
                    // 參數角：橢圓上的點 (cx+rx·cosθ, cy−ry·sinθ) 與中心到指定點的射線同向。
                    const auto parametric=[&](double x,double y){return std::atan2(-(y-cy)*rx,(x-cx)*ry);};
                    start=parametric(integer(5),integer(6));const auto end=parametric(integer(7),integer(8));const bool counter=dc.arc_direction==1;
                    sweep=counter?end-start:start-end;while(sweep<=0)sweep+=tau;while(sweep>tau)sweep-=tau;if(!counter)sweep=-sweep;
                }
                const auto at=[&](double theta){return std::array<double,2>{cx+rx*std::cos(theta),cy-ry*std::sin(theta)};};
                const auto begin=at(start);const bool connected=name=="ArcTo" || angle;
                if(connected){if(working.new_figure)add(working,transform,dc.x,dc.y,6);add(working,transform,begin[0],begin[1],2);}
                else add(working,transform,begin[0],begin[1],6);
                const auto magnitude=std::abs(sweep);
                if(magnitude>0){
                    const auto required=std::max(1.0,std::ceil(magnitude/(tau/4)-1e-9));
                    if(required>static_cast<double>((65536-working.points.size())/3))return fail(8);
                    const auto segments=static_cast<unsigned>(required);
                    double theta=start;
                    for(unsigned i=0;i<segments;++i){
                        // 單位圓上 P(θ)=(cosθ,sinθ)、切線 (−sinθ,cosθ)；映射到裝置座標時 y 取負號。
                        const double step=std::copysign(std::min(tau/4,magnitude-i*(tau/4)),sweep);
                        const double h=4.0/3*std::tan(step/4);
                        const double next=theta+step;
                        add(working,transform,cx+rx*(std::cos(theta)-h*std::sin(theta)),cy-ry*(std::sin(theta)+h*std::cos(theta)),4);
                        add(working,transform,cx+rx*(std::cos(next)+h*std::sin(next)),cy-ry*(std::sin(next)-h*std::cos(next)),4);
                        add(working,transform,cx+rx*std::cos(next),cy-ry*std::sin(next),4);theta=next;
                    }
                }
                outline=true;
                if(name=="Chord" || name=="Pie"){if(name=="Pie")add(working,transform,cx,cy,2);working.points.back().type|=1;working.new_figure=true;fill=true;}
                else if(connected){const auto end=at(start+sweep);final_x=static_cast<int>(std::lround(end[0]));final_y=static_cast<int>(std::lround(end[1]));update_position=true;}
                else working.new_figure=true;
            }else return fail(50);
            if(dc.path.state==1){dc.path=std::move(working);if(update_position){dc.x=final_x;dc.y=final_y;}return done(1);}
        }
        if(name=="StrokeAndFillPath"){for(std::size_t i=0;i<working.points.size();++i)if(i+1==working.points.size() || (working.points[i+1].type&6)==6)working.points[i].type|=1;}
        if(name=="PathToRegion" || name=="SelectClipPath"){
            auto region=native_path_region(working.points,dc.poly_fill_mode);
            if(name=="PathToRegion"){const auto handle=gui_.next_object++;gui_.regions.emplace(handle,std::move(region));dc.path={};return done(handle);}
            if(arg(1)<1 || arg(1)>5)return fail(87);
            GuiRegion old;if(dc.application_clip)old=*dc.application_clip;else {int width=1440,height=900;if(const auto bitmap=gui_.bitmaps.find(dc.bitmap);bitmap!=gui_.bitmaps.end()){width=static_cast<int>(bitmap->second.width);height=static_cast<int>(bitmap->second.height);}else if(window!=gui_.windows.end()){width=static_cast<int>(window->second.width);height=static_cast<int>(window->second.height);}else if(arg(0)>=0x10000000){if(const auto remote=shared_gui_window(arg(0)-0x10000000)){width=static_cast<int>(remote->width);height=static_cast<int>(remote->height);}}old={{0,0,width,height}};}
            auto applied=arg(1)==5?region:gui_region_combine(old,region,static_cast<unsigned>(arg(1)));dc.application_clip=std::move(applied);gui_refresh_dc_clip(dc);dc.path={};return done(1);
        }
        const bool inside_frame=line==6 && (name=="Rectangle" || name=="Ellipse" || name=="RoundRect" || name=="Pie" || name=="Chord");
        GuiRegion interior,edge,gaps;
        if(fill && (dc.brush!=0xb005 || inside_frame))interior=native_path_region(working.points,dc.poly_fill_mode);
        if(outline && pen_width){
            if(!geometric && !dash.empty()){
                auto runs=gui_cosmetic_dashes(working.points,dash,!pen.extended && dc.mode==2);
                edge=std::move(runs[0]);gaps=std::move(runs[1]);
            }else if(inside_frame){
                // 從原輪廓向內取完整筆寬，窄小圖形也能完全填滿，且不越過原有邊界。
                const auto band=native_stroke_path(working.points,stroke_transform,pen_width*2,stroke_style,dc.miter_limit);
                edge=gui_region_combine(interior,native_path_region(band,2),1);
                if(dc.brush==0xb005)interior.clear();
            }else edge=native_path_region(stroke(working.points),2);
        }
        if(!interior.empty() && !edge.empty())interior=gui_region_combine(interior,edge,4);
        const auto clip=[&](GuiRegion region){if(dc.clipped)region=gui_region_combine(region,GuiRegion{dc.clip},1);if(!dc.excluded.empty())region=gui_region_combine(region,dc.excluded,4);return region;};interior=clip(std::move(interior));edge=clip(std::move(edge));
        const auto paint=[&](const GuiRegion& region,bool outline_pen){
            GuiDraw draw;draw.kind=GuiDraw::Kind::Fill;draw.device_coordinates=true;draw.rop2=dc.rop2;
            if(outline_pen && pen.pattern_brush){
                const auto brush=pen.pattern_brush;
                draw.color=gui_.brushes.at(brush);
                if(const auto hatch=gui_.brush_hatches.find(brush);hatch!=gui_.brush_hatches.end())draw.hatch=hatch->second;
                if(gui_.brush_patterns.contains(brush))draw.pattern_brush=brush;
                draw.pattern_background=dc.background;draw.pattern_opaque=dc.mode==2;draw.pattern_color=dc.color;draw.brush_origin=dc.brush_origin;
            }else if(outline_pen)draw.color=dc.pen==0xb013?dc.dc_pen_color:gui_.pens.contains(dc.pen)?gui_.pens.at(dc.pen):dc.pen==0xb006?0xffffff:0;
            else {draw.color=dc.brush==0xb012?dc.dc_brush_color:gui_.brushes.contains(dc.brush)?gui_.brushes.at(dc.brush):dc.brush==0xb000?0xffffff:dc.brush==0xb001?0xc0c0c0:dc.brush==0xb002?0x808080:dc.brush==0xb003?0x404040:0;draw.hatch=gui_.brush_hatches.contains(dc.brush)?gui_.brush_hatches.at(dc.brush):-1;draw.pattern_opaque=dc.mode==2;draw.pattern_background=dc.background;draw.pattern_color=dc.color;draw.brush_origin=dc.brush_origin;if(gui_.brush_patterns.contains(dc.brush)){draw.pattern_brush=dc.brush;draw.pattern_monochrome=gui_.brush_patterns.at(dc.brush).bits_per_pixel==1;}}
            for(const auto& rect:region){draw.x=rect[0];draw.y=rect[1];draw.right=rect[2];draw.bottom=rect[3];if(!raster_draw(memory,arg(0),draw))throw std::domain_error("此 DC 尚無 path 繪圖後端");}
        };
        if(!gaps.empty()){
            gaps=clip(std::move(gaps));
            if(!interior.empty())interior=gui_region_combine(interior,gaps,4);
            GuiDraw back;back.kind=GuiDraw::Kind::Fill;back.device_coordinates=true;back.rop2=dc.rop2;back.color=dc.background;
            for(const auto& rect:gaps){back.x=rect[0];back.y=rect[1];back.right=rect[2];back.bottom=rect[3];if(!raster_draw(memory,arg(0),back))return fail(50);}
        }
        paint(interior,false);paint(edge,true);if(closed)dc.path={};if(update_position){dc.x=final_x;dc.y=final_y;}return done(1);
    }catch(const MemoryFault&){
        if(name=="ExtCreatePen" || name=="NtGdiExtCreatePen" || name=="CreatePenIndirect" || name=="PolyDraw" || name=="SetMiterLimit" || name=="GetMiterLimit")return fail(998);
        if(native_poly || name=="ExtCreateRegion")return done(0);
        if(name=="GetPath")return done(UINT32_MAX);
        throw;
    }catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::domain_error&){return fail(50);}catch(const std::invalid_argument&){return fail(87);}
}
}
