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
#include "windows_registry_hive_state.inc"
#include "windows_registry_hive_native.inc"
std::uint32_t WindowsServices::flush_registry_hives(bool explicit_flush){
    try{
        // 掛載時已取得來源的讀寫權限；後續 registry 寫回屬服務工作，不能被目前
        // 客體程序的完整性等級限制。複製檔案 provider，保留 SID、ACL、映射及分享
        // 檢查，只將私有服務脈絡設為 System IL，完全不更改客體 token／provider。
        initialize_tokens();const auto& primary=runtime_.token_objects.at(1);auto service_token=primary;service_token.integrity=0x4000U;
        auto backing=filesystem_;backing.set_security_context(service_token,primary.user.sid,primary.primary_group);
        for(const auto& [root,mount]:runtime_.registry.hives){
            if(!explicit_flush && (mount.flags&4U))continue;
            const auto path=utf16_to_utf8(mount.source);const auto current=backing.information(path);if(current.error)return current.error;
            if(current.metadata.device!=mount.device || current.metadata.id!=mount.id)return 1015;
            auto& file=registry_hive_files_[root];
            if(!file){auto opened=backing.open_host(path,0xc0000000U,3,0x80,7);if(opened.error)return opened.error;file=std::move(opened.file);}
            const auto info=file->information();if(info.error)return info.error;if(info.metadata.device!=mount.device || info.metadata.id!=mount.id)return 1015;
            const auto bytes=registry_hive::encode(registry_hive_tree(runtime_.registry,root),mount.minor);if(const auto error=registry_hive_write(*file,bytes))return error;
        }
        return 0;
    }catch(const registry_hive::Error& error){return error.code;}catch(const std::bad_alloc&){return 8;}
}
std::uint32_t WindowsServices::commit_registry_transaction(RegistryTransaction& transaction){
    if(const auto error=flush_registry_hives(false))return error;return transaction.commit();
}
std::optional<std::uint32_t> WindowsServices::invoke_registry_hive(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto saved_error=last_error_;const bool native=service.symbol.starts_with("Nt");
    const auto finish=[&](std::uint32_t code){last_error_=saved_error;poll_registry_notifications(false,{},0,&memory);arg.return_to(state,native?registry_native_status(code):code);return std::optional<std::uint32_t>{};};
    const bool save=service.symbol.starts_with("RegSaveKey") || service.symbol=="NtSaveKey",connect=service.symbol.starts_with("RegConnectRegistry"),unload=service.symbol.starts_with("RegUnLoadKey") || service.symbol=="NtUnloadKey",wide=service.symbol.ends_with('W');
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
        const auto& token=runtime_.token_objects.at(effective_token_id());
        const bool restore=service.symbol=="NtRestoreKey" || service.symbol.starts_with("RegRestoreKey");
        const bool replace=service.symbol=="NtReplaceKey" || service.symbol.starts_with("RegReplaceKey");
        if(!token.has_privilege(save?17U:18U,true) || (!native && !save && !unload && !token.has_privilege(17,true)) || (replace && !token.has_privilege(17,true)))return finish(1314);
        if(!runtime_.network.external_io || !filesystem_.writable())return finish(50);
        if(native){
            if(save || restore || replace){
                const auto key=arg(replace?1:0);if(!registry.handles.contains(key))return finish(6);
                const auto [root,access]=registry_security_handle(key,0);(void)access;
                if(replace)return finish(replace_registry_hive(transaction,root,registry_object_path(memory,arg(0),true),registry_object_path(memory,arg(2),true)));
                const auto flags=restore?static_cast<std::uint32_t>(arg(2)):0;
                const auto file=files_.find(arg(1));
                if(!(restore && flags==2) && (file==files_.end() || !file->second.native))return finish(6);
                if(file!=files_.end() && !(file->second.access&(save?0x40000002U:0x80000001U)))return finish(5);
                return finish(save?save_registry_hive(root,*file->second.native,3):restore_registry_hive(transaction,root,file==files_.end()?nullptr:file->second.native.get(),flags,key));
            }
            const auto named=registry_object_path(memory,arg(0),false),target=registry_hive::fold(named);
            if(unload)return finish(unload_registry_hive(transaction,target));
            const auto slash=named.find_last_of(u'\\');const auto flags=service.symbol=="NtLoadKey2"?static_cast<std::uint32_t>(arg(2)):0;
            return finish(load_registry_hive(transaction,target,named.substr(slash+1),registry_object_path(memory,arg(1),true),flags));
        }
        const auto [root,access]=registry_security_handle(arg(0),0);(void)access;
        if(save){
            const auto name=string(arg(1));if(name.empty())return finish(87);
            const auto format=service.symbol.starts_with("RegSaveKeyEx")?static_cast<std::uint32_t>(arg(3)):1U;if(format!=1 && format!=2 && format!=4)return finish(87);
            const auto source=static_cast<std::uint32_t>(arg(0));if((arg(0)==source || arg(0)==(UINT64_C(0xffffffff00000000)|source)) && source>=0x80000000U && source<=0x80000006U && source!=0x80000000U && source!=0x80000001U)return finish(87);
            const auto attributes=read_security_attributes(memory,arg(2),x86_,false);(void)attributes;
            // 編碼驗證先於建立目的檔，沿用 CREATE_NEW 與 SECURITY_ATTRIBUTES 契約。
            for(const auto& [path,values]:registry.keys){(void)values;if(path==root || path.starts_with(root+u"\\"))(void)registry_security_descriptor(path);}
            const auto bytes=registry_hive::encode(registry_hive_tree(registry,root),format==2?5U:3U);
            refresh_file_security_context();auto opened=filesystem_.open_host(utf16_to_utf8(name),0x40000000U,1,0x20,0,[&]{return file_creation_descriptor(memory,arg(2));});
            if(opened.error)return finish(opened.error==80?183U:opened.error);return finish(registry_hive_write(*opened.file,bytes));
        }
        if(restore){
            const auto flags=static_cast<std::uint32_t>(arg(2));if(flags==2)return finish(restore_registry_hive(transaction,root,nullptr,flags,arg(0)));
            const auto name=string(arg(1));if(name.empty())return finish(87);refresh_file_security_context();auto opened=filesystem_.open_host(utf16_to_utf8(name),0x80000000U,3,0x80,7);
            if(opened.error)return finish(opened.error);return finish(restore_registry_hive(transaction,root,opened.file.get(),flags,arg(0)));
        }
        if(replace){
            auto target=root;const auto subkey=registry_hive::fold(string(arg(1)));if(!subkey.empty())target+=u"\\"+subkey;
            return finish(replace_registry_hive(transaction,target,string(arg(2)),string(arg(3))));
        }
        if(root.size()!=1 || (root[0]!=2 && root[0]!=3))return finish(87);
        const auto original_name=string(arg(1)),name=registry_hive::fold(original_name);if(name.empty() || name.front()==u'\\' || name.back()==u'\\' || name.find(u'\0')!=name.npos)return finish(87);
        for(std::size_t start=0;start<name.size();){const auto end=name.find(u'\\',start),stop=end==name.npos?name.size():end;if(stop==start || stop-start>255)return finish(87);start=stop+1;}
        const auto target=root+u"\\"+name;
        if(unload)return finish(unload_registry_hive(transaction,target));
        return finish(load_registry_hive(transaction,target,original_name.substr(original_name.find_last_of(u'\\')+1),string(arg(2)),0));
    }catch(const registry_hive::Error& error){return finish(error.code);}catch(const SecurityDescriptorError& error){return finish(error.code);}catch(const MemoryFault&){return finish(native?998:87);}catch(const std::bad_alloc&){return finish(8);}catch(const std::invalid_argument&){return finish(123);}
}
