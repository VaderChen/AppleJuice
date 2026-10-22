            const bool constructs=name.starts_with("SetupDiGetClassDevs") || name.starts_with("SetupDiCreateDeviceInfoList") || name=="SetupDiOpenDevRegKey" || name=="SetupDiOpenDeviceInterfaceRegKey";
            const auto failed=[&](std::uint32_t error){last_error_=error;return finish(constructs?(x86_?UINT32_MAX:UINT64_MAX):0);};
            const auto registry_path=[&](const DeviceNode& node,bool driver){return folded(std::u16string(1,2)+(driver?u"\\system\\currentcontrolset\\control\\class\\"+node.class_guid+u"\\0000":u"\\system\\currentcontrolset\\enum\\"+node.id));};
            const auto ensure_registry=[&](const DeviceNode& node,bool driver)->std::uint32_t{
                // 裝置鍵通常已完整存在；沿用登錄快取，只有實際寫入才使快取失效。
                auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_);if(transaction.error)return transaction.error;
                const auto path=registry_path(node,driver);auto values=registry.keys.contains(path)?registry.keys.at(path):std::map<std::u16string,RegistryValue>{};
                std::uint64_t added=0;unsigned count=0;
                const auto add=[&](std::u16string key,const RegistryValue& value){key=folded(std::move(key));if(!values.contains(key)){values.emplace(std::move(key),value);added+=value.bytes.size();++count;}};
                for(const auto& [property,value]:node.properties){(void)property;add(value.first,value.second);}
                if(!driver && !node.port_name.empty()){
                    RegistryValue value;value.type=1;value.display_name=u"PortName";value.bytes=utf16_bytes(node.port_name);add(u"PortName",value);
                }
                if(driver && node.guid==display_identity::class_guid){
                    const auto text_value=[&](std::u16string key,std::u16string_view value){RegistryValue data;data.type=1;data.display_name=key;data.bytes=utf16_bytes(value);add(std::move(key),data);};
                    text_value(u"DriverVersion",display_identity::version);text_value(u"DriverDate",display_identity::date);text_value(u"ProviderName",display_identity::provider);
                    // 與 Windows 顯示驅動鍵相同的欄位：DriverDateData（FILETIME）、DriverDesc、MatchingDeviceId 與 HardwareInformation.*。
                    const auto binary_value=[&](std::u16string key,std::vector<std::uint8_t> bytes,std::uint32_t type){RegistryValue data;data.type=type;data.display_name=key;data.bytes=std::move(bytes);add(std::move(key),data);};
                    std::vector<std::uint8_t> date(8);for(unsigned i=0;i<8;++i)date[i]=static_cast<std::uint8_t>(display_identity::date_filetime>>(i*8));binary_value(u"DriverDateData",date,3);
                    const auto description=property_text(node,1);text_value(u"DriverDesc",description);text_value(u"MatchingDeviceId",u"root\\x64arm_display");
                    auto wide=[&](std::u16string_view text){auto bytes=utf16_bytes(text);bytes.push_back(0);bytes.push_back(0);return bytes;};
                    binary_value(u"HardwareInformation.AdapterString",wide(display_identity::adapter_name),3);binary_value(u"HardwareInformation.ChipType",wide(description),3);
                    binary_value(u"HardwareInformation.DacType",wide(u"Integrated RAMDAC"),3);binary_value(u"HardwareInformation.BiosString",wide(u""),3);
                    std::vector<std::uint8_t> size(8);const auto shared=gpu_memory_stats().limit;for(unsigned i=0;i<8;++i)size[i]=static_cast<std::uint8_t>(shared>>(i*8));binary_value(u"HardwareInformation.qwMemorySize",size,11);
                }
                if(!count && registry.keys.contains(path)){transaction.unchanged();return 0;}
                if(registry.bytes+added>output_limit || registry.values+count>4096 || registry.keys.size()+12>4096)return 8;
                const auto before=registry;
                for(std::size_t end=1;;){registry.keys.try_emplace(path.substr(0,end));end=path.find(u'\\',end+1);if(end==path.npos)break;}
                registry.keys[path]=std::move(values);registry.bytes+=added;registry.values+=count;
                if(const auto error=transaction.commit();error){registry=before;return error;}return 0;
            };
            auto& configuration=*device_configuration_;
            if(name.starts_with("SetupDiGetClassDevs") || name.starts_with("SetupDiCreateDeviceInfoList")){
                const bool empty=name.starts_with("SetupDiCreateDeviceInfoList"),extended=name.find("Ex")!=name.npos;
                const auto flags=empty?0U:static_cast<std::uint32_t>(arg(3));
                if(flags&~31U || (!empty && !(flags&4) && !arg(0)))return failed(87);
                if(extended){
                    if(arg(empty?3:6))return failed(87);auto machine=folded(read(arg(empty?2:5)));if(machine.starts_with(u"\\\\"))machine.erase(0,2);
                    if(!machine.empty() && machine!=u"." && machine!=u"localhost" && machine!=u"x64arm")return failed(50);
                }
                const auto existing=extended && !empty?arg(4):0;const auto held=configuration.sets.find(existing);
                if(existing && held==configuration.sets.end())return failed(6);
                if(!existing && (configuration.sets.size()>=4096 || configuration.next_set>UINT32_MAX))return failed(8);
                const bool interfaces=flags&16,specific_class=arg(0) && !(flags&4);
                std::array<std::uint8_t,16> requested_guid{};if(arg(0))memory.read(GuestAddress{arg(0)},requested_guid);
                // 以副本批次合併；既有集合與介面 token 在失敗時維持原狀。
                DeviceConfigurationState::InfoSet info=existing?held->second:DeviceConfigurationState::InfoSet{};
                if(!existing){info.filtered=specific_class && !interfaces;if(info.filtered)info.class_guid=requested_guid;}
                else if(info.filtered && specific_class && !interfaces && info.class_guid!=requested_guid)return failed(0xe0000201U);
                if(!interfaces && (flags&1))return failed(1004);
                const auto enumerator=empty?std::u16string{}:folded(read(arg(1)));
                auto next_interface=configuration.next_interface;
                if(!empty)for(const auto& [handle,node]:nodes){
                    if(node.pending_set || ((flags&2) && !node.present))continue;
                    if(info.filtered && info.class_guid!=node.guid)continue;
                    if(interfaces){
                        if(node.interface_path.empty() || (specific_class && !supports_interface(node,requested_guid)))continue;
                        // 尚無系統預設 COM 介面；不將第一個裝置冒充預設裝置。
                        if(flags&1)continue;
                    }else if(specific_class && requested_guid!=node.guid)continue;
                    if(!enumerator.empty() && enumerator!=folded(node.enumerator) && (!interfaces || enumerator!=folded(node.id)))continue;
                    if(!node.class_guid.empty())if(const auto error=ensure_registry(node,true))return failed(error);
                    if(std::find(info.nodes.begin(),info.nodes.end(),handle)==info.nodes.end())info.nodes.push_back(handle);
                    if(interfaces){
                        auto guids=node.interface_aliases;guids.insert(guids.begin(),node.interface_guid);
                        for(const auto& guid:guids){
                            if(specific_class && requested_guid!=guid)continue;
                            if(std::any_of(info.interfaces.begin(),info.interfaces.end(),[&](const auto& item){return item.second.node==handle && item.second.guid==guid;}))continue;
                            if(next_interface>UINT32_MAX)return failed(8);info.interfaces.emplace(next_interface++,DeviceConfigurationState::InfoSet::Interface{handle,guid});
                        }
                    }
                }
                const auto handle=existing?existing:configuration.next_set++;configuration.sets.insert_or_assign(handle,std::move(info));configuration.next_interface=next_interface;return finish(handle);
            }
