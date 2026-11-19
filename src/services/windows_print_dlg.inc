// PrintDlgA／W（經典 PRINTDLG）：與 PageSetupDlg 相同的作法——主機產生對話範本（或使用呼叫者的範本），以 DialogBoxIndirectParam 執行
// 內部對話程序；列印與列印設定兩個對話方塊共用同一個程序與狀態。虛擬印表機、DEVMODE／DEVNAMES 與 DC 沿用 PrintDlgEx 的配置。
// 由 invoke_print_dialog 引入；service 為 PrintDlgA／W 或 internal-print-dlg DialogProc。
if(service.symbol=="PrintDlgA" || service.symbol=="PrintDlgW" || service.family=="internal-print-dlg"){
    // PRINTDLG：x64 自然對齊 120 位元組；x86 為 pack(1) 的 66 位元組。
    struct ClassicLayout{unsigned size,owner,mode,names,dc,flags,from,to,minimum,maximum,copies,instance,print_hook,setup_hook,print_name,setup_name,print_handle,setup_handle;};
    static constexpr ClassicLayout x86_layout{66,4,8,12,16,20,24,26,28,30,32,34,42,46,50,54,58,62},x64_layout{120,8,16,24,32,40,44,46,48,50,52,56,72,80,88,96,104,112};
    static constexpr std::uint32_t selection=1,page_numbers=2,no_selection=4,no_page_numbers=8,collate=0x10,to_file=0x20,setup_only=0x40,return_default=0x400,show_help=0x800,
        print_hook=0x1000,setup_hook=0x2000,print_template=0x4000,setup_template=0x8000,print_handle=0x10000,setup_handle=0x20000,devmode_copies=0x40000,disable_file=0x80000,hide_file=0x100000;
    if(service.family=="internal-print-dlg")
        return nested_continuation(state,memory,"PrintDlg 對話程序",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto window=call(0);const auto message=static_cast<std::uint32_t>(call(1));
            const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            auto found=message==0x110?gui_.classic_prints.find(call(3)):std::find_if(gui_.classic_prints.begin(),gui_.classic_prints.end(),
                [&](const auto& entry){return window && (entry.second.print_window==window || entry.second.setup_window==window);});
            if(found==gui_.classic_prints.end() || !gui_.windows.contains(window))co_return finish(0);
            const auto id=found->first;const auto data=[&]()->GuiClassicPrint&{return gui_.classic_prints.at(id);};
            const auto child=[&](unsigned control)->GuiWindow*{const auto h=gui_child(window,control);return h?&gui_.windows.at(h):nullptr;};
            const auto present=[&](unsigned control){if(const auto h=gui_child(window,control))present_gui(h);};
            const auto status=[&](std::u16string text){if(auto* w=child(499)){w->title=std::move(text);present(499);}};
            const auto end=[&](std::uint64_t result){auto& w=gui_.windows.at(window);w.ended=true;w.dialog_result=result;};
            if(message==0x110){
                auto& d=data();const bool setup=d.opening_setup;d.opening_setup=false;(setup?d.setup_window:d.print_window)=window;
                const auto printer=[&](unsigned control){if(auto* w=child(control)){w->items={{gui_.next_item++,0,0,u"Win64 PDF",0}};w->selection=0;present(control);}};
                if(setup){
                    printer(0x470);
                    if(auto* paper=child(0x471)){paper->items.clear();paper->selection=-1;for(const auto& form:d.forms){if(form.paper==d.settings.paper)paper->selection=static_cast<int>(paper->items.size());
                        paper->items.push_back({gui_.next_item++,0,form.paper,form.form.empty()?u"自訂尺寸":form.form,0});}present(0x471);}
                    if(auto* source=child(0x472)){source->items={{gui_.next_item++,0,7,u"自動選擇",0}};source->selection=0;present(0x472);}
                    for(unsigned control:{0x420U,0x421U})if(auto* radio=child(control)){radio->check=(control==0x421)==d.settings.landscape;present(control);}
                }else{
                    printer(0x473);
                    const auto choice=d.flags&page_numbers?0x422U:d.flags&selection?0x421U:0x420U;const bool pages=!(d.flags&no_page_numbers) && d.minimum!=d.maximum;
                    for(unsigned control=0x420;control<=0x422;++control)if(auto* radio=child(control)){radio->check=control==choice;
                        if(control==0x421)gui_set_window_enabled(*radio,!(d.flags&no_selection));if(control==0x422)gui_set_window_enabled(*radio,pages);present(control);}
                    for(unsigned control:{0x480U,0x481U})if(auto* edit=child(control)){edit->title=number(control==0x480?d.from:d.to);gui_set_window_enabled(*edit,pages);present(control);}
                    if(auto* copies=child(0x482)){copies->title=number(d.settings.copies);present(0x482);}
                    if(auto* box=child(0x411)){box->check=d.settings.collate;present(0x411);}
                    if(auto* file=child(0x410)){file->check=(d.flags&to_file)!=0;gui_set_window_enabled(*file,!(d.flags&disable_file));present(0x410);}
                }
                // 隱藏沿用 ShowWindow(SW_HIDE)，與 WS_VISIBLE 樣式及原生視窗一致。
                if(!setup && (data().flags&hide_file))if(const auto file=gui_child(window,0x410))(void)co_await service_callback::call(resolve("user32.dll","ShowWindow").value,{file,0});
                if(!gui_.classic_prints.contains(id) || !gui_.windows.contains(window))co_return finish(0);
            }
            const bool setup=data().setup_window==window;
            if(const auto hook=setup?data().setup_hook:data().print_hook){
                // 掛勾程序先收到訊息（WM_INITDIALOG 於預設初始化之後，lParam 為 PRINTDLG）；回傳非零即不做預設處理。
                const auto handled=co_await service_callback::call(hook,{window,message,call(2),message==0x110?data().request:call(3)});
                if(message==0x110 || handled)co_return finish(handled);
                if(!gui_.classic_prints.contains(id) || !gui_.windows.contains(window))co_return finish(0);
            }else if(message==0x110)co_return finish(1);
            if(message==0x10){end(0);co_return finish(1);}
            if(message!=0x111)co_return finish(0);
            const auto control=static_cast<unsigned>(call(2)&65535U);
            if(control==2){end(0);co_return finish(1);}
            if(control==0x40e && (data().flags&show_help) && data().owner){
                (void)co_await service_callback::call(resolve("user32.dll","SendMessageW").value,{data().owner,gui_registered_message(u"commdlg_help"),window,data().request});co_return finish(1);}
            if(setup){
                if(control==0x420 || control==0x421){for(unsigned n:{0x420U,0x421U})if(auto* radio=child(n)){radio->check=n==control;present(n);}co_return finish(1);}
                if(control!=1)co_return finish(0);
                auto& d=data();
                if(const auto* paper=child(0x471);paper && paper->selection>=0 && static_cast<std::size_t>(paper->selection)<paper->items.size()){const auto value=paper->items[static_cast<std::size_t>(paper->selection)].data;
                    for(const auto& form:d.forms)if(form.paper==value){d.settings.paper=form.paper;d.settings.form=form.form;d.settings.paper_width=form.paper_width;d.settings.paper_height=form.paper_height;break;}}
                if(const auto* landscape=child(0x421))d.settings.landscape=landscape->check!=0;
                d.settings.width=static_cast<unsigned>((std::uint64_t{d.settings.paper_width}*96+12700)/25400);d.settings.height=static_cast<unsigned>((std::uint64_t{d.settings.paper_height}*96+12700)/25400);
                if(d.settings.landscape)std::swap(d.settings.width,d.settings.height);
                end(1);co_return finish(1);
            }
            if(control>=0x420 && control<=0x422){
                if(const auto* radio=child(control);radio && radio->enabled)for(unsigned n=0x420;n<=0x422;++n)if(auto* other=child(n)){other->check=n==control;present(n);}co_return finish(1);}
            if(control==0x401){
                // 「內容」開啟列印設定（同一個程序與狀態），確定後更新紙張與方向。
                data().opening_setup=true;
                const auto result=co_await service_callback::call(resolve("user32.dll",data().wide?"DialogBoxIndirectParamW":"DialogBoxIndirectParamA").value,
                    {data().instance,data().setup_source,window,resolve("internal-print-dlg","DialogProc").value,id});
                if(gui_.classic_prints.contains(id)){data().setup_window=0;data().opening_setup=false;}(void)result;co_return finish(1);
            }
            if(control!=1)co_return finish(0);
            auto& d=data();auto updated=d;
            if(const auto* copies=child(0x482)){const auto value=integer(copies->title);if(!value || !*value || *value>99){status(u"份數須介於 1 與 99。");co_return finish(1);}updated.settings.copies=*value;}
            unsigned choice=d.flags&page_numbers?0x422U:d.flags&selection?0x421U:0x420U;
            for(unsigned n=0x420;n<=0x422;++n)if(const auto* radio=child(n);radio && radio->check)choice=n;
            if(choice==0x422){
                const auto* first=child(0x480);const auto* last=child(0x481);const auto from=first?integer(first->title):std::optional<unsigned>{d.from},to=last?integer(last->title):std::optional<unsigned>{d.to};
                if(!from || !to || *from<d.minimum || *to>d.maximum || *from>*to){status(u"頁碼須介於 "+number(d.minimum)+u" 與 "+number(d.maximum)+u"，且起始頁不大於結束頁。");co_return finish(1);}
                updated.from=*from;updated.to=*to;
            }
            if(const auto* box=child(0x411))updated.settings.collate=box->check!=0;
            updated.flags=(d.flags&~(selection|page_numbers|collate|to_file))|(choice==0x421?selection:choice==0x422?page_numbers:0U)|(updated.settings.collate?collate:0U);
            if(const auto* file=child(0x410))updated.flags|=file->check?to_file:0U;else updated.flags|=d.flags&to_file;
            d=std::move(updated);end(1);co_return finish(1);
        });
    return nested_continuation(state,memory,"PrintDlg",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto& L=x86_?x86_layout:x64_layout;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto p=call(0);const bool wide=service.symbol=="PrintDlgW";
        const auto fail=[&](std::uint32_t code){gui_.common_dialog_error=code;call.return_to(s,0);return std::optional<std::uint32_t>{};};
        const auto release=[&](std::uint64_t h){if(!h)return;const auto f=gui_.global_memory.find(h);if(f==gui_.global_memory.end())return;const auto at=f->second.address;gui_.global_memory.erase(f);if(at)(void)free_heap(m,at,s);};
        // 對話範本：與 PageSetupDlg 相同的 DLGTEMPLATE 寫法（WS_EX_DLGMODALFRAME、DS_SETFONT，9 點 Segoe UI）；控制項 ID 依 dlgs.h 的標準 ID。
        const auto build=[&](bool setup,std::uint32_t flags){
            std::vector<std::uint8_t> items;unsigned count=0;const auto put=[](std::vector<std::uint8_t>& out,std::uint32_t value,unsigned size){for(unsigned i=0;i<size;++i)out.push_back(static_cast<std::uint8_t>(value>>(8*i)));};
            const auto text=[&](std::vector<std::uint8_t>& out,std::u16string_view value){for(const auto c:value)put(out,c,2);put(out,0,2);};
            const auto item=[&](unsigned id,unsigned cls,std::u16string_view label,unsigned x,unsigned y,unsigned w,unsigned h,std::uint32_t style=0){
                while(items.size()%4)items.push_back(0);put(items,0x50000000U|style,4);put(items,0,4);put(items,x,2);put(items,y,2);put(items,w,2);put(items,h,2);put(items,id,2);put(items,0xffff,2);put(items,cls,2);text(items,label);put(items,0,2);++count;};
            const unsigned bottom=setup?120:170;
            item(0xffff,0x82,u"印表機",10,10,40,12);item(setup?0x470:0x473,0x85,u"",52,8,170,80,0x210003);
            if(setup){
                item(0x432,0x80,u"紙張",10,30,170,70,7);item(0xffff,0x82,u"大小",18,46,30,12);item(0x471,0x85,u"",52,44,120,90,0x210003);
                item(0xffff,0x82,u"來源",18,70,30,12);item(0x472,0x85,u"",52,68,120,70,0x210003);
                item(0x433,0x80,u"方向",190,30,100,70,7);item(0x420,0x80,u"直向",198,46,80,12,0x30009);item(0x421,0x80,u"橫向",198,66,80,12,9);
            }else{
                item(0x401,0x80,u"內容…",230,8,60,14,0x10000);
                item(0x430,0x80,u"列印範圍",10,30,160,92,7);item(0x420,0x80,u"全部",18,44,140,12,0x30009);item(0x421,0x80,u"選取範圍",18,60,140,12,9);item(0x422,0x80,u"頁數",18,76,140,12,9);
                item(0xffff,0x82,u"從",30,96,14,12);item(0x480,0x81,u"",46,94,40,14,0x812080);item(0xffff,0x82,u"到",94,96,14,12);item(0x481,0x81,u"",110,94,40,14,0x812080);
                item(0x431,0x80,u"份數",180,30,110,92,7);item(0xffff,0x82,u"份數",188,46,30,12);item(0x482,0x81,u"",222,44,40,14,0x812080);item(0x411,0x80,u"自動分頁",188,66,90,12,0x10003);
                item(0x410,0x80,u"列印到檔案",10,128,140,12,0x10003);
            }
            item(499,0x82,u"",10,bottom-22,280,14);item(1,0x80,u"確定",180,bottom,50,16,0x10001);item(2,0x80,u"取消",240,bottom,50,16,0x10000);
            if(flags&show_help)item(0x40e,0x80,u"說明",10,bottom,50,16,0x10000);
            std::vector<std::uint8_t> bytes;put(bytes,0x90c80040U,4);put(bytes,1,4);put(bytes,count,2);put(bytes,30,2);put(bytes,30,2);put(bytes,300,2);put(bytes,bottom+24,2);put(bytes,0,2);put(bytes,0,2);
            text(bytes,setup?u"列印設定":u"列印");put(bytes,9,2);text(bytes,u"Segoe UI");bytes.insert(bytes.end(),items.begin(),items.end());return bytes;
        };
        GuiClassicPrint data;std::uint32_t error=2;std::uint64_t print_source=0;std::vector<std::uint8_t> print_bytes,setup_bytes;
        try{
            if(!p)co_return fail(2);if(m.load(GuestAddress{p},32)!=L.size)co_return fail(1);
            m.check(GuestAddress{p},L.size,Access::Write);
            const auto get=[&](unsigned offset,unsigned width){return m.load(GuestAddress{p+offset},static_cast<std::uint8_t>(width));};
            data.request=p;data.wide=wide;data.owner=get(L.owner,bits);data.flags=static_cast<std::uint32_t>(get(L.flags,32));data.instance=get(L.instance,bits);
            if(data.owner && !gui_.windows.contains(data.owner))co_return fail(2);
            if((data.flags&show_help) && !data.owner)co_return fail(2);
            data.print_hook=data.flags&print_hook?get(L.print_hook,bits):0;data.setup_hook=data.flags&setup_hook?get(L.setup_hook,bits):0;
            if(((data.flags&print_hook) && !data.print_hook) || ((data.flags&setup_hook) && !data.setup_hook))co_return fail(11);
            for(const auto h:{data.print_hook,data.setup_hook})if(h && !handles(GuestAddress{h}))m.check(GuestAddress{h},1,Access::Execute);
            const auto mode=get(L.mode,bits),names=get(L.names,bits);
            if((data.flags&return_default) && (mode || names))co_return fail(0x1003);
            if(mode && mode==names)co_return fail(10);
            for(const auto h:{mode,names})if(h){const auto f=gui_.global_memory.find(h);if(f==gui_.global_memory.end() || !(f->second.flags&2U) || !f->second.address || f->second.locks || f->second.clipboard_owned)co_return fail(10);}
            if(mode){const auto& b=gui_.global_memory.at(mode);if(b.size<(wide?104U:72U))co_return fail(0x1005);
                const auto size=m.load(GuestAddress{b.address+(wide?68:36)},16),extra=m.load(GuestAddress{b.address+(wide?70:38)},16);
                if(size+extra>b.size || read_printer_settings(m,b.address,wide,data.settings))co_return fail(0x1005);}
            if(names){const auto& b=gui_.global_memory.at(names);if(b.size<8)co_return fail(0x100b);const unsigned character=wide?2:1;
                for(unsigned field:{0U,2U,4U}){const auto offset=m.load(GuestAddress{b.address+field},16)*character;if(offset<8 || offset>=b.size)co_return fail(0x100b);std::u16string value;bool terminated=false;
                    for(auto at=offset;at+character<=b.size;at+=character){const auto c=m.load(GuestAddress{b.address+at},static_cast<std::uint8_t>(character*8));if(!c){terminated=true;break;}value.push_back(static_cast<char16_t>(c));}
                    if(!terminated || (field==2 && value!=u"Win64 PDF"))co_return fail(0x100b);}}
            if(!mode){const auto copies=static_cast<unsigned>(get(L.copies,16));data.settings.copies=std::clamp(copies,1U,99U);data.settings.collate=(data.flags&collate)!=0;}
            data.from=static_cast<std::uint32_t>(get(L.from,16));data.to=static_cast<std::uint32_t>(get(L.to,16));data.minimum=static_cast<std::uint32_t>(get(L.minimum,16));data.maximum=static_cast<std::uint32_t>(get(L.maximum,16));
            // PD_PAGENUMS 時起訖頁超出最小／最大頁為初始化錯誤；否則顯示前夾到範圍內。
            if(!(data.flags&(return_default|setup_only)) && (data.flags&page_numbers) && (data.minimum>data.maximum || data.from<data.minimum || data.from>data.maximum || data.to<data.minimum || data.to>data.maximum))co_return fail(0x1006);
            if(data.minimum<=data.maximum){data.from=std::clamp(data.from,data.minimum,data.maximum);data.to=std::clamp(data.to,data.minimum,data.maximum);}
            unsigned forms_error=0;data.forms=available_printer_settings(forms_error);if(forms_error || data.forms.empty())co_return fail(0x1008);
            if(!data.settings.paper)data.forms.push_back(data.settings);
            if(!(data.flags&return_default)){
                // 自訂範本：handle 版優先於資源名稱版；沒有指定時用內建範本。
                const auto source=[&](bool setup,std::uint64_t& address,std::vector<std::uint8_t>& builtin)->std::uint32_t{
                    const auto handle_flag=setup?setup_handle:print_handle,name_flag=setup?setup_template:print_template;
                    if(data.flags&handle_flag){error=8;const auto h=get(setup?L.setup_handle:L.print_handle,bits);const auto f=gui_.global_memory.find(h);if(f==gui_.global_memory.end() || !f->second.address)return 8;
                        address=f->second.address;(void)gui_dialog_layout(m,address,f->second.size);return 0;}
                    if(data.flags&name_flag){if(!data.instance)return 4;error=6;const auto name=get(setup?L.setup_name:L.print_name,bits);if(!name)return 3;
                        const auto entry=gui_resource(m,data.instance,name,5,wide);if(!entry)return 6;address=resource_image(entry,true)->base+m.load(GuestAddress{entry},32);
                        (void)gui_dialog_layout(m,address,m.load(GuestAddress{entry+4},32));return 0;}
                    error=9;builtin=build(setup,data.flags);return 0;};
                if(const auto code=source(true,data.setup_source,setup_bytes))co_return fail(code);
                error=2;
                if(!(data.flags&setup_only))if(const auto code=source(false,print_source,print_bytes))co_return fail(code);
            }
            error=9;if(gui_.classic_prints.size()>=64)co_return fail(9);
        }catch(const MemoryFault&){co_return fail(error);}catch(const HostCallFault&){co_return fail(error);}catch(const std::bad_alloc&){co_return fail(9);}catch(const std::invalid_argument&){co_return fail(error);}
        // 確定後才配置輸出：DC 以完整份數建立；沒有 PD_USEDEVMODECOPIES 時對外 DEVMODE 為單份、不逐份，份數由 nCopies 交給應用程式。
        const auto commit=[&](const GuiClassicPrint& d,bool defaults)->std::uint32_t{
            const std::array old{m.load(GuestAddress{p+L.mode},bits),m.load(GuestAddress{p+L.names},bits)};if(old[0] && old[0]==old[1])return 10;
            for(const auto h:old)if(h){const auto f=gui_.global_memory.find(h);if(f==gui_.global_memory.end() || f->second.locks || f->second.clipboard_owned)return 10;}
            const auto globals=allocate_printer_globals(s,m,d.wide,d.settings);if(!globals[0])return 9;
            std::uint64_t dc=0;
            if(d.flags&0x300U){dc=create_printer_dc(m,gui_.global_memory.at(globals[0]).address,d.wide,(d.flags&0x100U)==0);if(!dc){for(const auto h:globals)release(h);return 0x100a;}}
            const bool device_copies=(d.flags&devmode_copies)!=0;
            if(!device_copies){auto exported=d.settings;exported.copies=1;exported.collate=false;m.write(GuestAddress{gui_.global_memory.at(globals[0]).address},printer_devmode(d.wide,exported));}
            for(const auto h:old)release(h);
            m.store(GuestAddress{p+L.mode},bits,globals[0]);m.store(GuestAddress{p+L.names},bits,globals[1]);if(d.flags&0x300U)m.store(GuestAddress{p+L.dc},bits,dc);
            if(!defaults){
                m.store(GuestAddress{p+L.flags},32,device_copies?d.flags&~collate:d.flags);
                m.store(GuestAddress{p+L.from},16,d.from);m.store(GuestAddress{p+L.to},16,d.to);m.store(GuestAddress{p+L.copies},16,device_copies?1U:d.settings.copies);
            }
            return 0;};
        if(data.flags&return_default){
            std::uint32_t code=0;try{code=commit(data,true);}catch(const MemoryFault&){code=2;}catch(const std::bad_alloc&){code=9;}
            if(code)co_return fail(code);gui_.common_dialog_error=0;call.return_to(s,1);co_return std::optional<std::uint32_t>{};
        }
        GuestScratch scratch(*this,m,s);
        const auto place=[&](const std::vector<std::uint8_t>& bytes)->std::uint64_t{const auto at=scratch.allocate(bytes.size());if(at)m.write(GuestAddress{at},bytes);return at;};
        if(!setup_bytes.empty() && !(data.setup_source=place(setup_bytes)))co_return fail(9);
        if(!print_bytes.empty() && !(print_source=place(print_bytes)))co_return fail(9);
        const auto id=gui_.next_object++;data.opening_setup=(data.flags&setup_only)!=0;
        const auto instance=data.instance,owner=data.owner,source=data.flags&setup_only?data.setup_source:print_source;gui_.classic_prints.emplace(id,std::move(data));
        const auto result=co_await service_callback::call(resolve("user32.dll",wide?"DialogBoxIndirectParamW":"DialogBoxIndirectParamA").value,
            {instance,source,owner,resolve("internal-print-dlg","DialogProc").value,id});
        const auto ended=gui_.classic_prints.find(id);if(ended==gui_.classic_prints.end())co_return fail(0xffff);
        const auto final=std::move(ended->second);gui_.classic_prints.erase(ended);
        if(result==UINT32_MAX)co_return fail(0xffff);
        if(result!=1)co_return fail(0);
        std::uint32_t code=0;try{code=commit(final,false);}catch(const MemoryFault&){code=2;}catch(const std::bad_alloc&){code=9;}
        if(code)co_return fail(code);
        gui_.common_dialog_error=0;call.return_to(s,1);co_return std::optional<std::uint32_t>{};
    });
}
