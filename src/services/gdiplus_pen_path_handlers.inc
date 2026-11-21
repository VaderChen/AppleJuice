// GDI+ 畫筆進階屬性、自訂與箭頭線端、路徑迭代器、路徑延伸操作與 Graphics 容器。描邊實際效果在 gdiplus_vector_handlers.inc 的 render。
const auto read_reals=[&](std::uint64_t at,std::int64_t count){
    if(!at || count<0 || count>65536)throw std::invalid_argument("GDI+ 數值陣列無效");std::vector<double> out(static_cast<std::size_t>(count));
    for(std::size_t i=0;i<out.size();++i){const auto v=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{at+i*4},32)));if(!std::isfinite(v))throw std::invalid_argument("GDI+ 數值不是有限數");out[i]=v;}return out;};
const auto cap_of=[&](std::uint64_t handle)->GdiplusState::CustomCap&{const auto found=g.caps.find(handle);if(found==g.caps.end())throw std::invalid_argument("GDI+ 線端無效");return found->second;};
// 箭頭線端：尖端在原點、底邊在 −height，middle inset 讓底邊中點內凹；不填滿時以折線描邊。
const auto rebuild_arrow=[&](GdiplusState::CustomCap& cap){
    const auto w=cap.arrow_width/2,h=cap.arrow_height;cap.fill={};cap.stroke={};
    if(cap.filled){cap.fill.points={{0,0,6},{w,-h,2},{0,-h+cap.middle,2},{-w,-h,2}};cap.fill.points.back().type|=1;}
    else cap.stroke.points={{-w,-h,6},{0,0,2},{w,-h,2}};};
const auto new_cap=[&](GdiplusState::CustomCap cap,std::uint64_t output)->std::uint32_t{const auto handle=new_object(output);if(!handle)return out_of_memory;g.caps[handle]=std::move(cap);memory.store(GuestAddress{output},pointer_bits,handle);return ok;};
// ---- 畫筆 ----
if(symbol=="GdipSetPenDashArray"){
    auto& pen=pen_of(arg(0));if(s32(2)<=0)return finish(invalid_parameter);auto values=read_reals(arg(1),s32(2));
    if(std::any_of(values.begin(),values.end(),[](double v){return !(v>0);}))return finish(invalid_parameter);pen.dashes=std::move(values);pen.dash=5;return finish(ok);}
if(symbol=="GdipGetPenDashCount" || symbol=="GdipGetPenDashArray"){
    // 標準虛線樣式回傳其圖樣（筆寬倍數）。
    const auto& pen=pen_of(arg(0));const auto pattern=dash_pattern(pen,1);
    if(symbol=="GdipGetPenDashCount"){if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,pattern.size());return finish(ok);}
    if(!arg(1) || s32(2)<static_cast<std::int32_t>(pattern.size()) || pattern.empty())return finish(invalid_parameter);for(std::size_t i=0;i<pattern.size();++i)store_real(arg(1)+i*4,pattern[i]);return finish(ok);}
if(symbol=="GdipGetPenDashOffset"){const auto& pen=pen_of(arg(0));if(!arg(1))return finish(invalid_parameter);store_real(arg(1),pen.dash_offset);return finish(ok);}
if(symbol=="GdipSetPenDashOffset"){pen_of(arg(0)).dash_offset=real(1);return finish(ok);}
if(symbol=="GdipSetPenCompoundArray"){
    // 偶數個、由小到大且落在 0～1 的位置。
    auto& pen=pen_of(arg(0));const auto count=s32(2);if(count<2 || count%2)return finish(invalid_parameter);auto values=read_reals(arg(1),count);
    for(std::size_t i=0;i<values.size();++i)if(values[i]<0 || values[i]>1 || (i && values[i]<values[i-1]))return finish(invalid_parameter);pen.compound=std::move(values);return finish(ok);}
