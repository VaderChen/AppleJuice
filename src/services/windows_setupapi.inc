            const bool constructs=name.starts_with("SetupDiGetClassDevs") || name.starts_with("SetupDiCreateDeviceInfoList") || name=="SetupDiOpenDevRegKey" || name=="SetupDiOpenDeviceInterfaceRegKey" ||
                name.starts_with("SetupDiCreateDevRegKey");
            const auto failed=[&](std::uint32_t error){last_error_=error;return finish(constructs?(x86_?UINT32_MAX:UINT64_MAX):0);};
            const auto registry_path=[](const DeviceNode& node,bool driver){return device_key_path(node,driver);};
            const auto ensure_registry=[&](const DeviceNode& node,bool driver)->std::uint32_t{
                // 有 hardware 值表的節點：硬體鍵只放這些值，內容不同時覆寫；其他節點與驅動鍵維持只補缺少的值。
                if(!driver && !node.hardware.empty())return sync_hardware_key(runtime_.registry,registry_file_,&registry_store_cache_,node,output_limit);
                // 裝置鍵通常已完整存在；沿用登錄快取，只有實際寫入才使快取失效。
                auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_);if(transaction.error)return transaction.error;
                const auto path=registry_path(node,driver);const auto existing=registry.keys.find(path);
                std::map<std::u16string,RegistryValue> additions;std::uint64_t added=0;
                const auto add=[&](std::u16string key,const RegistryValue& value){
                    key=folded(std::move(key));
                    if(existing!=registry.keys.end() && existing->second.contains(key))return;
                    if(additions.try_emplace(std::move(key),value).second)added+=value.bytes.size();
                };
                for(const auto& [property,value]:node.properties){(void)property;add(value.first,value.second);}
                if(!driver && !node.port_name.empty()){
                    RegistryValue value;value.type=1;value.display_name=u"PortName";value.bytes=utf16_bytes(node.port_name);add(u"PortName",value);
                }
                if(driver && node.guid==display_identity::class_guid){
                    for(const auto& [key,value]:display_identity::driver_registry_values(property_text(node,1),gpu_memory_stats().limit))add(key,value);
                }
                if(additions.empty() && existing!=registry.keys.end()){transaction.unchanged();return 0;}
                if(registry.bytes+added>output_limit || registry.values+additions.size()>4096 || registry.keys.size()+12>4096)return 8;
                const auto before=registry;
                for(std::size_t end=1;;){registry.keys.try_emplace(path.substr(0,end));end=path.find(u'\\',end+1);if(end==path.npos)break;}
                registry.values+=additions.size();registry.keys[path].merge(additions);registry.bytes+=added;
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
                // SetupDiGetClassDevs* 成功時清除 last error：常見寫法以 ERROR_NO_MORE_ITEMS != GetLastError() 當列舉迴圈條件，
                // 且在第一次列舉前就檢查；上一輪列舉留下的 259 不可讓新集合一筆都不走。
                if(!empty)last_error_=0;
                const auto handle=existing?existing:configuration.next_set++;configuration.sets.insert_or_assign(handle,std::move(info));configuration.next_interface=next_interface;return finish(handle);
            }
            // 裝置屬性寫入後，下一次盤點重新同步宿主 USB 節點。
            if(name.starts_with("SetupDiSetDeviceRegistryProperty"))configuration.usb_snapshot.reset();
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
                if(!write_node(arg(2),info.nodes[static_cast<std::size_t>(arg(1))]))return failed(1784);last_error_=0;return finish(1);
            }
