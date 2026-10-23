namespace {
registry_hive::Tree registry_hive_tree(const RegistryState& state,const std::u16string& root){
    registry_hive::Tree tree;const auto prefix=root+u"\\";
    for(const auto& [path,values]:state.keys){
        if(path!=root && !path.starts_with(prefix))continue;
        bool volatile_key=false;for(const auto& item:state.volatile_keys)if(path==item || path.starts_with(item+u"\\")){volatile_key=true;break;}
        if(volatile_key)continue;
        registry_hive::Key key;const auto display=state.display_names.find(path);key.name=display==state.display_names.end()?(path.size()==1?u"ROOT":path.substr(path.find_last_of(u'\\')+1)):display->second;
        key.values=values;if(const auto item=state.modified_times.find(path);item!=state.modified_times.end())key.modified=item->second;
        if(const auto item=state.classes.find(path);item!=state.classes.end())key.class_name=item->second;
        if(const auto item=state.security_descriptors.find(path);item!=state.security_descriptors.end())key.security=item->second;
        tree.keys.emplace(path==root?std::u16string{}:path.substr(prefix.size()),std::move(key));
    }
    if(!tree.keys.contains(u""))throw registry_hive::Error{1021};return tree;
}
std::uint32_t registry_hive_write(HostFile& file,std::span<const std::uint8_t> bytes){
    // HostFile 的開啟／鎖定狀態仍由協調者序列化；只將私有 POSIX 登錄存放區 I/O 交給 worker。
    std::size_t offset=0;while(offset<bytes.size()){const auto written=file.write(bytes.subspan(offset),offset);if(written.error)return written.error;if(!written.bytes || written.bytes>bytes.size()-offset)return 29;offset+=static_cast<std::size_t>(written.bytes);}
    const auto result=file.truncate(bytes.size());return result?result:file.flush();
}
}
std::uint32_t WindowsServices::flush_registry_hives(){
    try{
        for(const auto& [root,mount]:runtime_.registry.hives){
            const auto path=utf16_to_utf8(mount.source);const auto current=filesystem_.information(path);if(current.error)return current.error;
            if(current.metadata.device!=mount.device || current.metadata.id!=mount.id)return 1015;
            auto& file=registry_hive_files_[root];
            if(!file){refresh_file_security_context();auto opened=filesystem_.open_host(path,0xc0000000U,3,0x80,7);if(opened.error)return opened.error;file=std::move(opened.file);}
            const auto info=file->information();if(info.error)return info.error;if(info.metadata.device!=mount.device || info.metadata.id!=mount.id)return 1015;
            const auto bytes=registry_hive::encode(registry_hive_tree(runtime_.registry,root),mount.minor);if(const auto error=registry_hive_write(*file,bytes))return error;
        }
        return 0;
    }catch(const registry_hive::Error& error){return error.code;}catch(const std::bad_alloc&){return 8;}
}
std::uint32_t WindowsServices::commit_registry_transaction(RegistryTransaction& transaction){
    if(const auto error=flush_registry_hives())return error;return transaction.commit();
}
std::optional<std::uint32_t> WindowsServices::invoke_registry_hive(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto saved_error=last_error_;
    const auto finish=[&](std::uint32_t code){last_error_=saved_error;arg.return_to(state,code);return std::optional<std::uint32_t>{};};
    const bool save=service.symbol.starts_with("RegSaveKey"),connect=service.symbol.starts_with("RegConnectRegistry"),unload=service.symbol.starts_with("RegUnLoadKey"),wide=service.symbol.ends_with('W');
    try{
        if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
        auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_,save || connect);
        if(transaction.error)return finish(transaction.error);initialize_registry_views(registry);initialize_tokens();
        const auto string=[&](std::uint64_t pointer){return !pointer?std::u16string{}:wide?read_utf16(memory,GuestAddress{pointer},32768):utf8_to_utf16(read_ansi(memory,pointer,32768));};
        if(connect){
            if(!arg(2))return finish(87);memory.check(GuestAddress{arg(2)},x86_?4U:8U,Access::Write);
            const auto root=static_cast<std::uint32_t>(arg(1));const bool encoding=arg(1)==root || arg(1)==(UINT64_C(0xffffffff00000000)|root);
            if(!encoding || (root!=0x80000002U && root!=0x80000003U && root!=0x80000004U))return finish(6);
            auto machine=registry_hive::fold(string(arg(0)));if(machine.starts_with(u"\\\\"))machine.erase(0,2);
            if(!machine.empty() && machine!=u"." && machine!=u"localhost" && machine!=u"x64arm")return finish(1722);
            if(root==0x80000004U)return finish(50); // PERF_DATA 需要共用即時 counter provider，不建造空 hive。
            if(registry.handles.size()>=4096)return finish(8);const std::u16string path(1,static_cast<char16_t>(root&255));registry.keys.try_emplace(path);
            const auto granted=registry_security_access(path,0x02000000U);const auto handle=registry.next_handle++;
            registry.handles.emplace(handle,RegistryHandle{path,granted,registry_view(0,x86_),path});memory.store(GuestAddress{arg(2)},x86_?32:64,handle);return finish(0);
        }
        const auto [root,access]=registry_security_handle(arg(0),0);(void)access;
        const auto& token=runtime_.token_objects.at(effective_token_id());
        if(!token.has_privilege(unload?18U:17U,true) || (!save && !unload && !token.has_privilege(18,true)))return finish(1314);
        if(!runtime_.network.external_io || !filesystem_.writable())return finish(50);
        if(save){
            const auto name=string(arg(1));if(name.empty())return finish(87);
            const auto format=service.symbol.starts_with("RegSaveKeyEx")?static_cast<std::uint32_t>(arg(3)):1U;if(format!=1 && format!=2 && format!=4)return finish(87);
            const auto source=static_cast<std::uint32_t>(arg(0));if((arg(0)==source || arg(0)==(UINT64_C(0xffffffff00000000)|source)) && source>=0x80000000U && source<=0x80000006U && source!=0x80000000U && source!=0x80000001U)return finish(87);
            const auto attributes=read_security_attributes(memory,arg(2),x86_,false);(void)attributes;
            for(const auto& [path,values]:registry.keys){(void)values;if(path==root || path.starts_with(root+u"\\"))(void)registry_security_descriptor(path);}
            const auto bytes=registry_hive::encode(registry_hive_tree(registry,root),format==2?5U:3U);
            refresh_file_security_context();auto opened=filesystem_.open_host(utf16_to_utf8(name),0x40000000U,1,0x20,0,[&]{return file_creation_descriptor(memory,arg(2));});
            if(opened.error)return finish(opened.error==80?183U:opened.error);return finish(registry_hive_write(*opened.file,bytes));
        }
        if(root.size()!=1 || (root[0]!=2 && root[0]!=3))return finish(87);
        const auto original_name=string(arg(1)),name=registry_hive::fold(original_name);if(name.empty() || name.find_first_of(std::u16string{u'\\',u'\0'})!=name.npos || name.size()>255)return finish(87);
        const auto target=root+u"\\"+name,prefix=target+u"\\";
        if(unload){
            if(!registry.hives.contains(target))return finish(2);
            for(const auto& [handle,item]:registry.handles){(void)handle;if(item.path==target || item.path.starts_with(prefix))return finish(170);}
            if(const auto error=flush_registry_hives())return finish(error);
            const auto before=registry;for(auto item=registry.keys.begin();item!=registry.keys.end();){
                if(item->first!=target && !item->first.starts_with(prefix)){++item;continue;}
                for(const auto& [key,value]:item->second){(void)key;registry.bytes-=value.bytes.size();--registry.values;}
                registry.display_names.erase(item->first);registry.modified_times.erase(item->first);registry.security_descriptors.erase(item->first);registry.classes.erase(item->first);registry.volatile_keys.erase(item->first);item=registry.keys.erase(item);
            }
            registry.hives.erase(target);const auto error=transaction.commit();if(error)registry=before;else {registry_hive_files_.erase(target);poll_registry_notifications(false,root,1);}
            return finish(error);
        }
        if(registry.keys.contains(target))return finish(183);if(registry.hives.size()>=128)return finish(8);
        const auto source_name=string(arg(2));if(source_name.empty())return finish(87);refresh_file_security_context();auto opened=filesystem_.open_host(utf16_to_utf8(source_name),0xc0000000U,4,0x80,7);if(opened.error)return finish(opened.error);
        registry_hive::Tree tree;if(!opened.existed){registry_hive::Key initial;initial.name=original_name;initial.modified=runtime_.system_filetime();initial.security=registry_creation_descriptor(registry_security_descriptor(root),{});tree.keys.emplace(u"",std::move(initial));
            if(const auto error=registry_hive_write(*opened.file,registry_hive::encode(tree)))return finish(error);
        }else{
            if(opened.metadata.size>32U*1024U*1024U)return finish(8);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(opened.metadata.size));std::size_t offset=0;
            while(offset<bytes.size()){const auto read=opened.file->read(std::span(bytes).subspan(offset),offset);if(read.error)return finish(read.error);if(!read.bytes || read.bytes>bytes.size()-offset)return finish(1015);offset+=static_cast<std::size_t>(read.bytes);}
            tree=registry_hive::decode(bytes);
        }
        std::uint64_t count=0,size=0;for(const auto& [path,key]:tree.keys){(void)path;count+=key.values.size();for(const auto& [value_name,value]:key.values){(void)value_name;size+=value.bytes.size();}}
        if(tree.keys.size()>4096-registry.keys.size() || count>4096-registry.values || size>16U*1024U*1024U-registry.bytes)return finish(8);
        const auto info=opened.file->information();if(info.error)return finish(info.error);const auto location=filesystem_.final_path(*opened.file,utf16_to_utf8(source_name));if(location.error)return finish(location.error);const auto before=registry;
        try{
            for(auto& [path,key]:tree.keys){const auto full=path.empty()?target:prefix+path;registry.keys.emplace(full,std::move(key.values));registry.display_names[full]=path.empty()?original_name:key.name;registry.modified_times[full]=key.modified;
                if(!key.security.empty())registry.security_descriptors[full]=std::move(key.security);if(!key.class_name.empty())registry.classes[full]=std::move(key.class_name);}
            registry.bytes+=size;registry.values+=count;registry.hives[target]={utf8_to_utf16(location.key),info.metadata.device,info.metadata.id,tree.minor>=5?5U:3U};registry_hive_files_[target]=std::move(opened.file);
            const auto error=transaction.commit();if(error){registry=before;registry_hive_files_.erase(target);return finish(error);}poll_registry_notifications(false,root,1);return finish(0);
        }catch(...){registry=before;registry_hive_files_.erase(target);throw;}
    }catch(const registry_hive::Error& error){return finish(error.code);}catch(const SecurityDescriptorError& error){return finish(error.code);}catch(const MemoryFault&){return finish(87);}catch(const std::bad_alloc&){return finish(8);}catch(const std::invalid_argument&){return finish(123);}
}
