// 邏輯調色盤與 DC 狀態共用；輸出裝置為 true color，不宣告 RC_PALETTE。
// RealizePalette 的零表示沒有實體系統色需要重映射，並非忽略所選邏輯調色盤。
if(symbol=="CreatePalette" || symbol=="NtGdiCreatePaletteInternal" || symbol=="CreateHalftonePalette" || symbol=="GetSystemPaletteEntries" || symbol=="GetPaletteEntries" || symbol=="SetPaletteEntries" || symbol=="ResizePalette" ||
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
        GuiState::Palette palette;
        palette.entries.resize(256);
        // 真彩色裝置的半色調色盤中間 236 格依序配置 R/G 各 3 位、B 兩位。
        std::copy_n(gui_default_palette.begin(),10,palette.entries.begin());
        for(unsigned i=10;i<246;++i){
            const auto red=(i&7U)<<5, green=((i>>3)&7U)<<5, blue=(i>>6)<<6;
            palette.entries[i]=red|(green<<8)|(blue<<16);
        }
        std::copy_n(gui_default_palette.begin()+10,10,palette.entries.begin()+246);
        const auto handle=gui_.next_object++;gui_.palettes.emplace(handle,std::move(palette));return finish(handle);
    }
    if(symbol=="GetSystemPaletteEntries"){
        if(!known_dc)return finish(0,6);
        // 與 GetDeviceCaps(RASTERCAPS／SIZEPALETTE) 一致：原生 true-color 裝置沒有系統索引表。
        // 邏輯調色盤由 GetPaletteEntries 查詢；不能把選取的 HPALETTE 冒充系統 palette。
        return finish(0);
    }
    if(symbol=="CreatePalette" || symbol=="NtGdiCreatePaletteInternal"){
        if(!arg(0))return finish(0,87);
        const bool native=symbol=="NtGdiCreatePaletteInternal";
        memory.check(GuestAddress{arg(0)},4,Access::Read);
        const auto count=native?d(1):static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+2},16));
        if(count>65535 || (!native && (!count || memory.load(GuestAddress{arg(0)},16)!=0x300)))return finish(0,87);
        GuiState::Palette palette;
        palette.entries.resize(count);
        if(count)memory.read(GuestAddress{arg(0)+4},std::span(reinterpret_cast<std::uint8_t*>(palette.entries.data()),count*4U));
        const auto handle=gui_.next_object;
        gui_.palettes.emplace(handle,std::move(palette));
        ++gui_.next_object;
        return finish(handle);
    }
    if(symbol=="SelectPalette" || symbol=="RealizePalette" || symbol=="UpdateColors" || symbol=="GetNearestColor"){
        if(!known_dc)return finish(symbol=="RealizePalette" || symbol=="GetNearestColor"?UINT32_MAX:0,6);
        auto& dc=gui_.contexts.try_emplace(arg(0)).first->second;
        if(symbol=="SelectPalette"){
            if(arg(1)!=0xb00f && !gui_.palettes.contains(arg(1)))return finish(0,6);
            const auto previous=dc.palette;dc.palette=arg(1);dc.palette_generation=0;return finish(previous);
        }
        if(symbol=="GetNearestColor")return finish(gui_resolve_color(gui_,dc,d(1)));
        if(symbol=="RealizePalette"){
            const auto palette=gui_.palettes.find(dc.palette);dc.palette_generation=palette==gui_.palettes.end()?1:palette->second.generation;return finish(0);
        }
        // 只有具備實體調色盤的裝置才有已寫入的系統色索引要重映射。
        if(!nested_service(state,memory,"gdi32.dll","GetDeviceCaps",{arg(0),104}))return finish(0);
        if(const auto surface=raster_surface(memory,arg(0),false))surface->flush(true);
        if(arg(0)>=0x10000000 && gui_.windows.contains(arg(0)-0x10000000))present_gui(arg(0)-0x10000000);
        return finish(1);
    }
    auto palette=gui_.palettes.find(arg(0));
    if(symbol=="UnrealizeObject"){
        if(palette!=gui_.palettes.end()){++palette->second.generation;return finish(1);}
        if(arg(0)==0xb00f || (arg(0)>=0xb000 && arg(0)<=0xb005) || arg(0)==0xb012)return finish(1);
        if(gui_.brushes.contains(arg(0))){rasters_.erase(arg(0));return finish(1);}return finish(0,6);
    }
    if(symbol=="ResizePalette"){
        if(palette==gui_.palettes.end())return finish(0,6);
        if(!d(1) || d(1)>65535)return finish(0,87);
        palette->second.entries.resize(d(1));++palette->second.generation;return finish(1);
    }
    const auto entries=gui_palette_entries(gui_,arg(0));
    if(arg(0)!=0xb00f && palette==gui_.palettes.end())
        return finish(symbol=="GetNearestPaletteIndex"?UINT32_MAX:0,native_palette?0:6);
    if(symbol=="GetNearestPaletteIndex")return finish(entries.empty()?UINT32_MAX:nearest(entries,d(1)));
    if(symbol=="GetPaletteEntries" && !arg(3))return finish(entries.size());
    const bool get=symbol=="GetPaletteEntries",animate=symbol=="AnimatePalette";
    if(!get && palette==gui_.palettes.end())return finish(0,native_palette?0:6);
    const auto start=native_palette?static_cast<std::uint16_t>(d(1)):d(1);
    const auto requested=native_palette?static_cast<std::uint16_t>(d(2)):d(2);
    if(start>=entries.size())return finish(0);
    const auto count=std::min<std::size_t>(requested,entries.size()-start);
    if(count && !arg(3))return finish(0,87);
    if(count)memory.check(GuestAddress{arg(3)},count*4,get?Access::Write:Access::Read);
    if(get){
        if(count)memory.write(GuestAddress{arg(3)},std::span(reinterpret_cast<const std::uint8_t*>(entries.data()+start),count*4));
        return finish(count);
    }
    std::vector<std::uint32_t> values(count);
    if(count)memory.read(GuestAddress{arg(3)},std::span(reinterpret_cast<std::uint8_t*>(values.data()),count*4));
    std::size_t updated=0;
    for(std::size_t i=0;i<count;++i){
        auto& entry=palette->second.entries[start+i];
        if(animate && !(entry&0x01000000U))continue;
        entry=values[i];
        ++updated;
    }
    if(updated)++palette->second.generation;
    return finish(animate && !native_palette?1:updated);
}
