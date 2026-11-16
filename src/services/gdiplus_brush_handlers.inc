// GDI+ 筆刷家族：線性漸層、路徑漸層、陰影線與材質筆刷的建立與屬性；逐像素著色在 gdiplus_brush.hpp，
// 所有 Fill*／Draw*（含畫筆筆刷）與 GdipFillRegion 經 paint_region 共用。
const auto brush_of=[&](std::uint64_t handle,unsigned type)->GdiplusBrush&{
    const auto found=g.brushes.find(handle);if(found==g.brushes.end() || found->second.type!=type)throw std::invalid_argument("GDI+ 筆刷型別不符");return found->second;};
const auto add_brush=[&](GdiplusBrush brush,std::uint64_t output)->std::uint32_t{
    const auto handle=new_object(output);if(!handle)return out_of_memory;g.brushes[handle]=std::move(brush);memory.store(GuestAddress{output},pointer_bits,handle);return ok;};
// 以「前綴＋筆刷種類＋後綴」比對共用屬性（例如 GdipSetLineWrapMode、GdipRotateTextureTransform）；回傳筆刷型別或 -1。
const auto brush_kind=[&](std::string_view prefix,std::string_view suffix)->int{
    static constexpr std::pair<std::string_view,int> kinds[]{{"Line",4},{"PathGradient",3},{"Texture",2}};
    for(const auto& [name,type]:kinds)if(symbol.size()==prefix.size()+name.size()+suffix.size() && symbol.starts_with(prefix) && symbol.ends_with(suffix) && symbol.substr(prefix.size(),name.size())==name)return type;
    return -1;
};
const auto real_array=[&](std::uint64_t at,std::size_t count){
    std::vector<double> out(count);for(std::size_t i=0;i<count;++i){const auto value=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{at+i*4},32)));
        if(!std::isfinite(value))throw std::invalid_argument("GDI+ 數值不是有限數");out[i]=value;}return out;};
const auto color_array=[&](std::uint64_t at,std::size_t count){std::vector<std::uint32_t> out(count);for(std::size_t i=0;i<count;++i)out[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{at+i*4},32));return out;};
// ---- 線性漸層 ----
if(symbol=="GdipCreateLineBrush" || symbol=="GdipCreateLineBrushI"){
    // (point1, point2, color1, color2, wrap, brush**)；兩點相同依 Windows 回 OutOfMemory，線性漸層不接受 Clamp。
    const bool integer=symbol.ends_with('I');if(!arg(0) || !arg(1) || !arg(5) || u32(4)>=4)return finish(invalid_parameter);
    const auto a=point_array(arg(0),1,integer)[0],b=point_array(arg(1),1,integer)[0];if(a==b)return finish(out_of_memory);
    GdiplusBrush brush;brush.type=4;brush.colors={u32(2),u32(3)};brush.wrap=u32(4);brush.rect=gdiplus_line_rect(a,b);brush.start=a;brush.end=b;
    return finish(add_brush(std::move(brush),arg(5)));
}
if(symbol=="GdipCreateLineBrushFromRect" || symbol=="GdipCreateLineBrushFromRectI" || symbol=="GdipCreateLineBrushFromRectWithAngle" || symbol=="GdipCreateLineBrushFromRectWithAngleI"){
    // FromRect(rect, c1, c2, mode, wrap, out)：Horizontal 0°、Vertical 90°、ForwardDiagonal 45°、BackwardDiagonal 135°（對角線隨矩形伸縮）。
    // WithAngle(rect, c1, c2, angle, isAngleScalable, wrap, out)。
    const bool integer=symbol.ends_with('I'),angled=symbol.find("WithAngle")!=symbol.npos;const unsigned wrap_at=angled?5U:4U,output=angled?6U:5U;
    if(!arg(0) || !arg(output) || u32(wrap_at)>=4 || (!angled && u32(3)>3))return finish(invalid_parameter);
    GdiplusBrush brush;brush.type=4;brush.colors={u32(1),u32(2)};brush.wrap=u32(wrap_at);brush.rect=rect_array(arg(0),1,integer)[0];
    if(!(brush.rect[2]>0) || !(brush.rect[3]>0))return finish(out_of_memory);
    static constexpr double angles[]{0,90,45,135};
    if(angled)gdiplus_line_direction(brush,real(3),u32(4)!=0);else gdiplus_line_direction(brush,angles[u32(3)],u32(3)>=2);
    return finish(add_brush(std::move(brush),arg(output)));
}
if(symbol=="GdipSetLineColors"){auto& brush=brush_of(arg(0),4);brush.colors={u32(1),u32(2)};return finish(ok);}
if(symbol=="GdipGetLineColors"){const auto& brush=brush_of(arg(0),4);if(!arg(1))return finish(invalid_parameter);memory.check(GuestAddress{arg(1)},8,Access::Write);
    memory.store(GuestAddress{arg(1)},32,brush.colors[0]);memory.store(GuestAddress{arg(1)+4},32,brush.colors[1]);return finish(ok);}