if(symbol=="GdipGetPenCompoundCount"){const auto& pen=pen_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,pen.compound.size());return finish(ok);}
if(symbol=="GdipGetPenCompoundArray"){
    const auto& pen=pen_of(arg(0));if(!arg(1) || s32(2)<static_cast<std::int32_t>(pen.compound.size()))return finish(invalid_parameter);
    for(std::size_t i=0;i<pen.compound.size();++i)store_real(arg(1)+i*4,pen.compound[i]);return finish(ok);}
if(symbol=="GdipSetPenTransform"){const auto& m=matrix_of(arg(1));if(!gui_inverse(m))return finish(invalid_parameter);pen_of(arg(0)).transform=m;return finish(ok);}
if(symbol=="GdipGetPenTransform"){matrix_of(arg(1))=pen_of(arg(0)).transform;return finish(ok);}
if(symbol=="GdipResetPenTransform"){pen_of(arg(0)).transform=gui_identity;return finish(ok);}
if(symbol=="GdipMultiplyPenTransform"){combine_matrix(pen_of(arg(0)).transform,matrix_of(arg(1)),u32(2));return finish(ok);}
if(symbol=="GdipTranslatePenTransform"){combine_matrix(pen_of(arg(0)).transform,{1,0,0,1,real(1),real(2)},u32(3));return finish(ok);}
if(symbol=="GdipScalePenTransform"){combine_matrix(pen_of(arg(0)).transform,{real(1),0,0,real(2),0,0},u32(3));return finish(ok);}
if(symbol=="GdipRotatePenTransform"){combine_matrix(pen_of(arg(0)).transform,rotation(real(1)),u32(2));return finish(ok);}
if(symbol=="GdipGetPenMode"){const auto& pen=pen_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,pen.alignment);return finish(ok);}
if(symbol=="GdipSetPenMode"){if(u32(1)>1)return finish(invalid_parameter);pen_of(arg(0)).alignment=u32(1);return finish(ok);}
if(symbol=="GdipSetPenCustomStartCap" || symbol=="GdipSetPenCustomEndCap"){
    auto& pen=pen_of(arg(0));const auto cap=std::make_shared<const GdiplusState::CustomCap>(cap_of(arg(1)));
    if(symbol=="GdipSetPenCustomStartCap"){pen.custom_start=cap;pen.start_cap=0xff;}else {pen.custom_end=cap;pen.end_cap=0xff;}return finish(ok);}
if(symbol=="GdipGetPenCustomStartCap" || symbol=="GdipGetPenCustomEndCap"){
    // 沒有自訂線端時回傳 NULL；否則回傳複本，由呼叫端刪除。
    const auto& pen=pen_of(arg(0));if(!arg(1))return finish(invalid_parameter);const auto& cap=symbol=="GdipGetPenCustomStartCap"?pen.custom_start:pen.custom_end;
    if(!cap){memory.store(GuestAddress{arg(1)},pointer_bits,0);return finish(ok);}return finish(new_cap(*cap,arg(1)));}
// ---- 自訂線端 ----
if(symbol=="GdipCreateCustomLineCap"){
    // (fillPath, strokePath, baseCap, baseInset, cap**)：兩條路徑至少一條；BaseCap 為 Flat、Square、Round 或 Triangle。
    if((!arg(0) && !arg(1)) || !arg(4) || u32(2)>3)return finish(invalid_parameter);GdiplusState::CustomCap cap;
    if(arg(0))cap.fill=path_of(arg(0));if(arg(1))cap.stroke=path_of(arg(1));cap.base_cap=u32(2);cap.inset=real(3);return finish(new_cap(std::move(cap),arg(4)));}
if(symbol=="GdipCreateAdjustableArrowCap"){
    // (height, width, isFilled, cap**)
    if(!arg(3))return finish(invalid_parameter);GdiplusState::CustomCap cap;cap.type=1;cap.arrow_height=real(0);cap.arrow_width=real(1);cap.filled=u32(2)!=0;cap.base_cap=3;rebuild_arrow(cap);
    return finish(new_cap(std::move(cap),arg(3)));}
