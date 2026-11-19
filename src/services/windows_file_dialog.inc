// IFileOpenDialog／IFileSaveDialog（CLSID_FileOpenDialog／CLSID_FileSaveDialog，comdlg32 的類別工廠）：物件狀態在主機，Show 依 FOS 選項轉成
// OPENFILENAMEW 交給既有的 GetOpenFileNameW／GetSaveFileNameW（FOS_PICKFOLDERS 改用既有的 SHBrowseForFolderW），結果以既有的 IShellItem／
// IShellItemArray 物件回傳；不另寫一套檔案瀏覽介面。由 invoke_common_dialog 引入。
if(service.family=="internal-file-dialog" || service.family=="internal-file-dialog-factory" || (service.family=="comdlg32.dll" && service.symbol=="DllGetClassObject")){
    using DialogGuid=std::array<std::uint8_t,16>;
    static constexpr auto make=[](std::uint32_t a,std::uint16_t b,std::uint16_t c,std::array<std::uint8_t,8> tail){DialogGuid g{};for(unsigned i=0;i<4;++i)g[i]=static_cast<std::uint8_t>(a>>(8*i));
        g[4]=static_cast<std::uint8_t>(b);g[5]=static_cast<std::uint8_t>(b>>8);g[6]=static_cast<std::uint8_t>(c);g[7]=static_cast<std::uint8_t>(c>>8);for(unsigned i=0;i<8;++i)g[8+i]=tail[i];return g;};
    static constexpr DialogGuid open_clsid=make(0xdc1c5a9cU,0xe88a,0x4dde,{0xa5,0xa1,0x60,0xf8,0x2a,0x20,0xae,0xf7}),save_clsid=make(0xc0b4e2f3U,0xba21,0x4773,{0x8d,0xba,0x33,0x5e,0xc9,0x46,0xeb,0x8b}),
        unknown_iid=make(0,0,0,{0xc0,0,0,0,0,0,0,0x46}),factory_iid=make(1,0,0,{0xc0,0,0,0,0,0,0,0x46}),modal_iid=make(0xb4db1657U,0x70d7,0x485e,{0x8e,0x3e,0x6f,0xcb,0x5a,0x5c,0x18,0x02}),
        dialog_iid=make(0x42f85136U,0xdb7e,0x439c,{0x85,0xf1,0xe4,0x07,0x5d,0x13,0x5f,0xc8}),open_iid=make(0xd57c7288U,0xd4ad,0x4768,{0xbe,0x02,0x9d,0x96,0x95,0x32,0xd9,0x60}),
        save_iid=make(0x84bccd23U,0x5fde,0x4cdb,{0xae,0xa4,0xaf,0x64,0xb8,0x3d,0x78,0xab});
    static constexpr std::uint32_t s_ok=0,s_false=1,e_notimpl=0x80004001U,e_nointerface=0x80004002U,e_pointer=0x80004003U,e_fail=0x80004005U,e_unexpected=0x8000ffffU,
        e_outofmemory=0x8007000eU,e_invalidarg=0x80070057U,cancelled=0x800704c7U,class_unavailable=0x80040111U,no_aggregation=0x80040110U;
    // IFileDialog 的 27 個方法（含 IUnknown、IModalWindow::Show），之後是 IFileOpenDialog 或 IFileSaveDialog 的方法。
    static constexpr std::array<std::string_view,27> shared{"QueryInterface","AddRef","Release","Show","SetFileTypes","SetFileTypeIndex","GetFileTypeIndex","Advise","Unadvise",
        "SetOptions","GetOptions","SetDefaultFolder","SetFolder","GetFolder","GetCurrentSelection","SetFileName","GetFileName","SetTitle","SetOkButtonLabel","SetFileNameLabel",
        "GetResult","AddPlace","SetDefaultExtension","Close","SetClientGuid","ClearClientData","SetFilter"};
    static constexpr std::array<std::string_view,2> open_methods{"GetResults","GetSelectedItems"};
    static constexpr std::array<std::string_view,5> save_methods{"SetSaveAsItem","SetProperties","SetCollectedProperties","GetProperties","ApplyProperties"};
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto supports=[](bool save,const DialogGuid& iid){return iid==unknown_iid || iid==modal_iid || iid==dialog_iid || iid==(save?save_iid:open_iid);};
    if(service.family!="internal-file-dialog"){
        // 類別工廠與 DllGetClassObject 不呼叫客體程式碼，同步完成；CoCreateInstance 也經由這裡建立物件。
        const WindowsCall arg(state,memory,service);const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto read_guid=[&](std::uint64_t at){DialogGuid g{};memory.read(GuestAddress{at},g);return g;};
        if(service.family=="comdlg32.dll"){
            if(!arg(2))return done(e_pointer);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(0) || !arg(1))return done(e_invalidarg);
            const auto clsid=read_guid(arg(0));if(clsid!=open_clsid && clsid!=save_clsid)return done(class_unavailable);
            const auto iid=read_guid(arg(1));if(iid!=unknown_iid && iid!=factory_iid)return done(e_nointerface);
            if(gui_.file_dialog_factories.size()>=4096)return done(e_outofmemory);
            if(!gui_.file_dialog_factory_vtable){const auto table=runtime_allocate(memory,{},5*unit);unsigned i=0;
                for(const auto name:{"QueryInterface","AddRef","Release","CreateInstance","LockServer"})memory.store(GuestAddress{table+unit*i++},bits,resolve("internal-file-dialog-factory",name).value);
                gui_.file_dialog_factory_vtable=table;}
            const auto factory=allocate_heap(memory,unit);if(!factory)return done(e_outofmemory);memory.store(GuestAddress{factory},bits,gui_.file_dialog_factory_vtable);
            gui_.file_dialog_factories.emplace(factory,GuiFileDialogFactory{clsid==save_clsid,1});memory.store(GuestAddress{arg(2)},bits,factory);return done(s_ok);
        }
        const auto found=gui_.file_dialog_factories.find(arg(0));
        if(found==gui_.file_dialog_factories.end())return done(service.symbol=="AddRef" || service.symbol=="Release"?0:e_pointer);
        auto& factory=found->second;
        if(service.symbol=="AddRef")return done(++factory.references);
        if(service.symbol=="Release"){const auto left=--factory.references;if(!left){gui_.file_dialog_factories.erase(found);(void)free_heap(memory,arg(0),state);}return done(left);}
        if(service.symbol=="LockServer"){if(arg(1))++gui_.file_dialog_locks;else if(gui_.file_dialog_locks)--gui_.file_dialog_locks;return done(s_ok);}
        if(service.symbol=="QueryInterface"){if(!arg(2))return done(e_pointer);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(e_invalidarg);
            const auto iid=read_guid(arg(1));if(iid!=unknown_iid && iid!=factory_iid)return done(e_nointerface);++factory.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return done(s_ok);}
        // CreateInstance(pUnkOuter, riid, ppv)
        if(!arg(3))return done(e_pointer);memory.store(GuestAddress{arg(3)},bits,0);if(arg(1))return done(no_aggregation);if(!arg(2))return done(e_invalidarg);
        const bool save=factory.save;const auto iid=read_guid(arg(2));if(!supports(save,iid))return done(e_nointerface);
        if(gui_.file_dialogs.size()>=1024)return done(e_outofmemory);
        auto& cache=save?gui_.file_save_dialog_vtable:gui_.file_open_dialog_vtable;
        if(!cache){const auto count=static_cast<unsigned>(shared.size()+(save?save_methods.size():open_methods.size()));const auto table=runtime_allocate(memory,{},count*unit);unsigned i=0;
            const auto put=[&](std::string_view name){memory.store(GuestAddress{table+unit*i++},bits,resolve("internal-file-dialog",name).value);};
            for(const auto name:shared)put(name);if(save)for(const auto name:save_methods)put(name);else for(const auto name:open_methods)put(name);cache=table;}
        const auto object=allocate_heap(memory,unit);if(!object)return done(e_outofmemory);memory.store(GuestAddress{object},bits,cache);
        // 預設選項：開啟 FOS_PATHMUSTEXIST｜FOS_FILEMUSTEXIST｜FOS_NOCHANGEDIR；另存 FOS_OVERWRITEPROMPT｜FOS_NOREADONLYRETURN｜FOS_PATHMUSTEXIST｜FOS_NOCHANGEDIR。
        GuiFileDialog dialog;dialog.save=save;dialog.options=save?0x880aU:0x1808U;gui_.file_dialogs.emplace(object,std::move(dialog));
        memory.store(GuestAddress{arg(3)},bits,object);return done(s_ok);
    }
    return nested_continuation(state,memory,"檔案對話方塊",[this,service,supports](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto object=call(0);const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        if(!gui_.file_dialogs.contains(object))co_return finish(name=="AddRef" || name=="Release"?0:e_pointer);
        const auto d=[&]()->GuiFileDialog&{return gui_.file_dialogs.at(object);};
        const auto api=[&](std::string_view dll,std::string_view symbol){return resolve(dll,symbol).value;};
        const auto method=[&](std::uint64_t target,unsigned slot){const auto table=m.load(GuestAddress{target},bits);if(!table)throw HostCallFault("檔案對話方塊回呼的 COM vtable 為空");
            const auto entry=m.load(GuestAddress{table+slot*unit},bits);if(!entry)throw HostCallFault("檔案對話方塊回呼的 COM 方法為空");return entry;};
        const auto text=[&](std::uint64_t at){return at?read_utf16(m,GuestAddress{at}):std::u16string{};};
        const auto parent=[](const std::u16string& path){const auto slash=path.find_last_of(u"\\/");if(slash==path.npos)return std::u16string{};
            return slash==2 && path.size()>1 && path[1]==u':'?path.substr(0,3):path.substr(0,slash);};
        const auto leaf=[](const std::u16string& path){const auto slash=path.find_last_of(u"\\/");return slash==path.npos?path:path.substr(slash+1);};
        const auto out=[&](unsigned index)->bool{const auto at=call(index);if(!at)return false;m.check(GuestAddress{at},unit,Access::Write);m.store(GuestAddress{at},bits,0);return true;};
        const auto item=[&](unsigned index,const std::u16string& path)->std::uint32_t{try{m.store(GuestAddress{call(index)},bits,create_shell_item(m,path));return s_ok;}catch(const std::bad_alloc&){return e_outofmemory;}};
        // 項目路徑：本引擎的 IShellItem 直接取用；其他實作以 GetDisplayName(SIGDN_FILESYSPATH) 取得。
        const auto item_path=[&](std::uint64_t target)->service_callback::Task<std::optional<std::u16string>>{
            if(const auto host=gui_.shell_items.find(target);host!=gui_.shell_items.end())co_return host->second.path;
            GuestScratch slot(*this,m,s);const auto at=slot.allocate(unit);if(!at)co_return std::nullopt;m.store(GuestAddress{at},bits,0);
            const auto hr=co_await service_callback::call(method(target,5),{target,0x80058000U,at});const auto pointer=m.load(GuestAddress{at},bits);
            if(hr&0x80000000U || !pointer)co_return std::nullopt;auto path=read_utf16(m,GuestAddress{pointer});(void)co_await service_callback::call(api("ole32.dll","CoTaskMemFree"),{pointer});co_return path;};
        const auto release=[&](std::uint64_t target)->service_callback::Task<int>{if(target)(void)co_await service_callback::call(method(target,2),{target});co_return 0;};
        const auto add_ref=[&](std::uint64_t target)->service_callback::Task<int>{if(target)(void)co_await service_callback::call(method(target,1),{target});co_return 0;};
        if(name=="AddRef")co_return finish(++d().references);
        if(name=="Release"){
            const auto left=--d().references;if(left)co_return finish(left);
            auto dialog=std::move(d());gui_.file_dialogs.erase(object);(void)free_heap(m,object,s);
            for(const auto& [cookie,sink]:dialog.events){(void)cookie;(void)co_await release(sink);}
            (void)co_await release(dialog.properties);(void)co_await release(dialog.collected);co_return finish(0);
        }
        if(name=="QueryInterface"){
            if(!out(2))co_return finish(e_pointer);if(!call(1))co_return finish(e_invalidarg);DialogGuid iid{};m.read(GuestAddress{call(1)},iid);
            if(!supports(d().save,iid))co_return finish(e_nointerface);++d().references;m.store(GuestAddress{call(2)},bits,object);co_return finish(s_ok);
        }
        if(name=="SetFileTypes"){
            if(!d().types.empty())co_return finish(e_unexpected);const auto count=static_cast<std::uint32_t>(call(1));if(!count || !call(2) || count>4096)co_return finish(e_invalidarg);
            std::vector<std::pair<std::u16string,std::u16string>> types;
            for(std::uint32_t i=0;i<count;++i){const auto at=call(2)+std::uint64_t{i}*2*unit;types.emplace_back(text(m.load(GuestAddress{at},bits)),text(m.load(GuestAddress{at+unit},bits)));}
            d().types=std::move(types);co_return finish(s_ok);
        }
        if(name=="SetFileTypeIndex"){d().type_index=static_cast<std::uint32_t>(call(1));co_return finish(s_ok);}
        if(name=="GetFileTypeIndex"){if(!call(1))co_return finish(e_pointer);m.store(GuestAddress{call(1)},32,d().type_index);co_return finish(s_ok);}
        if(name=="Advise"){
            if(!call(1) || !call(2))co_return finish(e_invalidarg);m.check(GuestAddress{call(2)},4,Access::Write);if(d().events.size()>=64)co_return finish(e_outofmemory);
            const auto sink=call(1);(void)co_await add_ref(sink);if(!gui_.file_dialogs.contains(object)){(void)co_await release(sink);co_return finish(e_unexpected);}
            const auto cookie=d().next_cookie++;d().events.emplace(cookie,sink);m.store(GuestAddress{call(2)},32,cookie);co_return finish(s_ok);
        }
        if(name=="Unadvise"){
            const auto found=d().events.find(static_cast<std::uint32_t>(call(1)));if(found==d().events.end())co_return finish(e_invalidarg);
            const auto sink=found->second;d().events.erase(found);(void)co_await release(sink);co_return finish(s_ok);
        }
        if(name=="SetOptions"){d().options=static_cast<std::uint32_t>(call(1));co_return finish(s_ok);}
        if(name=="GetOptions"){if(!call(1))co_return finish(e_pointer);m.store(GuestAddress{call(1)},32,d().options);co_return finish(s_ok);}
        if(name=="SetDefaultFolder" || name=="SetFolder" || name=="SetSaveAsItem" || name=="AddPlace"){
            if(!call(1))co_return finish(e_invalidarg);if(name=="AddPlace" && call(2)>1)co_return finish(e_invalidarg);
            const auto path=co_await item_path(call(1));if(!path || path->empty())co_return finish(e_invalidarg);if(!gui_.file_dialogs.contains(object))co_return finish(e_unexpected);
            if(name=="SetDefaultFolder")d().default_folder=*path;else if(name=="SetFolder")d().folder=*path;else if(name=="AddPlace")d().places.push_back(*path);
            else{d().folder=parent(*path);d().file_name=leaf(*path);}
            co_return finish(s_ok);
        }
        if(name=="GetFolder"){
            if(!out(1))co_return finish(e_pointer);auto path=!d().results.empty()?parent(d().results.front()):d().folder;
            if(path.empty() && d().has_client)if(const auto saved=gui_.file_dialog_folders.find(d().client);saved!=gui_.file_dialog_folders.end())path=saved->second;
            if(path.empty())path=d().default_folder;
            if(path.empty()){GuestScratch buffer(*this,m,s);const auto at=buffer.allocate(32768*2);if(!at)co_return finish(e_outofmemory);
                const auto length=co_await service_callback::call(api("kernel32.dll","GetCurrentDirectoryW"),{32768,at});if(!length || length>=32768)co_return finish(e_fail);path=read_utf16(m,GuestAddress{at});}
            co_return finish(item(1,path));
        }
        if(name=="GetCurrentSelection" || name=="GetResult"){if(!out(1))co_return finish(e_pointer);if(d().results.empty())co_return finish(name=="GetResult"?e_unexpected:e_fail);co_return finish(item(1,d().results.front()));}
        if(name=="GetResults" || name=="GetSelectedItems"){
            if(!out(1))co_return finish(e_pointer);if(d().results.empty())co_return finish(name=="GetResults"?e_unexpected:e_fail);
            try{m.store(GuestAddress{call(1)},bits,create_shell_item_array(m,d().results));}catch(const std::bad_alloc&){co_return finish(e_outofmemory);}co_return finish(s_ok);
        }
        if(name=="SetFileName" || name=="SetTitle" || name=="SetOkButtonLabel" || name=="SetFileNameLabel" || name=="SetDefaultExtension"){
            auto value=text(call(1));auto& dialog=d();
            if(name=="SetFileName")dialog.file_name=std::move(value);else if(name=="SetTitle")dialog.title=std::move(value);else if(name=="SetOkButtonLabel")dialog.ok_label=std::move(value);
            else if(name=="SetFileNameLabel")dialog.file_name_label=std::move(value);else{if(!value.empty() && value.front()==u'.')value.erase(0,1);dialog.default_extension=std::move(value);}
            co_return finish(s_ok);
        }
        if(name=="GetFileName"){
            if(!out(1))co_return finish(e_pointer);const auto value=!d().results.empty()?leaf(d().results.front()):d().file_name;const auto bytes=utf16_bytes(value);
            const auto block=nested_service(s,m,"ole32.dll","CoTaskMemAlloc",{bytes.size()+2});if(!block)co_return finish(e_outofmemory);
            m.write(GuestAddress{block},bytes);m.store(GuestAddress{block+bytes.size()},16,0);m.store(GuestAddress{call(1)},bits,block);co_return finish(s_ok);
        }
        if(name=="Close"){if(d().showing){d().closed=true;d().close_result=static_cast<std::uint32_t>(call(1));}co_return finish(s_ok);}
        if(name=="SetClientGuid"){if(!call(1))co_return finish(e_invalidarg);m.read(GuestAddress{call(1)},d().client);d().has_client=true;co_return finish(s_ok);}
        if(name=="ClearClientData"){if(d().has_client)gui_.file_dialog_folders.erase(d().client);co_return finish(s_ok);}
        if(name=="SetFilter")co_return finish(e_notimpl); // Windows 7 起不再提供 IShellItemFilter。
        if(name=="SetProperties" || name=="SetCollectedProperties"){
            if(!call(1))co_return finish(e_invalidarg);const auto target=call(1);(void)co_await add_ref(target);if(!gui_.file_dialogs.contains(object)){(void)co_await release(target);co_return finish(e_unexpected);}
            auto& slot=name=="SetProperties"?d().properties:d().collected;const auto previous=slot;slot=target;if(name=="SetCollectedProperties")d().append_default=call(2)!=0;
            (void)co_await release(previous);co_return finish(s_ok);
        }
        if(name=="GetProperties"){if(!out(1))co_return finish(e_pointer);const auto store=d().properties;if(!store)co_return finish(e_unexpected);(void)co_await add_ref(store);m.store(GuestAddress{call(1)},bits,store);co_return finish(s_ok);}
        if(name=="ApplyProperties")co_return finish(e_notimpl); // 尚未支援把屬性寫入檔案中繼資料。
        if(name!="Show")co_return finish(e_notimpl);
        // Show(hwndOwner)
        if(d().showing)co_return finish(e_unexpected);d().showing=true;d().closed=false;d().results.clear();const auto owner=call(1);
        const auto stop=[&](std::uint32_t hr){if(gui_.file_dialogs.contains(object))d().showing=false;return finish(hr);};
        for(unsigned attempt=0;attempt<16;++attempt){
            GuestScratch scratch(*this,m,s);std::vector<std::u16string> results;auto chosen=d().type_index;const bool save=d().save,folders=!save && (d().options&0x20U);
            auto initial=d().folder;if(initial.empty() && d().has_client)if(const auto saved=gui_.file_dialog_folders.find(d().client);saved!=gui_.file_dialog_folders.end())initial=saved->second;
            if(initial.empty())initial=d().default_folder;
            const auto place=[&](std::u16string_view value)->std::uint64_t{if(value.empty())return 0;const auto bytes=utf16_bytes(std::u16string(value));const auto at=scratch.allocate(bytes.size()+2);
                if(at){m.write(GuestAddress{at},bytes);m.store(GuestAddress{at+bytes.size()},16,0);}return at;};
            if(folders){
                // BROWSEINFOW：hwndOwner、pidlRoot、pszDisplayName、lpszTitle、ulFlags（BIF_RETURNONLYFSDIRS｜BIF_NEWDIALOGSTYLE）、lpfn、lParam、iImage。
                const auto size=x86_?32U:64U;const auto info=scratch.allocate(size),display=scratch.allocate(520*2),path=scratch.allocate(1024*2);
                if(!info || !display || !path)co_return stop(e_outofmemory);m.fill(GuestAddress{info},0,size);
                m.store(GuestAddress{info},bits,owner);m.store(GuestAddress{info+2*unit},bits,display);m.store(GuestAddress{info+3*unit},bits,place(d().title));m.store(GuestAddress{info+4*unit},32,0x41);
                auto pidl=co_await service_callback::call_value(api("shell32.dll","SHBrowseForFolderW"),{info});if(x86_)pidl&=UINT32_MAX;
                if(!gui_.file_dialogs.contains(object))co_return finish(e_unexpected);if(!pidl)co_return stop(cancelled);
                const auto ok=co_await service_callback::call(api("shell32.dll","SHGetPathFromIDListW"),{pidl,path});(void)co_await service_callback::call(api("ole32.dll","CoTaskMemFree"),{pidl});
                if(!gui_.file_dialogs.contains(object))co_return finish(e_unexpected);if(!ok)co_return stop(e_fail);results.push_back(read_utf16(m,GuestAddress{path}));
            }else{
                // OPENFILENAMEW：篩選字串為「名稱\0規格\0…\0」；FOS 與 OFN 共用的位元值相同，另加 OFN_EXPLORER｜OFN_HIDEREADONLY。
                const auto size=x86_?88U:152U;const bool multiple=!save && (d().options&0x200U);const std::uint32_t capacity=multiple?65536U:32768U;
                std::u16string filter;for(const auto& [label,spec]:d().types){filter+=label;filter.push_back(0);filter+=spec;filter.push_back(0);}
                std::uint64_t filter_at=0;if(!filter.empty()){const auto bytes=utf16_bytes(filter);filter_at=scratch.allocate(bytes.size()+2);if(!filter_at)co_return stop(e_outofmemory);m.write(GuestAddress{filter_at},bytes);m.store(GuestAddress{filter_at+bytes.size()},16,0);}
                const auto ofn=scratch.allocate(size),file=scratch.allocate(std::size_t{capacity}*2);if(!ofn || !file)co_return stop(e_outofmemory);
                m.fill(GuestAddress{ofn},0,size);m.store(GuestAddress{file},16,0);
                if(const auto name_bytes=utf16_bytes(d().file_name.substr(0,capacity-1));!name_bytes.empty()){m.write(GuestAddress{file},name_bytes);m.store(GuestAddress{file+name_bytes.size()},16,0);}
                const auto flags=(d().options&0x1211fb0aU)|0x80004U;
                const auto at=[&](unsigned small,unsigned large){return ofn+(x86_?small:large);};
                m.store(GuestAddress{ofn},32,size);m.store(GuestAddress{at(4,8)},bits,owner);m.store(GuestAddress{at(12,24)},bits,filter_at);m.store(GuestAddress{at(24,44)},32,std::max(1U,chosen));
                m.store(GuestAddress{at(28,48)},bits,file);m.store(GuestAddress{at(32,56)},32,capacity);m.store(GuestAddress{at(44,80)},bits,place(initial));m.store(GuestAddress{at(48,88)},bits,place(d().title));
                m.store(GuestAddress{at(52,96)},32,flags);m.store(GuestAddress{at(60,104)},bits,place(d().default_extension));
                const auto ok=co_await service_callback::call(api("comdlg32.dll",save?"GetSaveFileNameW":"GetOpenFileNameW"),{ofn});
                if(!gui_.file_dialogs.contains(object))co_return finish(e_unexpected);
                if(!ok){const auto error=co_await service_callback::call(api("comdlg32.dll","CommDlgExtendedError"),{});co_return stop(error?e_fail:cancelled);}
                chosen=static_cast<std::uint32_t>(m.load(GuestAddress{at(24,44)},32));
                // 多選：第一段是資料夾，其後各段是檔名；只選一個時第一段即完整路徑。
                auto first=read_utf16(m,GuestAddress{file});auto next=file+(first.size()+1)*2;
                if(multiple && m.load(GuestAddress{next},16)){while(const auto c=m.load(GuestAddress{next},16)){(void)c;auto part=read_utf16(m,GuestAddress{next});next+=(part.size()+1)*2;
                    results.push_back(first+(first.ends_with(u'\\')?u"":u"\\")+part);if(results.size()>=100000)break;}}
                else results.push_back(std::move(first));
            }
            auto& dialog=d();dialog.results=std::move(results);dialog.type_index=chosen;if(!dialog.results.empty())dialog.file_name=leaf(dialog.results.front());
            // IFileDialogEvents::OnFileOk：任一接收者回傳 S_FALSE 即拒絕此結果，重新顯示。
            const auto sinks=dialog.events;bool refused=false;
            for(const auto& [cookie,sink]:sinks){(void)cookie;if(!gui_.file_dialogs.contains(object))break;const auto hr=co_await service_callback::call(method(sink,3),{sink,object});if(hr==s_false)refused=true;}
            if(!gui_.file_dialogs.contains(object))co_return finish(e_unexpected);
            if(d().closed)co_return stop(d().close_result?d().close_result:cancelled);
            if(!refused){if(d().has_client && !d().results.empty())gui_.file_dialog_folders[d().client]=parent(d().results.front());co_return stop(s_ok);}
            d().results.clear();
        }
        co_return stop(cancelled);
    });
}