#include "windows_device_installation.inc"
            const auto found_set=configuration.sets.find(arg(0));if(found_set==configuration.sets.end())return failed(6);
            auto& info=found_set->second;
            if(name=="SetupDiDestroyDeviceInfoList"){
                // 未註冊的元素隨集合銷毀。
                std::erase_if(configuration.nodes,[&](const auto& item){return item.second.pending_set==arg(0);});configuration.sets.erase(found_set);return finish(1);
            }
            if(name=="SetupDiGetDeviceInfoListClass"){
                if(!arg(1))return failed(87);if(!info.filtered)return failed(0xe0000200U);memory.write(GuestAddress{arg(1)},info.class_guid);return finish(1);
            }
            if(name.starts_with("SetupDiGetDeviceInfoListDetail")){
                const auto size=x86_?(wide?550U:287U):(wide?560U:296U);
                if(!arg(1) || memory.load(GuestAddress{arg(1)},32)!=size)return failed(1784);
                memory.check(GuestAddress{arg(1)},size,Access::Write);std::vector<std::uint8_t> bytes(size);
                for(unsigned i=0;i<4;++i)bytes[i]=static_cast<std::uint8_t>(size>>(8*i));if(info.filtered)std::copy(info.class_guid.begin(),info.class_guid.end(),bytes.begin()+4);
                memory.write(GuestAddress{arg(1)},bytes);return finish(1);
            }
            const auto structure_size=x86_?28U:32U;
            const auto write_node=[&](std::uint64_t pointer,std::uint32_t node_handle)->bool{
                if(!pointer || memory.load(GuestAddress{pointer},32)!=structure_size)return false;
                const auto& node=nodes.at(node_handle);memory.check(GuestAddress{pointer},structure_size,Access::Write);
                memory.write(GuestAddress{pointer+4},node.guid);memory.store(GuestAddress{pointer+20},32,node_handle);
                memory.store(GuestAddress{pointer+24},x86_?32:64,arg(0));return true;
            };
            const auto read_node=[&](std::uint64_t pointer)->std::optional<std::uint32_t>{
                if(!pointer || memory.load(GuestAddress{pointer},32)!=structure_size)return {};
                const auto handle=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+20},32));
                if(!nodes.contains(handle) || memory.load(GuestAddress{pointer+24},x86_?32:64)!=arg(0) || std::find(info.nodes.begin(),info.nodes.end(),handle)==info.nodes.end())return {};
                std::array<std::uint8_t,16> guid;memory.read(GuestAddress{pointer+4},guid);return guid==nodes.at(handle).guid?std::optional{handle}:std::nullopt;
            };
