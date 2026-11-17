// GDI+ 1.1 影像效果（由 windows_gdiplus.cpp 引入，沿用點陣圖像素讀寫、影像 Graphics 的 DIB 同步與 store_bitmap）。
// GdipCreateEffect 的 GUID 以值傳遞（aggregate 參數）；參數經 GdipSetEffectParameters 設定並檢查範圍後才能套用。
// GdipBitmapApplyEffect 就地處理 ROI；GdipBitmapCreateApplyEffect 輸出新的 32bppARGB 點陣圖（模糊且 expandEdge 時四周外擴半徑）。
// 逐通道效果的 auxData 是 B、G、R、A 各 256 項的對照表（以 GdipAlloc 配置，呼叫端以 GdipFree 釋放）。
if(symbol=="GdipCreateEffect"){
    if(!arg(1))return finish(invalid_parameter);gdiplus_effect::Guid id{};memory.read(GuestAddress{arg(0)},id);
    memory.store(GuestAddress{arg(1)},pointer_bits,0);const auto kind=gdiplus_effect::kind(id);if(!kind)return finish(invalid_parameter);
    const auto handle=new_object(arg(1));if(!handle)return finish(out_of_memory);
    g.effects[handle]={*kind,std::vector<std::uint8_t>(gdiplus_effect::parameter_size(*kind,x86_)),{},false};memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);
}
if(symbol=="GdipDeleteEffect"){if(!g.effects.erase(arg(0)))return finish(invalid_parameter);release_handle(arg(0));return finish(ok);}
if(symbol=="GdipGetEffectParameterSize" || symbol=="GdipSetEffectParameters" || symbol=="GdipGetEffectParameters"){
    const auto found=g.effects.find(arg(0));if(found==g.effects.end())return finish(invalid_parameter);auto& effect=found->second;
    const auto size=static_cast<std::uint32_t>(effect.parameters.size());
    if(symbol=="GdipGetEffectParameterSize"){if(!arg(1))return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,size);return finish(ok);}
    if(symbol=="GdipSetEffectParameters"){
        if(!arg(1) || u32(2)!=size)return finish(invalid_parameter);
        std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{arg(1)},bytes);if(!gdiplus_effect::valid(effect.kind,bytes))return finish(invalid_parameter);
        std::vector<std::array<std::int32_t,4>> areas;
        if(effect.kind==gdiplus_effect::Kind::RedEye){
            // RedEyeCorrectionParams：區域數與 RECT 陣列指標；複製區域，呼叫端之後可以釋放陣列。
            const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));const auto rects=memory.load(GuestAddress{arg(1)+(x86_?4U:8U)},pointer_bits);
            if(count>1048576 || (count && !rects))return finish(invalid_parameter);
            for(std::uint32_t i=0;i<count;++i){std::array<std::int32_t,4> rect{};for(unsigned c=0;c<4;++c)rect[c]=static_cast<std::int32_t>(memory.load(GuestAddress{rects+i*16U+c*4U},32));areas.push_back(rect);}
        }
        effect.parameters=std::move(bytes);effect.areas=std::move(areas);effect.configured=true;return finish(ok);
    }
    if(!arg(1) || !arg(2))return finish(invalid_parameter);
    if(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32))<size)return finish(invalid_parameter);
    memory.write(GuestAddress{arg(2)},effect.parameters);memory.store(GuestAddress{arg(1)},32,size);return finish(ok);
}
if(symbol=="GdipBitmapApplyEffect" || symbol=="GdipBitmapCreateApplyEffect"){
    const bool create=symbol=="GdipBitmapCreateApplyEffect";
    // Apply(bitmap, effect, roi, useAux, auxData, auxSize)；CreateApply(inputs, count, effect, roi, outputRect, output, useAux, auxData, auxSize)。
    const auto aux_at=create?7U:4U;const bool use_aux=u32(create?6:3)!=0;
    if(arg(aux_at))memory.store(GuestAddress{arg(aux_at)},pointer_bits,0);if(arg(aux_at+1))memory.store(GuestAddress{arg(aux_at+1)},32,0);
    std::uint64_t source=arg(0);
    if(create){if(!arg(0) || s32(1)!=1 || !arg(5))return finish(invalid_parameter);memory.store(GuestAddress{arg(5)},pointer_bits,0);source=memory.load(GuestAddress{arg(0)},pointer_bits);
        for(auto& [handle,target]:g.image_graphics){(void)handle;if(target.bitmap==source && (target.dirty || target.held))copy_back(target);}}
    auto* b=bitmap_of(source);const auto found=g.effects.find(arg(create?2:1));
    if(!b || found==g.effects.end() || !found->second.configured)return finish(invalid_parameter);if(b->lock)return finish(object_busy);
    const auto& effect=found->second;
    std::int32_t left=0,top=0,right=static_cast<std::int32_t>(b->width),bottom=static_cast<std::int32_t>(b->height);
    if(const auto roi=arg(create?3:2)){
        const auto read=[&](unsigned i){return static_cast<std::int32_t>(memory.load(GuestAddress{roi+i*4U},32));};
        left=std::max(left,read(0));top=std::max(top,read(1));right=std::min(right,read(2));bottom=std::min(bottom,read(3));
    }
    if(left>=right || top>=bottom)return finish(invalid_parameter);
    std::vector<std::uint32_t> pixels;std::optional<gdiplus_effect::Table> lut;
    if(!create){
        pixels.resize(std::size_t{b->width}*b->height);
        for(std::uint32_t y=0;y<b->height;++y)for(std::uint32_t x=0;x<b->width;++x)pixels[std::size_t{y}*b->width+x]=read_pixel(*b,x,y);
        lut=gdiplus_effect::apply(effect.kind,effect.parameters,effect.areas,pixels,b->width,{left,top,right-left,bottom-top});
        for(auto y=top;y<bottom;++y)for(auto x=left;x<right;++x)write_pixel(*b,static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y),pixels[static_cast<std::size_t>(y)*b->width+static_cast<std::size_t>(x)]);
        mirror(source,{left,top,right-left,bottom-top});
    }else{
        // 模糊且 expandEdge：輸出四周外擴 ⌈半徑⌉ 的透明邊，讓邊緣能柔化；其他效果輸出與 ROI 同大小。
        const bool expand=effect.kind==gdiplus_effect::Kind::Blur && gdiplus_effect::integer(effect.parameters,4)!=0;
        const auto pad=expand?static_cast<std::int32_t>(std::ceil(gdiplus_effect::real(effect.parameters,0))):0;
        const auto width=static_cast<std::uint32_t>(right-left+2*pad),height=static_cast<std::uint32_t>(bottom-top+2*pad);
        if(std::uint64_t{width}*height>std::uint64_t{1}<<28)return finish(out_of_memory);
        pixels.assign(std::size_t{width}*height,0);
        for(auto y=top;y<bottom;++y)for(auto x=left;x<right;++x)pixels[static_cast<std::size_t>(y-top+pad)*width+static_cast<std::size_t>(x-left+pad)]=read_pixel(*b,static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y));
        lut=gdiplus_effect::apply(effect.kind,effect.parameters,effect.areas,pixels,width,{0,0,static_cast<std::int32_t>(width),static_cast<std::int32_t>(height)});
        std::vector<std::uint8_t> bytes(pixels.size()*4);for(std::size_t i=0;i<pixels.size();++i)for(unsigned c=0;c<4;++c)bytes[i*4+c]=static_cast<std::uint8_t>(pixels[i]>>(c*8));
        if(const auto status=store_bitmap(g,memory,bytes,width,height,arg(5),pointer_bits,[&](auto size){return allocate_heap(memory,size);},release_handle))return finish(status);
        if(arg(4)){const std::array<std::int32_t,4> rect{left-pad,top-pad,right+pad,bottom+pad};for(unsigned c=0;c<4;++c)memory.store(GuestAddress{arg(4)+c*4U},32,static_cast<std::uint32_t>(rect[c]));}
    }
    if(use_aux && lut && arg(aux_at) && arg(aux_at+1)){
        const auto buffer=allocate_heap(memory,lut->size());if(!buffer)return finish(out_of_memory);
        memory.write(GuestAddress{buffer},*lut);memory.store(GuestAddress{arg(aux_at)},pointer_bits,buffer);memory.store(GuestAddress{arg(aux_at+1)},32,lut->size());
    }
    return finish(ok);
}
