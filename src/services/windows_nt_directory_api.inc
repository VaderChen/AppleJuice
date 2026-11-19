// BaseGetNamedObjectDirectory 與 NtOpenDirectoryObject 共用真正的目錄物件及存取檢查。
// NtOpenDirectoryObject 經物件命名空間解析：中間與最後的符號連結（Global、Local、\??、使用者建立的連結）都會跟隨，
// 內建目錄沿用既有紀錄，NtCreateDirectoryObject 建立的動態目錄開啟同一份共享紀錄。
if(name=="BaseGetNamedObjectDirectory" || name=="NtOpenDirectoryObject"){
    const bool base=name=="BaseGetNamedObjectDirectory";
    memory.check(GuestAddress{call(0)},unit,Access::Write);
    if(base && runtime_.object_directories.contains(runtime_.named_object_directory)){
        memory.store(GuestAddress{call(0)},bits,runtime_.named_object_directory);return finish(0);
    }
    auto text=std::u16string(builtin_object_directories[3]);std::uint32_t attributes=0,desired=6;
    if(!base){
        const auto attr=object_attributes(memory,call(2),x86_);attributes=attr.flags;desired=static_cast<std::uint32_t>(call(1));
        text=rooted_object_name(runtime_,attr,false);
    }
    const auto found=lookup_object_namespace(text,false);
    if(found.status)return finish(found.status);
    if(found.link || (found.builtin==UINT32_MAX && (!found.entry || found.entry->symbolic_link())))return finish(0xc0000024U);
    if(runtime_.object_directories.size()>=process_handle_limit)return finish(0xc0000017U);
    try{
        auto directory=found.entry?found.entry:ObjectDirectory::open(mapping_session(),found.builtin,[&]{return synchronization_descriptor(memory,0).bytes;});
        const auto granted=object_security_access(shared_object_security(directory->descriptor()),desired,directory->generic_mapping());
        const auto handle=runtime_.allocate_wait_handle();runtime_.object_directories.emplace(handle,ObjectDirectoryHandle{std::move(directory),granted});
        if(attributes&2U)runtime_.handle_flags[handle]=1;
        if(base)runtime_.named_object_directory=handle;
        memory.store(GuestAddress{call(0)},bits,handle);return finish(0);
    }catch(const SecurityDescriptorError& error){return finish(nt_status(error.code));}
}