if(symbol=="GdipCloneCustomLineCap"){const auto copy=cap_of(arg(0));if(!arg(1))return finish(invalid_parameter);return finish(new_cap(copy,arg(1)));}
if(symbol=="GdipDeleteCustomLineCap"){if(!g.caps.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);}
if(symbol=="GdipGetCustomLineCapType"){const auto& cap=cap_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,cap.type);return finish(ok);}
if(symbol=="GdipSetCustomLineCapBaseCap"){if(u32(1)>3)return finish(invalid_parameter);cap_of(arg(0)).base_cap=u32(1);return finish(ok);}
if(symbol=="GdipSetCustomLineCapStrokeJoin"){if(u32(1)>3)return finish(invalid_parameter);cap_of(arg(0)).join=u32(1);return finish(ok);}
if(symbol=="GdipGetCustomLineCapStrokeCaps"){
    const auto& cap=cap_of(arg(0));
    if(!arg(1) || !arg(2))return finish(invalid_parameter);
    memory.store(GuestAddress{arg(1)},32,cap.start_cap);
    memory.store(GuestAddress{arg(2)},32,cap.end_cap);
    return finish(ok);
}
if(symbol=="GdipSetCustomLineCapStrokeCaps"){auto& cap=cap_of(arg(0));if(u32(1)>3 || u32(2)>3)return finish(invalid_parameter);cap.start_cap=u32(1);cap.end_cap=u32(2);return finish(ok);}
if(symbol=="GdipGetCustomLineCapBaseCap" || symbol=="GdipGetCustomLineCapStrokeJoin"){
    const auto& cap=cap_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,symbol=="GdipGetCustomLineCapBaseCap"?cap.base_cap:cap.join);return finish(ok);}
if(symbol=="GdipSetCustomLineCapBaseInset"){cap_of(arg(0)).inset=real(1);return finish(ok);}
if(symbol=="GdipSetCustomLineCapWidthScale"){cap_of(arg(0)).scale=real(1);return finish(ok);}
if(symbol=="GdipGetCustomLineCapBaseInset" || symbol=="GdipGetCustomLineCapWidthScale"){
    const auto& cap=cap_of(arg(0));if(!arg(1))return finish(invalid_parameter);store_real(arg(1),symbol=="GdipGetCustomLineCapBaseInset"?cap.inset:cap.scale);return finish(ok);}
if(symbol.starts_with("GdipSetAdjustableArrowCap") || symbol.starts_with("GdipGetAdjustableArrowCap")){
    auto& cap=cap_of(arg(0));if(cap.type!=1)return finish(invalid_parameter);const auto field=symbol.substr(25);const bool set=symbol.starts_with("GdipSet");
    if(!set && !arg(1))return finish(invalid_parameter);
    if(field=="FillState"){if(set){cap.filled=u32(1)!=0;rebuild_arrow(cap);}else memory.store(GuestAddress{arg(1)},32,cap.filled?1U:0U);return finish(ok);}
    double* value=field=="Height"?&cap.arrow_height:field=="Width"?&cap.arrow_width:field=="MiddleInset"?&cap.middle:nullptr;if(!value)return finish(not_implemented);
    if(set){*value=real(1);rebuild_arrow(cap);}else store_real(arg(1),*value);return finish(ok);
}
// ---- 路徑標記與資料 ----
if(symbol=="GdipSetPathMarker"){auto& path=path_of(arg(0));if(!path.points.empty())path.points.back().type|=0x20U;return finish(ok);}
if(symbol=="GdipClearPathMarkers"){for(auto& p:path_of(arg(0)).points)p.type&=static_cast<std::uint8_t>(~0x20U);return finish(ok);}
if(symbol=="GdipGetPathData"){
    // PathData：Count、Points*、Types*（呼叫端依 GetPointCount 配置陣列）。
    const auto& path=path_of(arg(0));if(!arg(1))return finish(invalid_parameter);const unsigned points_at=x86_?4U:8U,types_at=x86_?8U:16U;
    const auto capacity=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)},32));if(capacity<static_cast<std::int32_t>(path.points.size()))return finish(insufficient_buffer);
    const auto points=memory.load(GuestAddress{arg(1)+points_at},pointer_bits),types=memory.load(GuestAddress{arg(1)+types_at},pointer_bits);if(!points || !types)return finish(invalid_parameter);
    std::vector<std::array<double,2>> xy;for(const auto& p:path.points)xy.push_back({p.x,p.y});store_points(points,xy,false);
    for(std::size_t i=0;i<path.points.size();++i)memory.store(GuestAddress{types+i},8,public_type(path.points[i].type));
    memory.store(GuestAddress{arg(1)},32,path.points.size());return finish(ok);
}
// ---- 路徑延伸操作 ----
// 畫筆的中心描邊輪廓（虛線、筆尖變換與接合；不含線端）。
const auto pen_outline=[&](const GdiplusState::Pen& pen,const std::vector<GuiPathPoint>& points){
    const auto width=pen_width(pen);const auto pattern=dash_pattern(pen,width);const auto join=pen.join==1?0x1000U:pen.join==2?0U:0x2000U;
    const auto centre=pattern.empty()?points:dash_path(gui_flatten_path(points),pattern,pen.dash_offset*width);return native_stroke_path(centre,pen.transform,width,0x200U|join,pen.miter);};
