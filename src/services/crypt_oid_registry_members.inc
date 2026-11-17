// crypt32 OID 函式登錄的登錄存取（由 windows_certificates.cpp 在 invoke_certificate 之後引入）：
// HKLM\SOFTWARE\Microsoft\Cryptography\OID\EncodingType N\<函式名稱>\<OID>（OID 為空時只到函式名稱），依呼叫端套用 WOW64 檢視；
// 寫入沿用登錄的安全性檢查與交易，新機碼繼承父機碼的描述元。
std::u16string WindowsServices::crypt_oid_registry_path(std::uint32_t encoding,std::string_view function,std::string_view oid,std::vector<std::pair<std::u16string,std::u16string>>* levels){
    const auto fold=[](std::u16string text){for(auto& c:text)c=unicode_case_unit(c,false);return text;};
    std::vector<std::pair<std::u16string,std::u16string>> parts{{u"software",u"SOFTWARE"},{u"microsoft",u"Microsoft"},{u"cryptography",u"Cryptography"},{u"oid",u"OID"}};
    const auto type=utf8_to_utf16("EncodingType "+std::to_string(encoding));parts.emplace_back(fold(type),type);
    const auto name=utf8_to_utf16(std::string(function));parts.emplace_back(fold(name),name);
    if(!oid.empty()){const auto key=utf8_to_utf16(std::string(oid));parts.emplace_back(fold(key),key);}
    std::u16string logical(1,2);if(levels)levels->clear();
    for(const auto& [folded,display]:parts){logical+=u"\\";logical+=folded;if(levels)levels->emplace_back(registry_view_path(logical,registry_view(0,x86_)),display);}
    return registry_view_path(logical,registry_view(0,x86_));
}
std::optional<RegistryValue> WindowsServices::crypt_oid_registry_value(std::uint32_t encoding,std::string_view function,std::string_view oid,std::u16string_view name){
    if(!registry_file_.empty() && !runtime_.network.external_io)return std::nullopt;
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return std::nullopt;initialize_registry_views(runtime_.registry);
    const auto key=runtime_.registry.keys.find(crypt_oid_registry_path(encoding,function,oid));if(key==runtime_.registry.keys.end())return std::nullopt;
    std::u16string folded(name);for(auto& c:folded)c=unicode_case_unit(c,false);const auto value=key->second.find(folded);if(value==key->second.end())return std::nullopt;return value->second;
}
std::vector<std::pair<std::u16string,std::map<std::u16string,RegistryValue>>> WindowsServices::crypt_oid_registry_children(std::uint32_t encoding,std::string_view function){
    std::vector<std::pair<std::u16string,std::map<std::u16string,RegistryValue>>> out;if(!registry_file_.empty() && !runtime_.network.external_io)return out;
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return out;initialize_registry_views(runtime_.registry);
    const auto prefix=crypt_oid_registry_path(encoding,function,{})+u"\\";auto& registry=runtime_.registry;
    for(auto item=registry.keys.lower_bound(prefix);item!=registry.keys.end() && item->first.starts_with(prefix);++item){
        if(item->first.find(u'\\',prefix.size())!=std::u16string::npos)continue;
        out.emplace_back(registry.display_names.contains(item->first)?registry.display_names.at(item->first):item->first.substr(prefix.size()),item->second);
    }
    return out;
}
std::uint32_t WindowsServices::crypt_oid_registry_update(std::uint32_t encoding,std::string_view function,std::string_view oid,const std::vector<std::pair<std::u16string,std::optional<RegistryValue>>>& values,bool remove_key){
    if(!registry_file_.empty() && !runtime_.network.external_io)return 50;
    auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_,false);if(transaction.error)return transaction.error;initialize_registry_views(registry);
    std::vector<std::pair<std::u16string,std::u16string>> levels;const auto path=crypt_oid_registry_path(encoding,function,oid,&levels);const auto before=registry;const auto now=runtime_.system_filetime();
    try{
        if(remove_key){
            if(!registry.keys.contains(path))return 2;(void)registry_security_access(path,0x10000U);const auto prefix=path+u"\\";
            for(auto item=registry.keys.begin();item!=registry.keys.end();){if(item->first!=path && !item->first.starts_with(prefix)){++item;continue;}
                for(const auto& [n,v]:item->second){(void)n;registry.bytes-=v.bytes.size();--registry.values;}registry.display_names.erase(item->first);registry.modified_times.erase(item->first);registry.security_descriptors.erase(item->first);item=registry.keys.erase(item);}
        }else{
            for(const auto& [level,display]:levels){
                if(registry.keys.contains(level))continue;const auto parent=level.substr(0,level.find_last_of(u'\\'));registry.keys.try_emplace(parent);(void)registry_security_access(parent,4U);
                auto descriptor=registry_creation_descriptor(registry_security_descriptor(parent),{});registry.keys.try_emplace(level);registry.display_names[level]=display;registry.modified_times[level]=now;registry.security_descriptors[level]=std::move(descriptor);
            }
            if(!values.empty())(void)registry_security_access(path,2U);auto& key=registry.keys[path];
            for(const auto& [name,value]:values){std::u16string folded=name;for(auto& c:folded)c=unicode_case_unit(c,false);
                if(const auto old=key.find(folded);old!=key.end()){registry.bytes-=old->second.bytes.size();--registry.values;key.erase(old);}
                if(value){auto stored=*value;stored.display_name=name;registry.bytes+=stored.bytes.size();++registry.values;key[folded]=std::move(stored);}}
            registry.modified_times[path]=now;
        }
        if(const auto error=commit_registry_transaction(transaction)){registry=before;return error;}
    }catch(const SecurityDescriptorError& error){registry=before;return error.code;}
    return 0;
}
