// GDI+ DrawImage 家族、ImageAttributes 與 CachedBitmap：所有 DrawImage 變體換成「目的平行四邊形（世界座標三點）＋來源矩形（像素）」，
// 經世界→裝置變換後逐像素反向取樣、套用重新著色，交給 paint_region 依裁切合成；軸對齊、整數像素且無屬性與裁切時改走 GPU 拉伸複製。
const auto attributes_of=[&](std::uint64_t handle)->GdiplusImageAttributes&{const auto found=g.attributes.find(handle);if(found==g.attributes.end())throw std::invalid_argument("GDI+ ImageAttributes 無效");return found->second;};
// ---- ImageAttributes ----
if(symbol=="GdipCreateImageAttributes"){const auto handle=new_object(arg(0));if(!handle)return finish(out_of_memory);g.attributes[handle]={};memory.store(GuestAddress{arg(0)},pointer_bits,handle);return finish(ok);}
if(symbol=="GdipCloneImageAttributes"){const auto copy=attributes_of(arg(0));const auto handle=new_object(arg(1));if(!handle)return finish(out_of_memory);g.attributes[handle]=copy;memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);}
if(symbol=="GdipDisposeImageAttributes"){if(!g.attributes.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);}
if(symbol.starts_with("GdipSetImageAttributes") || symbol=="GdipResetImageAttributes"){
    auto& attributes=attributes_of(arg(0));
    if(symbol=="GdipSetImageAttributesWrapMode"){if(u32(1)>4)return finish(invalid_parameter);attributes.wrap=u32(1);attributes.wrap_color=u32(2);attributes.clamp=u32(3)!=0;return finish(ok);}
    if(symbol=="GdipSetImageAttributesCachedBackground"){attributes.cached=u32(1)!=0;return finish(ok);}
    if(u32(1)>4)return finish(invalid_parameter);auto& adjust=attributes.adjust[u32(1)];const bool enable=u32(2)!=0;
    if(symbol=="GdipResetImageAttributes"){adjust={};return finish(ok);}
    if(symbol=="GdipSetImageAttributesToIdentity"){std::array<double,25> identity{};for(unsigned i=0;i<5;++i)identity[i*6]=1;adjust.matrix=identity;adjust.matrix_flags=0;return finish(ok);}
    if(symbol=="GdipSetImageAttributesColorMatrix"){
        // (attr, type, enable, colorMatrix, grayMatrix, flags)：AltGray（2）需要灰階矩陣。
        if(!enable){adjust.matrix.reset();return finish(ok);}if(!arg(3) || u32(5)>2 || (u32(5)==2 && !arg(4)))return finish(invalid_parameter);
        const auto read_matrix=[&](std::uint64_t at){std::array<double,25> m{};const auto values=real_array(at,25);std::copy(values.begin(),values.end(),m.begin());return m;};
        adjust.matrix=read_matrix(arg(3));if(arg(4))adjust.gray=read_matrix(arg(4));adjust.matrix_flags=u32(5);return finish(ok);
    }
    if(symbol=="GdipSetImageAttributesThreshold"){if(enable)adjust.threshold=real(3);else adjust.threshold.reset();return finish(ok);}
    if(symbol=="GdipSetImageAttributesGamma"){if(!enable){adjust.gamma.reset();return finish(ok);}const auto gamma=real(3);if(!(gamma>0))return finish(invalid_parameter);adjust.gamma=gamma;return finish(ok);}
    if(symbol=="GdipSetImageAttributesNoOp"){adjust.noop=enable;return finish(ok);}
    if(symbol=="GdipSetImageAttributesColorKeys"){if(enable)adjust.keys=std::pair{u32(3),u32(4)};else adjust.keys.reset();return finish(ok);}
    if(symbol=="GdipSetImageAttributesOutputChannel"){if(!enable){adjust.channel.reset();return finish(ok);}if(u32(3)>3)return finish(invalid_parameter);adjust.channel=u32(3);return finish(ok);}
    if(symbol=="GdipSetImageAttributesOutputChannelColorProfile"){if(!enable){adjust.profile.reset();return finish(ok);}if(!arg(3))return finish(invalid_parameter);adjust.profile=read_utf16(memory,GuestAddress{arg(3)});return finish(ok);}
    if(symbol=="GdipSetImageAttributesRemapTable"){
        // (attr, type, enable, mapSize, ColorMap*)：ColorMap 為（舊色, 新色）。
        if(!enable){adjust.remap.clear();return finish(ok);}if(!arg(4) || !u32(3) || u32(3)>65536)return finish(invalid_parameter);
        const auto colors=color_array(arg(4),std::size_t{u32(3)}*2);adjust.remap.clear();for(std::size_t i=0;i<u32(3);++i)adjust.remap.push_back({colors[i*2],colors[i*2+1]});return finish(ok);
    }
    return finish(not_implemented);
}
if(symbol=="GdipGetImageAttributesAdjustedPalette"){
    const auto& attributes=attributes_of(arg(0));if(!arg(1) || u32(2)>4)return finish(invalid_parameter);
    const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+4},32));if(count>256)return finish(invalid_parameter);
    for(std::uint32_t i=0;i<count;++i){const auto at=arg(1)+8+i*4;memory.store(GuestAddress{at},32,gdiplus_recolor(attributes.category(u32(2)),static_cast<std::uint32_t>(memory.load(GuestAddress{at},32))));}
    return finish(ok);
}
// ---- 共用繪製 ----
// 來源單位換成像素：Point 3（1/72 吋）、Inch 4、Document 5（1/300 吋）、Millimeter 6 依影像 DPI；World 0、Display 1、Pixel 2 視為像素。
const auto source_pixels=[&](const GdiplusState::Bitmap& b,std::array<double,4> r,std::uint32_t unit){
    static constexpr double per_inch[]{0,0,0,72,1,300,25.4};if(unit>6)throw std::invalid_argument("GDI+ 來源單位無效");
    if(unit>=3){for(unsigned i=0;i<4;i+=2)r[i]*=b.dpi_x/per_inch[unit];for(unsigned i=1;i<4;i+=2)r[i]*=b.dpi_y/per_inch[unit];}return r;};
