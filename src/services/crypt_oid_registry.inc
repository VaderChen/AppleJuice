// crypt32 OID 函式與 OID 資訊的登錄（由 windows_certificates.cpp 的 invoke_certificate 引入，沿用 crypt_oid_registry_members.inc 的登錄存取）：
// - CryptRegisterOIDFunction／CryptUnregisterOIDFunction：EncodingType N\<函式>\<OID> 的 Dll（REG_SZ）與 FuncName；數值 OID 為「#n」。
// - CryptGetOIDFunctionValue／CryptSetOIDFunctionValue：同一機碼的任意值；未給 Dll 的 CryptRegisterOIDFunction 不寫入。
// - CryptRegisterDefaultOIDFunction／CryptUnregisterDefaultOIDFunction：<函式>\DEFAULT 的 Dll（REG_MULTI_SZ）清單；CryptGetDefaultOIDDllList 讀出；
//   CryptGetDefaultOIDFunctionAddress 依清單（或指定的 DLL）載入並取得函式位址，傳入前一個 handle 時從下一個 DLL 接續。
// - CryptRegisterOIDInfo／CryptUnregisterOIDInfo：EncodingType 0\CryptDllFindOIDInfo\<OID>!<群組> 的 Name、Algid、ExtraInfo、Flags、CNGAlgid、CNGExtraAlgid；
//   OID 表只在程序第一次查詢時讀取登錄，因此新登錄的項目由之後啟動的程序看到（CRYPT_INSTALL_OID_INFO_BEFORE_FLAG 排在內建項目之前）；群組為 0 時不寫入。
if(symbol=="CryptRegisterOIDFunction" || symbol=="CryptUnregisterOIDFunction" || symbol=="CryptGetOIDFunctionValue" || symbol=="CryptSetOIDFunctionValue" ||
   symbol=="CryptRegisterDefaultOIDFunction" || symbol=="CryptUnregisterDefaultOIDFunction" || symbol=="CryptGetDefaultOIDDllList" || symbol=="CryptGetDefaultOIDFunctionAddress" ||
   symbol=="CryptRegisterOIDInfo" || symbol=="CryptUnregisterOIDInfo"){
    const auto load=[&](std::uint64_t at){return memory.load(GuestAddress{at},bits);};
    constexpr std::uint32_t invalid_argument=0x80070057U;
    const auto oid_text=[&](std::uint64_t pointer,std::uint32_t error=87)->std::string{if(!pointer)throw CertificateError{error};if(pointer<=65535)return "#"+std::to_string(pointer);return read_ansi(memory,pointer,1024);};
    const auto required=[&](std::uint64_t pointer,std::uint32_t error=87){if(!pointer)throw CertificateError{error};return read_ansi(memory,pointer,1024);};
    const auto wide_value=[](const std::u16string& text,std::uint32_t type=1){return RegistryValue{type,utf16_bytes(text),{}};};
    const auto multi=[](const std::vector<std::u16string>& items){std::vector<std::uint8_t> bytes;for(const auto& item:items){const auto b=utf16_bytes(item);bytes.insert(bytes.end(),b.begin(),b.end());}bytes.push_back(0);bytes.push_back(0);return RegistryValue{7,std::move(bytes),{}};};
    const auto default_list=[&](std::uint32_t encoding,const std::string& function){std::vector<std::u16string> items;
        if(const auto value=crypt_oid_registry_value(encoding,function,"DEFAULT",u"Dll");value && (value->type==7 || value->type==1)){std::u16string current;
            for(std::size_t i=0;i+1<value->bytes.size();i+=2){const auto c=static_cast<char16_t>(value->bytes[i]|(value->bytes[i+1]<<8));if(c){current+=c;continue;}if(current.empty())break;items.push_back(std::move(current));current.clear();}
            if(!current.empty())items.push_back(std::move(current));}
        return items;};
    const auto fold=[](std::u16string text){for(auto& c:text)c=unicode_case_unit(c,false);return text;};
    const auto check=[](std::uint32_t error){if(error)throw CertificateError{error};};
    const auto set_function=[&](std::uint64_t handle)->const CryptOidFunctionSet&{const auto found=certs.oid_sets.find(handle);if(found==certs.oid_sets.end())throw CertificateError{6};return found->second;};
    // 載入 DLL（展開環境變數）並取得函式位址；失敗時卸載並回傳 0。
    const auto load_function=[&](const std::u16string& dll,const std::string& procedure)->std::pair<std::uint64_t,std::uint64_t>{
        GuestScratch scratch(*this,memory,state);std::u16string path=dll;
        if(path.find(u'%')!=std::u16string::npos){const auto source=scratch.allocate((path.size()+1)*2),target=scratch.allocate(2*32768);if(!source || !target)throw CertificateError{8};
            memory.write(GuestAddress{source},utf16_bytes(path));const auto length=nested_service(state,memory,"kernel32.dll","ExpandEnvironmentStringsW",{source,target,32768})&UINT32_MAX;
            if(length && length<=32768)path=read_utf16(memory,GuestAddress{target},32767);}
        const auto name=scratch.allocate((path.size()+1)*2),proc=scratch.allocate(procedure.size()+1);if(!name || !proc)throw CertificateError{8};
        memory.write(GuestAddress{name},utf16_bytes(path));std::vector<std::uint8_t> ansi(procedure.begin(),procedure.end());ansi.push_back(0);memory.write(GuestAddress{proc},ansi);
        auto module=nested_service(state,memory,"kernel32.dll","LoadLibraryW",{name});if(x86_)module&=UINT32_MAX;if(!module)return {0,0};
        auto address=nested_service(state,memory,"kernel32.dll","GetProcAddress",{module,proc});if(x86_)address&=UINT32_MAX;
        if(!address){(void)nested_service(state,memory,"kernel32.dll","FreeLibrary",{module});return {0,0};}return {module,address};};
    if(symbol=="CryptRegisterOIDFunction"){
        // (dwEncodingType, pszFuncName, pszOID, pwszDll, pszOverrideFuncName)
        // 未給 DLL 時不寫入任何內容並回報成功；函式名稱或 OID 為 NULL 時為 E_INVALIDARG。
        if(!arg(3)){result=1;arg.return_to(state,result);return std::nullopt;}
        const auto function=required(arg(1),invalid_argument);const auto oid=oid_text(arg(2),invalid_argument);
        std::vector<std::pair<std::u16string,std::optional<RegistryValue>>> values{{u"Dll",wide_value(read_utf16(memory,GuestAddress{arg(3)},32767))}};
        if(arg(4))values.emplace_back(u"FuncName",wide_value(ansi_to_utf16(read_ansi(memory,arg(4),1024))));
        check(crypt_oid_registry_update(u32(0)&65535U,function,oid,values));result=1;
    }else if(symbol=="CryptUnregisterOIDFunction"){
        const auto function=required(arg(1));check(crypt_oid_registry_update(u32(0)&65535U,function,oid_text(arg(2)),{},true));result=1;
    }else if(symbol=="CryptGetOIDFunctionValue"){
        // (dwEncodingType, pszFuncName, pszOID, pwszValueName, pdwValueType, pbValueData, pcbValueData)
        const auto function=required(arg(1));if(!arg(3))throw CertificateError{87};const auto value=crypt_oid_registry_value(u32(0)&65535U,function,oid_text(arg(2)),read_utf16(memory,GuestAddress{arg(3)},16383));
        if(!value)throw CertificateError{2};if(arg(4))memory.store(GuestAddress{arg(4)},32,value->type);
        if(arg(6)){const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(6)},32));memory.store(GuestAddress{arg(6)},32,value->bytes.size());
            if(arg(5)){if(capacity<value->bytes.size())throw CertificateError{234};if(!value->bytes.empty())memory.write(GuestAddress{arg(5)},value->bytes);}}else if(arg(5))throw CertificateError{87};
        result=1;
    }else if(symbol=="CryptSetOIDFunctionValue"){
        // (dwEncodingType, pszFuncName, pszOID, pwszValueName, dwValueType, pbValueData, cbValueData)
        const auto function=required(arg(1));if(!arg(3))throw CertificateError{87};const auto name=read_utf16(memory,GuestAddress{arg(3)},16383);
        // 與 RegSetValueExW 相同：資料為 NULL 且長度非 0 時為 ERROR_NOACCESS，長度 0 時寫入空值。
        if(!arg(5) && u32(6))throw CertificateError{998};std::vector<std::uint8_t> data(arg(5)?u32(6):0U);if(data.size()>1024*1024)throw CertificateError{8};if(!data.empty())memory.read(GuestAddress{arg(5)},data);
        check(crypt_oid_registry_update(u32(0)&65535U,function,oid_text(arg(2)),{{name,RegistryValue{u32(4),std::move(data),{}}}}));result=1;
    }else if(symbol=="CryptRegisterDefaultOIDFunction" || symbol=="CryptUnregisterDefaultOIDFunction"){
        // Register(dwEncodingType, pszFuncName, dwIndex, pwszDll)／Unregister(dwEncodingType, pszFuncName, pwszDll)
        const bool registering=symbol=="CryptRegisterDefaultOIDFunction";const auto dll_pointer=arg(registering?3:2);if(!dll_pointer)throw CertificateError{invalid_argument};
        const auto function=required(arg(1),invalid_argument);const auto dll=read_utf16(memory,GuestAddress{dll_pointer},32767);if(dll.empty())throw CertificateError{invalid_argument};const auto encoding=u32(0)&65535U;auto items=default_list(encoding,function);
        const auto found=std::find_if(items.begin(),items.end(),[&](const auto& item){return fold(item)==fold(dll);});
        if(registering){if(found!=items.end())throw CertificateError{80};const auto index=u32(2);items.insert(index>=items.size()?items.end():items.begin()+index,dll);}
        else{if(found==items.end())throw CertificateError{2};items.erase(found);}
        check(crypt_oid_registry_update(encoding,function,"DEFAULT",{{u"Dll",items.empty()?std::optional<RegistryValue>{}:std::optional<RegistryValue>{multi(items)}}}));result=1;
    }else if(symbol=="CryptGetDefaultOIDDllList"){
        // (hFuncSet, dwEncodingType, pwszDllList, pcchDllList)：字元數含兩個結尾 NUL。
        const auto& set=set_function(arg(0));if(!arg(3))throw CertificateError{87};const auto items=default_list(u32(1)&65535U,set.original.empty()?set.name:set.original);
        const auto list=multi(items);const auto chars=static_cast<std::uint32_t>(list.bytes.size()/2);const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));
        memory.store(GuestAddress{arg(3)},32,chars);if(arg(2)){if(capacity<chars)throw CertificateError{234};memory.write(GuestAddress{arg(2)},list.bytes);}result=1;
    }else if(symbol=="CryptGetDefaultOIDFunctionAddress"){
        // (hFuncSet, dwEncodingType, pwszDll, dwFlags, ppvFuncAddr, phFuncAddr)
        const auto& set=set_function(arg(0));if(!arg(4) || !arg(5))throw CertificateError{87};const auto function=set.original.empty()?set.name:set.original;const auto encoding=u32(1)&65535U;
        // 指定 DLL 時忽略 *phFuncAddr；否則它是前一次的 handle（第一次為 NULL），釋放後從清單下一個 DLL 接續。
        const auto previous=arg(2)?0U:load(arg(5));memory.store(GuestAddress{arg(4)},bits,0);memory.store(GuestAddress{arg(5)},bits,0);
        std::uint32_t start=0;
        if(previous){const auto entry=certs.oid_addresses.find(previous);if(entry!=certs.oid_addresses.end()){start=entry->second.default_index==UINT32_MAX?0U:entry->second.default_index+1;
            const bool owned=entry->second.loaded;const auto module=entry->second.module;certs.oid_addresses.erase(entry);if(owned && module)(void)nested_service(state,memory,"kernel32.dll","FreeLibrary",{module});}}
        std::vector<std::u16string> candidates;
        if(arg(2))candidates.push_back(read_utf16(memory,GuestAddress{arg(2)},32767));else candidates=default_list(encoding,function);
        for(std::uint32_t i=arg(2)?0U:start;i<candidates.size();++i){
            const auto [module,address]=load_function(candidates[i],function);if(!address)continue;
            if(certs.oid_addresses.size()>=65536){(void)nested_service(state,memory,"kernel32.dll","FreeLibrary",{module});throw CertificateError{8};}
            const auto handle=runtime_.allocate_wait_handle();certs.oid_addresses.emplace(handle,CryptOidFunction{"",address,module,encoding,true,arg(2)?UINT32_MAX:i});
            memory.store(GuestAddress{arg(4)},bits,address);memory.store(GuestAddress{arg(5)},bits,handle);result=1;break;
        }
        if(!result)throw CertificateError{2};
    }else{
        // CryptRegisterOIDInfo(pInfo, dwFlags)／CryptUnregisterOIDInfo(pInfo)
        // 結構、cbSize（至少為不含 CNG 欄位的大小）或 OID 無效時為 E_INVALIDARG；登錄時群組為 0 不寫入並回報成功。
        const bool registering=symbol=="CryptRegisterOIDInfo";const auto info=arg(0);if(!info)throw CertificateError{invalid_argument};
        const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{info},32));const std::uint32_t minimum=x86_?28U:48U;if(size<minimum)throw CertificateError{invalid_argument};
        const auto oid_pointer=load(info+(x86_?4U:8U));if(!oid_pointer)throw CertificateError{invalid_argument};const auto oid=read_ansi(memory,oid_pointer,1024);
        const auto group=static_cast<std::uint32_t>(memory.load(GuestAddress{info+(x86_?12U:24U)},32));const auto key=oid+"!"+std::to_string(group);
        if(registering && !group){result=1;arg.return_to(state,result);return std::nullopt;}
        if(registering){
            const auto name=load(info+(x86_?8U:16U));const auto algid=static_cast<std::uint32_t>(memory.load(GuestAddress{info+(x86_?16U:28U)},32));
            const auto extra_size=static_cast<std::uint32_t>(memory.load(GuestAddress{info+(x86_?20U:32U)},32));const auto extra=load(info+(x86_?24U:40U));if(extra_size>4096 && extra)throw CertificateError{invalid_argument};
            const auto dword=[](std::uint32_t value){return RegistryValue{4,{static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value>>8),static_cast<std::uint8_t>(value>>16),static_cast<std::uint8_t>(value>>24)},{}};};
            // 只寫入有內容的欄位：Flags 非 0、名稱非 NULL、ALG_ID 非 0、ExtraInfo 有資料、CNG 名稱非 NULL。
            std::vector<std::pair<std::u16string,std::optional<RegistryValue>>> values;if(u32(1))values.emplace_back(u"Flags",dword(u32(1)));
            if(name)values.emplace_back(u"Name",wide_value(read_utf16(memory,GuestAddress{name},1024)));if(algid)values.emplace_back(u"Algid",dword(algid));
            if(extra_size && extra){std::vector<std::uint8_t> bytes(extra_size);memory.read(GuestAddress{extra},bytes);values.emplace_back(u"ExtraInfo",RegistryValue{3,std::move(bytes),{}});}
            if(size>=(x86_?36U:64U)){const auto cng=load(info+(x86_?28U:48U)),cng_extra=load(info+(x86_?32U:56U));
                if(cng)values.emplace_back(u"CNGAlgid",wide_value(read_utf16(memory,GuestAddress{cng},256)));if(cng_extra)values.emplace_back(u"CNGExtraAlgid",wide_value(read_utf16(memory,GuestAddress{cng_extra},256)));}
            check(crypt_oid_registry_update(0,"CryptDllFindOIDInfo",key,values));
        }else check(crypt_oid_registry_update(0,"CryptDllFindOIDInfo",key,{},true));
        result=1;
    }
    arg.return_to(state,result);return std::nullopt;
}
