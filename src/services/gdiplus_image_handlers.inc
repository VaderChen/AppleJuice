// GDI+ 影像家族：影像資訊、解析度、ImageFormat、畫格、調色盤、旋轉翻轉、區域複製與格式轉換、色階統計、縮圖、
// GDI DIB／HICON／資源點陣圖互轉與屬性項目。像素編解碼共用 gdiplus_pixels.hpp 與 read_pixel／write_pixel。
const auto image_of=[&](std::uint64_t handle)->GdiplusState::Bitmap&{auto* b=bitmap_of(handle);if(!b)throw std::invalid_argument("GDI+ 影像無效");return *b;};
// 新的自有點陣圖（像素清零）；索引色格式帶入指定或預設調色盤。
const auto make_bitmap=[&](std::uint32_t width,std::uint32_t height,std::uint32_t format,std::vector<std::uint32_t> palette)->GdiplusState::Bitmap{
    if(!width || !height || !gdiplus_pixel::supported(format))throw std::invalid_argument("GDI+ 點陣圖參數無效");
    GdiplusState::Bitmap b{width,height,format,gdiplus_pixel::stride(format,width),0,true};
    const auto size=static_cast<std::uint64_t>(b.stride)*height;if(size>stream_limit)throw std::bad_alloc();
    b.scan0=allocate_heap(memory,size);if(!b.scan0)throw std::bad_alloc();memory.fill(GuestAddress{b.scan0},0,static_cast<std::size_t>(size));
    b.palette=gdiplus_pixel::indexed(format)?(palette.empty()?gdiplus_pixel::default_palette(format):std::move(palette)):std::vector<std::uint32_t>{};return b;
};
// 影像 Graphics 仍附著時不可更換像素緩衝。
const auto attached=[&](std::uint64_t bitmap){return std::any_of(g.image_graphics.begin(),g.image_graphics.end(),[&](const auto& item){return item.second.bitmap==bitmap;});};
const auto register_bitmap=[&](GdiplusState::Bitmap b,std::uint64_t output)->std::uint32_t{
    const auto handle=new_handle();if(!handle){(void)free_heap(memory,b.scan0,state);return out_of_memory;}
    g.bitmaps[handle]=std::move(b);memory.store(GuestAddress{output},pointer_bits,handle);return ok;
};
const auto store_guid=[&](std::uint64_t at,std::uint32_t data1,std::uint16_t data2,std::uint16_t data3,std::array<std::uint8_t,8> data4){
    memory.store(GuestAddress{at},32,data1);memory.store(GuestAddress{at+4},16,data2);memory.store(GuestAddress{at+6},16,data3);memory.write(GuestAddress{at+8},std::span<const std::uint8_t>(data4));};
// 畫格維度：GIF 為 FrameDimensionTime，其餘為 FrameDimensionPage。
const auto frame_dimension=[&](const GdiplusState::Bitmap& b){
    return b.raw==0xb0?std::tuple<std::uint32_t,std::uint16_t,std::uint16_t,std::array<std::uint8_t,8>>{0x6aedbd6dU,0x3fb5,0x418a,{0x83,0xa6,0x7f,0x45,0x22,0x9d,0xc8,0x72}}
                      :std::tuple<std::uint32_t,std::uint16_t,std::uint16_t,std::array<std::uint8_t,8>>{0x7462dc86U,0x6180,0x4c7e,{0x8e,0x3f,0xee,0x73,0x33,0xa7,0xa4,0x83}};};
const auto same_dimension=[&](const GdiplusState::Bitmap& b,std::uint64_t at){
    const auto [d1,d2,d3,d4]=frame_dimension(b);std::array<std::uint8_t,16> got{};memory.read(GuestAddress{at},got);
    std::array<std::uint8_t,16> want{};for(unsigned i=0;i<4;++i)want[i]=static_cast<std::uint8_t>(d1>>(8*i));want[4]=static_cast<std::uint8_t>(d2);want[5]=static_cast<std::uint8_t>(d2>>8);
    want[6]=static_cast<std::uint8_t>(d3);want[7]=static_cast<std::uint8_t>(d3>>8);std::copy(d4.begin(),d4.end(),want.begin()+8);return got==want;};
const auto all_pixels=[&](const GdiplusState::Bitmap& b){std::vector<std::uint32_t> out(static_cast<std::size_t>(b.width)*b.height);
    for(std::uint32_t y=0;y<b.height;++y)for(std::uint32_t x=0;x<b.width;++x)out[std::size_t{y}*b.width+x]=read_pixel(b,x,y);return out;};
