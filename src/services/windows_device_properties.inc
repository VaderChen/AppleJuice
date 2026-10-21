// 與舊版 CM_DRP／SPDRP 共用來源；DEVPROPKEY 依 Windows 的 GUID + DWORD 排列。
struct DevicePropertyKey {std::array<std::uint8_t,16> format{};std::uint32_t id{};};
struct UnifiedDeviceProperty {std::uint32_t type{};std::vector<std::uint8_t> bytes;};
DevicePropertyKey read_device_property_key(const GuestMemory& memory,std::uint64_t pointer){
    DevicePropertyKey key;memory.read(GuestAddress{pointer},key.format);
    key.id=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+16},32));return key;
}
bool valid_device_property_key(const DevicePropertyKey& key){
    return key.id>=2 && std::any_of(key.format.begin(),key.format.end(),[](auto byte){return byte!=0;});
}
std::optional<UnifiedDeviceProperty> device_property(const DeviceNode& node,const DevicePropertyKey& key,const std::map<std::uint32_t,DeviceNode>& nodes){
    static constexpr std::array<std::uint8_t,16> legacy{0x4e,0x25,0x5c,0xa4,0x1c,0xdf,0xfd,0x4e,0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0};
    static constexpr std::array<std::uint8_t,16> instance{0xc8,0x4f,0xc3,0x78,0x4a,0x10,0xca,0x4a,0x9e,0xa4,0x52,0x4d,0x52,0x99,0x6e,0x57};
    static constexpr std::array<std::uint8_t,16> state{0xc5,0xa6,0x40,0x43,0xfa,0x93,0x06,0x47,0x97,0x2c,0x7b,0x64,0x80,0x08,0xa5,0xa7};
    static constexpr std::array<std::uint8_t,16> present{0x7e,0x94,0x0b,0x54,0x40,0x8b,0xbc,0x45,0xa8,0xa2,0x6a,0x0b,0x89,0x4c,0xbd,0xa2};
    static constexpr std::array<std::uint8_t,16> driver{0xdd,0x65,0xb8,0xa8,0x3d,0x2e,0x94,0x40,0xad,0x97,0xe5,0x93,0xa7,0x0c,0x75,0xd6};
    const auto text=[](std::u16string_view value){return UnifiedDeviceProperty{18,utf16_bytes(value)};};
    const auto number=[](std::uint32_t value){std::vector<std::uint8_t> bytes;for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>(value>>shift));return UnifiedDeviceProperty{7,std::move(bytes)};};
    if(key.format==legacy){
        if(key.id==10 && !node.class_guid.empty())return UnifiedDeviceProperty{13,{node.guid.begin(),node.guid.end()}};
        const auto property=node.properties.find(key.id-1);if(property==node.properties.end())return {};
        const auto& value=property->second.second;
        const auto type=value.type==1?18U:value.type==7?0x2012U:value.type==4?7U:0U;
        if(type)return UnifiedDeviceProperty{type,value.bytes};
    }else if(key.format==instance && key.id==256)return text(node.id);
    else if(key.format==state){
        if(key.id==2)return number(node.status);
        if(key.id==3)return number(node.problem);
        if(key.id==8 && node.parent && nodes.contains(node.parent))return text(nodes.at(node.parent).id);
    }else if(key.format==present && key.id==5)return UnifiedDeviceProperty{17,{static_cast<std::uint8_t>(node.present?0xff:0)}};
    else if(key.format==driver && node.guid==display_identity::class_guid){
        if(key.id==3)return text(display_identity::version);
        if(key.id==9)return text(display_identity::provider);
    }
    return {};
}
