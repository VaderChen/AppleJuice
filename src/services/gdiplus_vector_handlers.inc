// GDI+ 向量物件共用 GDI 的原生路徑幾何與 raster 合成，不建立第二個視窗後端。
const auto real=[&](unsigned n)->double{
    auto bits=u32(n);if(!x86_ && n<4){bits=0;for(unsigned i=0;i<4;++i)bits|=std::uint32_t{state.xmm[n][i]}<<(8*i);}
    const auto value=std::bit_cast<float>(bits);if(!std::isfinite(value))throw std::invalid_argument("GDI+ 座標不是有限數值");return value;
};
const auto add_line=[](GdiplusState::Path& path,double x,double y,double right,double bottom){
    if(path.new_figure || path.points.empty()){path.points.push_back({x,y,6});path.new_figure=false;}
    else if(path.points.back().x!=x || path.points.back().y!=y)path.points.push_back({x,y,2});
    path.points.push_back({right,bottom,2});
};
const auto add_arc=[](GdiplusState::Path& path,double x,double y,double width,double height,double start,double sweep){
    if(width<=0 || height<=0)throw std::invalid_argument("GDI+ 橢圓尺寸無效");
    sweep=std::clamp(sweep,-360.0,360.0);if(sweep==0)return;
    constexpr double radians=3.14159265358979323846/180;
    const auto rx=width/2,ry=height/2,cx=x+rx,cy=y+ry;
    auto angle=std::fmod(start,360.0)*radians;const auto segments=static_cast<unsigned>(std::ceil(std::abs(sweep)/90));
    const auto delta=sweep*radians/segments;
    const auto first_x=cx+rx*std::cos(angle),first_y=cy+ry*std::sin(angle);
    if(path.new_figure || path.points.empty())path.points.push_back({first_x,first_y,6});
    else if(path.points.back().x!=first_x || path.points.back().y!=first_y)path.points.push_back({first_x,first_y,2});
    path.new_figure=false;
    for(unsigned i=0;i<segments;++i){const auto end=angle+delta,k=4.0/3*std::tan(delta/4);
        path.points.push_back({cx+rx*(std::cos(angle)-k*std::sin(angle)),cy+ry*(std::sin(angle)+k*std::cos(angle)),4});
        path.points.push_back({cx+rx*(std::cos(end)+k*std::sin(end)),cy+ry*(std::sin(end)-k*std::cos(end)),4});
        path.points.push_back({cx+rx*std::cos(end),cy+ry*std::sin(end),4});angle=end;
    }
};
if(symbol=="GdipCreatePath"){
    if(u32(0)>1 || !arg(1))return finish(invalid_parameter);
    memory.check(GuestAddress{arg(1)},pointer_bits/8,Access::Write);const auto handle=new_handle();if(!handle)return finish(out_of_memory);
    g.paths.emplace(handle,GdiplusState::Path{{},u32(0),true});memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipDeletePath"){
    if(!g.paths.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);
}
if(symbol=="GdipAddPathLine" || symbol=="GdipAddPathArc" || symbol=="GdipClosePathFigure"){
    const auto found=g.paths.find(arg(0));if(found==g.paths.end())return finish(invalid_parameter);
    auto& path=found->second;
    if(symbol=="GdipClosePathFigure"){if(!path.points.empty())path.points.back().type|=1;path.new_figure=true;}
    else if(symbol=="GdipAddPathLine")add_line(path,real(1),real(2),real(3),real(4));
    else add_arc(path,real(1),real(2),real(3),real(4),real(5),real(6));
    return finish(ok);
}
if(symbol=="GdipCreateSolidFill" || symbol=="GdipCloneBrush"){
    if(!arg(1) || (symbol=="GdipCloneBrush" && !g.brushes.contains(arg(0))))return finish(invalid_parameter);
    memory.check(GuestAddress{arg(1)},pointer_bits/8,Access::Write);
    GdiplusBrush brush;if(symbol=="GdipCloneBrush")brush=g.brushes.at(arg(0));else brush.color=u32(0);
    const auto handle=new_handle();if(!handle)return finish(out_of_memory);
    g.brushes[handle]=std::move(brush);memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipDeleteBrush"){
    if(!g.brushes.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);
}
if(symbol=="GdipCreatePen1"){
    const auto width=real(1);if(!arg(3) || width<0 || u32(2)>6)return finish(invalid_parameter);
    memory.check(GuestAddress{arg(3)},pointer_bits/8,Access::Write);const auto handle=new_handle();if(!handle)return finish(out_of_memory);
    {GdiplusState::Pen pen;pen.color=u32(0);pen.width=width;pen.unit=u32(2);g.pens[handle]=std::move(pen);}memory.store(GuestAddress{arg(3)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipDeletePen"){
    if(!g.pens.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);
}
if(symbol=="GdipSetPenStartCap" || symbol=="GdipSetPenEndCap" || symbol=="GdipSetPenLineJoin"){
    // LineCap：Flat 0、Square 1、Round 2、Triangle 3、NoAnchor 0x10～ArrowAnchor 0x14；Custom 只能經 SetPenCustom*Cap 設定。LineJoin 0～3。
    const auto found=g.pens.find(arg(0));const bool join=symbol=="GdipSetPenLineJoin";
    if(found==g.pens.end() || (join?u32(1)>3:!(u32(1)<=3 || (u32(1)>=0x10 && u32(1)<=0x14))))return finish(invalid_parameter);
    if(symbol=="GdipSetPenStartCap")found->second.start_cap=u32(1);
    else if(symbol=="GdipSetPenEndCap")found->second.end_cap=u32(1);else found->second.join=u32(1);
    return finish(ok);
}
if(symbol=="GdipSetSmoothingMode" || symbol=="GdipGetSmoothingMode"){
    if(!g.graphics.contains(arg(0)))return finish(invalid_parameter);
    if(symbol=="GdipSetSmoothingMode"){if(u32(1)>4)return finish(invalid_parameter);g.smoothing[arg(0)]=u32(1);}
    else {if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,g.smoothing[arg(0)]);}
    return finish(ok);
}
// GDI 的 COLORREF 像素（0xAABBGGRR，alpha 只在 argb32 時採用）建立新的 GDI+ 點陣圖；HBITMAP、資源點陣圖與 HICON 共用。
const auto import_pixels=[&](std::uint32_t width,std::uint32_t height,const std::vector<std::uint32_t>& pixels,std::uint32_t format,std::uint64_t output)->std::uint32_t{
    const auto size=std::uint64_t{width}*height*4;if(!size || size>stream_limit || pixels.size()<std::size_t{width}*height)return out_of_memory;
    memory.check(GuestAddress{output},pointer_bits/8,Access::Write);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    for(std::size_t i=0;i<std::size_t{width}*height;++i){const auto c=pixels[i];const auto argb=(format==argb32?(c&0xff000000U):0xff000000U)|((c&255U)<<16)|(c&0xff00U)|((c>>16)&255U);
        for(unsigned k=0;k<4;++k)bytes[i*4+k]=static_cast<std::uint8_t>(argb>>(8*k));}
    const auto buffer=allocate_heap(memory,size);if(!buffer)return out_of_memory;memory.write(GuestAddress{buffer},bytes);
    const auto handle=new_handle();if(!handle){release_handle(buffer);return out_of_memory;}
    g.bitmaps[handle]={width,height,format,static_cast<std::int64_t>(width)*4,buffer,true};memory.store(GuestAddress{output},pointer_bits,handle);return ok;
};
if(symbol=="GdipCreateBitmapFromHBITMAP"){
    if(!arg(2) || !gui_.bitmaps.contains(arg(0)))return finish(invalid_parameter);
    memory.check(GuestAddress{arg(2)},pointer_bits/8,Access::Write);memory.store(GuestAddress{arg(2)},pointer_bits,0);
    sync_raster_bitmap(memory,arg(0));const auto& source=gui_.bitmaps.at(arg(0));return finish(import_pixels(source.width,source.height,source.pixels,rgb32,arg(2)));
}
// Graphics 的世界→頁面→裝置變換：頁面單位依 96 DPI 換算（UnitDisplay 與 UnitPixel 為像素），再乘頁面比例。
const auto page_factor=[](const GdiplusState::Graphics& info){static constexpr double units[]{1,1,1,96.0/72,96,96.0/300,96.0/25.4};return units[std::min(info.page_unit,6U)]*info.page_scale;};
const auto world_to_device=[&](std::uint64_t handle,const GuiDc& dc){const auto& info=g.graphics_state[handle];const auto factor=page_factor(info);
    return gui_multiply(gui_multiply(info.world,GuiTransform{factor,0,0,factor,0,0}),gui_device_transform(dc));};
// 裝置座標區域（scale 為超取樣倍數）先與裁切交集，再依筆刷著色器在像素中心取色、以覆蓋率合成；路徑繪製與 GdipFillRegion 共用。
const auto paint_region=[&](std::uint64_t graphics_handle,const GdiplusShader& shader,GuiRegion region,unsigned scale)->std::uint32_t{
    const auto graphics=g.graphics.find(graphics_handle);if(graphics==g.graphics.end() || !gui_.contexts.contains(graphics->second))return invalid_parameter;
    auto dc=gui_.contexts.at(graphics->second);
    if(const auto& clip=g.graphics_state[graphics_handle].clip){
        // 裁切為裝置座標；超取樣時一併放大後交集。
        GuiRegion scaled=*clip;for(auto& rect:scaled)for(auto& v:rect)v=static_cast<std::int32_t>(std::clamp<std::int64_t>(std::int64_t{v}*scale,-(1LL<<30),1LL<<30));
        region=gui_region_combine(region,scaled,1);
    }
    if(region.empty())return ok;
    const auto surface=raster_surface(memory,graphics->second);if(!surface)return generic_error;
    auto bounds=gui_region_bounds(region);
    const auto left=std::max(0,static_cast<int>(std::floor(static_cast<double>(bounds[0])/scale)));
    const auto top=std::max(0,static_cast<int>(std::floor(static_cast<double>(bounds[1])/scale)));
    const auto right=std::min(static_cast<int>(surface->width()),static_cast<int>(std::ceil(static_cast<double>(bounds[2])/scale)));
    const auto bottom=std::min(static_cast<int>(surface->height()),static_cast<int>(std::ceil(static_cast<double>(bounds[3])/scale)));
    if(left>=right || top>=bottom)return ok;
    const auto width=static_cast<unsigned>(right-left),height=static_cast<unsigned>(bottom-top);
    if(std::uint64_t{width}*height>stream_limit/4)return out_of_memory;
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(width)*height);
    const auto sample=static_cast<int>(scale);
    for(const auto& rect:region)for(auto y=std::max(rect[1],top*sample);y<std::min(rect[3],bottom*sample);++y)
        for(auto x=std::max(rect[0],left*sample);x<std::min(rect[2],right*sample);++x)
            ++coverage[static_cast<std::size_t>(y/sample-top)*width+static_cast<unsigned>(x/sample-left)];
    std::vector<std::uint32_t> pixels(coverage.size());
    for(std::size_t i=0;i<pixels.size();++i){if(!coverage[i])continue;
        const auto color=shader(left+static_cast<double>(i%width)+0.5,top+static_cast<double>(i/width)+0.5);const auto alpha=((color>>24)*coverage[i]+scale*scale/2)/(scale*scale);const auto argb=premultiply((color&0xffffffU)|(alpha<<24));pixels[i]=(argb&0xff00ff00U)|((argb&255U)<<16)|((argb>>16)&255U);}
    const auto source=make_hardware_raster(width,height);source->upload(pixels,true);
    dc.world=gui_identity;dc.viewport_extent=dc.window_extent={1,1};dc.viewport_origin=dc.window_origin={0,0};
    surface->blit(*source,{left,top,right,bottom},{0,0,static_cast<int>(width),static_cast<int>(height)},dc,0x01ff0000U);
    return ok;
};
// 畫筆的實際寬度（世界座標）：寬度 0 為 1 個單位。
const auto pen_width=[](const GdiplusState::Pen& pen){static constexpr double units[]{1,1,1,96.0/72,96,96.0/300,96.0/25.4};return pen.width==0?1.0:pen.width*units[std::min(pen.unit,6U)];};
// 依虛線圖樣（已乘筆寬）把攤平的路徑切成開放折線；每個 figure 從 offset 處重新開始，封閉 figure 回到起點。
const auto dash_path=[](const std::vector<GuiPathPoint>& flat,const std::vector<double>& pattern,double offset){
    std::vector<GuiPathPoint> out;double total=0;for(const auto v:pattern)total+=v;if(!(total>0))return flat;
    for(std::size_t begin=0;begin<flat.size();){
        auto end=begin+1;while(end<flat.size() && (flat[end].type&6)!=6)++end;
        std::vector<std::array<double,2>> points;for(auto i=begin;i<end;++i)points.push_back({flat[i].x,flat[i].y});if(flat[end-1].type&1)points.push_back({flat[begin].x,flat[begin].y});
        auto phase=std::fmod(offset,total);if(phase<0)phase+=total;std::size_t index=0;while(phase>=pattern[index]){phase-=pattern[index];index=(index+1)%pattern.size();}
        double remain=pattern[index]-phase;bool on=index%2==0;if(on && !points.empty())out.push_back({points[0][0],points[0][1],6});
        for(std::size_t i=1;i<points.size();++i){
            const auto a=points[i-1],b=points[i];const auto length=std::hypot(b[0]-a[0],b[1]-a[1]);double t=0;
            while(length-t>remain){t+=remain;const auto f=t/length;const GuiPathPoint at{a[0]+(b[0]-a[0])*f,a[1]+(b[1]-a[1])*f,static_cast<std::uint8_t>(on?2:6)};out.push_back(at);
                on=!on;index=(index+1)%pattern.size();remain=pattern[index];}
            remain-=length-t;if(on)out.push_back({b[0],b[1],2});
        }
        begin=end;
    }
    // 去掉只有起點的虛線段。
    std::vector<GuiPathPoint> cleaned;for(std::size_t i=0;i<out.size();++i){if((out[i].type&6)==6 && (i+1==out.size() || (out[i+1].type&6)==6))continue;cleaned.push_back(out[i]);}
    return cleaned;
};
const auto dash_pattern=[&](const GdiplusState::Pen& pen,double width)->std::vector<double>{
    std::vector<double> pattern;switch(pen.dash){case 1:pattern={3,1};break;case 2:pattern={1,1};break;case 3:pattern={3,1,1,1};break;case 4:pattern={3,1,1,1,1,1};break;case 5:pattern=pen.dashes;break;default:break;}
    for(auto& v:pattern)v*=width;return pattern;};
