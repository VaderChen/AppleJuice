// msdmo DMO 登錄（由 windows_system_objects.cpp 引入；在 invoke_system_objects 的 try 區塊內）：
// HKEY_CLASSES_ROOT\DirectShow\MediaObjects\{CLSID} 的預設值為名稱，InputTypes／OutputTypes 為 DMO_PARTIAL_MEDIATYPE（type、subtype）陣列的二進位值，
// DMO_REGISTERF_IS_KEYED 時 Keyed（DWORD 1）；分類為 DirectShow\MediaObjects\Categories\{分類}\{CLSID} 機碼。寫入經 advapi32 登錄服務（同一套權限與 WOW64 檢視），
// 讀取走使用者與機器類別的合併檢視。
// - DMORegister 覆寫既有登錄；DMOUnregister 自指定分類（GUID_NULL 為全部分類）移除，沒有任何分類時刪除 DMO 機碼。
// - DMOEnum 依名稱外的 CLSID 文字排序列出分類內（GUID_NULL 為全部）的 DMO；預設不含 keyed DMO（DMO_ENUMF_INCLUDE_KEYED 才列）；
//   指定輸入／輸出型別時，DMO 只要支援其中一個即符合，型別或子型別為 GUID_NULL 視為萬用。結果在建立列舉器時取快照。
// - IEnumDMO（宿主物件，vtable 與快照放在同一塊客體 heap）：Next 的名稱以 CoTaskMemAlloc 配置；取得數量不足 S_FALSE。
// - DMOGetName 最多 79 字元；DMOGetTypes 回報實際提供數量，DMO 型別多於要求時 S_FALSE；沒有登錄 HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)。
if(symbol.starts_with("DMO") || service.family=="internal-msdmo-enum"){
    using Guid=std::array<std::uint8_t,16>;constexpr Guid null_guid{};
    constexpr std::uint32_t s_ok=0,s_false=1,e_pointer=0x80004003U,e_invalidarg=0x80070057U,e_outofmemory=0x8007000eU,e_nointerface=0x80004002U,e_unexpected=0x8000ffffU,not_found=0x80070002U;
    constexpr std::uint64_t classes_root_handle=0x80000000U;const std::uint64_t classes_root=x86_?classes_root_handle:0xffffffff80000000ULL;
    const std::u16string objects=u"DirectShow\\MediaObjects",categories=u"DirectShow\\MediaObjects\\Categories";
    const auto read_guid=[&](std::uint64_t at){Guid value{};memory.read(GuestAddress{at},value);return value;};
    const auto hresult=[](std::uint64_t status){const auto code=static_cast<std::uint32_t>(status);return code?0x80070000U|(code&0xffffU):0U;};
    struct DmoRecord {Guid clsid{};std::u16string name;std::vector<std::uint8_t> inputs,outputs;bool keyed{},present{};};
    // 合併檢視：使用者類別、HKCR、機器類別；子機碼名稱以摺疊後的文字回傳。
    const auto with_classes=[&](const auto& visit){
        if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw std::runtime_error("登錄交易失敗");initialize_tokens();
        const RegistryTextView lookup{runtime_.registry,class_registry_roots(runtime_.registry,token_user_sid_string(effective_token_id()),token_user_sid_string(1)),registry_view(0,x86_)};
        return visit(lookup);};
    const auto subkeys=[&](const RegistryTextView& lookup,const std::u16string& path){
        std::set<std::u16string> names;
        for(const auto& root:lookup.roots){const auto prefix=lookup.path_key(root+u"\\"+path)+u"\\";
            for(auto it=runtime_.registry.keys.lower_bound(prefix);it!=runtime_.registry.keys.end() && it->first.starts_with(prefix);++it){
                auto rest=it->first.substr(prefix.size());const auto slash=rest.find(u'\\');if(slash!=rest.npos)rest.resize(slash);if(!rest.empty())names.insert(rest);}}
        return names;};
    const auto raw=[&](const RegistryTextView& lookup,const std::u16string& path,const std::u16string& name)->const RegistryValue*{
        for(const auto& root:lookup.roots){const auto key=runtime_.registry.keys.find(lookup.path_key(root+u"\\"+path));if(key==runtime_.registry.keys.end())continue;
            const auto value=key->second.find(name);return value==key->second.end()?nullptr:&value->second;}
        return nullptr;};
    const auto record=[&](const RegistryTextView& lookup,const Guid& clsid){
        DmoRecord out;out.clsid=clsid;const auto path=objects+u"\\"+windows_guid_text(clsid);out.present=lookup.exists(path);if(!out.present)return out;
        if(const auto name=lookup.value(path,u""))out.name=*name;
        if(const auto* value=raw(lookup,path,u"inputtypes");value && value->type==3)out.inputs=value->bytes;
        if(const auto* value=raw(lookup,path,u"outputtypes");value && value->type==3)out.outputs=value->bytes;
        out.inputs.resize(out.inputs.size()/32*32);out.outputs.resize(out.outputs.size()/32*32);
        out.keyed=raw(lookup,path,u"keyed")!=nullptr;return out;};
    const auto host=[&](std::string_view name,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,"advapi32.dll",name,args)&UINT32_MAX;};
    const auto read_types=[&](std::uint64_t count,std::uint64_t pointer){if(count>4096)throw MemoryFault(GuestAddress{pointer},Access::Read,0,"型別過多");std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count*32));if(count)memory.read(GuestAddress{pointer},bytes);return bytes;};
    const auto matches=[](const std::vector<std::uint8_t>& supported,const std::vector<std::uint8_t>& wanted){
        if(wanted.empty())return true;
        for(std::size_t w=0;w<wanted.size();w+=32)for(std::size_t s=0;s<supported.size();s+=32){bool ok=true;
            for(unsigned part=0;part<2 && ok;++part){const auto* a=&wanted[w+part*16];const auto* b=&supported[s+part*16];
                const bool wildcard=std::all_of(a,a+16,[](std::uint8_t c){return !c;}) || std::all_of(b,b+16,[](std::uint8_t c){return !c;});if(!wildcard && !std::equal(a,a+16,b))ok=false;}
            if(ok)return true;}
        return false;};
    const auto task_text=[&](const std::u16string& text)->std::uint64_t{const auto bytes=utf16_bytes(text);const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)return 0;gui_.task_allocations.insert(pointer);memory.write(GuestAddress{pointer},bytes);return pointer;};
    // IEnumDMO：+0 vtable 指標、+unit 識別碼與參考數、+unit+8 數量與位置、+unit+16 自帶 vtable（7 個方法）、其後每筆 {CLSID, 名稱位移, 名稱長度} 與名稱。
    constexpr std::uint32_t enum_magic=0x454f4d44U;const std::uint64_t table_at=unit+16,entries_at=table_at+7U*unit;
    const auto make_enum=[&](const std::vector<DmoRecord>& items,std::uint32_t position)->std::uint64_t{
        std::size_t size=static_cast<std::size_t>(entries_at+24*items.size());for(const auto& item:items)size+=item.name.size()*2;
        const auto object=allocate_heap(memory,size);if(!object)return 0;memory.fill(GuestAddress{object},0,size);
        memory.store(GuestAddress{object},bits,object+table_at);memory.store(GuestAddress{object+unit},32,enum_magic);memory.store(GuestAddress{object+unit+4},32,1);
        memory.store(GuestAddress{object+unit+8},32,items.size());memory.store(GuestAddress{object+unit+12},32,position);
        static constexpr std::array<std::string_view,7> methods{"QueryInterface","AddRef","Release","Next","Skip","Reset","Clone"};
        for(unsigned i=0;i<methods.size();++i)memory.store(GuestAddress{object+table_at+i*unit},bits,resolve("internal-msdmo-enum",methods[i]).value);
        auto text=entries_at+24*items.size();
        for(std::size_t i=0;i<items.size();++i){const auto at=object+entries_at+24*i;memory.write(GuestAddress{at},items[i].clsid);
            memory.store(GuestAddress{at+16},32,text);memory.store(GuestAddress{at+20},32,items[i].name.size());
            for(std::size_t c=0;c<items[i].name.size();++c)memory.store(GuestAddress{object+text+c*2},16,items[i].name[c]);text+=items[i].name.size()*2;}
        return object;};
    if(service.family=="internal-msdmo-enum"){
        const auto self=arg(0);
        if(!self || memory.load(GuestAddress{self},bits)!=self+table_at || memory.load(GuestAddress{self+unit},32)!=enum_magic)return finish(e_unexpected);
        const auto references=static_cast<std::uint32_t>(memory.load(GuestAddress{self+unit+4},32)),count=static_cast<std::uint32_t>(memory.load(GuestAddress{self+unit+8},32));
        auto position=static_cast<std::uint32_t>(memory.load(GuestAddress{self+unit+12},32));
        if(symbol=="QueryInterface"){
            if(!arg(2))return finish(e_pointer);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return finish(e_invalidarg);
            static constexpr Guid unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},enum_dmo{0x8a,0xd9,0x3c,0x2c,0xfa,0x2b,0x53,0x4a,0x9c,0x27,0x52,0x49,0xba,0x64,0xba,0x0f};
            const auto iid=read_guid(arg(1));if(iid!=unknown && iid!=enum_dmo)return finish(e_nointerface);
            memory.store(GuestAddress{self+unit+4},32,references+1);memory.store(GuestAddress{arg(2)},bits,self);return finish(s_ok);
        }
        if(symbol=="AddRef"){memory.store(GuestAddress{self+unit+4},32,references+1);return finish(references+1);}
        if(symbol=="Release"){
            const auto left=references?references-1:0U;memory.store(GuestAddress{self+unit+4},32,left);
            if(!left){memory.store(GuestAddress{self+unit},32,0);(void)free_heap(memory,self,state);}return finish(left);
        }
        if(symbol=="Reset"){memory.store(GuestAddress{self+unit+12},32,0);return finish(s_ok);}
        if(symbol=="Skip"){const auto wanted=u32(1);const auto moved=std::min(wanted,count-std::min(position,count));memory.store(GuestAddress{self+unit+12},32,position+moved);return finish(moved==wanted?s_ok:s_false);}
        if(symbol=="Clone"){
            if(!arg(1))return finish(e_pointer);std::vector<DmoRecord> items(count);
            for(std::uint32_t i=0;i<count;++i){const auto at=self+entries_at+24*i;memory.read(GuestAddress{at},items[i].clsid);const auto text=memory.load(GuestAddress{at+16},32),length=memory.load(GuestAddress{at+20},32);
                for(std::uint64_t c=0;c<length;++c)items[i].name.push_back(static_cast<char16_t>(memory.load(GuestAddress{self+text+c*2},16)));}
            const auto copy=make_enum(items,position);if(!copy)return finish(e_outofmemory);memory.store(GuestAddress{arg(1)},bits,copy);return finish(s_ok);
        }
        // Next(cItemsToFetch, CLSID*, WCHAR** Names, DWORD* pcItemsFetched)
        const auto wanted=u32(1);if(!arg(2) || (!arg(4) && wanted>1))return finish(e_pointer);
        std::uint32_t fetched=0;
        for(;fetched<wanted && position<count;++fetched,++position){
            const auto at=self+entries_at+24*position;memory.move(GuestAddress{arg(2)+16ULL*fetched},GuestAddress{at},16);
            if(arg(3)){std::u16string name;const auto text=memory.load(GuestAddress{at+16},32),length=memory.load(GuestAddress{at+20},32);
                for(std::uint64_t c=0;c<length;++c)name.push_back(static_cast<char16_t>(memory.load(GuestAddress{self+text+c*2},16)));
                const auto pointer=task_text(name);if(!pointer)return finish(e_outofmemory);memory.store(GuestAddress{arg(3)+std::uint64_t{fetched}*unit},bits,pointer);}
        }
        memory.store(GuestAddress{self+unit+12},32,position);if(arg(4))memory.store(GuestAddress{arg(4)},32,fetched);return finish(fetched==wanted?s_ok:s_false);
    }
    if(symbol=="DMORegister"){
        // (szName, clsidDMO, guidCategory, dwFlags, cInTypes, pInTypes, cOutTypes, pOutTypes)
        if(!arg(1) || !arg(2))return finish(e_pointer);if(u32(3)&~1U)return finish(e_invalidarg);
        if((u32(4) && !arg(5)) || (u32(6) && !arg(7)))return finish(e_pointer);
        const auto clsid=read_guid(arg(1)),category=read_guid(arg(2));const auto name=arg(0)?read_utf16(memory,GuestAddress{arg(0)},32767):std::u16string{};
        const auto inputs=read_types(u32(4),arg(5)),outputs=read_types(u32(6),arg(7));
        GuestScratch temporary(*this,memory,state);const auto path=objects+u"\\"+windows_guid_text(clsid);
        const auto key=temporary.text(path),name_text=temporary.text(name),in_name=temporary.text(u"InputTypes"),out_name=temporary.text(u"OutputTypes"),keyed_name=temporary.text(u"Keyed");
        const auto in_data=temporary.allocate(std::max<std::size_t>(inputs.size(),1)),out_data=temporary.allocate(std::max<std::size_t>(outputs.size(),1)),one=temporary.allocate(4);
        const auto category_key=temporary.text(categories+u"\\"+windows_guid_text(category)+u"\\"+windows_guid_text(clsid)),slot=temporary.allocate(unit);
        if(!key || !name_text || !in_name || !out_name || !keyed_name || !in_data || !out_data || !one || !category_key || !slot)return finish(e_outofmemory);
        if(!inputs.empty())memory.write(GuestAddress{in_data},inputs);if(!outputs.empty())memory.write(GuestAddress{out_data},outputs);memory.store(GuestAddress{one},32,1);
        if(const auto status=host("RegSetKeyValueW",{classes_root,key,0,1,name_text,(name.size()+1)*2}))return finish(hresult(status));
        if(const auto status=host("RegSetKeyValueW",{classes_root,key,in_name,3,in_data,inputs.size()}))return finish(hresult(status));
        if(const auto status=host("RegSetKeyValueW",{classes_root,key,out_name,3,out_data,outputs.size()}))return finish(hresult(status));
        if(u32(3)&1U){if(const auto status=host("RegSetKeyValueW",{classes_root,key,keyed_name,4,one,4}))return finish(hresult(status));}
        else (void)host("RegDeleteKeyValueW",{classes_root,key,keyed_name});
        memory.store(GuestAddress{slot},bits,0);
        if(const auto status=host("RegCreateKeyExW",{classes_root,category_key,0,0,0,0xf003f,0,slot,0}))return finish(hresult(status));
        (void)host("RegCloseKey",{memory.load(GuestAddress{slot},bits)});return finish(s_ok);
    }
    if(symbol=="DMOUnregister"){
        // (clsidDMO, guidCategory)：GUID_NULL 分類為全部分類。
        if(!arg(0) || !arg(1))return finish(e_pointer);const auto clsid=read_guid(arg(0)),category=read_guid(arg(1));const auto clsid_text=registry_text_fold(windows_guid_text(clsid));
        std::vector<std::u16string> listed;bool remaining=false;
        with_classes([&](const RegistryTextView& lookup){for(const auto& item:subkeys(lookup,categories)){const auto members=subkeys(lookup,categories+u"\\"+item);if(!members.contains(clsid_text))continue;
            if(category==null_guid || registry_text_fold(windows_guid_text(category))==item)listed.push_back(item);else remaining=true;}return 0;});
        GuestScratch temporary(*this,memory,state);
        for(const auto& item:listed){const auto path=temporary.text(categories+u"\\"+item+u"\\"+windows_guid_text(clsid));if(!path)return finish(e_outofmemory);(void)host("RegDeleteTreeW",{classes_root,path});}
        if(!remaining){const auto path=temporary.text(objects+u"\\"+windows_guid_text(clsid));if(!path)return finish(e_outofmemory);(void)host("RegDeleteTreeW",{classes_root,path});}
        return finish(s_ok);
    }
    if(symbol=="DMOGetName" || symbol=="DMOGetTypes"){
        if(!arg(0))return finish(e_pointer);const auto clsid=read_guid(arg(0));
        const auto item=with_classes([&](const RegistryTextView& lookup){return record(lookup,clsid);});if(!item.present)return finish(not_found);
        if(symbol=="DMOGetName"){
            // (clsidDMO, WCHAR szName[80])
            if(!arg(1))return finish(e_pointer);auto name=item.name;if(name.size()>79)name.resize(79);memory.write(GuestAddress{arg(1)},utf16_bytes(name));return finish(s_ok);
        }
        // (clsidDMO, ulInputTypesRequested, *pulInputTypesSupplied, pInputTypes, ulOutputTypesRequested, *pulOutputTypesSupplied, pOutputTypes)
        if(!arg(2) || !arg(5) || (u32(1) && !arg(3)) || (u32(4) && !arg(6)))return finish(e_pointer);
        const auto give=[&](const std::vector<std::uint8_t>& types,std::uint32_t requested,std::uint64_t supplied,std::uint64_t output){
            const auto available=static_cast<std::uint32_t>(types.size()/32),count=std::min(available,requested);
            if(count)memory.write(GuestAddress{output},std::span(types).first(std::size_t{count}*32));memory.store(GuestAddress{supplied},32,count);return available>requested;};
        const bool more_inputs=give(item.inputs,u32(1),arg(2),arg(3)),more_outputs=give(item.outputs,u32(4),arg(5),arg(6));
        return finish(more_inputs || more_outputs?s_false:s_ok);
    }
    // DMOEnum(guidCategory, dwFlags, cInTypes, pInTypes, cOutTypes, pOutTypes, IEnumDMO** ppEnum)
    if(!arg(6))return finish(e_pointer);memory.store(GuestAddress{arg(6)},bits,0);if(!arg(0))return finish(e_pointer);if(u32(1)&~1U)return finish(e_invalidarg);
    if((u32(2) && !arg(3)) || (u32(4) && !arg(5)))return finish(e_pointer);
    const auto category=read_guid(arg(0));const auto wanted_inputs=read_types(u32(2),arg(3)),wanted_outputs=read_types(u32(4),arg(5));const bool keyed=(u32(1)&1U)!=0;
    auto items=with_classes([&](const RegistryTextView& lookup){
        std::set<std::u16string> names;
        if(category==null_guid){for(const auto& item:subkeys(lookup,objects))if(item!=u"categories")names.insert(item);}
        else names=subkeys(lookup,categories+u"\\"+windows_guid_text(category));
        std::vector<DmoRecord> out;
        for(const auto& text:names){const auto parsed=parse_windows_guid(text,true);if(!parsed)continue;auto item=record(lookup,*parsed);
            if(!item.present || (item.keyed && !keyed) || !matches(item.inputs,wanted_inputs) || !matches(item.outputs,wanted_outputs))continue;out.push_back(std::move(item));}
        return out;});
    const auto object=make_enum(items,0);if(!object)return finish(e_outofmemory);memory.store(GuestAddress{arg(6)},bits,object);return finish(s_ok);
}