if(symbol=="GdipIsOutlineVisiblePathPoint" || symbol=="GdipIsOutlineVisiblePathPointI"){
    // (path, x, y, pen, graphics, result)：以世界座標的描邊區域判斷。
    const auto& path=path_of(arg(0));const auto& pen=pen_of(arg(3));if(!arg(5))return finish(invalid_parameter);const bool integer=symbol.ends_with('I');
    const auto region=native_path_region(pen_outline(pen,path.points),2);const auto px=std::floor(coordinate(1,integer)),py=std::floor(coordinate(2,integer));
    memory.store(GuestAddress{arg(5)},32,std::any_of(region.begin(),region.end(),[&](const GuiRect& r){return px>=r[0] && px<r[2] && py>=r[1] && py<r[3];})?1U:0U);return finish(ok);
}
if(symbol=="GdipWidenPath"){
    // (path, pen, matrix, flatness)：路徑換成描邊輪廓，填滿模式改為 Winding。
    auto& path=path_of(arg(0));const auto& pen=pen_of(arg(1));if(arg(2))transform_path(path,matrix_of(arg(2)));
    path.points=pen_outline(pen,path.points);path.fill=1;path.new_figure=true;return finish(ok);
}
if(symbol=="GdipWarpPath"){
    // (path, matrix, points, count, srcx, srcy, srcw, srch, warpMode, flatness)：來源矩形映到三點（平行四邊形）或四點（Perspective 0 投影、Bilinear 1 雙線性）。
    auto& path=path_of(arg(0));const auto count=s32(3);if(!arg(2) || (count!=3 && count!=4) || u32(8)>1)return finish(invalid_parameter);
    const auto sx=real(4),sy=real(5),sw=real(6),sh=real(7);if(sw==0 || sh==0)return finish(invalid_parameter);auto q=point_array(arg(2),count,false);
    if(count==3)q.push_back({q[1][0]+q[2][0]-q[0][0],q[1][1]+q[2][1]-q[0][1]});
    if(arg(1))transform_path(path,matrix_of(arg(1)));auto flat=gui_flatten_path(path.points);
    // 單位正方形→四邊形的投影矩陣（Heckbert）：退化為平行四邊形時即仿射。
    const auto dx1=q[1][0]-q[3][0],dx2=q[2][0]-q[3][0],dy1=q[1][1]-q[3][1],dy2=q[2][1]-q[3][1],sx3=q[0][0]-q[1][0]+q[3][0]-q[2][0],sy3=q[0][1]-q[1][1]+q[3][1]-q[2][1];
    const auto den=dx1*dy2-dx2*dy1;const auto g3=den!=0?(sx3*dy2-dx2*sy3)/den:0.0,h3=den!=0?(dx1*sy3-sx3*dy1)/den:0.0;
    for(auto& p:flat){const auto u=(p.x-sx)/sw,v=(p.y-sy)/sh;
        if(u32(8)==1){p.x=(1-u)*(1-v)*q[0][0]+u*(1-v)*q[1][0]+(1-u)*v*q[2][0]+u*v*q[3][0];p.y=(1-u)*(1-v)*q[0][1]+u*(1-v)*q[1][1]+(1-u)*v*q[2][1]+u*v*q[3][1];continue;}
        const auto a=q[1][0]-q[0][0]+g3*q[1][0],b=q[2][0]-q[0][0]+h3*q[2][0],d=q[1][1]-q[0][1]+g3*q[1][1],e=q[2][1]-q[0][1]+h3*q[2][1];const auto w=g3*u+h3*v+1;
        p.x=(a*u+b*v+q[0][0])/w;p.y=(d*u+e*v+q[0][1])/w;}
    path.points=std::move(flat);return finish(ok);
}
if(symbol=="GdipWindingModeOutline"){
    // (path, matrix, flatness)：Winding 填滿區域的外框。以 1/8 單位精度的區域光柵近似，結果為互不重疊的矩形（交替填滿）。
    auto& path=path_of(arg(0));if(arg(1))transform_path(path,matrix_of(arg(1)));auto flat=gui_flatten_path(path.points);if(flat.empty())return finish(ok);
    double l=INFINITY,t=INFINITY,r=-INFINITY,b=-INFINITY;for(const auto& p:flat){l=std::min(l,p.x);t=std::min(t,p.y);r=std::max(r,p.x);b=std::max(b,p.y);}
    const auto scale=std::min(8.0,4096.0/std::max({r-l,b-t,1e-9}));for(auto& p:flat){p.x=(p.x-l)*scale;p.y=(p.y-t)*scale;}
    GdiplusState::Path outline;outline.fill=0;for(const auto& rect:native_path_region(flat,2))add_rectangle(outline,rect[0]/scale+l,rect[1]/scale+t,(rect[2]-rect[0])/scale,(rect[3]-rect[1])/scale);
    path.points=std::move(outline.points);path.fill=0;path.new_figure=true;return finish(ok);
}
// ---- 路徑迭代器 ----
const auto iterator_of=[&](std::uint64_t handle)->GdiplusState::PathIterator&{const auto found=g.iterators.find(handle);if(found==g.iterators.end())throw std::invalid_argument("GDI+ 路徑迭代器無效");return found->second;};
const auto figure_end=[](const std::vector<GuiPathPoint>& points,std::size_t begin){auto end=begin+1;while(end<points.size() && (points[end].type&6)!=6)++end;return end;};
const auto store_range=[&](const std::vector<GuiPathPoint>& points,std::size_t begin,std::size_t end,std::uint64_t xy,std::uint64_t types){
    std::vector<std::array<double,2>> values;for(auto i=begin;i<end;++i)values.push_back({points[i].x,points[i].y});if(xy)store_points(xy,values,false);
    if(types)for(auto i=begin;i<end;++i)memory.store(GuestAddress{types+(i-begin)},8,public_type(points[i].type));};
