// RegLoadAppKey 的應用程式 hive（由 windows_registry.cpp 在 hive 輔助函式之後引入）。
// hive 掛在不可列舉的 \REGISTRY\A\{GUID}（內部根 0x0a），只能經由回傳的 handle 存取；權威內容保存在本程序，
// 不寫入共用登錄存放區（根標為產生的鍵，序列化時略過），每次交易重新載入存放區後重新安裝。內容變更後寫回 hive 檔，
// 本程序最後一個指向 hive 的 handle 關閉時卸載。同一檔案在本程序重複載入時回傳同一個 hive；REG_PROCESS_APPKEY 的 hive 不可再次載入。
// 限制：不同程序各自載入同一檔案時各有一份內容，最後寫入者為準。
std::uint32_t WindowsServices::load_app_hive(const std::u16string& source,bool exclusive,std::u16string& root){
    auto& registry=runtime_.registry;refresh_file_security_context();
    const auto path=utf16_to_utf8(source);
    if(const auto info=filesystem_.information(path);!info.error){
        for(const auto& [mounted,hive]:app_hives_){
            if(hive.device!=info.metadata.device || hive.id!=info.metadata.id)continue;
            if(hive.exclusive || exclusive)return 32;
            root=mounted;return 0;
        }
    }
    if(app_hives_.size()>=64)return 8;
    auto opened=filesystem_.open_host(path,0xc0000000U,4,0x80,exclusive?0U:3U);if(opened.error)return opened.error;
    std::array<std::uint8_t,16> random{};std::random_device device;for(auto& byte:random)byte=static_cast<std::uint8_t>(device());
    random[6]=static_cast<std::uint8_t>((random[6]&0x0f)|0x40);random[8]=static_cast<std::uint8_t>((random[8]&0x3f)|0x80);
    std::u16string name=u"{";static constexpr char16_t digits[]=u"0123456789ABCDEF";
    for(unsigned i=0;i<16;++i){if(i==4 || i==6 || i==8 || i==10)name+=u'-';name+=digits[random[i]>>4];name+=digits[random[i]&15];}name+=u'}';
    std::u16string folded_name=name;for(auto& c:folded_name)c=unicode_case_unit(c,false);
    const auto target=std::u16string(1,0x0a)+u"\\"+folded_name;
    registry_hive::Tree tree;
    if(!opened.existed){
        registry_hive::Key key;key.name=name;key.modified=runtime_.system_filetime();
        key.security=registry_creation_descriptor(registry_security_descriptor(registry_current_user_path()),{});tree.keys.emplace(u"",std::move(key));
        if(const auto error=registry_hive_write(*opened.file,registry_hive::encode(tree)))return error;
    }else tree=registry_hive_read(*opened.file);
    const auto info=opened.file->information();if(info.error)return info.error;
    const auto minor=tree.minor>=5?5U:3U;
    registry_hive_install(registry,target,name,std::move(tree));registry.generated_keys.insert(target);
    AppHive hive;hive.source=source;hive.file=std::move(opened.file);hive.device=info.metadata.device;hive.id=info.metadata.id;hive.minor=minor;hive.exclusive=exclusive;
    hive.image=registry_hive::encode(registry_hive_tree(registry,target),minor);hive.name=name;
    app_hives_.insert_or_assign(target,std::move(hive));root=target;return 0;
}
void WindowsServices::restore_app_hives(RegistryState& registry){
    for(const auto& [root,hive]:app_hives_){
        if(registry.keys.contains(root))continue;
        registry_hive_install(registry,root,hive.name,registry_hive::decode(hive.image));registry.generated_keys.insert(root);
    }
}
std::uint32_t WindowsServices::persist_app_hives(){
    std::uint32_t result=0;
    for(auto& [root,hive]:app_hives_){
        if(!runtime_.registry.keys.contains(root))continue;
        auto image=registry_hive::encode(registry_hive_tree(runtime_.registry,root),hive.minor);if(image==hive.image)continue;
        if(const auto error=registry_hive_write(*hive.file,image)){if(!result)result=error;continue;}
        hive.image=std::move(image);
    }
    return result;
}
void WindowsServices::release_app_hives(){
    auto& registry=runtime_.registry;
    for(auto it=app_hives_.begin();it!=app_hives_.end();){
        const auto& root=it->first;const auto prefix=root+u"\\";
        const auto inside=[&](const RegistryHandle& handle){return handle.path==root || handle.path.starts_with(prefix);};
        const bool used=std::any_of(registry.handles.begin(),registry.handles.end(),[&](const auto& item){return inside(item.second);}) ||
            std::any_of(runtime_.registry_predefined_overrides.begin(),runtime_.registry_predefined_overrides.end(),[&](const auto& item){return inside(item.second);});
        if(used){++it;continue;}
        (void)persist_app_hives();registry_hive_erase(registry,root);registry.generated_keys.erase(root);it=app_hives_.erase(it);
    }
}
