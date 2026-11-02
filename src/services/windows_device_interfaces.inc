            const auto write_interface=[&](std::uint64_t pointer,std::uint64_t token)->bool{
                if(!pointer || memory.load(GuestAddress{pointer},32)!=structure_size)return false;
                const auto& item=info.interfaces.at(token);const auto& node=nodes.at(item.node);memory.check(GuestAddress{pointer},structure_size,Access::Write);
                memory.write(GuestAddress{pointer+4},item.guid);memory.store(GuestAddress{pointer+20},32,node.present?1:4);
                memory.store(GuestAddress{pointer+24},x86_?32:64,token);return true;
            };
            const auto read_interface=[&](std::uint64_t pointer)->std::optional<std::uint64_t>{
                if(!pointer || memory.load(GuestAddress{pointer},32)!=structure_size)return {};
                const auto token=memory.load(GuestAddress{pointer+24},x86_?32:64);
                if(!info.interfaces.contains(token))return {};std::array<std::uint8_t,16> guid;
                memory.read(GuestAddress{pointer+4},guid);return guid==info.interfaces.at(token).guid?std::optional{token}:std::nullopt;
            };
            if(name=="SetupDiEnumDeviceInterfaces"){
                if(!arg(2))return failed(87);std::array<std::uint8_t,16> guid;memory.read(GuestAddress{arg(2)},guid);
                const auto filter=arg(1)?read_node(arg(1)):std::nullopt;if(arg(1) && !filter)return failed(87);
                if(!arg(4) || memory.load(GuestAddress{arg(4)},32)!=structure_size)return failed(1784);
                std::uint32_t index=0;
                for(const auto& [token,item]:info.interfaces){
                    if(item.guid!=guid || (filter && item.node!=*filter))continue;
                    if(index++==arg(3)){(void)write_interface(arg(4),token);return finish(1);}
                }return failed(259);
            }
            if(name=="SetupDiOpenDeviceInterfaceA" || name=="SetupDiOpenDeviceInterfaceW"){
                if(arg(2)&~1U)return failed(1004);if(!arg(1))return failed(87);const auto path=folded(read(arg(1)));
                const auto found_node=std::find_if(nodes.begin(),nodes.end(),[&](const auto& entry){return !entry.second.interface_path.empty() && folded(entry.second.interface_path)==path;});
                if(found_node==nodes.end())return failed(0xe0000225U);
                const auto handle=found_node->first;
                if(std::find(info.nodes.begin(),info.nodes.end(),handle)==info.nodes.end()){
                    if(arg(2)&1)return failed(0xe0000225U);
                    if(info.filtered && info.class_guid!=found_node->second.guid)return failed(0xe0000201U);
                    info.nodes.push_back(handle);
                }
                auto found=std::find_if(info.interfaces.begin(),info.interfaces.end(),[&](const auto& item){return item.second.node==handle && item.second.guid==found_node->second.interface_guid;});
                if(found==info.interfaces.end()){
                    if(configuration.next_interface>UINT32_MAX)return failed(8);found=info.interfaces.emplace(configuration.next_interface++,DeviceConfigurationState::InfoSet::Interface{handle,found_node->second.interface_guid}).first;
                }
                if(arg(3) && !write_interface(arg(3),found->first))return failed(1784);return finish(1);
            }
            if(name=="SetupDiGetDeviceInterfacePropertyW" || name=="SetupDiGetDeviceInterfacePropertyKeys" || name=="SetupDiSetDeviceInterfacePropertyW"){
                const bool keys=name.ends_with("Keys"),setting=name.starts_with("SetupDiSet");
                if(arg(keys?5:setting?6:7))return failed(1004);
                const auto token=read_interface(arg(1));if(!token)return failed(87);
                // 現有別名共享同一 SymbolicLink；屬性身分跟隨該路徑的主要介面，CM 與別名集合必須一致。
                const auto& entry=info.interfaces.at(*token);const auto& node=nodes.at(entry.node);const auto error=interface_property_call(node,node.interface_guid,false);
                if(error)return failed(error);return finish(1);
            }
            if(name.starts_with("SetupDiGetDeviceInterfaceDetail") || name=="SetupDiOpenDeviceInterfaceRegKey" || name=="SetupDiDeleteDeviceInterfaceData"){
                const auto token=read_interface(arg(1));if(!token)return failed(87);
                const auto& entry=info.interfaces.at(*token);const auto node_handle=entry.node;const auto& node=nodes.at(node_handle);
                if(name=="SetupDiDeleteDeviceInterfaceData"){info.interfaces.erase(*token);return finish(1);}
                if(name.starts_with("SetupDiGetDeviceInterfaceDetail")){
                    if((!arg(2) && arg(3)) || (arg(2) && arg(3)<4+(wide?2U:1U)))return failed(87);
                    const auto encoded=utf16_to_utf8(node.interface_path);
                    auto bytes=wide?utf16_bytes(node.interface_path):std::vector<std::uint8_t>(encoded.begin(),encoded.end());if(!wide)bytes.push_back(0);
                    const auto required=4+bytes.size();
                    if(arg(4))memory.check(GuestAddress{arg(4)},4,Access::Write);
                    if(arg(5) && memory.load(GuestAddress{arg(5)},32)!=structure_size)return failed(1784);
                    const bool fits=arg(2) && arg(3)>=required;
                    if(arg(2) && memory.load(GuestAddress{arg(2)},32)!=(x86_?(wide?6U:5U):8U))return failed(1784);
                    if(fits)memory.check(GuestAddress{arg(2)},required,Access::Write);
                    if(arg(5))memory.check(GuestAddress{arg(5)},structure_size,Access::Write);
                    if(arg(4))memory.store(GuestAddress{arg(4)},32,required);
                    if(arg(5))(void)write_node(arg(5),node_handle);
                    if(!fits)return failed(122);memory.write(GuestAddress{arg(2)+4},bytes);return finish(1);
                }
                if(arg(2) || (arg(3)&0x300U)==0x300U)return failed(87);
                if((arg(3)&0x200U) || (arg(3)&~0x020f013fU))return failed(50);
                if(runtime_.registry.handles.size()>=4096)return failed(8);
                auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_);if(transaction.error)return failed(transaction.error);
                auto instance=node.port_name.empty()?node.id:node.port_name;std::replace(instance.begin(),instance.end(),u'\\',u'#');
                const auto path=folded(std::u16string(1,2)+u"\\system\\currentcontrolset\\control\\deviceclasses\\"+guid_name(entry.guid)+u"\\"+instance+u"\\#\\device parameters");
                if(!registry.keys.contains(path)){
                    std::map<std::u16string,RegistryValue> values;
                    const auto add=[&](std::u16string key,std::u16string_view text){RegistryValue value;value.type=1;value.display_name=key;value.bytes=utf16_bytes(text);values.emplace(folded(std::move(key)),std::move(value));};
                    add(u"DeviceInstance",node.id);add(u"SymbolicLink",node.interface_path);if(!node.port_name.empty())add(u"PortName",node.port_name);
                    std::uint64_t added=0;for(const auto& [key,value]:values){(void)key;added+=value.bytes.size();}
                    if(registry.bytes+added>output_limit || registry.values+values.size()>4096 || registry.keys.size()+12>4096)return failed(8);
                    const auto before=registry;
                    for(std::size_t end=1;;){registry.keys.try_emplace(path.substr(0,end));end=path.find(u'\\',end+1);if(end==path.npos)break;}
                    registry.values+=static_cast<std::uint32_t>(values.size());registry.bytes+=added;registry.keys[path]=std::move(values);
                    if(const auto error=transaction.commit();error){registry=before;return failed(error);}
                }
                const auto handle=registry.next_handle++;
                const auto access=static_cast<std::uint32_t>(arg(3)&0x02000000U?0xf003fU:arg(3)&~0x100U);
                registry.handles.emplace(handle,RegistryHandle{path,access});return finish(handle);
            }