const auto copy_to_path=[&](std::uint64_t target,const std::vector<GuiPathPoint>& points,std::size_t begin,std::size_t end){
    auto& path=path_of(target);path.points.assign(points.begin()+static_cast<std::ptrdiff_t>(begin),points.begin()+static_cast<std::ptrdiff_t>(end));
    if(!path.points.empty())path.points.front().type=static_cast<std::uint8_t>((path.points.front().type&~6U)|6U);path.new_figure=true;};
if(symbol=="GdipCreatePathIter"){
    // (iterator**, path)：path 可為 NULL；保存建立時的路徑內容。
    if(!arg(0))return finish(invalid_parameter);GdiplusState::PathIterator iterator;if(arg(1))iterator.points=path_of(arg(1)).points;
    const auto handle=new_object(arg(0));if(!handle)return finish(out_of_memory);g.iterators[handle]=std::move(iterator);memory.store(GuestAddress{arg(0)},pointer_bits,handle);return finish(ok);}
if(symbol=="GdipDeletePathIter"){if(!g.iterators.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);}
if(symbol=="GdipPathIterRewind"){auto& it=iterator_of(arg(0));it.subpath=it.marker=it.type_begin=it.type_end=0;return finish(ok);}
if(symbol=="GdipPathIterGetCount" || symbol=="GdipPathIterGetSubpathCount" || symbol=="GdipPathIterHasCurve" || symbol=="GdipPathIterIsValid"){
    const auto& it=iterator_of(arg(0));if(!arg(1))return finish(invalid_parameter);std::uint32_t value=1;
    if(symbol=="GdipPathIterGetCount")value=static_cast<std::uint32_t>(it.points.size());
    else if(symbol=="GdipPathIterGetSubpathCount")value=static_cast<std::uint32_t>(std::count_if(it.points.begin(),it.points.end(),[](const auto& p){return (p.type&6)==6;}));
    else if(symbol=="GdipPathIterHasCurve")value=std::any_of(it.points.begin(),it.points.end(),[](const auto& p){return (p.type&6)==4;})?1U:0U;
    memory.store(GuestAddress{arg(1)},32,value);return finish(ok);}
