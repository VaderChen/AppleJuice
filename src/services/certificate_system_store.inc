// 憑證系統存放區（ROOT 以外）的登錄持久化（由 windows_certificates.cpp 在 invoke_certificate 之後引入）：
// CurrentUser 為 HKCU\Software\Microsoft\SystemCertificates\<名稱>（依虛擬使用者隔離），LocalMachine 為 HKLM 下同一路徑；
// 每張憑證是 Certificates\<SHA-1 指紋> 的 Blob 值、每份 CRL 是 CRLs\<SHA-1> 的 Blob 值，內容為序列化元素（屬性＋DER）。
// 開啟時讀入；之後每次成功的新增、刪除或屬性設定，把各存放區與上次寫入的內容比對，只寫回差異。
std::u16string WindowsServices::system_certificate_store_root(std::uint32_t location){
    return (location==0x20000U?std::u16string(1,2):registry_current_user_path())+u"\\software\\microsoft\\systemcertificates";
}
std::uint32_t WindowsServices::open_system_certificate_store(CertificateStore& store,std::uint32_t location,const std::u16string& display,std::uint32_t flags){
    std::u16string name=display;for(auto& c:name)c=unicode_case_unit(c,false);
    if(name.empty() || name.size()>255 || name.find(u'\\')!=name.npos)return 87;
    if(!registry_file_.empty() && !runtime_.network.external_io)return 50;
    const bool readonly=(flags&0x8000U)!=0;auto& registry=runtime_.registry;auto& certs=runtime_.certificates;
    RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_,readonly);if(transaction.error)return transaction.error;
    const auto root=system_certificate_store_root(location),path=root+u"\\"+name;const bool exists=registry.keys.contains(path);
    if((flags&0x4000U) && !exists)return 2;if((flags&0x2000U) && exists)return 80;
    if(!exists && !readonly){
        // 依序建立 Software\Microsoft\SystemCertificates\<名稱>，新機碼繼承父機碼的安全性描述元。
        const auto before=registry;const auto base=location==0x20000U?std::u16string(1,2):registry_current_user_path();std::u16string current=base;
        try{
            for(const auto& [folded,shown]:std::initializer_list<std::pair<std::u16string,std::u16string>>{{u"software",u"Software"},{u"microsoft",u"Microsoft"},{u"systemcertificates",u"SystemCertificates"},{name,display}}){
                const auto parent=current;current+=u"\\";current+=folded;if(registry.keys.contains(current))continue;
                registry.keys.try_emplace(parent);(void)registry_security_access(parent,4U);auto descriptor=registry_creation_descriptor(registry_security_descriptor(parent),{});
                registry.keys.try_emplace(current);registry.display_names[current]=shown;registry.modified_times[current]=runtime_.system_filetime();registry.security_descriptors[current]=std::move(descriptor);
            }
            if(const auto error=commit_registry_transaction(transaction)){registry=before;return error;}
        }catch(const SecurityDescriptorError& error){registry=before;return error.code;}
    }
    store.system_path=path;store.system_saved.clear();
    for(const auto* group:{u"certificates",u"crls"}){
        const auto prefix=path+u"\\"+group+u"\\";const bool crl=group[0]==u'c' && group[1]==u'r';
        for(auto item=registry.keys.lower_bound(prefix);item!=registry.keys.end() && item->first.starts_with(prefix);++item){
            if(item->first.find(u'\\',prefix.size())!=std::u16string::npos)continue;const auto value=item->second.find(u"blob");if(value==item->second.end() || value->second.type!=3)continue;
            SerializedStoreElement element;try{element=parse_store_element(value->second.bytes);}catch(const CertificateError&){continue;}
            if(element.type!=(crl?33U:32U))continue;
            try{if(crl)(void)decode_crl(element.der);else(void)decode_certificate(element.der);}catch(const CertificateError&){ERR_clear_error();continue;}
            const auto identity=store.next_identity++,property=certs.next_property_identity++;
            (crl?store.crls:store.items).emplace(identity,element.der);(crl?store.crl_property_identities:store.property_identities).emplace(identity,property);
            certs.properties.emplace(property,std::move(element.properties));store.system_saved.emplace(item->first.substr(path.size()+1),value->second.bytes);
        }
    }
    return 0;
}
std::uint32_t WindowsServices::persist_system_certificate_stores(){
    auto& certs=runtime_.certificates;auto& registry=runtime_.registry;
    std::vector<std::pair<std::uint64_t,std::map<std::u16string,std::vector<std::uint8_t>>>> pending;
    for(const auto& [handle,store]:certs.stores){
        if(store.system_path.empty() || store.readonly || !store.references)continue;std::map<std::u16string,std::vector<std::uint8_t>> current;
        for(const bool crl:{false,true})for(const auto& entry:store_entries(certs,handle,0,crl)){
            CryptoHash hash("SHA1",false,{});hash.update(*entry.der);std::u16string key=crl?u"crls\\":u"certificates\\";
            for(const auto byte:hash.digest()){key+=u"0123456789abcdef"[byte>>4];key+=u"0123456789abcdef"[byte&15];}
            current[key]=serialize_store_element(certs.properties.at(entry.properties),*entry.der,crl?33U:32U);
        }
        if(current!=store.system_saved)pending.emplace_back(handle,std::move(current));
    }
    if(pending.empty())return 0;if(!registry_file_.empty() && !runtime_.network.external_io)return 50;
    RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_,false);if(transaction.error)return transaction.error;const auto before=registry;
    try{
        const auto now=runtime_.system_filetime();
        for(auto& [handle,current]:pending){
            auto& store=certs.stores.at(handle);const auto& path=store.system_path;if(!registry.keys.contains(path))registry.keys.try_emplace(path);
            for(const auto& [key,blob]:store.system_saved)if(!current.contains(key)){
                const auto full=path+u"\\"+key;if(const auto found=registry.keys.find(full);found!=registry.keys.end()){(void)registry_security_access(full,0x10000U);
                    for(const auto& [n,v]:found->second){(void)n;registry.bytes-=v.bytes.size();--registry.values;}registry.keys.erase(found);registry.display_names.erase(full);registry.modified_times.erase(full);registry.security_descriptors.erase(full);}}
            for(const auto& [key,blob]:current){
                if(const auto saved=store.system_saved.find(key);saved!=store.system_saved.end() && saved->second==blob)continue;
                const auto group=path+u"\\"+key.substr(0,key.find(u'\\')),full=path+u"\\"+key;
                for(const auto& [level,shown]:std::initializer_list<std::pair<std::u16string,std::u16string>>{{group,key.starts_with(u"crls")?std::u16string(u"CRLs"):std::u16string(u"Certificates")},{full,key.substr(key.find(u'\\')+1)}}){
                    if(registry.keys.contains(level))continue;const auto parent=level.substr(0,level.find_last_of(u'\\'));(void)registry_security_access(parent,4U);
                    auto descriptor=registry_creation_descriptor(registry_security_descriptor(parent),{});registry.keys.try_emplace(level);
                    auto upper=shown;for(auto& c:upper)if(c>=u'a' && c<=u'f' && level==full)c=static_cast<char16_t>(c-32);registry.display_names[level]=upper;registry.security_descriptors[level]=std::move(descriptor);
                }
                (void)registry_security_access(full,2U);auto& values=registry.keys[full];
                if(const auto old=values.find(u"blob");old!=values.end()){registry.bytes-=old->second.bytes.size();--registry.values;}
                registry.bytes+=blob.size();++registry.values;values[u"blob"]=RegistryValue{3,blob,u"Blob"};registry.modified_times[full]=now;
            }
            registry.modified_times[path]=now;
        }
        if(const auto error=commit_registry_transaction(transaction)){registry=before;return error;}
    }catch(const SecurityDescriptorError& error){registry=before;return error.code;}
    for(auto& [handle,current]:pending)certs.stores.at(handle).system_saved=std::move(current);
    return 0;
}