// 影像（或其中一塊像素）的實際大小換成世界座標：依影像 DPI 換成吋，再以 96 DPI 與頁面單位換算。
const auto physical_size=[&](std::uint64_t graphics,const GdiplusState::Bitmap& b,double w,double h){const auto factor=page_factor(graphics_of(graphics));return std::array<double,2>{w*96/b.dpi_x/factor,h*96/b.dpi_y/factor};};
const auto draw_image=[&](std::uint64_t graphics,const GdiplusState::Bitmap& b,const std::array<std::array<double,2>,3>& points,const std::array<double,4>& src,const GdiplusImageAttributes* attributes)->std::uint32_t{
    const auto found=g.graphics.find(graphics);if(found==g.graphics.end() || !gui_.contexts.contains(found->second))return invalid_parameter;if(b.lock)return wrong_state;
    if(!(src[2]>0) || !(src[3]>0))return ok;
    const auto m=device_of(graphics);std::array<std::array<double,2>,4> d{};for(unsigned i=0;i<3;++i)d[i]=gui_point(m,points[i][0],points[i][1]);
    const std::array<double,2> e1{d[1][0]-d[0][0],d[1][1]-d[0][1]},e2{d[2][0]-d[0][0],d[2][1]-d[0][1]};d[3]={d[1][0]+e2[0],d[1][1]+e2[1]};
    const auto det=e1[0]*e2[1]-e1[1]*e2[0];if(det==0)return ok;
    const auto& info=g.graphics_state[graphics];const auto surface=raster_surface(memory,found->second);if(!surface)return generic_error;
    // 需要的來源視窗（雙線性取樣多取一圈）。
    const auto x0=static_cast<std::int64_t>(std::floor(src[0]))-1,y0=static_cast<std::int64_t>(std::floor(src[1]))-1,x1=static_cast<std::int64_t>(std::ceil(src[0]+src[2]))+1,y1=static_cast<std::int64_t>(std::ceil(src[1]+src[3]))+1;
    const auto integral=[](double v){return std::abs(v-std::round(v))<1e-6;};
    const bool fast=!attributes && !info.clip && e1[1]==0 && e2[0]==0 && e1[0]>0 && e2[1]>0 && integral(d[0][0]) && integral(d[0][1]) && integral(d[3][0]) && integral(d[3][1]) &&
        integral(src[0]) && integral(src[1]) && integral(src[2]) && integral(src[3]) && src[0]>=0 && src[1]>=0 && src[0]+src[2]<=b.width && src[1]+src[3]<=b.height;
    if(fast){
        const auto sx=static_cast<std::uint32_t>(std::lround(src[0])),sy=static_cast<std::uint32_t>(std::lround(src[1])),sw=static_cast<std::uint32_t>(std::lround(src[2])),sh=static_cast<std::uint32_t>(std::lround(src[3]));
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(sw)*sh);
        for(std::uint32_t y=0;y<sh;++y)for(std::uint32_t x=0;x<sw;++x){const auto c=premultiply(read_pixel(b,sx+x,sy+y));pixels[std::size_t{y}*sw+x]=(c&0xff00ff00U)|((c&255U)<<16)|((c>>16)&255U);}
        const auto source=make_hardware_raster(sw,sh,pixels);auto target=gui_.contexts.at(found->second);
        target.world=gui_identity;target.viewport_extent=target.window_extent={1,1};target.viewport_origin=target.window_origin={0,0};
        const std::array<std::int32_t,4> rect{static_cast<std::int32_t>(std::lround(d[0][0])),static_cast<std::int32_t>(std::lround(d[0][1])),static_cast<std::int32_t>(std::lround(d[3][0])),static_cast<std::int32_t>(std::lround(d[3][1]))};
        target.stretch_mode=info.interpolation==5 || (rect[2]-rect[0]==static_cast<std::int32_t>(sw) && rect[3]-rect[1]==static_cast<std::int32_t>(sh))?3U:4U;
        RasterBlit options;options.operation=0xcc;surface->blit(*source,rect,{0,0,static_cast<std::int32_t>(sw),static_cast<std::int32_t>(sh)},target,0x01ff0000U,options);return ok;
    }
    // 來源視窗先解碼並重新著色；超出影像的位置依 ImageAttributes 外圍模式（預設 Clamp 透明）。
    const auto cx0=std::clamp<std::int64_t>(x0,0,b.width),cy0=std::clamp<std::int64_t>(y0,0,b.height),cx1=std::clamp<std::int64_t>(x1,0,b.width),cy1=std::clamp<std::int64_t>(y1,0,b.height);
    const bool tiled=attributes && attributes->wrap<4;std::vector<std::uint32_t> texels;std::int64_t ox=0,oy=0,tw=0,th=0;
    if(tiled){ox=0;oy=0;tw=b.width;th=b.height;}else {ox=cx0;oy=cy0;tw=cx1-cx0;th=cy1-cy0;}
    if(tw>0 && th>0){if(static_cast<std::uint64_t>(tw*th)>stream_limit/4)return out_of_memory;texels.resize(static_cast<std::size_t>(tw*th));
        for(std::int64_t y=0;y<th;++y)for(std::int64_t x=0;x<tw;++x){auto c=read_pixel(b,static_cast<std::uint32_t>(ox+x),static_cast<std::uint32_t>(oy+y));if(attributes)c=gdiplus_recolor(attributes->category(1),c);texels[static_cast<std::size_t>(y*tw+x)]=c;}}
    const auto outside=attributes && attributes->wrap==4?attributes->wrap_color:0U;const auto wrap=attributes?attributes->wrap:4U;
    const auto bw=static_cast<std::int64_t>(b.width),bh=static_cast<std::int64_t>(b.height);
    const auto fetch=[tiled,tw,th,ox,oy,bw,bh,wrap,outside,&texels](std::int64_t x,std::int64_t y)->std::uint32_t{
        if(tiled){const auto tile_x=x>=0?x/tw:-((-x-1)/tw)-1,tile_y=y>=0?y/th:-((-y-1)/th)-1;auto px=x-tile_x*tw,py=y-tile_y*th;
            if((wrap==1 || wrap==3) && (tile_x&1))px=tw-1-px;if((wrap==2 || wrap==3) && (tile_y&1))py=th-1-py;return texels[static_cast<std::size_t>(py*tw+px)];}
        if(x<0 || y<0 || x>=bw || y>=bh || x<ox || y<oy || x>=ox+tw || y>=oy+th)return outside;
        return texels[static_cast<std::size_t>((y-oy)*tw+(x-ox))];};
    const bool nearest=info.interpolation==5;
    const auto shader=[nearest,d,e1,e2,det,src,&fetch](double x,double y)->std::uint32_t{
        const auto dx=x-d[0][0],dy=y-d[0][1];const auto u=(dx*e2[1]-dy*e2[0])/det,v=(e1[0]*dy-e1[1]*dx)/det;
        const auto sx=src[0]+u*src[2],sy=src[1]+v*src[3];
        if(nearest)return fetch(static_cast<std::int64_t>(std::floor(sx)),static_cast<std::int64_t>(std::floor(sy)));
        const auto fx=sx-0.5,fy=sy-0.5;const auto ix=static_cast<std::int64_t>(std::floor(fx)),iy=static_cast<std::int64_t>(std::floor(fy));const auto ax=fx-static_cast<double>(ix),ay=fy-static_cast<double>(iy);
        std::array<double,4> sum{};const std::array<std::pair<std::array<std::int64_t,2>,double>,4> taps{{{{ix,iy},(1-ax)*(1-ay)},{{ix+1,iy},ax*(1-ay)},{{ix,iy+1},(1-ax)*ay},{{ix+1,iy+1},ax*ay}}};
        for(const auto& [at,weight]:taps){const auto c=premultiply(fetch(at[0],at[1]));for(unsigned k=0;k<4;++k)sum[k]+=static_cast<double>((c>>(8*k))&255U)*weight;}
        std::uint32_t c=0;for(unsigned k=0;k<4;++k)c|=static_cast<std::uint32_t>(std::lround(std::clamp(sum[k],0.0,255.0)))<<(8*k);return unpremultiply(c);};
    // 目的平行四邊形先裁到表面範圍再轉成區域，避免超大目的矩形配置遮罩。
    std::vector<std::array<double,2>> polygon{d[0],d[1],d[3],d[2]};
    const auto bounds=surface_region(graphics);const auto box=gui_region_bounds(bounds);
    const std::array<double,4> edge{static_cast<double>(box[0])-1,static_cast<double>(box[1])-1,static_cast<double>(box[2])+1,static_cast<double>(box[3])+1};
    for(unsigned side=0;side<4 && !polygon.empty();++side){
        std::vector<std::array<double,2>> next;const auto axis=side%2;const bool low=side<2;const auto limit=edge[side];
        const auto inside=[&](const std::array<double,2>& p){return low?p[axis]>=limit:p[axis]<=limit;};
        for(std::size_t i=0;i<polygon.size();++i){const auto& a=polygon[i];const auto& c=polygon[(i+1)%polygon.size()];
            if(inside(a))next.push_back(a);
            if(inside(a)!=inside(c)){const auto t=(limit-a[axis])/(c[axis]-a[axis]);next.push_back({a[0]+(c[0]-a[0])*t,a[1]+(c[1]-a[1])*t});}}
        polygon=std::move(next);
    }
    if(polygon.size()<3)return ok;
    std::vector<GuiPathPoint> outline;for(std::size_t i=0;i<polygon.size();++i)outline.push_back({polygon[i][0],polygon[i][1],static_cast<std::uint8_t>(i?2:6)});outline.back().type|=1;
    return paint_region(graphics,shader,native_path_region(outline,1),1);
};
const auto rect_points=[](double x,double y,double w,double h){return std::array<std::array<double,2>,3>{{{x,y},{x+w,y},{x,y+h}}};};
if(symbol=="GdipDrawImage" || symbol=="GdipDrawImageI" || symbol=="GdipDrawImageRect" || symbol=="GdipDrawImageRectI"){
    // DrawImage(x, y) 以影像實際大小繪製；DrawImageRect 縮放到指定矩形。
    const auto& b=image_of(arg(1));const bool integer=symbol.ends_with('I'),rectangle=symbol.find("Rect")!=symbol.npos;const auto x=coordinate(2,integer),y=coordinate(3,integer);
    const auto size=rectangle?std::array<double,2>{coordinate(4,integer),coordinate(5,integer)}:physical_size(arg(0),b,b.width,b.height);
    return finish(draw_image(arg(0),b,rect_points(x,y,size[0],size[1]),{0,0,static_cast<double>(b.width),static_cast<double>(b.height)},nullptr));
}
if(symbol=="GdipDrawImagePoints" || symbol=="GdipDrawImagePointsI"){
    // 三點：左上、右上、左下。
    const auto& b=image_of(arg(1));if(!arg(2) || s32(3)!=3)return finish(invalid_parameter);const auto p=point_array(arg(2),3,symbol.ends_with('I'));
    return finish(draw_image(arg(0),b,{{p[0],p[1],p[2]}},{0,0,static_cast<double>(b.width),static_cast<double>(b.height)},nullptr));
}
if(symbol=="GdipDrawImagePointRect" || symbol=="GdipDrawImagePointRectI"){
    // (x, y, srcx, srcy, srcwidth, srcheight, srcUnit)：來源區塊以實際大小繪製。
    const auto& b=image_of(arg(1));const bool integer=symbol.ends_with('I');
    const auto src=source_pixels(b,{coordinate(4,integer),coordinate(5,integer),coordinate(6,integer),coordinate(7,integer)},u32(8));const auto size=physical_size(arg(0),b,src[2],src[3]);
    return finish(draw_image(arg(0),b,rect_points(coordinate(2,integer),coordinate(3,integer),size[0],size[1]),src,nullptr));
}
if(symbol=="GdipDrawImageRectRect" || symbol=="GdipDrawImageRectRectI"){
    // (dst x, y, w, h, src x, y, w, h, srcUnit, attributes, callback, data)
    const auto& b=image_of(arg(1));const bool integer=symbol.ends_with('I');const auto* attributes=arg(11)?&attributes_of(arg(11)):nullptr;
    const auto src=source_pixels(b,{coordinate(6,integer),coordinate(7,integer),coordinate(8,integer),coordinate(9,integer)},u32(10));
    return finish(draw_image(arg(0),b,rect_points(coordinate(2,integer),coordinate(3,integer),coordinate(4,integer),coordinate(5,integer)),src,attributes));
}
if(symbol=="GdipDrawImagePointsRect" || symbol=="GdipDrawImagePointsRectI"){
    // (points, count, src x, y, w, h, srcUnit, attributes, callback, data)
    const auto& b=image_of(arg(1));const bool integer=symbol.ends_with('I');if(!arg(2) || s32(3)!=3)return finish(invalid_parameter);
    const auto p=point_array(arg(2),3,integer);const auto* attributes=arg(9)?&attributes_of(arg(9)):nullptr;
    const auto src=source_pixels(b,{coordinate(4,integer),coordinate(5,integer),coordinate(6,integer),coordinate(7,integer)},u32(8));
    return finish(draw_image(arg(0),b,{{p[0],p[1],p[2]}},src,attributes));
}
if(symbol=="GdipDrawImageFX"){
    // (image, source, xForm, effect, attributes, srcUnit)：來源矩形（預設整張）經 xForm 變換後即目的位置；效果物件尚未建立任何實例。
    const auto& b=image_of(arg(1));if(arg(4))return finish(invalid_parameter);const auto* attributes=arg(5)?&attributes_of(arg(5)):nullptr;
    auto src=arg(2)?rect_array(arg(2),1,false)[0]:std::array<double,4>{0,0,static_cast<double>(b.width),static_cast<double>(b.height)};src=source_pixels(b,src,u32(6));
    auto points=rect_points(src[0],src[1],src[2],src[3]);if(arg(3)){const auto& m=matrix_of(arg(3));for(auto& p:points){const auto q=gui_point(m,p[0],p[1]);p={q[0],q[1]};}}
    return finish(draw_image(arg(0),b,points,src,attributes));
}
// ---- CachedBitmap：建立時複製像素；繪製只允許平移的世界變換 ----
if(symbol=="GdipCreateCachedBitmap"){
    const auto& b=image_of(arg(0));if(!g.graphics.contains(arg(1)) || !arg(2))return finish(invalid_parameter);
    auto copy=make_bitmap(b.width,b.height,pargb32,{});for(std::uint32_t y=0;y<b.height;++y)for(std::uint32_t x=0;x<b.width;++x)write_pixel(copy,x,y,read_pixel(b,x,y));
    const auto handle=new_object(arg(2));if(!handle){(void)free_heap(memory,copy.scan0,state);return finish(out_of_memory);}g.cached[handle]=std::move(copy);memory.store(GuestAddress{arg(2)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipDeleteCachedBitmap"){const auto found=g.cached.find(arg(0));if(found==g.cached.end())return finish(invalid_parameter);(void)free_heap(memory,found->second.scan0,state);g.cached.erase(found);release_handle(arg(0));return finish(ok);}
if(symbol=="GdipDrawCachedBitmap"){
    const auto found=g.cached.find(arg(1));if(found==g.cached.end() || !g.graphics.contains(arg(0)))return finish(invalid_parameter);
    const auto& world=graphics_of(arg(0)).world;if(world[0]!=1 || world[1]!=0 || world[2]!=0 || world[3]!=1)return finish(wrong_state);
    const auto& b=found->second;const auto size=physical_size(arg(0),b,b.width,b.height);
    return finish(draw_image(arg(0),b,rect_points(s32(2),s32(3),size[0],size[1]),{0,0,static_cast<double>(b.width),static_cast<double>(b.height)},nullptr));
}
