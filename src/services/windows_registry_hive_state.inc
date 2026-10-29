namespace {
registry_hive::Tree registry_hive_read(HostFile& file){
    const auto info=file.information();if(info.error)throw registry_hive::Error{info.error};
    if(info.metadata.attributes&0x10U)throw registry_hive::Error{87};
    if(info.metadata.size>32U*1024U*1024U)throw registry_hive::Error{8};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.metadata.size));std::size_t offset=0;
    while(offset<bytes.size()){const auto read=file.read(std::span(bytes).subspan(offset),offset);if(read.error)throw registry_hive::Error{read.error};
        if(!read.bytes || read.bytes>bytes.size()-offset)throw registry_hive::Error{1015};offset+=read.bytes;}
    return registry_hive::decode(bytes);
}
void registry_hive_erase(RegistryState& registry,const std::u16string& root){
    const auto prefix=root+u"\\";
    for(auto item=registry.keys.begin();item!=registry.keys.end();){
        if(item->first!=root && !item->first.starts_with(prefix)){++item;continue;}
        for(const auto& [name,value]:item->second){(void)name;registry.bytes-=value.bytes.size();--registry.values;}
        registry.display_names.erase(item->first);registry.modified_times.erase(item->first);registry.security_descriptors.erase(item->first);
        registry.classes.erase(item->first);registry.volatile_keys.erase(item->first);item=registry.keys.erase(item);
    }
}
void registry_hive_install(RegistryState& registry,const std::u16string& root,const std::u16string& name,registry_hive::Tree tree){
    std::uint64_t count=0,size=0;for(const auto& [path,key]:tree.keys){(void)path;count+=key.values.size();for(const auto& [value_name,value]:key.values){(void)value_name;size+=value.bytes.size();}}
    if(registry.keys.size()+tree.keys.size()>4096 || registry.values+count>4096 || registry.bytes+size>16U*1024U*1024U)throw registry_hive::Error{8};
    for(auto& [path,key]:tree.keys){const auto full=path.empty()?root:root+u"\\"+path;
        if(registry.keys.contains(full))throw registry_hive::Error{183};
        registry.keys.emplace(full,std::move(key.values));registry.display_names[full]=path.empty()?name:key.name;registry.modified_times[full]=key.modified;
        if(!key.security.empty())registry.security_descriptors[full]=std::move(key.security);if(!key.class_name.empty())registry.classes[full]=std::move(key.class_name);
    }
    registry.bytes+=size;registry.values+=count;
}
}
struct WindowsServices::RegistrySession {
    int gate{-1},lease{-1};bool first{};
    explicit RegistrySession(const std::filesystem::path& path){
        // gate 防止「最後一個程序離開」與新程序判斷啟動邊界競爭；lease 持有至服務結束。
        const auto fail=[&]{if(lease>=0)::close(lease);if(gate>=0)::close(gate);lease=gate=-1;throw HostCallFault("無法取得 sandbox 登錄 session 鎖");};
        gate=::open((path.string()+".session-start").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        if(gate<0 || ::flock(gate,LOCK_EX))fail();
        lease=::open((path.string()+".session").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(lease<0)fail();
        if(::flock(lease,LOCK_EX|LOCK_NB)==0)first=true;
        else if((errno!=EWOULDBLOCK && errno!=EAGAIN) || ::flock(lease,LOCK_SH))fail();
    }
    void ready(){if(::flock(lease,LOCK_SH))throw HostCallFault("無法發布 sandbox 登錄 session");::flock(gate,LOCK_UN);::close(gate);gate=-1;}
    ~RegistrySession(){if(lease>=0)::close(lease);if(gate>=0)::close(gate);}
};
void WindowsServices::initialize_registry_session(){
    auto session=std::make_shared<RegistrySession>(registry_file_);
    if(session->first){
        auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_);
        if(transaction.error){session->ready();registry_session_=std::move(session);return;}
        const auto before=registry;const char* phase="decode";
        try{
            const auto volatile_roots=registry.volatile_keys;for(const auto& root:volatile_roots)registry_hive_erase(registry,root);
            for(const auto& [root,bytes]:registry.replacements){
                auto tree=registry_hive::decode(bytes);const auto name=registry.display_names.contains(root)?registry.display_names.at(root):root.substr(root.find_last_of(u'\\')+1);
                registry_hive_erase(registry,root);registry_hive_install(registry,root,name,std::move(tree));
            }
            if(!before.replacements.empty() || !volatile_roots.empty()){
                phase="flush";
                if(const auto error=flush_registry_hives())throw registry_hive::Error{error};registry.replacements.clear();
                phase="commit";
                if(const auto error=transaction.commit())throw registry_hive::Error{error};
            }else transaction.unchanged();
        }catch(const registry_hive::Error& error){registry=before;throw HostCallFault(std::string("sandbox 登錄初始化失敗：")+phase+" error="+std::to_string(error.code));}
        catch(...){registry=before;throw;}
    }
    session->ready();registry_session_=std::move(session);
}
std::uint32_t WindowsServices::save_registry_hive(const std::u16string& root,HostFile& file,std::uint32_t minor){
    for(const auto& [path,values]:runtime_.registry.keys){(void)values;if(path==root || path.starts_with(root+u"\\"))(void)registry_security_descriptor(path);}
    return registry_hive_write(file,registry_hive::encode(registry_hive_tree(runtime_.registry,root),minor));
}
std::uint32_t WindowsServices::load_registry_hive(RegistryTransaction& transaction,const std::u16string& target,const std::u16string& name,const std::u16string& source,std::uint32_t flags){
    auto& registry=runtime_.registry;if(flags&~4U)return 87;
    const auto slash=target.find_last_of(u'\\');if(slash==target.npos || (target[0]!=2 && target[0]!=3) || name.empty() || name.size()>255 || source.empty())return 87;
    if(!registry.keys.contains(target.substr(0,slash)))return 3;
    if(registry.keys.contains(target))return 183;if(registry.hives.size()>=128)return 8;
    refresh_file_security_context();auto opened=filesystem_.open_host(utf16_to_utf8(source),0xc0000000U,4,0x80,7);if(opened.error)return opened.error;
    registry_hive::Tree tree;if(!opened.existed){registry_hive::Key root;root.name=name;root.modified=runtime_.system_filetime();root.security=registry_creation_descriptor(registry_security_descriptor(target.substr(0,slash)),{});tree.keys.emplace(u"",std::move(root));
        if(const auto error=registry_hive_write(*opened.file,registry_hive::encode(tree)))return error;
    }else tree=registry_hive_read(*opened.file);
    const auto info=opened.file->information();if(info.error)return info.error;
    const auto location=filesystem_.final_path(*opened.file,utf16_to_utf8(source));if(location.error)return location.error;
    const auto before=registry;const auto minor=tree.minor>=5?5U:3U;
    try{
        registry_hive_install(registry,target,name,std::move(tree));
        registry.hives[target]={utf8_to_utf16(location.key),info.metadata.device,info.metadata.id,minor,flags};registry_hive_files_[target]=std::move(opened.file);
        if(const auto error=transaction.commit()){registry=before;registry_hive_files_.erase(target);return error;}
        poll_registry_notifications(false,target.substr(0,slash),1);return 0;
    }catch(...){registry=before;registry_hive_files_.erase(target);throw;}
}
std::uint32_t WindowsServices::unload_registry_hive(RegistryTransaction& transaction,const std::u16string& root){
    auto& registry=runtime_.registry;if(!registry.hives.contains(root))return 2;const auto prefix=root+u"\\";
    for(const auto& [handle,item]:registry.handles){(void)handle;if(item.path==root || item.path.starts_with(prefix))return 170;}
    if(registry.replacements.contains(root))return 170;
    if(const auto error=flush_registry_hives())return error;const auto before=registry;
    registry_hive_erase(registry,root);registry.hives.erase(root);
    if(const auto error=transaction.commit()){registry=before;return error;}
    registry_hive_files_.erase(root);poll_registry_notifications(false,root.substr(0,root.find_last_of(u'\\')),1);return 0;
}
std::uint32_t WindowsServices::restore_registry_hive(RegistryTransaction& transaction,const std::u16string& root,HostFile* file,std::uint32_t flags,std::uint64_t handle){
    auto& registry=runtime_.registry;if(flags!=0 && flags!=8 && flags!=2 && flags!=1)return 87;
    // 記憶體 hive 的頂層根由 Windows Configuration Manager 決定；尚無此 provider 時明確拒絕。
    if(flags==1)return 50;
    if(flags!=8)for(const auto& [id,key]:registry.handles)if(id!=handle && (key.path==root || key.path.starts_with(root+u"\\")))return 170;
    for(const auto& [mount,info]:registry.hives){(void)info;if(mount.starts_with(root+u"\\"))return 170;}
    std::shared_ptr<HostFile> refresh;
    if(flags==2){
        const auto mount=registry.hives.find(root);if(mount==registry.hives.end() || !(mount->second.flags&4U))return 87;
        if(!runtime_.token_objects.at(effective_token_id()).has_privilege(7,true))return 1314;
        refresh_file_security_context();auto opened=filesystem_.open_host(utf16_to_utf8(mount->second.source),0x80000000U,3,0x80,7);if(opened.error)return opened.error;
        if(opened.metadata.device!=mount->second.device || opened.metadata.id!=mount->second.id)return 1015;refresh=std::move(opened.file);file=refresh.get();
    }
    if(!file)return 6;auto tree=registry_hive_read(*file);const auto before=registry;
    const auto name=registry.display_names.contains(root)?registry.display_names.at(root):root.substr(root.find_last_of(u'\\')+1);
    try{
        registry_hive_erase(registry,root);registry_hive_install(registry,root,name,std::move(tree));
        // 強制還原使既有子鍵 handle 失效，即使還原後再次出現同名子鍵也不能復活。
        if(flags==8)for(auto& [id,key]:registry.handles)if(id!=handle && key.path.starts_with(root+u"\\"))key.path=u"\uffff"+key.path;
        if(const auto error=commit_registry_transaction(transaction)){registry=before;return error;}
        poll_registry_notifications(false,root,15);return 0;
    }catch(...){registry=before;throw;}
}
std::uint32_t WindowsServices::replace_registry_hive(RegistryTransaction& transaction,const std::u16string& requested,const std::u16string& source,const std::u16string& backup){
    auto& registry=runtime_.registry;if(registry_file_.empty())return 50;if(source.empty() || backup.empty())return 87;
    std::u16string root=requested;
    while(!registry.hives.contains(root) && root.find(u'\\')!=root.npos)root.resize(root.find_last_of(u'\\'));
    if(root.size()==1 && root[0]==2){const auto slash=requested.find(u'\\',2);root=requested.substr(0,slash);}
    if(!registry.keys.contains(root))return 2;
    if(registry.replacements.contains(root))return 170;
    for(const auto& [mount,info]:registry.hives){(void)info;if(mount.starts_with(root+u"\\"))return 170;}
    refresh_file_security_context();auto input=filesystem_.open_host(utf16_to_utf8(source),0x80000000U,3,0x80,1);if(input.error)return input.error;
    const auto tree=registry_hive_read(*input.file);auto bytes=registry_hive::encode(tree,tree.minor>=5?5U:3U);
    // 先以副本核對完整範圍與容量；失敗不得建立備份或發布待替換資料。
    auto probe=registry;registry_hive_erase(probe,root);registry_hive_install(probe,root,u"ROOT",tree);
    auto output=filesystem_.open_host(utf16_to_utf8(backup),0x40000000U,1,0x80,0);if(output.error)return output.error==80?183U:output.error;
    if(const auto error=save_registry_hive(root,*output.file,3))return error;
    registry.replacements.emplace(root,std::move(bytes));
    if(const auto error=transaction.commit()){registry.replacements.erase(root);return error;}
    registry_replacement_files_[root]=std::move(input.file);return 0;
}
