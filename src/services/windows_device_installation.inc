// 裝置安裝：SetupDiCreateDeviceInfo 建立未註冊元素，DIF_REGISTERDEVICE 將它註冊為 root 列舉 devnode，
// 並寫入 sandbox 登錄的 Enum\<instance>；DIF_REMOVE 移除 devnode 與登錄。只處理經 SetupAPI 建立的裝置，
// 原生橋接裝置（顯示、序列埠、HID、藍牙）由宿主狀態決定，不可由客體刪除。
if(name.starts_with("SetupDiCreateDeviceInfo") || name.starts_with("SetupDiSetDeviceRegistryProperty") || name.find("ClassInstallParams")!=name.npos ||
   name=="SetupDiCallClassInstaller" || name=="SetupDiRegisterDeviceInfo" || name=="SetupDiRemoveDevice" || name=="DiUninstallDevice"){
    // SetupDiCallClassInstaller(DIF, set, data)、DiUninstallDevice(hwnd, set, data, Flags, NeedReboot)：集合在第二個參數。
    const bool call=name=="SetupDiCallClassInstaller" || name=="DiUninstallDevice";
    const auto set_handle=call?arg(1):arg(0),element_pointer=call?arg(2):arg(1);
    const auto held=configuration.sets.find(set_handle);if(held==configuration.sets.end())return failed(6);
    auto& set=held->second;auto& all=configuration.nodes;const auto element_size=x86_?28U:32U;
    const auto dword=[](const std::vector<std::uint8_t>& bytes,std::size_t at){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=static_cast<std::uint32_t>(bytes[at+i])<<(8*i);return value;};
    const auto element=[&](std::uint64_t pointer)->std::optional<std::uint32_t>{
        if(!pointer || memory.load(GuestAddress{pointer},32)!=element_size)return {};
        const auto handle=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+20},32));
        if(!all.contains(handle) || memory.load(GuestAddress{pointer+24},x86_?32:64)!=set_handle || std::find(set.nodes.begin(),set.nodes.end(),handle)==set.nodes.end())return {};
        return handle;
    };
    const auto publish=[&](std::uint64_t pointer,std::uint32_t handle){
        memory.write(GuestAddress{pointer+4},all.at(handle).guid);memory.store(GuestAddress{pointer+20},32,handle);memory.store(GuestAddress{pointer+24},x86_?32:64,set_handle);
    };
    const auto administrator=[&]{
        initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());
        return token.integrity>=0x3000U && std::any_of(token.groups.begin(),token.groups.end(),[](const auto& group){return group.sid==security::account_sid(*security::find_account("Administrators")) && (group.attributes&0x14U)==4U;});
    };
    const auto store_node=[&](const DeviceNode& node){return write_enum_key(runtime_.registry,registry_file_,node,output_limit);};
    const auto erase_node=[&](const DeviceNode& node){return erase_registry_tree(runtime_.registry,registry_file_,std::u16string(1,2)+u"\\SYSTEM\\CurrentControlSet\\Enum\\"+node.id);};
    const auto text_of=[&](const DeviceNode& node,std::uint32_t property){return property_text(node,property);};
    const auto register_node=[&](std::uint32_t handle)->std::uint32_t{
        auto& node=all.at(handle);if(!node.pending_set)return 0;
        if(std::any_of(all.begin(),all.end(),[&](const auto& item){return item.first!=handle && !item.second.pending_set && folded(item.second.id)==folded(node.id);}))return 0xe0000207U;
        auto saved=node;node.pending_set=0;node.present=true;node.status=0x0401U; // DN_HAS_PROBLEM
        if(!node.properties.contains(11))number_property(node,11,u"ConfigFlags",0x40U); // CONFIGFLAG_FINISH_INSTALL
        node.problem=setup_problem(node);
        if(const auto error=store_node(node)){node=std::move(saved);return error;}return 0;
    };
    const auto remove_node=[&](std::uint32_t handle)->std::uint32_t{
        const auto& node=all.at(handle);if(!node.setup_created)return 50;
        if(!node.pending_set)if(const auto error=erase_node(node))return error;
        // 已安裝驅動的裝置一併刪除其驅動軟體鍵（Control\\Class\\{guid}\\NNNN）。
        if(const auto driver=property_text(node,10);!node.pending_set && !driver.empty())
            if(const auto error=erase_registry_tree(runtime_.registry,registry_file_,std::u16string(1,2)+u"\\SYSTEM\\CurrentControlSet\\Control\\Class\\"+driver))return error;
        for(auto& [owner,other]:configuration.sets){(void)owner;std::erase(other.nodes,handle);std::erase_if(other.interfaces,[&](const auto& item){return item.second.node==handle;});
            std::erase_if(other.driver_lists,[&](const auto& item){return item.first.first==handle;});other.install_params.erase(handle);}
        all.erase(handle);return 0;
    };
    if(name.starts_with("SetupDiCreateDeviceInfo")){
        // SetupDiCreateDeviceInfo(set, DeviceName, ClassGuid, DeviceDescription, hwndParent, CreationFlags, DeviceInfoData)
        const auto flags=static_cast<std::uint32_t>(arg(5));if(flags&~3U)return failed(1004);
        if(!arg(2))return failed(87);if(arg(6) && memory.load(GuestAddress{arg(6)},32)!=element_size)return failed(1784);
        std::array<std::uint8_t,16> guid{};memory.read(GuestAddress{arg(2)},guid);if(set.filtered && set.class_guid!=guid)return failed(0xe0000201U);
        auto requested=read(arg(1));for(auto& c:requested)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
        std::u16string id;
        if(flags&1U){
            // DICD_GENERATE_ID：名稱為 root 列舉的裝置名稱，實例編號取最小未使用的四位十進位。
            if(requested.empty() || requested.find(u'\\')!=requested.npos || requested.size()>200)return failed(0xe0000205U);
            for(unsigned number=0;number<10000 && id.empty();++number){
                const auto digits=std::to_string(number);auto candidate=u"ROOT\\"+requested+u"\\"+std::u16string(4-digits.size(),u'0');candidate.append(digits.begin(),digits.end());
                if(std::none_of(all.begin(),all.end(),[&](const auto& item){return folded(item.second.id)==folded(candidate);}))id=std::move(candidate);
            }
            if(id.empty())return failed(0xe0000207U);
        }else{
            const auto first=requested.find(u'\\'),second=first==requested.npos?requested.npos:requested.find(u'\\',first+1);
            if(second==requested.npos || !first || second==first+1 || second+1>=requested.size() || requested.find(u'\\',second+1)!=requested.npos || requested.size()>200)return failed(0xe0000205U);
            if(std::any_of(all.begin(),all.end(),[&](const auto& item){return folded(item.second.id)==folded(requested);}))return failed(0xe0000207U);
            id=std::move(requested);
        }
        if(all.size()>=65536 || set.nodes.size()>=65536)return failed(8);
        while(all.contains(configuration.next_dynamic_node)){if(configuration.next_dynamic_node==UINT32_MAX)return failed(8);++configuration.next_dynamic_node;}
        DeviceNode node;node.id=std::move(id);node.enumerator=node.id.substr(0,node.id.find(u'\\'));node.parent=1;node.present=false;node.status=0;node.problem=0;
        node.guid=guid;node.class_guid=guid_name(guid);node.pending_set=set_handle;node.setup_created=true;
        text_property(node,9,u"ClassGUID",node.class_guid);text_property(node,23,u"EnumeratorName",node.enumerator);
        if(arg(3))text_property(node,1,u"DeviceDesc",read(arg(3)));
        // 類別名稱取自已安裝類別（Control\Class\{guid}\Class）或同類別 devnode。
        {RegistryTransaction view(runtime_.registry,registry_file_,nullptr,true);
            const auto key=runtime_.registry.keys.find(std::u16string(1,2)+u"\\system\\currentcontrolset\\control\\class\\"+folded(node.class_guid));
            std::u16string class_name;
            if(key!=runtime_.registry.keys.end())if(const auto value=key->second.find(u"class");value!=key->second.end() && value->second.type==1){
                const auto& bytes=value->second.bytes;for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(std::uint16_t{bytes[i+1]}<<8));if(!c)break;class_name.push_back(c);}}
            if(class_name.empty())for(const auto& [other_handle,other]:all){(void)other_handle;if(other.guid==guid){class_name=text_of(other,8);if(!class_name.empty())break;}}
            if(!class_name.empty())text_property(node,8,u"Class",class_name);}
        const auto handle=configuration.next_dynamic_node++;all.emplace(handle,std::move(node));set.nodes.push_back(handle);
        if(arg(6))publish(arg(6),handle);return finish(1);
    }
    if(name.starts_with("SetupDiSetDeviceRegistryProperty")){
        const auto handle=element(element_pointer);if(!handle)return failed(87);auto& node=all.at(*handle);
        const auto property=static_cast<std::uint32_t>(arg(2)),size=static_cast<std::uint32_t>(arg(4));
        // 可由安裝程式寫入的 SPDRP；其餘（Service、Class、Driver、Mfg、Capabilities……）由安裝流程決定。
        constexpr std::array<std::uint32_t,13> settable{0,1,2,0xa,0xc,0xd,0x11,0x12,0x19,0x1a,0x1b,0x1d,0x21};
        const auto writable=std::find_if(setup_property_names.begin(),setup_property_names.end(),[&](const auto& item){return item.first==property+1;});
        if(writable==setup_property_names.end() || std::find(settable.begin(),settable.end(),property)==settable.end())return failed(0xe0000209U);
        if(!arg(3) && size)return failed(87);
        const bool number=property==10 || property==0x19 || property==0x1a || property==0x1b || property==0x21,multiple=property==1 || property==2 || property==0x11 || property==0x12;
        auto saved=node;
        if(!arg(3)){node.properties.erase(property+1);}
        else{
            if(size>65536)return failed(13);std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{arg(3)},bytes);
            RegistryValue value;value.display_name=std::u16string(writable->second);value.type=number?4U:multiple?7U:1U;
            if(number){if(size!=4)return failed(13);value.bytes=std::move(bytes);}
            else if(wide){if(size%2)return failed(13);value.bytes=std::move(bytes);}
            else{
                std::u16string text;std::string part;
                for(const auto byte:bytes){if(byte){part.push_back(static_cast<char>(byte));continue;}text+=utf8_to_utf16(part);text.push_back(0);part.clear();}
                if(!part.empty()){text+=utf8_to_utf16(part);text.push_back(0);}value.bytes=utf16_bytes(text);
            }
            // 字串值補足 NUL 結尾；REG_MULTI_SZ 另補清單結尾。
            if(!number){const auto ends_with_nul=[&]{const auto n=value.bytes.size();return n>=2 && !value.bytes[n-1] && !value.bytes[n-2];};
                if(!ends_with_nul()){value.bytes.push_back(0);value.bytes.push_back(0);}
                if(multiple && (value.bytes.size()<4 || value.bytes[value.bytes.size()-3] || value.bytes[value.bytes.size()-4])){value.bytes.push_back(0);value.bytes.push_back(0);}}
            node.properties.insert_or_assign(property+1,std::pair{value.display_name,std::move(value)});
        }
        if(!node.pending_set)if(const auto error=store_node(node)){node=std::move(saved);return failed(error);}
        return finish(1);
    }
    if(name.find("ClassInstallParams")!=name.npos){
        // SP_CLASSINSTALL_HEADER{cbSize=8, InstallFunction} 加上各 DIF 的參數；NULL 清除。
        std::uint32_t key=0;if(element_pointer){const auto handle=element(element_pointer);if(!handle)return failed(87);key=*handle;}
        if(name.starts_with("SetupDiGetClassInstallParams")){
            const auto stored=set.install_params.find(key);if(stored==set.install_params.end())return failed(0xe0000215U);
            const auto capacity=static_cast<std::uint32_t>(arg(3));if(arg(4))memory.store(GuestAddress{arg(4)},32,stored->second.size());
            if(!arg(2) || memory.load(GuestAddress{arg(2)},32)!=8)return failed(1784);if(capacity<stored->second.size())return failed(122);
            memory.write(GuestAddress{arg(2)},stored->second);return finish(1);
        }
        const auto size=static_cast<std::uint32_t>(arg(3));if(!arg(2)){if(size)return failed(87);set.install_params.erase(key);return finish(1);}
        if(size<8 || memory.load(GuestAddress{arg(2)},32)!=8)return failed(1784);
        const auto function=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+4},32));
        // DIF_REMOVE／DIF_UNREMOVE：Scope、HwProfile；DIF_PROPERTYCHANGE：StateChange、Scope、HwProfile。
        const auto expected=function==5 || function==0x16?16U:function==0x12?20U:0U;if(!expected || size!=expected)return failed(87);
        std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{arg(2)},bytes);
        const auto field=[&](unsigned at){return dword(bytes,at);};
        if(function==5 && field(8)!=1 && field(8)!=2)return failed(87);
        if(function==0x12 && (field(8)<1 || field(8)>5 || (field(12)!=1 && field(12)!=2)))return failed(87);
        set.install_params.insert_or_assign(key,std::move(bytes));return finish(1);
    }
    if(name=="DiUninstallDevice" && arg(3))return failed(1004);
    std::uint32_t function=name=="SetupDiCallClassInstaller"?static_cast<std::uint32_t>(arg(0)):name=="SetupDiRegisterDeviceInfo"?0x19U:5U;
    const auto handle=element(element_pointer);
    const auto needs_element=function==0x19 || function==5 || function==0x12 || function==2 || function==0x16;
    if(needs_element && !handle)return failed(87);
    if(needs_element && x86_)return failed(0xe0000235U); // ERROR_IN_WOW64：32 位元程序不可在 64 位元系統安裝裝置。
    if(needs_element && !administrator())return failed(5);
    const auto params=[&]()->std::optional<std::vector<std::uint8_t>>{
        for(const auto key:{handle.value_or(0),0U})if(const auto found=set.install_params.find(key);found!=set.install_params.end() && found->second.size()>=8 &&
            dword(found->second,4)==function)return found->second;
        return std::nullopt;
    };
    switch(function){
    case 0x19:{if(const auto error=register_node(*handle))return failed(error);return finish(1);}
    case 5:{
        const auto removal=params();if(removal && (*removal)[8]==2)return failed(50); // 硬體設定檔專屬移除不適用於單一設定檔。
        if(const auto error=remove_node(*handle))return failed(error);
        if(name=="DiUninstallDevice" && arg(4))memory.store(GuestAddress{arg(4)},32,0); // 不需重新開機。
        return finish(1);
    }
    case 0x12:{
        auto& node=all.at(*handle);if(node.pending_set)return failed(0xe0000208U);if(!node.setup_created)return failed(50);
        const auto change=params();if(!change)return failed(0xe0000215U);const auto transition=(*change)[8];
        auto saved=node;auto flags=0U;if(const auto found=node.properties.find(11);found!=node.properties.end() && found->second.second.bytes.size()==4)flags=dword(found->second.second.bytes,0);
        // DICS_DISABLE 設 CONFIGFLAG_DISABLED 與 CM_PROB_DISABLED；DICS_ENABLE 還原為未安裝驅動狀態。
        if(transition==2)flags|=1U;else if(transition==1)flags&=~1U;
        node.properties.erase(11);number_property(node,11,u"ConfigFlags",flags);node.problem=setup_problem(node);node.status=0x0401U;
        if(const auto error=store_node(node)){node=std::move(saved);return failed(error);}return finish(1);
    }
    case 2:return failed(0xe0000203U); // DIF_INSTALLDEVICE：尚未選取驅動程式。
    case 0x16:return failed(50);
    default:return failed(0xe000020eU); // ERROR_DI_DO_DEFAULT：沒有類別安裝程式處理此 DIF。
    }
}
