// NT 物件命名空間（由 windows_nt_native.cpp 引入，位於 x64arm 命名空間）：
// - 內建目錄（\、\Sessions、\Sessions\1、兩個 BaseNamedObjects、\GLOBAL??、\KnownDlls、\KnownDlls32）與內建符號連結
//   （Global／Local、\DosDevices、\SystemRoot、\GLOBAL??\<磁碟機>:、GLOBALROOT、NUL、PIPE、MAILSLOT、KnownDllPath），
//   磁碟機連結依目前卷配置產生；\?? 對應 \GLOBAL??（本平台只有一個登入工作階段）。
// - NtCreateDirectoryObject(Ex)／NtCreateSymbolicLinkObject 建立的動態項目與私有命名空間由 ObjectDirectory 紀錄跨程序共享；
//   暫時項目在最後一個控制代碼關閉時刪除，NtMakePermanentObject 需要 SeCreatePermanentPrivilege。
// - 具名事件、mutex、semaphore、section、job 建在動態目錄或私有命名空間時，以建立時的隨機身分組成內部 Win32 名稱，
//   沿用既有具名物件儲存；BaseNamedObjects 本身的物件維持原有 Win32 名稱。
// - 邊界描述元（Rtl*BoundaryDescriptor、CreateBoundaryDescriptor 等）是程序內 heap 區塊：
//   {Version=1, Items, TotalSize, Flags} 之後接 {EntryType(1 名稱／2 SID／3 完整性標籤), EntrySize, 資料}，項目 4 位元組對齊。
namespace {
constexpr char16_t namespace_marker=u'\uF8FF';
std::u16string namespace_fold(std::u16string_view text){std::u16string out(text);for(auto& c:out)c=windows_path_upper(c);return out;}
bool namespace_equal(std::u16string_view a,std::u16string_view b){
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](char16_t x,char16_t y){return windows_path_upper(x)==windows_path_upper(y);});
}
std::u16string namespace_parent(std::u16string_view path){const auto at=path.rfind(u'\\');return at==0 || at==path.npos?std::u16string(u"\\"):std::u16string(path.substr(0,at));}
std::u16string namespace_leaf(std::u16string_view path){const auto at=path.rfind(u'\\');return std::u16string(at==path.npos?path:path.substr(at+1));}
std::u16string namespace_hex(std::uint64_t value){std::u16string out(16,u'0');for(int i=15;i>=0;--i){const auto digit=static_cast<unsigned>(value&15U);out[static_cast<std::size_t>(i)]=static_cast<char16_t>(digit<10?u'0'+digit:u'a'+digit-10);value>>=4;}return out;}
std::uint32_t builtin_directory_index(std::u16string_view path){
    for(std::size_t n=0;n<builtin_object_directories.size();++n)if(namespace_equal(builtin_object_directories[n],path))return static_cast<std::uint32_t>(n);
    return UINT32_MAX;
}
// 動態目錄或私有命名空間內物件的內部 Win32 名稱（不含反斜線，既有具名物件儲存可直接使用）。
std::u16string namespace_member_name(std::uint64_t serial,std::u16string_view leaf){
    std::u16string out=u"Global\\";out.push_back(namespace_marker);out+=namespace_hex(serial);out.push_back(namespace_marker);out+=leaf;return out;
}
std::u16string private_namespace_path(std::uint64_t serial){std::u16string out=u"\\";out.push_back(namespace_marker);out+=namespace_hex(serial);return out;}
std::uint64_t namespace_random(){return static_cast<std::uint64_t>(::arc4random())<<32|::arc4random();}
// 既有具名物件（事件、semaphore、mutex、section、job）的共享紀錄：只讀標頭中的型別與名稱，不改變參考數或存活狀態。
struct NamedObjectListing {std::u16string name;std::u16string_view type;};
std::vector<NamedObjectListing> scan_named_objects(const std::string& session){
    std::vector<NamedObjectListing> result;
#if !defined(_WIN32)
    const int directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(directory<0)return result;
    DIR* stream=::fdopendir(directory);if(!stream){::close(directory);return result;}
    while(const auto* item=::readdir(stream)){
        if(std::strncmp(item->d_name,"section-",8))continue;
        const int file=::openat(::dirfd(stream),item->d_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(file<0)continue;
        std::array<std::uint8_t,600> header{};const auto read=::pread(file,header.data(),header.size(),0);::close(file);
        if(read<64)continue;
        const auto qword=[&](unsigned at){std::uint64_t value;std::memcpy(&value,header.data()+at,8);return value;};
        const auto dword=[&](unsigned at){std::uint32_t value;std::memcpy(&value,header.data()+at,4);return value;};
        unsigned chars_at=0,name_at=0;std::u16string_view type;bool live=true;
        switch(qword(0)){
        case UINT64_C(0x544e564552413458):type=u"Event";chars_at=24;name_at=28;live=qword(8)!=0;break;
        case UINT64_C(0x504d455352413458):type=u"Semaphore";chars_at=24;name_at=28;live=qword(8)!=0;break;
        case UINT64_C(0x5854554d52413458):type=u"Mutant";chars_at=40;name_at=44;live=qword(8)!=0;break;
        case UINT64_C(0x50414d4d52413458):type=u"Section";chars_at=28;name_at=32;live=qword(16)!=0;break;
        case UINT64_C(0x33424f4a52413458):type=u"Job";chars_at=32;name_at=64;break;
        default:continue;
        }
        const auto chars=dword(chars_at);if(!live || !chars || chars>=260 || name_at+chars*2>static_cast<unsigned>(read))continue;
        std::u16string name(chars,u'\0');std::memcpy(name.data(),header.data()+name_at,chars*2U);
        result.push_back({std::move(name),type});
    }
    ::closedir(stream);
#else
    (void)session;
#endif
    return result;
}
// 邊界描述元：名稱、SID 與完整性標籤；key 為跨程序比對用的正規化字串（SID 排序後以十六進位表示）。
struct BoundaryDescriptor {std::u16string name;std::vector<std::vector<std::uint8_t>> sids;std::vector<std::uint8_t> label;std::uint32_t flags{};
    std::u16string key() const {
        std::u16string out=name;out.push_back(0);auto sorted=sids;std::sort(sorted.begin(),sorted.end());
        const auto hex=[&](const std::vector<std::uint8_t>& bytes){for(auto b:bytes){static constexpr char16_t digits[]=u"0123456789abcdef";out.push_back(digits[b>>4]);out.push_back(digits[b&15]);}};
        for(const auto& sid:sorted){hex(sid);out.push_back(u',');}
        out.push_back(0);hex(label);return out;
    }
};
bool valid_boundary_sid(std::span<const std::uint8_t> sid){return sid.size()>=8 && sid[0]==1 && sid[1]<=15 && sid.size()==8U+sid[1]*4U;}
bool integrity_label_sid(std::span<const std::uint8_t> sid){
    return valid_boundary_sid(sid) && sid[1]==1 && sid[2]==0 && sid[3]==0 && sid[4]==0 && sid[5]==0 && sid[6]==0 && sid[7]==16;
}
// 讀取客體邊界描述元；格式錯誤時回 STATUS_INVALID_PARAMETER。
std::uint32_t read_boundary(const GuestMemory& memory,std::uint64_t address,BoundaryDescriptor& out){
    if(!address)return 0xc000000dU;
    memory.check(GuestAddress{address},16,Access::Read);
    const auto version=memory.load(GuestAddress{address},32),items=memory.load(GuestAddress{address+4},32),total=memory.load(GuestAddress{address+8},32);
    if(version!=1 || total<16 || total>0x10000 || items>1024)return 0xc000000dU;
    out.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{address+12},32));
    std::vector<std::uint8_t> bytes(total);memory.read(GuestAddress{address},bytes);
    std::size_t at=16;bool named=false;
    for(std::uint64_t n=0;n<items;++n){
        if(at+8>total)return 0xc000000dU;
        std::uint32_t type,size;std::memcpy(&type,bytes.data()+at,4);std::memcpy(&size,bytes.data()+at+4,4);
        if(size<8 || at+size>total)return 0xc000000dU;
        const auto data=std::span<const std::uint8_t>(bytes).subspan(at+8,size-8);
        if(type==1){
            if(named || data.size()<2)return 0xc000000dU;named=true;
            out.name.assign(data.size()/2,u'\0');std::memcpy(out.name.data(),data.data(),out.name.size()*2);
            while(!out.name.empty() && !out.name.back())out.name.pop_back();
            if(out.name.empty())return 0xc000000dU;
        }else if(type==2 || type==3){
            const auto length=data.size()>=2?8U+data[1]*4U:0U;if(!length || length>data.size() || !valid_boundary_sid(data.first(length)))return 0xc000000dU;
            std::vector<std::uint8_t> sid(data.begin(),data.begin()+length);
            if(type==3){if(!integrity_label_sid(sid) || !out.label.empty())return 0xc000000dU;out.label=std::move(sid);}
            else out.sids.push_back(std::move(sid));
        }else return 0xc000000dU;
        at+=(size+3U)&~3U;
    }
    return named?0U:0xc000000dU;
}
}
std::vector<std::pair<std::u16string,std::u16string>> WindowsServices::builtin_namespace_links(){
    std::vector<std::pair<std::u16string,std::u16string>> links{
        {u"\\Sessions\\1\\BaseNamedObjects\\Global",u"\\BaseNamedObjects"},{u"\\Sessions\\1\\BaseNamedObjects\\Local",u"\\Sessions\\1\\BaseNamedObjects"},
        {u"\\BaseNamedObjects\\Global",u"\\BaseNamedObjects"},{u"\\BaseNamedObjects\\Local",u"\\BaseNamedObjects"},
        {u"\\DosDevices",u"\\??"},{u"\\SystemRoot",windows_device_name(u'C')+u"\\Windows"},
        {u"\\GLOBAL??\\GLOBALROOT",u""},{u"\\GLOBAL??\\Global",u"\\GLOBAL??"},{u"\\GLOBAL??\\NUL",u"\\Device\\Null"},
        {u"\\GLOBAL??\\PIPE",u"\\Device\\NamedPipe"},{u"\\GLOBAL??\\MAILSLOT",u"\\Device\\Mailslot"},
        {u"\\KnownDlls\\KnownDllPath",u"C:\\Windows\\System32"},{u"\\KnownDlls32\\KnownDllPath",u"C:\\Windows\\SysWOW64"}};
    const auto state=filesystem_.volume_state();
    if(!state.error)for(const auto& [letter,volume]:state.drives){
        std::u16string path=u"\\GLOBAL??\\";path.push_back(static_cast<char16_t>(letter));path.push_back(u':');
        links.emplace_back(std::move(path),windows_device_name(static_cast<char16_t>(volume)));
    }
    return links;
}
WindowsServices::NamespaceLookup WindowsServices::lookup_object_namespace(std::u16string path,bool open_link){
    NamespaceLookup out;std::optional<std::vector<std::pair<std::u16string,std::u16string>>> links;
    const auto builtin_link=[&](std::u16string_view candidate)->const std::pair<std::u16string,std::u16string>*{
        if(!links)links=builtin_namespace_links();
        for(const auto& link:*links)if(namespace_equal(link.first,candidate))return &link;
        return nullptr;
    };
    for(unsigned reparse=0;reparse<=32;++reparse){
        if(path.empty())path=u"\\";
        if(path.front()!=u'\\'){out.status=0xc000003bU;return out;}
        if(path.size()>=3 && path[1]==u'?' && path[2]==u'?' && (path.size()==3 || path[3]==u'\\'))path=u"\\GLOBAL??"+path.substr(3);
        if(path.size()==1){out.path=u"\\";out.builtin=0;return out;}
        std::u16string current=u"\\";std::uint32_t current_builtin=0;std::shared_ptr<ObjectDirectory> current_entry;
        std::size_t at=1;bool restart=false;
        while(true){
            auto next=path.find(u'\\',at);const bool last=next==path.npos;if(last)next=path.size();
            const auto component=std::u16string_view(path).substr(at,next-at);
            if(component.empty()){out.status=0xc0000033U;return out;}
            auto candidate=(current.size()==1?current:current+u"\\")+std::u16string(component);
            std::uint32_t index=builtin_directory_index(candidate);std::shared_ptr<ObjectDirectory> entry;
            if(index!=UINT32_MAX)candidate=std::u16string(builtin_object_directories[index]);
            else if(const auto* link=builtin_link(candidate)){
                if(last && open_link){out.path=link->first;out.parent=current;out.leaf=namespace_leaf(link->first);out.link=out.builtin_link=true;out.target=link->second;
                    out.parent_builtin=current_builtin;out.parent_entry=current_entry;return out;}
                path=link->second+path.substr(next);restart=true;break;
            }else{
                auto opened=ObjectDirectory::open_entry(mapping_session(),ObjectDirectory::path_key(candidate),nullptr,{});
                if(opened.status==0xc0000034U){
                    if(!last){out.status=0xc000003aU;return out;}
                    out.status=opened.status;out.path=candidate;out.parent=current;out.leaf=std::u16string(component);out.parent_builtin=current_builtin;out.parent_entry=current_entry;return out;
                }
                if(opened.status){out.status=opened.status;return out;}
                entry=std::move(opened.object);
                if(entry->symbolic_link()){
                    if(last && open_link){out.path=std::u16string(entry->path());out.parent=current;out.leaf=namespace_leaf(out.path);out.link=true;out.target=std::u16string(entry->target());
                        out.entry=std::move(entry);out.parent_builtin=current_builtin;out.parent_entry=current_entry;return out;}
                    path=std::u16string(entry->target())+path.substr(next);restart=true;break;
                }
                candidate=std::u16string(entry->path());
            }
            if(last){out.path=candidate;out.parent=current;out.leaf=namespace_leaf(candidate);out.builtin=index;out.entry=std::move(entry);
                out.parent_builtin=current_builtin;out.parent_entry=current_entry;return out;}
            current=std::move(candidate);current_builtin=index;current_entry=std::move(entry);at=next+1;
        }
        if(!restart)break;
    }
    out.status=0xc000003aU;return out;
}
std::uint32_t WindowsServices::namespace_object_name(std::u16string& name,bool creating){
    if(name.empty())return 0;
    if(auto mapped=win32_object_name(name)){
        if(mapped->empty())return 0xc0000033U;
        const auto rest=std::u16string_view(*mapped).substr(mapped->starts_with(u"Global\\")?7:mapped->starts_with(u"Local\\")?6:0);
        if(rest.empty())return 0xc0000033U;
        if(rest.find(u'\\')==rest.npos){name=std::move(*mapped);return 0;}
    }
    if(name.front()!=u'\\')return 0xc000003bU;
    // 私有命名空間或無名目錄的內部根：\<U+F8FF><身分>\<名稱>
    if(name.size()>=18 && name[1]==namespace_marker){
        const auto rest=std::u16string_view(name).substr(18);
        if(rest.size()<2 || rest.front()!=u'\\')return rest.empty()?0xc0000024U:0xc000003aU;
        if(rest.find(u'\\',1)!=rest.npos)return 0xc000003aU;
        std::uint64_t serial=0;for(std::size_t n=2;n<18;++n){const auto c=name[n];serial=serial<<4|static_cast<std::uint64_t>(c<=u'9'?c-u'0':c-u'a'+10);}
        name=namespace_member_name(serial,rest.substr(1));return 0;
    }
    const auto leaf=namespace_leaf(name);if(leaf.empty())return 0xc0000033U;
    const auto parent=lookup_object_namespace(namespace_parent(name),false);
    if(parent.status)return parent.status==0xc0000034U?0xc000003aU:parent.status;
    if(parent.builtin==UINT32_MAX && !parent.entry)return 0xc000003aU;
    // 名稱已是目錄或符號連結時不可再建立同名物件。
    if(const auto child=lookup_object_namespace((parent.path.size()==1?parent.path:parent.path+u"\\")+leaf,true);!child.status)return creating?0xc0000035U:0xc0000024U;
    if(parent.builtin==3)name=leaf;
    else if(parent.builtin==4)name=u"Global\\"+leaf;
    else if(parent.entry)name=namespace_member_name(parent.entry->entry().serial,leaf);
    else name=namespace_member_name(parent.builtin,leaf);
    return 0;
}
std::uint32_t WindowsServices::win32_namespace_object_name(std::u16string& name,bool creating){
    const bool global=name.starts_with(u"Global\\"),local=name.starts_with(u"Local\\");
    const auto rest=std::u16string_view(name).substr(global?7:local?6:0);
    const auto slash=rest.find(u'\\');if(slash==rest.npos)return 0;
    if(!global && !local){
        if(const auto alias=private_namespace_aliases_.find(namespace_fold(rest.substr(0,slash)));alias!=private_namespace_aliases_.end()){
            const auto handle=runtime_.object_directories.find(alias->second.first);const auto object=alias->second.second.lock();
            if(handle!=runtime_.object_directories.end() && object && handle->second.object==object){
                const auto leaf=rest.substr(slash+1);if(leaf.empty())return 123;if(leaf.find(u'\\')!=leaf.npos)return 3;
                name=namespace_member_name(object->entry().serial,leaf);return 0;
            }
        }
    }
    std::u16string path=(global?u"\\BaseNamedObjects\\":u"\\Sessions\\1\\BaseNamedObjects\\")+std::u16string(rest);
    const auto status=namespace_object_name(path,creating);
    if(status)return status==0xc000003aU || status==0xc0000034U?3U:status==0xc0000035U?183U:status==0xc0000024U?6U:status==0xc0000022U?5U:123U;
    name=std::move(path);return 0;
}
std::u16string WindowsServices::namespace_display_name(std::u16string_view name){
    if(!name.starts_with(u"Global\\") || name.size()<25 || name[7]!=namespace_marker || name[24]!=namespace_marker)return std::u16string(name);
    const auto leaf=name.substr(25);
    for(const auto& entry:ObjectDirectory::list_entries(mapping_session())){
        if(namespace_hex(entry.serial)!=name.substr(8,16))continue;
        return entry.flags&object_namespace_private?u"\\"+std::u16string(leaf):entry.path+u"\\"+std::u16string(leaf);
    }
    for(std::size_t n=0;n<builtin_object_directories.size();++n)
        if(namespace_hex(n)==name.substr(8,16))return (n?std::u16string(builtin_object_directories[n])+u"\\":std::u16string(u"\\"))+std::u16string(leaf);
    return u"\\"+std::u16string(leaf);
}
std::vector<std::pair<std::u16string,std::u16string>> WindowsServices::object_directory_listing(const ObjectDirectory& directory){
    std::vector<std::pair<std::u16string,std::u16string>> items;
    const auto path=std::u16string(directory.path());
    const auto add=[&](std::u16string name,std::u16string type){
        if(std::none_of(items.begin(),items.end(),[&](const auto& item){return namespace_equal(item.first,name);}))items.emplace_back(std::move(name),std::move(type));};
    if(!directory.private_namespace()){
        for(std::size_t n=1;n<builtin_object_directories.size();++n)
            if(namespace_equal(namespace_parent(builtin_object_directories[n]),path))add(namespace_leaf(builtin_object_directories[n]),u"Directory");
        for(const auto& [link,target]:builtin_namespace_links()){(void)target;if(namespace_equal(namespace_parent(link),path))add(namespace_leaf(link),u"SymbolicLink");}
    }
    const auto session=mapping_session();
    std::vector<std::pair<std::u16string,std::u16string>> dynamic;
    for(const auto& entry:ObjectDirectory::list_entries(session))
        if(!(entry.flags&(object_namespace_private|object_namespace_builtin)) && namespace_equal(namespace_parent(entry.path),path))
            dynamic.emplace_back(namespace_leaf(entry.path),entry.kind==ObjectNamespaceKind::SymbolicLink?u"SymbolicLink":u"Directory");
    std::sort(dynamic.begin(),dynamic.end());for(auto& item:dynamic)add(std::move(item.first),std::move(item.second));
    std::u16string prefix;bool session_local=false;
    if(directory.index()==4)prefix=u"Global\\";
    else if(directory.index()==3)session_local=true;
    else prefix=namespace_member_name(directory.dynamic()?directory.entry().serial:directory.index(),u"");
    std::vector<std::pair<std::u16string,std::u16string>> named;
    for(auto& object:scan_named_objects(session)){
        if(session_local){if(object.name.starts_with(u"Global\\") || object.name.starts_with(u"Local\\"))continue;named.emplace_back(std::move(object.name),std::u16string(object.type));}
        else if(object.name.starts_with(prefix) && object.name.size()>prefix.size()){
            if(directory.index()==4 && object.name[7]==namespace_marker)continue;
            named.emplace_back(object.name.substr(prefix.size()),std::u16string(object.type));
        }
    }
    std::sort(named.begin(),named.end());for(auto& item:named)add(std::move(item.first),std::move(item.second));
    return items;
}
std::optional<std::uint32_t> WindowsServices::invoke_object_namespace(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto name=service.symbol;const auto saved_error=last_error_;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto word=[&](unsigned index){return static_cast<std::uint32_t>(call(index));};
    const bool native=name.starts_with("Nt") || name.starts_with("Zw") || name.starts_with("Rtl");
    const auto finish=[&](std::uint64_t value){
        if(name.starts_with("Nt") || name.starts_with("Zw"))threads_.items.at(threads_.current).last_native_status=static_cast<std::uint32_t>(value);
        if(native)last_error_=saved_error;call.return_to(state,value);return std::optional<std::uint32_t>{};
    };
    const auto win32_error=[&](std::uint32_t status){
        const auto saved=last_error_;const auto error=static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status}));last_error_=saved;return error;};
    const auto fail=[&](std::uint32_t error,std::uint64_t value){last_error_=error;return finish(value);};
    const auto session=[&]{return mapping_session();};
    // OBJECT_ATTRIBUTES：OBJ_PERMANENT（0x10）需要 SeCreatePermanentPrivilege；其餘旗標沿用共用檢查。
    const auto token_privilege=[&](std::uint64_t luid){initialize_tokens();return runtime_.token_objects.at(effective_token_id()).has_privilege(luid,true);};
    const auto read_attributes=[&](std::uint64_t address,bool optional,ObjectAttributes& attr)->std::uint32_t{
        if(!address){if(optional)return 0;return 0xc000000dU;}
        memory.check(GuestAddress{address},x86_?24U:48U,Access::Read);
        const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{address+3U*unit},32));
        if(flags&0x10U){
            if(!token_privilege(16))return 0xc0000061U;
            GuestScratch copy(*this,memory,state);const auto block=copy.allocate(x86_?24U:48U);if(!block)return 0xc0000017U;
            std::vector<std::uint8_t> bytes(x86_?24U:48U);memory.read(GuestAddress{address},bytes);memory.write(GuestAddress{block},bytes);
            memory.store(GuestAddress{block+3U*unit},32,flags&~0x10U);attr=object_attributes(memory,block,x86_);attr.flags|=0x10U;return 0;
        }
        attr=object_attributes(memory,address,x86_);return 0;
    };
    const auto security_factory=[&](std::uint64_t descriptor)->ObjectSecurityFactory{
        return [this,&memory,&state,descriptor]{
            GuestScratch block(*this,memory,state);const unsigned size=x86_?12U:24U;const auto sa=block.allocate(size);if(!sa)throw SecurityDescriptorError{8};
            memory.store(GuestAddress{sa},32,size);memory.store(GuestAddress{sa+(x86_?4U:8U)},x86_?32:64,descriptor);
            return synchronization_descriptor(memory,sa).bytes;
        };
    };
    const auto descriptor_access=[&](const std::shared_ptr<ObjectDirectory>& object,std::uint32_t desired){
        return object_security_access(shared_object_security(object->descriptor()),desired,object->generic_mapping());};
    const auto install=[&](std::shared_ptr<ObjectDirectory> object,std::uint32_t access,bool inherit,std::uint64_t out)->std::uint32_t{
        if(runtime_.object_directories.size()>=process_handle_limit)return 0xc0000017U;
        const auto handle=runtime_.allocate_wait_handle();runtime_.object_directories.emplace(handle,ObjectDirectoryHandle{std::move(object),access});
        if(inherit)runtime_.handle_flags[handle]=1;
        if(out)memory.store(GuestAddress{out},bits,handle);
        return 0;
    };
    // 父目錄建立權：根目錄控制代碼已在 rooted_object_name 檢查；絕對路徑依父目錄的安全描述元檢查。
    const auto parent_allows=[&](const NamespaceLookup& found,std::uint32_t right)->std::uint32_t{
        std::shared_ptr<ObjectDirectory> parent=found.parent_entry;
        if(!parent){
            if(found.parent_builtin==UINT32_MAX)return 0xc000003aU;
            parent=ObjectDirectory::open(session(),found.parent_builtin,[&]{return synchronization_descriptor(memory,0).bytes;});
        }
        if(parent->symbolic_link())return 0xc0000024U;
        (void)object_security_access(shared_object_security(parent->descriptor()),right,parent->generic_mapping());return 0;
    };
    try{
        // (Handle*, DesiredAccess, ObjectAttributes*[, Target*|ShadowDirectory, Flags])
        if(name.ends_with("CreateDirectoryObject") || name.ends_with("CreateDirectoryObjectEx") || name.ends_with("CreateSymbolicLinkObject")){
            const bool link=name.ends_with("CreateSymbolicLinkObject");
            if(!call(0))return finish(0xc0000005U);memory.check(GuestAddress{call(0)},unit,Access::Write);
            if(name.ends_with("Ex")){if(word(4)&~1U)return finish(0xc000000dU);if(call(3) && !runtime_.object_directories.contains(call(3)))return finish(0xc0000008U);}
            ObjectAttributes attr;if(const auto status=read_attributes(call(2),true,attr))return finish(status);
            std::u16string target;
            if(link){if(!call(3))return finish(0xc000000dU);target=native_string(memory,call(3),x86_);if(target.size()>object_namespace_target_limit)return finish(0xc0000106U);}
            const auto desired=word(1);
            ObjectNamespaceEntry create;create.kind=link?ObjectNamespaceKind::SymbolicLink:ObjectNamespaceKind::Directory;create.target=target;
            if(attr.flags&0x10U)create.flags|=object_namespace_permanent;
            if(attr.name.empty() && !attr.root){
                // 無名目錄／連結：只能經控制代碼使用；內部路徑含隨機身分，不列入目錄也不經名稱查詢。
                create.serial=namespace_random();create.path=private_namespace_path(create.serial);create.flags|=object_namespace_private|object_namespace_anonymous;
                auto opened=ObjectDirectory::open_entry(session(),ObjectDirectory::path_key(create.path),&create,security_factory(attr.security));if(opened.status)return finish(opened.status);
                const auto granted=descriptor_access(opened.object,desired);
                return finish(install(std::move(opened.object),granted,attr.flags&2U,call(0)));
            }
            const auto rooted=rooted_object_name(runtime_,attr,true);
            auto found=lookup_object_namespace(rooted,true);
            if(!found.status){
                // OBJ_OPENIF：同種類的既有項目（含核心既有目錄）直接開啟並回 STATUS_OBJECT_NAME_EXISTS。
                const bool builtin_directory=!link && !found.link && found.builtin!=UINT32_MAX;
                const bool same=link?found.link && !found.builtin_link:(!found.link && (found.entry || builtin_directory));
                if(!(attr.flags&0x80U) || !same)return finish(0xc0000035U);
                auto existing=builtin_directory?ObjectDirectory::open(session(),found.builtin,[&]{return synchronization_descriptor(memory,0).bytes;}):found.entry;
                const auto granted=descriptor_access(existing,desired);
                if(const auto status=install(std::move(existing),granted,attr.flags&2U,call(0)))return finish(status);
                return finish(0x40000000U);
            }
            if(found.status!=0xc0000034U)return finish(found.status);
            if(found.parent_entry && found.parent_entry->private_namespace())return finish(0xc0000024U);
            if(const auto status=parent_allows(found,link?4U:8U))return finish(status);
            create.path=found.path;
            auto opened=ObjectDirectory::open_entry(session(),ObjectDirectory::path_key(found.path),&create,security_factory(attr.security));if(opened.status)return finish(opened.status);
            if(opened.existed){
                // 其他程序在解析與建立之間建立同名項目。
                if(!(attr.flags&0x80U) || opened.object->kind()!=create.kind)return finish(0xc0000035U);
                const auto granted=descriptor_access(opened.object,desired);
                if(const auto status=install(std::move(opened.object),granted,attr.flags&2U,call(0)))return finish(status);
                return finish(0x40000000U);
            }
            const auto granted=descriptor_access(opened.object,desired);
            return finish(install(std::move(opened.object),granted,attr.flags&2U,call(0)));
        }
        if(name.ends_with("OpenSymbolicLinkObject")){
            if(!call(0))return finish(0xc0000005U);memory.check(GuestAddress{call(0)},unit,Access::Write);
            ObjectAttributes attr;if(const auto status=read_attributes(call(2),false,attr))return finish(status);
            if(attr.name.empty())return finish(attr.root?0xc0000024U:0xc000003bU);
            const auto rooted=rooted_object_name(runtime_,attr,false);
            auto found=lookup_object_namespace(rooted,true);
            if(found.status)return finish(found.status);
            if(!found.link)return finish(0xc0000024U);
            auto object=found.entry;
            if(found.builtin_link){
                ObjectNamespaceEntry create;create.kind=ObjectNamespaceKind::SymbolicLink;create.flags=object_namespace_builtin|object_namespace_permanent;
                create.path=found.path;create.target=found.target;
                auto opened=ObjectDirectory::open_entry(session(),ObjectDirectory::path_key(found.path),&create,[&]{return synchronization_descriptor(memory,0).bytes;});
                if(opened.status)return finish(opened.status);
                if(!opened.object->symbolic_link())return finish(0xc0000024U);
                if(const auto status=opened.object->update_target(found.target))return finish(status);
                object=std::move(opened.object);
            }
            const auto granted=descriptor_access(object,word(1));
            return finish(install(std::move(object),granted,attr.flags&2U,call(0)));
        }
        if(name.ends_with("QuerySymbolicLinkObject")){
            // (LinkHandle, UNICODE_STRING* LinkTarget, ULONG* ReturnedLength)
            const auto handle=runtime_.object_directories.find(call(0));
            if(handle==runtime_.object_directories.end())return finish(object_type(call(0)).empty()?0xc0000008U:0xc0000024U);
            if(!handle->second.object->symbolic_link())return finish(0xc0000024U);
            if(!(handle->second.access&1U))return finish(0xc0000022U);
            const auto target=std::u16string(handle->second.object->target());const auto bytes=target.size()*2;
            memory.check(GuestAddress{call(1)},x86_?8U:16U,Access::Write);
            const auto maximum=memory.load(GuestAddress{call(1)+2},16),buffer=memory.load(GuestAddress{call(1)+unit},bits);
            if(maximum<bytes){if(call(2))memory.store(GuestAddress{call(2)},32,bytes+2);return finish(0xc0000023U);}
            if(bytes){const auto data=utf16_bytes(target);memory.write(GuestAddress{buffer},std::span<const std::uint8_t>(data).first(bytes));}
            std::uint32_t returned=static_cast<std::uint32_t>(bytes);
            if(maximum>=bytes+2){memory.store(GuestAddress{buffer+bytes},16,0);returned+=2;}
            memory.store(GuestAddress{call(1)},16,bytes);
            if(call(2))memory.store(GuestAddress{call(2)},32,returned);
            return finish(0);
        }
        if(name.ends_with("QueryDirectoryObject")){
            // (DirectoryHandle, Buffer, Length, ReturnSingleEntry, RestartScan, Context*, ReturnLength*)
            const auto handle=runtime_.object_directories.find(call(0));
            if(handle==runtime_.object_directories.end())return finish(object_type(call(0)).empty()?0xc0000008U:0xc0000024U);
            if(handle->second.object->symbolic_link())return finish(0xc0000024U);
            if(!(handle->second.access&1U))return finish(0xc0000022U);
            if(!call(5))return finish(0xc0000005U);
            memory.check(GuestAddress{call(5)},4,Access::Write);
            const auto items=object_directory_listing(*handle->second.object);
            const std::uint32_t start=(word(4)&0xffU)?0U:static_cast<std::uint32_t>(memory.load(GuestAddress{call(5)},32));
            const bool single=(word(3)&0xffU)!=0;const auto length=word(2);const std::size_t entry=x86_?16U:32U;
            if(start>=items.size()){if(call(6))memory.store(GuestAddress{call(6)},32,0);return finish(0x8000001aU);}
            std::size_t used=entry,count=0;
            for(std::size_t n=start;n<items.size();++n){
                const auto need=entry+(items[n].first.size()+1)*2+(items[n].second.size()+1)*2;
                if(used+need>length)break;used+=need;++count;if(single)break;
            }
            if(!count){
                if(call(6))memory.store(GuestAddress{call(6)},32,2*entry+(items[start].first.size()+1)*2+(items[start].second.size()+1)*2);
                return finish(0xc0000023U);
            }
            memory.check(GuestAddress{call(1)},used,Access::Write);memory.write(GuestAddress{call(1)},std::vector<std::uint8_t>(used));
            std::uint64_t text=call(1)+(count+1)*entry;
            const auto put=[&](std::uint64_t at,const std::u16string& value){
                const auto data=utf16_bytes(value);memory.write(GuestAddress{text},data);
                memory.store(GuestAddress{at},16,data.size()-2);memory.store(GuestAddress{at+2},16,data.size());memory.store(GuestAddress{at+unit},bits,text);text+=data.size();};
            for(std::size_t n=0;n<count;++n){const auto at=call(1)+n*entry;put(at,items[start+n].first);put(at+entry/2,items[start+n].second);}
            memory.store(GuestAddress{call(5)},32,start+count);
            if(call(6))memory.store(GuestAddress{call(6)},32,used);
            return finish(single || start+count==items.size()?0U:0x105U);
        }
        if(name.ends_with("MakePermanentObject")){
            if(!token_privilege(16))return finish(0xc0000061U);
            const auto handle=runtime_.object_directories.find(call(0));
            if(handle==runtime_.object_directories.end())return finish(object_type(call(0)).empty()?0xc0000008U:0xc00000bbU);
            return finish(handle->second.object->set_permanent(true));
        }
        if(name.ends_with("CreatePrivateNamespace") || name.ends_with("OpenPrivateNamespace")){
            // (Handle*, DesiredAccess, ObjectAttributes*, BoundaryDescriptor*)
            const bool creating=name.ends_with("CreatePrivateNamespace");
            if(!call(0))return finish(0xc0000005U);memory.check(GuestAddress{call(0)},unit,Access::Write);
            ObjectAttributes attr;if(const auto status=read_attributes(call(2),true,attr))return finish(status);
            if(!attr.name.empty() || attr.root)return finish(0xc000000dU);
            BoundaryDescriptor boundary;if(const auto status=read_boundary(memory,call(3),boundary))return finish(status);
            // 邊界的每個 SID 都必須是呼叫端權杖中已啟用的使用者或群組；完整性標籤不得高於呼叫端。
            initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());
            for(const auto& sid:boundary.sids){
                const bool member=token.user.sid==sid || std::any_of(token.groups.begin(),token.groups.end(),[&](const auto& group){return group.sid==sid && (group.attributes&4U) && !(group.attributes&0x10U);});
                if(!member)return finish(0xc0000022U);
            }
            if(!boundary.label.empty()){std::uint32_t rid;std::memcpy(&rid,boundary.label.data()+8,4);if(rid>token.integrity)return finish(0xc0000022U);}
            const auto key=u"\x01"+boundary.key();
            if(!creating){
                auto opened=ObjectDirectory::open_entry(session(),key,nullptr,{});if(opened.status)return finish(opened.status);
                const auto granted=descriptor_access(opened.object,word(1));
                return finish(install(std::move(opened.object),granted,attr.flags&2U,call(0)));
            }
            ObjectNamespaceEntry create;create.serial=namespace_random();create.path=private_namespace_path(create.serial);create.target=boundary.key();create.flags=object_namespace_private;
            auto opened=ObjectDirectory::open_entry(session(),key,&create,security_factory(attr.security));if(opened.status)return finish(opened.status);
            if(opened.existed)return finish(0xc0000035U);
            const auto granted=descriptor_access(opened.object,word(1));
            return finish(install(std::move(opened.object),granted,attr.flags&2U,call(0)));
        }
        if(name.ends_with("DeletePrivateNamespace")){
            const auto handle=runtime_.object_directories.find(call(0));
            if(handle==runtime_.object_directories.end())return finish(object_type(call(0)).empty()?0xc0000008U:0xc0000024U);
            if(!handle->second.object->private_namespace())return finish(0xc0000024U);
            if(!(handle->second.access&0x10000U))return finish(0xc0000022U);
            return finish(handle->second.object->remove_name());
        }
        // 邊界描述元：Rtl 版回指標或 NTSTATUS，kernel32 版回 HANDLE／BOOL 並設定 last error。
        const auto boundary_bytes=[&](const BoundaryDescriptor& value){
            std::vector<std::uint8_t> out(16);std::uint32_t items=0;
            const auto append=[&](std::uint32_t type,std::span<const std::uint8_t> data){
                const auto size=static_cast<std::uint32_t>(8+data.size());const auto at=out.size();out.resize(at+((size+3U)&~3U));
                std::memcpy(out.data()+at,&type,4);std::memcpy(out.data()+at+4,&size,4);std::memcpy(out.data()+at+8,data.data(),data.size());++items;};
            append(1,std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.name.data()),value.name.size()*2));
            for(const auto& sid:value.sids)append(2,sid);
            if(!value.label.empty())append(3,value.label);
            const std::uint32_t version=1,total=static_cast<std::uint32_t>(out.size());
            std::memcpy(out.data(),&version,4);std::memcpy(out.data()+4,&items,4);std::memcpy(out.data()+8,&total,4);std::memcpy(out.data()+12,&value.flags,4);
            return out;
        };
        const auto create_boundary=[&](std::u16string text,std::uint32_t flags)->std::uint64_t{
            BoundaryDescriptor value;value.name=std::move(text);value.flags=flags;const auto bytes=boundary_bytes(value);
            const auto block=allocate_heap(memory,bytes.size());if(!block)return 0;memory.write(GuestAddress{block},bytes);return block;
        };
        if(name=="RtlCreateBoundaryDescriptor" || name.starts_with("CreateBoundaryDescriptor")){
            std::u16string text;
            if(name=="RtlCreateBoundaryDescriptor"){if(!call(0))return finish(0);text=native_string(memory,call(0),x86_);}
            else if(call(0))text=name.ends_with('W')?read_utf16(memory,GuestAddress{call(0)},0x8000):utf8_to_utf16(read_ansi(memory,call(0),0x8000));
            if(text.empty() || (word(1)&~1U)){if(!native)last_error_=87;return finish(0);}
            const auto block=create_boundary(std::move(text),word(1));
            if(!block && !native)last_error_=8;
            return finish(block);
        }
        if(name=="RtlDeleteBoundaryDescriptor" || name=="DeleteBoundaryDescriptor"){if(call(0))(void)free_heap(memory,call(0),state);return finish(0);}
        if(name=="RtlAddSIDToBoundaryDescriptor" || name=="AddSIDToBoundaryDescriptor" || name=="RtlAddIntegrityLabelToBoundaryDescriptor" || name=="AddIntegrityLabelToBoundaryDescriptor"){
            // (BoundaryDescriptor**, PSID)
            const bool label=name.find("IntegrityLabel")!=std::string_view::npos;
            const auto done=[&](std::uint32_t status){if(native)return finish(status);if(status){last_error_=win32_error(status);return finish(0);}return finish(1);};
            if(!call(0))return done(0xc000000dU);
            memory.check(GuestAddress{call(0)},unit,Access::Write);const auto pointer=memory.load(GuestAddress{call(0)},bits);
            BoundaryDescriptor value;if(const auto status=read_boundary(memory,pointer,value))return done(status);
            std::vector<std::uint8_t> sid;try{sid=security::read_sid(memory,call(1));}catch(const SecurityDescriptorError&){return done(0xc000000dU);}
            if(label){if(!integrity_label_sid(sid) || !value.label.empty())return done(0xc000000dU);value.label=std::move(sid);}
            else value.sids.push_back(std::move(sid));
            const auto bytes=boundary_bytes(value);
            const auto block=reallocate_heap(memory,pointer,bytes.size(),state);if(!block)return done(0xc0000017U);
            memory.write(GuestAddress{block},bytes);memory.store(GuestAddress{call(0)},bits,block);return done(0);
        }
        if(name.starts_with("CreatePrivateNamespace") || name.starts_with("OpenPrivateNamespace")){
            // CreatePrivateNamespace(SECURITY_ATTRIBUTES*, Boundary, Alias)、OpenPrivateNamespace(Boundary, Alias)
            const bool creating=name.starts_with("Create");const unsigned base=creating?1U:0U;const bool wide=name.ends_with('W');
            if(!call(base+1))return fail(87,0);
            const auto alias=wide?read_utf16(memory,GuestAddress{call(base+1)},0x8000):utf8_to_utf16(read_ansi(memory,call(base+1),0x8000));
            if(alias.empty() || alias.find(u'\\')!=alias.npos)return fail(87,0);
            GuestScratch block(*this,memory,state);const auto out=block.allocate(8),attributes=block.allocate(x86_?24U:48U);if(!out || !attributes)return fail(8,0);
            memory.store(GuestAddress{attributes},32,x86_?24U:48U);
            if(creating && call(0)){
                const auto security=read_security_attributes(memory,call(0),x86_,false,true);
                memory.store(GuestAddress{attributes+3U*unit},32,security.inherit?2U:0U);
                memory.store(GuestAddress{attributes+4U*unit},bits,memory.load(GuestAddress{call(0)+unit},bits));
            }
            const auto status=static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll",creating?"NtCreatePrivateNamespace":"NtOpenPrivateNamespace",{out,0xf000fU,attributes,call(base)}));
            if(status)return fail(win32_error(status),0);
            const auto handle=memory.load(GuestAddress{out},bits);const auto object=runtime_.object_directories.find(handle);
            if(object!=runtime_.object_directories.end())private_namespace_aliases_[namespace_fold(alias)]={handle,object->second.object};
            last_error_=saved_error;return finish(handle);
        }
        if(name=="ClosePrivateNamespace"){
            // (Handle, Flags)：PRIVATE_NAMESPACE_FLAG_DESTROY（1）同時刪除命名空間名稱。
            const auto handle=runtime_.object_directories.find(call(0));
            if(handle==runtime_.object_directories.end() || !handle->second.object->private_namespace())return fail(6,0);
            if(word(1)&~1U)return fail(87,0);
            if(word(1)&1U){if(!(handle->second.access&0x10000U))return fail(5,0);(void)handle->second.object->remove_name();}
            const auto object=handle->second.object;
            for(auto alias=private_namespace_aliases_.begin();alias!=private_namespace_aliases_.end();)
                alias=alias->second.first==call(0)?private_namespace_aliases_.erase(alias):std::next(alias);
            runtime_.object_directories.erase(handle);runtime_.handle_flags.erase(call(0));
            last_error_=saved_error;return finish(1);
        }
    }catch(const NativeError& error){return native?finish(error.status):fail(win32_error(error.status),0);}
    catch(const SecurityDescriptorError& error){return native?finish(nt_status(error.code)):fail(error.code,0);}
    catch(const MemoryFault&){return native?finish(0xc0000005U):fail(998,0);}
    catch(const std::bad_alloc&){return native?finish(0xc0000017U):fail(8,0);}
    throw HostCallFault("未支援的物件命名空間服務："+std::string(name));
}
