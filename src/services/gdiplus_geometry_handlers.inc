// GDI+ 幾何家族：GpMatrix、Graphics 狀態／變換／裁切、GpPath 編輯與查詢、Draw*／Fill* 變體與曲線、畫筆／筆刷存取。
// 所有繪製都轉成路徑交給 gdiplus_vector_handlers.inc 的 render；座標變換與 GDI 共用 GuiTransform（列向量）。
const auto point_array=[&](std::uint64_t at,std::int64_t count,bool integer){
    if(!at || count<0 || count>1048576)throw std::invalid_argument("GDI+ 點陣列無效");
    std::vector<std::array<double,2>> out(static_cast<std::size_t>(count));
    for(std::size_t i=0;i<out.size();++i)for(unsigned k=0;k<2;++k){
        const auto raw=static_cast<std::uint32_t>(memory.load(GuestAddress{at+i*8+k*4},32));
        const double value=integer?static_cast<double>(std::bit_cast<std::int32_t>(raw)):static_cast<double>(std::bit_cast<float>(raw));
        if(!std::isfinite(value))throw std::invalid_argument("GDI+ 座標不是有限數值");out[i][k]=value;}
    return out;
};
const auto rect_array=[&](std::uint64_t at,std::int64_t count,bool integer){
    const auto values=point_array(at,count*2,integer);std::vector<std::array<double,4>> out(static_cast<std::size_t>(count));
    for(std::size_t i=0;i<out.size();++i)out[i]={values[i*2][0],values[i*2][1],values[i*2+1][0],values[i*2+1][1]};return out;
};
const auto store_real=[&](std::uint64_t at,double value){memory.store(GuestAddress{at},32,std::bit_cast<std::uint32_t>(static_cast<float>(value)));};
const auto store_points=[&](std::uint64_t at,const std::vector<std::array<double,2>>& points,bool integer){
    memory.check(GuestAddress{at},points.size()*8,Access::Write);
    for(std::size_t i=0;i<points.size();++i)for(unsigned k=0;k<2;++k){const auto value=points[i][k];
        memory.store(GuestAddress{at+i*8+k*4},32,integer?static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(value+0.5))):std::bit_cast<std::uint32_t>(static_cast<float>(value)));}
};
const auto store_rect=[&](std::uint64_t at,double x,double y,double w,double h,bool integer){store_points(at,{{x,y},{w,h}},integer);};
const auto coordinate=[&](unsigned n,bool integer){return integer?static_cast<double>(s32(n)):real(n);};
const auto new_object=[&](std::uint64_t output)->std::uint64_t{if(!output)throw std::invalid_argument("GDI+ 輸出指標為 NULL");memory.check(GuestAddress{output},pointer_bits/8,Access::Write);return new_handle();};
// MatrixOrderPrepend（0）讓新變換先套用；MatrixOrderAppend（1）讓新變換後套用。
const auto combine_matrix=[](GuiTransform& target,const GuiTransform& operation,std::uint32_t order){
    if(order>1)throw std::invalid_argument("GDI+ MatrixOrder 無效");target=order?gui_multiply(target,operation):gui_multiply(operation,target);};
const auto rotation=[](double degrees){const auto r=degrees*3.14159265358979323846/180;return GuiTransform{std::cos(r),std::sin(r),-std::sin(r),std::cos(r),0,0};};
// 路徑建構：延續目前 figure 的線段與 Bezier；封閉形狀另起 figure 並在結尾標記關閉。
const auto line_to=[](GdiplusState::Path& path,double x,double y){
    if(path.new_figure || path.points.empty()){path.points.push_back({x,y,6});path.new_figure=false;}
    else if(path.points.back().x!=x || path.points.back().y!=y)path.points.push_back({x,y,2});
};
const auto add_lines=[&](GdiplusState::Path& path,const std::vector<std::array<double,2>>& points){for(const auto& p:points)line_to(path,p[0],p[1]);};
const auto close_figure=[](GdiplusState::Path& path){if(!path.points.empty())path.points.back().type|=1;path.new_figure=true;};
const auto add_beziers=[&](GdiplusState::Path& path,const std::vector<std::array<double,2>>& points){
    if(points.empty() || (points.size()-1)%3)throw std::invalid_argument("GDI+ Bezier 點數必須為 3n+1");
    line_to(path,points[0][0],points[0][1]);for(std::size_t i=1;i<points.size();++i)path.points.push_back({points[i][0],points[i][1],4});path.new_figure=false;
};
// 基數樣條：每段控制點為端點加減相鄰兩點差的 tension／3；封閉曲線的索引環狀延伸。
const auto cardinal=[](const std::vector<std::array<double,2>>& points,double tension,bool closed,std::size_t first,std::size_t segments){
    std::vector<std::array<double,2>> out;const auto n=points.size();if(n<2)throw std::invalid_argument("GDI+ 曲線至少需要兩點");
    const auto at=[&](std::ptrdiff_t i){if(closed)return points[static_cast<std::size_t>((i%static_cast<std::ptrdiff_t>(n)+static_cast<std::ptrdiff_t>(n))%static_cast<std::ptrdiff_t>(n))];return points[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(i,0,static_cast<std::ptrdiff_t>(n)-1))];};
    const auto k=tension/3;out.push_back(at(static_cast<std::ptrdiff_t>(first)));
    for(std::size_t s=0;s<segments;++s){const auto i=static_cast<std::ptrdiff_t>(first+s);const auto p0=at(i-1),p1=at(i),p2=at(i+1),p3=at(i+2);
        out.push_back({p1[0]+(p2[0]-p0[0])*k,p1[1]+(p2[1]-p0[1])*k});out.push_back({p2[0]-(p3[0]-p1[0])*k,p2[1]-(p3[1]-p1[1])*k});out.push_back(p2);}
    return out;
};
const auto add_rectangle=[&](GdiplusState::Path& path,double x,double y,double w,double h){
    path.new_figure=true;line_to(path,x,y);path.points.push_back({x+w,y,2});path.points.push_back({x+w,y+h,2});path.points.push_back({x,y+h,2});close_figure(path);};
