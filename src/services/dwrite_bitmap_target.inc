const auto remove_target_bitmap=[&](std::uint64_t bitmap){
    const auto found_bitmap=gui_.bitmaps.find(bitmap);if(found_bitmap!=gui_.bitmaps.end()){(void)free_heap(memory,found_bitmap->second.dib_address,state);gui_.bitmaps.erase(found_bitmap);}
    rasters_.erase(bitmap);raster_shared_.erase(bitmap);
};
const auto target_bitmap=[&](std::uint32_t width,std::uint32_t height){
    if(width>16384 || height>16384 || static_cast<std::uint64_t>(width)*height>16*1024*1024)return UINT64_C(0);
    return create_dib_bitmap(memory,state,std::max(1U,width),-static_cast<std::int32_t>(std::max(1U,height)),32);
};
if(o.kind==Kind::GdiInterop && method=="CreateBitmapRenderTarget"){
    if(!arg(4))return finish(pointer_error);out(4,0);if(arg(1) && !gui_.contexts.contains(arg(1)))return finish(invalid);
    const auto width=static_cast<std::uint32_t>(arg(2)),height=static_cast<std::uint32_t>(arg(3));const auto bitmap=target_bitmap(width,height);if(!bitmap)return finish(0x8007000eU);
    const auto dc=gui_.next_object++;Object value;value.kind=Kind::BitmapRenderTarget;value.target_dc=dc;value.target_bitmap=bitmap;value.target_size={width,height};
    try {GuiDc context;context.bitmap=bitmap;context.graphics_mode=2;gui_.contexts.emplace(dc,std::move(context));out(4,create(std::move(value)));}
    catch(...){gui_.contexts.erase(dc);remove_target_bitmap(bitmap);throw;}return finish(0);
}
if(o.kind==Kind::BitmapRenderTarget){
    const auto write_float=[&](std::uint64_t at,float value){memory.store(GuestAddress{at},32,std::bit_cast<std::uint32_t>(value));};
    if(method=="GetMemoryDC")return finish(o.target_dc);
    if(method=="GetPixelsPerDip")return float_result(o.target_scale);
    if(method=="SetPixelsPerDip"){const auto scale=floating(1);if(!std::isfinite(scale) || scale<=0)return finish(invalid);o.target_scale=scale;return finish(0);}
    if(method=="GetCurrentTransform"){if(!arg(1))return finish(pointer_error);memory.check(GuestAddress{arg(1)},24,Access::Write);for(unsigned i=0;i<6;++i)write_float(arg(1)+i*4,o.target_transform[i]);return finish(0);}
    if(method=="SetCurrentTransform"){
        std::array<float,6> transform{1,0,0,1,0,0};if(arg(1))for(unsigned i=0;i<6;++i)transform[i]=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+i*4},32)));
        if(std::any_of(transform.begin(),transform.end(),[](float v){return !std::isfinite(v);}))return finish(invalid);o.target_transform=transform;return finish(0);
    }
    if(method=="GetSize"){if(!arg(1))return finish(pointer_error);memory.check(GuestAddress{arg(1)},8,Access::Write);for(unsigned i=0;i<2;++i)memory.store(GuestAddress{arg(1)+i*4},32,o.target_size[i]);return finish(0);}
    if(method=="Resize"){
        const std::array<std::uint32_t,2> size{static_cast<std::uint32_t>(arg(1)),static_cast<std::uint32_t>(arg(2))};if(size==o.target_size)return finish(0);
        if(!gui_.contexts.contains(o.target_dc))return finish(invalid);const auto bitmap=target_bitmap(size[0],size[1]);if(!bitmap)return finish(0x8007000eU);
        gui_.contexts.at(o.target_dc).bitmap=bitmap;const auto old=std::exchange(o.target_bitmap,bitmap);o.target_size=size;remove_target_bitmap(old);return finish(0);
    }
    if(method=="DrawGlyphRun"){
        if(!arg(4) || !arg(5))return finish(pointer_error);if(arg(3)>2 || !std::isfinite(floating(1)) || !std::isfinite(floating(2)))return finish(invalid);
        const auto dc=gui_.contexts.find(o.target_dc);if(dc==gui_.contexts.end() || dc->second.bitmap!=o.target_bitmap || !gui_.bitmaps.contains(o.target_bitmap))return finish(invalid);
        if(arg(7))memory.check(GuestAddress{arg(7)},16,Access::Write);
        const auto params=bridge.objects.find(arg(5));if(params==bridge.objects.end() || params->second.kind!=Kind::RenderingParams)return finish(invalid);
        const auto at=arg(4);memory.check(GuestAddress{at},unit==8?48:32,Access::Read);const auto face=bridge.objects.find(memory.load(GuestAddress{at},bits));if(face==bridge.objects.end() || face->second.kind!=Kind::FontFace)return finish(invalid);
        const auto read_float=[&](std::uint64_t address){return std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{address},32)));};
        NativeGlyphRun run;run.em=read_float(at+unit);run.baseline_x=floating(1);run.baseline_y=floating(2);run.pixels_per_dip=o.target_scale;run.transform=o.target_transform;run.measuring=static_cast<unsigned>(arg(3));
        run.aliased=params->second.rendering_mode==1;const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{at+unit+4},32));if(count>1048576 || !std::isfinite(run.em) || run.em<=0)return finish(invalid);
        const auto indices=memory.load(GuestAddress{at+(unit==8?16:12)},bits),advances=memory.load(GuestAddress{at+(unit==8?24:16)},bits),offsets=memory.load(GuestAddress{at+(unit==8?32:20)},bits);
        run.sideways=memory.load(GuestAddress{at+(unit==8?40:24)},32)!=0;run.rtl=(memory.load(GuestAddress{at+(unit==8?44:28)},32)&1)!=0;if(count && !indices)return finish(pointer_error);
        run.indices.resize(count);if(advances)run.advances.resize(count);if(offsets)run.offsets.resize(count);
        for(unsigned i=0;i<count;++i){run.indices[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{indices+i*2},16));if(advances){run.advances[i]=read_float(advances+i*4);if(!std::isfinite(run.advances[i]))return finish(invalid);}if(offsets){run.offsets[i]={read_float(offsets+i*8),read_float(offsets+i*8+4)};if(!std::isfinite(run.offsets[i][0]) || !std::isfinite(run.offsets[i][1]))return finish(invalid);}}
        NativeGlyphRaster raster;try{raster=native_font_glyph_raster(face->second.fonts->faces.at(face->second.indices.at(0)),run);}catch(const std::invalid_argument&){return finish(invalid);}
        sync_raster_bitmap(memory,o.target_bitmap);auto& bitmap=gui_.bitmaps.at(o.target_bitmap);const auto& bounds=raster.bounds;
        const auto left=std::max(0,bounds[0]),top=std::max(0,bounds[1]),right=std::min(static_cast<std::int32_t>(o.target_size[0]),bounds[2]),bottom=std::min(static_cast<std::int32_t>(o.target_size[1]),bounds[3]);
        const auto color=static_cast<std::uint32_t>(arg(6));const auto width=static_cast<std::size_t>(static_cast<std::int64_t>(bounds[2])-bounds[0]);
        for(auto y=top;y<bottom;++y)for(auto x=left;x<right;++x){
            const auto offset=(static_cast<std::size_t>(y-bounds[1])*width+static_cast<std::size_t>(x-bounds[0]))*raster.channels;auto& pixel=bitmap.pixels[static_cast<std::size_t>(y)*bitmap.width+static_cast<std::size_t>(x)];auto result=pixel&0xff000000U;
            for(unsigned channel=0;channel<3;++channel){const auto alpha=raster.alpha[offset+(raster.channels==1?0:channel)];const auto source=(color>>(channel*8))&255U,previous=(pixel>>(channel*8))&255U;result|=((source*alpha+previous*(255U-alpha)+127U)/255U)<<(channel*8);}pixel=result;
        }
        bitmap.resource_id=0;bitmap.dib_upload_pending=true;export_dib_bitmap(memory,o.target_bitmap);if(auto surface=rasters_.find(o.target_bitmap);surface!=rasters_.end()){surface->second->upload(bitmap.pixels,true);bitmap.dib_upload_pending=false;}
        if(arg(7))for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(7)+i*4},32,static_cast<std::uint32_t>(bounds[i]));return finish(0);
    }
}
