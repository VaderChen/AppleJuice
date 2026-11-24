// setupapi 字串表（StringTable*／pSetupStringTable*）與公用輔助函式（由 windows_setup_inf.cpp 的 invoke_setup_inf 引入）。
// 字串表 handle 是一小塊程序堆積記憶體（不透明指標）；字串另存客體堆積複本，StringTableStringFromId 回傳該複本，生命期到 StringTableDestroy。
// ID 從 1 起算，失敗為 -1；比對預設不分大小寫（旗標 1 區分大小寫），旗標 4 在字串已存在時改寫附加資料；附加資料長度上限由 StringTableInitializeEx 指定。
// 記憶體輔助函式使用程序堆積（與 HeapAlloc／HeapFree 相同）；ANSI 與引擎其他轉碼相同使用 UTF-8。
{
    const auto base_name=name.starts_with("pSetup")?name.substr(6):name;
    const auto guest_copy=[&](std::span<const std::uint8_t> bytes){const auto address=allocate_heap(memory,bytes.size());if(!address)throw std::bad_alloc{};memory.write(GuestAddress{address},bytes);return address;};
    const auto wide_copy=[&](std::u16string_view text){return guest_copy(utf16_bytes(std::u16string(text)));};
    if(base_name.starts_with("StringTable")){
        using Table=SetupInfState::StringTable;auto& tables=setup_inf_->string_tables;const std::uint64_t none=0xffffffffU;
        const auto table=[&](std::uint64_t handle)->Table*{const auto found=tables.find(handle);return found==tables.end()?nullptr:&found->second;};
        const auto entry=[&](Table* t,std::uint64_t id)->Table::Entry*{const auto index=static_cast<std::uint32_t>(id);if(!t || !index || index>t->entries.size())return nullptr;return &t->entries[index-1];};
        const auto same=[](std::u16string_view a,std::u16string_view b,bool sensitive){
            if(a.size()!=b.size())return false;
            for(std::size_t i=0;i<a.size();++i){const auto x=sensitive?a[i]:unicode_case_unit(a[i],true),y=sensitive?b[i]:unicode_case_unit(b[i],true);if(x!=y)return false;}
            return true;};
        const auto find=[&](const Table& t,std::u16string_view text,bool sensitive)->std::uint32_t{
            for(std::size_t i=0;i<t.entries.size();++i)if(same(t.entries[i].text,text,sensitive))return static_cast<std::uint32_t>(i+1);return 0;};
        const auto read_extra=[&](std::uint64_t address,std::uint64_t size,std::uint32_t maximum){
            std::vector<std::uint8_t> bytes(maximum);if(address && size)memory.read(GuestAddress{address},std::span(bytes).first(static_cast<std::size_t>(std::min<std::uint64_t>(size,maximum))));return bytes;};
        const auto create=[&](std::uint32_t maximum){if(tables.size()>=65536)throw std::bad_alloc{};const auto handle=allocate_heap(memory,16);if(!handle)throw std::bad_alloc{};tables[handle].max_extra=maximum;return handle;};
        if(base_name=="StringTableInitialize")return finish(create(0));
        if(base_name=="StringTableInitializeEx"){const auto maximum=static_cast<std::uint32_t>(arg(0));if(maximum>1024*1024)return finish(0);return finish(create(maximum));}
        if(base_name=="StringTableDestroy"){
            if(auto* t=table(arg(0))){for(const auto& item:t->entries)(void)free_heap(memory,item.guest,state);tables.erase(arg(0));(void)free_heap(memory,arg(0),state);}
            return finish(0);
        }
        if(base_name=="StringTableAddString" || base_name=="StringTableAddStringEx"){
            // (Table, String, Flags[, ExtraData, ExtraDataSize])
            auto* t=table(arg(0));if(!t || !arg(1))return finish(none);const bool ex=base_name.ends_with("Ex");const auto flags=static_cast<std::uint32_t>(arg(2));
            if(ex && static_cast<std::uint32_t>(arg(4))>t->max_extra)return finish(none);
            const auto text=read_utf16(memory,GuestAddress{arg(1)},32768);
            if(const auto existing=find(*t,text,flags&1U)){if(ex && (flags&4U))t->entries[existing-1].extra=read_extra(arg(3),static_cast<std::uint32_t>(arg(4)),t->max_extra);return finish(existing);}
            if(t->entries.size()>=0x7fffffff)return finish(none);
            Table::Entry item{text,wide_copy(text),ex?read_extra(arg(3),static_cast<std::uint32_t>(arg(4)),t->max_extra):std::vector<std::uint8_t>(t->max_extra)};
            t->entries.push_back(std::move(item));return finish(t->entries.size());
        }
        if(base_name=="StringTableLookUpString" || base_name=="StringTableLookUpStringEx"){
            // (Table, String, Flags[, ExtraData, ExtraDataSize])：找到時 Ex 版本複製附加資料（最多上限長度）。
            auto* t=table(arg(0));if(!t || !arg(1))return finish(none);
            const auto id=find(*t,read_utf16(memory,GuestAddress{arg(1)},32768),static_cast<std::uint32_t>(arg(2))&1U);if(!id)return finish(none);
            if(base_name.ends_with("Ex") && arg(3) && static_cast<std::uint32_t>(arg(4))){const auto& extra=t->entries[id-1].extra;
                memory.write(GuestAddress{arg(3)},std::span(extra).first(std::min<std::size_t>(extra.size(),static_cast<std::uint32_t>(arg(4)))));}
            return finish(id);
        }
        if(base_name=="StringTableStringFromId"){const auto* item=entry(table(arg(0)),arg(1));return finish(item?item->guest:0);}
        if(base_name=="StringTableStringFromIdEx"){
            // (Table, Id, Buffer, BufferLength*)：長度以字元計、含結尾 NUL；空間不足時回報需要的長度。
            const auto* item=entry(table(arg(0)),arg(1));if(!arg(3))return finish(0);
            if(!item){memory.store(GuestAddress{arg(3)},32,0);return finish(0);}
            const auto needed=item->text.size()+1;const auto capacity=memory.load(GuestAddress{arg(3)},32);memory.store(GuestAddress{arg(3)},32,needed);
            if(!arg(2) || capacity<needed)return finish(0);memory.write(GuestAddress{arg(2)},utf16_bytes(item->text));return finish(1);
        }
        if(base_name=="StringTableGetExtraData" || base_name=="StringTableSetExtraData"){
            // (Table, Id, ExtraData, ExtraDataSize)：長度超過上限時失敗；設定時其餘位元組清 0。
            auto* t=table(arg(0));auto* item=entry(t,arg(1));const auto size=static_cast<std::uint32_t>(arg(3));if(!item || size>t->max_extra)return finish(0);
            if(base_name=="StringTableSetExtraData")item->extra=read_extra(arg(2),size,t->max_extra);
            else if(size){if(!arg(2))return finish(0);memory.write(GuestAddress{arg(2)},std::span(item->extra).first(size));}
            return finish(1);
        }
        if(base_name=="StringTableDuplicate"){
            const auto* t=table(arg(0));if(!t)return finish(0);const auto copy=*t;const auto handle=create(copy.max_extra);auto& target=tables[handle];
            for(const auto& item:copy.entries)target.entries.push_back({item.text,wide_copy(item.text),item.extra});
            return finish(handle);
        }
        if(base_name=="StringTableTrim")return finish(0); // 本實作的儲存沒有可回收的保留空間。
    }
    if(name=="MyMalloc" || name=="pSetupMalloc")return finish(allocate_heap(memory,static_cast<std::uint32_t>(arg(0))));
    if(name=="MyFree" || name=="pSetupFree"){if(arg(0))(void)free_heap(memory,arg(0),state);return finish(0);}
    if(name=="MyRealloc" || name=="pSetupRealloc"){
        const auto size=static_cast<std::uint32_t>(arg(1));return finish(arg(0)?reallocate_heap(memory,arg(0),size,state):allocate_heap(memory,size));
    }
    if(name=="DuplicateString")return finish(arg(0)?wide_copy(read_utf16(memory,GuestAddress{arg(0)},32768)):0);
    if(name=="MultiByteToUnicode")return finish(arg(0)?wide_copy(ansi_to_utf16(read_ansi(memory,arg(0),65536))):0);
    if(name=="UnicodeToMultiByte"){
        if(!arg(0))return finish(0);const auto text=utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)},32768));std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);return finish(guest_copy(bytes));
    }
    if(name=="CaptureStringArg" || name=="CaptureAndConvertAnsiArg"){
        // (String, Captured*) → Win32 錯誤碼：成功時 Captured 為以 MyFree 釋放的 Unicode 複本。
        if(!arg(0) || !arg(1))return finish(87);
        const auto text=name=="CaptureStringArg"?read_utf16(memory,GuestAddress{arg(0)},32768):ansi_to_utf16(read_ansi(memory,arg(0),65536));
        memory.check(GuestAddress{arg(1)},unit,Access::Write);memory.store(GuestAddress{arg(1)},width,wide_copy(text));return finish(0);
    }
    if(name=="IsUserAdmin" || name=="pSetupIsUserAdmin"){
        // 與 IsUserAnAdmin 相同：有效權杖的 Administrators 群組已啟用（篩選後的權杖為拒絕專用時為 FALSE）。
        initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());const auto administrators=sddl_sid("BA");
        return finish(security::token_sid_matches(token,administrators,false) && (!token.restriction_check || security::token_sid_matches(token,administrators,false,true)));
    }
    if(name=="FileExists"){
        // (FileName, WIN32_FIND_DATAW* 可為 NULL)：存在時填入屬性、時間、大小與檔名，不存在時設定 LastError。
        if(!arg(0))return fail(87);const auto path=full_path(read_utf16(memory,GuestAddress{arg(0)},32768));const auto info=filesystem_.information(utf16_to_utf8(path));
        if(info.error)return fail(info.error);
        if(arg(1)){std::vector<std::uint8_t> data(592);const auto put=[&](std::size_t at,std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)data[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
            const auto& m=info.metadata;put(0,m.attributes?m.attributes:0x80,4);put(4,m.created,8);put(12,m.accessed,8);put(20,m.modified,8);put(28,m.size>>32,4);put(32,m.size&0xffffffffU,4);
            const auto slash=path.find_last_of(u"\\/");const auto file=slash==std::u16string::npos?path:path.substr(slash+1);const auto bytes=utf16_bytes(file.substr(0,259));
            std::copy(bytes.begin(),bytes.end(),data.begin()+44);memory.write(GuestAddress{arg(1)},data);}
        return succeed();
    }
    if(name=="pSetupGetGlobalFlags")return finish(setup_inf_->global_flags);
    if(name=="pSetupSetGlobalFlags"){setup_inf_->global_flags=static_cast<std::uint32_t>(arg(0));return finish(0);}
}
