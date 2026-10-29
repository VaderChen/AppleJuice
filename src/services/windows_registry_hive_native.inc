std::u16string WindowsServices::registry_object_path(const GuestMemory& memory,std::uint64_t address,bool file){
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(!address || memory.load(GuestAddress{address},32)!=6*unit)throw registry_hive::Error{87};
    memory.check(GuestAddress{address},6*unit,Access::Read);
    const auto pointer=[&](unsigned offset){return memory.load(GuestAddress{address+offset*unit},bits);};
    const auto root=pointer(1),name=pointer(2);if(!name || (pointer(3)&~0x42U) || pointer(4) || pointer(5))throw registry_hive::Error{87};
    const auto length=memory.load(GuestAddress{name},16),maximum=memory.load(GuestAddress{name+2},16),buffer=memory.load(GuestAddress{name+unit},bits);
    if(!length || (length&1U) || length>maximum || !buffer)throw registry_hive::Error{87};
    std::u16string value;for(std::uint64_t offset=0;offset<length;offset+=2){const auto c=static_cast<char16_t>(memory.load(GuestAddress{buffer+offset},16));if(!c)throw registry_hive::Error{123};value+=c;}
    if(file){
        if(root){
            const auto handle=files_.find(root);if(handle==files_.end())throw registry_hive::Error{6};
            if(!(handle->second.metadata.attributes&0x10U) || value.starts_with(u"\\") || value.find(u':')!=value.npos)throw registry_hive::Error{87};
            auto path=handle->second.path;const auto identity=handle->second.native?handle->second.native:handle->second.identity;
            if(identity){const auto current=filesystem_.final_path(*identity,path);if(current.error)throw registry_hive::Error{current.error};path=current.key;}
            return utf8_to_utf16(path)+u"\\"+value;
        }
        const auto folded=registry_hive::fold(value);
        if(folded.starts_with(u"\\??\\") || folded.starts_with(u"\\dosdevices\\")){
            value.erase(0,folded.starts_with(u"\\??\\")?4:12);
            if(registry_hive::fold(value).starts_with(u"unc\\"))value=u"\\\\"+value.substr(4);
            else if(windows_dos_path_type(value)!=WindowsDosPathType::DriveAbsolute)value=u"\\\\?\\"+value;
        }else if(folded.starts_with(u"\\device\\")){
            bool found=false;for(char16_t drive=u'A';drive<=u'Z';++drive){const auto device=windows_device_name(drive);if(folded.starts_with(registry_hive::fold(device)+u"\\")){value=std::u16string{drive,u':'}+value.substr(device.size());found=true;break;}}if(!found)throw registry_hive::Error{123};
        }else if(windows_dos_path_type(value)!=WindowsDosPathType::DriveAbsolute)throw registry_hive::Error{123};
        return value;
    }
    std::u16string path;
    if(root){const auto key=runtime_.registry.handles.find(root);if(key==runtime_.registry.handles.end())throw registry_hive::Error{6};if(!runtime_.registry.keys.contains(key->second.path))throw registry_hive::Error{1018};
        if(value.starts_with(u"\\"))throw registry_hive::Error{123};path=registry_logical_path(key->second)+u"\\"+value;
    }else{
        const auto folded=registry_hive::fold(value);constexpr std::u16string_view machine=u"\\registry\\machine\\",users=u"\\registry\\user\\";
        if(folded.starts_with(machine))path=std::u16string(1,2)+u"\\"+value.substr(machine.size());
        else if(folded.starts_with(users))path=std::u16string(1,3)+u"\\"+value.substr(users.size());else throw registry_hive::Error{123};
    }
    if(path.size()<3 || path.back()==u'\\' || path.find(u"\\\\")!=path.npos)throw registry_hive::Error{123};
    return path;
}
