        // 正規化兩套 API 的索引與容量單位；外層只轉換 Win32／CONFIGRET 的回傳方式。
        const auto interface_property_call=[&](const DeviceNode& node,const WindowsGuid& guid,bool cm)->std::uint32_t{
            const bool keys=name.find("Keys")!=name.npos || name.find("_Keys")!=name.npos;
            const bool setting=name.starts_with("CM_Set_") || name.starts_with("SetupDiSet");
            const auto key_at=arg(cm?1:2),buffer=arg(keys?(cm?1:2):(cm?3:4));
            const auto count_at=keys?arg(cm?2:4):setting?0:arg(cm?4:6);
            const auto type_at=keys || setting?0:arg(cm?2:3);
            if((!keys && !key_at) || (!keys && !setting && !type_at))return cm?1784U:87U;
            if(cm && !setting && !count_at)return 1784;
            const auto capacity=static_cast<std::uint32_t>(cm && !setting?memory.load(GuestAddress{count_at},32):arg(keys?3:cm?4:5));
            if(!buffer && capacity)return 1784;
            DevicePropertyKey key;if(!keys){key=read_device_property_key(memory,key_at);if(!valid_device_property_key(key))return 0xe0000209U;}
            UnifiedDeviceProperty input;
            if(setting){
                input.type=static_cast<std::uint32_t>(arg(cm?2:3));
                if(input.type<2 && (buffer || capacity))return 13;
                if(capacity>65534)return 13;
                input.bytes.resize(capacity);if(capacity)memory.read(GuestAddress{buffer},input.bytes);
                if(!valid_device_property_value(input))return 13;
                if(key==interface_friendly && input.type>1 && input.type!=18)return 13;
                if(readonly_interface_property(key))return 5;
                initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());
                const auto admins=security::account_sid(*security::find_account("Administrators"));
                if(token.integrity<0x3000U || !std::any_of(token.groups.begin(),token.groups.end(),[&](const auto& group){return group.sid==admins && (group.attributes&0x14U)==4U;}))return 5;
            }
            auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_,!setting);
            if(transaction.error)return transaction.error;
            std::uint32_t error=0;const auto values=interface_properties(node,guid,registry,error);if(error)return error;
            if(setting){if(!input.type && !values.contains(key))return 1168;return save_interface_property(registry,transaction,node,guid,key,input,output_limit);}
            std::vector<std::uint8_t> bytes;std::uint32_t type=0,required=0;
            if(keys){
                required=static_cast<std::uint32_t>(values.size());bytes.reserve(values.size()*20);
                for(const auto& [item,value]:values){(void)value;bytes.insert(bytes.end(),item.format.begin(),item.format.end());for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(item.id>>(8*i)));}
            }else{
                const auto found=values.find(key);if(found==values.end())return 1168;
                type=found->second.type;bytes=found->second.bytes;required=static_cast<std::uint32_t>(bytes.size());
            }
            const bool fits=capacity>=required && (buffer || !required);
            if(type_at)memory.check(GuestAddress{type_at},4,Access::Write);if(count_at)memory.check(GuestAddress{count_at},4,Access::Write);
            if(fits && !bytes.empty())memory.check(GuestAddress{buffer},bytes.size(),Access::Write);
            if(type_at)memory.store(GuestAddress{type_at},32,type);if(count_at)memory.store(GuestAddress{count_at},32,required);
            if(!fits)return 122;if(!bytes.empty())memory.write(GuestAddress{buffer},bytes);return 0;
        };
        if(name.starts_with("CM_Get_Device_Interface_Property") || name.starts_with("CM_Set_Device_Interface_Property")){
            const bool keys=name.find("_Keys")!=name.npos,extended=name.find("_ExW")!=name.npos;
            if(arg(keys?3:5))return finish(4);if(extended && arg(keys?4:6))return finish(49);if(!arg(0))return finish(3);
            const auto path=folded(read(arg(0)));
            const auto found=std::find_if(nodes.begin(),nodes.end(),[&](const auto& item){return !item.second.pending_set && !item.second.interface_path.empty() && folded(item.second.interface_path)==path;});
            if(found==nodes.end())return finish(55);
            const auto error=interface_property_call(found->second,found->second.interface_guid,true);
            switch(error){case 0:return finish(0);case 1784:return finish(3);case 8:return finish(2);case 5:return finish(51);case 122:return finish(26);
                case 1168:return finish(37);case 0xe0000209U:return finish(53);case 13:case 87:return finish(31);default:return finish(29);}
        }