const auto add_ellipse=[&](GdiplusState::Path& path,double x,double y,double w,double h){path.new_figure=true;add_arc(path,x,y,w,h,0,360);close_figure(path);};
const auto add_pie=[&](GdiplusState::Path& path,double x,double y,double w,double h,double start,double sweep){
    path.new_figure=true;line_to(path,x+w/2,y+h/2);path.new_figure=false;add_arc(path,x,y,w,h,start,sweep);close_figure(path);};
const auto path_of=[&](std::uint64_t handle)->GdiplusState::Path&{const auto found=g.paths.find(handle);if(found==g.paths.end())throw std::invalid_argument("GDI+ 路徑無效");return found->second;};
const auto matrix_of=[&](std::uint64_t handle)->GuiTransform&{const auto found=g.matrices.find(handle);if(found==g.matrices.end())throw std::invalid_argument("GDI+ 矩陣無效");return found->second;};
const auto pen_of=[&](std::uint64_t handle)->GdiplusState::Pen&{const auto found=g.pens.find(handle);if(found==g.pens.end())throw std::invalid_argument("GDI+ 畫筆無效");return found->second;};
const auto graphics_of=[&](std::uint64_t handle)->GdiplusState::Graphics&{if(!g.graphics.contains(handle))throw std::invalid_argument("GDI+ Graphics 無效");return g.graphics_state[handle];};
const auto transform_path=[](GdiplusState::Path& path,const GuiTransform& m){for(auto& point:path.points){const auto mapped=gui_point(m,point.x,point.y);point.x=mapped[0];point.y=mapped[1];}};
// GDI+ 路徑點型別：Start 0、Line 1、Bezier 3、CloseSubpath 0x80；內部沿用 GDI 的 PT_MOVETO 6、PT_LINETO 2、PT_BEZIERTO 4、PT_CLOSEFIGURE 1。
// 標記點（PathPointTypePathMarker 0x20）在內部沿用同一個位元。
const auto public_type=[](std::uint8_t type){return static_cast<std::uint8_t>(((type&6)==6?0U:(type&6)==4?3U:1U)|((type&1)?0x80U:0U)|(type&0x20U));};
const auto private_type=[](std::uint8_t type){const auto kind=type&7;return static_cast<std::uint8_t>((kind==0?6U:kind==3?4U:2U)|((type&0x80)?1U:0U)|(type&0x20U));};
const auto device_of=[&](std::uint64_t graphics)->GuiTransform{const auto found=g.graphics.find(graphics);if(found==g.graphics.end() || !gui_.contexts.contains(found->second))throw std::invalid_argument("GDI+ Graphics 無效");return world_to_device(graphics,gui_.contexts.at(found->second));};
// 世界座標的路徑轉成裝置座標區域（裁切與可見性判斷共用）。
const auto device_region=[&](std::uint64_t graphics,GdiplusState::Path path){transform_path(path,device_of(graphics));return native_path_region(gui_flatten_path(path.points),path.fill+1);};
const GuiRegion infinite{{-(1<<26),-(1<<26),(1<<26)-1,(1<<26)-1}};
// CombineMode：Replace 0、Intersect 1、Union 2、Xor 3、Exclude 4、Complement 5；nullopt 表示無限大裁切。
const auto combine_clip=[&](std::optional<GuiRegion>& clip,const std::optional<GuiRegion>& region,std::uint32_t mode){
    if(mode>5)throw std::invalid_argument("GDI+ CombineMode 無效");
    const auto current=clip?*clip:infinite,added=region?*region:infinite;
    if(mode==0){clip=region;return;}
    if(mode==1){if(!clip)clip=region;else if(region)clip=gui_region_combine(current,added,1);return;}
    if(mode==2){if(!clip || !region)clip.reset();else clip=gui_region_combine(current,added,2);return;}
    clip=mode==3?gui_region_combine(current,added,3):mode==4?gui_region_combine(current,added,4):gui_region_combine(added,current,4);
};
const auto surface_region=[&](std::uint64_t graphics)->GuiRegion{
    const auto surface=raster_surface(memory,g.graphics.at(graphics));if(!surface)return infinite;
    return {{0,0,static_cast<std::int32_t>(surface->width()),static_cast<std::int32_t>(surface->height())}};};
