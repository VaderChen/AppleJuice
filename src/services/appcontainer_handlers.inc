    if(symbol=="CreateAppContainerProfile" || symbol=="DeleteAppContainerProfile" ||
       symbol=="DeriveAppContainerSidFromAppContainerName" || symbol=="GetAppContainerFolderPath"){
        const bool creating=symbol=="CreateAppContainerProfile",deleting=symbol=="DeleteAppContainerProfile";
        const bool deriving=symbol=="DeriveAppContainerSidFromAppContainerName",folder=symbol=="GetAppContainerFolderPath";
        const auto output=deleting?0:arg(creating?5:1);std::uint64_t allocation{};
        const auto finish=[&](std::uint32_t code){
            if((code&0x80000000U) && allocation){runtime_.local_allocations.erase(allocation);free_heap(memory,allocation,state);}
            arg.return_to(state,code);return std::optional<std::uint32_t>{};
        };
        const auto winerror=[](std::uint32_t code){return code?0x80070000U|(code&0xffffU):0U;};
        try{
            if(!deleting){if(!output)return finish(0x80070057U);memory.store(GuestAddress{output},x86_?32:64,0);}
            if(!arg(0))return finish(0x80070057U);
            const auto name=read_utf16(memory,GuestAddress{arg(0)},32767);
            if((creating || deleting) && !appcontainer::valid_profile_name(name))return finish(0x80070057U);
            const auto sid=folder?sddl_sid(utf16_to_utf8(name)):appcontainer::derive(name);
            const auto sid_name=appcontainer::name_of_sid(sid);
            if(deriving){
                allocation=allocate_heap(memory,sid.size());if(!allocation)return finish(0x8007000eU);
                memory.write(GuestAddress{allocation},sid);runtime_.local_allocations.insert(allocation);
                memory.store(GuestAddress{output},x86_?32:64,allocation);return finish(0);
            }
            refresh_file_security_context();const auto token_id=effective_token_id();
            const auto user=token_user_sid_string(token_id),primary=token_user_sid_string(1);
            auto user_sid=utf8_to_utf16(user);user_sid[0]=u's';
            const auto hive=user==primary?std::u16string(1,1):std::u16string(1,3)+u"\\"+user_sid;
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,folder);
            if(transaction.error)return finish(winerror(transaction.error));
            auto& registry=runtime_.registry;
            auto sid_key=utf8_to_utf16(sid_name);sid_key[0]=u's';
            const auto key=hive+u"\\software\\classes\\local settings\\software\\microsoft\\windows\\currentversion\\appcontainer\\mappings\\"+user_sid+u"\\"+sid_key;
            const auto found=registry.keys.find(key);
            if(folder){
                if(found==registry.keys.end())return finish(winerror(2));
                (void)registry_security_access(key,1);
                const auto value=found->second.find(u"path");
                if(value==found->second.end() || value->second.type!=1 || value->second.bytes.size()<2 || value->second.bytes.size()%2 || value->second.bytes[value->second.bytes.size()-1] || value->second.bytes[value->second.bytes.size()-2])return finish(winerror(13));
                allocation=allocate_heap(memory,value->second.bytes.size());if(!allocation)return finish(0x8007000eU);
                memory.write(GuestAddress{allocation},value->second.bytes);
                memory.store(GuestAddress{output},x86_?32:64,allocation);return finish(0);
            }
            if(!filesystem_.writable())return finish(0x80070005U);
            const auto profile=profile_directory(registry,utf8_to_utf16(user),3,user==primary);
            if(profile.empty() && (!deleting || found==registry.keys.end()))return finish(deleting?0:winerror(2));
            // SID 作為實體目錄元件，避免合法 moniker 的尾端空白／句點造成名稱別名。
            auto package=utf16_to_utf8(profile)+"\\AppData\\Local\\Packages\\"+user+"\\"+sid_name;
            const auto path=package+"\\AC";
            if(deleting){
                if(found!=registry.keys.end()){
                    (void)registry_security_access(key,0x10001U);
                    const auto saved=found->second.find(u"path");
                    if(saved==found->second.end() || saved->second.type!=1 || saved->second.bytes.size()<2 || saved->second.bytes.size()%2)return finish(winerror(13));
                    std::u16string stored;
                    for(std::size_t at=0;at<saved->second.bytes.size();at+=2){
                        const auto c=static_cast<char16_t>(saved->second.bytes[at]|(unsigned{saved->second.bytes[at+1]}<<8));
                        if(!c){if(at+2!=saved->second.bytes.size())return finish(winerror(13));break;}
                        stored.push_back(c);
                    }
                    if(saved->second.bytes[saved->second.bytes.size()-2] || saved->second.bytes.back())return finish(winerror(13));
                    const auto previous=utf16_to_utf8(stored),suffix="\\AppData\\Local\\Packages\\"+user+"\\"+sid_name+"\\AC";
                    if(previous.size()<3 || previous[1]!=':' || previous[2]!='\\' || !previous.ends_with(suffix))return finish(winerror(13));
                    for(std::size_t start=3;start<previous.size();){
                        const auto stop=previous.find('\\',start);const auto part=previous.substr(start,stop==previous.npos?previous.size()-start:stop-start);
                        if(part.empty() || part=="." || part==".." || part.find_first_of("/:")!=part.npos)return finish(winerror(13));
                        if(stop==previous.npos)break;start=stop+1;
                    }
                    package=previous.substr(0,previous.size()-3);
                }
                // 不跟隨 reparse point；保留刪除失敗的登錄紀錄，允許呼叫者重試。
                const auto remove_tree=[&](const auto& self,const std::string& directory,unsigned depth)->std::uint32_t{
                    if(depth>128)return 206;
                    const auto info=filesystem_.information(directory);if(info.error==2 || info.error==3)return 0;if(info.error)return info.error;
                    const bool is_directory=(info.metadata.attributes&0x10U)!=0;
                    if(is_directory && !(info.metadata.attributes&0x400U)){
                        const auto entries=filesystem_.list(directory);if(entries.error)return entries.error;
                        for(const auto& entry:entries.entries){if(entry.name=="." || entry.name=="..")continue;
                            if(const auto error=self(self,directory+"\\"+entry.name,depth+1))return error;
                        }
                    }
                    return filesystem_.remove(directory,is_directory);
                };
                if(const auto error=remove_tree(remove_tree,package,0))return finish(winerror(error));
                for(auto it=registry.keys.begin();it!=registry.keys.end();){
                    if(it->first==key || it->first.starts_with(key+u"\\")){
                        for(const auto& [unused,value]:it->second){(void)unused;registry.bytes-=value.bytes.size();--registry.values;}
                        registry.security_descriptors.erase(it->first);registry.display_names.erase(it->first);registry.modified_times.erase(it->first);it=registry.keys.erase(it);
                    }else ++it;
                }
                return finish(winerror(transaction.commit()));
            }
            if(found!=registry.keys.end())return finish(winerror(183));
            if(!arg(1) || !arg(2))return finish(0x80070057U);
            const auto display=read_utf16(memory,GuestAddress{arg(1)},512),description=read_utf16(memory,GuestAddress{arg(2)},2048);
            const auto capability_count=static_cast<std::uint32_t>(arg(4));
            if(capability_count>1024 || (capability_count && !arg(3)))return finish(0x80070057U);
            if(capability_count)memory.check(GuestAddress{arg(3)},capability_count*(x86_?8U:16U),Access::Read);
            std::vector<std::uint8_t> capabilities;
            for(unsigned i=0;i<capability_count;++i){
                const auto item=arg(3)+i*(x86_?8U:16U);
                const auto capability=security::read_sid(memory,memory.load(GuestAddress{item},x86_?32:64));
                if(capability.size()<16 || capability[7]!=15 || capability[8]!=3 || capability[9] || capability[10] || capability[11] || std::any_of(capability.begin()+2,capability.begin()+7,[](auto c){return c!=0;}))return finish(0x80070057U);
                const auto at=capabilities.size();capabilities.resize(at+8);sddl_put(capabilities,at,static_cast<std::uint32_t>(capability.size()));
                sddl_put(capabilities,at+4,static_cast<std::uint32_t>(memory.load(GuestAddress{item+(x86_?4:8)},32)));
                capabilities.insert(capabilities.end(),capability.begin(),capability.end());
            }
            auto explicit_descriptor=security::parse_relative_descriptor(sddl_descriptor("O:"+user+"D:P(A;OICI;GA;;;"+user+")(A;OICI;GA;;;SY)(A;OICI;GA;;;"+sid_name+")S:P(ML;OICI;NW;;;LW)"));
            explicit_descriptor.data[1]=runtime_.token_objects.at(token_id).primary_group;
            const auto descriptor=security::relative_descriptor(explicit_descriptor);
            std::map<std::u16string,std::vector<std::uint8_t>> descriptors;
            for(std::size_t pos=2;pos<=key.size();){
                const auto end=key.find(u'\\',pos),stop=end==key.npos?key.size():end;const auto part=key.substr(0,stop);
                if(!registry.keys.contains(part)){
                    const auto parent=part.substr(0,part.find_last_of(u'\\'));
                    const auto parent_descriptor=descriptors.contains(parent)?descriptors.at(parent):registry_security_descriptor(parent);
                    (void)object_security_access(parent_descriptor,4U,{0x20019U,0x20006U,0x20019U,0xf003fU});
                    descriptors[part]=registry_creation_descriptor(parent_descriptor,part==key?explicit_descriptor:security::SecurityDescriptor{});
                }
                if(end==key.npos)break;pos=end+1;
            }
            if(registry.keys.size()+descriptors.size()>4096 || registry.values+5>4096)return finish(winerror(8));
            allocation=allocate_heap(memory,sid.size());if(!allocation)return finish(0x8007000eU);
            memory.write(GuestAddress{allocation},sid);runtime_.local_allocations.insert(allocation);
            auto saved=registry;
            std::vector<std::string> created;created.reserve(static_cast<std::size_t>(std::count(path.begin(),path.end(),'\\'))+2);
            auto temporary=path+"\\Temp";bool committed=false;
            // 在例外或錯誤回傳時一起回復登錄及本次新增的空目錄。
            const auto rollback=[&]() noexcept {
                if(committed)return;
                registry=std::move(saved);
                for(auto it=created.rbegin();it!=created.rend();++it){
                    try{(void)filesystem_.remove(*it,true);}catch(...){}
                }
            };
            struct Rollback {const decltype(rollback)& action;~Rollback(){action();}} cleanup{rollback};
            for(std::size_t pos=3;pos<=path.size();){
                const auto end=path.find('\\',pos),stop=end==path.npos?path.size():end;auto part=path.substr(0,stop);
                const auto info=filesystem_.information(part);
                if(!info.error){
                    if(!(info.metadata.attributes&0x10U) || (info.metadata.attributes&0x400U)){return finish(winerror(5));}
                    if(part==package){return finish(winerror(183));}
                }else if(info.error==2 || info.error==3){
                    const auto error=filesystem_.create_directory(part,part.size()>=package.size()?std::function<std::vector<std::uint8_t>()>{[&]{return descriptor;}}:std::function<std::vector<std::uint8_t>()>{});
                    if(error){return finish(winerror(error));}created.push_back(std::move(part));
                }else {return finish(winerror(info.error));}
                if(end==path.npos)break;pos=end+1;
            }
            if(const auto error=filesystem_.create_directory(temporary,[&]{return descriptor;})){return finish(winerror(error));}
            created.push_back(std::move(temporary));
            for(auto& [part,bytes]:descriptors){registry.keys.try_emplace(part);registry.security_descriptors[part]=std::move(bytes);registry.modified_times[part]=runtime_.system_filetime();}
            auto& values=registry.keys[key];
            const auto save=[&](std::u16string value_name,std::uint32_t type,std::vector<std::uint8_t> bytes){registry.bytes+=bytes.size();++registry.values;values.emplace(std::move(value_name),RegistryValue{type,std::move(bytes),{}});};
            save(u"moniker",1,utf16_bytes(name));save(u"displayname",1,utf16_bytes(display));save(u"description",1,utf16_bytes(description));
            save(u"path",1,utf16_bytes(utf8_to_utf16(path)));save(u"capabilities",3,std::move(capabilities));
            if(const auto error=transaction.commit())return finish(winerror(error));
            committed=true;
            memory.store(GuestAddress{output},x86_?32:64,allocation);return finish(0);
        }catch(const MemoryFault&){return finish(0x80070057U);}
         catch(const SecurityDescriptorError& error){return finish(winerror(error.code));}
         catch(const std::invalid_argument&){return finish(0x80070057U);}
         catch(const std::length_error&){return finish(0x80070057U);}
         catch(const std::bad_alloc&){return finish(0x8007000eU);}
    }
