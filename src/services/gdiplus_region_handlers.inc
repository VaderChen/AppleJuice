// GDI+ Region 家族：GpRegion 以節點樹保存（世界座標），需要時依變換轉成與 GDI region 相同的裝置矩形帶；
// 裁切、填滿、可見性、掃描線與 HRGN 共用同一份轉換，裁切交集沿用幾何家族的 combine_clip。
using RegionPtr=std::shared_ptr<const GdiplusState::RegionNode>;
constexpr std::uint32_t region_rect=0x10000000U,region_path=0x10000001U,region_empty=0x10000002U,region_infinite=0x10000003U;
const auto region_node=[](std::uint32_t type){auto node=std::make_shared<GdiplusState::RegionNode>();node->type=type;return node;};
const auto region_leaf=[&](std::uint32_t type)->RegionPtr{return region_node(type);};
const auto region_box=[&](double x,double y,double w,double h)->RegionPtr{auto node=region_node(region_rect);node->rect={x,y,w,h};return node;};
const auto region_shape=[&](GdiplusState::Path path)->RegionPtr{auto node=region_node(region_path);path.new_figure=true;node->path=std::move(path);return node;};
const auto region_of=[&](std::uint64_t handle)->RegionPtr&{const auto found=g.regions.find(handle);if(found==g.regions.end())throw std::invalid_argument("GDI+ Region 無效");return found->second;};
// 矩形依像素中心規則轉成裝置矩形（左上含、右下不含）；有旋轉或錯切時改走路徑。
const auto device_rect=[&](const GuiTransform& m,const std::array<double,4>& r)->GuiRegion{
    if(r[2]==0 || r[3]==0)return {};
    if(m[1]==0 && m[2]==0){
        const auto a=gui_point(m,r[0],r[1]),b=gui_point(m,r[0]+r[2],r[1]+r[3]);
        const auto edge=[](double v){return static_cast<std::int32_t>(std::clamp(std::ceil(v-0.5),-67108864.0,67108863.0));};
        const GuiRect out{edge(std::min(a[0],b[0])),edge(std::min(a[1],b[1])),edge(std::max(a[0],b[0])),edge(std::max(a[1],b[1]))};
        return gui_rect_empty(out)?GuiRegion{}:GuiRegion{out};
    }
    GdiplusState::Path path;add_rectangle(path,r[0],r[1],r[2],r[3]);transform_path(path,m);return native_path_region(gui_flatten_path(path.points),1);
};
const auto raster=[&](const auto& self,const GdiplusState::RegionNode& node,const GuiTransform& m)->GuiRegion{
    if(node.type==region_rect)return device_rect(m,node.rect);
    if(node.type==region_path){auto path=node.path;transform_path(path,m);return native_path_region(gui_flatten_path(path.points),path.fill+1);}
    if(node.type==region_empty)return {};
    if(node.type==region_infinite)return infinite;
    if(node.type<1 || node.type>5 || !node.left || !node.right)throw std::invalid_argument("GDI+ Region 節點無效");
    const auto a=self(self,*node.left,m),b=self(self,*node.right,m);return node.type==5?gui_region_combine(b,a,4):gui_region_combine(a,b,node.type);
};
const auto region_raster=[&](const RegionPtr& node,const GuiTransform& m){return raster(raster,*node,m);};
const auto same_region=[](const GuiRegion& a,const GuiRegion& b){return gui_region_combine(a,b,3).empty();};
// 合併時先化簡空與無限大的運算元（語義相同，避免樹無謂加深）；Complement 為「新區域減去目前區域」。
const auto region_combine=[&](const RegionPtr& left,const RegionPtr& right,std::uint32_t mode)->RegionPtr{
    if(mode>5)throw std::invalid_argument("GDI+ CombineMode 無效");if(mode==0)return right;
    const bool le=left->type==region_empty,re=right->type==region_empty,li=left->type==region_infinite,ri=right->type==region_infinite;
    if(mode==1){if(le || re)return region_leaf(region_empty);if(li)return right;if(ri)return left;}
    if(mode==2){if(li || ri)return region_leaf(region_infinite);if(le)return right;if(re)return left;}
    if(mode==3){if(le)return right;if(re)return left;}
    if(mode==4){if(le || re)return left;if(ri)return region_leaf(region_empty);}
    if(mode==5){if(le || re)return right;if(li)return region_leaf(region_empty);}
    auto node=region_node(mode);node->left=left;node->right=right;return node;
};
const auto transform_region=[&](const auto& self,const RegionPtr& node,const GuiTransform& m)->RegionPtr{
    if(node->type==region_empty || node->type==region_infinite)return node;
    if(node->type==region_rect){
        const auto& r=node->rect;
        if(m[1]==0 && m[2]==0){const auto a=gui_point(m,r[0],r[1]),b=gui_point(m,r[0]+r[2],r[1]+r[3]);return region_box(std::min(a[0],b[0]),std::min(a[1],b[1]),std::abs(b[0]-a[0]),std::abs(b[1]-a[1]));}
        GdiplusState::Path path;add_rectangle(path,r[0],r[1],r[2],r[3]);transform_path(path,m);return region_shape(std::move(path));
    }
    if(node->type==region_path){auto path=node->path;transform_path(path,m);return region_shape(std::move(path));}
    auto copy=region_node(node->type);copy->left=self(self,node->left,m);copy->right=self(self,node->right,m);return copy;
};
// 裝置矩形清單（裁切、HRGN）換回區域：平衡的聯集樹避免深層遞迴；inverse 為裝置→世界變換。
const auto union_tree=[&](const auto& self,const std::vector<RegionPtr>& leaves,std::size_t begin,std::size_t end)->RegionPtr{
    if(begin==end)return region_leaf(region_empty);if(end-begin==1)return leaves[begin];
    const auto middle=begin+(end-begin)/2;auto node=region_node(2);node->left=self(self,leaves,begin,middle);node->right=self(self,leaves,middle,end);return node;
};
const auto rects_region=[&](const GuiRegion& rects,const GuiTransform& inverse)->RegionPtr{
    if(same_region(rects,infinite))return region_leaf(region_infinite);std::vector<RegionPtr> leaves;
    for(const auto& r:rects)leaves.push_back(transform_region(transform_region,region_box(r[0],r[1],r[2]-r[0],r[3]-r[1]),inverse));
    return union_tree(union_tree,leaves,0,leaves.size());
};
// GDI+ 以 ±4194304 表示無限大區域的邊界。
const auto public_edge=[](std::int32_t v){return v<=-67108864?-4194304.0:v>=67108863?4194304.0:static_cast<double>(v);};
const auto region_bounds=[&](const RegionPtr& node)->std::array<double,4>{
    if(node->type==region_infinite)return {-4194304,-4194304,8388608,8388608};
    if(node->type==region_empty)return {0,0,0,0};
    if(node->type==region_rect){auto r=node->rect;if(r[2]<0){r[0]+=r[2];r[2]=-r[2];}if(r[3]<0){r[1]+=r[3];r[3]=-r[3];}return r;}
    if(node->type==region_path){
        const auto flat=gui_flatten_path(node->path.points);if(flat.empty())return {0,0,0,0};double l=INFINITY,t=INFINITY,r=-INFINITY,b=-INFINITY;
        for(const auto& p:flat){l=std::min(l,p.x);t=std::min(t,p.y);r=std::max(r,p.x);b=std::max(b,p.y);}return {l,t,r-l,b-t};
    }
    const auto rects=region_raster(node,gui_identity);if(rects.empty())return {0,0,0,0};const auto box=gui_region_bounds(rects);
    const auto l=public_edge(box[0]),t=public_edge(box[1]);return {l,t,public_edge(box[2])-l,public_edge(box[3])-t};
};
// GdipGetRegionData：標頭為（其後位元組數, CRC32, 版本 0xdbc01002, 子節點數），接著前序節點；
// 路徑沿用 EMF+ 路徑物件（版本、點數、旗標、點、型別、補齊 4 位元組），全為 16 位元整數時以 0x4000 旗標壓縮。
const auto region_bytes=[&](const RegionPtr& root){
    std::vector<std::uint8_t> out(16);std::uint32_t children=0;
    const auto put=[&](std::uint32_t v){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(v>>(8*i)));};
    const auto put_real=[&](double v){put(std::bit_cast<std::uint32_t>(static_cast<float>(v)));};
    const auto write=[&](const auto& self,const GdiplusState::RegionNode& node)->void{
        put(node.type);
        if(node.type>=1 && node.type<=5){children+=2;self(self,*node.left);self(self,*node.right);return;}
        if(node.type==region_rect){for(const auto v:node.rect)put_real(v);return;}
        if(node.type!=region_path)return;
        const auto& points=node.path.points;
        const bool compact=std::all_of(points.begin(),points.end(),[](const GuiPathPoint& p){return p.x==std::trunc(p.x) && p.y==std::trunc(p.y) && std::abs(p.x)<=32767 && std::abs(p.y)<=32767;});
        const auto size_at=out.size();put(0);const auto begin=out.size();
        put(0xdbc01001U);put(static_cast<std::uint32_t>(points.size()));put(compact?0x4000U:0U);
        for(const auto& p:points){
            if(!compact){put_real(p.x);put_real(p.y);continue;}
            for(const auto v:{p.x,p.y}){const auto bits=static_cast<std::uint16_t>(static_cast<std::int16_t>(v));out.push_back(static_cast<std::uint8_t>(bits));out.push_back(static_cast<std::uint8_t>(bits>>8));}
        }
        for(const auto& p:points)out.push_back(public_type(p.type));
        while((out.size()-begin)%4)out.push_back(0);
        const auto size=static_cast<std::uint32_t>(out.size()-begin);for(unsigned i=0;i<4;++i)out[size_at+i]=static_cast<std::uint8_t>(size>>(8*i));
    };
    write(write,*root);
    const auto set=[&](std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)out[at+i]=static_cast<std::uint8_t>(v>>(8*i));};
    set(0,static_cast<std::uint32_t>(out.size()-8));set(8,0xdbc01002U);set(12,children);
    set(4,static_cast<std::uint32_t>(::crc32(0,out.data()+8,static_cast<uInt>(out.size()-8))));
    return out;
};
const auto region_parse=[&](std::span<const std::uint8_t> data)->RegionPtr{
    std::size_t at=0;
    const auto take=[&](std::size_t bytes)->std::uint32_t{if(at+bytes>data.size())throw std::invalid_argument("GDI+ Region 資料不完整");std::uint32_t v=0;for(std::size_t i=0;i<bytes;++i)v|=std::uint32_t{data[at+i]}<<(8*i);at+=bytes;return v;};
    const auto take_real=[&]{const auto v=static_cast<double>(std::bit_cast<float>(take(4)));if(!std::isfinite(v))throw std::invalid_argument("GDI+ Region 座標無效");return v;};
    const auto size=take(4);(void)take(4);if(size<8 || std::size_t{size}+8>data.size())throw std::invalid_argument("GDI+ Region 資料大小無效");
    data=data.first(std::size_t{size}+8);if((take(4)&0xfffff000U)!=0xdbc01000U)throw std::invalid_argument("GDI+ Region 版本無效");(void)take(4);
    const auto read=[&](const auto& self,unsigned depth)->RegionPtr{
        if(depth>256)throw std::invalid_argument("GDI+ Region 巢狀過深");const auto type=take(4);
        if(type>=1 && type<=5){auto node=region_node(type);node->left=self(self,depth+1);node->right=self(self,depth+1);return node;}
        if(type==region_rect){const auto x=take_real(),y=take_real(),w=take_real(),h=take_real();return region_box(x,y,w,h);}
        if(type==region_empty || type==region_infinite)return region_leaf(type);
        if(type!=region_path)throw std::invalid_argument("GDI+ Region 節點型別無效");
        const auto bytes=take(4);const auto end=at+bytes;if(bytes<12 || end>data.size())throw std::invalid_argument("GDI+ Region 路徑大小無效");
        if((take(4)&0xfffff000U)!=0xdbc01000U)throw std::invalid_argument("GDI+ Region 路徑版本無效");
        const auto count=take(4),flags=take(4);if(flags&0x1800U)throw std::invalid_argument("GDI+ Region 相對或 RLE 路徑未支援");
        const std::size_t point_bytes=(flags&0x4000U)?4:8;if(count>(end-at)/(point_bytes+1))throw std::invalid_argument("GDI+ Region 路徑點數無效");
        GdiplusState::Path path;path.points.resize(count);
        for(auto& p:path.points){if(point_bytes==4){p.x=static_cast<std::int16_t>(static_cast<std::uint16_t>(take(2)));p.y=static_cast<std::int16_t>(static_cast<std::uint16_t>(take(2)));}else {p.x=take_real();p.y=take_real();}}
        for(auto& p:path.points)p.type=private_type(static_cast<std::uint8_t>(take(1)));
        if(!path.points.empty() && (path.points.front().type&6)!=6)throw std::invalid_argument("GDI+ Region 路徑起點無效");
        at=end;return region_shape(std::move(path));
    };
    return read(read,0);
};
if(symbol=="GdipCreateRegion" || symbol=="GdipCreateRegionRect" || symbol=="GdipCreateRegionRectI" || symbol=="GdipCreateRegionPath" ||
   symbol=="GdipCreateRegionRgnData" || symbol=="GdipCreateRegionHrgn" || symbol=="GdipCloneRegion"){
    RegionPtr value;unsigned output=1;
    if(symbol=="GdipCreateRegion"){value=region_leaf(region_infinite);output=0;}
    else if(symbol=="GdipCreateRegionRect" || symbol=="GdipCreateRegionRectI"){if(!arg(0))return finish(invalid_parameter);const auto r=rect_array(arg(0),1,symbol.ends_with('I'))[0];value=region_box(r[0],r[1],r[2],r[3]);}
    else if(symbol=="GdipCreateRegionPath")value=region_shape(path_of(arg(0)));
    else if(symbol=="GdipCreateRegionRgnData"){
        output=2;if(!arg(0) || s32(1)<16 || s32(1)>16*1024*1024)return finish(invalid_parameter);
        std::vector<std::uint8_t> bytes(u32(1));memory.read(GuestAddress{arg(0)},bytes);value=region_parse(bytes);
    }
    else if(symbol=="GdipCreateRegionHrgn"){const auto found=gui_.regions.find(arg(0));if(found==gui_.regions.end())return finish(invalid_parameter);value=rects_region(found->second,gui_identity);}
    else value=region_of(arg(0));
    if(!arg(output))return finish(invalid_parameter);const auto handle=new_object(arg(output));if(!handle)return finish(out_of_memory);
    g.regions[handle]=value;memory.store(GuestAddress{arg(output)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipDeleteRegion"){if(!g.regions.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);}
if(symbol=="GdipSetInfinite" || symbol=="GdipSetEmpty"){region_of(arg(0))=region_leaf(symbol=="GdipSetEmpty"?region_empty:region_infinite);return finish(ok);}
if(symbol=="GdipCombineRegionRect" || symbol=="GdipCombineRegionRectI" || symbol=="GdipCombineRegionPath" || symbol=="GdipCombineRegionRegion"){
    auto& target=region_of(arg(0));if(u32(2)>5)return finish(invalid_parameter);RegionPtr other;
    if(symbol=="GdipCombineRegionRegion")other=region_of(arg(1));
    else if(symbol=="GdipCombineRegionPath")other=region_shape(path_of(arg(1)));
    else {if(!arg(1))return finish(invalid_parameter);const auto r=rect_array(arg(1),1,symbol.ends_with('I'))[0];other=region_box(r[0],r[1],r[2],r[3]);}
    target=region_combine(target,other,u32(2));return finish(ok);
}
if(symbol=="GdipTransformRegion" || symbol=="GdipTranslateRegion" || symbol=="GdipTranslateRegionI"){
    auto& target=region_of(arg(0));GuiTransform m{1,0,0,1,0,0};
    if(symbol=="GdipTransformRegion")m=matrix_of(arg(1));else {const bool integer=symbol.ends_with('I');m[4]=coordinate(1,integer);m[5]=coordinate(2,integer);}
    target=transform_region(transform_region,target,m);return finish(ok);
}
if(symbol=="GdipGetRegionBounds" || symbol=="GdipGetRegionBoundsI"){
    const auto& region=region_of(arg(0));graphics_of(arg(1));if(!arg(2))return finish(invalid_parameter);
    const auto box=region_bounds(region);store_rect(arg(2),box[0],box[1],box[2],box[3],symbol.ends_with('I'));return finish(ok);
}
if(symbol=="GdipGetRegionDataSize" || symbol=="GdipGetRegionData"){
    const auto bytes=region_bytes(region_of(arg(0)));if(!arg(1))return finish(invalid_parameter);
    if(symbol=="GdipGetRegionDataSize"){memory.store(GuestAddress{arg(1)},32,bytes.size());return finish(ok);}
    if(u32(2)<bytes.size())return finish(insufficient_buffer);memory.write(GuestAddress{arg(1)},std::span<const std::uint8_t>(bytes));
    if(arg(3))memory.store(GuestAddress{arg(3)},32,bytes.size());return finish(ok);
}
if(symbol=="GdipGetRegionHRgn"){
    // 無限大區域依 Windows 回傳 NULL HRGN；其餘依 Graphics 的世界→裝置變換（未指定時為單位矩陣）轉成 GDI region。
    const auto& region=region_of(arg(0));if(!arg(2))return finish(invalid_parameter);
    auto rects=region_raster(region,arg(1)?device_of(arg(1)):gui_identity);
    if(same_region(rects,infinite)){memory.store(GuestAddress{arg(2)},pointer_bits,0);return finish(ok);}
    if(gui_.regions.size()>=65536)return finish(out_of_memory);const auto handle=gui_.next_object++;gui_.regions.emplace(handle,std::move(rects));
    memory.store(GuestAddress{arg(2)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipIsEmptyRegion" || symbol=="GdipIsInfiniteRegion"){
    const auto& region=region_of(arg(0));const auto m=device_of(arg(1));if(!arg(2))return finish(invalid_parameter);const auto rects=region_raster(region,m);
    memory.store(GuestAddress{arg(2)},32,(symbol=="GdipIsEmptyRegion"?rects.empty():same_region(rects,infinite))?1U:0U);return finish(ok);
}
if(symbol=="GdipIsEqualRegion"){
    const auto m=device_of(arg(2));if(!arg(3))return finish(invalid_parameter);
    memory.store(GuestAddress{arg(3)},32,same_region(region_raster(region_of(arg(0)),m),region_raster(region_of(arg(1)),m))?1U:0U);return finish(ok);
}
if(symbol=="GdipIsVisibleRegionPoint" || symbol=="GdipIsVisibleRegionPointI" || symbol=="GdipIsVisibleRegionRect" || symbol=="GdipIsVisibleRegionRectI"){
    // 區域與點都以世界座標判斷（Graphics 可為 NULL）。
    const auto& region=region_of(arg(0));const bool integer=symbol.ends_with('I'),rectangle=symbol.find("Rect")!=symbol.npos;const unsigned out=rectangle?6U:4U;if(!arg(out))return finish(invalid_parameter);
    const auto rects=region_raster(region,gui_identity);bool visible=false;
    if(rectangle)visible=!gui_region_combine(device_rect(gui_identity,{coordinate(1,integer),coordinate(2,integer),coordinate(3,integer),coordinate(4,integer)}),rects,1).empty();
    else {const auto x=std::floor(coordinate(1,integer)),y=std::floor(coordinate(2,integer));visible=std::any_of(rects.begin(),rects.end(),[&](const GuiRect& r){return x>=r[0] && x<r[2] && y>=r[1] && y<r[3];});}
    memory.store(GuestAddress{arg(out)},32,visible?1U:0U);return finish(ok);
}
if(symbol=="GdipGetRegionScansCount" || symbol=="GdipGetRegionScans" || symbol=="GdipGetRegionScansI"){
    // (region, [rects,] count, matrix)：依矩陣轉換後的水平帶矩形。
    const bool count_only=symbol=="GdipGetRegionScansCount";const auto& region=region_of(arg(0));
    const unsigned count_at=count_only?1U:2U,matrix_at=count_only?2U:3U;if(!arg(count_at) || !arg(matrix_at))return finish(invalid_parameter);
    const auto rects=region_raster(region,matrix_of(arg(matrix_at)));memory.store(GuestAddress{arg(count_at)},32,rects.size());
    if(!count_only && arg(1)){
        memory.check(GuestAddress{arg(1)},rects.size()*16,Access::Write);
        for(std::size_t i=0;i<rects.size();++i){const auto l=public_edge(rects[i][0]),t=public_edge(rects[i][1]);store_rect(arg(1)+i*16,l,t,public_edge(rects[i][2])-l,public_edge(rects[i][3])-t,symbol.ends_with('I'));}
    }
    return finish(ok);
}
if(symbol=="GdipFillRegion"){
    const auto& info=graphics_of(arg(0));const auto brush=g.brushes.find(arg(1));if(brush==g.brushes.end())return finish(invalid_parameter);
    const auto m=device_of(arg(0));return finish(paint_region(arg(0),gdiplus_shader(brush->second,m,info.origin_x,info.origin_y),region_raster(region_of(arg(2)),m),1));
}
if(symbol=="GdipSetClipRegion" || symbol=="GdipSetClipHrgn"){
    // GpRegion 為世界座標；HRGN 依 Windows 為裝置座標。
    auto& info=graphics_of(arg(0));std::optional<GuiRegion> added;
    if(symbol=="GdipSetClipRegion"){auto rects=region_raster(region_of(arg(1)),device_of(arg(0)));if(!same_region(rects,infinite))added=std::move(rects);}
    else {const auto found=gui_.regions.find(arg(1));if(found==gui_.regions.end())return finish(invalid_parameter);added=found->second;}
    combine_clip(info.clip,added,u32(2));return finish(ok);
}
if(symbol=="GdipGetClip"){
    const auto& info=graphics_of(arg(0));auto& target=region_of(arg(1));
    if(!info.clip){target=region_leaf(region_infinite);return finish(ok);}
    const auto inverse=gui_inverse(device_of(arg(0)));if(!inverse)return finish(invalid_parameter);target=rects_region(*info.clip,*inverse);return finish(ok);
}
