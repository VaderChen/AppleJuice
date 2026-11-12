    if(symbol=="TryCreatePackageDependency" || symbol=="DeletePackageDependency" || symbol=="AddPackageDependency" || symbol=="RemovePackageDependency" || symbol=="GetResolvedPackageFullNameForPackageDependency" || symbol=="GetIdForPackageDependencyContext" || symbol=="GetPackageGraphRevisionId"){
        const auto returned=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto hr=[](std::uint32_t error){return error?0x80070000U|(error&0xffffU):0U;};const auto bits=static_cast<std::uint8_t>(x86_?32:64);
        // 普通 Win32 sandbox 沒有已部署的 MSIX framework；相依定義仍有真正的建立、持久化、生命期與刪除契約。
        // 不將「略過解析」誤當成已安裝套件，也不把未解析相依加入程序的空 package graph。
        if(symbol=="GetPackageGraphRevisionId" || symbol=="RemovePackageDependency")return returned(0);
        try{
            if(symbol=="GetIdForPackageDependencyContext"){
                if(!arg(1))return returned(0x80070057U);memory.store(GuestAddress{arg(1)},bits,0);return returned(0x80070057U);
            }
            initialize_tokens();const auto user_root=registry_current_user_path()+u"\\software\\applejuice\\packagedependencies\\";
            const auto machine_root=std::u16string(1,2)+u"\\software\\applejuice\\packagedependencies\\";
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_);if(transaction.error)return returned(hr(transaction.error));
            const auto number=[](const auto& values,const char16_t* key){const auto it=values.find(key);std::uint64_t n=0;if(it!=values.end())for(unsigned i=0;i<std::min<std::size_t>(8,it->second.bytes.size());++i)n|=std::uint64_t{it->second.bytes[i]}<<(8*i);return n;};
            const auto text=[](const auto& values,const char16_t* key){const auto it=values.find(key);std::u16string result;if(it!=values.end())for(std::size_t i=0;i+1<it->second.bytes.size();i+=2){const auto c=static_cast<char16_t>(it->second.bytes[i]|(unsigned{it->second.bytes[i+1]}<<8));if(!c)break;result+=c;}return result;};
            const auto registry_artifact=[&](std::u16string name){for(auto& c:name)c=unicode_case_unit(c,false);
                if(name.starts_with(u"hkey_current_user\\"))return registry_current_user_path()+name.substr(17);
                if(name.starts_with(u"hkcu\\"))return registry_current_user_path()+name.substr(4);
                if(name.starts_with(u"hkey_local_machine\\"))return std::u16string(1,2)+name.substr(18);
                if(name.starts_with(u"hklm\\"))return std::u16string(1,2)+name.substr(4);
                return std::u16string{};};
            if(symbol=="TryCreatePackageDependency"){
                if(!arg(7))return returned(0x80070057U);memory.store(GuestAddress{arg(7)},bits,0);
                const auto lifetime=dword(4),options=dword(6),architecture=dword(3);if(!arg(1) || lifetime>2 || (options&~3U) || (architecture&~0x3fU) || (lifetime==0?bool(arg(5)):!arg(5)) || ((options&2U) && arg(0)))return returned(0x80070057U);
                const auto family=read_utf16(memory,GuestAddress{arg(1)},255);const auto split=family.find_last_of(u'_');if(split==family.npos || !split || split+1==family.size() || family.find_first_of(u"\\/:*?\"<>| ")!=family.npos)return returned(0x80070057U);
                const auto& token=runtime_.token_objects.at(effective_token_id());if((options&2U) && token.integrity<0x3000U)return returned(0x80070005U);
                if(arg(0) && security::read_sid(memory,arg(0))!=token.user.sid)return returned(0x80070005U);
                const auto artifact=lifetime?read_utf16(memory,GuestAddress{arg(5)},32767):std::u16string{};std::u16string artifact_key;
                if(lifetime==1){const auto info=filesystem_.information(utf16_to_utf8(artifact));if(info.error)return returned(hr(info.error));}
                if(lifetime==2){artifact_key=registry_artifact(artifact);if(artifact_key.empty())return returned(0x80070057U);if(!runtime_.registry.keys.contains(artifact_key))return returned(0x80070002U);}
                if(!(options&1U))return returned(hr(15603)); // 解析真正的空部署清單；並非把沒有的 framework 偽造為可用。
                const auto identity=process_identity::query(process_id()).identity;if(!identity)return returned(0x80070006U);
                const auto id=utf8_to_utf16("aj-"+std::to_string(identity.pid)+"-"+std::to_string(identity.birth)+"-"+std::to_string(runtime_.allocate_wait_handle()));
                const auto bytes=utf16_bytes(id);const auto pointer=allocate_heap(memory,bytes.size());if(!pointer)return returned(0x8007000eU);
                const auto key=((options&2U)?machine_root:user_root)+id;auto& values=runtime_.registry.keys[key];
                const auto put=[&](const char16_t* key,std::uint64_t n){std::vector<std::uint8_t> data(8);for(unsigned i=0;i<8;++i)data[i]=static_cast<std::uint8_t>(n>>(8*i));values[key]=RegistryValue{11,std::move(data)};};
                values[u"family"]={1,utf16_bytes(family)};values[u"artifact"]={1,utf16_bytes(lifetime==2?artifact_key:artifact)};
                put(u"version",arg(2));put(u"architecture",architecture);put(u"lifetime",lifetime);put(u"pid",identity.pid);put(u"birth",identity.birth);
                const auto error=commit_registry_transaction(transaction);if(error){runtime_.registry.keys.erase(key);(void)free_heap(memory,pointer,state);return returned(hr(error));}
                memory.write(GuestAddress{pointer},bytes);memory.store(GuestAddress{arg(7)},bits,pointer);return returned(0);
            }
            if(!arg(0))return returned(0x80070057U);const auto id=read_utf16(memory,GuestAddress{arg(0)},255);if(id.find_first_of(u"\\/")!=id.npos)return returned(0x80070057U);
            if(symbol=="GetResolvedPackageFullNameForPackageDependency"){if(!arg(1))return returned(0x80070057U);memory.store(GuestAddress{arg(1)},bits,0);}
            if(symbol=="AddPackageDependency"){if(!arg(3) || (dword(2)&~1U))return returned(0x80070057U);memory.store(GuestAddress{arg(3)},bits,0);if(arg(4))memory.store(GuestAddress{arg(4)},bits,0);}
            auto entry=runtime_.registry.keys.find(user_root+id);if(entry==runtime_.registry.keys.end())entry=runtime_.registry.keys.find(machine_root+id);
            if(entry==runtime_.registry.keys.end())return returned(hr(1168));
            const auto& values=entry->second;const auto lifetime=number(values,u"lifetime");bool expired=false;
            if(lifetime==0){const ProcessIdentity identity{static_cast<std::uint32_t>(number(values,u"pid")),number(values,u"birth")};expired=process_identity::query(identity.pid).gone(identity);}
            else if(lifetime==1)expired=filesystem_.information(utf16_to_utf8(text(values,u"artifact"))).error!=0;
            else if(lifetime==2)expired=!runtime_.registry.keys.contains(text(values,u"artifact"));
            if(symbol=="DeletePackageDependency" || expired){
                if(!expired && entry->first.starts_with(machine_root) && runtime_.token_objects.at(effective_token_id()).integrity<0x3000U)return returned(0x80070005U);
                runtime_.registry.keys.erase(entry);const auto error=commit_registry_transaction(transaction);return returned(hr(error?error:expired?1168U:0U));
            }
            return returned(symbol=="AddPackageDependency"?hr(15603):0U);
        }catch(const MemoryFault&){return returned(0x800703e6U);}catch(const SecurityDescriptorError& error){return returned(hr(error.code));}catch(const std::bad_alloc&){return returned(0x8007000eU);}
    }