if(symbol=="GdipGetLineRect" || symbol=="GdipGetLineRectI"){const auto& r=brush_of(arg(0),4).rect;if(!arg(1))return finish(invalid_parameter);store_rect(arg(1),r[0],r[1],r[2],r[3],symbol.ends_with('I'));return finish(ok);}
// ---- 路徑漸層 ----
if(symbol=="GdipCreatePathGradient" || symbol=="GdipCreatePathGradientI" || symbol=="GdipCreatePathGradientFromPath"){
    // 點陣列版本 (points, count, wrap, out)：中心色預設黑色；路徑版本 (path, out)：中心色白色、外圍模式 Clamp。中心點為各點平均。
    GdiplusState::Path path;unsigned output=3;GdiplusBrush brush;brush.type=3;
    if(symbol=="GdipCreatePathGradientFromPath"){output=1;path=path_of(arg(0));brush.wrap=4;brush.center_color=0xffffffffU;}
    else {
        if(!arg(0) || !arg(3) || u32(2)>4)return finish(invalid_parameter);if(s32(1)<2)return finish(out_of_memory);
        path.new_figure=true;add_lines(path,point_array(arg(0),s32(1),symbol.ends_with('I')));close_figure(path);brush.wrap=u32(2);brush.center_color=0xff000000U;
    }
    if(!arg(output))return finish(invalid_parameter);if(path.points.size()<2)return finish(out_of_memory);
    brush.points=path.points.size();double sx=0,sy=0;for(const auto& p:path.points){sx+=p.x;sy+=p.y;}brush.center={sx/static_cast<double>(brush.points),sy/static_cast<double>(brush.points)};
    brush.path=gui_flatten_path(path.points);
    double l=INFINITY,t=INFINITY,r=-INFINITY,b=-INFINITY;for(const auto& p:brush.path){l=std::min(l,p.x);t=std::min(t,p.y);r=std::max(r,p.x);b=std::max(b,p.y);}brush.rect={l,t,r-l,b-t};
    return finish(add_brush(std::move(brush),arg(output)));
}
if(symbol=="GdipGetPathGradientCenterColor"){const auto& brush=brush_of(arg(0),3);if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,brush.center_color);return finish(ok);}
if(symbol=="GdipSetPathGradientCenterColor"){brush_of(arg(0),3).center_color=u32(1);return finish(ok);}
if(symbol=="GdipGetPathGradientCenterPoint" || symbol=="GdipGetPathGradientCenterPointI"){const auto& brush=brush_of(arg(0),3);if(!arg(1))return finish(invalid_parameter);store_points(arg(1),{brush.center},symbol.ends_with('I'));return finish(ok);}
if(symbol=="GdipSetPathGradientCenterPoint" || symbol=="GdipSetPathGradientCenterPointI"){auto& brush=brush_of(arg(0),3);if(!arg(1))return finish(invalid_parameter);brush.center=point_array(arg(1),1,symbol.ends_with('I'))[0];return finish(ok);}
if(symbol=="GdipGetPathGradientFocusScales"){
    const auto& brush=brush_of(arg(0),3);if(!arg(1) || !arg(2))return finish(invalid_parameter);store_real(arg(1),brush.focus[0]);store_real(arg(2),brush.focus[1]);return finish(ok);}
