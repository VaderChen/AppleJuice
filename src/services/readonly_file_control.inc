// 與 readonly_files.cpp 共用原生 descriptor、inode registry 及 DOS 屬性處理。
FileInformation ReadOnlyFiles::final_path(const HostFile& file,std::string fallback) const {
#if defined(_WIN32)
    (void)file;(void)fallback;return {{},{},50};
#else
    const auto info=file.information();if(info.error)return info;std::array<char,4096> buffer{};
#if defined(__APPLE__)
    if(::fcntl(file.descriptor_,F_GETPATH,buffer.data())<0)return {{},{},native_error()};const std::filesystem::path native{buffer.data()};
#else
    const auto name="/proc/self/fd/"+std::to_string(file.descriptor_);const auto length=::readlink(name.c_str(),buffer.data(),buffer.size()-1);if(length<0)return {{},{},native_error()};const std::filesystem::path native{std::string(buffer.data(),static_cast<std::size_t>(length))};
#endif
    std::string guest;std::size_t matched=0;
    const auto mount=[&](const std::filesystem::path& root,std::string prefix){
        if(root.empty())return;const auto relative=native.lexically_relative(root);if(relative.empty() || *relative.begin()==".." || root.native().size()<matched)return;
        matched=root.native().size();guest=std::move(prefix);if(relative!=".")guest+=relative.generic_string();
    };
    const auto state=volume_state();if(state.error)return {{},{},state.error};
    const auto volume_prefix=[&](char id){auto prefix=windows_volume_path(id);std::replace(prefix.begin(),prefix.end(),'\\','/');const auto paths=volume_mount_paths(id);if(!paths.error && !paths.paths.empty()){prefix=paths.paths.front();for(const auto& path:paths.paths)if(path.size()==3){prefix=path;break;}}return prefix;};
    if(!relative_volume_)mount(root_,"C:/");
    for(const auto& volume:volumes())if(!file.volume_ || file.volume_==volume.id)mount(volume.host_path,volume_prefix(volume.id));
    if(!file.volume_)mount(install_root_,volume_prefix('C')+"Program Files/");
    const auto overlay_prefix=[&](std::string prefix){if(prefix.size()>2 && prefix[1]==':'){char id=prefix[0];if(id>='a' && id<='z')id=static_cast<char>(id-32);const auto known=volumes();if(std::any_of(known.begin(),known.end(),[&](const auto& volume){return volume.id==id;}))prefix=volume_prefix(id)+prefix.substr(3);}return prefix;};
    for(const auto& [prefix,path]:application_sources_)mount(path,overlay_prefix(prefix));
    for(const auto& [prefix,path]:readonly_mounts_)mount(path,overlay_prefix(prefix));
    for(const auto& [key,path]:readonly_files_)if(path==native && path.native().size()>=matched){matched=path.native().size();guest=overlay_prefix(key);}
    if(guest.empty()){
        auto resolved=resolve(fallback);FileDescriptor fd{resolved.descriptor};struct stat current{};
        if(resolved.error || ::fstat(fd.value,&current)<0 || static_cast<std::uint64_t>(current.st_dev)!=info.metadata.device || static_cast<std::uint64_t>(current.st_ino)!=info.metadata.id)return {{},{},3};
        guest=resolved.key;if(guest.starts_with("relative:/"))guest="C:/"+guest.substr(10);
    }
    if(guest.size()<3 || (guest[1]!=':' && !windows_volume_letter(std::string_view(guest))))return {{},{},3};if(guest[1]==':' && guest[0]>='a' && guest[0]<='z')guest[0]=static_cast<char>(guest[0]-32);return {guest,info.metadata,0};
#endif
}
OpenHostFile ReadOnlyFiles::reopen(const HostFile& original,std::string path,std::uint32_t access,std::uint32_t sharing,std::uint32_t flags) try {
    const auto current=final_path(original,std::move(path));if(current.error)return {current.key,{},{},current.error,false};
#if defined(_WIN32)
    (void)access;(void)sharing;(void)flags;return {current.key,{},{},50,true};
#else
    const bool deleting=flags&0x04000000U;if(deleting)access|=0x10000U;
    if(original.readonly_ || !writable_){
        constexpr std::uint32_t allowed=0x801200a9U;if((access&~allowed) || deleting)return {current.key,{},{},5,true};
        if(const auto error=authorize(original.descriptor_,access))return {current.key,{},{},error,true};
        if(original.state_)if(const auto error=sharing_error(current.metadata,access,sharing))return {current.key,{},{},error,true};FileDescriptor fd{::fcntl(original.descriptor_,F_DUPFD_CLOEXEC,0)};if(fd.value<0)return {current.key,{},{},native_error(),true};
        auto file=std::make_shared<HostFile>(fd.value);fd.value=-1;file->state_=original.state_;file->readonly_=true;file->volume_=original.volume_;file->access_=access;file->sharing_=sharing;if(file->state_){if(const auto error=attach_shared_file(*file))return {current.key,{},{},error,true};file->state_->opens.push_back(file);}return {current.key,std::move(file),current.metadata,0,true};
    }
    auto opened=open_host(current.key,access,3,flags&~0x04000000U,sharing);if(opened.error)return opened;
    if(opened.metadata.device!=current.metadata.device || opened.metadata.id!=current.metadata.id)return {current.key,{},{},2,true};
    if(deleting){if(const auto error=set_disposition(*opened.file,current.key,true))return {current.key,{},{},error,true};(void)set_disposition(*opened.file,current.key,false);opened.file->delete_on_close_=true;if(const auto error=attach_shared_file(*opened.file))return {current.key,{},{},error,true};}
    return opened;
#endif
}catch(const std::bad_alloc&){return {{},{},{},8,false};}
std::uint32_t HostFile::set_basic(std::uint64_t created,std::uint64_t accessed,std::uint64_t modified,std::uint64_t changed,std::uint32_t attributes) const {
    if(null_device_)return 1;
    if(readonly_)return 5;if(attributes&~0x31b7U || created==UINT64_MAX || accessed==UINT64_MAX || modified==UINT64_MAX || changed==UINT64_MAX)return 50;
#if defined(_WIN32)
    (void)created;(void)accessed;(void)modified;(void)changed;return 50;
#else
    struct stat info{};if(::fstat(descriptor_,&info)<0)return native_error();
    if(changed)return 50; // inode change time 由宿主檔案系統管理，不能略過要求。
    if(attributes)if(const auto error=set_descriptor_attributes(descriptor_,info,attributes))return error;return set_times(created,accessed,modified);
#endif
}
std::uint32_t HostFile::copy_template_metadata(int destination,bool directory,std::uint32_t attributes) const {
#if defined(__APPLE__) || defined(__linux__)
    const auto source=information();if(source.error)return source.error;
    if(static_cast<bool>(source.metadata.attributes&0x10U)!=directory)return 267;
    if(source.metadata.attributes&(0x800U|0x4000U))return 50;
    const auto list=[&](char* bytes,std::size_t size){
#if defined(__APPLE__)
        return ::flistxattr(descriptor_,bytes,size,0);
#else
        return ::flistxattr(descriptor_,bytes,size);
#endif
    };
    auto length=list(nullptr,0);
    if(length<0 && errno!=ENOTSUP)return native_error();
    if(length>65536)return 8;
    std::vector<char> names(length>0?static_cast<std::size_t>(length):0);
    if(length>0){length=list(names.data(),names.size());if(length<0)return native_error();names.resize(static_cast<std::size_t>(length));}
    std::size_t total=0;
    for(std::size_t at=0;at<names.size();) {
        const auto end=std::find(names.begin()+static_cast<std::ptrdiff_t>(at),names.end(),'\0');if(end==names.end())return 13;
        const auto name=std::string_view(names.data()+at,static_cast<std::size_t>(end-(names.begin()+static_cast<std::ptrdiff_t>(at))));at+=name.size()+1;
        // 僅複製 Windows EA／串流 payload；DACL、reparse、有效資料長度及宿主安全性中繼資料各自保留契約。
        if(!name.starts_with("com.x64arm.ea.") && !name.starts_with("user.x64arm.ea.") &&
           !name.starts_with("com.x64arm.stream.") && !name.starts_with("user.x64arm.stream.") && name!="com.apple.ResourceFork")continue;
        const auto get=[&](void* bytes,std::size_t size){
#if defined(__APPLE__)
            return ::fgetxattr(descriptor_,name.data(),bytes,size,0,0);
#else
            return ::fgetxattr(descriptor_,name.data(),bytes,size);
#endif
        };
        const auto size=get(nullptr,0);if(size<0)return native_error();
        if(static_cast<std::uint64_t>(size)>16U*1024U*1024U-total)return 8;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));const auto received=get(bytes.data(),bytes.size());
        if(received!=size)return received<0?(errno==ERANGE?1237U:native_error()):1237U;
