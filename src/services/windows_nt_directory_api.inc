// BaseGetNamedObjectDirectory 與 NtOpenDirectoryObject 共用真正的目錄物件及存取檢查。
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
    const auto equal=[&](std::u16string_view a,std::u16string_view b){return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[&](char16_t x,char16_t y){return attributes&0x40U?windows_path_upper(x)==windows_path_upper(y):x==y;});};
    if(equal(text,u"\\Sessions\\1\\BaseNamedObjects\\Global"))text=builtin_object_directories[4];
    else if(equal(text,u"\\Sessions\\1\\BaseNamedObjects\\Local"))text=builtin_object_directories[3];
    const auto found=std::find_if(builtin_object_directories.begin(),builtin_object_directories.end(),[&](auto candidate){return equal(text,candidate);});
    if(found==builtin_object_directories.end())return finish(0xc0000034U);
    if(runtime_.object_directories.size()>=process_handle_limit)return finish(0xc0000017U);
    try{
        auto directory=ObjectDirectory::open(mapping_session(),static_cast<std::uint32_t>(found-builtin_object_directories.begin()),[&]{return synchronization_descriptor(memory,0).bytes;});
        const auto granted=object_security_access(shared_object_security(directory->descriptor()),desired,{0x20001U,0x2000cU,0x20002U,0xf000fU});
        const auto handle=runtime_.allocate_wait_handle();runtime_.object_directories.emplace(handle,ObjectDirectoryHandle{std::move(directory),granted});
        if(attributes&2U)runtime_.handle_flags[handle]=1;
        if(base)runtime_.named_object_directory=handle;
        memory.store(GuestAddress{call(0)},bits,handle);return finish(0);
    }catch(const SecurityDescriptorError& error){return finish(nt_status(error.code));}
}