const auto read_palette=[&](std::uint64_t at,std::size_t limit)->std::pair<std::uint32_t,std::vector<std::uint32_t>>{
    const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32)),count=static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32));
    if(count>limit)throw std::invalid_argument("GDI+ 調色盤過大");std::vector<std::uint32_t> colors(count);for(std::size_t i=0;i<count;++i)colors[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{at+8+i*4},32));return {flags,colors};};
// ---- 影像資訊 ----
if(symbol=="GdipGetImageBounds"){const auto& b=image_of(arg(0));if(!arg(1) || !arg(2))return finish(invalid_parameter);store_rect(arg(1),0,0,b.width,b.height,false);memory.store(GuestAddress{arg(2)},32,2);return finish(ok);}
if(symbol=="GdipGetImageDimension"){const auto& b=image_of(arg(0));if(!arg(1) || !arg(2))return finish(invalid_parameter);store_real(arg(1),b.width);store_real(arg(2),b.height);return finish(ok);}
if(symbol=="GdipGetImageFlags"){
    // ImageFlagsHasAlpha 0x2 依格式或調色盤判斷，其餘沿用解碼時的旗標。
    const auto& b=image_of(arg(0));if(!arg(1))return finish(invalid_parameter);
    const bool alpha=gdiplus_pixel::alpha(b.format) || std::any_of(b.palette.begin(),b.palette.end(),[](auto c){return (c>>24)!=255;});
    memory.store(GuestAddress{arg(1)},32,b.image_flags|(alpha?2U:0U));return finish(ok);
}
if(symbol=="GdipGetImageHorizontalResolution" || symbol=="GdipGetImageVerticalResolution"){
    const auto& b=image_of(arg(0));if(!arg(1))return finish(invalid_parameter);store_real(arg(1),symbol=="GdipGetImageHorizontalResolution"?b.dpi_x:b.dpi_y);return finish(ok);}
if(symbol=="GdipBitmapSetResolution"){auto& b=image_of(arg(0));const auto x=real(1),y=real(2);if(!(x>0) || !(y>0))return finish(invalid_parameter);b.dpi_x=x;b.dpi_y=y;return finish(ok);}
if(symbol=="GdipGetImageRawFormat"){const auto& b=image_of(arg(0));if(!arg(1))return finish(invalid_parameter);store_guid(arg(1),0xb96b3c00U|b.raw,0x0728,0x11d3,{0x9d,0x7b,0x00,0x00,0xf8,0x1e,0xf3,0x2e});return finish(ok);}
if(symbol=="GdipGetImageType"){image_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,1);return finish(ok);}
if(symbol=="GdipImageForceValidation" || symbol=="GdipImageSetAbort"){image_of(arg(0));return finish(ok);}
if(symbol=="GdipGraphicsSetAbort"){if(!g.graphics.contains(arg(0)))return finish(invalid_parameter);return finish(ok);}
if(symbol=="GdipGetNearestColor"){
    // 32 位元表面可表示所有顏色，原值不變。
    if(!g.graphics.contains(arg(0)) || !arg(1))return finish(invalid_parameter);memory.check(GuestAddress{arg(1)},4,Access::Write);return finish(ok);}
// ---- 畫格 ----
if(symbol=="GdipImageGetFrameDimensionsCount"){image_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,1);return finish(ok);}
if(symbol=="GdipImageGetFrameDimensionsList"){
    const auto& b=image_of(arg(0));if(!arg(1) || u32(2)!=1)return finish(invalid_parameter);const auto [d1,d2,d3,d4]=frame_dimension(b);store_guid(arg(1),d1,d2,d3,d4);return finish(ok);}
if(symbol=="GdipImageGetFrameCount"){
    const auto& b=image_of(arg(0));if(!arg(1) || !arg(2) || !same_dimension(b,arg(1)))return finish(invalid_parameter);memory.store(GuestAddress{arg(2)},32,1);return finish(ok);}
