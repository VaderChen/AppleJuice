// 存放區元素序列化與系統存放區管理（由 windows_certificates.cpp 的 invoke_certificate 引入）：
// - CertSerializeCertificateStoreElement／CertSerializeCRLStoreElement：context 的持久屬性＋DER，與系統存放區 Blob 相同格式。
// - CertAddSerializedElementToStore：解析序列化元素，經 CertAddEncodedCertificateToStore／CertAddEncodedCRLToStore 加入後套用其中的屬性。
// - CertAddEncodedCertificateToSystemStoreA／W：CurrentUser 系統存放區（CERT_STORE_ADD_USE_EXISTING）。
// - CertEnumSystemStore：預先定義的系統存放區與登錄中的存放區；CertEnumPhysicalStore：每個系統存放區的 .Default 實體存放區。
// - CertRegisterSystemStore／CertUnregisterSystemStore：建立或刪除系統存放區的登錄機碼（刪除時連同內容）。
if(symbol=="CertSerializeCertificateStoreElement" || symbol=="CertSerializeCRLStoreElement" || symbol=="CertAddSerializedElementToStore" ||
   symbol.starts_with("CertAddEncodedCertificateToSystemStore") || symbol=="CertEnumSystemStore" || symbol=="CertEnumPhysicalStore" ||
   symbol=="CertRegisterSystemStore" || symbol=="CertUnregisterSystemStore"){
    const auto location_of=[&](std::uint32_t flags){const auto location=flags&0xff0000U;if(location!=0x10000U && location!=0x20000U)throw CertificateError{0x80070057};return location;};
    const auto blob_pointer=[&](std::uint64_t pointer,std::uint32_t size){if(!size || !pointer)throw CertificateError{0x80092004};if(size>output_limit)throw CertificateError{8};std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{pointer},bytes);return bytes;};
    static constexpr const char16_t* predefined[]{u"My",u"Root",u"Trust",u"CA",u"TrustedPublisher",u"Disallowed",u"TrustedPeople",u"AddressBook"};
    if(symbol=="CertSerializeCertificateStoreElement" || symbol=="CertSerializeCRLStoreElement"){
        // (pContext, dwFlags, pbElement, pcbElement)
        const bool crl=symbol=="CertSerializeCRLStoreElement";if(u32(1))throw CertificateError{0x80070057};if(!arg(3))throw CertificateError{87};
        const auto& table=crl?certs.crl_contexts:certs.contexts;const auto found=table.find(arg(0));if(found==table.end())throw CertificateError{0x80070057};
        const auto bytes=serialize_store_element(certs.properties.at(found->second.property_identity),found->second.der,crl?33U:32U);
        const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));memory.store(GuestAddress{arg(3)},32,bytes.size());
        if(arg(2)){if(capacity<bytes.size())throw CertificateError{234};memory.write(GuestAddress{arg(2)},bytes);}result=1;
    }else if(symbol=="CertAddSerializedElementToStore"){
        // (hCertStore, pbElement, cbElement, dwAddDisposition, dwFlags, dwContextTypeFlags, pdwContextType, ppvContext)
        if(u32(4))throw CertificateError{0x80070057};const auto element=parse_store_element(blob_pointer(arg(1),u32(2)));
        const unsigned kind=element.type==32?1U:element.type==33?2U:3U;if(!(u32(5)&(1U<<kind)))throw CertificateError{0x80092004};if(kind==3)throw CertificateError{50};
        if(arg(6))memory.store(GuestAddress{arg(6)},32,0);if(arg(7))memory.store(GuestAddress{arg(7)},bits,0);
        GuestScratch scratch(*this,memory,state);const auto data=scratch.allocate(element.der.size()),slot=scratch.allocate(8);if(!data || !slot)throw CertificateError{8};
        memory.write(GuestAddress{data},element.der);memory.store(GuestAddress{slot},bits,0);
        const auto added=nested_service(state,memory,"crypt32.dll",kind==1?"CertAddEncodedCertificateToStore":"CertAddEncodedCRLToStore",{arg(0),1,data,element.der.size(),arg(3),slot})&UINT32_MAX;
        if(!added)throw CertificateError{last_error_};const auto context=memory.load(GuestAddress{slot},bits);
        auto& table=kind==1?certs.contexts:certs.crl_contexts;if(const auto found=table.find(context);found!=table.end()){auto& properties=certs.properties.at(found->second.property_identity);for(const auto& [id,value]:element.properties)properties[id]=value;}
        if(arg(6))memory.store(GuestAddress{arg(6)},32,kind);
        if(arg(7))memory.store(GuestAddress{arg(7)},bits,context);else(void)nested_service(state,memory,"crypt32.dll",kind==1?"CertFreeCertificateContext":"CertFreeCRLContext",{context});
        result=1;
    }else if(symbol.starts_with("CertAddEncodedCertificateToSystemStore")){
        // (szCertStoreName, pbCertEncoded, cbCertEncoded)
        if(!arg(0))throw CertificateError{87};
        const auto store=nested_service(state,memory,"crypt32.dll",symbol.ends_with('W')?"CertOpenSystemStoreW":"CertOpenSystemStoreA",{0,arg(0)})&(x86_?UINT32_MAX:UINT64_MAX);if(!store)throw CertificateError{last_error_};
        const auto added=nested_service(state,memory,"crypt32.dll","CertAddEncodedCertificateToStore",{store,1,arg(1),arg(2),2,0})&UINT32_MAX;const auto error=last_error_;
        (void)nested_service(state,memory,"crypt32.dll","CertCloseStore",{store,0});if(!added)throw CertificateError{error};result=1;
    }else if(symbol=="CertRegisterSystemStore" || symbol=="CertUnregisterSystemStore"){
        // Register(pvSystemStore, dwFlags, pStoreInfo, pvReserved)／Unregister(pvSystemStore, dwFlags)
        const bool registering=symbol=="CertRegisterSystemStore";if(!arg(0) || (u32(1)&0x80000000U) || (registering && arg(3)))throw CertificateError{87};
        const auto location=location_of(u32(1));const auto display=read_utf16(memory,GuestAddress{arg(0)},256);
        if(registering){CertificateStore temporary;if(const auto error=open_system_certificate_store(temporary,location,display,(u32(1)&0x2000U)|location))throw CertificateError{error};result=1;}
        else{
            if(!registry_file_.empty() && !runtime_.network.external_io)throw CertificateError{50};auto name=display;for(auto& c:name)c=unicode_case_unit(c,false);
            if(name.empty() || name.find(u'\\')!=name.npos)throw CertificateError{87};
            auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_,false);if(transaction.error)throw CertificateError{transaction.error};
            const auto path=system_certificate_store_root(location)+u"\\"+name;if(!registry.keys.contains(path))throw CertificateError{2};const auto before=registry;
            try{(void)registry_security_access(path,0x10000U);const auto prefix=path+u"\\";
                for(auto item=registry.keys.begin();item!=registry.keys.end();){if(item->first!=path && !item->first.starts_with(prefix)){++item;continue;}
                    for(const auto& [n,v]:item->second){(void)n;registry.bytes-=v.bytes.size();--registry.values;}registry.display_names.erase(item->first);registry.modified_times.erase(item->first);registry.security_descriptors.erase(item->first);item=registry.keys.erase(item);}
                if(const auto error=commit_registry_transaction(transaction)){registry=before;throw CertificateError{error};}
            }catch(const SecurityDescriptorError& error){registry=before;throw CertificateError{error.code};}
            for(auto& [handle,store]:certs.stores)if(store.system_path==path){(void)handle;store.system_path.clear();store.system_saved.clear();}
            result=1;
        }
    }else{
        // CertEnumSystemStore(dwFlags, pvSystemStoreLocationPara, pvArg, pfnEnum)／CertEnumPhysicalStore(pvSystemStore, dwFlags, pvArg, pfnEnum)
        const bool physical=symbol=="CertEnumPhysicalStore";const auto flags=u32(physical?1:0);if(!arg(3) || (!physical && arg(1)) || (flags&0x80000000U))throw CertificateError{87};
        const auto location=location_of(flags);std::vector<std::u16string> names;
        {
            if(!registry_file_.empty() && !runtime_.network.external_io)throw CertificateError{50};RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw CertificateError{transaction.error};
            const auto root=system_certificate_store_root(location)+u"\\";for(const auto* name:predefined)names.emplace_back(name);
            for(auto item=runtime_.registry.keys.lower_bound(root);item!=runtime_.registry.keys.end() && item->first.starts_with(root);++item){
                if(item->first.find(u'\\',root.size())!=std::u16string::npos)continue;const auto shown=runtime_.registry.display_names.contains(item->first)?runtime_.registry.display_names.at(item->first):item->first.substr(root.size());
                auto folded=shown;for(auto& c:folded)c=unicode_case_unit(c,false);
                if(std::none_of(names.begin(),names.end(),[&](const auto& existing){auto lower=existing;for(auto& c:lower)c=unicode_case_unit(c,false);return lower==folded;}))names.push_back(shown);
            }
        }
        if(physical){
            if(!arg(0))throw CertificateError{87};auto wanted=read_utf16(memory,GuestAddress{arg(0)},256);for(auto& c:wanted)c=unicode_case_unit(c,false);
            if(std::none_of(names.begin(),names.end(),[&](const auto& existing){auto lower=existing;for(auto& c:lower)c=unicode_case_unit(c,false);return lower==wanted;}))throw CertificateError{2};
        }
        const auto callback=arg(3),context=arg(2),store_name=physical?arg(0):0;const auto unit_size=unit;
        return nested_continuation(state,memory,service.symbol,[this,service,names,callback,context,flags,physical,store_name,unit_size](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);std::uint64_t value=1;const auto width=static_cast<std::uint8_t>(unit_size*8);
            const auto info_size=physical?(unit_size==4?32U:48U):4U;const auto info=allocate_heap(m,info_size+64);if(!info){last_error_=8;call.return_to(s,0);co_return std::nullopt;}
            m.fill(GuestAddress{info},0,info_size+64);m.store(GuestAddress{info},32,info_size);
            if(physical){const char provider[]="SystemRegistry";m.write(GuestAddress{info+info_size},std::span(reinterpret_cast<const std::uint8_t*>(provider),sizeof provider));m.store(GuestAddress{info+unit_size},width,info+info_size);}
            const std::vector<std::u16string> list=physical?std::vector<std::u16string>{u".Default"}:names;
            for(const auto& name:list){
                const auto bytes=utf16_bytes(name);const auto text=allocate_heap(m,bytes.size());if(!text){value=0;last_error_=8;break;}m.write(GuestAddress{text},bytes);
                const auto keep=physical?co_await service_callback::call_value(callback,{store_name,flags,text,info,0,context}):co_await service_callback::call_value(callback,{text,flags,info,0,context});
                (void)free_heap(m,text,s);if(!(keep&UINT32_MAX)){value=0;break;}
            }
            (void)free_heap(m,info,s);call.return_to(s,value);co_return std::nullopt;
        });
    }
    // 屬性在巢狀新增之後才套用，需另外寫回系統存放區。
    if(result && symbol=="CertAddSerializedElementToStore")if(const auto error=persist_system_certificate_stores()){last_error_=error;result=0;}
    arg.return_to(state,result);return std::nullopt;
}
