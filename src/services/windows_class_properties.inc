// Word／Long／Ptr 共用類別狀態；額外類別資料由同類別的所有視窗共享。
const bool info=symbol.starts_with("GetClassInfo"),unregister=symbol.starts_with("UnregisterClass");
try {
    GuiClass* cls=nullptr;std::u16string class_name;
    if(info || unregister){
        const auto requested=arg(info?1:0);
        if(requested>65535){class_name=read(requested);cls=gui_class_record(class_name,!unregister);}
        else {
            for(auto& [key,value]:gui_.classes)if(value.atom==requested){class_name=key;cls=&value;break;}
            if(!cls && !unregister)for(auto& [key,value]:gui_.system_classes)if(value.atom==requested){class_name=key;cls=&value;break;}
        }
        const auto instance=arg(info?0:1);
        if(!cls || (cls->instance!=instance && !(info && !instance && cls->instance==runtime_.image.base) && !(info && (cls->style&0x4000U)))){fail(1411);cls=nullptr;}
        else if(unregister){
            if(std::any_of(gui_.windows.begin(),gui_.windows.end(),[&](const auto& pair){return pair.second.class_atom==cls->atom;}))fail(1412);
            else {
                for(const auto buffer:cls->name_buffers)if(buffer)(void)free_heap(memory,buffer,state);
                for(const auto buffer:cls->menu_buffers)if(buffer)(void)free_heap(memory,buffer,state);
                for(const auto buffer:cls->retired_buffers)if(buffer)(void)free_heap(memory,buffer,state);
                std::erase_if(gui_.classes,[&](const auto& pair){return &pair.second==cls;});result=1;
            }
        }
    }else {
        const auto window=gui_.windows.find(arg(0));
        if(window==gui_.windows.end())fail(1400);
        else {class_name=window->second.class_name;cls=gui_class_record(class_name);if(!cls)fail(1413);}
    }
    const auto string_buffer=[&](std::u16string_view value,std::array<std::uint64_t,2>& buffers){
        const auto index=wide?1U:0U;
        if(!buffers[index]){
            std::vector<std::uint8_t> bytes;
            if(wide)bytes=utf16_bytes(value);
            else {const auto text=utf16_to_utf8(value);bytes.assign(text.begin(),text.end());bytes.push_back(0);}
            const auto buffer=allocate_heap(memory,bytes.size());if(!buffer)throw std::bad_alloc();
            try{memory.write(GuestAddress{buffer},bytes);}catch(...){(void)free_heap(memory,buffer,state);throw;}
            buffers[index]=buffer;
        }
        return buffers[index];
    };
    if(cls && !unregister && (info || !result)){
        if(info){
            const GuiClassAbi abi{x86_,symbol.starts_with("GetClassInfoEx")};
            if(!arg(2))fail(87);
            else {
                memory.check(GuestAddress{arg(2)},abi.size,Access::Write);
                const auto name_buffer=string_buffer(class_name,cls->name_buffers);
                const auto menu=cls->menu_name.empty()?cls->menu_ordinal:string_buffer(cls->menu_name,cls->menu_buffers);
                memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(abi.size));
                if(symbol.starts_with("GetClassInfoEx"))memory.store(GuestAddress{arg(2)},32,abi.size);
                memory.store(GuestAddress{arg(2)+abi.style},32,cls->style);
                memory.store(GuestAddress{arg(2)+abi.class_extra},32,cls->class_extra_size);
                memory.store(GuestAddress{arg(2)+abi.window_extra},32,cls->extra_size);
                auto procedure=cls->procedure;
                if(procedure==stock_gui_proc(class_name,true))procedure=stock_gui_proc(class_name,wide);
                for(const auto [offset,value]:std::array<std::pair<unsigned,std::uint64_t>,8>{{
                    {abi.procedure,procedure},{abi.instance,cls->instance},{abi.icon,cls->icon},{abi.cursor,cls->cursor},
                    {abi.background,cls->background},{abi.menu,menu},{abi.name,name_buffer},{abi.small_icon,cls->small_icon}}})
                    if(offset<abi.size)memory.store(GuestAddress{arg(2)+offset},abi.pointer_bits,value);
                result=1;
            }
        }else {
            const bool set=symbol.starts_with("Set"),word=symbol.ends_with("Word"),pointer=symbol.find("Ptr")!=std::string_view::npos;
            const auto index=s32(arg(1));const auto width=word?2U:pointer && !x86_?8U:4U;
            if(index>=0){
                const auto offset=static_cast<unsigned>(index);
                if(offset>cls->extra.size() || width>cls->extra.size()-offset)fail(1413);
                else {
                    for(unsigned i=0;i<width;++i)result|=std::uint64_t{cls->extra[offset+i]}<<(8*i);
                    if(set)for(unsigned i=0;i<width;++i)cls->extra[offset+i]=static_cast<std::uint8_t>(arg(2)>>(8*i));
                }
            }else if(word){if(!set && index==-32)result=cls->atom;else fail(1413);}
            else {
                std::uint64_t* field=nullptr;
                switch(index){case -12:field=&cls->cursor;break;case -10:field=&cls->background;break;case -16:field=&cls->instance;break;case -24:field=&cls->procedure;break;case -14:field=&cls->icon;break;case -34:field=&cls->small_icon;break;default:break;}
                if(field){
                    result=pointer?*field:static_cast<std::uint32_t>(*field);
                    if(index==-24 && *field==stock_gui_proc(class_name,true))result=pointer?stock_gui_proc(class_name,wide):static_cast<std::uint32_t>(stock_gui_proc(class_name,wide));
                    if(set){if(!x86_ && !pointer)fail(1413);else {*field=x86_?d(2):arg(2);if(index==-24)cls->unicode=wide;}}
                }else if(index==-26){result=cls->style;if(set)cls->style=d(2);}
                else if(index==-18){result=cls->extra_size;if(set)cls->extra_size=d(2);}
                else if(index==-20){result=cls->class_extra_size;if(set)cls->class_extra_size=d(2);}
                else if(!set && index==-32)result=cls->atom;
                else if(index==-8){
                    result=cls->menu_name.empty()?cls->menu_ordinal:string_buffer(cls->menu_name,cls->menu_buffers);
                    if(!pointer)result=static_cast<std::uint32_t>(result);
                    if(set){
                        if(!x86_ && !pointer)fail(1413);
                        else {const auto replacement=arg(2)>65535?read(arg(2)):std::u16string{};cls->menu_name=replacement;cls->menu_ordinal=arg(2)<=65535?static_cast<std::uint16_t>(arg(2)):0;
                            // 上次回傳的字串在類別存續期間保持有效。
                            for(const auto buffer:cls->menu_buffers)if(buffer)cls->retired_buffers.push_back(buffer);cls->menu_buffers={};}
                    }
                }else fail(1413);
            }
        }
    }
}catch(const MemoryFault&){fail(998);}catch(const std::bad_alloc&){fail(8);}