#if defined(__APPLE__)
        const auto stored=::fsetxattr(destination,name.data(),bytes.data(),bytes.size(),0,0);
#else
        const auto stored=::fsetxattr(destination,name.data(),bytes.data(),bytes.size(),0);
#endif
        if(stored<0)return native_error();total+=bytes.size();
    }
    struct stat target{};if(::fstat(destination,&target)<0)return native_error();
    return set_descriptor_attributes(destination,target,(source.metadata.attributes|attributes)&0x31b7U);
#else
    (void)destination;(void)directory;(void)attributes;return 50;
#endif
}
std::uint32_t HostFile::allocate(std::uint64_t size) const {
    if(null_device_)return 1;
    if(readonly_)return 5;if(size>INT64_MAX)return 87;const auto info=information();if(info.error)return info.error;if(info.metadata.attributes&0x10U)return 5;
    if(size<info.metadata.size)return truncate(size);if(size==info.metadata.allocation_size)return 0;
#if defined(__APPLE__)
    // 配置大小縮小但不小於檔案大小：Windows 會釋放多出的配置；宿主無法在不截斷的情況下歸還已預配區塊，
    // 多保留的配置對客體不可見（檔案大小與內容不變），因此照 Windows 契約回報成功。
    if(size<info.metadata.allocation_size)return 0;
    fstore_t request{};request.fst_flags=F_ALLOCATEALL;request.fst_posmode=F_PEOFPOSMODE;request.fst_length=static_cast<off_t>(size-info.metadata.allocation_size);
    return ::fcntl(descriptor_,F_PREALLOCATE,&request)<0?native_error():0;
#else
    return 50;
#endif
}
std::uint32_t ReadOnlyFiles::set_disposition(HostFile& file,std::string path,bool deleting) try {
    if(!writable_ || file.readonly_)return 5;if(!file.state_)return 6;
    if(!deleting){if(file.state_->shared)if(const auto error=file.state_->shared->disposition(0,false,file.transfer_identity()))return error;if(file.delete_target_)file.delete_target_->pending=false;file.state_->pending=std::any_of(file.state_->targets.begin(),file.state_->targets.end(),[](const auto& target){return target->pending;});return 0;}
    const auto current=final_path(file,std::move(path));if(current.error)return current.error;if(current.metadata.attributes&1U)return 5;
#if defined(_WIN32)
    return 50;
#else
    if(file.state_->shared){if(const auto error=file.state_->shared->sharing(0x10000U,7,file.transfer_identity(),true))return error;}
    for(const auto& weak:file.state_->opens)if(const auto peer=weak.lock())if(peer.get()!=&file && (!file.transfer_identity() || peer->transfer_identity()!=file.transfer_identity()) && !(peer->sharing_&4U))return 32;
    if(current.metadata.attributes&0x10U){const auto copy=::fcntl(file.descriptor_,F_DUPFD_CLOEXEC,0);if(copy<0)return native_error();std::unique_ptr<DIR,decltype(&::closedir)> directory(::fdopendir(copy),::closedir);if(!directory){::close(copy);return native_error();}while(const auto* entry=::readdir(directory.get()))if(std::string_view(entry->d_name)!="." && std::string_view(entry->d_name)!="..")return 145;}
    std::string leaf;auto parent=resolve(current.key,true,&leaf);FileDescriptor directory{parent.descriptor};if(parent.error)return parent.error;
    struct stat identity{},parent_info{};if(::fstatat(directory.value,leaf.c_str(),&identity,AT_SYMLINK_NOFOLLOW)<0 || ::fstat(directory.value,&parent_info)<0)return native_error();
    if(static_cast<std::uint64_t>(identity.st_dev)!=current.metadata.device || static_cast<std::uint64_t>(identity.st_ino)!=current.metadata.id)return 2;if(::faccessat(directory.value,".",W_OK|X_OK,0)<0)return native_error();
    if(!file.delete_target_){auto target=std::make_shared<HostDeleteTarget>();target->leaf=leaf;target->directory=::fcntl(directory.value,F_DUPFD_CLOEXEC,0);if(target->directory<0)return native_error();target->device=current.metadata.device;target->inode=current.metadata.id;target->parent_device=static_cast<std::uint64_t>(parent_info.st_dev);target->parent_inode=static_cast<std::uint64_t>(parent_info.st_ino);target->is_directory=(current.metadata.attributes&0x10U)!=0;file.state_->targets.push_back(target);file.delete_target_=std::move(target);}
    else if(file.delete_target_->leaf!=leaf || file.delete_target_->parent_device!=static_cast<std::uint64_t>(parent_info.st_dev) || file.delete_target_->parent_inode!=static_cast<std::uint64_t>(parent_info.st_ino)){
        // 取消刪除後，宿主也可能更名；重新設定時固定目前仍屬於原 inode 的項目。
        FileDescriptor updated{::fcntl(directory.value,F_DUPFD_CLOEXEC,0)};if(updated.value<0)return native_error();std::swap(file.delete_target_->directory,updated.value);file.delete_target_->leaf.swap(leaf);file.delete_target_->parent_device=static_cast<std::uint64_t>(parent_info.st_dev);file.delete_target_->parent_inode=static_cast<std::uint64_t>(parent_info.st_ino);
    }
    if(file.state_->shared){const auto info=file.information();if(info.error)return info.error;auto [index,error]=file.state_->shared->target(file.delete_target_->directory,file.delete_target_->leaf,info.metadata,file.delete_target_->is_directory);if(error)return error;file.delete_target_->shared_index=index;if(const auto code=file.state_->shared->disposition(index,true,file.transfer_identity()))return code;}
    file.delete_target_->pending=true;file.state_->pending=true;return 0;
#endif
}catch(const std::bad_alloc&){return 8;}

OpenHostFile ReadOnlyFiles::pin(std::string path) const try {
    auto resolved=resolve(std::move(path));
#if defined(_WIN32)
    return {resolved.key,{},{},50,false};
#else
    FileDescriptor fd{resolved.descriptor};if(resolved.error)return {resolved.key,{},{},resolved.error,false};struct stat info{};if(::fstat(fd.value,&info)<0)return {resolved.key,{},{},native_error(),false};if(!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))return {resolved.key,{},{},5,false};
    auto file=std::make_shared<HostFile>(fd.value);fd.value=-1;file->readonly_=true;return {resolved.key,file,file->information().metadata,0,true};
#endif
}catch(const std::bad_alloc&){return {{},{},{},8,false};}