if(symbol=="GdipPathIterNextSubpath" || symbol=="GdipPathIterNextSubpathPath"){
    // Subpath(iter, count*, start*, end*, closed*)；SubpathPath(iter, count*, path, closed*)。下一個 NextPathType 在此 figure 內進行。
    auto& it=iterator_of(arg(0));const bool to_path=symbol.ends_with("Path");if(!arg(1))return finish(invalid_parameter);
    if(it.subpath>=it.points.size()){memory.store(GuestAddress{arg(1)},32,0);if(!to_path){if(arg(2))memory.store(GuestAddress{arg(2)},32,0);if(arg(3))memory.store(GuestAddress{arg(3)},32,0);}return finish(ok);}
    const auto begin=it.subpath,end=figure_end(it.points,begin);it.subpath=end;it.type_begin=begin;it.type_end=end;
    const bool closed=(it.points[end-1].type&1)!=0;memory.store(GuestAddress{arg(1)},32,end-begin);
    if(to_path){copy_to_path(arg(2),it.points,begin,end);if(arg(3))memory.store(GuestAddress{arg(3)},32,closed?1U:0U);}
    else {if(arg(2))memory.store(GuestAddress{arg(2)},32,begin);if(arg(3))memory.store(GuestAddress{arg(3)},32,end-1);if(arg(4))memory.store(GuestAddress{arg(4)},32,closed?1U:0U);}
    return finish(ok);
}
if(symbol=="GdipPathIterNextPathType"){
    // (iter, count*, type*, start*, end*)：目前 figure 內同一種線段（直線 1 或 Bezier 3）的連續範圍，含前一段的終點。
    auto& it=iterator_of(arg(0));if(!arg(1) || !arg(2) || !arg(3) || !arg(4))return finish(invalid_parameter);
    if(it.type_begin+1>=it.type_end){memory.store(GuestAddress{arg(1)},32,0);return finish(ok);}
    const auto begin=it.type_begin;const auto kind=it.points[begin+1].type&6U;auto end=begin+1;while(end+1<it.type_end && (it.points[end+1].type&6U)==kind)++end;
    it.type_begin=end;memory.store(GuestAddress{arg(1)},32,end-begin+1);memory.store(GuestAddress{arg(2)},8,kind==4?3U:1U);
    memory.store(GuestAddress{arg(3)},32,begin);memory.store(GuestAddress{arg(4)},32,end);return finish(ok);
}
if(symbol=="GdipPathIterNextMarker" || symbol=="GdipPathIterNextMarkerPath"){
    // Marker(iter, count*, start*, end*)；MarkerPath(iter, count*, path)：以標記點（含）分段。
    auto& it=iterator_of(arg(0));const bool to_path=symbol.ends_with("Path");if(!arg(1))return finish(invalid_parameter);
    if(it.marker>=it.points.size()){memory.store(GuestAddress{arg(1)},32,0);return finish(ok);}
    const auto begin=it.marker;auto end=begin;while(end<it.points.size() && !(it.points[end].type&0x20U))++end;end=std::min(end+1,it.points.size());it.marker=end;
    memory.store(GuestAddress{arg(1)},32,end-begin);
    if(to_path)copy_to_path(arg(2),it.points,begin,end);else {if(arg(2))memory.store(GuestAddress{arg(2)},32,begin);if(arg(3))memory.store(GuestAddress{arg(3)},32,end-1);}
    return finish(ok);
}
if(symbol=="GdipPathIterEnumerate"){
    // (iter, count*, points, types, count)
    const auto& it=iterator_of(arg(0));if(!arg(1) || s32(4)<0)return finish(invalid_parameter);const auto n=std::min<std::size_t>(it.points.size(),static_cast<std::size_t>(s32(4)));
    store_range(it.points,0,n,arg(2),arg(3));memory.store(GuestAddress{arg(1)},32,n);return finish(ok);}
