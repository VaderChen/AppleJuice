// 純量設定與客體常式讀寫同一頁；需要繪圖／訊息的效果沿既有 GUI pump。
void WindowsServices::initialize_gui_timing(GuestMemory& memory,bool shared){
    if(!gui_timing_cell_.value){
        gui_timing_cell_=GuestAddress{callback_return_.value+X64ARM_GUI_TIMING_PAGE_OFFSET};
        memory.map(gui_timing_cell_,GuestMemory::guest_page_size,read_write);
        memory.store(gui_timing_cell_,64,gui_timing_cell_.value+X64ARM_GUI_TIMING_LOCAL_OFFSET);
        memory.store(GuestAddress{gui_timing_cell_.value+8},32,500);
        memory.store(GuestAddress{gui_timing_cell_.value+12},32,500);
    }
    if(!shared || !runtime_.network.external_io)return;
#if !defined(_WIN32)
    std::array<std::uint8_t,4096> initial{};
    for(unsigned i=0;i<2;++i){const auto value=gui_timing(memory,i);for(unsigned n=0;n<4;++n)initial[i*4+n]=static_cast<std::uint8_t>(value>>(n*8));}
    const auto saved=last_error_;
    const auto address=map_shared_table(memory,u"Win32InputTiming",u"v1",initial,true);
    if(!address)throw HostCallFault("無法建立共用 GUI 時序設定，錯誤 "+std::to_string(last_error_));
    memory.store(gui_timing_cell_,64,address);last_error_=saved;
#endif
}
std::uint32_t WindowsServices::gui_timing(const GuestMemory& memory,unsigned index) const {
    return static_cast<std::uint32_t>(memory.load(GuestAddress{memory.load(gui_timing_cell_,x86_?32:64)+index*4},32));
}
void WindowsServices::set_gui_timing(GuestMemory& memory,unsigned index,std::uint32_t value){
    if(!index)value=value?std::min(value,5000U):500U;
    memory.store(GuestAddress{memory.load(gui_timing_cell_,x86_?32:64)+index*4},32,value);
}
bool WindowsServices::gui_input_connected(std::uint64_t first,std::uint64_t second) const {
    std::set<std::uint64_t> visited{first};std::vector<std::uint64_t> pending{first};
    for(std::size_t i=0;i<pending.size();++i){if(pending[i]==second)return true;
        for(const auto& [a,b]:gui_.input_attachments){const auto next=a==pending[i]?b:b==pending[i]?a:0;
            if(next && visited.insert(next).second)pending.push_back(next);}}
    return false;
}
GuiCaret* WindowsServices::gui_caret(std::uint64_t thread){
    GuiCaret* result=nullptr;
    for(auto& [owner,caret]:gui_.carets)if(gui_input_connected(owner,thread) && (!result || caret.generation>result->generation))result=&caret;
    return result;
}
void WindowsServices::draw_gui_caret(GuestMemory&,GuiCaret& caret,bool visible){
    if(caret.drawn==visible)return;
    const auto window=gui_.windows.find(caret.window);if(window==gui_.windows.end()){caret.drawn=false;return;}
    auto& w=window->second;const auto dc=UINT64_C(0x10000000)+caret.window;
    if(!w.width || !w.height){caret.drawn=false;return;}
    auto surface=rasters_.contains(dc)?rasters_.at(dc):std::shared_ptr<RasterSurface>{};
    if(!surface){
        // 沿用原生控制項／GDI 的既有內容，不能因第一次顯示 caret 清掉視窗。
        const auto captured=native_gui_?native_gui_->capture_client(caret.window):std::optional<GuiBitmap>{};
        const bool valid=captured && captured->width==w.width && captured->height==w.height && captured->pixels.size()==std::uint64_t{w.width}*w.height;
        const std::span<const std::uint32_t> pixels=valid?std::span<const std::uint32_t>{captured->pixels}:std::span<const std::uint32_t>{};
        surface=make_hardware_raster(w.width,w.height,pixels,native_gui_?native_gui_->graphics_view(caret.window):nullptr);
        if(!valid){GuiDraw background;background.right=static_cast<std::int32_t>(w.width);background.bottom=static_cast<std::int32_t>(w.height);background.color=0xffffff;surface->draw(background);
            for(const auto& draw:w.drawing)surface->draw(draw);}
        rasters_[dc]=surface;if(native_gui_)native_gui_->graphics_surface(caret.window,surface);
    }
    GuiDc context;
    if(const auto found=gui_.contexts.find(dc);found!=gui_.contexts.end())context=found->second;
    context.application_clip.reset();context.meta_clip.reset();context.paint_clip.reset();context.excluded.clear();
    context.clipped=true;context.clip={0,0,static_cast<std::int32_t>(w.width),static_cast<std::int32_t>(w.height)};
    const auto right=static_cast<std::int32_t>(std::clamp<std::int64_t>(std::int64_t{caret.x}+caret.width,INT32_MIN,INT32_MAX));
    const auto bottom=static_cast<std::int32_t>(std::clamp<std::int64_t>(std::int64_t{caret.y}+caret.height,INT32_MIN,INT32_MAX));
    if(caret.bitmap){
        auto mask=make_hardware_raster(caret.mask.width,caret.mask.height,caret.mask.pixels);
        RasterBlit op;op.operation=0x66;
        surface->blit(*mask,{caret.x,caret.y,right,bottom},{0,0,static_cast<std::int32_t>(caret.width),static_cast<std::int32_t>(caret.height)},context,UINT32_MAX,op);
    }else {
        GuiDraw draw;draw.x=caret.x;draw.y=caret.y;draw.right=right;draw.bottom=bottom;draw.color=0xffffff;draw.rop2=7;
        draw.clipped=true;draw.clip=context.clip;draw.transform=gui_device_transform(context);surface->draw(draw);
    }
    surface->flush();caret.drawn=visible;
}
void WindowsServices::pump_gui_carets(GuestMemory& memory){
    if(gui_.carets.empty())return;
    const auto interval=gui_timing(memory,1);const auto now=runtime_.clock_ticks;
    for(auto& [thread,caret]:gui_.carets){(void)thread;
        const auto w=gui_.windows.find(caret.window);
        const bool shown=w!=gui_.windows.end() && w->second.visible && w->second.desktop_object==gui_.input_desktop && !caret.hidden && !caret.painting;
        if(!shown){draw_gui_caret(memory,caret,false);caret.deadline=UINT64_MAX;continue;}
        if(caret.interval!=interval || caret.deadline==UINT64_MAX){
            const bool start=caret.interval!=interval || !caret.drawn;
            caret.interval=interval;caret.deadline=UINT64_MAX;
            if(start)draw_gui_caret(memory,caret,true);
            if(interval && interval!=UINT32_MAX && start)caret.deadline=now+std::uint64_t{interval}*10000;
        }else if(now>=caret.deadline){
            const auto ticks=std::uint64_t{interval}*10000;
            const auto steps=(now-caret.deadline)/ticks+1;
            if(steps&1)draw_gui_caret(memory,caret,!caret.drawn);
            caret.deadline+=steps*ticks;
        }
    }
}
std::uint64_t WindowsServices::gui_caret_deadline() const {
    auto result=UINT64_MAX;for(const auto& [thread,caret]:gui_.carets){(void)thread;result=std::min(result,caret.deadline);}return result;
}
void WindowsServices::paint_gui_caret(GuestMemory& memory,std::uint64_t window,bool begin){
    for(auto& [thread,caret]:gui_.carets){(void)thread;if(caret.window!=window)continue;
        if(begin){if(caret.painting!=UINT32_MAX)++caret.painting;draw_gui_caret(memory,caret,false);caret.deadline=UINT64_MAX;}
        else if(caret.painting)--caret.painting;
    }
    if(!begin)pump_gui_carets(memory);
}
std::optional<std::uint32_t> WindowsServices::invoke_gui_caret(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto name=service.symbol;
    auto* caret=gui_caret(threads_.current);std::uint64_t result=0;
    const auto fail=[&](std::uint32_t error){last_error_=error;};
    if(name=="CreateCaret"){
        const auto window=gui_.windows.find(call(0));
        const auto width=static_cast<std::int32_t>(call(2)),height=static_cast<std::int32_t>(call(3));
        const auto bitmap=gui_.bitmaps.find(call(1));
        if(window==gui_.windows.end())fail(1400);
        else if(!gui_input_connected(threads_.current,window->second.thread))fail(5);
        else if(call(1)>1 && bitmap==gui_.bitmaps.end())fail(6);
        else if(call(1)<=1 && (width<0 || height<0 || width>16384 || height>16384))fail(87);
        else {
            GuiCaret next;next.window=call(0);next.bitmap=call(1);next.generation=++gui_.caret_generation;
            next.width=width?static_cast<std::uint32_t>(width):system_metric(5);next.height=height?static_cast<std::uint32_t>(height):system_metric(6);
            if(call(1)>1){sync_raster_bitmap(memory,call(1));next.mask=bitmap->second;next.width=next.mask.width;next.height=next.mask.height;}
            else if(call(1)==1){next.mask.width=next.width;next.mask.height=next.height;next.mask.pixels.resize(std::size_t{next.width}*next.height);
                for(unsigned y=0;y<next.height;++y)for(unsigned x=0;x<next.width;++x)next.mask.pixels[std::size_t{y}*next.width+x]=((x+y)&1)?0:0xffffff;}
            for(auto it=gui_.carets.begin();it!=gui_.carets.end();)if(gui_input_connected(it->first,threads_.current)){draw_gui_caret(memory,it->second,false);it=gui_.carets.erase(it);}else ++it;
            gui_.carets.emplace(threads_.current,std::move(next));result=1;
        }
    }else if(!caret)fail(5);
    else if(name=="DestroyCaret"){
        const auto window=caret->window;draw_gui_caret(memory,*caret,false);
        std::erase_if(gui_.carets,[&](const auto& entry){return entry.second.window==window && gui_input_connected(entry.first,threads_.current);});result=1;
    }else if(name=="GetCaretPos"){
        if(!call(0))fail(87);else {memory.check(GuestAddress{call(0)},8,Access::Write);memory.store(GuestAddress{call(0)},32,static_cast<std::uint32_t>(caret->x));memory.store(GuestAddress{call(0)+4},32,static_cast<std::uint32_t>(caret->y));result=1;}
    }else if(name=="SetCaretPos"){
        draw_gui_caret(memory,*caret,false);caret->x=static_cast<std::int32_t>(call(0));caret->y=static_cast<std::int32_t>(call(1));caret->deadline=UINT64_MAX;pump_gui_carets(memory);result=1;
    }else if(call(0) && call(0)!=caret->window)fail(5);
    else if(name=="HideCaret"){
        if(caret->hidden!=UINT32_MAX)++caret->hidden;draw_gui_caret(memory,*caret,false);caret->deadline=UINT64_MAX;result=1;
    }else if(name=="ShowCaret"){
        if(caret->hidden)--caret->hidden;pump_gui_carets(memory);result=caret->hidden?0:1;
    }
    call.return_to(state,result);return std::nullopt;
}
void WindowsServices::classify_gui_click(GuestMemory& memory,GuiMessage& message,std::int32_t hit,bool control){
    const auto target=gui_.windows.find(message.window);if(target==gui_.windows.end())return;
    auto key=target->second.thread;for(const auto& [a,b]:gui_.input_attachments)if(gui_input_connected(key,a))key=std::min({key,a,b});
    auto& previous=gui_.mouse_clicks[key];auto code=message.message;
    const bool nonclient=code>=0xa0 && code<=0xad;
    if(nonclient)code+=0x160;
    if(control)code=0x201;
    const bool up=code==0x202 || code==0x205 || code==0x208 || code==0x20c;
    const auto detail=control?(message.wparam^(message.lparam<<32)):message.wparam&0xffff0000U;
    if(up){if(previous.valid && previous.window==message.window && previous.message+1==code && previous.detail==detail)previous.released=true;return;}
    if(code==0x203 || code==0x206 || code==0x209 || code==0x20d)code-=2;
    if(code!=0x201 && code!=0x204 && code!=0x207 && code!=0x20b)return;
    const auto cls=gui_class_record(target->second.class_name);
    const bool allowed=control || nonclient || hit!=1 || (cls && (cls->style&8));
    const auto time=message.mouse_time.value_or(message.queued_time);
    const auto point=message.has_point?message.point:message.queued_cursor;
    const auto dx=std::int64_t{point[0]}-previous.point[0],dy=std::int64_t{point[1]}-previous.point[1];
    const auto width=std::max(1U,system_metric(36)),height=std::max(1U,system_metric(37));
    const bool doubled=allowed && previous.valid && previous.released && previous.window==message.window && previous.message==code && previous.detail==detail && previous.hit==hit &&
        static_cast<std::uint32_t>(time-previous.time)<=gui_timing(memory,0) && dx>=-std::int64_t{width/2} && dx<std::int64_t{(width+1)/2} && dy>=-std::int64_t{height/2} && dy<std::int64_t{(height+1)/2};
    previous={message.window,detail,code,time,hit,point,control,allowed && !doubled};
    message.message=control?(doubled?0x10006U:0x10009U):code+(doubled?2U:0U)-(nonclient?0x160U:0U);
}