if(symbol=="GdipSetPathGradientFocusScales"){auto& brush=brush_of(arg(0),3);const auto x=real(1),y=real(2);if(x<0 || y<0 || x>1 || y>1)return finish(invalid_parameter);brush.focus={x,y};return finish(ok);}
if(symbol=="GdipGetPathGradientPointCount" || symbol=="GdipGetPathGradientSurroundColorCount"){const auto& brush=brush_of(arg(0),3);if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,brush.points);return finish(ok);}
if(symbol=="GdipSetPathGradientSurroundColorsWithCount"){
    // (brush, colors, count*)：最多與點數相同；不足的點沿用最後一色。
    auto& brush=brush_of(arg(0),3);if(!arg(1) || !arg(2))return finish(invalid_parameter);const auto count=static_cast<std::int32_t>(memory.load(GuestAddress{arg(2)},32));
    if(count<=0 || static_cast<std::size_t>(count)>brush.points)return finish(invalid_parameter);brush.surround=color_array(arg(1),static_cast<std::size_t>(count));return finish(ok);
}
if(symbol=="GdipGetPathGradientSurroundColorsWithCount"){
    const auto& brush=brush_of(arg(0),3);if(!arg(1) || !arg(2))return finish(invalid_parameter);const auto count=static_cast<std::int32_t>(memory.load(GuestAddress{arg(2)},32));
    if(count<0 || static_cast<std::size_t>(count)<brush.surround.size())return finish(insufficient_buffer);
    memory.check(GuestAddress{arg(1)},brush.surround.size()*4,Access::Write);for(std::size_t i=0;i<brush.surround.size();++i)memory.store(GuestAddress{arg(1)+i*4},32,brush.surround[i]);
    memory.store(GuestAddress{arg(2)},32,brush.surround.size());return finish(ok);
}
if(symbol=="GdipGetPathGradientRect" || symbol=="GdipGetPathGradientRectI"){const auto& r=brush_of(arg(0),3).rect;if(!arg(1))return finish(invalid_parameter);store_rect(arg(1),r[0],r[1],r[2],r[3],symbol.ends_with('I'));return finish(ok);}
// Windows 的 GdipGetPathGradientPath／SetPathGradientPath 一律回 NotImplemented。
if(symbol=="GdipGetPathGradientPath" || symbol=="GdipSetPathGradientPath"){brush_of(arg(0),3);path_of(arg(1));return finish(not_implemented);}
// ---- 陰影線 ----
if(symbol=="GdipCreateHatchBrush"){
    if(u32(0)>52 || !arg(3))return finish(invalid_parameter);GdiplusBrush brush;brush.type=1;brush.hatch=u32(0);brush.color=u32(1);brush.back=u32(2);return finish(add_brush(std::move(brush),arg(3)));}
if(symbol=="GdipGetHatchStyle" || symbol=="GdipGetHatchForegroundColor" || symbol=="GdipGetHatchBackgroundColor"){
    const auto& brush=brush_of(arg(0),1);if(!arg(1))return finish(invalid_parameter);
    memory.store(GuestAddress{arg(1)},32,symbol=="GdipGetHatchStyle"?brush.hatch:symbol=="GdipGetHatchForegroundColor"?brush.color:brush.back);return finish(ok);}
