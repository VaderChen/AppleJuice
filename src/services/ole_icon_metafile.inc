// OLE 圖示中繼檔與就地啟動選單（由 windows_legacy_user.cpp 的 invoke_legacy_user 引入）：
// - OleMetafilePictFromIconAndLabel：Windows 中繼檔（MM_ANISOTROPIC，HIMETRIC 範圍）畫圖示（置中）、MFCOMMENT「IconOnly」、標籤（圖示標題字型、置中於圖示下方），
//   最後以 MFCOMMENT「OLE icon source」記下來源檔與索引；回傳可移動全域記憶體中的 METAFILEPICT。
// - OleGetIconOfFile：ExtractAssociatedIcon 的圖示，fUseFileAsLabel 時以檔名（不含路徑）為標籤。
// - OleGetIconOfClass：HKCR\CLSID\{clsid}\DefaultIcon（「路徑,索引」），其次 LocalServer32／InprocServer32 的第一個圖示，最後是 IDI_APPLICATION；
//   fUseTypeAsLabel 時以 OleRegGetUserType 的短名稱為標籤，否則用 lpszLabel。
// - OleSetMenuDescriptor：以 SetWindowSubclass 掛在框架視窗；依 OLEMENUGROUPWIDTHS 判斷頂層選單屬於容器（偶數群組）或物件（奇數群組），
//   物件選單的 WM_INITMENUPOPUP／WM_MENUSELECT／選單 WM_COMMAND 轉送給物件視窗；holemenu 為 NULL 時移除。
if(name=="OleMetafilePictFromIconAndLabel" || name=="OleGetIconOfFile" || name=="OleGetIconOfClass" || name=="OleSetMenuDescriptor" || service.family=="internal-ole-menu"){
    return nested_continuation(state,memory,"OLE 圖示與選單",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto symbol=service.symbol;const unsigned width=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(width*8);
        const auto done=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const auto api=[&](std::string_view dll,std::string_view entry){return resolve(dll,entry).value;};
        const auto value_of=[&](std::uint64_t v){return x86_?v&UINT32_MAX:v;};
        GuestScratch scratch(*this,m,s);
        const auto text_at=[&](std::u16string_view text)->std::uint64_t{const auto bytes=utf16_bytes(text);const auto at=scratch.allocate(bytes.size());if(at)m.write(GuestAddress{at},bytes);return at;};
        const auto read=[&](std::uint64_t at){return at?read_utf16(m,GuestAddress{at},32768):std::u16string{};};
        if(service.family=="internal-ole-menu"){
            // SUBCLASSPROC(hWnd, uMsg, wParam, lParam, uIdSubclass, dwRefData)：dwRefData 是 OleCreateMenuDescriptor 的 HOLEMENU。
            const auto window=call(0),wparam=call(2),lparam=call(3),descriptor=call(5);const auto message=static_cast<std::uint32_t>(call(1));
            const auto fallback=[&]()->service_callback::Task<std::uint64_t>{co_return co_await service_callback::call_value(api("comctl32.dll","DefSubclassProc"),{window,message,wparam,lparam});};
            if(message==0x82){(void)co_await service_callback::call(api("comctl32.dll","RemoveWindowSubclass"),{window,api("internal-ole-menu","SubclassProc"),0x4f4c45});gui_.ole_menu_object.erase(window);co_return done(co_await fallback());}
            if(message!=0x117 && message!=0x11f && message!=0x111)co_return done(co_await fallback());
            const auto data=co_await service_callback::call_value(api("kernel32.dll","GlobalLock"),{descriptor});if(!data)co_return done(co_await fallback());
            const auto active=value_of(m.load(GuestAddress{data+width},bits)),combined=value_of(m.load(GuestAddress{data+2*width+24},bits));std::array<std::uint32_t,6> widths{};
            for(unsigned i=0;i<6;++i)widths[i]=static_cast<std::uint32_t>(m.load(GuestAddress{data+2*width+i*4},32));
            (void)co_await service_callback::call(api("kernel32.dll","GlobalUnlock"),{descriptor});
            const auto object_group=[&](std::uint32_t position){std::uint32_t start=0;for(unsigned group=0;group<6;++group){if(position<start+widths[group])return (group&1U)!=0;start+=widths[group];}return false;};
            const auto top_index=[&](std::uint64_t popup)->service_callback::Task<std::int32_t>{
                const auto count=static_cast<std::int32_t>(co_await service_callback::call(api("user32.dll","GetMenuItemCount"),{combined}));
                for(std::int32_t i=0;i<count;++i)if(value_of(co_await service_callback::call_value(api("user32.dll","GetSubMenu"),{combined,static_cast<std::uint64_t>(i)}))==value_of(popup))co_return i;
                co_return -1;
            };
            if(message==0x117 && !((lparam>>16)&0xffffU)){
                // 頂層選單才重新判斷擁有者；子選單沿用目前的擁有者。
                const auto index=co_await top_index(wparam);if(index>=0)gui_.ole_menu_object[window]=object_group(static_cast<std::uint32_t>(index));
            }else if(message==0x11f && value_of(lparam)==combined && combined){
                const auto flags=(wparam>>16)&0xffffU,item=wparam&0xffffU;if(flags!=0xffffU)gui_.ole_menu_object[window]=object_group(static_cast<std::uint32_t>(item));
            }
            const auto owner=gui_.ole_menu_object.find(window);const bool to_object=owner!=gui_.ole_menu_object.end() && owner->second && active;
            if(message==0x111){
                if(lparam || ((wparam>>16)&0xffffU) || !to_object)co_return done(co_await fallback());
                gui_.ole_menu_object.erase(window);co_return done(co_await service_callback::call_value(api("user32.dll","SendMessageW"),{active,message,wparam,lparam}));
            }
            if(to_object)co_return done(co_await service_callback::call_value(api("user32.dll","SendMessageW"),{active,message,wparam,lparam}));
            co_return done(co_await fallback());
        }
        if(symbol=="OleSetMenuDescriptor"){
            // (holemenu, hwndFrame, hwndActiveObject, lpFrame, lpActiveObj)
            const auto descriptor=call(0),frame=call(1),active=call(2);if(!frame || !gui_.windows.contains(frame))co_return done(0x80070057U);
            const auto procedure=api("internal-ole-menu","SubclassProc");
            if(!descriptor){(void)co_await service_callback::call(api("comctl32.dll","RemoveWindowSubclass"),{frame,procedure,0x4f4c45});gui_.ole_menu_object.erase(frame);co_return done(0);}
            const auto data=co_await service_callback::call_value(api("kernel32.dll","GlobalLock"),{descriptor});if(!data)co_return done(0x80070057U);
            m.store(GuestAddress{data},bits,frame);m.store(GuestAddress{data+width},bits,active);(void)co_await service_callback::call(api("kernel32.dll","GlobalUnlock"),{descriptor});
            if(!co_await service_callback::call(api("comctl32.dll","SetWindowSubclass"),{frame,procedure,0x4f4c45,descriptor}))co_return done(0x80004005U);
            gui_.ole_menu_object.erase(frame);co_return done(0);
        }
        // 圖示＋標籤 → METAFILEPICT。
        const auto make_picture=[&](std::uint64_t icon,const std::u16string& label,const std::u16string& source,std::uint32_t index)->service_callback::Task<std::uint64_t>{
            if(!icon)co_return 0;
            const auto icon_width=static_cast<std::int32_t>(co_await service_callback::call(api("user32.dll","GetSystemMetrics"),{11})),icon_height=static_cast<std::int32_t>(co_await service_callback::call(api("user32.dll","GetSystemMetrics"),{12}));
            const auto logfont=scratch.allocate(92),extent=scratch.allocate(8);if(!logfont || !extent)co_return 0;
            std::uint64_t font=0;
            if(co_await service_callback::call(api("user32.dll","SystemParametersInfoW"),{0x1f,92,logfont,0}))font=value_of(co_await service_callback::call_value(api("gdi32.dll","CreateFontIndirectW"),{logfont}));
            std::int32_t label_width=0,label_height=0;const auto label_text=label.empty()?0:text_at(label);
            if(label_text){
                const auto screen=value_of(co_await service_callback::call_value(api("user32.dll","GetDC"),{0}));
                const auto previous=font?value_of(co_await service_callback::call_value(api("gdi32.dll","SelectObject"),{screen,font})):0;
                if(co_await service_callback::call(api("gdi32.dll","GetTextExtentPoint32W"),{screen,label_text,label.size(),extent})){label_width=static_cast<std::int32_t>(m.load(GuestAddress{extent},32));label_height=static_cast<std::int32_t>(m.load(GuestAddress{extent+4},32));}
                if(previous)(void)co_await service_callback::call(api("gdi32.dll","SelectObject"),{screen,previous});(void)co_await service_callback::call(api("user32.dll","ReleaseDC"),{0,screen});
            }
            const auto picture_width=std::max(icon_width,label_width),picture_height=icon_height+label_height;
            const auto dc=value_of(co_await service_callback::call_value(api("gdi32.dll","CreateMetaFileW"),{0}));if(!dc){if(font)(void)co_await service_callback::call(api("gdi32.dll","DeleteObject"),{font});co_return 0;}
            const auto signed_value=[](std::int32_t v){return static_cast<std::uint64_t>(static_cast<std::uint32_t>(v));};
            (void)co_await service_callback::call(api("gdi32.dll","SetWindowOrgEx"),{dc,0,0,0});(void)co_await service_callback::call(api("gdi32.dll","SetWindowExtEx"),{dc,signed_value(picture_width),signed_value(picture_height),0});
            (void)co_await service_callback::call(api("user32.dll","DrawIcon"),{dc,signed_value((picture_width-icon_width)/2),0,icon});
            const auto comment=[&](std::string_view bytes)->service_callback::Task<bool>{const auto at=scratch.allocate(bytes.size()+1);if(!at)co_return false;
                std::vector<std::uint8_t> data(bytes.begin(),bytes.end());data.push_back(0);m.write(GuestAddress{at},data);
                co_return co_await service_callback::call(api("gdi32.dll","Escape"),{dc,15,data.size(),at,0})!=0;};
            (void)co_await comment("IconOnly");
            if(label_text){
                if(font)(void)co_await service_callback::call(api("gdi32.dll","SelectObject"),{dc,font});
                (void)co_await service_callback::call(api("gdi32.dll","SetBkMode"),{dc,1});
                (void)co_await service_callback::call(api("gdi32.dll","TextOutW"),{dc,signed_value((picture_width-label_width)/2),signed_value(icon_height),label_text,label.size()});
            }
            (void)co_await comment("OLE icon source\n"+utf16_to_utf8(source)+"\n"+std::to_string(index));
            const auto metafile=value_of(co_await service_callback::call_value(api("gdi32.dll","CloseMetaFile"),{dc}));if(font)(void)co_await service_callback::call(api("gdi32.dll","DeleteObject"),{font});
            if(!metafile)co_return 0;
            const auto global=value_of(co_await service_callback::call_value(api("kernel32.dll","GlobalAlloc"),{0x2002,x86_?16U:24U}));
            if(!global){(void)co_await service_callback::call(api("gdi32.dll","DeleteMetaFile"),{metafile});co_return 0;}
            const auto picture=co_await service_callback::call_value(api("kernel32.dll","GlobalLock"),{global});
            // HIMETRIC：像素 × 2540 ／ 96。
            m.store(GuestAddress{picture},32,8);m.store(GuestAddress{picture+4},32,static_cast<std::uint32_t>(picture_width*2540/96));m.store(GuestAddress{picture+8},32,static_cast<std::uint32_t>(picture_height*2540/96));
            m.store(GuestAddress{picture+(x86_?12U:16U)},bits,metafile); // METAFILEPICT：mm、xExt、yExt、hMF（x64 對齊到 16）(void)co_await service_callback::call(api("kernel32.dll","GlobalUnlock"),{global});
            co_return global;
        };
        if(symbol=="OleMetafilePictFromIconAndLabel")
            co_return done(co_await make_picture(call(0),read(call(1)),read(call(2)),static_cast<std::uint32_t>(call(3))));
        if(symbol=="OleGetIconOfFile"){
            if(!call(0))co_return done(0);auto path=read(call(0));if(path.empty() || path.size()>=260)co_return done(0);
            const auto buffer=scratch.allocate(520),index=scratch.allocate(4);if(!buffer || !index)co_return done(0);
            m.write(GuestAddress{buffer},std::vector<std::uint8_t>(520));m.write(GuestAddress{buffer},utf16_bytes(path));m.store(GuestAddress{index},16,0);
            const auto icon=value_of(co_await service_callback::call_value(api("shell32.dll","ExtractAssociatedIconW"),{0,buffer,index}));if(!icon)co_return done(0);
            std::u16string label;if(static_cast<std::uint32_t>(call(1))){const auto slash=path.find_last_of(u"\\/");label=slash==path.npos?path:path.substr(slash+1);}
            const auto picture=co_await make_picture(icon,label,read_utf16(m,GuestAddress{buffer},260),static_cast<std::uint32_t>(m.load(GuestAddress{index},16)));
            (void)co_await service_callback::call(api("user32.dll","DestroyIcon"),{icon});co_return done(picture);
        }
        // OleGetIconOfClass(rclsid, lpszLabel, fUseTypeAsLabel)
        if(!call(0))co_return done(0);
        const auto guid=scratch.allocate(80);if(!guid || !co_await service_callback::call(api("ole32.dll","StringFromGUID2"),{call(0),guid,40}))co_return done(0);
        const auto clsid=read_utf16(m,GuestAddress{guid},40);
        const auto registry_text=[&](const std::u16string& key)->service_callback::Task<std::u16string>{
            const auto key_text=text_at(key),slot=scratch.allocate(8),size=scratch.allocate(4),value=scratch.allocate(1040);if(!key_text || !slot || !size || !value)co_return std::u16string{};
            if(co_await service_callback::call(api("advapi32.dll","RegOpenKeyExW"),{x86_?0x80000000U:0xffffffff80000000ULL,key_text,0,0x20019,slot}))co_return std::u16string{};
            const auto handle=m.load(GuestAddress{slot},bits);m.store(GuestAddress{size},32,1040);m.write(GuestAddress{value},std::vector<std::uint8_t>(1040));
            const auto error=co_await service_callback::call(api("advapi32.dll","RegQueryValueExW"),{handle,0,0,0,value,size});(void)co_await service_callback::call(api("advapi32.dll","RegCloseKey"),{handle});
            co_return error?std::u16string{}:read_utf16(m,GuestAddress{value},519);
        };
        std::u16string source;std::uint32_t index=0;std::uint64_t icon=0;bool extracted=false;
        if(auto location=co_await registry_text(u"CLSID\\"+clsid+u"\\DefaultIcon");!location.empty()){
            const auto comma=location.find_last_of(u',');source=location.substr(0,comma);
            if(comma!=location.npos){try{index=static_cast<std::uint32_t>(std::stoi(utf16_to_utf8(location.substr(comma+1))));}catch(...){index=0;}}
        }
        if(source.empty())source=co_await registry_text(u"CLSID\\"+clsid+u"\\LocalServer32");
        if(source.empty())source=co_await registry_text(u"CLSID\\"+clsid+u"\\InprocServer32");
        if(source.size()>1 && source.front()==u'"'){const auto end=source.find(u'"',1);source=source.substr(1,end==source.npos?source.npos:end-1);}
        if(!source.empty()){const auto path=text_at(source);if(path)icon=value_of(co_await service_callback::call_value(api("shell32.dll","ExtractIconW"),{0,path,index}));if(icon==1)icon=0;extracted=icon!=0;}
        if(!icon){icon=value_of(co_await service_callback::call_value(api("user32.dll","LoadIconW"),{0,32512}));source.clear();index=0;}
        std::u16string label=read(call(1));
        if(static_cast<std::uint32_t>(call(2))){
            const auto slot=scratch.allocate(8);if(slot){m.store(GuestAddress{slot},bits,0);
                if(!co_await service_callback::call(api("ole32.dll","OleRegGetUserType"),{call(0),2,slot})){const auto type=m.load(GuestAddress{slot},bits);if(type){label=read(type);(void)co_await service_callback::call(api("ole32.dll","CoTaskMemFree"),{type});}}}
        }
        const auto picture=co_await make_picture(icon,label,source,index);
        if(extracted)(void)co_await service_callback::call(api("user32.dll","DestroyIcon"),{icon});
        co_return done(picture);
    });
}
