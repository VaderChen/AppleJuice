// CM 與 SetupAPI 共用的介面屬性；資料留在 sandbox registry，不修改宿主裝置設定。
using InterfaceProperties=std::map<DevicePropertyKey,UnifiedDeviceProperty>;
constexpr std::array<std::uint8_t,16> interface_property_format{0x6e,0x51,0x6e,0x02,0x14,0xb8,0x4b,0x41,0x83,0xcd,0x85,0x6d,0x6f,0xef,0x48,0x22};
constexpr DevicePropertyKey interface_friendly{interface_property_format,2},interface_enabled{interface_property_format,3},interface_class{interface_property_format,4};
constexpr DevicePropertyKey interface_instance{{0xc8,0x4f,0xc3,0x78,0x4a,0x10,0xca,0x4a,0x9e,0xa4,0x52,0x4d,0x52,0x99,0x6e,0x57},256};
constexpr DevicePropertyKey interface_name{{0x30,0xf1,0x25,0xb7,0xef,0x47,0x1a,0x10,0xa5,0xf1,0x02,0x60,0x8c,0x9e,0xeb,0xac},10};
bool readonly_interface_property(const DevicePropertyKey& key){return key==interface_enabled || key==interface_class || key==interface_instance || key==interface_name;}
std::u16string interface_property_path(const DeviceNode& node,const WindowsGuid& guid){
    // 與既有 Device Parameters 同一介面命名空間；型別資料另外保存，避免 REG_* 與 DEVPROP_* 混用。
    auto instance=node.port_name.empty()?node.id:node.port_name;std::replace(instance.begin(),instance.end(),u'\\',u'#');
    return folded(std::u16string(1,2)+u"\\system\\currentcontrolset\\control\\deviceclasses\\"+guid_name(guid)+u"\\"+instance+u"\\#\\properties");
}
std::u16string interface_property_value_name(const DevicePropertyKey& key){return guid_name(key.format)+u"."+utf8_to_utf16(std::to_string(key.id));}
std::optional<DevicePropertyKey> interface_property_key_name(std::u16string_view name){
    if(name.size()<40 || name[38]!=u'.')return {};
    const auto guid=parse_windows_guid(name.substr(0,38),true);if(!guid)return {};
    std::uint64_t id=0;for(const auto c:name.substr(39)){if(c<u'0' || c>u'9')return {};id=id*10+static_cast<unsigned>(c-u'0');if(id>UINT32_MAX)return {};}
    DevicePropertyKey key{*guid,static_cast<std::uint32_t>(id)};return valid_device_property_key(key)?std::optional{key}:std::nullopt;
}
bool valid_device_property_value(const UnifiedDeviceProperty& value){
    const auto type=value.type,base=type&0xfffU,modifier=type&0xf000U;const auto& bytes=value.bytes;
    if(type&~0x3fffU || base>25 || bytes.size()>65534)return false;
    if(base<2)return !modifier && bytes.empty();
    constexpr std::array<unsigned,26> widths{0,0,1,1,2,2,4,4,8,8,4,8,16,16,8,8,8,1,0,0,0,20,4,4,4,0};
    const auto width=widths[base];
    if(width)return modifier==0?bytes.size()==width:modifier==0x1000 && bytes.size()%width==0;
    if(base==19){
        if(modifier)return false;
        try{(void)security::parse_relative_descriptor(bytes);return true;}catch(const SecurityDescriptorError&){return false;}
    }
    if(modifier && !(modifier==0x2000 && (base==18 || base==20)))return false;
    if(bytes.size()<2 || bytes.size()%2)return false;
    const auto zero=[&](std::size_t at){return bytes[at]==0 && bytes[at+1]==0;};
    if(!zero(bytes.size()-2))return false;
    if(!modifier){for(std::size_t at=0;at+2<bytes.size();at+=2)if(zero(at))return false;return true;}
    if(bytes.size()<4 || !zero(bytes.size()-4))return false;
    // 空清單為兩個 NUL；其他清單不允許結尾之前提早終止。
    for(std::size_t at=0;at+4<bytes.size();at+=2)if(zero(at) && (at==0 || zero(at+2)))return false;
    return true;
}
InterfaceProperties interface_properties(const DeviceNode& node,const WindowsGuid& guid,const RegistryState& registry,std::uint32_t& error){
    const auto friendly=property_text(node,13);
    InterfaceProperties result{
        {interface_friendly,{18,utf16_bytes(friendly.empty()?node.id:friendly)}},
        {interface_enabled,{17,{static_cast<std::uint8_t>(node.present?0xff:0)}}},
        {interface_class,{13,{guid.begin(),guid.end()}}},{interface_instance,{18,utf16_bytes(node.id)}}};
    if(const auto stored=registry.keys.find(interface_property_path(node,guid));stored!=registry.keys.end()){
        for(const auto& [name,entry]:stored->second){
            const auto key=interface_property_key_name(name);
            if(!key || entry.type!=3 || entry.bytes.size()<4){error=13;return {};}
            if(readonly_interface_property(*key))continue;
            std::uint32_t type=0;for(unsigned i=0;i<4;++i)type|=std::uint32_t{entry.bytes[i]}<<(8*i);
            UnifiedDeviceProperty property{type,{entry.bytes.begin()+4,entry.bytes.end()}};
            if(!valid_device_property_value(property)){error=13;return {};}
            if(type)result.insert_or_assign(*key,std::move(property));else result.erase(*key);
        }
    }
    if(const auto name=result.find(interface_friendly);name!=result.end() && name->second.type==18)result.emplace(interface_name,name->second);
    return result;
}
std::uint32_t save_interface_property(RegistryState& registry,RegistryTransaction& transaction,const DeviceNode& node,const WindowsGuid& guid,
                                    const DevicePropertyKey& key,const UnifiedDeviceProperty& property,std::size_t limit){
    const auto before=registry;const auto path=interface_property_path(node,guid),name=interface_property_value_name(key);
    const bool tombstone=key==interface_friendly; // 刪除預設名稱後，不得在下次盤點時重新產生。
    if(!property.type && !tombstone){
        const auto found=registry.keys.find(path);if(found==registry.keys.end() || !found->second.contains(name))return 1168;
        registry.bytes-=found->second.at(name).bytes.size();--registry.values;found->second.erase(name);
    }else{
        RegistryValue value;value.type=3;value.display_name=name;
        for(unsigned i=0;i<4;++i)value.bytes.push_back(static_cast<std::uint8_t>(property.type>>(8*i)));
        value.bytes.insert(value.bytes.end(),property.bytes.begin(),property.bytes.end());
        for(std::size_t end=1;;end=path.find(u'\\',end+1)){registry.keys.try_emplace(path.substr(0,end));if(end==path.npos)break;}
        auto& values=registry.keys[path];if(const auto old=values.find(name);old!=values.end()){registry.bytes-=old->second.bytes.size();--registry.values;}
        if(registry.keys.size()>4096 || registry.values>=4096 || registry.bytes+value.bytes.size()>limit){registry=before;return 8;}
        registry.bytes+=value.bytes.size();++registry.values;values.insert_or_assign(name,std::move(value));
    }
    if(const auto error=transaction.commit();error){registry=before;return error;}return 0;
}