// ---- 材質 ----
if(symbol=="GdipCreateTexture" || symbol=="GdipCreateTexture2" || symbol=="GdipCreateTexture2I" || symbol=="GdipCreateTextureIA" || symbol=="GdipCreateTextureIAI"){
    // Texture(image, wrap, out)；Texture2(image, wrap, x, y, w, h, out)；TextureIA(image, attributes, x, y, w, h, out)。像素於建立時複製。
    const auto* b=bitmap_of(arg(0));if(!b)return finish(invalid_parameter);if(b->lock)return finish(object_busy);
    const bool integer=symbol.ends_with('I'),whole=symbol=="GdipCreateTexture";const unsigned output=whole?2U:6U;
    // ImageAttributes 版本：外圍模式取自屬性，像素依 Brush 類別重新著色。
    const GdiplusImageAttributes* attributes=nullptr;
    if(symbol.find("IA")!=symbol.npos && arg(1)){const auto found=g.attributes.find(arg(1));if(found==g.attributes.end())return finish(invalid_parameter);attributes=&found->second;}
    const auto wrap=symbol.find("IA")!=symbol.npos?(attributes?attributes->wrap:0U):u32(1);if(!arg(output) || wrap>4)return finish(invalid_parameter);
    double x=0,y=0,w=b->width,h=b->height;if(!whole){x=coordinate(2,integer);y=coordinate(3,integer);w=coordinate(4,integer);h=coordinate(5,integer);}
    const auto left=static_cast<std::int64_t>(std::floor(x)),top=static_cast<std::int64_t>(std::floor(y)),width=static_cast<std::int64_t>(std::floor(w)),height=static_cast<std::int64_t>(std::floor(h));
    if(left<0 || top<0 || width<=0 || height<=0 || left+width>b->width || top+height>b->height)return finish(out_of_memory);
    GdiplusBrush brush;brush.type=2;brush.wrap=wrap;brush.width=static_cast<std::uint32_t>(width);brush.height=static_cast<std::uint32_t>(height);brush.pixels.reserve(static_cast<std::size_t>(width*height));
    for(std::int64_t j=0;j<height;++j)for(std::int64_t i=0;i<width;++i){const auto c=read_pixel(*b,static_cast<std::uint32_t>(left+i),static_cast<std::uint32_t>(top+j));brush.pixels.push_back(attributes?gdiplus_recolor(attributes->category(2),c):c);}
    return finish(add_brush(std::move(brush),arg(output)));
}
if(symbol=="GdipGetTextureImage"){
    const auto& brush=brush_of(arg(0),2);if(!arg(1))return finish(invalid_parameter);std::vector<std::uint8_t> bytes(brush.pixels.size()*4);
    for(std::size_t i=0;i<brush.pixels.size();++i)for(unsigned k=0;k<4;++k)bytes[i*4+k]=static_cast<std::uint8_t>(brush.pixels[i]>>(8*k));
    return finish(store_bitmap(g,memory,bytes,brush.width,brush.height,arg(1),pointer_bits,[&](auto size){return allocate_heap(memory,size);},release_handle));
}
// ---- 線性／路徑漸層與材質共用：變換、外圍模式、gamma、Blend 與 InterpolationColors ----
if(const auto type=brush_kind("GdipSet","Transform");type>=0){const auto& m=matrix_of(arg(1));if(!gui_inverse(m))return finish(invalid_parameter);brush_of(arg(0),static_cast<unsigned>(type)).transform=m;return finish(ok);}
if(const auto type=brush_kind("GdipGet","Transform");type>=0){matrix_of(arg(1))=brush_of(arg(0),static_cast<unsigned>(type)).transform;return finish(ok);}
if(const auto type=brush_kind("GdipReset","Transform");type>=0){brush_of(arg(0),static_cast<unsigned>(type)).transform=gui_identity;return finish(ok);}
if(const auto type=brush_kind("GdipMultiply","Transform");type>=0){combine_matrix(brush_of(arg(0),static_cast<unsigned>(type)).transform,matrix_of(arg(1)),u32(2));return finish(ok);}
if(const auto type=brush_kind("GdipTranslate","Transform");type>=0){combine_matrix(brush_of(arg(0),static_cast<unsigned>(type)).transform,{1,0,0,1,real(1),real(2)},u32(3));return finish(ok);}
if(const auto type=brush_kind("GdipScale","Transform");type>=0){combine_matrix(brush_of(arg(0),static_cast<unsigned>(type)).transform,{real(1),0,0,real(2),0,0},u32(3));return finish(ok);}
if(const auto type=brush_kind("GdipRotate","Transform");type>=0){combine_matrix(brush_of(arg(0),static_cast<unsigned>(type)).transform,rotation(real(1)),u32(2));return finish(ok);}
if(const auto type=brush_kind("GdipSet","WrapMode");type>=0){if(u32(1)>(type==4?3U:4U))return finish(invalid_parameter);brush_of(arg(0),static_cast<unsigned>(type)).wrap=u32(1);return finish(ok);}
if(const auto type=brush_kind("GdipGet","WrapMode");type>=0){const auto& brush=brush_of(arg(0),static_cast<unsigned>(type));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,brush.wrap);return finish(ok);}
if(const auto type=brush_kind("GdipSet","GammaCorrection");type>=0){brush_of(arg(0),static_cast<unsigned>(type)).gamma=u32(1)!=0;return finish(ok);}
if(const auto type=brush_kind("GdipGet","GammaCorrection");type>=0){const auto& brush=brush_of(arg(0),static_cast<unsigned>(type));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,brush.gamma?1U:0U);return finish(ok);}
if(const auto type=brush_kind("GdipSet","Blend");type>=0){
    // (brush, factors, positions, count)：兩點以上時起點須為 0、終點須為 1；設定後清除 InterpolationColors。
    auto& brush=brush_of(arg(0),static_cast<unsigned>(type));const auto count=s32(3);if(!arg(1) || !arg(2) || count<=0 || count>65536)return finish(invalid_parameter);
    auto factors=real_array(arg(1),static_cast<std::size_t>(count)),positions=real_array(arg(2),static_cast<std::size_t>(count));
    if(count>=2 && (positions.front()!=0 || positions.back()!=1))return finish(invalid_parameter);
    brush.factors=std::move(factors);brush.positions=std::move(positions);brush.preset_colors.clear();brush.preset_positions.clear();return finish(ok);
}
if(const auto type=brush_kind("GdipGet","Blend");type>=0){
    const auto& brush=brush_of(arg(0),static_cast<unsigned>(type));if(!arg(1) || !arg(2))return finish(invalid_parameter);
    if(s32(3)<static_cast<std::int32_t>(brush.factors.size()))return finish(insufficient_buffer);
    memory.check(GuestAddress{arg(1)},brush.factors.size()*4,Access::Write);memory.check(GuestAddress{arg(2)},brush.positions.size()*4,Access::Write);
    for(std::size_t i=0;i<brush.factors.size();++i){store_real(arg(1)+i*4,brush.factors[i]);store_real(arg(2)+i*4,brush.positions[i]);}return finish(ok);
}
if(const auto type=brush_kind("GdipGet","BlendCount");type>=0){const auto& brush=brush_of(arg(0),static_cast<unsigned>(type));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,brush.factors.size());return finish(ok);}
if(const auto type=brush_kind("GdipSet","PresetBlend");type>=0){
    // (brush, colors, positions, count)：至少兩色，起點 0、終點 1；設定後 Blend 回到線性。
    auto& brush=brush_of(arg(0),static_cast<unsigned>(type));const auto count=s32(3);if(!arg(1) || !arg(2) || count<2 || count>65536)return finish(invalid_parameter);
    auto positions=real_array(arg(2),static_cast<std::size_t>(count));if(positions.front()!=0 || positions.back()!=1)return finish(invalid_parameter);
    brush.preset_colors=color_array(arg(1),static_cast<std::size_t>(count));brush.preset_positions=std::move(positions);brush.factors={1};brush.positions={1};return finish(ok);
}
if(const auto type=brush_kind("GdipGet","PresetBlend");type>=0){
    const auto& brush=brush_of(arg(0),static_cast<unsigned>(type));if(!arg(1) || !arg(2))return finish(invalid_parameter);if(brush.preset_colors.empty())return finish(generic_error);
    if(s32(3)<static_cast<std::int32_t>(brush.preset_colors.size()))return finish(insufficient_buffer);
    memory.check(GuestAddress{arg(1)},brush.preset_colors.size()*4,Access::Write);memory.check(GuestAddress{arg(2)},brush.preset_positions.size()*4,Access::Write);
    for(std::size_t i=0;i<brush.preset_colors.size();++i){memory.store(GuestAddress{arg(1)+i*4},32,brush.preset_colors[i]);store_real(arg(2)+i*4,brush.preset_positions[i]);}return finish(ok);
}
if(const auto type=brush_kind("GdipGet","PresetBlendCount");type>=0){const auto& brush=brush_of(arg(0),static_cast<unsigned>(type));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,brush.preset_colors.size());return finish(ok);}
if(const auto type=brush_kind("GdipSet","LinearBlend");type>=0){gdiplus_triangular_blend(brush_of(arg(0),static_cast<unsigned>(type)),real(1),real(2));return finish(ok);}
if(const auto type=brush_kind("GdipSet","SigmaBlend");type>=0){gdiplus_bell_blend(brush_of(arg(0),static_cast<unsigned>(type)),real(1),real(2));return finish(ok);}
