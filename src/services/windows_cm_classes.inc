// CM 類別函式：與 SetupDi 類別共用 Control\Class 與 DeviceClasses 的登錄資料（上方已同步裝置節點的類別）。
const auto class_guid=[&](std::uint64_t pointer){WindowsGuid guid{};memory.read(GuestAddress{pointer},guid);return guid;};
const auto class_key=[&](bool interface_kind,const WindowsGuid& guid){return (interface_kind?interface_root:root)+u"\\"+folded(windows_guid_text(guid));};
const auto encode=[&](std::u16string_view text){std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(text);else {const auto ansi=utf16_to_ansi(std::u16string(text));bytes.assign(ansi.begin(),ansi.end());bytes.push_back(0);}return bytes;};
const auto value_of=[&](const std::u16string& path,std::u16string_view value_name)->const RegistryValue*{
    const auto key=registry.keys.find(path);if(key==registry.keys.end())return nullptr;const auto found=key->second.find(std::u16string(value_name));return found==key->second.end()?nullptr:&found->second;
};
if(name=="CM_Enumerate_Classes"){
    // (ulClassIndex, ClassGuid, ulFlags)：CM_ENUMERATE_CLASSES_INSTALLER／INTERFACE；超過結尾回 CR_NO_SUCH_VALUE。
    const auto kind=static_cast<std::uint32_t>(arg(2));if(!arg(1))return finish(3);if(kind>1)return finish(4);
    const auto prefix=(kind?interface_root:root)+u"\\";std::vector<WindowsGuid> list;
    for(const auto& [path,entries]:registry.keys){
        (void)entries;if(!path.starts_with(prefix))continue;const auto tail=std::u16string_view(path).substr(prefix.size());
        if(tail.find(u'\\')!=tail.npos)continue;if(const auto guid=parse_windows_guid(tail,true))list.push_back(*guid);
    }
    const auto index=static_cast<std::uint32_t>(arg(0));if(index>=list.size())return finish(37);
    memory.write(GuestAddress{arg(1)},list[index]);return finish(0);
}
if(name.starts_with("CM_Get_Class_Key_Name")){
    // (ClassGuid, pszKeyName, pulLength, ulFlags)：類別鍵名稱即大寫的 {GUID}；長度以字元（A 為位元組）計並含結尾 NUL。
    if(!arg(0) || !arg(2))return finish(3);if(arg(3))return finish(4);
    const auto text=windows_guid_text(class_guid(arg(0)));const auto bytes=encode(text);const auto required=wide?bytes.size()/2:bytes.size();
    const auto capacity=memory.load(GuestAddress{arg(2)},32);memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,required);
    if(!arg(1) || capacity<required)return finish(26);memory.write(GuestAddress{arg(1)},bytes);return finish(0);
}
if(name.starts_with("CM_Open_Class_Key")){
    // (ClassGuid, pszClassName, samDesired, Disposition, phkClass, ulFlags)：HKLM 類別鍵對未提升的帳號只開放讀取，
    // 與 CM_Open_DevNode_Key 相同；建立新類別鍵需要寫入權，回 CR_ACCESS_DENIED。
    if(!arg(4))return finish(3);const auto kind=static_cast<std::uint32_t>(arg(5)),disposition=static_cast<std::uint32_t>(arg(3)),access=static_cast<std::uint32_t>(arg(2));
    if(kind>1)return finish(4);if(disposition>1)return finish(31);memory.check(GuestAddress{arg(4)},x86_?4:8,Access::Write);
    const auto path=arg(0)?class_key(kind==1,class_guid(arg(0))):kind==1?interface_root:root;
    if(!registry.keys.contains(path))return finish(disposition==1?46U:51U);
    const auto requested=access&~0x02000300U;if(requested&~0x20019U)return finish(51);
    if(registry.handles.size()>=4096)return finish(2);
    const auto token=registry.next_handle++;registry.handles.emplace(token,RegistryHandle{path,(access&0x02000000U)?0x20019U:requested,registry_view(access,x86_),path});
    memory.store(GuestAddress{arg(4)},x86_?32:64,token);return finish(0);
}
if(name.starts_with("CM_Get_Class_Registry_Property") || name.starts_with("CM_Set_Class_Registry_Property")){
    // Get：(ClassGuid, ulProperty, pulRegDataType, Buffer, pulLength, ulFlags, hMachine)；Set：(ClassGuid, ulProperty, Buffer, ulLength, ulFlags, hMachine)。
    // 篩選器在類別鍵，其餘在 Properties 子鍵；SECURITY_SDS 與 SECURITY 共用同一個二進位值，以 SDDL 文字交換。
    const bool set=name.starts_with("CM_Set");if(!machine_valid(arg(set?5:6)))return finish(49);
    if(!arg(0) || (!set && !arg(4)))return finish(3);if(arg(set?4:5))return finish(4);
    const auto property=static_cast<std::uint32_t>(arg(1));std::u16string_view value_name;bool nested=true;std::uint32_t type=4;
    switch(property){case 0x12:value_name=u"upperfilters";nested=false;type=7;break;case 0x13:value_name=u"lowerfilters";nested=false;type=7;break;
        case 0x18:case 0x19:value_name=u"security";type=3;break;case 0x1a:value_name=u"devicetype";break;case 0x1b:value_name=u"exclusive";break;case 0x1c:value_name=u"devicecharacteristics";break;default:return finish(53);}
    const auto base=class_key(false,class_guid(arg(0)));if(!registry.keys.contains(base))return finish(46);
    const auto path=nested?base+u"\\properties":base;
    if(!set){
        const auto* value=value_of(path,value_name);if(!value)return finish(37);
        std::vector<std::uint8_t> bytes;std::uint32_t reported=value->type;
        if(property==0x19){bytes=encode(utf8_to_utf16(sddl_text(security::parse_relative_descriptor(value->bytes),0xfU,machine_account_domain())));reported=1;}
        else bytes=property_bytes(*value,wide);
        const auto capacity=memory.load(GuestAddress{arg(4)},32);memory.check(GuestAddress{arg(4)},4,Access::Write);if(arg(2))memory.check(GuestAddress{arg(2)},4,Access::Write);
        memory.store(GuestAddress{arg(4)},32,bytes.size());if(arg(2))memory.store(GuestAddress{arg(2)},32,reported);
        if(!arg(3) || capacity<bytes.size())return finish(26);memory.write(GuestAddress{arg(3)},bytes);return finish(0);
    }
    if(!administrator())return finish(51);
    const auto length=static_cast<std::uint32_t>(arg(3));std::optional<RegistryValue> data;
    if(arg(2) && length){
        std::vector<std::uint8_t> bytes(length);memory.read(GuestAddress{arg(2)},bytes);RegistryValue value;value.type=type;
        if(type==7){
            if(wide){if(length%2)return finish(31);value.bytes=std::move(bytes);}
            else {value.bytes=utf16_bytes(utf8_to_utf16(std::string(bytes.begin(),bytes.end())));value.bytes.resize(value.bytes.size()-2);}
            if(value.bytes.size()<4 || value.bytes[value.bytes.size()-1] || value.bytes[value.bytes.size()-2] || value.bytes[value.bytes.size()-3] || value.bytes[value.bytes.size()-4])return finish(31);
        }else if(property==0x19){
            std::u16string text;if(wide){for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(bytes[i+1]<<8));if(!c)break;text.push_back(c);}}else text=utf8_to_utf16(std::string(reinterpret_cast<const char*>(bytes.data()),strnlen(reinterpret_cast<const char*>(bytes.data()),bytes.size())));
            try{value.bytes=sddl_descriptor(utf16_to_utf8(text),machine_account_domain());}catch(const SddlError&){return finish(31);}
        }else if(type==3){try{(void)security::parse_relative_descriptor(bytes);}catch(...){return finish(31);}value.bytes=std::move(bytes);}
        else {if(length!=4)return finish(31);value.bytes=std::move(bytes);}
        value.display_name=std::u16string(value_name);data=std::move(value);
    }
    const auto previous=registry;
    if(data){
        for(std::size_t end=1;;){registry.keys.try_emplace(path.substr(0,end));end=path.find(u'\\',end+1);if(end==path.npos){registry.keys.try_emplace(path);break;}}
        auto& entries=registry.keys[path];const auto old=entries.find(std::u16string(value_name));
        if(old!=entries.end()){registry.bytes-=old->second.bytes.size();entries.erase(old);}else ++registry.values;
        registry.bytes+=data->bytes.size();entries.emplace(std::u16string(value_name),std::move(*data));
        if(registry.bytes>output_limit || registry.values>4096 || registry.keys.size()>4096){registry=previous;return finish(2);}
    }else if(const auto key=registry.keys.find(path);key!=registry.keys.end()){
        if(const auto old=key->second.find(std::u16string(value_name));old!=key->second.end()){registry.bytes-=old->second.bytes.size();--registry.values;key->second.erase(old);}
        else return finish(0);
    }else return finish(0);
    if(transaction.commit()){registry=previous;return finish(29);}
    return finish(0);
}
if(name.starts_with("CM_Get_Class_Property")){
    // Get：(ClassGUID, PropertyKey, PropertyType, PropertyBuffer, PropertyBufferSize, ulFlags)；Keys：(ClassGUID, PropertyKeyArray, PropertyKeyCount, ulFlags)。
    // CM_CLASS_PROPERTY_INSTALLER 讀 DEVPKEY_DeviceClass_*，INTERFACE 讀 DEVPKEY_DeviceInterfaceClass_*；值都來自同一個類別鍵。
    const bool keys=name=="CM_Get_Class_Property_Keys";const auto kind=static_cast<std::uint32_t>(arg(keys?3:5));if(kind>1)return finish(4);
    if(!arg(0) || !arg(2) || (!keys && (!arg(1) || !arg(4))))return finish(3);
    const bool interface_kind=kind==1;const auto path=class_key(interface_kind,class_guid(arg(0)));if(!registry.keys.contains(path))return finish(46);
    static constexpr std::array<std::uint8_t,16> device_class_format{0xfc,0xbf,0x9a,0x25,0xa7,0x50,0xce,0x47,0xaf,0x08,0x68,0xc9,0xa7,0xd7,0x33,0x66};
    static constexpr std::array<std::uint8_t,16> interface_class_format{0x99,0x3a,0xc8,0x14,0x3f,0x0b,0xb7,0x44,0xbe,0x4c,0xa1,0x78,0xd3,0x99,0x05,0x64};
    const auto& format=interface_kind?interface_class_format:device_class_format;
    const auto text_property=[&](const RegistryValue* value,std::uint32_t type)->std::optional<UnifiedDeviceProperty>{
        if(!value || (value->type!=1 && value->type!=2 && value->type!=7) || value->bytes.size()<2)return {};return UnifiedDeviceProperty{type,value->bytes};
    };
    const auto lookup=[&](std::uint32_t pid)->std::optional<UnifiedDeviceProperty>{
        const auto nested=path+u"\\properties";
        if(interface_kind)return pid==3?text_property(value_of(path,u""),18):std::nullopt;
        switch(pid){
        case 2:{const auto* value=value_of(path,u"");return text_property(value?value:value_of(path,u"class"),18);}
        case 3:return text_property(value_of(path,u"class"),18);
        case 4:return text_property(value_of(path,u"icon"),18);
        case 5:return text_property(value_of(path,u"installer32"),18);
        case 6:return text_property(value_of(path,u"enumproppages32"),18);
        case 7:case 8:case 9:case 10:{constexpr std::array<std::u16string_view,4> flags_names{u"noinstallclass",u"nodisplayclass",u"silentinstall",u"nouseclass"};
            if(!value_of(path,flags_names[pid-7]))return {};return UnifiedDeviceProperty{17,{0xff}};}
        case 11:return text_property(value_of(path,u"default service"),18);
        case 12:return text_property(value_of(path,u"iconpath"),0x2012);
        case 19:return text_property(value_of(path,u"upperfilters"),0x2012);
        case 20:return text_property(value_of(path,u"lowerfilters"),0x2012);
        case 25:{const auto* value=value_of(nested,u"security");if(!value)return {};return UnifiedDeviceProperty{0x13,value->bytes};}
        case 26:{const auto* value=value_of(nested,u"security");if(!value)return {};return UnifiedDeviceProperty{0x14,utf16_bytes(utf8_to_utf16(sddl_text(security::parse_relative_descriptor(value->bytes),0xfU,machine_account_domain())))};}
        case 27:case 29:{const auto* value=value_of(nested,pid==27?u"devicetype":u"devicecharacteristics");if(!value || value->bytes.size()!=4)return {};return UnifiedDeviceProperty{7,value->bytes};}
        case 28:{const auto* value=value_of(nested,u"exclusive");if(!value || value->bytes.size()!=4)return {};return UnifiedDeviceProperty{17,{static_cast<std::uint8_t>(std::any_of(value->bytes.begin(),value->bytes.end(),[](auto b){return b!=0;})?0xff:0)}};}
        default:return {};
        }
    };
    if(keys){
        std::vector<std::uint8_t> list;std::uint32_t count=0;
        for(const auto pid:{2U,3U,4U,5U,6U,7U,8U,9U,10U,11U,12U,19U,20U,25U,26U,27U,28U,29U})if(lookup(pid)){list.insert(list.end(),format.begin(),format.end());for(unsigned shift=0;shift<32;shift+=8)list.push_back(static_cast<std::uint8_t>(pid>>shift));++count;}
        const auto capacity=memory.load(GuestAddress{arg(2)},32);if(!arg(1) && capacity)return finish(3);memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,count);
        if(!arg(1) || capacity<count)return finish(26);memory.write(GuestAddress{arg(1)},list);return finish(0);
    }
    const auto capacity=memory.load(GuestAddress{arg(4)},32);if(!arg(3) && capacity)return finish(3);
    const auto key=read_device_property_key(memory,arg(1));if(!valid_device_property_key(key))return finish(53);
    const auto property=key.format==format?lookup(key.id):std::nullopt;if(!property)return finish(37);
    memory.check(GuestAddress{arg(2)},4,Access::Write);memory.check(GuestAddress{arg(4)},4,Access::Write);
    const bool fits=arg(3) && capacity>=property->bytes.size();if(fits)memory.check(GuestAddress{arg(3)},property->bytes.size(),Access::Write);
    memory.store(GuestAddress{arg(2)},32,property->type);memory.store(GuestAddress{arg(4)},32,property->bytes.size());
    if(!fits)return finish(26);memory.write(GuestAddress{arg(3)},property->bytes);return finish(0);
}
throw HostCallFault("尚未實作 CM 類別服務："+std::string(name));