const auto visible_region=[&](std::uint64_t graphics){const auto& info=graphics_of(graphics);const auto bounds=surface_region(graphics);return info.clip?gui_region_combine(*info.clip,bounds,1):bounds;};
// 裝置座標的矩形換回世界座標的外接矩形。
const auto world_bounds=[&](std::uint64_t graphics,const GuiRect& rect)->std::array<double,4>{
    const auto inverse=gui_inverse(device_of(graphics));if(!inverse)return {0,0,0,0};double l=INFINITY,t=INFINITY,r=-INFINITY,b=-INFINITY;
    for(const auto& p:{gui_point(*inverse,rect[0],rect[1]),gui_point(*inverse,rect[2],rect[1]),gui_point(*inverse,rect[0],rect[3]),gui_point(*inverse,rect[2],rect[3])}){l=std::min(l,p[0]);t=std::min(t,p[1]);r=std::max(r,p[0]);b=std::max(b,p[1]);}
    return {l,t,r-l,b-t};
};
// ---- GpMatrix ----
if(symbol=="GdipCreateMatrix" || symbol=="GdipCreateMatrix2" || symbol=="GdipCreateMatrix3" || symbol=="GdipCreateMatrix3I" || symbol=="GdipCloneMatrix"){
    GuiTransform value{1,0,0,1,0,0};unsigned output=0;
    if(symbol=="GdipCreateMatrix2"){value={real(0),real(1),real(2),real(3),real(4),real(5)};output=6;}
    else if(symbol.starts_with("GdipCreateMatrix3")){
        // 把矩形映到由左上、右上、左下三點定義的平行四邊形。
        const bool integer=symbol.ends_with('I');const auto rect=rect_array(arg(0),1,integer)[0];const auto points=point_array(arg(1),3,integer);
        if(rect[2]==0 || rect[3]==0)return finish(invalid_parameter);
        const auto m11=(points[1][0]-points[0][0])/rect[2],m12=(points[1][1]-points[0][1])/rect[2],m21=(points[2][0]-points[0][0])/rect[3],m22=(points[2][1]-points[0][1])/rect[3];
        value={m11,m12,m21,m22,points[0][0]-m11*rect[0]-m21*rect[1],points[0][1]-m12*rect[0]-m22*rect[1]};output=2;
    }else if(symbol=="GdipCloneMatrix"){value=matrix_of(arg(0));output=1;}
    const auto handle=new_object(arg(output));if(!handle)return finish(out_of_memory);
    g.matrices[handle]=value;memory.store(GuestAddress{arg(output)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipDeleteMatrix"){if(!g.matrices.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);}
if(symbol=="GdipSetMatrixElements"){matrix_of(arg(0))={real(1),real(2),real(3),real(4),real(5),real(6)};return finish(ok);}
if(symbol=="GdipGetMatrixElements"){const auto& m=matrix_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.check(GuestAddress{arg(1)},24,Access::Write);for(unsigned i=0;i<6;++i)store_real(arg(1)+i*4,m[i]);return finish(ok);}
if(symbol=="GdipMultiplyMatrix"){combine_matrix(matrix_of(arg(0)),matrix_of(arg(1)),u32(2));return finish(ok);}
if(symbol=="GdipTranslateMatrix"){combine_matrix(matrix_of(arg(0)),{1,0,0,1,real(1),real(2)},u32(3));return finish(ok);}
if(symbol=="GdipScaleMatrix"){combine_matrix(matrix_of(arg(0)),{real(1),0,0,real(2),0,0},u32(3));return finish(ok);}
if(symbol=="GdipShearMatrix"){combine_matrix(matrix_of(arg(0)),{1,real(2),real(1),1,0,0},u32(3));return finish(ok);}
if(symbol=="GdipRotateMatrix"){combine_matrix(matrix_of(arg(0)),rotation(real(1)),u32(2));return finish(ok);}
if(symbol=="GdipInvertMatrix"){auto& m=matrix_of(arg(0));const auto inverse=gui_inverse(m);if(!inverse)return finish(invalid_parameter);m=*inverse;return finish(ok);}
if(symbol=="GdipTransformMatrixPoints" || symbol=="GdipTransformMatrixPointsI" || symbol=="GdipVectorTransformMatrixPoints" || symbol=="GdipVectorTransformMatrixPointsI"){
    const bool integer=symbol.ends_with('I'),vector=symbol.starts_with("GdipVector");auto m=matrix_of(arg(0));if(vector)m[4]=m[5]=0;
    if(s32(2)<=0)return finish(invalid_parameter);auto points=point_array(arg(1),s32(2),integer);
    for(auto& p:points){const auto mapped=gui_point(m,p[0],p[1]);p={mapped[0],mapped[1]};}store_points(arg(1),points,integer);return finish(ok);
}
if(symbol=="GdipIsMatrixIdentity" || symbol=="GdipIsMatrixInvertible" || symbol=="GdipIsMatrixEqual"){
    const auto& m=matrix_of(arg(0));const unsigned out=symbol=="GdipIsMatrixEqual"?2U:1U;if(!arg(out))return finish(invalid_parameter);
    bool result=false;
    if(symbol=="GdipIsMatrixIdentity")result=m==GuiTransform{1,0,0,1,0,0};
    else if(symbol=="GdipIsMatrixInvertible")result=gui_inverse(m).has_value();
    else {const auto& other=matrix_of(arg(1));result=true;for(unsigned i=0;i<6;++i)if(static_cast<float>(m[i])!=static_cast<float>(other[i]))result=false;}
    memory.store(GuestAddress{arg(out)},32,result?1U:0U);return finish(ok);
}
// ---- Graphics：世界變換、頁面、提示與狀態 ----
if(symbol=="GdipCreateFromHWND" || symbol=="GdipCreateFromHWNDICM" || symbol=="GdipCreateFromHDC2"){
    const bool window=symbol!="GdipCreateFromHDC2";const unsigned output=window?1U:2U;if(!arg(output))return finish(invalid_parameter);
    const auto dc=window?nested_service(state,memory,"user32.dll","GetDC",{arg(0)}):arg(0);if(!dc || !gui_.contexts.contains(dc))return finish(window?out_of_memory:invalid_parameter);
    const auto handle=new_object(arg(output));if(!handle){if(window)(void)nested_service(state,memory,"user32.dll","ReleaseDC",{arg(0),dc});return finish(out_of_memory);}
    g.graphics[handle]=dc;if(window){auto& info=g.graphics_state[handle];info.owned_dc=true;info.window=arg(0);}
    memory.store(GuestAddress{arg(output)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipGetDC"){graphics_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},pointer_bits,g.graphics.at(arg(0)));return finish(ok);}
if(symbol=="GdipReleaseDC"){graphics_of(arg(0));return finish(arg(1)==g.graphics.at(arg(0))?ok:invalid_parameter);}
if(symbol=="GdipFlush"){graphics_of(arg(0));(void)nested_service(state,memory,"gdi32.dll","GdiFlush",{});return finish(ok);}
if(symbol=="GdipSetWorldTransform"){graphics_of(arg(0)).world=matrix_of(arg(1));return finish(ok);}
if(symbol=="GdipGetWorldTransform"){matrix_of(arg(1))=graphics_of(arg(0)).world;return finish(ok);}
if(symbol=="GdipResetWorldTransform"){graphics_of(arg(0)).world={1,0,0,1,0,0};return finish(ok);}
if(symbol=="GdipMultiplyWorldTransform"){combine_matrix(graphics_of(arg(0)).world,matrix_of(arg(1)),u32(2));return finish(ok);}
if(symbol=="GdipTranslateWorldTransform"){combine_matrix(graphics_of(arg(0)).world,{1,0,0,1,real(1),real(2)},u32(3));return finish(ok);}
if(symbol=="GdipScaleWorldTransform"){combine_matrix(graphics_of(arg(0)).world,{real(1),0,0,real(2),0,0},u32(3));return finish(ok);}
if(symbol=="GdipRotateWorldTransform"){combine_matrix(graphics_of(arg(0)).world,rotation(real(1)),u32(2));return finish(ok);}
if(symbol=="GdipSetPageUnit"){auto& info=graphics_of(arg(0));if(u32(1)<1 || u32(1)>6)return finish(invalid_parameter);info.page_unit=u32(1);return finish(ok);}
if(symbol=="GdipSetPageScale"){auto& info=graphics_of(arg(0));const auto scale=real(1);if(!(scale>0) || scale>1000000000)return finish(invalid_parameter);info.page_scale=scale;return finish(ok);}
if(symbol=="GdipGetPageScale" || symbol=="GdipGetDpiX" || symbol=="GdipGetDpiY"){
    const auto& info=graphics_of(arg(0));if(!arg(1))return finish(invalid_parameter);store_real(arg(1),symbol=="GdipGetPageScale"?info.page_scale:96.0);return finish(ok);
}
if(symbol=="GdipSetRenderingOrigin"){auto& info=graphics_of(arg(0));info.origin_x=s32(1);info.origin_y=s32(2);return finish(ok);}
if(symbol=="GdipGetRenderingOrigin"){const auto& info=graphics_of(arg(0));if(!arg(1) || !arg(2))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(info.origin_x));memory.store(GuestAddress{arg(2)},32,static_cast<std::uint32_t>(info.origin_y));return finish(ok);}
{
    // 整數提示：(Set 名稱, 上限, 欄位)；Get 版本寫回同一欄位。
    struct Hint{std::string_view name;std::uint32_t maximum;unsigned GdiplusState::Graphics::* field;};
    static constexpr Hint hints[]{{"PageUnit",6,&GdiplusState::Graphics::page_unit},{"CompositingMode",1,&GdiplusState::Graphics::compositing},
        {"CompositingQuality",4,&GdiplusState::Graphics::quality},{"InterpolationMode",7,&GdiplusState::Graphics::interpolation},
        {"PixelOffsetMode",4,&GdiplusState::Graphics::pixel_offset},{"TextRenderingHint",5,&GdiplusState::Graphics::text_hint},{"TextContrast",12,&GdiplusState::Graphics::text_contrast}};
    for(const auto& hint:hints){
        if(symbol.size()>7 && symbol.substr(7)==hint.name && (symbol.starts_with("GdipSet") || symbol.starts_with("GdipGet"))){
            auto& info=graphics_of(arg(0));
            if(symbol.starts_with("GdipGet")){if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,info.*hint.field);return finish(ok);}
            if(u32(1)>hint.maximum)return finish(invalid_parameter);info.*hint.field=u32(1);return finish(ok);
        }
    }
}
if(symbol=="GdipSaveGraphics"){
    const auto& info=graphics_of(arg(0));if(!arg(1))return finish(invalid_parameter);const auto id=g.next_save++;
    g.saved_graphics[arg(0)].emplace_back(id,info);memory.store(GuestAddress{arg(1)},32,id);return finish(ok);
}
if(symbol=="GdipRestoreGraphics"){
    // 還原指定的保存點，並捨棄其後所有保存點；找不到時不變更。
    auto& info=graphics_of(arg(0));auto& stack=g.saved_graphics[arg(0)];
    const auto found=std::find_if(stack.begin(),stack.end(),[&](const auto& item){return item.first==u32(1);});
    if(found!=stack.end()){const auto owned=info.owned_dc;const auto window=info.window;info=found->second;info.owned_dc=owned;info.window=window;stack.erase(found,stack.end());}
    return finish(ok);
}
if(symbol=="GdipTransformPoints" || symbol=="GdipTransformPointsI"){
    // CoordinateSpace：World 0、Page 1、Device 2。
    const auto destination=u32(1),source=u32(2);if(destination>2 || source>2 || s32(4)<=0)return finish(invalid_parameter);
    const auto found=g.graphics.find(arg(0));if(found==g.graphics.end() || !gui_.contexts.contains(found->second))return finish(invalid_parameter);
    const auto& info=g.graphics_state[arg(0)];const auto factor=page_factor(info);const auto page=gui_multiply(GuiTransform{factor,0,0,factor,0,0},gui_device_transform(gui_.contexts.at(found->second)));
    const auto to_device=[&](std::uint32_t space){return space==0?gui_multiply(info.world,page):space==1?page:GuiTransform{1,0,0,1,0,0};};
    const auto inverse=gui_inverse(to_device(destination));if(!inverse)return finish(invalid_parameter);const auto m=gui_multiply(to_device(source),*inverse);
    const bool integer=symbol.ends_with('I');auto points=point_array(arg(3),s32(4),integer);for(auto& p:points){const auto mapped=gui_point(m,p[0],p[1]);p={mapped[0],mapped[1]};}
    store_points(arg(3),points,integer);return finish(ok);
}
if(symbol=="GdipGraphicsClear"){
    // 以來源複製填滿整個表面（依裁切）：顏色含 alpha 直接取代原像素。
    graphics_of(arg(0));const auto surface=raster_surface(memory,g.graphics.at(arg(0)));if(!surface)return finish(generic_error);
    auto dc=gui_.contexts.at(g.graphics.at(arg(0)));dc.world=gui_identity;dc.viewport_extent=dc.window_extent={1,1};dc.viewport_origin=dc.window_origin={0,0};
    const auto argb=premultiply(u32(1));const auto pixel=(argb&0xff00ff00U)|((argb&255U)<<16)|((argb>>16)&255U);
    for(const auto& rect:visible_region(arg(0))){
        const auto width=static_cast<unsigned>(std::max(0,rect[2]-rect[0])),height=static_cast<unsigned>(std::max(0,rect[3]-rect[1]));if(!width || !height)continue;
        if(std::uint64_t{width}*height>stream_limit/4)return finish(out_of_memory);
        const auto source=make_hardware_raster(width,height);source->upload(std::vector<std::uint32_t>(static_cast<std::size_t>(width)*height,pixel),true);
        surface->blit(*source,rect,{0,0,static_cast<int>(width),static_cast<int>(height)},dc,UINT32_MAX);
    }
    return finish(ok);
}
// ---- 裁切 ----
if(symbol=="GdipSetClipRect" || symbol=="GdipSetClipRectI" || symbol=="GdipSetClipPath" || symbol=="GdipSetClipGraphics" || symbol=="GdipResetClip"){
    auto& info=graphics_of(arg(0));
    if(symbol=="GdipResetClip"){info.clip.reset();return finish(ok);}
    if(symbol=="GdipSetClipGraphics"){const auto& source=graphics_of(arg(1));combine_clip(info.clip,source.clip,u32(2));return finish(ok);}
    GdiplusState::Path path;std::uint32_t mode=0;
    if(symbol=="GdipSetClipPath"){path=path_of(arg(1));mode=u32(2);}
    else {const bool integer=symbol.ends_with('I');add_rectangle(path,coordinate(1,integer),coordinate(2,integer),coordinate(3,integer),coordinate(4,integer));mode=u32(5);}
    combine_clip(info.clip,device_region(arg(0),path),mode);return finish(ok);
}
if(symbol=="GdipTranslateClip" || symbol=="GdipTranslateClipI"){
    auto& info=graphics_of(arg(0));if(!info.clip)return finish(ok);const bool integer=symbol.ends_with('I');const auto m=device_of(arg(0));
    const auto origin=gui_point(m,0,0),moved=gui_point(m,coordinate(1,integer),coordinate(2,integer));
    const auto dx=static_cast<std::int32_t>(std::lround(moved[0]-origin[0])),dy=static_cast<std::int32_t>(std::lround(moved[1]-origin[1]));
    for(auto& rect:*info.clip){rect[0]+=dx;rect[2]+=dx;rect[1]+=dy;rect[3]+=dy;}return finish(ok);
}
if(symbol=="GdipGetClipBounds" || symbol=="GdipGetClipBoundsI" || symbol=="GdipGetVisibleClipBounds" || symbol=="GdipGetVisibleClipBoundsI"){
    // 無裁切時為 GDI+ 的無限大矩形；其他情形把裝置座標的外接矩形換回世界座標。
    const auto& info=graphics_of(arg(0));if(!arg(1))return finish(invalid_parameter);const bool integer=symbol.ends_with('I'),visible=symbol.find("Visible")!=symbol.npos;
    if(!visible && !info.clip){store_rect(arg(1),-4194304,-4194304,8388608,8388608,integer);return finish(ok);}
    const auto region=visible?visible_region(arg(0)):*info.clip;if(region.empty()){store_rect(arg(1),0,0,0,0,integer);return finish(ok);}
    const auto bounds=world_bounds(arg(0),gui_region_bounds(region));store_rect(arg(1),bounds[0],bounds[1],bounds[2],bounds[3],integer);return finish(ok);
}
if(symbol=="GdipIsClipEmpty" || symbol=="GdipIsVisibleClipEmpty"){
    const auto& info=graphics_of(arg(0));if(!arg(1))return finish(invalid_parameter);
    const bool empty=symbol=="GdipIsClipEmpty"?(info.clip && info.clip->empty()):visible_region(arg(0)).empty();memory.store(GuestAddress{arg(1)},32,empty?1U:0U);return finish(ok);
}
if(symbol=="GdipIsVisiblePoint" || symbol=="GdipIsVisiblePointI" || symbol=="GdipIsVisibleRect" || symbol=="GdipIsVisibleRectI"){
    graphics_of(arg(0));const bool integer=symbol.ends_with('I'),rectangle=symbol.find("Rect")!=symbol.npos;const unsigned out=rectangle?5U:3U;if(!arg(out))return finish(invalid_parameter);
    const auto region=visible_region(arg(0));bool visible=false;
    if(rectangle){GdiplusState::Path path;add_rectangle(path,coordinate(1,integer),coordinate(2,integer),coordinate(3,integer),coordinate(4,integer));visible=!gui_region_combine(device_region(arg(0),path),region,1).empty();}
    else {const auto p=gui_point(device_of(arg(0)),coordinate(1,integer),coordinate(2,integer));const auto x=static_cast<std::int32_t>(std::floor(p[0])),y=static_cast<std::int32_t>(std::floor(p[1]));
        visible=std::any_of(region.begin(),region.end(),[&](const GuiRect& r){return x>=r[0] && x<r[2] && y>=r[1] && y<r[3];});}
    memory.store(GuestAddress{arg(out)},32,visible?1U:0U);return finish(ok);
}
// ---- GpPath ----
if(symbol=="GdipCreatePath2" || symbol=="GdipCreatePath2I"){
    // (points, types, count, fillMode, path**)
    if(s32(2)<0 || u32(3)>1 || !arg(1))return finish(invalid_parameter);const auto points=point_array(arg(0),s32(2),symbol.ends_with('I'));
    GdiplusState::Path path;path.fill=u32(3);
    for(std::size_t i=0;i<points.size();++i)path.points.push_back({points[i][0],points[i][1],private_type(static_cast<std::uint8_t>(memory.load(GuestAddress{arg(1)+i},8)))});
    path.new_figure=path.points.empty() || (path.points.back().type&1);
    const auto handle=new_object(arg(4));if(!handle)return finish(out_of_memory);g.paths.emplace(handle,std::move(path));memory.store(GuestAddress{arg(4)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipClonePath"){const auto copy=path_of(arg(0));const auto handle=new_object(arg(1));if(!handle)return finish(out_of_memory);g.paths.emplace(handle,copy);memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);}
if(symbol=="GdipResetPath"){auto& path=path_of(arg(0));path.points.clear();path.fill=0;path.new_figure=true;return finish(ok);}
if(symbol=="GdipStartPathFigure"){path_of(arg(0)).new_figure=true;return finish(ok);}
if(symbol=="GdipClosePathFigures"){
    auto& path=path_of(arg(0));for(std::size_t i=0;i<path.points.size();++i)if(i+1==path.points.size() || (path.points[i+1].type&6)==6)path.points[i].type|=1;path.new_figure=true;return finish(ok);
}
if(symbol=="GdipGetPointCount"){const auto& path=path_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,path.points.size());return finish(ok);}
if(symbol=="GdipGetPathFillMode"){const auto& path=path_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,path.fill);return finish(ok);}
if(symbol=="GdipSetPathFillMode"){auto& path=path_of(arg(0));if(u32(1)>1)return finish(invalid_parameter);path.fill=u32(1);return finish(ok);}
if(symbol=="GdipGetPathPoints" || symbol=="GdipGetPathPointsI" || symbol=="GdipGetPathTypes"){
    const auto& path=path_of(arg(0));if(!arg(1) || s32(2)<=0)return finish(invalid_parameter);if(static_cast<std::size_t>(s32(2))<path.points.size())return finish(insufficient_buffer);
    if(symbol=="GdipGetPathTypes"){memory.check(GuestAddress{arg(1)},path.points.size(),Access::Write);for(std::size_t i=0;i<path.points.size();++i)memory.store(GuestAddress{arg(1)+i},8,public_type(path.points[i].type));}
    else {std::vector<std::array<double,2>> points;for(const auto& p:path.points)points.push_back({p.x,p.y});store_points(arg(1),points,symbol.ends_with('I'));}
    return finish(ok);
}
if(symbol=="GdipGetPathLastPoint"){const auto& path=path_of(arg(0));if(!arg(1) || path.points.empty())return finish(invalid_parameter);store_points(arg(1),{{path.points.back().x,path.points.back().y}},false);return finish(ok);}
if(symbol=="GdipTransformPath"){auto& path=path_of(arg(0));if(arg(1))transform_path(path,matrix_of(arg(1)));return finish(ok);}
if(symbol=="GdipFlattenPath"){auto& path=path_of(arg(0));if(arg(1))transform_path(path,matrix_of(arg(1)));path.points=gui_flatten_path(path.points);return finish(ok);}
if(symbol=="GdipReversePath"){
    // 反轉點序與 figure 順序：每段的型別跟著原本結束該段的點移動，起點與關閉標記留在新 figure 的頭尾。
    auto& path=path_of(arg(0));std::vector<GuiPathPoint> reversed;
    std::vector<std::pair<std::size_t,std::size_t>> figures;for(std::size_t i=0;i<path.points.size();++i){if((path.points[i].type&6)==6 || figures.empty())figures.push_back({i,i});figures.back().second=i;}
    for(auto it=figures.rbegin();it!=figures.rend();++it){const auto [begin,end]=*it;const bool closed=path.points[end].type&1;
        for(auto i=end+1;i-->begin;){std::uint8_t type=i==end?6:static_cast<std::uint8_t>(path.points[i+1].type&6);reversed.push_back({path.points[i].x,path.points[i].y,type});}
        if(closed)reversed.back().type|=1;}
    path.points=std::move(reversed);return finish(ok);
}
if(symbol=="GdipAddPathPath"){
    auto& path=path_of(arg(0));const auto adding=path_of(arg(1));if(adding.points.empty())return finish(ok);auto points=adding.points;
    if(arg(2) && !path.new_figure && !path.points.empty())points.front().type=static_cast<std::uint8_t>((points.front().type&~6U)|2U);
    path.points.insert(path.points.end(),points.begin(),points.end());path.new_figure=adding.new_figure;return finish(ok);
}
if(symbol=="GdipGetPathWorldBounds" || symbol=="GdipGetPathWorldBoundsI"){
    // (path, bounds, matrix, pen)：Bezier 先攤平；有畫筆時每邊外擴半個筆寬。
    auto path=path_of(arg(0));if(!arg(1))return finish(invalid_parameter);if(arg(2))transform_path(path,matrix_of(arg(2)));
    const bool integer=symbol.ends_with('I');const auto flat=gui_flatten_path(path.points);if(flat.empty()){store_rect(arg(1),0,0,0,0,integer);return finish(ok);}
    double l=INFINITY,t=INFINITY,r=-INFINITY,b=-INFINITY;for(const auto& p:flat){l=std::min(l,p.x);t=std::min(t,p.y);r=std::max(r,p.x);b=std::max(b,p.y);}
    if(arg(3)){const auto half=pen_of(arg(3)).width/2;l-=half;t-=half;r+=half;b+=half;}
    store_rect(arg(1),l,t,r-l,b-t,integer);return finish(ok);
}
if(symbol=="GdipIsVisiblePathPoint" || symbol=="GdipIsVisiblePathPointI"){
    const auto& path=path_of(arg(0));if(!arg(4))return finish(invalid_parameter);const bool integer=symbol.ends_with('I');
    const auto x=coordinate(1,integer),y=coordinate(2,integer);const auto region=native_path_region(gui_flatten_path(path.points),path.fill+1);
    const auto px=static_cast<std::int32_t>(std::floor(x)),py=static_cast<std::int32_t>(std::floor(y));
    memory.store(GuestAddress{arg(4)},32,std::any_of(region.begin(),region.end(),[&](const GuiRect& r){return px>=r[0] && px<r[2] && py>=r[1] && py<r[3];})?1U:0U);return finish(ok);
}
if(symbol.starts_with("GdipAddPath") && !symbol.starts_with("GdipAddPathString")){
    auto& path=path_of(arg(0));auto base=symbol.substr(11);const bool integer=base.ends_with('I') && base!="Pie";if(integer)base.remove_suffix(1);
    if(base=="Line")add_line(path,coordinate(1,integer),coordinate(2,integer),coordinate(3,integer),coordinate(4,integer));
    else if(base=="Line2"){if(s32(2)<1)return finish(invalid_parameter);add_lines(path,point_array(arg(1),s32(2),integer));}
    else if(base=="Arc")add_arc(path,coordinate(1,integer),coordinate(2,integer),coordinate(3,integer),coordinate(4,integer),real(5),real(6));
    else if(base=="Bezier")add_beziers(path,{{coordinate(1,integer),coordinate(2,integer)},{coordinate(3,integer),coordinate(4,integer)},{coordinate(5,integer),coordinate(6,integer)},{coordinate(7,integer),coordinate(8,integer)}});
    else if(base=="Beziers")add_beziers(path,point_array(arg(1),s32(2),integer));
    else if(base=="Rectangle")add_rectangle(path,coordinate(1,integer),coordinate(2,integer),coordinate(3,integer),coordinate(4,integer));
    else if(base=="Rectangles"){for(const auto& r:rect_array(arg(1),s32(2),integer))add_rectangle(path,r[0],r[1],r[2],r[3]);}
    else if(base=="Ellipse")add_ellipse(path,coordinate(1,integer),coordinate(2,integer),coordinate(3,integer),coordinate(4,integer));
    else if(base=="Polygon"){const auto points=point_array(arg(1),s32(2),integer);if(points.size()<3)return finish(invalid_parameter);path.new_figure=true;add_lines(path,points);close_figure(path);}
    else if(base=="Pie")add_pie(path,coordinate(1,integer),coordinate(2,integer),coordinate(3,integer),coordinate(4,integer),real(5),real(6));
    else if(base=="Curve" || base=="Curve2" || base=="Curve3" || base=="ClosedCurve" || base=="ClosedCurve2"){
        const auto points=point_array(arg(1),s32(2),integer);const bool closed=base.starts_with("Closed");
        const double tension=base=="Curve2" || base=="ClosedCurve2"?real(3):base=="Curve3"?real(5):0.5;
        const std::size_t first=base=="Curve3"?static_cast<std::size_t>(std::max(0,s32(3))):0,segments=base=="Curve3"?static_cast<std::size_t>(std::max(0,s32(4))):closed?points.size():points.size()-1;
        if(first+segments>(closed?points.size():points.size()-1) || !segments)return finish(invalid_parameter);
        if(closed)path.new_figure=true;add_beziers(path,cardinal(points,tension,closed,first,segments));if(closed)close_figure(path);
    }
    else return finish(not_implemented);
    return finish(ok);
}
// ---- Draw*／Fill* 變體 ----
{
    const bool draw=symbol.starts_with("GdipDraw"),fill=symbol.starts_with("GdipFill");
    if(draw || fill){
        auto base=symbol.substr(8);const bool integer=base.ends_with('I') && base!="Pie";if(integer)base.remove_suffix(1);
        static constexpr std::string_view bases[]{"Lines","Rectangles","Polygon","Polygon2","Arc","Pie","Bezier","Beziers","Curve","Curve2","Curve3","ClosedCurve","ClosedCurve2"};
        if(std::find(std::begin(bases),std::end(bases),base)!=std::end(bases) && !(draw && base=="Polygon2") && !(fill && (base=="Lines" || base=="Arc" || base.starts_with("Bezier") || (base.starts_with("Curve"))))){
            GdiplusState::Path path;
            if(base=="Lines"){const auto points=point_array(arg(2),s32(3),integer);if(points.size()<2)return finish(invalid_parameter);path.new_figure=true;add_lines(path,points);}
            else if(base=="Rectangles"){if(s32(3)<1)return finish(invalid_parameter);for(const auto& r:rect_array(arg(2),s32(3),integer))add_rectangle(path,r[0],r[1],r[2],r[3]);}
            else if(base=="Polygon" || base=="Polygon2"){
                const auto points=point_array(arg(2),s32(3),integer);if(points.size()<(fill?3U:2U))return finish(invalid_parameter);
                if(fill && base=="Polygon"){if(u32(4)>1)return finish(invalid_parameter);path.fill=u32(4);}
                path.new_figure=true;add_lines(path,points);close_figure(path);
            }
            else if(base=="Arc"){const auto w=coordinate(4,integer),h=coordinate(5,integer);if(w<=0 || h<=0)return finish(invalid_parameter);path.new_figure=true;add_arc(path,coordinate(2,integer),coordinate(3,integer),w,h,real(6),real(7));}
            else if(base=="Pie"){const auto w=coordinate(4,integer),h=coordinate(5,integer);if(w<=0 || h<=0)return finish(invalid_parameter);add_pie(path,coordinate(2,integer),coordinate(3,integer),w,h,real(6),real(7));}
            else if(base=="Bezier"){path.new_figure=true;add_beziers(path,{{coordinate(2,integer),coordinate(3,integer)},{coordinate(4,integer),coordinate(5,integer)},{coordinate(6,integer),coordinate(7,integer)},{coordinate(8,integer),coordinate(9,integer)}});}
            else if(base=="Beziers"){path.new_figure=true;add_beziers(path,point_array(arg(2),s32(3),integer));}
            else {
                const auto points=point_array(arg(2),s32(3),integer);const bool closed=base.starts_with("Closed");
                const double tension=base=="Curve2" || base=="ClosedCurve2"?real(4):base=="Curve3"?real(6):0.5;
                if(points.size()<(closed?3U:2U))return finish(invalid_parameter);
                const std::size_t first=base=="Curve3"?static_cast<std::size_t>(std::max(0,s32(4))):0,segments=base=="Curve3"?static_cast<std::size_t>(std::max(0,s32(5))):closed?points.size():points.size()-1;
                if(first+segments>(closed?points.size():points.size()-1) || !segments)return finish(invalid_parameter);
                if(fill && base=="ClosedCurve2"){if(u32(5)>1)return finish(invalid_parameter);path.fill=u32(5);}
                path.new_figure=true;add_beziers(path,cardinal(points,tension,closed,first,segments));if(closed)close_figure(path);
            }
            return finish(render(arg(0),fill,arg(1),std::move(path)));
        }
    }
}
// ---- 畫筆與筆刷 ----
// 畫筆的填滿：純色筆刷只取顏色，其他筆刷保存複本供描邊著色。
const auto set_pen_fill=[](GdiplusState::Pen& pen,const GdiplusBrush& brush){
    if(brush.type==0){pen.color=brush.color;pen.fill.reset();}else {pen.color=0;pen.fill=std::make_shared<const GdiplusBrush>(brush);}};
if(symbol=="GdipCreatePen2"){
    // (brush, width, unit, pen**)
    const auto brush=g.brushes.find(arg(0));const auto width=real(1);if(brush==g.brushes.end() || width<0 || u32(2)>6)return finish(invalid_parameter);
    const auto handle=new_object(arg(3));if(!handle)return finish(out_of_memory);GdiplusState::Pen pen;set_pen_fill(pen,brush->second);pen.width=width;pen.unit=u32(2);
    g.pens[handle]=pen;memory.store(GuestAddress{arg(3)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipClonePen"){const auto copy=pen_of(arg(0));const auto handle=new_object(arg(1));if(!handle)return finish(out_of_memory);g.pens[handle]=copy;memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);}
if(symbol=="GdipSetPenWidth"){const auto width=real(1);if(width<0)return finish(invalid_parameter);pen_of(arg(0)).width=width;return finish(ok);}
if(symbol=="GdipGetPenWidth" || symbol=="GdipGetPenMiterLimit"){const auto& pen=pen_of(arg(0));if(!arg(1))return finish(invalid_parameter);store_real(arg(1),symbol=="GdipGetPenWidth"?pen.width:pen.miter);return finish(ok);}
if(symbol=="GdipSetPenMiterLimit"){pen_of(arg(0)).miter=std::max(1.0,real(1));return finish(ok);}
if(symbol=="GdipSetPenColor"){auto& pen=pen_of(arg(0));pen.color=u32(1);pen.fill.reset();return finish(ok);}
if(symbol=="GdipSetPenUnit"){if(u32(1)>6 || u32(1)==1)return finish(invalid_parameter);pen_of(arg(0)).unit=u32(1);return finish(ok);}
if(symbol=="GdipSetPenDashStyle"){if(u32(1)>5)return finish(invalid_parameter);pen_of(arg(0)).dash=u32(1);return finish(ok);}
if(symbol=="GdipSetPenDashCap197819"){if(u32(1)!=0 && u32(1)!=2 && u32(1)!=3)return finish(invalid_parameter);pen_of(arg(0)).dash_cap=u32(1);return finish(ok);}
if(symbol=="GdipSetPenLineCap197819"){auto& pen=pen_of(arg(0));const auto valid=[](std::uint32_t c){return c<=3 || (c>=0x10 && c<=0x14);};if(!valid(u32(1)) || !valid(u32(2)) || (u32(3)!=0 && u32(3)!=2 && u32(3)!=3))return finish(invalid_parameter);pen.start_cap=u32(1);pen.end_cap=u32(2);pen.dash_cap=u32(3);return finish(ok);}
if(symbol=="GdipGetPenColor" || symbol=="GdipGetPenUnit" || symbol=="GdipGetPenStartCap" || symbol=="GdipGetPenEndCap" || symbol=="GdipGetPenLineJoin" ||
   symbol=="GdipGetPenDashStyle" || symbol=="GdipGetPenDashCap197819" || symbol=="GdipGetPenFillType"){
    const auto& pen=pen_of(arg(0));if(!arg(1) || (symbol=="GdipGetPenColor" && pen.fill))return finish(invalid_parameter);
    const std::uint32_t value=symbol=="GdipGetPenFillType"?(pen.fill?pen.fill->type:0U):symbol=="GdipGetPenColor"?pen.color:symbol=="GdipGetPenUnit"?pen.unit:symbol=="GdipGetPenStartCap"?pen.start_cap:symbol=="GdipGetPenEndCap"?pen.end_cap:
        symbol=="GdipGetPenLineJoin"?pen.join:symbol=="GdipGetPenDashStyle"?pen.dash:pen.dash_cap;
    memory.store(GuestAddress{arg(1)},32,value);return finish(ok);
}
if(symbol=="GdipGetPenBrushFill"){
    const auto& pen=pen_of(arg(0));GdiplusBrush brush;if(pen.fill)brush=*pen.fill;else brush.color=pen.color;
    const auto handle=new_object(arg(1));if(!handle)return finish(out_of_memory);g.brushes[handle]=std::move(brush);memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipSetPenBrushFill"){const auto brush=g.brushes.find(arg(1));if(brush==g.brushes.end())return finish(invalid_parameter);set_pen_fill(pen_of(arg(0)),brush->second);return finish(ok);}
if(symbol=="GdipGetBrushType" || symbol=="GdipGetSolidFillColor"){
    // GetSolidFillColor 只適用純色筆刷。
    const auto brush=g.brushes.find(arg(0));if(brush==g.brushes.end() || !arg(1) || (symbol=="GdipGetSolidFillColor" && brush->second.type))return finish(invalid_parameter);
    memory.store(GuestAddress{arg(1)},32,symbol=="GdipGetBrushType"?brush->second.type:brush->second.color);return finish(ok);
}
if(symbol=="GdipSetSolidFillColor"){const auto brush=g.brushes.find(arg(0));if(brush==g.brushes.end() || brush->second.type)return finish(invalid_parameter);brush->second.color=u32(1);return finish(ok);}