#include "windows_driver_lists.inc"
            if(name=="SetupDiOpenDeviceInfoA" || name=="SetupDiOpenDeviceInfoW"){
                if(arg(3)&~6U)return failed(1004);const auto id=folded(read(arg(1)));
                const auto found_node=id.empty()?nodes.find(1):std::find_if(nodes.begin(),nodes.end(),[&](const auto& entry){return (!entry.second.pending_set || entry.second.pending_set==arg(0)) && folded(entry.second.id)==id;});
                if(found_node==nodes.end())return failed(0xe000020bU);
                if(info.filtered && info.class_guid!=found_node->second.guid)return failed(0xe0000201U);
                const auto handle=found_node->first;
                if(std::find(info.nodes.begin(),info.nodes.end(),handle)==info.nodes.end())info.nodes.push_back(handle);
                if(arg(4) && !write_node(arg(4),handle))return failed(1784);return finish(1);
            }
            if(name=="SetupDiEnumDeviceInfo"){
                if(arg(1)>=info.nodes.size())return failed(259);
                if(!write_node(arg(2),info.nodes[static_cast<std::size_t>(arg(1))]))return failed(1784);return finish(1);
            }
#include "windows_device_interfaces.inc"
            const auto checked_node=read_node(arg(1));if(!checked_node)return failed(87);const auto node_handle=*checked_node;
            const auto& node=nodes.at(node_handle);
            if(name=="SetupDiDeleteDeviceInfo"){
                std::erase(info.nodes,node_handle);std::erase_if(info.interfaces,[&](const auto& item){return item.second.node==node_handle;});
                std::erase_if(info.driver_lists,[&](const auto& item){return item.first.first==node_handle;});info.install_params.erase(node_handle);
                if(node.pending_set==arg(0))configuration.nodes.erase(node_handle);return finish(1);
            }
            if(name.starts_with("SetupDiGetDeviceInstanceId")){
                const auto encoded=utf16_to_utf8(node.id);const auto length=(wide?node.id.size():encoded.size())+1;
                if(arg(4))memory.store(GuestAddress{arg(4)},32,length);
                if(!arg(2) || arg(3)<length)return failed(122);
                auto data=wide?utf16_bytes(node.id):std::vector<std::uint8_t>(encoded.begin(),encoded.end());if(!wide)data.push_back(0);
                memory.write(GuestAddress{arg(2)},data);return finish(1);
            }
            if(name.starts_with("SetupDiGetDeviceRegistryProperty")){
                if(arg(2)>36)return failed(87);const auto property=node.properties.find(static_cast<std::uint32_t>(arg(2))+1);
                if(property==node.properties.end())return failed(13);
                const auto data=property_bytes(property->second.second,wide);
                if(arg(3))memory.store(GuestAddress{arg(3)},32,property->second.second.type);
                if(arg(6))memory.store(GuestAddress{arg(6)},32,data.size());
                if(!arg(4) || arg(5)<data.size())return failed(122);
                memory.write(GuestAddress{arg(4)},data);return finish(1);
            }
            if(name=="SetupDiGetDevicePropertyW"){
                if(arg(7))return failed(1004);if(!arg(2) || !arg(3))return failed(87);
                if(!arg(4) && arg(5))return failed(1784);
                const auto key=read_device_property_key(memory,arg(2));if(!valid_device_property_key(key))return failed(0xe0000209U);
                const auto property=device_property(node,key,nodes);if(!property)return failed(1168);
                memory.check(GuestAddress{arg(3)},4,Access::Write);if(arg(6))memory.check(GuestAddress{arg(6)},4,Access::Write);
                const bool fits=arg(4) && arg(5)>=property->bytes.size();if(fits)memory.check(GuestAddress{arg(4)},property->bytes.size(),Access::Write);
                memory.store(GuestAddress{arg(3)},32,property->type);if(arg(6))memory.store(GuestAddress{arg(6)},32,property->bytes.size());
                if(!fits)return failed(122);memory.write(GuestAddress{arg(4)},property->bytes);return finish(1);
            }
            if(name=="SetupDiOpenDevRegKey"){
                if(arg(2)!=1 || arg(3) || (arg(4)!=1 && arg(4)!=2))return failed(87);
                const bool driver=arg(4)==2;if(driver && node.class_guid.empty())return failed(2);
                if(arg(5)&~0x20319U)return failed(5);if(runtime_.registry.handles.size()>=4096)return failed(8);
                if(const auto error=ensure_registry(node,driver))return failed(error);
                const auto handle=runtime_.registry.next_handle++;
                runtime_.registry.handles.emplace(handle,RegistryHandle{registry_path(node,driver),static_cast<std::uint32_t>(arg(5)&~0x300U)});return finish(handle);
            }
            throw HostCallFault("尚未實作 SetupAPI 集合操作："+std::string(name));
