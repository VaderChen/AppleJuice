// 邏輯調色盤與 DC 狀態共用；輸出裝置為 true color，不宣告 RC_PALETTE。
// RealizePalette 的零表示沒有實體系統色需要重映射，並非忽略所選邏輯調色盤。
if(symbol=="CreatePalette" || symbol=="CreateHalftonePalette" || symbol=="GetSystemPaletteEntries" || symbol=="GetPaletteEntries" || symbol=="SetPaletteEntries" || symbol=="ResizePalette" ||
   symbol=="AnimatePalette" || symbol=="GetNearestPaletteIndex" || symbol=="SelectPalette" || symbol=="RealizePalette" ||
   symbol=="UnrealizeObject" || symbol=="UpdateColors" || symbol=="GetNearestColor"){
    const auto finish=[&](std::uint64_t value,std::uint32_t error=0)->std::optional<std::uint32_t>{if(error)last_error_=error;arg.return_to(state,value);return {};};
    const auto nearest=[](std::span<const std::uint32_t> entries,std::uint32_t color){
        std::uint32_t best=0,distance=UINT32_MAX;
        for(std::uint32_t i=0;i<entries.size();++i){const auto candidate=gui_palette_color(entries[i]);std::uint32_t delta=0;
            for(unsigned shift:{0U,8U,16U}){const int difference=int((candidate>>shift)&255U)-int((color>>shift)&255U);delta+=static_cast<unsigned>(difference*difference);}
            if(delta<distance){best=i;distance=delta;if(!delta)break;}
        }return best;
    };
    if(symbol=="CreateHalftonePalette"){
        if(arg(0) && !known_dc)return finish(0,6);
        GuiState::Palette palette;palette.entries=gui_dib_palette(8);
        const auto handle=gui_.next_object++;gui_.palettes.emplace(handle,std::move(palette));return finish(handle);
    }
    if(symbol=="GetSystemPaletteEntries"){
        if(!known_dc)return finish(0,6);
        // 與 GetDeviceCaps(RASTERCAPS／SIZEPALETTE) 一致：原生 true-color 裝置沒有系統索引表。
        // 邏輯調色盤由 GetPaletteEntries 查詢；不能把選取的 HPALETTE 冒充系統 palette。
        return finish(0);
    }
    if(symbol=="CreatePalette"){
        if(!arg(0))return finish(0,87);
        const auto version=memory.load(GuestAddress{arg(0)},16),count=memory.load(GuestAddress{arg(0)+2},16);
        if(version!=0x300 || !count)return finish(0,87);
        memory.check(GuestAddress{arg(0)+4},static_cast<std::size_t>(count)*4,Access::Read);
        GuiState::Palette palette;palette.entries.reserve(count);
        for(std::uint64_t i=0;i<count;++i)palette.entries.push_back(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+4+i*4},32)));
        const auto handle=gui_.next_object++;gui_.palettes.emplace(handle,std::move(palette));return finish(handle);
    }
    if(symbol=="SelectPalette" || symbol=="RealizePalette" || symbol=="UpdateColors" || symbol=="GetNearestColor"){
        if(!known_dc)return finish(symbol=="RealizePalette" || symbol=="GetNearestColor"?UINT32_MAX:0,6);
        auto& dc=gui_.contexts.try_emplace(arg(0)).first->second;
        if(symbol=="SelectPalette"){
            if(gui_palette_entries(gui_,arg(1)).empty())return finish(0,6);
            const auto previous=dc.palette;dc.palette=arg(1);dc.palette_generation=0;return finish(previous);
        }
        if(symbol=="GetNearestColor")return finish(gui_resolve_color(gui_,dc,d(1)));
        if(symbol=="RealizePalette"){
            const auto palette=gui_.palettes.find(dc.palette);dc.palette_generation=palette==gui_.palettes.end()?1:palette->second.generation;return finish(0);
        }
        // true color 的既有像素不隨邏輯調色盤變動；提交尚未呈現的繪圖即可。
        if(const auto surface=raster_surface(memory,arg(0),false))surface->flush(true);
        if(arg(0)>=0x10000000 && gui_.windows.contains(arg(0)-0x10000000))present_gui(arg(0)-0x10000000);
        return finish(1);
    }
    auto palette=gui_.palettes.find(arg(0));
    if(symbol=="UnrealizeObject"){
        if(palette!=gui_.palettes.end()){++palette->second.generation;return finish(1);}
        if(gui_.brushes.contains(arg(0))){rasters_.erase(arg(0));return finish(1);}return finish(0,6);
    }
    const auto entries=gui_palette_entries(gui_,arg(0));if(entries.empty())return finish(symbol=="GetNearestPaletteIndex"?UINT32_MAX:0,6);
    if(symbol=="GetNearestPaletteIndex")return finish(nearest(entries,d(1)));
    if(symbol=="GetPaletteEntries" && !arg(3))return finish(entries.size());
    if(symbol=="ResizePalette"){
        if(palette==gui_.palettes.end())return finish(0,6);
        if(!d(1) || d(1)>65535)return finish(0,87);
        palette->second.entries.resize(d(1));++palette->second.generation;return finish(1);
    }
    const bool get=symbol=="GetPaletteEntries",animate=symbol=="AnimatePalette";
    if(!get && palette==gui_.palettes.end())return finish(0,6);
    if(d(1)>=entries.size())return finish(0);
    const auto count=std::min<std::size_t>(d(2),entries.size()-d(1));
    if(count && !arg(3))return finish(0,87);
    memory.check(GuestAddress{arg(3)},count*4,get?Access::Write:Access::Read);
    // 先讀完輸入再更新，避免故障留下半套調色盤。
    std::vector<std::uint32_t> values;values.reserve(count);
    for(std::size_t i=0;i<count;++i)values.push_back(get?entries[d(1)+i]:static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+i*4},32)));
    for(std::size_t i=0;i<count;++i){if(get)memory.store(GuestAddress{arg(3)+i*4},32,values[i]);else if(!animate || (palette->second.entries[d(1)+i]&0x01000000U))palette->second.entries[d(1)+i]=values[i];}
    if(!get)++palette->second.generation;
    return finish(animate?1:count);
}