// 共用繪製：畫筆描成輪廓（虛線、複合線、筆尖變換、內縮與線端），再經世界／頁面／裝置變換、裁切與超取樣覆蓋率合成；所有 Draw*／Fill* 都經此處。
// forced_scale 非 0 時指定超取樣倍數（文字依 TextRenderingHint 決定反鋸齒）。
const auto render=[&](std::uint64_t graphics_handle,bool fill,std::uint64_t tool,GdiplusState::Path path,unsigned forced_scale=0)->std::uint32_t{
    const auto graphics=g.graphics.find(graphics_handle);
    if(graphics==g.graphics.end() || !gui_.contexts.contains(graphics->second) || (fill?!g.brushes.contains(tool):!g.pens.contains(tool)))return invalid_parameter;
    auto dc=gui_.contexts.at(graphics->second);
    if(path.points.empty())return ok;
    // 每層為（外輪廓, 要扣除的內輪廓）；inside 非空時再與原路徑內部交集（內縮畫筆）。
    struct Layer {std::vector<GuiPathPoint> outer,inner;};std::vector<Layer> layers;std::vector<GuiPathPoint> inside;
    if(!fill){
        const auto& pen=g.pens.at(tool);const auto width=pen_width(pen);const auto join=pen.join==1?0x1000U:pen.join==2?0U:0x2000U;
        auto flat=gui_flatten_path(path.points);
        // 自訂線端先依 base inset 縮短開放 figure 的兩端。
        const auto shorten=[&](GuiPathPoint& at,const GuiPathPoint& toward,double by){const auto dx=toward.x-at.x,dy=toward.y-at.y,length=std::hypot(dx,dy);if(by<=0 || length==0)return;const auto f=std::min(by,length*0.999)/length;at.x+=dx*f;at.y+=dy*f;};
        std::vector<std::pair<std::size_t,std::size_t>> open;
        for(std::size_t begin=0;begin<flat.size();){auto end=begin+1;while(end<flat.size() && (flat[end].type&6)!=6)++end;if(end-begin>=2 && !(flat[end-1].type&1))open.push_back({begin,end-1});begin=end;}
        const auto anchors=flat;
        for(const auto& [b,e]:open){if(pen.start_cap==0xff && pen.custom_start)shorten(flat[b],flat[b+1],pen.custom_start->inset*width*pen.custom_start->scale);
            if(pen.end_cap==0xff && pen.custom_end)shorten(flat[e],flat[e-1],pen.custom_end->inset*width*pen.custom_end->scale);}
        const auto pattern=dash_pattern(pen,width);const bool custom=pen.start_cap==0xff || pen.end_cap==0xff;
        const auto centre=!pattern.empty()?dash_path(flat,pattern,pen.dash_offset*width):custom?flat:path.points;
        const auto cap_bits=!pattern.empty() && pen.dash_cap==2?0U:0x200U;
        const auto stroke=[&](double w){return w>0?native_stroke_path(centre,pen.transform,w,cap_bits|join,pen.miter):std::vector<GuiPathPoint>{};};
        if(pen.compound.size()>=2)for(std::size_t i=0;i+1<pen.compound.size();i+=2){
            // 複合線以中心對稱近似：每段為外寬輪廓扣除內寬輪廓。
            const auto d1=(pen.compound[i]-0.5)*width,d2=(pen.compound[i+1]-0.5)*width;const auto outer=2*std::max(std::abs(d1),std::abs(d2)),inner=d1*d2>0?2*std::min(std::abs(d1),std::abs(d2)):0.0;
            layers.push_back({stroke(outer),stroke(inner)});}
        else layers.push_back({stroke(pen.alignment==1?2*width:width),{}});
        if(layers.empty())layers.push_back({});
        if(pen.alignment==1 && open.empty())inside=gui_flatten_path(path.points);
        auto& extra=layers.front().outer;
        // 線端：Square 1、Round 2、Triangle 3；錨點 SquareAnchor 0x11、RoundAnchor 0x12、DiamondAnchor 0x13、ArrowAnchor 0x14 置中於端點；Custom 0xff。
        const auto cap=[&](const GuiPathPoint& at,const GuiPathPoint& near,unsigned type,const std::shared_ptr<const GdiplusState::CustomCap>& custom_cap){
            if(!type || type==0x10)return;const auto dx=at.x-near.x,dy=at.y-near.y,length=std::hypot(dx,dy);if(length==0)return;
            const auto r=width/2,ux=dx/length,uy=dy/length;
            const auto polygon=[&](std::initializer_list<std::array<double,2>> local,double unit){bool first=true;for(const auto& p:local){extra.push_back({at.x+(p[0]*uy+p[1]*ux)*unit,at.y+(-p[0]*ux+p[1]*uy)*unit,static_cast<std::uint8_t>(first?6:2)});first=false;}extra.back().type|=1;};
            const auto circle=[&](double radius){GdiplusState::Path c;add_arc(c,at.x-radius,at.y-radius,radius*2,radius*2,0,360);c.points.back().type|=1;extra.insert(extra.end(),c.points.begin(),c.points.end());};
            if(type==1)polygon({{-1,0},{-1,1},{1,1},{1,0}},r);else if(type==2)circle(r);else if(type==3)polygon({{-1,0},{0,1},{1,0}},r);
            else if(type==0x11)polygon({{-1,-1},{-1,1},{1,1},{1,-1}},width);else if(type==0x12)circle(width);else if(type==0x13)polygon({{0,-1.5},{-1.5,0},{0,1.5},{1.5,0}},width);
            else if(type==0x14)polygon({{0,1},{-1.2,-1.5},{1.2,-1.5}},width);
            else if(type==0xff && custom_cap){
                // 自訂線端座標以筆寬（乘 WidthScale）為單位，+Y 沿線向外。
                const auto unit=width*custom_cap->scale;const auto place=[&](GuiPathPoint p){return GuiPathPoint{at.x+(p.x*uy+p.y*ux)*unit,at.y+(-p.x*ux+p.y*uy)*unit,p.type};};
                for(const auto& p:gui_flatten_path(custom_cap->fill.points))extra.push_back(place(p));
                if(!custom_cap->stroke.points.empty()){std::vector<GuiPathPoint> placed;for(const auto& p:gui_flatten_path(custom_cap->stroke.points))placed.push_back(place(p));
                    const auto outline=native_stroke_path(placed,gui_identity,width,0x200U|join,pen.miter);extra.insert(extra.end(),outline.begin(),outline.end());}
            }
        };
        for(const auto& [b,e]:open){cap(anchors[b],anchors[b+1],pen.start_cap,pen.custom_start);cap(anchors[e],anchors[e-1],pen.end_cap,pen.custom_end);}
    }else layers.push_back({path.points,{}});
    const unsigned scale=forced_scale?forced_scale:g.smoothing[graphics_handle]==2 || g.smoothing[graphics_handle]==4?4:1;
    const auto transform=world_to_device(graphics_handle,dc);
    const auto device=[&](std::vector<GuiPathPoint> points){for(auto& point:points){const auto mapped=gui_point(transform,point.x,point.y);point.x=mapped[0]*scale;point.y=mapped[1]*scale;}return points;};
    const auto fill_mode=fill?path.fill+1:2U;GuiRegion region;bool first=true;
    for(const auto& layer:layers){
        if(layer.outer.empty())continue;auto part=native_path_region(device(layer.outer),fill_mode);
        if(!layer.inner.empty())part=gui_region_combine(part,native_path_region(device(layer.inner),2),4);
        region=first?std::move(part):gui_region_combine(region,part,2);first=false;
    }
    if(!inside.empty())region=gui_region_combine(region,native_path_region(device(inside),path.fill+1),1);
    const auto& info=g.graphics_state[graphics_handle];const auto& pen_fill=fill?nullptr:g.pens.at(tool).fill;
    const auto shader=fill?gdiplus_shader(g.brushes.at(tool),transform,info.origin_x,info.origin_y):pen_fill?gdiplus_shader(*pen_fill,transform,info.origin_x,info.origin_y):
        GdiplusShader{[color=g.pens.at(tool).color](double,double){return color;}};
    return paint_region(graphics_handle,shader,std::move(region),scale);
};
if(symbol=="GdipFillPath" || symbol=="GdipDrawPath" || symbol=="GdipFillRectangle" || symbol=="GdipDrawRectangle" ||
   symbol=="GdipFillEllipse" || symbol=="GdipDrawEllipse" || symbol=="GdipFillPolygon" || symbol=="GdipDrawLine" || symbol=="GdipDrawLineI" ||
   symbol=="GdipFillRectangleI" || symbol=="GdipDrawRectangleI" || symbol=="GdipFillEllipseI" || symbol=="GdipDrawEllipseI"){
    const bool fill=symbol.starts_with("GdipFill");
    const auto graphics=g.graphics.find(arg(0));
    if(graphics==g.graphics.end() || !gui_.contexts.contains(graphics->second) ||
       (fill?!g.brushes.contains(arg(1)):!g.pens.contains(arg(1))))return finish(invalid_parameter);
    GdiplusState::Path path;
    if(symbol.ends_with("Path")){const auto p=g.paths.find(arg(2));if(p==g.paths.end())return finish(invalid_parameter);path=p->second;}
    else if(symbol=="GdipFillPolygon"){
        if(!arg(2) || s32(3)<3 || u32(3)>1048576 || u32(4)>1)return finish(invalid_parameter);path.fill=u32(4);
        for(unsigned i=0;i<u32(3);++i){const auto at=arg(2)+i*8;
            const auto x=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{at},32)));
            const auto y=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32)));
            if(!std::isfinite(x) || !std::isfinite(y))return finish(invalid_parameter);
            path.points.push_back({x,y,static_cast<std::uint8_t>(i?2:6)});
        }path.points.back().type|=1;
    }else {
        const bool integer=symbol.ends_with('I');const auto number=[&](unsigned n){return integer?static_cast<double>(s32(n)):real(n);};
        const auto x=number(2),y=number(3),w=number(4),h=number(5);
        if(symbol.find("Line")!=symbol.npos)add_line(path,x,y,w,h);
        else {if(w<=0 || h<=0)return finish(invalid_parameter);
            if(symbol.find("Ellipse")!=symbol.npos)add_arc(path,x,y,w,h,0,360);
            else path.points={{x,y,6},{x+w,y,2},{x+w,y+h,2},{x,y+h,2}};
            path.points.back().type|=1;
        }
    }
    return finish(render(arg(0),fill,arg(1),std::move(path)));
}
