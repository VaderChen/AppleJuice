// WinINet 快取群組、容器、設定、空間回收與標頭計數：資料存於 url_cache::Metadata（與項目索引同一個登錄交易）。
if(const bool groups=symbol=="CreateUrlCacheGroup" || symbol=="DeleteUrlCacheGroup" || symbol.starts_with("SetUrlCacheEntryGroup") || symbol.starts_with("GetUrlCacheGroupAttribute") ||
       symbol.starts_with("SetUrlCacheGroupAttribute") || symbol=="FindFirstUrlCacheGroup" || symbol=="FindNextUrlCacheGroup" || symbol.starts_with("IsUrlCacheEntryExpired") ||
       symbol.starts_with("GetUrlCacheConfigInfo") || symbol.starts_with("SetUrlCacheConfigInfo") || symbol.starts_with("FreeUrlCacheSpace") || symbol.starts_with("CreateUrlCacheContainer") ||
       symbol.starts_with("DeleteUrlCacheContainer") || symbol.starts_with("FindFirstUrlCacheContainer") || symbol.starts_with("FindNextUrlCacheContainer") || symbol=="IncrementUrlCacheHeaderData";groups){
    Transaction transaction(runtime_.registry,registry_file_);auto& registry=runtime_.registry;auto metadata=url_cache::load_metadata(registry);
    const auto find_group=[&](std::uint64_t id)->url_cache::Group&{for(auto& group:metadata.groups)if(group.id==id)return group;throw CacheError{2};};
    const auto all_entries=[&]{std::vector<Entry> entries;const auto index=registry.keys.find(url_cache::index);if(index!=registry.keys.end())for(const auto& [name,value]:index->second){(void)name;auto entry=url_cache::decode(value.bytes);if(entry.type)entries.push_back(std::move(entry));}return entries;};
    const auto encoded=[&](std::u16string_view value,std::size_t limit){std::vector<std::uint8_t> bytes;if(wide){bytes=utf16_bytes(value);}else {const auto ansi=utf16_to_ansi(std::u16string(value));bytes.assign(ansi.begin(),ansi.end());bytes.push_back(0);}if(limit && bytes.size()>limit*width)throw CacheError{87};return bytes;};
    // 內建容器：Content、Cookies、History，前綴與 FindFirstUrlCacheEntry 的搜尋樣式一致。
    std::vector<url_cache::Container> containers{{u"Content",u"",directory,metadata.quota,0,0},{u"Cookies",u"Cookie:",directory,metadata.quota,0,0},{u"History",u"Visited:",directory,metadata.quota,0,0}};
    for(const auto& item:metadata.containers)containers.push_back(item);
    if(symbol=="CreateUrlCacheGroup"){
        // (dwFlags, lpReserved)：CACHEGROUP_FLAG_GIDONLY 只配置編號不建立群組；GROUPID 為 64 位元回傳值。
        if(arg(1) || (d(0)&~7U))throw CacheError{87};const auto id=metadata.next_group++;
        if(!(d(0)&4U)){if(metadata.groups.size()>=4096)throw CacheError{8};url_cache::Group group;group.id=id;group.flags=d(0)&3U;metadata.groups.push_back(group);}
        url_cache::store_metadata(registry,metadata);transaction.commit();last_error_=0;arg.return_to(state,id,true);return std::nullopt;
    }
    if(symbol=="DeleteUrlCacheGroup"){
        // (GroupId, dwFlags, lpReserved)：CACHEGROUP_FLAG_FLUSHURL_ONDELETE 同時刪除群組成員項目。
        if(arg(2) || (d(1)&~3U))throw CacheError{87};auto& group=find_group(arg(0));const auto urls=group.urls;const bool flush=(d(1)&2U) || (group.flags&2U);
        std::erase_if(metadata.groups,[&](const auto& item){return item.id==arg(0);});
        if(flush)for(const auto& url:urls){
            auto entry=url_cache::get(registry,url);if(!entry)continue;prune(*entry);
            if(!entry->leases.empty()){entry->deleting=true;url_cache::put(registry,*entry);continue;}
            if(!entry->file.empty() && own_file(entry->file))(void)filesystem_.remove(utf16_to_utf8(entry->file),false);url_cache::erase(registry,url);
        }
        url_cache::store_metadata(registry,metadata);transaction.commit();result=1;
    }else if(symbol.starts_with("SetUrlCacheEntryGroup")){
        // (lpszUrlName, dwFlags, GroupId, pbGroupAttributes, cbGroupAttributes, lpReserved)：INTERNET_CACHE_GROUP_ADD（0）／REMOVE（1）。
        const auto url=text(arg(0));if(url.empty() || d(1)>1 || arg(3) || d(4) || arg(5))throw CacheError{87};
        if(!url_cache::get(registry,url))throw CacheError{2};auto& group=find_group(arg(2));const auto key=url_cache::key(url);
        const auto member=std::find_if(group.urls.begin(),group.urls.end(),[&](const auto& item){return url_cache::key(item)==key;});
        if(d(1)==0){if(member==group.urls.end()){if(group.urls.size()>=65536)throw CacheError{8};group.urls.push_back(url);}}
        else {if(member==group.urls.end())throw CacheError{2};group.urls.erase(member);}
        url_cache::store_metadata(registry,metadata);transaction.commit();result=1;
    }else if(symbol.starts_with("GetUrlCacheGroupAttribute") || symbol.starts_with("SetUrlCacheGroupAttribute")){
        // INTERNET_CACHE_GROUP_INFO：大小、旗標、類型、使用量（KB）、配額（KB）、四個擁有者 DWORD 與 120 字元名稱。
        const bool get=symbol.starts_with("Get");const unsigned size=36+120*width;auto& group=find_group(arg(0));const auto attributes=d(2);
        if(d(1) || arg(get?5:4))throw CacheError{87};
        if(get){
            if(!arg(3) || !arg(4))throw CacheError{87};const auto capacity=memory.load(GuestAddress{arg(4)},32);memory.check(GuestAddress{arg(4)},4,Access::Write);memory.store(GuestAddress{arg(4)},32,size);
            if(capacity<size)throw CacheError{122};std::uint64_t usage=0;for(const auto& url:group.urls)if(const auto entry=url_cache::get(registry,url))usage+=entry->size;
            std::vector<std::uint8_t> info(size);const auto put=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)info[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
            put(0,size);if(attributes&3U)put(4,group.flags);if(attributes&5U)put(8,group.type);if(attributes&9U){put(12,static_cast<std::uint32_t>((usage+1023)/1024));put(16,group.quota);}
            if(attributes&0x20U)for(unsigned i=0;i<4;++i)put(20+i*4,group.owner[i]);
            if(attributes&0x10U){auto name=encoded(group.name,0);name.resize(std::min<std::size_t>(name.size(),120*width));std::copy(name.begin(),name.end(),info.begin()+36);}
            memory.write(GuestAddress{arg(3)},info);result=1;
        }else{
            if(!arg(3))throw CacheError{87};if(memory.load(GuestAddress{arg(3)},32)!=size)throw CacheError{87};
            if(attributes&2U)group.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+4},32))&3U;
            if(attributes&4U)group.type=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+8},32));
            if(attributes&8U)group.quota=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+16},32));
            if(attributes&0x20U)for(unsigned i=0;i<4;++i)group.owner[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+20+i*4},32));
            if(attributes&0x10U){
                std::u16string name;
                if(wide){for(unsigned i=0;i<120;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{arg(3)+36+i*2},16));if(!c)break;name.push_back(c);}}
                else {std::string ansi;for(unsigned i=0;i<120;++i){const auto c=static_cast<char>(memory.load(GuestAddress{arg(3)+36+i},8));if(!c)break;ansi.push_back(c);}name=utf8_to_utf16(ansi);}
                group.name=std::move(name);
            }
            url_cache::store_metadata(registry,metadata);transaction.commit();result=1;
        }
    }else if(symbol=="FindFirstUrlCacheGroup" || symbol=="FindNextUrlCacheGroup"){
        // FindFirst：(dwFlags, dwFilter, lpSearchCondition, dwSearchCondition, lpGroupId, lpReserved)；FindNext：(hFind, lpGroupId, lpReserved)。
        const bool first=symbol=="FindFirstUrlCacheGroup";std::uint64_t handle=arg(0);UrlCacheState::GroupEnumeration* current=nullptr;UrlCacheState::GroupEnumeration enumeration;
        if(first){if(d(0) || arg(2) || d(3) || arg(5))throw CacheError{87};for(const auto& group:metadata.groups)enumeration.ids.push_back(group.id);current=&enumeration;}
        else {if(arg(2))throw CacheError{87};const auto found=local.group_enumerations.find(handle);if(found==local.group_enumerations.end())throw CacheError{6};current=&found->second;}
        const auto out=arg(first?4:1);if(!out)throw CacheError{87};if(current->cursor>=current->ids.size())throw CacheError{259};
        memory.check(GuestAddress{out},8,Access::Write);memory.store(GuestAddress{out},64,current->ids[current->cursor++]);
        if(first){if(local.group_enumerations.size()>=4096)throw CacheError{8};handle=local.next++;local.group_enumerations.emplace(handle,std::move(enumeration));result=handle;}else result=1;
    }else if(symbol.starts_with("IsUrlCacheEntryExpired")){
        // (lpszUrlName, dwFlags, pftLastModified)：沒有到期時間或已過期都視為過期；找不到項目時回 TRUE 並設 ERROR_FILE_NOT_FOUND。
        const auto url=text(arg(0));if(url.empty() || !arg(2))throw CacheError{87};memory.check(GuestAddress{arg(2)},8,Access::Write);memory.store(GuestAddress{arg(2)},64,0);
        const auto entry=url_cache::get(registry,url,true);if(!entry){last_error_=2;arg.return_to(state,1);return std::nullopt;}
        memory.store(GuestAddress{arg(2)},64,entry->modified);result=!entry->expires || entry->expires<=url_cache::now()?1:0;last_error_=0;arg.return_to(state,result);return std::nullopt;
    }else if(symbol.starts_with("GetUrlCacheConfigInfo") || symbol.starts_with("SetUrlCacheConfigInfo")){
        // INTERNET_CACHE_CONFIG_INFO：dwStructSize、dwContainer、dwQuota、dwReserved4、fPerUser、dwSyncMode、dwNumCachePaths、CachePath[MAX_PATH]＋dwCacheSize、dwNormalUsage、dwExemptUsage。
        const bool get=symbol.starts_with("Get");const unsigned path_bytes=260*width,size=28+path_bytes+12;if(!arg(0))throw CacheError{87};
        if(get){
            if(arg(1)){const auto capacity=memory.load(GuestAddress{arg(1)},32);memory.check(GuestAddress{arg(1)},4,Access::Write);memory.store(GuestAddress{arg(1)},32,size);if(capacity<size)throw CacheError{122};}
            std::uint64_t normal=0,exempt=0;for(const auto& entry:all_entries())((entry.exemption || (entry.type&4U))?exempt:normal)+=entry.size;
            std::vector<std::uint8_t> info(size);const auto put=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)info[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
            put(0,size);put(8,metadata.quota);put(16,1);put(20,metadata.sync_mode);put(24,1);auto path=encoded(std::u16string(directory)+u"\\",260);std::copy(path.begin(),path.end(),info.begin()+28);
            put(28+path_bytes,metadata.quota);put(32+path_bytes,static_cast<std::uint32_t>((normal+1023)/1024));put(36+path_bytes,static_cast<std::uint32_t>((exempt+1023)/1024));
            memory.write(GuestAddress{arg(0)},info);result=1;
        }else{
            // 可調整同步模式（CACHE_CONFIG_SYNC_MODE_FC）、配額（QUOTA_FC）與快取大小（DISK_CACHE_PATHS_FC）；不支援移動快取目錄。
            const auto fields=d(1);if(fields&~0x08e0U)throw CacheError{87};
            if(fields&0x80U){const auto mode=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+20},32));if(mode>4)throw CacheError{87};metadata.sync_mode=mode;}
            if(fields&0x800U)metadata.quota=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+8},32));
            if(fields&0x40U)metadata.quota=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+28+path_bytes},32));
            url_cache::store_metadata(registry,metadata);transaction.commit();result=1;
        }
    }else if(symbol.starts_with("FreeUrlCacheSpace")){
        // (lpszCachePath, dwSize 百分比, dwFilter)：自最久未存取的項目開始刪除，略過使用中的項目；STICKY_CACHE_ENTRY 只在 dwFilter 包含時才回收。
        const auto path=text(arg(0));const auto percent=d(1);if(percent<1 || percent>100)throw CacheError{87};
        if(!path.empty()){auto a=path,b=std::u16string(directory);for(auto* item:{&a,&b})for(auto& c:*item){if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);if(c==u'/')c=u'\\';}while(a.size()>b.size() && a.back()==u'\\')a.pop_back();if(a!=b)throw CacheError{2};}
        auto entries=all_entries();std::uint64_t total=0;for(const auto& entry:entries)total+=entry.size;const auto target=total*percent/100;std::uint64_t freed=0;
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.accessed<b.accessed;});
        for(auto& entry:entries){
            if(freed>=target && percent<100)break;prune(entry);if(!entry.leases.empty() || entry.deleting)continue;if((entry.exemption || (entry.type&4U)) && !(d(2)&4U))continue;
            if(!entry.file.empty() && own_file(entry.file))(void)filesystem_.remove(utf16_to_utf8(entry.file),false);url_cache::erase(registry,entry.url);freed+=entry.size;
            for(auto& group:metadata.groups)std::erase_if(group.urls,[&](const auto& url){return url_cache::key(url)==url_cache::key(entry.url);});
        }
        url_cache::store_metadata(registry,metadata);transaction.commit();result=1;
    }else if(symbol.starts_with("CreateUrlCacheContainer") || symbol.starts_with("DeleteUrlCacheContainer")){
        // Create：(Name, lpCachePrefix, lpszCachePath, KBCacheLimit, dwContainerType, dwOptions, pvBuffer, cbBuffer)；Delete：(Name, dwOptions)。內建容器不可刪除。
        const auto name=text(arg(0));if(name.empty() || name.size()>64)throw CacheError{87};const auto same=[&](const std::u16string& a){auto x=a,y=name;for(auto* item:{&x,&y})for(auto& c:*item)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return x==y;};
        if(symbol.starts_with("Create")){
            if(arg(6) || d(7))throw CacheError{87};const auto prefix=text(arg(1));if(prefix.empty() || prefix.size()>64)throw CacheError{87};
            if(std::any_of(containers.begin(),containers.end(),[&](const auto& item){return same(item.name);}))throw CacheError{183};if(metadata.containers.size()>=64)throw CacheError{8};
            metadata.containers.push_back({name,prefix,arg(2)?text(arg(2)):std::u16string(directory),d(3),d(4),d(5)});
        }else{
            if(std::any_of(containers.begin(),containers.begin()+3,[&](const auto& item){return same(item.name);}))throw CacheError{5};
            const auto before=metadata.containers.size();std::erase_if(metadata.containers,[&](const auto& item){return same(item.name);});if(metadata.containers.size()==before)throw CacheError{2};
        }
        url_cache::store_metadata(registry,metadata);transaction.commit();result=1;
    }else if(symbol.starts_with("FindFirstUrlCacheContainer") || symbol.starts_with("FindNextUrlCacheContainer")){
        // FindFirst：(pdwModified, lpContainerInfo, lpcbContainerInfo, dwOptions)；FindNext：(hEnumHandle, lpContainerInfo, lpcbContainerInfo)。
        // INTERNET_CACHE_CONTAINER_INFO：dwCacheVersion 與 Name、CachePrefix、VolumeLabel、VolumeTitle 字串指標，字串接在結構之後。
        const bool first=symbol.starts_with("FindFirst");std::uint64_t handle=arg(0);UrlCacheState::ContainerEnumeration enumeration;UrlCacheState::ContainerEnumeration* current=nullptr;
        if(first){if(arg(0)){memory.check(GuestAddress{arg(0)},4,Access::Write);memory.store(GuestAddress{arg(0)},32,0);}enumeration.items=containers;current=&enumeration;}
        else {const auto found=local.container_enumerations.find(handle);if(found==local.container_enumerations.end())throw CacheError{6};current=&found->second;}
        if(!arg(2))throw CacheError{87};if(current->cursor>=current->items.size())throw CacheError{259};
        const auto& item=current->items[current->cursor];const unsigned unit=x86_?4:8,header=unit*5;
        const auto name=encoded(item.name,0),prefix=encoded(item.prefix,0),label=encoded(u"",0),title=encoded(u"",0);
        const auto size=header+name.size()+prefix.size()+label.size()+title.size();
        const auto capacity=memory.load(GuestAddress{arg(2)},32);memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,size);
        if(!arg(1) || capacity<size)throw CacheError{122};
        std::vector<std::uint8_t> info(header);std::size_t cursor=header;const auto pointer=[&](unsigned slot,const std::vector<std::uint8_t>& bytes){const auto at=arg(1)+cursor;for(unsigned i=0;i<unit;++i)info[slot*unit+i]=static_cast<std::uint8_t>(at>>(i*8));info.insert(info.end(),bytes.begin(),bytes.end());cursor+=bytes.size();};
        pointer(1,name);pointer(2,prefix);pointer(3,label);pointer(4,title);memory.write(GuestAddress{arg(1)},info);++current->cursor;
        if(first){if(local.container_enumerations.size()>=4096)throw CacheError{8};handle=local.next++;local.container_enumerations.emplace(handle,std::move(enumeration));result=handle;}else result=1;
    }else if(symbol=="IncrementUrlCacheHeaderData"){
        // (nIdx, lpdwData)：32 個持久的快取標頭 DWORD，遞增後回傳新值。
        if(d(0)>=32 || !arg(1))throw CacheError{87};memory.check(GuestAddress{arg(1)},4,Access::Write);const auto value=++metadata.header[d(0)];
        url_cache::store_metadata(registry,metadata);transaction.commit();memory.store(GuestAddress{arg(1)},32,value);result=1;
    }
    if(result)last_error_=0;arg.return_to(state,result);return std::nullopt;
}
