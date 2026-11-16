// 與一般檔案共用 HostFile、FILE_OBJECT 分享、鎖與實際資料 I/O。
file_streams::Directory ReadOnlyFiles::stream_directory(const HostFile& base,bool create) const {
#if !defined(_WIN32)
    const auto sandbox=volumes_->open_root('C');FileDescriptor root{sandbox.descriptor};
    if(sandbox.error)return {-1,sandbox.error};
    return file_streams::directory(root.value,base.security_file().descriptor_,create);
#else
    (void)base;(void)create;return {-1,50};
#endif
}
OpenHostFile ReadOnlyFiles::open_stream(const file_streams::Path& path,std::uint32_t access,std::uint32_t disposition,std::uint32_t flags,std::uint32_t sharing,const std::function<std::vector<std::uint8_t>()>& creation) {
#if !defined(_WIN32)
    if(!writable_)return {{},{},{},50,false};
    if(disposition<1 || disposition>5 || (sharing&~7U))return {{},{},{},87,false};
    const bool readable=access&(0x80000000U|1U),writing=access&(0x40000000U|2U|4U),deleting=flags&0x04000000U;
    if((disposition==2 || disposition==5) && !writing)return {{},{},{},87,false};
    if(deleting)access|=0x10000U;
    const bool create=disposition==1 || disposition==2 || disposition==4;
    // 具名串流的資料分享各自獨立；基底 lease 只加入跨串流的刪除分享限制。
    auto base=open_host(path.base,0,3,0x02000000U,3U|(sharing&4U));
    if(base.error==2 && create){
        auto made=open_host(path.base,0x40000000U,4,flags&0xffffU,7,creation);if(made.error)return made;
        made.file.reset();base=open_host(path.base,0,3,0x02000000U,3U|(sharing&4U));
    }
    if(base.error)return {base.key,{},{},base.error,false};
    if(base.file->readonly_){
        // 主映像的唯讀 pin 是資料保護；別名（GUID／GLOBALROOT）不能因 key 不同而失去原映射權限。
        bool writable_source=false;
        for(const auto& [guest,host]:readonly_files_){
            struct stat source{};if(::stat(host.c_str(),&source)<0)continue;
            if(static_cast<std::uint64_t>(source.st_dev)==base.metadata.device && source.st_ino==base.metadata.id && writable_image_source(guest,host)){writable_source=true;break;}
        }
        if(!writable_source)return {base.key,{},{},5,false};
    }
    if((writing || deleting) && (base.metadata.attributes&1U))return {base.key,{},{},5,false};
    if(const auto code=authorize(base.file->descriptor_,access))return {base.key,{},{},code,false};
    auto directory=std::make_shared<file_streams::Directory>(stream_directory(*base.file,create));
    if(directory->error)return {base.key,{},{},directory->error,false};
    int native_flags=writing?(readable?O_RDWR:O_WRONLY):O_RDONLY;if(flags&0x80000000U)native_flags|=O_SYNC;
    const auto opened=file_streams::open(*directory,path.name,native_flags,create,disposition==1);FileDescriptor data{opened.descriptor};
    const auto key=base.key+":"+path.name+":$DATA";
    if(opened.error)return {key,{},{},opened.error,opened.existed};
    const auto storage=HostFile::descriptor_information(data.value);if(storage.error)return {key,{},{},storage.error,opened.existed};
    if(const auto code=sharing_error(storage.metadata,access,sharing))return {key,{},{},code,opened.existed};
    auto file=std::make_shared<HostFile>(data.value);data.value=-1;file->access_=access;file->sharing_=sharing;
    file->state_=file_state(storage.metadata,true);file->stream_base_=std::move(base.file);file->stream_directory_=std::move(directory);file->volume_=file->stream_base_->volume_;
    std::erase_if(file->state_->opens,[](const auto& weak){return weak.expired();});file->state_->opens.push_back(file);
    if(deleting){
        auto target=std::make_shared<HostDeleteTarget>();target->directory=::fcntl(file->stream_directory_->descriptor,F_DUPFD_CLOEXEC,0);if(target->directory<0)return {key,{},{},native_error(),opened.existed};
        struct stat parent{};if(::fstat(target->directory,&parent)<0)return {key,{},{},native_error(),opened.existed};
        target->leaf=file_streams::storage_key(path.name);target->device=storage.metadata.device;target->inode=storage.metadata.id;target->parent_device=static_cast<std::uint64_t>(parent.st_dev);target->parent_inode=parent.st_ino;
        file->state_->targets.push_back(target);file->delete_target_=std::move(target);file->delete_on_close_=true;
    }
    if(const auto code=attach_shared_file(*file))return {key,{},{},code,opened.existed};
    if(disposition==2 || disposition==5)if(const auto code=file->truncate(0))return {key,{},{},code,opened.existed};
    const auto visible=file->information();return {key,std::move(file),visible.metadata,visible.error,opened.existed};
#else
    (void)path;(void)access;(void)disposition;(void)flags;(void)sharing;(void)creation;return {{},{},{},50,false};
#endif
}
DirectorySnapshot ReadOnlyFiles::streams(std::string path){
    const auto parsed=file_streams::parse(path);if(parsed.error)return {{},parsed.error};
    auto base=open_host(parsed.base,0x80U,3,0x02000000U,7);if(base.error)return {{},base.error};return streams(*base.file);
}
DirectorySnapshot ReadOnlyFiles::streams(const HostFile& file){
    const auto& base=file.security_file();const auto info=base.information();if(info.error)return {{},info.error};
    // 已開啟的 FILE_OBJECT 查詢 FileStreamInformation 不再要求額外存取權。
    DirectorySnapshot result;if(!(info.metadata.attributes&0x10U))result.entries.push_back({"::$DATA",info.metadata});
    auto directory=stream_directory(base,false);if(directory.error==2)return result;if(directory.error)return {{},directory.error};
    const auto named=file_streams::list(directory);if(named.error)return {{},named.error};
    for(const auto& entry:named.entries){auto details=info.metadata;details.size=entry.size;details.allocation_size=entry.allocation;details.attributes&=~0x10U;result.entries.push_back({":"+entry.name+":$DATA",details});}return result;
}
