// DrawState 與 GrayString 共用記憶體 DC／回呼的生命週期；原 HDC 不借給客體回呼。
std::optional<std::uint32_t> WindowsServices::invoke_draw_state(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool wide=service.symbol.ends_with('W');
    const auto fail=[&](std::uint32_t error){last_error_=error;arg.return_to(state,0);return std::optional<std::uint32_t>{};};
    const auto context=gui_.contexts.find(arg(0));if(context==gui_.contexts.end())return fail(6);
    const auto flags=static_cast<std::uint32_t>(arg(9)),type=flags&15U;const bool effect=flags&0xb0U;
    if(type>4 || (flags&~0x86bfU) || (type==0 && !arg(2)))return fail(87);
    const auto brush=arg(1)?arg(1):UINT64_C(0xb004);
    if((flags&0x80U) && !(flags&0x20U) && !gray_brush(gui_,brush))return fail(6);
    auto width=signed_value(arg(7)),height=signed_value(arg(8));const auto x=signed_value(arg(5)),y=signed_value(arg(6));
    if(width<0 || height<0 || (type==0 && (!width || !height)))return fail(87);
    std::u16string text;auto count=arg(4);
    if((type==1 || type==2) && !(type==1 && arg(2) && width && height)){
        if(!arg(3) || count>65536)return fail(87);
        if(!count){
            if(wide){text=read_utf16(memory,GuestAddress{arg(3)});count=text.size();}
            else {const auto bytes=read_ansi(memory,arg(3));count=bytes.size();text=utf8_to_utf16_lossy(bytes);}
            if(count>65536)return fail(87);
        }else {
            memory.check(GuestAddress{arg(3)},static_cast<std::size_t>(count)*(wide?2U:1U),Access::Read);
            if(wide)for(std::uint64_t i=0;i<count;++i)text.push_back(static_cast<char16_t>(memory.load(GuestAddress{arg(3)+i*2},16)));
            else {std::string bytes(static_cast<std::size_t>(count),'\0');for(std::uint64_t i=0;i<count;++i)bytes[static_cast<std::size_t>(i)]=static_cast<char>(memory.load(GuestAddress{arg(3)+i},8));text=utf8_to_utf16_lossy(bytes);}
        }
        if(!width || !height){
            if(type==2){std::u16string visible;for(std::size_t i=0;i<text.size();++i){if(text[i]==u'&'){if(++i==text.size())break;}visible.push_back(text[i]);}text=std::move(visible);}
            const auto font=gui_font(gui_,context->second.font);
            const auto metrics=native_gui_?native_gui_->font_metrics(font).value_or(fallback_font_metrics(font)):fallback_font_metrics(font);
            if(!width)width=native_gui_?native_gui_->text_width(font,text).value_or(static_cast<std::int32_t>(text.size())*metrics.average_width):static_cast<std::int32_t>(text.size())*metrics.average_width;
            if(!height)height=metrics.height;
        }
        if(text.empty() && !count){arg.return_to(state,1);return std::nullopt;}
    }else if(type==3){
        const auto icon=gui_.icons.find(arg(3));if(icon==gui_.icons.end())return fail(6);
        if(!width)width=static_cast<std::int32_t>(icon->second.width);if(!height)height=static_cast<std::int32_t>(icon->second.height);
    }else if(type==4){
        const auto image=gui_.bitmaps.find(arg(3));if(image==gui_.bitmaps.end())return fail(6);
        for(const auto& [handle,dc]:gui_.contexts){(void)handle;if(dc.bitmap==arg(3))return fail(87);}
        if(!width)width=static_cast<std::int32_t>(image->second.width);if(!height)height=static_cast<std::int32_t>(image->second.height);
    }
    if(width<=0 || height<=0 || width>4096 || height>4096 || std::int64_t{x}+width+(flags&0x20U?1:0)>INT32_MAX || std::int64_t{y}+height+(flags&0x20U?1:0)>INT32_MAX)return fail(87);
    if(gui_.gray_strings.size()>=256 || gui_.contexts.size()>=8190 || gui_.bitmaps.size()>=4096)return fail(8);
    const auto target=raster_surface(memory,arg(0));if(!target)return fail(50);
    const auto id=gui_.next_object++,temporary=gui_.next_object++,bitmap=gui_.next_object++;
    GuiGrayString operation{arg(0),temporary,bitmap,brush,threads_.current,static_cast<std::int32_t>(count),x,y,width,height};
    operation.draw_state=true;operation.draw_state_flags=flags;
    try {
        gui_.gray_strings.emplace(id,operation);
        GuiBitmap image{static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),std::vector<std::uint32_t>(static_cast<std::size_t>(width)*static_cast<unsigned>(height),0xffffffU)};
        image.bits_per_pixel=32;gui_.bitmaps.emplace(bitmap,std::move(image));
        GuiDc dc;dc.bitmap=bitmap;dc.font=context->second.font;dc.color=effect?0U:context->second.color;
        dc.background=effect?0xffffffU:context->second.background;dc.mode=effect?2:context->second.mode;
        gui_.contexts.emplace(temporary,dc);const auto surface=raster_surface(memory,temporary);
        if(!surface){discard_gray_string(id,memory,state);return fail(50);}
        if(!effect){const std::array<std::int32_t,4> local{0,0,width,height},source{x,y,x+width,y+height};surface->blit(*target,local,source,GuiDc{},UINT32_MAX);}
        GuestCallback callback;
        if(type==0 || (type==1 && arg(2)))callback={arg(2),{temporary,arg(3),arg(4),static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height)}};
        else if(type==1 || type==2){
            const auto rectangle=allocate_heap(memory,16);gui_.gray_strings.at(id).scratch=rectangle;
            memory.store(GuestAddress{rectangle},32,0);memory.store(GuestAddress{rectangle+4},32,0);memory.store(GuestAddress{rectangle+8},32,static_cast<std::uint32_t>(width));memory.store(GuestAddress{rectangle+12},32,static_cast<std::uint32_t>(height));
            const auto text_flags=0x20U|(type==1?0x800U:0U)|(flags&0x200U?0x100000U:0U)|(flags&0x400U?0x200000U:0U)|(flags&0x8000U?2U:0U);
            callback={resolve("user32.dll",wide?"DrawTextW":"DrawTextA").value,{temporary,arg(3),count,rectangle,text_flags}};
        }else if(type==3)callback={resolve("user32.dll","DrawIconEx").value,{temporary,0,0,arg(3),static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),0,0,3}};
        else {
            const auto source_dc=gui_.next_object++;gui_.gray_strings.at(id).source_dc=source_dc;GuiDc source;source.bitmap=arg(3);gui_.contexts.emplace(source_dc,source);
            callback={resolve("gdi32.dll","BitBlt").value,{temporary,0,0,static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),source_dc,0,0,0x00cc0020U}};
        }
        start_callbacks(state,memory,{std::move(callback)},Continuation::GuiGrayString,id);return std::nullopt;
    }catch(...){discard_gray_string(id,memory,state);throw;}
}
std::optional<std::uint32_t> WindowsServices::resume_draw_state(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t value){
    const auto operation=gui_.gray_strings.at(frame.value);const auto flags=operation.draw_state_flags;std::uint64_t result=static_cast<std::uint32_t>(value)!=0;
    try {
        const auto context=gui_.contexts.find(operation.dc);const auto source=raster_surface(memory,operation.temporary_dc,false);
        const auto target=context==gui_.contexts.end()?std::shared_ptr<RasterSurface>{}:raster_surface(memory,operation.dc);
        if(!source || !target){last_error_=6;result=0;}
        else if(result){
            GuiDc dc=context->second;dc.world=gui_identity;dc.map_mode=1;dc.window_origin={};dc.viewport_origin={};dc.window_extent={1,1};dc.viewport_extent={1,1};
            const std::array<std::int32_t,4> destination{operation.x,operation.y,operation.x+operation.width,operation.y+operation.height},rectangle{0,0,operation.width,operation.height};
            if(!(flags&0xb0U))target->blit(*source,destination,rectangle,dc,UINT32_MAX);
            else {
                const auto image=source->snapshot();const auto width=static_cast<unsigned>(operation.width),height=static_cast<unsigned>(operation.height);
                std::vector<std::uint32_t> mask(static_cast<std::size_t>(width)*height);
                for(unsigned y=0;y<std::min(height,image.height);++y)for(unsigned x=0;x<std::min(width,image.width);++x){
                    const auto pixel=image.pixels[static_cast<std::size_t>(y)*image.width+x]&0xffffffU;
                    if(pixel!=0xffffffU && (!(flags&0x10U) || ((x+y)&1U)==0))mask[static_cast<std::size_t>(y)*width+x]=0xffffffU;
                }
                auto stencil=make_hardware_raster(width,height,mask);
                const auto paint=[&](std::uint64_t handle,std::uint32_t color,int offset){
                    if(handle==0xb005)return;
                    RasterBlit options;options.operation=0xe2;auto& brush=options.brush;brush.color=color;
                    brush.hatch=gui_.brush_hatches.contains(handle)?gui_.brush_hatches.at(handle):-1;brush.pattern_opaque=dc.mode==2;brush.pattern_background=dc.background;brush.pattern_color=dc.color;brush.brush_origin=dc.brush_origin;
                    std::shared_ptr<RasterSurface> pattern;
                    if(gui_.brush_patterns.contains(handle)){const auto& bitmap=gui_.brush_patterns.at(handle);brush.pattern_brush=handle;brush.pattern_monochrome=bitmap.bits_per_pixel==1;pattern=make_hardware_raster(bitmap.width,bitmap.height,bitmap.pixels);}
                    auto dest=destination;for(auto& coordinate:dest)coordinate+=offset;target->blit(*stencil,dest,rectangle,dc,UINT32_MAX,options,pattern.get());
                };
                if(flags&0x20U){paint(0,gui_system_color(20),1);paint(0,gui_system_color(16),0);}
                else {const auto handle=(flags&0x80U)?operation.brush:UINT64_C(0xb004);if(!gray_brush(gui_,handle)){last_error_=6;result=0;}
                    else paint(handle,handle==0xb012?dc.dc_brush_color:gui_.brushes.contains(handle)?gui_.brushes.at(handle):handle==0xb000?0xffffffU:handle==0xb001?0xc0c0c0U:handle==0xb002?0x808080U:handle==0xb003?0x404040U:0U,0);}
            }
            if(operation.dc>=0x10000000 && gui_.windows.contains(operation.dc-0x10000000))present_gui(operation.dc-0x10000000);
        }
    }catch(const std::bad_alloc&){last_error_=8;result=0;}catch(const MemoryFault&){discard_gray_string(frame.value,memory,state);throw;}catch(const std::exception&){last_error_=50;result=0;}
    discard_gray_string(frame.value,memory,state);complete_call(state,memory,result);return std::nullopt;
}
