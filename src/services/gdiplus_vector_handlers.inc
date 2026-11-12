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
    const auto color=symbol=="GdipCloneBrush"?g.brushes.at(arg(0)):u32(0);const auto handle=new_handle();if(!handle)return finish(out_of_memory);
    g.brushes[handle]=color;memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipDeleteBrush"){
    if(!g.brushes.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);
}
if(symbol=="GdipCreatePen1"){
    const auto width=real(1);if(!arg(3) || width<0 || u32(2)>6)return finish(invalid_parameter);
    memory.check(GuestAddress{arg(3)},pointer_bits/8,Access::Write);const auto handle=new_handle();if(!handle)return finish(out_of_memory);
    g.pens[handle]={u32(0),width,u32(2)};memory.store(GuestAddress{arg(3)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipDeletePen"){
    if(!g.pens.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);
}
if(symbol=="GdipSetPenStartCap" || symbol=="GdipSetPenEndCap" || symbol=="GdipSetPenLineJoin"){
    const auto found=g.pens.find(arg(0));if(found==g.pens.end() || u32(1)>3)return finish(invalid_parameter);
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
if(symbol=="GdipCreateBitmapFromHBITMAP"){
    if(!arg(2) || !gui_.bitmaps.contains(arg(0)))return finish(invalid_parameter);
    memory.check(GuestAddress{arg(2)},pointer_bits/8,Access::Write);memory.store(GuestAddress{arg(2)},pointer_bits,0);
    sync_raster_bitmap(memory,arg(0));const auto& source=gui_.bitmaps.at(arg(0));
    const auto size=std::uint64_t{source.width}*source.height*4;if(!size || size>stream_limit)return finish(out_of_memory);
    const auto buffer=allocate_heap(memory,size);if(!buffer)return finish(out_of_memory);
    for(std::size_t i=0;i<source.pixels.size();++i)memory.store(GuestAddress{buffer+i*4},32,((source.pixels[i]&0xff00U)|((source.pixels[i]&255U)<<16)|((source.pixels[i]>>16)&255U)|0xff000000U));
    const auto handle=new_handle();if(!handle){release_handle(buffer);return finish(out_of_memory);}
    g.bitmaps[handle]={source.width,source.height,rgb32,static_cast<std::int64_t>(source.width)*4,buffer,true};
    memory.store(GuestAddress{arg(2)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipFillPath" || symbol=="GdipDrawPath" || symbol=="GdipFillRectangle" || symbol=="GdipDrawRectangle" ||
   symbol=="GdipFillEllipse" || symbol=="GdipDrawEllipse" || symbol=="GdipFillPolygon" || symbol=="GdipDrawLine" || symbol=="GdipDrawLineI"){
    const bool fill=symbol.starts_with("GdipFill");
    const auto graphics=g.graphics.find(arg(0));
    if(graphics==g.graphics.end() || !gui_.contexts.contains(graphics->second) ||
       (fill?!g.brushes.contains(arg(1)):!g.pens.contains(arg(1))))return finish(invalid_parameter);
    auto dc=gui_.contexts.at(graphics->second);GdiplusState::Path path;
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
            if(symbol.ends_with("Ellipse"))add_arc(path,x,y,w,h,0,360);
            else path.points={{x,y,6},{x+w,y,2},{x+w,y+h,2},{x,y+h,2}};
            path.points.back().type|=1;
        }
    }
    if(path.points.empty())return finish(ok);
    const auto color=fill?g.brushes.at(arg(1)):g.pens.at(arg(1)).color;
    if(!fill){
        const auto& pen=g.pens.at(arg(1));const double units[]{1,1,1,96.0/72,96,96.0/300,96.0/25.4};
        const auto width=pen.width==0?1:pen.width*units[pen.unit];
        const auto style=0x200U|(pen.join==1?0x1000U:pen.join==2?0U:0x2000U);
        const auto original=path.points;path.points=native_stroke_path(original,gui_identity,width,style,10);path.fill=1;
        // 不對稱端點在同一輪廓加上 cap；封閉 figure 沒有端點。
        const auto flat=gui_flatten_path(original);std::size_t begin=0;
        const auto cap=[&](const GuiPathPoint& at,const GuiPathPoint& near,unsigned type){
            if(!type)return;const auto dx=at.x-near.x,dy=at.y-near.y,length=std::hypot(dx,dy);if(length==0)return;
            const auto r=width/2,ux=dx/length,uy=dy/length;
            if(type==2){GdiplusState::Path circle;add_arc(circle,at.x-r,at.y-r,width,width,0,360);circle.points.back().type|=1;path.points.insert(path.points.end(),circle.points.begin(),circle.points.end());}
            else {path.points.push_back({at.x-uy*r,at.y+ux*r,6});
                if(type==1){path.points.push_back({at.x+ux*r-uy*r,at.y+uy*r+ux*r,2});path.points.push_back({at.x+ux*r+uy*r,at.y+uy*r-ux*r,2});}
                else path.points.push_back({at.x+ux*r,at.y+uy*r,2});
                path.points.push_back({at.x+uy*r,at.y-ux*r,3});}
        };
        for(std::size_t i=0;i<flat.size();++i){if((flat[i].type&6)==6)begin=i;
            if(i+1==flat.size() || (flat[i+1].type&6)==6){if(i>begin && !(flat[i].type&1)){cap(flat[begin],flat[begin+1],pen.start_cap);cap(flat[i],flat[i-1],pen.end_cap);}}}
    }
    const unsigned scale=g.smoothing[arg(0)]==2 || g.smoothing[arg(0)]==4?4:1;
    const auto transform=gui_device_transform(dc);
    for(auto& point:path.points){const auto mapped=gui_point(transform,point.x,point.y);point.x=mapped[0]*scale;point.y=mapped[1]*scale;}
    auto region=native_path_region(path.points,path.fill+1);if(region.empty())return finish(ok);
    const auto surface=raster_surface(memory,graphics->second);if(!surface)return finish(generic_error);
    auto bounds=gui_region_bounds(region);
    const auto left=std::max(0,static_cast<int>(std::floor(static_cast<double>(bounds[0])/scale)));
    const auto top=std::max(0,static_cast<int>(std::floor(static_cast<double>(bounds[1])/scale)));
    const auto right=std::min(static_cast<int>(surface->width()),static_cast<int>(std::ceil(static_cast<double>(bounds[2])/scale)));
    const auto bottom=std::min(static_cast<int>(surface->height()),static_cast<int>(std::ceil(static_cast<double>(bounds[3])/scale)));
    if(left>=right || top>=bottom)return finish(ok);
    const auto width=static_cast<unsigned>(right-left),height=static_cast<unsigned>(bottom-top);
    if(std::uint64_t{width}*height>stream_limit/4)return finish(out_of_memory);
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(width)*height);
    const auto sample=static_cast<int>(scale);
    for(const auto& rect:region)for(auto y=std::max(rect[1],top*sample);y<std::min(rect[3],bottom*sample);++y)
        for(auto x=std::max(rect[0],left*sample);x<std::min(rect[2],right*sample);++x)
            ++coverage[static_cast<std::size_t>(y/sample-top)*width+static_cast<unsigned>(x/sample-left)];
    std::vector<std::uint32_t> pixels(coverage.size());
    for(std::size_t i=0;i<pixels.size();++i){const auto alpha=((color>>24)*coverage[i]+scale*scale/2)/(scale*scale);const auto argb=premultiply((color&0xffffffU)|(alpha<<24));pixels[i]=(argb&0xff00ff00U)|((argb&255U)<<16)|((argb>>16)&255U);}
    const auto source=make_hardware_raster(width,height);source->upload(pixels,true);
    dc.world=gui_identity;dc.viewport_extent=dc.window_extent={1,1};dc.viewport_origin=dc.window_origin={0,0};
    surface->blit(*source,{left,top,right,bottom},{0,0,static_cast<int>(width),static_cast<int>(height)},dc,0x01ff0000U);
    return finish(ok);
}