#include "windows_device_interfaces.inc"
            if(name.starts_with("SetupDiGetDeviceInstallParams") || name.starts_with("SetupDiSetDeviceInstallParams")){
                // SP_DEVINSTALL_PARAMS(A/W)：cbSize、Flags、FlagsEx、hwndParent、InstallMsgHandler、InstallMsgHandlerContext、FileQueue、ClassInstallReserved、
                // Reserved、DriverPath[MAX_PATH]。DeviceInfoData 為 NULL 時是整個集合的參數，元素沒有自己的參數時沿用集合的參數。
                // DI_NOVCP（0x8）表示使用呼叫端的檔案佇列，此時 FileQueue 必須是有效的控制代碼。
                std::uint32_t key=0;if(arg(1)){const auto handle=read_node(arg(1));if(!handle)return failed(87);key=*handle;}
                const unsigned pointer_size=x86_?4U:8U,header=x86_?36U:60U,path_bytes=wide?520U:260U;
                const unsigned size=x86_?header+path_bytes:((header+path_bytes+7U)&~7U),pointer_at=x86_?12U:16U;
                if(!arg(2) || memory.load(GuestAddress{arg(2)},32)!=size)return failed(1784);
                using InstallParams=DeviceConfigurationState::InfoSet::InstallParams;
                if(name.starts_with("SetupDiGet")){
                    auto found=info.device_install.find(key);if(found==info.device_install.end() && key)found=info.device_install.find(0);
                    const auto params=found==info.device_install.end()?InstallParams{}:found->second;
                    std::vector<std::uint8_t> bytes(size);const auto put=[&](unsigned at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
                    put(0,size,4);put(4,params.flags,4);put(8,params.flags_ex,4);
                    const std::array<std::uint64_t,5> pointers{params.parent,params.handler,params.context,params.queue,params.class_reserved};
                    for(unsigned i=0;i<5;++i)put(pointer_at+i*pointer_size,pointers[i],pointer_size);
                    put(header-4,params.reserved,4);
                    std::vector<std::uint8_t> path;
                    if(wide)path=utf16_bytes(params.driver_path);else{const auto narrow=utf16_to_utf8(params.driver_path);path.assign(narrow.begin(),narrow.end());path.push_back(0);}
                    path.resize(std::min<std::size_t>(path.size(),path_bytes));std::copy(path.begin(),path.end(),bytes.begin()+header);
                    memory.write(GuestAddress{arg(2)},bytes);return finish(1);
                }
                InstallParams params;const auto field=[&](unsigned at,unsigned width){return memory.load(GuestAddress{arg(2)+at},static_cast<std::uint8_t>(width*8));};
                params.flags=static_cast<std::uint32_t>(field(4,4));params.flags_ex=static_cast<std::uint32_t>(field(8,4));
                params.parent=field(pointer_at,pointer_size);params.handler=field(pointer_at+pointer_size,pointer_size);params.context=field(pointer_at+2*pointer_size,pointer_size);
                params.queue=field(pointer_at+3*pointer_size,pointer_size);params.class_reserved=field(pointer_at+4*pointer_size,pointer_size);params.reserved=static_cast<std::uint32_t>(field(header-4,4));
                params.driver_path=wide?read_utf16_counted(memory,GuestAddress{arg(2)+header},259):utf8_to_utf16_lossy(read_ansi(memory,arg(2)+header,259));
                if(const auto nul=params.driver_path.find(u'\0');nul!=std::u16string::npos)params.driver_path.resize(nul);
                if((params.flags&8U) && (!params.queue || params.queue==(x86_?UINT32_MAX:UINT64_MAX)))return failed(87);
                info.device_install.insert_or_assign(key,std::move(params));return finish(1);
            }
            const auto checked_node=read_node(arg(1));if(!checked_node)return failed(87);const auto node_handle=*checked_node;
            const auto& node=nodes.at(node_handle);
            if(name=="SetupDiDeleteDeviceInfo"){
                std::erase(info.nodes,node_handle);std::erase_if(info.interfaces,[&](const auto& item){return item.second.node==node_handle;});
                std::erase_if(info.driver_lists,[&](const auto& item){return item.first.first==node_handle;});info.install_params.erase(node_handle);info.device_install.erase(node_handle);
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
            if(name=="SetupDiGetDevicePropertyKeys"){
                // (Set, Data, PropertyKeyArray, PropertyKeyCount, RequiredPropertyKeyCount*, Flags)：與 CM_Get_DevNode_Property_Keys 同一份鍵清單（DEVPROPKEY 20 bytes）。
                if(arg(5))return failed(1004);if(!arg(2) && arg(3))return failed(87);
                const auto keys=device_property_keys(node,nodes);
                if(arg(4))memory.store(GuestAddress{arg(4)},32,keys.size());
                if(arg(3)<keys.size())return failed(122);
                for(std::size_t i=0;i<keys.size();++i){memory.write(GuestAddress{arg(2)+i*20},keys[i].format);memory.store(GuestAddress{arg(2)+i*20+16},32,keys[i].id);}
                return finish(1);
            }
            if(name.starts_with("SetupDiGetCustomDeviceProperty")){
                // (Set, Data, CustomPropertyName, Flags, PropertyRegDataType*, PropertyBuffer, PropertyBufferSize, RequiredSize*)：讀裝置硬體鍵（與 SetupDiOpenDevRegKey
                // 的 DIREG_DEV 相同）中的同名值。DICUSTOMDEVPROP_MERGE_MULTISZ（1）合併裝置專屬與 devnode 專屬的 REG_SZ／REG_MULTI_SZ 並以 REG_MULTI_SZ 傳回；
                // 本機只有硬體鍵這一份來源，REG_SZ 轉成只含一項的 REG_MULTI_SZ。
                if(!arg(2) || (arg(3)&~1U))return failed(87);
                const auto value_name=folded(read(arg(2)));if(value_name.empty())return failed(87);
                if(const auto error=ensure_registry(node,false))return failed(error);
                RegistryValue value;
                {
                    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return failed(transaction.error);
                    const auto key=runtime_.registry.keys.find(registry_path(node,false));if(key==runtime_.registry.keys.end())return failed(13);
                    const auto found=key->second.find(value_name);if(found==key->second.end())return failed(13);value=found->second;
                }
                auto type=value.type;auto data=property_bytes(value,wide);
                if((arg(3)&1U) && (type==1 || type==7)){
                    if(type==1){if(wide){data.push_back(0);data.push_back(0);}else data.push_back(0);}type=7;
                }
                if(arg(4))memory.store(GuestAddress{arg(4)},32,type);if(arg(7))memory.store(GuestAddress{arg(7)},32,data.size());
                if(!arg(5) || arg(6)<data.size())return failed(122);
                memory.write(GuestAddress{arg(5)},data);return finish(1);
            }
            if(name.starts_with("SetupDiCreateDevRegKey")){
                // (Set, Data, Scope, HwProfile, KeyType, InfHandle, InfSectionName)：與 SetupDiOpenDevRegKey 同一個鍵，不存在時建立，回傳 KEY_ALL_ACCESS 控制代碼。
                // Scope 只支援 DICS_FLAG_GLOBAL；DIREG_DRV 而元素尚無驅動鍵時，與安裝流程相同地指派 Control\Class\{guid}\<編號> 並記入 SPDRP_DRIVER。
                // 指定 INF 時由 invoke_setup_inf 先以 InfHandle＝NULL 呼叫本入口，再以 SetupInstallFromInfSection（SPINST_REGISTRY）套用到新鍵。
                const auto kind=static_cast<std::uint32_t>(arg(4));
                if(arg(2)!=1 || arg(3) || (kind!=1 && kind!=2) || (arg(5) && !arg(6)))return failed(87);
                if(runtime_.registry.handles.size()>=4096)return failed(8);
                // SetupAPI 建立且尚未記錄 SPDRP_DRIVER 的元素同樣指派空閒編號；主機列舉的節點沿用既有驅動鍵。
                if(kind==2 && (node.class_guid.empty() || (node.setup_created && !node.properties.contains(10)))){
                    if(std::all_of(node.guid.begin(),node.guid.end(),[](auto byte){return byte==0;}))return failed(0xe0000209U); // ERROR_INVALID_CLASS
                    auto& target=configuration.nodes.at(node_handle);const auto saved=target;target.class_guid=guid_name(target.guid);
                    std::set<std::u16string> used;
                    {
                        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return failed(transaction.error);
                        const auto prefix=folded(std::u16string(1,2)+u"\\system\\currentcontrolset\\control\\class\\"+target.class_guid+u"\\");
                        for(const auto& [path,values]:runtime_.registry.keys){(void)values;if(path.starts_with(prefix) && path.find(u'\\',prefix.size())==std::u16string::npos)used.insert(path.substr(prefix.size()));}
                    }
                    for(unsigned index=0;index<10000;++index){std::u16string text=utf8_to_utf16(std::to_string(index));text.insert(0,4-std::min<std::size_t>(4,text.size()),u'0');if(!used.contains(text)){target.driver_instance=text;break;}}
                    RegistryValue driver;driver.type=1;driver.display_name=u"Driver";driver.bytes=utf16_bytes(target.class_guid+u"\\"+target.driver_instance);
                    target.properties.insert_or_assign(10,std::pair{std::u16string(u"Driver"),std::move(driver)});
                    if(!target.pending_set)if(const auto error=write_enum_key(runtime_.registry,registry_file_,target,output_limit)){target=saved;return failed(error);}
                }
                const auto& current=configuration.nodes.at(node_handle);
                if(const auto error=ensure_registry(current,kind==2))return failed(error);
                const auto handle=runtime_.registry.next_handle++;
                runtime_.registry.handles.emplace(handle,RegistryHandle{registry_path(current,kind==2),0xf003fU});
                // INF 區段由 invoke_setup_inf 的協程在建立鍵之後套用（不可從裝置組態服務內巢狀呼叫安裝服務）。
                return finish(handle);
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
