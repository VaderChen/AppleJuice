// shell32 的工具函式（由 windows_shell.cpp 的 invoke_shell 引入）：
// - SHGetImageList：系統影像清單（SHIL_LARGE 32、SMALL 16、EXTRALARGE 48、SYSSMALL＝SM_CXSMICON、JUMBO 256）以 HIMAGELIST_QueryInterface 取得 IImageList。
// - SHGetNewLinkInfo(A／W)：新捷徑的完整路徑「顯示名稱 - Shortcut.lnk」（SHGNLI_PREFIXNAME 為「Shortcut to 顯示名稱」），同名時加上 (2)、(3)…；
//   目標本身是捷徑（.lnk／.pif／.url）時 *pfMustCopy 為 TRUE 並沿用原檔名。
// - SHGetSetFolderCustomSettings：資料夾的 desktop.ini（[.ShellClassInfo] 的 InfoTip、CLSID、IconResource、Logo，[ViewState] 的 Vid）讀寫；寫入時資料夾設為唯讀、desktop.ini 設為隱藏＋系統。
// - SHIsFileAvailableOffline：沙箱沒有離線檔案（CSC）快取，存在的路徑回 E_FAIL。
if(service.symbol=="#15" || service.symbol=="#186" || service.symbol=="SHGetImageList" || service.symbol.starts_with("SHGetNewLinkInfo") ||
   service.symbol=="SHGetSetFolderCustomSettings" || service.symbol=="SHIsFileAvailableOffline"){
    return nested_continuation(state,memory,"shell32 工具函式",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto symbol=service.symbol;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const auto api=[&](std::string_view dll,std::string_view name){return resolve(dll,name).value;};
        GuestScratch scratch(*this,m,s);
        const auto wide_text=[&](std::u16string_view text)->std::uint64_t{const auto bytes=utf16_bytes(text);const auto at=scratch.allocate(bytes.size());if(at)m.write(GuestAddress{at},bytes);return at;};
        if(symbol=="#15" || symbol=="#186"){
            // NT 的 ILGetDisplayName 與 ILGetDisplayNameEx 共用 Unicode 輸出。
            const bool extended=symbol=="#186";
            auto folder=extended?call(0):0;
            auto pidl=call(extended?1:0);
            const auto output=call(extended?2:1);
            const auto type=extended?static_cast<std::uint32_t>(call(3)):0U;
            if(!pidl || !output)co_return finish(0);
            const bool parent=type!=0 && m.load(GuestAddress{pidl},16)!=0;
            const bool owned=parent || !folder;
            const auto storage=scratch.allocate(272); // STRRET（64 位元為 272 bytes）。
            if(!storage)co_return finish(0);
            if(owned){
                const auto slot=scratch.allocate(unit*2);
                if(!slot)co_return finish(0);
                std::uint32_t hr;
                if(parent){
                    constexpr std::array<std::uint8_t,16> iid_folder{0xe6,0x14,0x02,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
                    m.write(GuestAddress{storage},iid_folder);
                    hr=co_await service_callback::call(api("shell32.dll","SHBindToParent"),{pidl,storage,slot,slot+unit});
                    if(!(hr&0x80000000U))pidl=m.load(GuestAddress{slot+unit},bits);
                }else{
                    hr=co_await service_callback::call(api("shell32.dll","SHGetDesktopFolder"),{slot});
                }
                if(hr&0x80000000U)co_return finish(0);
                folder=m.load(GuestAddress{slot},bits);
            }
            const auto method=[&](unsigned index){return m.load(GuestAddress{m.load(GuestAddress{folder},bits)+unit*index},bits);};
            const auto flags=type==1?0U:type==2?1U:0xc000U;
            auto hr=co_await service_callback::call(method(11),{folder,pidl,flags,storage});
            if(!(hr&0x80000000U))hr=co_await service_callback::call(api("shlwapi.dll","StrRetToBufW"),{storage,pidl,output,260});
            if(owned)(void)co_await service_callback::call(method(2),{folder});
            co_return finish(!(hr&0x80000000U));
        }
        if(symbol=="SHGetImageList"){
            // (iImageList, riid, ppv)
            if(!call(2))co_return finish(0x80004003U);m.store(GuestAddress{call(2)},bits,0);
            const auto list=static_cast<std::int32_t>(call(0));std::uint32_t size=0;
            if(list==0)size=32;else if(list==1)size=16;else if(list==2)size=48;else if(list==4)size=256;
            else if(list==3)size=static_cast<std::uint32_t>(co_await service_callback::call(api("user32.dll","GetSystemMetrics"),{49})); // SM_CXSMICON
            if(!size || !call(1))co_return finish(0x80070057U);
            const auto images=shell_image_list(size);if(!images)co_return finish(0x80004005U);
            const auto result=co_await service_callback::call(api("comctl32.dll","HIMAGELIST_QueryInterface"),{images,call(1),call(2)});
            // 系統影像清單由 shell 持有：第一次交出介面時多保留一個參考，應用程式 Release 到 0 也不會銷毀清單。
            if(!(result&0x80000000U) && gui_.shell_pinned_image_lists.insert(images).second){
                const auto object=m.load(GuestAddress{call(2)},bits);const auto table=m.load(GuestAddress{object},bits);
                (void)co_await service_callback::call(m.load(GuestAddress{table+unit},bits),{object});
            }
            co_return finish(result);
        }
        if(symbol=="SHIsFileAvailableOffline"){
            if(!call(0))co_return finish(0x80070057U);const auto path=wide_text(read_utf16(m,GuestAddress{call(0)},32768));if(!path)co_return finish(0x8007000eU);
            const auto attributes=co_await service_callback::call(api("kernel32.dll","GetFileAttributesW"),{path});
            co_return finish(attributes==UINT32_MAX?0x80070057U:0x80004005U);
        }
        if(symbol.starts_with("SHGetNewLinkInfo")){
            // (pszLinkTo, pszDir, pszName[MAX_PATH] 輸出, pfMustCopy, uFlags)；SHGNLI_PIDL 1、PREFIXNAME 2、NOUNIQUE 4、NOLNK 8、NOLOCNAME 0x10、USEURLEXT 0x20。
            const bool wide=symbol.ends_with('W');const auto flags=static_cast<std::uint32_t>(call(4));
            const auto read=[&](std::uint64_t at){return wide?read_utf16(m,GuestAddress{at},32768):ansi_to_utf16_lossy(read_ansi(m,at,32768));};
            if(!call(0) || !call(1) || !call(2) || !call(3))co_return finish(0);
            std::u16string target;
            if(flags&1U){const auto buffer=scratch.allocate(520);if(!buffer)co_return finish(0);
                if(!co_await service_callback::call(api("shell32.dll","SHGetPathFromIDListW"),{call(0),buffer}))co_return finish(0);target=read_utf16(m,GuestAddress{buffer},260);}
            else target=read(call(0));
            auto directory=read(call(1));if(target.empty() || directory.empty())co_return finish(0);
            while(target.size()>3 && (target.back()==u'\\' || target.back()==u'/'))target.pop_back();
            const auto slash=target.find_last_of(u"\\/");auto leaf=slash==target.npos?target:target.substr(slash+1);
            const auto dot=leaf.find_last_of(u'.');auto extension=dot==leaf.npos?std::u16string{}:leaf.substr(dot);for(auto& c:extension)c=unicode_case_unit(c,false);
            const bool must_copy=extension==u".lnk" || extension==u".pif" || extension==u".url";
            m.store(GuestAddress{call(3)},32,must_copy?1:0);
            std::u16string name;
            if(must_copy)name=leaf;
            else{
                // 顯示名稱（依「隱藏已知副檔名」等設定）；SHGNLI_NOLOCNAME 或取不到時用檔名。磁碟根目錄取顯示名稱。
                std::u16string display=leaf;
                if(!(flags&0x10U) || leaf.empty() || leaf.back()==u':'){
                    const auto path=wide_text(target);const unsigned info_size=x86_?692U:696U;const auto file_info=scratch.allocate(info_size);
                    if(path && file_info && co_await service_callback::call_value(api("shell32.dll","SHGetFileInfoW"),{path,0,file_info,info_size,0x200})){
                        const auto shown=read_utf16(m,GuestAddress{file_info+(x86_?12U:16U)},260);if(!shown.empty())display=shown;}
                }
                for(auto& c:display)if(c==u':' || c==u'\\' || c==u'/' || c==u'*' || c==u'?' || c==u'"' || c==u'<' || c==u'>' || c==u'|')c=u'_';
                name=(flags&2U)?u"Shortcut to "+display:display+u" - Shortcut";
                name+=(flags&8U)?u"":(flags&0x20U)?u".url":u".lnk";
            }
            if(directory.back()!=u'\\' && directory.back()!=u'/')directory+=u'\\';
            auto result=directory+name;
            if(!must_copy && !(flags&4U)){
                const auto stem=name.substr(0,name.size()-((flags&8U)?0:4)),suffix=(flags&8U)?std::u16string{}:name.substr(name.size()-4);
                const auto candidate=scratch.allocate(260*sizeof(char16_t));
                if(!candidate)co_return finish(0);
                for(std::uint64_t copy=2;;++copy){
                    if(result.size()>=260)co_return finish(0);
                    m.write(GuestAddress{candidate},utf16_bytes(result));
                    if(co_await service_callback::call(api("kernel32.dll","GetFileAttributesW"),{candidate})==UINT32_MAX)break;
                    const auto number=utf8_to_utf16(std::to_string(copy));
                    result=directory+((flags&2U)?u"Shortcut ("+number+u") to "+stem.substr(12):stem+u" ("+number+u")")+suffix;
                }
            }
            if(result.size()>=260)co_return finish(0);
            if(wide)m.write(GuestAddress{call(2)},utf16_bytes(result));else{auto ansi=utf16_to_ansi(result);if(ansi.size()>=260)co_return finish(0);ansi.push_back(0);m.write(GuestAddress{call(2)},std::span(reinterpret_cast<const std::uint8_t*>(ansi.data()),ansi.size()));}
            co_return finish(1);
        }
        // SHGetSetFolderCustomSettings(pfcs, pszPath, dwReadWrite)：FCS_READ 1、FCS_FORCEWRITE 2、FCS_WRITE 3（只寫入尚未設定的值）。
        const auto settings=call(0);const auto flags=static_cast<std::uint32_t>(call(2));const unsigned struct_size=x86_?60U:104U;
        if(!settings || !call(1) || !flags || flags>3 || m.load(GuestAddress{settings},32)!=struct_size)co_return finish(0x80070057U);
        const auto folder=read_utf16(m,GuestAddress{call(1)},32768);
        const auto at=[&](unsigned x86_offset,unsigned x64_offset){return settings+(x86_?x86_offset:x64_offset);};
        const auto mask=static_cast<std::uint32_t>(m.load(GuestAddress{settings+4},32));
        const auto folder_path=wide_text(folder);const auto ini=wide_text(folder+(folder.ends_with(u'\\')?u"":u"\\")+u"desktop.ini");
        const auto class_info=wide_text(u".ShellClassInfo"),view_state=wide_text(u"ViewState");const auto buffer=scratch.allocate(2048);
        if(!folder_path || !ini || !class_info || !view_state || !buffer)co_return finish(0x8007000eU);
        const auto attributes=co_await service_callback::call(api("kernel32.dll","GetFileAttributesW"),{folder_path});
        if(attributes==UINT32_MAX || !(attributes&0x10U))co_return finish(0x80070003U); // HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
        const auto get=[&](std::uint64_t section,const char16_t* key)->service_callback::Task<std::u16string>{
            const auto name=wide_text(key);const auto empty=wide_text(u"");if(!name || !empty)co_return std::u16string{};
            (void)co_await service_callback::call(api("kernel32.dll","GetPrivateProfileStringW"),{section,name,empty,buffer,1024,ini});co_return read_utf16(m,GuestAddress{buffer},1024);
        };
        const auto put=[&](std::uint64_t section,const char16_t* key,const std::u16string& value)->service_callback::Task<bool>{
            const auto name=wide_text(key);const auto text=wide_text(value);if(!name || !text)co_return false;
            co_return co_await service_callback::call(api("kernel32.dll","WritePrivateProfileStringW"),{section,name,text,ini})!=0;
        };
        const auto guid_text=[&](std::uint64_t pointer){std::array<std::uint8_t,16> g{};m.read(GuestAddress{pointer},g);constexpr char hex[]="0123456789ABCDEF";std::string out="{";
            const auto byte=[&](unsigned i){out+=hex[g[i]>>4];out+=hex[g[i]&15];};for(unsigned i=4;i-->0;)byte(i);out+='-';byte(5);byte(4);out+='-';byte(7);byte(6);out+='-';byte(8);byte(9);out+='-';for(unsigned i=10;i<16;++i)byte(i);out+='}';return utf8_to_utf16(out);};
        const auto parse_guid=[&](const std::u16string& text,std::uint64_t pointer)->bool{
            if(text.size()!=38 || text[0]!=u'{' || text[37]!=u'}')return false;std::array<std::uint8_t,16> g{};std::string digits;
            for(std::size_t i=1;i<37;++i){if(text[i]==u'-')continue;const auto c=static_cast<char>(text[i]);if(!std::isxdigit(static_cast<unsigned char>(c)))return false;digits+=c;}
            if(digits.size()!=32)return false;const auto value=[&](std::size_t i){return static_cast<std::uint8_t>(std::stoul(digits.substr(i*2,2),nullptr,16));};
            constexpr std::array<unsigned,16> order{3U,2U,1U,0U,5U,4U,7U,6U,8U,9U,10U,11U,12U,13U,14U,15U};for(unsigned i=0;i<16;++i)g[order[i]]=value(i);m.write(GuestAddress{pointer},g);return true;
        };
        const auto copy_out=[&](std::uint64_t pointer,std::uint32_t capacity,const std::u16string& value){
            if(!pointer || !capacity)return;auto text=value.substr(0,capacity-1);m.write(GuestAddress{pointer},utf16_bytes(text));
        };
        const bool read_mode=flags==1,overwrite=flags==2;bool wrote=false;
        // 讀取：要求的欄位依 desktop.ini 填入；沒有值時字串為空、圖示索引 0。
        // 寫入：FCS_FORCEWRITE 一律寫入，FCS_WRITE 只寫入 desktop.ini 還沒有的值。
        if(mask&0x1U){const auto pointer=m.load(GuestAddress{at(8,8)},bits);
            if(read_mode){if(pointer)(void)parse_guid(co_await get(view_state,u"Vid"),pointer);}
            else if(pointer && (overwrite || (co_await get(view_state,u"Vid")).empty()))wrote|=co_await put(view_state,u"Vid",guid_text(pointer));}
        if(mask&0x4U){const auto pointer=m.load(GuestAddress{at(24,40)},bits);const auto capacity=static_cast<std::uint32_t>(m.load(GuestAddress{at(28,48)},32));
            if(read_mode)copy_out(pointer,capacity,co_await get(class_info,u"InfoTip"));
            else if(pointer && (overwrite || (co_await get(class_info,u"InfoTip")).empty()))wrote|=co_await put(class_info,u"InfoTip",read_utf16(m,GuestAddress{pointer},4096));}
        if(mask&0x8U){const auto pointer=m.load(GuestAddress{at(32,56)},bits);
            if(read_mode){if(pointer)(void)parse_guid(co_await get(class_info,u"CLSID"),pointer);}
            else if(pointer && (overwrite || (co_await get(class_info,u"CLSID")).empty()))wrote|=co_await put(class_info,u"CLSID",guid_text(pointer));}
        if(mask&0x10U){const auto pointer=m.load(GuestAddress{at(40,72)},bits);const auto capacity=static_cast<std::uint32_t>(m.load(GuestAddress{at(44,80)},32));
            if(read_mode){
                auto resource=co_await get(class_info,u"IconResource");std::int32_t index=0;std::u16string file;
                if(!resource.empty()){const auto comma=resource.find_last_of(u',');file=resource.substr(0,comma);if(comma!=resource.npos){try{index=std::stoi(utf16_to_utf8(resource.substr(comma+1)));}catch(...){index=0;}}}
                else{file=co_await get(class_info,u"IconFile");try{const auto number=co_await get(class_info,u"IconIndex");index=number.empty()?0:std::stoi(utf16_to_utf8(number));}catch(...){index=0;}}
                copy_out(pointer,capacity,file);m.store(GuestAddress{at(48,84)},32,static_cast<std::uint32_t>(index));
            }else if(pointer && (overwrite || (co_await get(class_info,u"IconResource")).empty())){
                const auto index=static_cast<std::int32_t>(m.load(GuestAddress{at(48,84)},32));
                wrote|=co_await put(class_info,u"IconResource",read_utf16(m,GuestAddress{pointer},4096)+u","+utf8_to_utf16(std::to_string(index)));
            }}
        if(mask&0x20U){const auto pointer=m.load(GuestAddress{at(52,88)},bits);const auto capacity=static_cast<std::uint32_t>(m.load(GuestAddress{at(56,96)},32));
            if(read_mode)copy_out(pointer,capacity,co_await get(class_info,u"Logo"));
            else if(pointer && (overwrite || (co_await get(class_info,u"Logo")).empty()))wrote|=co_await put(class_info,u"Logo",read_utf16(m,GuestAddress{pointer},4096));}
        if(wrote){
            // 資料夾設為唯讀（Explorer 才會讀 desktop.ini）；desktop.ini 隱藏＋系統。
            (void)co_await service_callback::call(api("kernel32.dll","SetFileAttributesW"),{folder_path,(attributes&~0x10U)|0x1U});
            (void)co_await service_callback::call(api("kernel32.dll","SetFileAttributesW"),{ini,0x6U});
        }
        co_return finish(0);
    });
}
