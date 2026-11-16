// 字型實現與字型檔查詢（gdi32）：GetFontRealizationInfo／GdiRealizationInfo 為 DC 目前選取的 face 指定實現 ID，
// GetFontFileInfo／GetFontFileData 以該 ID 取得字型檔大小、路徑與內容（與 GetFontData(0) 共用 native_font_file 的 sfnt），
// GetFontResourceInfoW 解析字型資源檔（與 AddFontResource 共用 native_load_font_faces）。本檔由 invoke_fonts 引入。
if(service.symbol=="GetFontRealizationInfo" || service.symbol=="GdiRealizationInfo"){
    // GetFontRealizationInfo：{size(16／24), flags, cache_num, instance_id[, unk, face_index(WORD), simulations(WORD)]}；
    // GdiRealizationInfo：{flags, cache_num, instance_id}。flags 3 表示可縮放字型（本機 face 皆為輪廓字型）。
    const bool legacy=service.symbol=="GdiRealizationInfo";
    if(!gui_.dc_objects.contains(arg(0)) && !gui_.contexts.contains(arg(0))){last_error_=6;return finish(0);}
    if(!arg(1)){last_error_=87;return finish(0);}
    const auto size=legacy?12U:static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
    if(size!=12 && size!=16 && size!=24){last_error_=87;return finish(0);}
    const auto context=gui_.contexts.find(arg(0));const auto face=native_resolve_font(gui_font(gui_,context==gui_.contexts.end()?0xb011:context->second.font));
    if(!face){last_error_=50;return finish(0);}
    // 原生字型物件每次解析都是新的參考，以字面身分（PostScript 名稱、家族、樣式、粗細、斜體與模擬）判定同一實現。
    const auto same_face=[&](const NativeFontFace& item){return item.postscript_name==face->postscript_name && item.family==face->family && item.style==face->style &&
        item.full_name==face->full_name && item.weight==face->weight && item.italic==face->italic && item.simulations==face->simulations;};
    auto found=std::find_if(gui_.realized_fonts.begin(),gui_.realized_fonts.end(),[&](const auto& item){return same_face(item.face);});
    if(found==gui_.realized_fonts.end()){
        if(gui_.realized_fonts.size()>=4096){last_error_=8;return finish(0);}
        gui_.realized_fonts.push_back({*face,{},false});found=std::prev(gui_.realized_fonts.end());++gui_.font_cache_number;
    }
    const auto id=static_cast<std::uint32_t>(found-gui_.realized_fonts.begin()+1);const auto base=arg(1)+(legacy?0U:4U);
    memory.check(GuestAddress{arg(1)},size,Access::Write);
    memory.store(GuestAddress{base},32,3);memory.store(GuestAddress{base+4},32,gui_.font_cache_number);memory.store(GuestAddress{base+8},32,id);
    if(size==24){memory.store(GuestAddress{arg(1)+16},32,0);memory.store(GuestAddress{arg(1)+20},16,0);memory.store(GuestAddress{arg(1)+22},16,face->simulations&3U);}
    return finish(1);
}
if(service.symbol=="GetFontFileInfo" || service.symbol=="GetFontFileData"){
    // 實現 ID 對應單一檔案（file_index 0）；資料為 face 的完整 sfnt，路徑位於系統字型目錄。
    const auto id=static_cast<std::uint32_t>(arg(0));
    if(!id || id>gui_.realized_fonts.size() || static_cast<std::uint32_t>(arg(1))){last_error_=87;return finish(0);}
    auto& realized=gui_.realized_fonts[id-1];
    if(!realized.loaded){realized.file=native_font_file(realized.face);realized.loaded=true;}
    if(realized.file.empty()){last_error_=13;return finish(0);}
    if(service.symbol=="GetFontFileData"){
        const std::uint64_t offset=arg(2);const auto count=static_cast<std::uint32_t>(arg(4));
        if(offset>realized.file.size() || count>realized.file.size()-offset || (count && !arg(3))){last_error_=87;return finish(0);}
        if(count)memory.write(GuestAddress{arg(3)},std::span<const std::uint8_t>(realized.file).subspan(static_cast<std::size_t>(offset),count));
        return finish(1);
    }
    // font_fileinfo：{FILETIME 寫入時間, LARGE_INTEGER 大小, WCHAR 路徑[]}；needed 為 SIZE_T。
    auto file_name=realized.face.postscript_name.empty()?realized.face.family:realized.face.postscript_name;
    for(auto& c:file_name)if(c==u'\\' || c==u'/' || c==u':' || c==u'*' || c==u'?' || c==u'"' || c==u'<' || c==u'>' || c==u'|')c=u'_';
    const bool cff=realized.file.size()>=4 && realized.file[0]=='O' && realized.file[1]=='T' && realized.file[2]=='T' && realized.file[3]=='O';
    const auto path=u"C:\\Windows\\Fonts\\"+file_name+(cff?u".otf":u".ttf");
    const std::uint64_t needed=16+(path.size()+1)*2;
    if(arg(4)){memory.check(GuestAddress{arg(4)},x86_?4:8,Access::Write);memory.store(GuestAddress{arg(4)},x86_?32:64,needed);}
    if(!arg(2) || arg(3)<needed){last_error_=122;return finish(0);}
    memory.check(GuestAddress{arg(2)},needed,Access::Write);
    memory.store(GuestAddress{arg(2)},64,0);memory.store(GuestAddress{arg(2)+8},64,realized.file.size());
    memory.write(GuestAddress{arg(2)+16},utf16_bytes(path));return finish(1);
}
if(service.symbol=="GetFontResourceInfoW"){
    // {檔名, DWORD* 位元組數（輸入容量／輸出需求）, 緩衝區, 查詢種類}：0 face 數、1 描述（各 face 全名以 " & " 相連）、
    // 2 LOGFONTW 陣列、3 是否為 TrueType／OpenType、4 字型檔路徑、5 是否已移除。容量為 0 或沒有緩衝區時只回報需求。
    const auto type=static_cast<std::uint32_t>(arg(3));
    if(!arg(0) || !arg(1) || type>5){last_error_=87;return finish(0);}
    const auto name=read_utf16(memory,GuestAddress{arg(0)});const auto file=filesystem_.open(utf16_to_utf8(name));
    if(file.error){last_error_=file.error;return finish(0);}
    const auto faces=native_load_font_faces(*file.bytes);if(faces.empty()){last_error_=13;return finish(0);}
    std::vector<std::uint8_t> out;
    const auto dword=[&](std::uint32_t value){out.resize(4);for(unsigned i=0;i<4;++i)out[i]=static_cast<std::uint8_t>(value>>(8*i));};
    if(type==0)dword(static_cast<std::uint32_t>(faces.size()));
    else if(type==3)dword(1);
    else if(type==5)dword(0);
    else if(type==2){for(const auto& face:faces){const auto logical=record(face,true,static_cast<std::uint8_t>(face.signature[4]&1U?0:1),u"").logical;out.insert(out.end(),logical.begin(),logical.begin()+92);}}
    else {
        std::u16string text;
        if(type==4)text=name;
        else for(const auto& face:faces){if(!text.empty())text+=u" & ";text+=face.full_name.empty()?face.family:face.full_name;}
        out=utf16_bytes(text);
    }
    const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
    if(!capacity || !arg(2)){memory.store(GuestAddress{arg(1)},32,out.size());return finish(1);}
    if(capacity<out.size()){last_error_=122;return finish(0);}
    memory.write(GuestAddress{arg(2)},out);memory.store(GuestAddress{arg(1)},32,out.size());return finish(1);
}
