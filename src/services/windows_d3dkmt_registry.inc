// KMTQAITYPE_QUERYREGISTRY 的兩層狀態、原生對齊及可變輸出契約。
// 所有值與檔案均來自 sandbox；沒有外部驅動套件時，路徑查詢回報實際缺少資料。
const auto request=arg(0);
const unsigned unit=x86_?4U:8U;
const auto data=memory.load(GuestAddress{request+8},x86_?32:64);
const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{request+8+unit},32));
if(!data || capacity<552)return finish(0xc000000dU);
memory.check(GuestAddress{data},552,Access::Read);memory.check(GuestAddress{data+540},4,Access::Write);
const auto failure=[&](std::uint32_t status)->std::optional<std::uint32_t>{memory.store(GuestAddress{data+540},32,2);return finish(status);};
const auto query=static_cast<std::uint32_t>(memory.load(GuestAddress{data},32));
const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{data+4},32));
const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{data+528},32));
if(query>3 || flags&~1U || memory.load(GuestAddress{data+532},32) ||
   (query>=2 && (type || flags)) || (query<2 && type!=1 && type!=2 && type!=3 && type!=4 && type!=7 && type!=11) ||
   ((flags&1U) && type!=1 && type!=2 && type!=7))return failure(0xc000000dU);
std::u16string value_name;
if(query<2){
    bool terminated=false;
    for(unsigned i=0;i<260;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{data+8+i*2},16));if(!c){terminated=true;break;}value_name.push_back(c);}
    if(!terminated)return failure(0xc000000dU);
}
if(!trace_services_.empty() && (trace_services_=="*" || trace_services_.find(name)!=trace_services_.npos))
    std::fprintf(stderr,"[d3dkmt_registry] query=%u flags=%u type=%u value=%s capacity=%u\n",query,flags,type,utf16_to_utf8(value_name).c_str(),capacity);
refresh_device_configuration(false);
const auto adapter=device_configuration_->nodes.find(2);
if(adapter==device_configuration_->nodes.end() || !adapter->second.present)return failure(0xc000000eU);
const auto& node=adapter->second;
const auto adapter_path=device_key_path(node,true);
const auto service_path=std::u16string(1,2)+u"\\SYSTEM\\CurrentControlSet\\Services\\"+node.service;
RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
if(transaction.error)return failure(transaction.error==5?0xc0000022U:0xc000014cU);
restore_app_hives(runtime_.registry);
std::uint32_t lookup_status=0xc0000034U;
const auto lookup=[&](std::u16string root,std::u16string name,bool defaults)->std::optional<RegistryValue>{
    const auto split=name.find_last_of(u'\\');
    if(split!=name.npos){root+=u"\\"+name.substr(0,split);name.erase(0,split+1);defaults=false;}
    root=registry_text_fold(std::move(root));name=registry_text_fold(std::move(name));
    try{if((registry_security_access(root,1)&1U)==0){lookup_status=0xc0000022U;return {};}}
    catch(const SecurityDescriptorError&){lookup_status=0xc0000022U;return {};}
    if(const auto key=runtime_.registry.keys.find(root);key!=runtime_.registry.keys.end())
        if(const auto value=key->second.find(name);value!=key->second.end())return value->second;
    if(defaults){
        if(const auto value=node.software.find(name);value!=node.software.end())return value->second;
        for(const auto& [key,value]:display_identity::driver_registry_values(property_text(node,1),gpu_memory_stats().limit))
            if(registry_text_fold(key)==name)return value;
    }
    return {};
};
const auto string_value=[](const RegistryValue& value)->std::optional<std::u16string>{
    if((value.type!=1 && value.type!=2) || value.bytes.size()%2)return {};
    std::u16string text;for(std::size_t i=0;i<value.bytes.size();i+=2){const auto c=static_cast<char16_t>(value.bytes[i]|(unsigned(value.bytes[i+1])<<8));if(!c)return text;text.push_back(c);}return {};
};
std::vector<std::uint8_t> output;
if(query<2){
    const auto value=lookup(query?adapter_path:service_path,value_name,query==1);
    if(!value)return failure(lookup_status);
    if(value->type!=type)return failure(0xc0000024U);
    // 全部驅動資料已在客體命名空間，TranslatePath 不需跨宿主 DriverStore 轉換。
    output=value->bytes;
}else{
    std::u16string path;
    if(query==2){
        const auto inf=lookup(adapter_path,u"InfPath",false);if(!inf)return failure(lookup_status);
        const auto text=string_value(*inf);if(!text)return failure(0xc000003eU);
        try{path=driver_store::parent_of(driver_store::stored_inf(filesystem_,*text,false));}
        catch(const driver_store::Error& error){return failure(error.code==5?0xc0000022U:0xc0000034U);}
    }else{
        const auto image=lookup(service_path,u"ImagePath",false);if(!image)return failure(lookup_status);
        const auto text=string_value(*image);if(!text)return failure(0xc000003eU);
        try{path=expand_guest_environment(*text,runtime_.environment);}catch(const std::length_error&){return failure(0xc0000106U);}
        if(path.size()>1 && path.front()==u'"' && path.back()==u'"')path=path.substr(1,path.size()-2);
        const auto root=runtime_.environment.find(u"SYSTEMROOT");const auto windows=root==runtime_.environment.end()?std::u16string(u"C:\\Windows"):root->second.second;
        const std::u16string_view prefix=u"\\SystemRoot\\";
        if(registry_text_fold(path).starts_with(registry_text_fold(std::u16string(prefix))))path=windows+u"\\"+path.substr(prefix.size());
        else if(registry_text_fold(path).starts_with(u"system32\\"))path=windows+u"\\"+path;
        try{path=normalized_windows_path(std::move(path),runtime_.current_directory);}catch(const std::invalid_argument&){return failure(0xc000000dU);}
        const auto file=filesystem_.information(utf16_to_utf8(path));if(file.error || (file.metadata.attributes&0x10U))return failure(file.error==5?0xc0000022U:0xc0000034U);
    }
    output=utf16_bytes(path);
}
if(output.size()>UINT32_MAX)return failure(0xc000009aU);
memory.check(GuestAddress{data+536},8,Access::Write);
if(output.size()>capacity-544){memory.store(GuestAddress{data+536},32,output.size());memory.store(GuestAddress{data+540},32,1);return finish(0);}
memory.check(GuestAddress{data+544},output.size(),Access::Write);memory.write(GuestAddress{data+544},output);
memory.store(GuestAddress{data+536},32,output.size());memory.store(GuestAddress{data+540},32,0);return finish(0);
