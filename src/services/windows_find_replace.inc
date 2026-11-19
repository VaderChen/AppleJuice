// FindTextA／W、ReplaceTextA／W：非模態尋找／取代對話方塊。與 PrintDlg 相同由主機產生對話範本（或使用呼叫者的範本），以 CreateDialogIndirectParam
// 建立並由內部對話程序處理；按鈕以註冊訊息 commdlg_FindReplace（FINDMSGSTRING）同步通知擁有者，lParam 為呼叫者的 FINDREPLACE。
// 字串一律經既有的 Get／SetDlgItemTextA／W 直接讀寫呼叫者緩衝區，沿用同一套 ANSI 轉換。由 invoke_common_dialog 引入。
if(service.symbol.starts_with("FindText") || service.symbol.starts_with("ReplaceText") || service.family=="internal-find-replace"){
    // FINDREPLACE：x64 為 80 位元組；x86 為 40 位元組（pack(1) 與自然對齊相同）。
    struct FindLayout{unsigned size,owner,instance,flags,find,replace,find_length,replace_length,hook,name;};
    static constexpr FindLayout x86_find{40,4,8,12,16,20,24,26,32,36},x64_find{80,8,16,24,32,40,48,50,64,72};
    static constexpr std::uint32_t down=1,whole_word=2,match_case=4,find_next=8,replace_one=0x10,replace_all=0x20,dialog_term=0x40,show_help=0x80,enable_hook=0x100,
        enable_template=0x200,no_up_down=0x400,no_match_case=0x800,no_whole_word=0x1000,template_handle=0x2000,hide_up_down=0x4000,hide_match_case=0x8000,hide_whole_word=0x10000;
    if(service.family=="internal-find-replace")
        return nested_continuation(state,memory,"尋找／取代對話程序",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto window=call(0);const auto message=static_cast<std::uint32_t>(call(1));
            const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            auto found=message==0x110?gui_.find_replaces.find(call(3)):std::find_if(gui_.find_replaces.begin(),gui_.find_replaces.end(),[&](const auto& entry){return window && entry.second.window==window;});
            if(found==gui_.find_replaces.end() || !gui_.windows.contains(window))co_return finish(0);
            const auto id=found->first;const auto data=[&]()->GuiFindReplace&{return gui_.find_replaces.at(id);};
            const auto& L=x86_?x86_find:x64_find;const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto request=data().request;const bool wide=data().wide;
            const auto child=[&](unsigned control)->GuiWindow*{const auto h=gui_child(window,control);return h?&gui_.windows.at(h):nullptr;};
            const auto present=[&](unsigned control){if(const auto h=gui_child(window,control))present_gui(h);};
            const auto api=[&](std::string_view name){return resolve("user32.dll",name).value;};
            // 尋找字串為空時停用「找下一個」與取代按鈕。
            const auto refresh=[&]{const auto* edit=child(0x480);const bool any=edit && !edit->title.empty();
                for(unsigned control:{1U,0x400U,0x401U})if(auto* button=child(control)){gui_set_window_enabled(*button,any);present(control);}};
            if(message==0x82){gui_.find_replaces.erase(id);co_return finish(0);}
            if(message==0x110){
                data().window=window;const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{request+L.flags},32));
                (void)co_await service_callback::call(api(wide?"SetDlgItemTextW":"SetDlgItemTextA"),{window,0x480,m.load(GuestAddress{request+L.find},bits)});
                if(data().replace)(void)co_await service_callback::call(api(wide?"SetDlgItemTextW":"SetDlgItemTextA"),{window,0x481,m.load(GuestAddress{request+L.replace},bits)});
                if(!gui_.find_replaces.contains(id) || !gui_.windows.contains(window))co_return finish(0);
                std::vector<std::uint64_t> hidden;
                const auto option=[&](unsigned control,bool checked,std::uint32_t disable,std::uint32_t hide){if(auto* w=child(control)){w->check=checked;
                    if(flags&disable)gui_set_window_enabled(*w,false);if(flags&hide)hidden.push_back(gui_child(window,control));present(control);}};
                option(0x410,(flags&whole_word)!=0,no_whole_word,hide_whole_word);option(0x411,(flags&match_case)!=0,no_match_case,hide_match_case);
                option(0x420,!(flags&down),no_up_down,hide_up_down);option(0x421,(flags&down)!=0,no_up_down,hide_up_down);if(child(0x430) && (flags&hide_up_down))hidden.push_back(gui_child(window,0x430));
                if(child(0x40e) && !(flags&show_help))hidden.push_back(gui_child(window,0x40e));
                refresh();
                // 隱藏沿用 ShowWindow(SW_HIDE)，與 WS_VISIBLE 樣式及原生視窗一致。
                for(const auto h:hidden)(void)co_await service_callback::call(api("ShowWindow"),{h,0});
                if(!gui_.find_replaces.contains(id) || !gui_.windows.contains(window))co_return finish(0);
            }
            if(const auto hook=data().hook){
                // 掛勾程序先收到訊息（WM_INITDIALOG 於預設初始化之後，lParam 為 FINDREPLACE）；回傳非零即不做預設處理。
                const auto handled=co_await service_callback::call(hook,{window,message,call(2),message==0x110?request:call(3)});
                if(message==0x110 || handled)co_return finish(handled);
                if(!gui_.find_replaces.contains(id) || !gui_.windows.contains(window))co_return finish(0);
            }else if(message==0x110)co_return finish(1);
            const auto owner=data().owner;const auto notify_message=gui_registered_message(u"commdlg_FindReplace");
            const auto terminate=[&]()->service_callback::Task<std::uint64_t>{
                const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{request+L.flags},32));
                m.store(GuestAddress{request+L.flags},32,(flags&~(find_next|replace_one|replace_all))|dialog_term);
                (void)co_await service_callback::call(api("SendMessageW"),{owner,notify_message,0,request});
                (void)co_await service_callback::call(api("DestroyWindow"),{window});co_return 1;};
            if(message==0x10)co_return finish(co_await terminate());
            if(message!=0x111)co_return finish(0);
            const auto control=static_cast<unsigned>(call(2)&65535U),code=static_cast<unsigned>((call(2)>>16)&65535U);
            if(control==0x480 && code==0x300){refresh();co_return finish(1);}
            if(control==0x420 || control==0x421){if(const auto* radio=child(control);radio && radio->enabled)for(unsigned n:{0x420U,0x421U})if(auto* other=child(n)){other->check=n==control;present(n);}co_return finish(1);}
            if(control==2)co_return finish(co_await terminate());
            if(control==0x40e){const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{request+L.flags},32));
                if(flags&show_help)(void)co_await service_callback::call(api("SendMessageW"),{owner,gui_registered_message(u"commdlg_help"),window,request});co_return finish(1);}
            const std::uint32_t action=control==1?find_next:control==0x400 && data().replace?replace_one:control==0x401 && data().replace?replace_all:0U;
            if(!action)co_return finish(0);
            if(const auto* button=child(control);button && !button->enabled)co_return finish(1);
            // 先把編輯框內容寫回呼叫者緩衝區（含 NUL，最多 wFindWhatLen／wReplaceWithLen 個字元），再更新旗標並通知擁有者。
            const bool replacing=data().replace;
            (void)co_await service_callback::call(api(wide?"GetDlgItemTextW":"GetDlgItemTextA"),{window,0x480,m.load(GuestAddress{request+L.find},bits),m.load(GuestAddress{request+L.find_length},16)});
            if(replacing)(void)co_await service_callback::call(api(wide?"GetDlgItemTextW":"GetDlgItemTextA"),{window,0x481,m.load(GuestAddress{request+L.replace},bits),m.load(GuestAddress{request+L.replace_length},16)});
            if(!gui_.find_replaces.contains(id) || !gui_.windows.contains(window))co_return finish(1);
            auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{request+L.flags},32))&~(find_next|replace_one|replace_all|dialog_term);
            const auto option=[&](unsigned control,std::uint32_t bit){if(const auto* w=child(control)){flags=(flags&~bit)|(w->check?bit:0U);}};
            option(0x410,whole_word);option(0x411,match_case);if(child(0x421))option(0x421,down);
            m.store(GuestAddress{request+L.flags},32,flags|action);
            (void)co_await service_callback::call(api("SendMessageW"),{owner,notify_message,0,request});co_return finish(1);
        });
    return nested_continuation(state,memory,"FindText",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto& L=x86_?x86_find:x64_find;const auto bits=static_cast<std::uint8_t>(x86_?32:64);
        const bool wide=service.symbol.ends_with('W'),replace=service.symbol.starts_with("ReplaceText");const auto p=call(0);
        const auto fail=[&](std::uint32_t code){gui_.common_dialog_error=code;call.return_to(s,0);return std::optional<std::uint32_t>{};};
        GuiFindReplace data;std::uint64_t source=0,instance=0;std::vector<std::uint8_t> builtin;std::uint32_t error=2;
        try{
            if(!p)co_return fail(2);if(m.load(GuestAddress{p},32)!=L.size)co_return fail(1);m.check(GuestAddress{p},L.size,Access::Write);
            const auto get=[&](unsigned offset,unsigned width){return m.load(GuestAddress{p+offset},static_cast<std::uint8_t>(width));};
            const auto flags=static_cast<std::uint32_t>(get(L.flags,32));data.request=p;data.wide=wide;data.replace=replace;data.owner=get(L.owner,bits);instance=get(L.instance,bits);
            if(!data.owner || !gui_.windows.contains(data.owner))co_return fail(2);
            if(!get(L.find,bits) || !get(L.find_length,16) || (replace && (!get(L.replace,bits) || !get(L.replace_length,16))))co_return fail(0x4001);
            data.hook=flags&enable_hook?get(L.hook,bits):0;if((flags&enable_hook) && !data.hook)co_return fail(11);
            if(data.hook && !handles(GuestAddress{data.hook}))m.check(GuestAddress{data.hook},1,Access::Execute);
            const unsigned character=wide?2:1;
            m.check(GuestAddress{get(L.find,bits)},get(L.find_length,16)*character,Access::Write);
            if(replace)m.check(GuestAddress{get(L.replace,bits)},get(L.replace_length,16)*character,Access::Write);
            // 自訂範本：FR_ENABLETEMPLATEHANDLE 時 hInstance 即範本記憶體；FR_ENABLETEMPLATE 由 hInstance 的資源載入。
            if(flags&template_handle){error=8;const auto f=gui_.global_memory.find(instance);if(f==gui_.global_memory.end() || !f->second.address)co_return fail(8);
                source=f->second.address;(void)gui_dialog_layout(m,source,f->second.size);instance=0;}
            else if(flags&enable_template){if(!instance)co_return fail(4);const auto name=get(L.name,bits);if(!name)co_return fail(3);error=6;
                const auto entry=gui_resource(m,instance,name,5,wide);if(!entry)co_return fail(6);source=resource_image(entry,true)->base+m.load(GuestAddress{entry},32);
                (void)gui_dialog_layout(m,source,m.load(GuestAddress{entry+4},32));}
            else{
                // 內建範本：與 PrintDlg 相同的 DLGTEMPLATE 寫法；控制項 ID 依 dlgs.h（edt1／edt2、chx1 全字、chx2 大小寫、rad1／rad2 方向、psh1／psh2 取代）。
                std::vector<std::uint8_t> items;unsigned count=0;const auto put=[](std::vector<std::uint8_t>& out,std::uint32_t value,unsigned size){for(unsigned i=0;i<size;++i)out.push_back(static_cast<std::uint8_t>(value>>(8*i)));};
                const auto text=[&](std::vector<std::uint8_t>& out,std::u16string_view value){for(const auto c:value)put(out,c,2);put(out,0,2);};
                const auto item=[&](unsigned id,unsigned cls,std::u16string_view label,unsigned x,unsigned y,unsigned w,unsigned h,std::uint32_t style=0){
                    while(items.size()%4)items.push_back(0);put(items,0x50000000U|style,4);put(items,0,4);put(items,x,2);put(items,y,2);put(items,w,2);put(items,h,2);put(items,id,2);put(items,0xffff,2);put(items,cls,2);text(items,label);put(items,0,2);++count;};
                unsigned height=0;
                if(replace){
                    item(0xffff,0x82,u"尋找目標",10,10,52,12);item(0x480,0x81,u"",64,8,166,14,0x810080);item(0xffff,0x82,u"取代為",10,28,52,12);item(0x481,0x81,u"",64,26,166,14,0x810080);
                    item(0x410,0x80,u"全字拼寫須符合",10,48,140,12,0x10003);item(0x411,0x80,u"大小寫須相符",10,64,140,12,0x10003);
                    item(1,0x80,u"找下一個",240,8,60,14,0x10001);item(0x400,0x80,u"取代",240,26,60,14,0x10000);item(0x401,0x80,u"全部取代",240,44,60,14,0x10000);
                    item(2,0x80,u"取消",240,62,60,14,0x10000);item(0x40e,0x80,u"說明",240,80,60,14,0x10000);height=100;
                }else{
                    item(0xffff,0x82,u"尋找目標",10,10,52,12);item(0x480,0x81,u"",64,8,166,14,0x810080);
                    item(0x410,0x80,u"全字拼寫須符合",10,30,110,12,0x10003);item(0x411,0x80,u"大小寫須相符",10,46,110,12,0x10003);
                    item(0x430,0x80,u"方向",130,26,100,36,7);item(0x420,0x80,u"向上",138,42,40,12,0x30009);item(0x421,0x80,u"向下",182,42,44,12,9);
                    item(1,0x80,u"找下一個",240,8,60,14,0x10001);item(2,0x80,u"取消",240,26,60,14,0x10000);item(0x40e,0x80,u"說明",240,44,60,14,0x10000);height=66;
                }
                // WS_POPUP｜WS_VISIBLE｜WS_CAPTION｜WS_SYSMENU｜DS_SETFONT，非模態。
                put(builtin,0x90c80040U,4);put(builtin,1,4);put(builtin,count,2);put(builtin,30,2);put(builtin,30,2);put(builtin,310,2);put(builtin,height,2);put(builtin,0,2);put(builtin,0,2);
                text(builtin,replace?u"取代":u"尋找");put(builtin,9,2);text(builtin,u"Segoe UI");builtin.insert(builtin.end(),items.begin(),items.end());
            }
            error=9;if(gui_.find_replaces.size()>=256)co_return fail(9);
        }catch(const MemoryFault&){co_return fail(error);}catch(const HostCallFault&){co_return fail(error);}catch(const std::bad_alloc&){co_return fail(9);}catch(const std::invalid_argument&){co_return fail(error);}
        GuestScratch scratch(*this,m,s);
        if(!builtin.empty()){source=scratch.allocate(builtin.size());if(!source)co_return fail(9);m.write(GuestAddress{source},builtin);}
        const auto id=gui_.next_object++;const auto owner=data.owner;gui_.find_replaces.emplace(id,std::move(data));
        const auto window=co_await service_callback::call_value(resolve("user32.dll",wide?"CreateDialogIndirectParamW":"CreateDialogIndirectParamA").value,
            {instance,source,owner,resolve("internal-find-replace","DialogProc").value,id});
        const auto created=x86_?window&UINT32_MAX:window;
        if(!created || !gui_.windows.contains(created)){gui_.find_replaces.erase(id);co_return fail(0xffff);}
        gui_.common_dialog_error=0;call.return_to(s,created);co_return std::optional<std::uint32_t>{};
    });
}
