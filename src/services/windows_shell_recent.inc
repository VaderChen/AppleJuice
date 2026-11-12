// SHAddToRecentDocs：使用 sandbox 檔案系統與同一份跨程序 registry 交易；不寫入 macOS 最近項目。
namespace {
std::u16string recent_fold(std::u16string value){for(auto& c:value)c=unicode_case_unit(c,false);return value;}
std::u16string recent_id(std::u16string_view value){
    std::uint64_t hash=14695981039346656037ULL;for(auto c:value){hash^=c&255;hash*=1099511628211ULL;hash^=c>>8;hash*=1099511628211ULL;}
    std::u16string result(16,u'0');for(unsigned i=16;i-->0;){result[i]=u"0123456789abcdef"[hash&15];hash>>=4;}return result;
}
std::vector<std::uint8_t> recent_link(const std::u16string& path,const FileMetadata& metadata){
    // MS-SHLLINK：標準 LinkInfo 同時保存 ANSI／Unicode 路徑，可由一般 Shell Link 讀取器解析。
    std::vector<std::uint8_t> bytes(76);
    const auto put=[&](std::size_t at,std::uint64_t n,unsigned count=4){for(unsigned i=0;i<count;++i)bytes.at(at+i)=static_cast<std::uint8_t>(n>>(8*i));};
    put(0,76);put(4,0x21401);bytes[12]=0xc0;bytes[19]=0x46;put(20,0x82);put(24,metadata.attributes);put(52,metadata.size);put(60,1);
    const auto start=bytes.size();bytes.resize(start+36+20);put(start+4,36);put(start+8,1);put(start+12,36);
    put(start+36,20);put(start+40,3);put(start+48,16);
    put(start+16,bytes.size()-start);for(const auto c:path)bytes.push_back(c<128?static_cast<std::uint8_t>(c):'?');bytes.push_back(0);
    put(start+24,bytes.size()-start);bytes.push_back(0);
    put(start+28,bytes.size()-start);const auto wide=utf16_bytes(path);bytes.insert(bytes.end(),wide.begin(),wide.end());
    put(start+32,bytes.size()-start);bytes.insert(bytes.end(),2,0);put(start,bytes.size()-start);bytes.insert(bytes.end(),4,0);return bytes;
}
}
std::uint32_t WindowsServices::store_recent_document(std::u16string path,std::u16string app,bool link,std::u16string arguments,std::u16string icon,int icon_index){
    const auto root=registry_current_user_path()+u"\\software\\applejuice\\recentdocs";
    const auto folder=known_folder(8);RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_);
    if(transaction.error)return transaction.error;
    if(path.empty()){
        const auto listing=filesystem_.list(utf16_to_utf8(folder));
        if(!listing.error)for(const auto& entry:listing.entries){if(recent_fold(utf8_to_utf16_lossy(entry.name)).ends_with(u".lnk") && !(entry.metadata.attributes&16)){
            const auto error=filesystem_.remove(utf16_to_utf8(folder+u"\\"+utf8_to_utf16_lossy(entry.name)),false);if(error)return error;}}
        else if(listing.error!=2 && listing.error!=3)return listing.error;
        for(auto it=runtime_.registry.keys.begin();it!=runtime_.registry.keys.end();)if(it->first==root || it->first.starts_with(root+u"\\"))it=runtime_.registry.keys.erase(it);else ++it;
        return commit_registry_transaction(transaction);
    }
    path=normalized(std::move(path));if(path.empty())return 123;
    const auto info=filesystem_.information(utf16_to_utf8(path));if(info.error)return info.error;
    const auto folded=recent_fold(path);if(folded.ends_with(u".exe"))return 0;
    const bool directory=(info.metadata.attributes&16)!=0;
    if(directory)app=u"Explorer"; // 資料夾只屬於檔案總管的 Jump List。
    const auto id=recent_id(folded),application=recent_id(recent_fold(app));
    const auto lookup=[&](std::u16string_view key,std::u16string_view name)->const RegistryValue*{
        const auto k=runtime_.registry.keys.find(std::u16string(key));if(k==runtime_.registry.keys.end())return nullptr;
        const auto v=k->second.find(std::u16string(name));return v==k->second.end()?nullptr:&v->second;};
    for(const auto& hive:std::array<std::u16string,2>{registry_current_user_path(),std::u16string(1,2)})
        if(const auto* policy=lookup(hive+u"\\software\\microsoft\\windows\\currentversion\\policies\\explorer",u"norecentdocshistory");policy && policy->type==4 && policy->bytes.size()>=4 && std::any_of(policy->bytes.begin(),policy->bytes.begin()+4,[](auto b){return b!=0;}))return 0;
    const auto extension=folded.substr(folded.find_last_of(u'.')==folded.npos?folded.size():folded.find_last_of(u'.'));
    for(const auto& hive:std::array<std::u16string,2>{registry_current_user_path()+u"\\software\\classes",std::u16string(1,2)+u"\\software\\classes"}){
        auto association=hive+u"\\"+extension;
        if(const auto* value=lookup(association,u"");value && value->type==1){std::u16string name;for(std::size_t i=0;i+1<value->bytes.size();i+=2){const auto c=static_cast<char16_t>(value->bytes[i]|(unsigned{value->bytes[i+1]}<<8));if(!c)break;name+=c;}if(!name.empty())association=hive+u"\\"+recent_fold(name);}
        if(lookup(association,u"norecentdocs"))return 0;
        if(const auto* flags=lookup(association,u"editflags");flags && flags->bytes.size()>=4 && (flags->bytes[2]&0x10))return 0;
    }
    if(!link && !directory){
        for(std::size_t at=3;at<=folder.size();++at)if(at==folder.size() || folder[at]==u'\\'){
            const auto error=filesystem_.create_directory(utf16_to_utf8(folder.substr(0,at)));if(error && error!=183)return error;}
        const auto destination=utf16_to_utf8(folder+u"\\"+leaf_name(path).substr(0,120)+u"-"+id+u".lnk");
        const auto bytes=recent_link(path,info.metadata);auto output=filesystem_.open_host(destination,0x40000000U,2,0);
        if(output.error)return output.error;const auto written=output.file->write(bytes,0);
        if(written.error || written.bytes!=bytes.size())return written.error?written.error:112;
        if(const auto error=output.file->flush())return error;
    }
    auto& values=runtime_.registry.keys[root+u"\\"+application+u"\\"+id];
    const auto put=[&](const char16_t* name,const std::u16string& text){values[name]=RegistryValue{1,utf16_bytes(text)};};
    put(u"path",path);put(u"application",app);put(u"arguments",arguments);put(u"icon",icon);
    std::uint32_t count=0;if(const auto old=values.find(u"count");old!=values.end() && old->second.bytes.size()==4)for(unsigned i=0;i<4;++i)count|=unsigned{old->second.bytes[i]}<<(8*i);
    if(count!=UINT32_MAX)++count;
    const auto number=[&](const char16_t* name,std::uint64_t n,unsigned size){std::vector<std::uint8_t> bytes(size);for(unsigned i=0;i<size;++i)bytes[i]=static_cast<std::uint8_t>(n>>(8*i));values[name]=RegistryValue{size==8?11U:4U,std::move(bytes)};};
    number(u"count",count,4);number(u"iconindex",static_cast<std::uint32_t>(icon_index),4);
    number(u"lastaccess",static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()/100)+116444736000000000ULL,8);
    return commit_registry_transaction(transaction);
}
void WindowsServices::discard_recent_document(GuestState& state,GuestMemory& memory,std::uint64_t id){
    const auto found=recent_document_calls_.find(id);if(found==recent_document_calls_.end())return;
    if(!found->second.link){const auto value=memory.load(GuestAddress{id},x86_?32:64);if(value && allocations_.contains(value)){gui_.task_allocations.erase(value);(void)free_heap(memory,value,state);}}
    recent_document_calls_.erase(found);(void)free_heap(memory,id,state);
}
std::optional<std::uint32_t> WindowsServices::resume_recent_document(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint32_t status){
    auto& call=recent_document_calls_.at(id);const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    try{
        if(!(status&0x80000000U)){
            if(!call.link){const auto text=memory.load(GuestAddress{id},bits);if(text)call.path=read_utf16(memory,GuestAddress{text},32767);}
            else if(call.stage==0)call.path=read_utf16(memory,GuestAddress{id},32767);
            else if(call.stage==1)call.arguments=read_utf16(memory,GuestAddress{id},32767);
            else {call.icon=read_utf16(memory,GuestAddress{id},32767);call.icon_index=static_cast<std::int32_t>(memory.load(GuestAddress{id+65536},32));}
            if(call.link && call.stage<2){++call.stage;const unsigned slot=call.stage==1?10:16;
                const auto table=memory.load(GuestAddress{call.object},bits),target=memory.load(GuestAddress{table+slot*(bits/8)},bits);
                memory.store(GuestAddress{id},16,0);start_callbacks(state,memory,{{target,call.stage==1?std::vector<std::uint64_t>{call.object,id,32768}:std::vector<std::uint64_t>{call.object,id,32768,id+65536}}},Continuation::RecentDocument,id);return {};}
            if(!call.path.empty())if(const auto error=store_recent_document(call.path,call.app,call.link,call.arguments,call.icon,call.icon_index))last_error_=error;
        }
    }catch(...){discard_recent_document(state,memory,id);throw;}
    discard_recent_document(state,memory,id);complete_call(state,memory,0);return {};
}
std::optional<std::uint32_t> WindowsServices::invoke_recent_document(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto flags=static_cast<unsigned>(arg(0));auto object=arg(1);
    const auto finish=[&](std::uint32_t error){if(error)last_error_=error;arg.return_to(state,0);return std::optional<std::uint32_t>{};};
    if(!object)return finish(store_recent_document({}, {},false));
    std::u16string app=gui_.explicit_app_user_model_id.empty()?runtime_.image.path:gui_.explicit_app_user_model_id;const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    if(flags==4 || flags==5 || flags==7){const auto name=memory.load(GuestAddress{object+bits/8},bits);if(!name)return finish(87);app=read_utf16(memory,GuestAddress{name},128);object=memory.load(GuestAddress{object},bits);if(!object)return finish(87);}
    if(flags==1 || flags==5){const auto found=gui_.shell_pidls.find(object);return finish(found==gui_.shell_pidls.end()?87:store_recent_document(found->second,app,false));}
    if(flags==2 || flags==3)return finish(store_recent_document(flags==3?read_utf16(memory,GuestAddress{object},32767):utf8_to_utf16_lossy(read_ansi(memory,object,32767)),app,false));
    if(flags!=4 && flags!=6 && flags!=7 && flags!=8)return finish(87);
    const bool link=flags==6 || flags==7;
    if(!link)if(const auto item=gui_.shell_items.find(object);item!=gui_.shell_items.end())return finish(store_recent_document(item->second.path,app,false));
    const auto table=memory.load(GuestAddress{object},bits),target=memory.load(GuestAddress{table+(link?3:5)*(bits/8)},bits);
    const auto scratch=allocate_heap(memory,65544);if(!scratch)return finish(8);memory.store(GuestAddress{scratch},bits,0);
    recent_document_calls_.emplace(scratch,RecentDocumentCall{object,scratch,0,link,app,{},{},{},0});
    start_callbacks(state,memory,{{target,link?std::vector<std::uint64_t>{object,scratch,32768,0,4}:std::vector<std::uint64_t>{object,0x80058000U,scratch}}},Continuation::RecentDocument,scratch);return {};
}
