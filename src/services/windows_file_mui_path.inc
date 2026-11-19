// GetFileMUIPath(dwFlags, pcwszFilePath, pwszLanguage, pcchLanguage, pwszFileMUIPath, pcchFileMUIPath, pululEnumerator)：
// 語言資源檔位於檔案所在目錄的語言子目錄。LN 檔（MUI_LANG_NEUTRAL_PE_FILE，未指定檔案類型時的預設）為 <目錄>\<語言>\<檔名>.mui，
// MUI_NON_LANG_NEUTRAL_FILE 為 <目錄>\<語言>\<檔名>；兩者都指定時同一語言先找 .mui。
// pwszLanguage 有內容時只找該語言（格式依 MUI_LANGUAGE_ID／NAME）；否則依篩選旗標：MUI_USE_SEARCH_ALL_LANGUAGES 為目錄內名稱是有效語言的子目錄
// （不分大小寫排序），MUI_USE_INSTALLED_LANGUAGES 為已安裝 UI 語言（與 EnumUILanguages 相同），其他為執行緒偏好 UI 語言、程序與使用者清單，再加上各語言的中性上層語言。
// *pululEnumerator 是下一個要檢查的候選位置：找到時寫入語言（MUI_LANGUAGE_ID 為四位十六進位）與完整路徑並前進；pwszFileMUIPath 為 NULL 且
// *pcchFileMUIPath 為 0 時只回報兩個所需長度（含結尾 NUL）、不前進。第一次就找不到為 ERROR_FILE_NOT_FOUND，列舉完畢為 ERROR_NO_MORE_FILES，
// 緩衝區不足為 ERROR_INSUFFICIENT_BUFFER（同時回報所需長度）。
if(name=="GetFileMUIPath"){
    const auto flags=static_cast<std::uint32_t>(arg(0));
    if((flags&~0x37cU) || (flags&0xc)==0xc || std::popcount(flags&0x70U)>1 || !arg(1) || !arg(3) || !arg(5) || !arg(6))return fail(87);
    const bool id=(flags&4)!=0;
    const auto language_capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32)),path_capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(5)},32));
    if(!arg(2) && language_capacity)return fail(87);
    auto file=read_utf16(memory,GuestAddress{arg(1)},32767);for(auto& c:file)if(c==u'/')c=u'\\';
    if(file.empty())return fail(87);
    if(!(file.size()>=3 && file[1]==u':' && file[2]==u'\\') && !file.starts_with(u"\\\\")){
        // 相對路徑以目前目錄展開；"\x" 取目前磁碟機。
        const auto& current=runtime_.current_directory;if(current.size()<2)return fail(3);
        file=file.starts_with(u"\\")?current.substr(0,2)+file:current+(current.ends_with(u'\\')?u"":u"\\")+file;
    }
    const auto slash=file.rfind(u'\\');const auto directory=file.substr(0,slash),leaf=file.substr(slash+1);if(leaf.empty())return fail(87);
    // 候選：{目錄名稱, 語言名稱}；同一語言只列一次。
    std::vector<std::pair<std::u16string,std::u16string>> candidates;
    const auto add=[&](const std::u16string& folder,const std::u16string& language){
        if(std::none_of(candidates.begin(),candidates.end(),[&](const auto& item){return item.second==language;}))candidates.emplace_back(folder,language);};
    std::u16string asked;if(arg(2) && language_capacity)asked=read_utf16(memory,GuestAddress{arg(2)},std::min<std::uint32_t>(language_capacity,85));
    if(!asked.empty()){const auto parsed=mui_parse(asked,id);if(!parsed)return fail(87);add(*parsed,*parsed);}
    else if(flags&0x40){
        const auto [host_directory,error]=filesystem_.unix_file_name(utf16_to_utf8(directory));
        std::vector<std::pair<std::u16string,std::u16string>> found;
        if(!error && !host_directory.empty()){
            std::error_code walk;std::size_t visited=0;
            for(std::filesystem::directory_iterator it(host_directory,walk),end;!walk && it!=end && ++visited<=4096;it.increment(walk)){
                std::error_code kind;if(!it->is_directory(kind))continue;
                const auto [guest,guest_error]=filesystem_.dos_file_name(it->path());if(guest_error || guest.empty())continue;
                const auto text=utf8_to_utf16(guest);const auto folder=text.substr(text.find_last_of(u'\\')==text.npos?0:text.find_last_of(u'\\')+1);
                if(const auto locale=named(folder);locale && !locale->name.empty())found.emplace_back(folder,locale->name);
            }
        }
        const auto lower=[](std::u16string text){for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return text;};
        std::sort(found.begin(),found.end(),[&](const auto& a,const auto& b){return lower(a.first)<lower(b.first);});
        for(const auto& [folder,language]:found)add(folder,language);
    }
    else if(flags&0x20){for(const auto& language:user_languages)add(language,language);}
    else{
        if(const auto* current=current_thread_ui_languages())for(const auto& language:*current)add(language,language);
        for(const auto& language:runtime_.preferred_ui_languages)add(language,language);
        for(const auto& language:user_languages)add(language,language);
        const auto listed=candidates;
        for(const auto& item:listed){const auto dash=item.second.find(u'-');if(dash==item.second.npos)continue;
            if(const auto parent=named(item.second.substr(0,dash));parent && !parent->name.empty())add(parent->name,parent->name);}
    }
    const bool neutral=(flags&0x100) || !(flags&0x200),plain=(flags&0x200)!=0;
    const auto exists=[&](const std::u16string& path){
        const auto [host,error]=filesystem_.unix_file_name(utf16_to_utf8(path));if(error || host.empty())return false;
        std::error_code status;return std::filesystem::is_regular_file(host,status);};
    const auto start=memory.load(GuestAddress{arg(6)},64);
    for(auto index=start;index<candidates.size();++index){
        const auto base=directory+u"\\"+candidates[index].first+u"\\"+leaf;std::u16string found;
        if(neutral && exists(base+u".mui"))found=base+u".mui";else if(plain && exists(base))found=base;
        if(found.empty())continue;
        const auto language=mui_text(candidates[index].second,id);
        const auto language_needed=static_cast<std::uint32_t>(language.size()+1),path_needed=static_cast<std::uint32_t>(found.size()+1);
        memory.check(GuestAddress{arg(3)},4,Access::Write);memory.check(GuestAddress{arg(5)},4,Access::Write);
        if(!arg(4) && !path_capacity){memory.store(GuestAddress{arg(3)},32,language_needed);memory.store(GuestAddress{arg(5)},32,path_needed);last_error_=0;return finish(1);}
        if(!arg(4) || path_capacity<path_needed || (arg(2) && language_capacity<language_needed)){
            memory.store(GuestAddress{arg(3)},32,language_needed);memory.store(GuestAddress{arg(5)},32,path_needed);return fail(122);}
        memory.check(GuestAddress{arg(4)},std::uint64_t{path_needed}*2,Access::Write);if(arg(2))memory.check(GuestAddress{arg(2)},std::uint64_t{language_needed}*2,Access::Write);
        if(arg(2))memory.write(GuestAddress{arg(2)},utf16_bytes(language));memory.write(GuestAddress{arg(4)},utf16_bytes(found));
        memory.store(GuestAddress{arg(3)},32,language_needed);memory.store(GuestAddress{arg(5)},32,path_needed);memory.store(GuestAddress{arg(6)},64,index+1);
        last_error_=0;return finish(1);
    }
    memory.store(GuestAddress{arg(6)},64,std::max<std::uint64_t>(start,candidates.size()));
    return fail(start?18U:2U);
}