if(symbol=="GdipImageSelectActiveFrame"){const auto& b=image_of(arg(0));if(!arg(1) || !same_dimension(b,arg(1)) || u32(2)!=0)return finish(invalid_parameter);return finish(ok);}
// ---- 調色盤 ----
if(symbol=="GdipSetImagePalette"){
    auto& b=image_of(arg(0));if(!arg(1))return finish(invalid_parameter);
    const auto limit=gdiplus_pixel::indexed(b.format)?std::size_t{1}<<gdiplus_pixel::bits(b.format):256;auto [flags,colors]=read_palette(arg(1),limit);
    if(colors.empty())return finish(invalid_parameter);b.palette_flags=flags;b.palette=std::move(colors);return finish(ok);
}
if(symbol=="GdipInitializePalette"){
    // (palette, type, optimalColors, useTransparentColor, bitmap)：Count 欄位為呼叫端提供的容量。
    if(!arg(0))return finish(invalid_parameter);const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+4},32));
    const auto type=u32(1);const bool transparent=u32(3)!=0;std::vector<std::uint32_t> colors;std::uint32_t flags=0;
    if(type==1){if(!arg(4) || s32(2)<=0 || static_cast<std::uint32_t>(s32(2))>capacity || s32(2)>256)return finish(invalid_parameter);colors=gdiplus_pixel::optimal_palette(all_pixels(image_of(arg(4))),static_cast<std::size_t>(s32(2)),transparent);}
    else if(type>=2 && type<=9){colors=gdiplus_pixel::fixed_palette(type);flags=type==2?2U:4U;if(transparent && !colors.empty())colors.front()=0;}
    else return finish(invalid_parameter);
    if(colors.size()>capacity)return finish(invalid_parameter);if(transparent)flags|=1U;
    memory.store(GuestAddress{arg(0)},32,flags);memory.store(GuestAddress{arg(0)+4},32,colors.size());
    for(std::size_t i=0;i<colors.size();++i)memory.store(GuestAddress{arg(0)+8+i*4},32,colors[i]);return finish(ok);
}
if(symbol=="GdipCreateHalftonePalette")return finish(nested_service(state,memory,"gdi32.dll","CreateHalftonePalette",{0}));
// ---- 旋轉與翻轉 ----
if(symbol=="GdipImageRotateFlip"){
    // RotateFlipType：0～3 為旋轉 0／90／180／270 度，4～7 再加水平翻轉；寬高互換時改配置新的像素緩衝。
    auto& b=image_of(arg(0));const auto type=u32(1);if(type>7)return finish(invalid_parameter);if(b.lock)return finish(wrong_state);if(!type)return finish(ok);if(attached(arg(0)))return finish(object_busy);
    const auto source=all_pixels(b);const auto quarter=type&3U;const bool flip=type>=4,swap=quarter&1U;
    const auto w=b.width,h=b.height,out_w=swap?h:w,out_h=swap?w:h;
    auto target=swap?make_bitmap(out_w,out_h,b.format,b.palette):b;
    for(std::uint32_t y=0;y<out_h;++y)for(std::uint32_t x=0;x<out_w;++x){
        const auto fx=flip?out_w-1-x:x;std::uint32_t sx=0,sy=0;
        if(quarter==0){sx=fx;sy=y;}else if(quarter==1){sx=y;sy=h-1-fx;}else if(quarter==2){sx=w-1-fx;sy=h-1-y;}else {sx=w-1-y;sy=fx;}
        write_pixel(target,x,y,source[std::size_t{sy}*w+sx]);}
    if(swap){target.palette_flags=b.palette_flags;target.dpi_x=b.dpi_y;target.dpi_y=b.dpi_x;target.raw=b.raw;target.image_flags=b.image_flags;target.properties=b.properties;
        if(b.owned)(void)free_heap(memory,b.scan0,state);b=std::move(target);}
    return finish(ok);
}
// ---- 區域複製、格式轉換、色階 ----
if(symbol=="GdipCloneBitmapArea" || symbol=="GdipCloneBitmapAreaI"){
    // (x, y, w, h, format, source, output)；PixelFormatUndefined（0）沿用來源格式。
    const bool integer=symbol.ends_with('I');const auto& b=image_of(arg(5));if(!arg(6))return finish(invalid_parameter);
    const auto x=coordinate(0,integer),y=coordinate(1,integer),w=coordinate(2,integer),h=coordinate(3,integer);const auto format=u32(4)?u32(4):b.format;
    const auto left=static_cast<std::int64_t>(std::floor(x)),top=static_cast<std::int64_t>(std::floor(y)),width=static_cast<std::int64_t>(std::floor(w)),height=static_cast<std::int64_t>(std::floor(h));
    if(left<0 || top<0 || width<=0 || height<=0 || left+width>b.width || top+height>b.height || !gdiplus_pixel::supported(format))return finish(invalid_parameter);
    auto copy=make_bitmap(static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),format,format==b.format?b.palette:std::vector<std::uint32_t>{});
    copy.dpi_x=b.dpi_x;copy.dpi_y=b.dpi_y;copy.palette_flags=format==b.format?b.palette_flags:0;
    for(std::int64_t j=0;j<height;++j)for(std::int64_t i=0;i<width;++i)write_pixel(copy,static_cast<std::uint32_t>(i),static_cast<std::uint32_t>(j),read_pixel(b,static_cast<std::uint32_t>(left+i),static_cast<std::uint32_t>(top+j)));
    return finish(register_bitmap(std::move(copy),arg(6)));
}
if(symbol=="GdipBitmapConvertFormat"){
    // (bitmap, format, dither, paletteType, palette, alphaThresholdPercent)：索引色須提供調色盤；DitherType 2～8 以有序抖動、9 以誤差擴散，
    // alpha 低於門檻的像素轉成透明色。
    auto& b=image_of(arg(0));const auto format=u32(1),dither=u32(2);if(b.lock)return finish(wrong_state);if(attached(arg(0)))return finish(object_busy);
    if(!gdiplus_pixel::supported(format) || dither>9)return finish(invalid_parameter);
    std::vector<std::uint32_t> palette;std::uint32_t palette_flags=0;
    if(gdiplus_pixel::indexed(format)){if(!arg(4))return finish(invalid_parameter);auto [flags,colors]=read_palette(arg(4),std::size_t{1}<<gdiplus_pixel::bits(format));if(colors.empty())return finish(invalid_parameter);palette=std::move(colors);palette_flags=flags;}
    const auto threshold=real(5);auto pixels=all_pixels(b);auto target=make_bitmap(b.width,b.height,format,palette);target.palette_flags=palette_flags;
    if(gdiplus_pixel::indexed(format))for(auto& c:pixels){if((c>>24)*100.0<threshold*255)c=0;else c|=0xff000000U;}
    const auto bayer=[](unsigned size,unsigned x,unsigned y){unsigned v=0;for(unsigned bit=0;(1U<<bit)<size;++bit){const auto a=(x>>bit)&1U,c=(y>>bit)&1U;v=(v<<2)|((a^c)<<1)|c;}return v;};
    std::vector<std::array<double,3>> error(dither==9?static_cast<std::size_t>(b.width)*2:0);
    for(std::uint32_t y=0;y<b.height;++y){
        if(dither==9){std::rotate(error.begin(),error.begin()+b.width,error.end());std::fill(error.begin()+b.width,error.end(),std::array<double,3>{});}
        for(std::uint32_t x=0;x<b.width;++x){
            auto c=pixels[std::size_t{y}*b.width+x];
            if(gdiplus_pixel::indexed(format) && dither>=2 && (c>>24)){
                std::array<double,3> v{};for(unsigned k=0;k<3;++k)v[k]=(c>>(8*k))&255U;
                if(dither==9)for(unsigned k=0;k<3;++k)v[k]+=error[x][k];
                else {const unsigned size=dither==2 || dither==5 || dither==7?4:dither==4?16:8;const auto t=(bayer(size,x%size,y%size)+0.5)/(size*size)-0.5;for(auto& e:v)e+=t*51;}
                std::uint32_t adjusted=0xff000000U;for(unsigned k=0;k<3;++k)adjusted|=static_cast<std::uint32_t>(std::clamp(std::lround(v[k]),0L,255L))<<(8*k);
                const auto chosen=palette[gdiplus_pixel::nearest(palette,adjusted)];
                if(dither==9){const auto spread=[&](std::size_t at,double weight){for(unsigned k=0;k<3;++k)error[at][k]+=(v[k]-static_cast<double>((chosen>>(8*k))&255U))*weight;};
                    if(x+1<b.width)spread(x+1,7.0/16);if(x>0)spread(b.width+x-1,3.0/16);spread(b.width+x,5.0/16);if(x+1<b.width)spread(b.width+x+1,1.0/16);}
                c=chosen;
            }
            write_pixel(target,x,y,c);
        }
    }
    target.dpi_x=b.dpi_x;target.dpi_y=b.dpi_y;target.raw=b.raw;target.image_flags=b.image_flags;target.properties=b.properties;
    if(b.owned)(void)free_heap(memory,b.scan0,state);b=std::move(target);return finish(ok);
}
if(symbol=="GdipBitmapGetHistogramSize"){if(u32(0)>7 || !arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,256);return finish(ok);}
if(symbol=="GdipBitmapGetHistogram"){
    // HistogramFormat：ARGB 0（A、R、G、B）、PARGB 1、RGB 2、Gray 3、B 4、G 5、R 6、A 7。
    const auto& b=image_of(arg(0));const auto format=u32(1);if(format>7 || u32(2)!=256)return finish(invalid_parameter);
    const unsigned channels=format<=1?4U:format==2?3U:1U;for(unsigned i=0;i<channels;++i)if(!arg(3+i))return finish(invalid_parameter);
    std::vector<std::array<std::uint32_t,256>> counts(channels);for(auto& c:counts)c.fill(0);
    for(const auto pixel:all_pixels(b)){
        const auto c=format==1?premultiply(pixel):pixel;const unsigned a=c>>24,r=(c>>16)&255U,gch=(c>>8)&255U,bl=c&255U;
        if(format<=1){++counts[0][a];++counts[1][r];++counts[2][gch];++counts[3][bl];}
        else if(format==2){++counts[0][r];++counts[1][gch];++counts[2][bl];}
        else ++counts[0][format==3?(r*299+gch*587+bl*114+500)/1000:format==4?bl:format==5?gch:format==6?r:a];
    }
    for(unsigned i=0;i<channels;++i){memory.check(GuestAddress{arg(3+i)},1024,Access::Write);for(unsigned k=0;k<256;++k)memory.store(GuestAddress{arg(3+i)+k*4},32,counts[i][k]);}
    return finish(ok);
}
if(symbol=="GdipGetImageThumbnail"){
    // (image, width, height, thumbnail, callback, data)：寬高為 0 時取 120；以面積平均縮放成 32bppPARGB。
    const auto& b=image_of(arg(0));if(!arg(3))return finish(invalid_parameter);const auto w=u32(1)?u32(1):120U,h=u32(2)?u32(2):120U;
    if(w>65536 || h>65536)return finish(invalid_parameter);const auto source=all_pixels(b);auto thumb=make_bitmap(w,h,pargb32,{});
    for(std::uint32_t y=0;y<h;++y)for(std::uint32_t x=0;x<w;++x){
        const auto x0=std::uint64_t{x}*b.width/w,x1=std::max<std::uint64_t>(x0+1,std::uint64_t{x+1}*b.width/w),y0=std::uint64_t{y}*b.height/h,y1=std::max<std::uint64_t>(y0+1,std::uint64_t{y+1}*b.height/h);
        std::array<std::uint64_t,4> sum{};for(auto sy=y0;sy<y1;++sy)for(auto sx=x0;sx<x1;++sx){const auto c=premultiply(source[sy*b.width+sx]);for(unsigned k=0;k<4;++k)sum[k]+=(c>>(8*k))&255U;}
        const auto n=(x1-x0)*(y1-y0);std::uint32_t c=0;for(unsigned k=0;k<4;++k)c|=static_cast<std::uint32_t>((sum[k]+n/2)/n)<<(8*k);write_pixel(thumb,x,y,unpremultiply(c));}
    return finish(register_bitmap(std::move(thumb),arg(3)));
}
// ---- 與 GDI 物件互轉 ----
if(symbol=="GdipCreateBitmapFromGraphics"){
    if(s32(0)<=0 || s32(1)<=0 || !g.graphics.contains(arg(2)) || !arg(3))return finish(invalid_parameter);
    return finish(register_bitmap(make_bitmap(u32(0),u32(1),pargb32,{}),arg(3)));
}
if(symbol=="GdipCreateBitmapFromGdiDib"){
    // BITMAPINFO＋像素：1／4／8 位元帶色表、16 位元 555（BI_BITFIELDS 綠色遮罩 0x7e0 時為 565）、24 與 32 位元；高度為正時由下而上。
    if(!arg(0) || !arg(1) || !arg(2))return finish(invalid_parameter);
    const auto header=arg(0);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{header},32));
    const auto width=static_cast<std::int32_t>(memory.load(GuestAddress{header+4},32)),height=static_cast<std::int32_t>(memory.load(GuestAddress{header+8},32));
    const auto bit_count=static_cast<unsigned>(memory.load(GuestAddress{header+14},16)),compression=static_cast<std::uint32_t>(memory.load(GuestAddress{header+16},32));
    if(size<40 || width<=0 || !height || (compression!=0 && compression!=3))return finish(invalid_parameter);
    std::uint32_t format=0;
    switch(bit_count){case 1:format=gdiplus_pixel::indexed1;break;case 4:format=gdiplus_pixel::indexed4;break;case 8:format=gdiplus_pixel::indexed8;break;
        case 16:format=compression==3 && memory.load(GuestAddress{header+size+4},32)==0x7e0?gdiplus_pixel::rgb565:gdiplus_pixel::rgb555;break;case 24:format=rgb24;break;case 32:format=rgb32;break;default:return finish(invalid_parameter);}
    std::vector<std::uint32_t> palette;
    if(bit_count<=8){auto used=static_cast<std::uint32_t>(memory.load(GuestAddress{header+32},32));if(!used || used>(1U<<bit_count))used=1U<<bit_count;
        for(std::uint32_t i=0;i<used;++i)palette.push_back(0xff000000U|static_cast<std::uint32_t>(memory.load(GuestAddress{header+size+i*4},32)&0xffffffU));}
    const auto rows=static_cast<std::uint32_t>(std::abs(height));auto b=make_bitmap(static_cast<std::uint32_t>(width),rows,format,palette);
    const auto source_stride=gdiplus_pixel::stride(format,static_cast<std::uint32_t>(width));std::vector<std::uint8_t> row(static_cast<std::size_t>(source_stride));
    for(std::uint32_t y=0;y<rows;++y){const auto from=height>0?rows-1-y:y;memory.read(GuestAddress{arg(1)+static_cast<std::uint64_t>(from)*static_cast<std::uint64_t>(source_stride)},row);memory.write(GuestAddress{row_of(b,y)},row);}
    return finish(register_bitmap(std::move(b),arg(2)));
}
if(symbol=="GdipCreateBitmapFromHICON"){
    // 圖示的 rgba 為 COLORREF＋alpha（已依遮罩或 alpha 通道合成）。
    const auto icon=gui_.icons.find(arg(0));if(icon==gui_.icons.end() || !arg(1))return finish(invalid_parameter);
    return finish(import_pixels(icon->second.width,icon->second.height,icon->second.rgba,argb32,arg(1)));
}
if(symbol=="GdipCreateHICONFromBitmap"){
    // 與 CreateIconIndirect 相同的圖示資料：alpha 0 的像素在遮罩中為透明；尺寸上限 256。
    const auto& b=image_of(arg(0));if(!arg(1))return finish(invalid_parameter);if(b.width>256 || b.height>256)return finish(7);
    GuiIcon icon;icon.is_icon=true;icon.width=b.width;icon.height=b.height;icon.hotspot_x=b.width/2;icon.hotspot_y=b.height/2;icon.alpha=true;
    const auto count=static_cast<std::size_t>(b.width)*b.height;icon.rgba.resize(count);icon.color_pixels.resize(count);icon.mask_pixels.resize(count);
    for(std::uint32_t y=0;y<b.height;++y)for(std::uint32_t x=0;x<b.width;++x){const auto c=read_pixel(b,x,y);const auto i=std::size_t{y}*b.width+x;
        const auto colorref=((c>>16)&255U)|(c&0xff00U)|((c&255U)<<16);icon.rgba[i]=colorref|(c&0xff000000U);const auto pm=premultiply(c);icon.color_pixels[i]=((pm>>16)&255U)|(pm&0xff00U)|((pm&255U)<<16)|(pm&0xff000000U);icon.mask_pixels[i]=(c>>24)?0:1;}
    finish_icon(icon);memory.store(GuestAddress{arg(1)},pointer_bits,store_gui_icon(std::move(icon)));return finish(ok);
}
if(symbol=="GdipCreateBitmapFromResource"){
    // RT_BITMAP 資源經 LoadImageW 取得 DIB section，再轉成 32bppRGB 點陣圖。
    if(!arg(1) || !arg(2))return finish(invalid_parameter);
    const auto hbitmap=nested_service(state,memory,"user32.dll","LoadImageW",{arg(0),arg(1),0,0,0,0x2000});if(!hbitmap || !gui_.bitmaps.contains(hbitmap))return finish(invalid_parameter);
    sync_raster_bitmap(memory,hbitmap);const auto pixels=gui_.bitmaps.at(hbitmap).pixels;const auto width=gui_.bitmaps.at(hbitmap).width,height=gui_.bitmaps.at(hbitmap).height;
    (void)nested_service(state,memory,"gdi32.dll","DeleteObject",{hbitmap});return finish(import_pixels(width,height,pixels,rgb32,arg(2)));
}
// ---- 屬性項目：PropertyItem 為 (id, length, type, value*)，x86 16 位元組、x64 24 位元組，值緊接在結構之後 ----
{
    const unsigned item_size=x86_?16U:24U,value_at=x86_?12U:16U;
    const auto find_property=[](GdiplusState::Bitmap& b,std::uint32_t id){return std::find_if(b.properties.begin(),b.properties.end(),[&](const auto& p){return p.id==id;});};
    const auto store_item=[&](std::uint64_t at,std::uint64_t value,const GdiplusState::Property& p){
        memory.store(GuestAddress{at},32,p.id);memory.store(GuestAddress{at+4},32,p.value.size());memory.store(GuestAddress{at+8},16,p.type);memory.store(GuestAddress{at+10},16,0);
        if(!x86_)memory.store(GuestAddress{at+12},32,0);memory.store(GuestAddress{at+value_at},pointer_bits,p.value.empty()?0:value);if(!p.value.empty())memory.write(GuestAddress{value},std::span<const std::uint8_t>(p.value));};
    const auto total=[&](const GdiplusState::Bitmap& b){std::uint64_t n=0;for(const auto& p:b.properties)n+=item_size+p.value.size();return n;};
    if(symbol=="GdipGetPropertyCount"){const auto& b=image_of(arg(0));if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,b.properties.size());return finish(ok);}
    if(symbol=="GdipGetPropertyIdList"){
        const auto& b=image_of(arg(0));if(!arg(2) || u32(1)!=b.properties.size())return finish(invalid_parameter);
        for(std::size_t i=0;i<b.properties.size();++i)memory.store(GuestAddress{arg(2)+i*4},32,b.properties[i].id);return finish(ok);}
    if(symbol=="GdipGetPropertyItemSize"){auto& b=image_of(arg(0));if(!arg(2))return finish(invalid_parameter);const auto p=find_property(b,u32(1));if(p==b.properties.end())return finish(property_not_found);
        memory.store(GuestAddress{arg(2)},32,item_size+p->value.size());return finish(ok);}
    if(symbol=="GdipGetPropertyItem"){auto& b=image_of(arg(0));if(!arg(3))return finish(invalid_parameter);const auto p=find_property(b,u32(1));if(p==b.properties.end())return finish(property_not_found);
        if(u32(2)<item_size+p->value.size())return finish(invalid_parameter);store_item(arg(3),arg(3)+item_size,*p);return finish(ok);}
    if(symbol=="GdipGetPropertySize"){const auto& b=image_of(arg(0));if(!arg(1) || !arg(2))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,total(b));memory.store(GuestAddress{arg(2)},32,b.properties.size());return finish(ok);}
    if(symbol=="GdipGetAllPropertyItems"){
        const auto& b=image_of(arg(0));if(!arg(3) || u32(1)!=total(b) || u32(2)!=b.properties.size())return finish(invalid_parameter);
        auto value=arg(3)+b.properties.size()*item_size;for(std::size_t i=0;i<b.properties.size();++i){store_item(arg(3)+i*item_size,value,b.properties[i]);value+=b.properties[i].value.size();}return finish(ok);}
    if(symbol=="GdipRemovePropertyItem"){auto& b=image_of(arg(0));const auto p=find_property(b,u32(1));if(p==b.properties.end())return finish(property_not_found);b.properties.erase(p);return finish(ok);}
    if(symbol=="GdipSetPropertyItem"){
        auto& b=image_of(arg(0));if(!arg(1))return finish(invalid_parameter);GdiplusState::Property item;item.id=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
        const auto length=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+4},32));item.type=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(1)+8},16));
        const auto value=memory.load(GuestAddress{arg(1)+value_at},pointer_bits);if(length>16*1024*1024 || (length && !value))return finish(invalid_parameter);
        item.value.resize(length);if(length)memory.read(GuestAddress{value},item.value);
        if(const auto p=find_property(b,item.id);p!=b.properties.end())*p=std::move(item);else b.properties.push_back(std::move(item));return finish(ok);}
}