if(symbol=="GdipPathIterCopyData"){
    // (iter, count*, points, types, start, end)：範圍無效時回傳 0 個。
    const auto& it=iterator_of(arg(0));if(!arg(1))return finish(invalid_parameter);const auto start=s32(4),end=s32(5);
    if(start<0 || end<start || static_cast<std::size_t>(end)>=it.points.size()){memory.store(GuestAddress{arg(1)},32,0);return finish(ok);}
    store_range(it.points,static_cast<std::size_t>(start),static_cast<std::size_t>(end)+1,arg(2),arg(3));memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(end-start+1));return finish(ok);}
// ---- Graphics 容器（與 Save／RestoreGraphics 共用保存堆疊） ----
if(symbol=="GdipBeginContainer" || symbol=="GdipBeginContainerI" || symbol=="GdipBeginContainer2"){
    // Container(graphics, dst, src, unit, state*)：容器內座標由來源矩形（以 unit 計）映到目的矩形；Container2(graphics, state*) 只保存狀態。
    auto& info=graphics_of(arg(0));const bool simple=symbol=="GdipBeginContainer2";const unsigned output=simple?1U:4U;if(!arg(output))return finish(invalid_parameter);
    GuiTransform mapping=gui_identity;
    if(!simple){
        if(!arg(1) || !arg(2) || u32(3)>6 || u32(3)==1)return finish(invalid_parameter);const bool integer=symbol.ends_with('I');
        const auto dst=rect_array(arg(1),1,integer)[0];auto src=rect_array(arg(2),1,integer)[0];
        static constexpr double px[]{1,1,1,96.0/72,96,96.0/300,96.0/25.4};const auto factor=u32(3)==0?1.0:px[u32(3)]/page_factor(info);for(auto& v:src)v*=factor;
        if(src[2]==0 || src[3]==0)return finish(invalid_parameter);const auto kx=dst[2]/src[2],ky=dst[3]/src[3];mapping={kx,0,0,ky,dst[0]-src[0]*kx,dst[1]-src[1]*ky};
    }
    const auto id=g.next_save++;g.saved_graphics[arg(0)].emplace_back(id,info);if(!simple)combine_matrix(info.world,mapping,0);
    memory.store(GuestAddress{arg(output)},32,id);return finish(ok);
}
if(symbol=="GdipEndContainer"){
    auto& info=graphics_of(arg(0));auto& stack=g.saved_graphics[arg(0)];
    const auto found=std::find_if(stack.begin(),stack.end(),[&](const auto& item){return item.first==u32(1);});
    if(found!=stack.end()){const auto owned=info.owned_dc;const auto window=info.window;info=found->second;info.owned_dc=owned;info.window=window;stack.erase(found,stack.end());}
    return finish(ok);
}
if(symbol=="GdipResetPageTransform"){auto& info=graphics_of(arg(0));info.page_unit=1;info.page_scale=1;return finish(ok);}
