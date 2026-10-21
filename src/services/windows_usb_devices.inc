// USB 匯流排：依 IOKit 的實際裝置與集線器拓撲建立 Windows USB devnode。每個主控制器一個節點，
// 其下為 USB\VID_xxxx&PID_xxxx\<序號或位置>；複合裝置的每個介面為 &MI_nn 子節點。
// 已由 macOS 驅動接管的介面、集線器與複合裝置回報啟動；沒有驅動接管者為 CM_PROB_FAILED_INSTALL。
{
    auto& config=*device_configuration_;
    const auto hex=[](unsigned value,unsigned digits){constexpr char16_t table[]=u"0123456789ABCDEF";std::u16string out(digits,u'0');for(unsigned i=0;i<digits;++i)out[digits-1-i]=table[(value>>(4*i))&15U];return out;};
    struct UsbClass {std::u16string_view name,guid,service,description;};
    const auto classify=[](unsigned cls)->UsbClass{
        switch(cls){
        case 0x01:return {u"MEDIA",u"{4d36e96c-e325-11ce-bfc1-08002be10318}",u"usbaudio",u"USB Audio Device"};
        case 0x02:case 0x0a:return {u"Ports",u"{4d36e978-e325-11ce-bfc1-08002be10318}",u"usbser",u"USB Serial Device"};
        case 0x03:return {u"HIDClass",u"{745a17a0-74d3-11d0-b6fe-00a0c90f57da}",u"HidUsb",u"USB Input Device"};
        case 0x08:return {u"USB",u"{36fc9e60-c465-11cf-8056-444553540000}",u"USBSTOR",u"USB Mass Storage Device"};
        case 0x09:return {u"USB",u"{36fc9e60-c465-11cf-8056-444553540000}",u"USBHUB3",u"Generic USB Hub"};
        case 0x0e:return {u"Camera",u"{ca3e7ab9-b4c3-4ae6-8251-579ef933890f}",u"usbvideo",u"USB Video Device"};
        case 0xe0:return {u"Bluetooth",u"{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}",u"BTHUSB",u"Generic Bluetooth Adapter"};
        case 0xff:return {u"USBDevice",u"{88bae032-5a81-49f0-bc3d-a4ff138216d6}",u"",u"USB Device"};
        default:return {u"USB",u"{36fc9e60-c465-11cf-8056-444553540000}",u"",u"USB Device"};
        }
    };
    const auto allocate=[&](auto& table,const auto& key){
        if(const auto found=table.find(key);found!=table.end())return found->second;
        while(config.nodes.contains(config.next_dynamic_node)){if(config.next_dynamic_node==UINT32_MAX)throw std::bad_alloc{};++config.next_dynamic_node;}
        const auto handle=config.next_dynamic_node++;table.emplace(key,handle);return handle;
    };
    const auto ids=[&](std::u16string_view prefix,unsigned cls,unsigned subclass,unsigned protocol){
        const auto base=std::u16string(prefix)+hex(cls,2);
        return base+u"&SubClass_"+hex(subclass,2)+u"&Prot_"+hex(protocol,2)+u'\0'+base+u"&SubClass_"+hex(subclass,2)+u'\0'+base;
    };
    const auto fill=[&](DeviceNode& node,const UsbClass& kind,std::u16string description,const std::u16string& hardware,const std::u16string& compatible,bool started,const usb::Device& device){
        node.enumerator=u"USB";node.present=true;node.service=std::u16string(kind.service);node.class_guid=std::u16string(kind.guid);
        node.guid=parse_windows_guid(node.class_guid,true).value_or(WindowsGuid{});node.status=started?0x000bU:0x0401U;node.problem=started?0U:28U;
        text_property(node,1,u"DeviceDesc",std::move(description));text_property(node,2,u"HardwareID",hardware,true);text_property(node,3,u"CompatibleIDs",compatible,true);
        if(!node.service.empty())text_property(node,5,u"Service",node.service);text_property(node,8,u"Class",std::u16string(kind.name));text_property(node,9,u"ClassGUID",node.class_guid);
        if(!device.vendor_name.empty())text_property(node,12,u"Mfg",device.vendor_name);if(!device.product_name.empty())text_property(node,13,u"FriendlyName",device.product_name);
        text_property(node,14,u"LocationInformation",u"Port_#"+utf8_to_utf16(std::to_string(10000+device.port).substr(1))+u".Hub_#"+utf8_to_utf16(std::to_string(10000+((device.location>>20)&0xfU)).substr(1)));
        text_property(node,23,u"EnumeratorName",node.enumerator);number_property(node,11,u"ConfigFlags",0);number_property(node,16,u"Capabilities",device.cls==9?0U:0x84U); // CM_DEVCAP_REMOVABLE｜SURPRISEREMOVALOK
        number_property(node,0x1d,u"Address",device.port);
    };
    for(auto& [key,handle]:config.usb_nodes){(void)key;if(const auto found=config.nodes.find(handle);found!=config.nodes.end()){found->second.present=false;found->second.status=0x0401U;found->second.problem=45;}}
    for(auto& [key,handle]:config.usb_interfaces){(void)key;if(const auto found=config.nodes.find(handle);found!=config.nodes.end()){found->second.present=false;found->second.status=0x0401U;found->second.problem=45;}}
    const auto list=usb::devices();std::set<std::u16string> used;config.usb_paths.clear();config.usb_targets.clear();
    // 裝置介面路徑：\\?\ + 實例 ID（\ 換成 #）+ #{介面類別 GUID}，與 Windows 相同採小寫。
    constexpr std::u16string_view usb_device_interface=u"{a5dcbf10-6530-11d2-901f-00c04fb951ed}",usb_hub_interface=u"{f18a0e88-c30c-11d0-8815-00a0c906bed8}",
        winusb_interface=u"{dee824ef-729b-4a0e-9c14-b7117d33a817}";
    const auto publish=[&](DeviceNode& node,std::u16string_view primary,std::vector<std::u16string_view> aliases,const usb::Target& target){
        auto path=u"\\\\?\\"+folded(node.id);for(auto& c:path)if(c==u'\\')c=u'#';path[0]=path[1]=path[3]=u'\\';path+=u"#"+std::u16string(primary);
        node.interface_path=path;node.interface_guid=parse_windows_guid(primary,true).value_or(WindowsGuid{});
        for(const auto alias:aliases)node.interface_aliases.push_back(parse_windows_guid(alias,true).value_or(WindowsGuid{}));
        config.usb_targets.insert_or_assign(folded(path),target);
    };
    for(const auto& device:list){
        // 主控制器：相容層節點，不冒充宿主 PCI 識別。
        const auto controller=allocate(config.usb_controllers,device.controller);
        if(!config.nodes.contains(controller) || config.nodes.at(controller).id.empty()){
            DeviceNode host;host.id=u"ROOT\\X64ARM_USB_HOST\\"+hex(device.controller>>24,4);host.enumerator=u"ROOT";host.service=u"x64armUsbHost";host.parent=1;host.status=0x000b;
            host.class_guid=u"{36fc9e60-c465-11cf-8056-444553540000}";host.guid=parse_windows_guid(host.class_guid,true).value_or(WindowsGuid{});
            text_property(host,1,u"DeviceDesc",u"USB xHCI Compliant Host Controller");text_property(host,2,u"HardwareID",u"ROOT\\X64ARM_USB_HOST",true);
            text_property(host,5,u"Service",host.service);text_property(host,8,u"Class",u"USB");text_property(host,9,u"ClassGUID",host.class_guid);
            text_property(host,23,u"EnumeratorName",host.enumerator);number_property(host,11,u"ConfigFlags",0);number_property(host,16,u"Capabilities",0);
            config.nodes.insert_or_assign(controller,std::move(host));
        }
        const auto handle=allocate(config.usb_nodes,device.registry_id);
        const auto vidpid=u"USB\\VID_"+hex(device.vendor,4)+u"&PID_"+hex(device.product,4);
        // 實例：裝置回報可辨識序號時使用序號，否則以位置區分。
        std::u16string instance=device.serial;
        if(instance.empty() || instance.size()>64 || !std::all_of(instance.begin(),instance.end(),[](char16_t c){return (c>=u'0' && c<=u'9') || (c>=u'A' && c<=u'Z') || (c>=u'a' && c<=u'z');}))instance=u"X64ARM&"+hex(device.location,8);
        for(auto& c:instance)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
        if(used.contains(vidpid+u"\\"+instance))instance+=u"&"+hex(device.location,8);used.insert(vidpid+u"\\"+instance);
        const bool composite=device.cls==0 && device.interfaces.size()>1;
        const unsigned function=device.cls?device.cls:device.interfaces.empty()?0U:device.interfaces.front().cls;
        const auto kind=composite?UsbClass{u"USB",u"{36fc9e60-c465-11cf-8056-444553540000}",u"usbccgp",u"USB Composite Device"}:classify(function);
        std::u16string compatible=composite?std::u16string(u"USB\\DevClass_00&SubClass_00&Prot_00")+u'\0'+u"USB\\DevClass_00&SubClass_00"+u'\0'+u"USB\\DevClass_00"+u'\0'+u"USB\\COMPOSITE":
            device.cls?ids(u"USB\\DevClass_",device.cls,device.subclass,device.protocol):
            device.interfaces.empty()?std::u16string{}:ids(u"USB\\Class_",device.interfaces.front().cls,device.interfaces.front().subclass,device.interfaces.front().protocol);
        const bool claimed=std::any_of(device.interfaces.begin(),device.interfaces.end(),[](const auto& item){return item.claimed;});
        // 沒有任何 macOS 驅動接管的單一功能裝置：WinUSB 為其功能驅動，經 IOUSBHost 存取。
        const bool winusb=!composite && device.cls!=9 && !device.interfaces.empty() && !claimed;
        const auto device_kind=winusb?UsbClass{u"USBDevice",u"{88bae032-5a81-49f0-bc3d-a4ff138216d6}",u"WinUSB",u"WinUsb Device"}:kind;
        DeviceNode node;node.id=vidpid+u"\\"+instance;
        node.parent=device.parent_id && config.usb_nodes.contains(device.parent_id)?config.usb_nodes.at(device.parent_id):controller;
        fill(node,device_kind,std::u16string(device_kind.description),
             vidpid+u"&REV_"+hex(device.revision,4)+u'\0'+vidpid,compatible,device.cls==9 || composite || claimed || winusb,device);
        {usb::Target target;target.registry_id=device.registry_id;target.winusb=winusb;target.speed=device.speed;for(const auto& item:device.interfaces)target.interfaces.push_back(item.number);
            if(device.cls==9)publish(node,usb_hub_interface,{usb_device_interface},target);else if(winusb)publish(node,usb_device_interface,{winusb_interface},target);else publish(node,usb_device_interface,{},target);}
        config.nodes.insert_or_assign(handle,std::move(node));
        // 其下服務（例如 HID）以 IOService 路徑前綴找到所屬的 USB 節點。
        if(!composite){config.usb_paths.emplace_back(device.path,handle);for(const auto& item:device.interfaces)config.usb_paths.emplace_back(item.path,handle);continue;}
        for(const auto& item:device.interfaces){
            const auto child=allocate(config.usb_interfaces,std::pair{device.registry_id,item.number});const auto mi=u"&MI_"+hex(item.number,2);
            const auto interface_kind=item.claimed?classify(item.cls):UsbClass{u"USBDevice",u"{88bae032-5a81-49f0-bc3d-a4ff138216d6}",u"WinUSB",u"WinUsb Device"};
            DeviceNode function_node;function_node.id=vidpid+mi+u"\\"+instance+u"&"+hex(item.number,4);function_node.parent=handle;
            fill(function_node,interface_kind,std::u16string(interface_kind.description),vidpid+u"&REV_"+hex(device.revision,4)+mi+u'\0'+vidpid+mi,
                 ids(u"USB\\Class_",item.cls,item.subclass,item.protocol),true,device);
            // 未被 macOS 接管的複合介面以 WinUSB 公開；已接管者由對應的原生後端（HID、儲存…）提供功能。
            if(!item.claimed){usb::Target target;target.registry_id=device.registry_id;target.interfaces={item.number};target.winusb=true;target.speed=device.speed;publish(function_node,winusb_interface,{},target);}
            config.nodes.insert_or_assign(child,std::move(function_node));
            config.usb_paths.emplace_back(item.path,child);
        }
    }
}
