        // DiInstallDriver：套件放入 DriverStore 後綁定符合的 devnode。功能由原生後端提供，不執行 INF 安裝段落與核心驅動；
        // 有宿主硬體的 devnode 維持啟動，SetupAPI 建立的 root 虛擬裝置沒有可載入的原生後端（CM_PROB_DRIVER_FAILED_LOAD）。
        // DiUninstallDriver：解除綁定並移除套件。
        if(name.starts_with("DiInstallDriver") || name.starts_with("DiUninstallDriver")){
            const bool install=name.starts_with("DiInstallDriver");
            const auto failed=[&](std::uint32_t error){last_error_=error;return finish(0);};
            const auto administrator=[&]{
                initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());
                return token.integrity>=0x3000U && std::any_of(token.groups.begin(),token.groups.end(),[](const auto& group){return group.sid==security::account_sid(*security::find_account("Administrators")) && (group.attributes&0x14U)==4U;});
            };
            // (hwndParent, InfPath, Flags, NeedReboot)
            const auto flags=static_cast<std::uint32_t>(arg(2));
            if(flags&~(install?0x1aU:0x1U))return failed(1004);
            if(!arg(1))return failed(87);
            if(x86_)return failed(0xe0000235U); // ERROR_IN_WOW64
            if(!administrator())return failed(5);
            std::u16string inf;try{inf=normalized_windows_path(read(arg(1)),runtime_.current_directory);}catch(const std::invalid_argument&){return failed(87);}
            std::u16string published;
            if(install)published=driver_store::stage_package(filesystem_,inf,0).published;
            else {
                const auto lower=folded(inf),directory=folded(std::u16string(driver_store::inf_directory)+u"\\");
                if(lower.starts_with(directory) && lower.find(u'\\',directory.size())==lower.npos){if(const auto info=filesystem_.information(utf16_to_utf8(inf));info.error)return failed(info.error);published=inf;}
                else if(const auto copy=driver_store::published_copy(filesystem_,*driver_store::read_file(filesystem_,inf)))published=*copy;
                else return failed(2);
            }
            const auto oem=driver_store::leaf_of(published);
            refresh_device_configuration(false);auto& nodes=device_configuration_->nodes;
            const auto class_root=std::u16string(1,2)+u"\\SYSTEM\\CurrentControlSet\\Control\\Class\\";
            const auto multi=[&](const DeviceNode& node,std::uint32_t property){
                std::vector<std::u16string> list;const auto found=node.properties.find(property);if(found==node.properties.end())return list;std::u16string item;const auto& bytes=found->second.second.bytes;
                for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(std::uint16_t{bytes[i+1]}<<8));if(c)item.push_back(c);else if(!item.empty()){list.push_back(folded(item));item.clear();}}
                return list;
            };
            const auto installed_inf=[&](const DeviceNode& node)->std::u16string{
                RegistryTransaction view(runtime_.registry,registry_file_,nullptr,true);const auto driver=property_text(node,10);if(driver.empty())return {};
                const auto key=runtime_.registry.keys.find(folded(class_root+driver));if(key==runtime_.registry.keys.end())return {};
                const auto value=key->second.find(u"infpath");if(value==key->second.end())return {};
                std::u16string text;for(std::size_t i=0;i+1<value->second.bytes.size();i+=2){const auto c=static_cast<char16_t>(value->second.bytes[i]|(std::uint16_t{value->second.bytes[i+1]}<<8));if(!c)break;text.push_back(c);}return folded(text);
            };
            const auto text_value=[](std::u16string name_text,std::u16string_view value){RegistryValue data;data.type=1;data.bytes=utf16_bytes(value);data.display_name=name_text;return std::pair{std::move(name_text),std::move(data)};};
            const std::vector<std::u16string> driver_values{u"DriverDesc",u"ProviderName",u"DriverDate",u"DriverVersion",u"MatchingDeviceId",u"InfPath",u"InfSection"};
            if(install){
                const auto models=driver_store::models(filesystem_,published,false);
                const auto inf_sections=driver_store::parse(*driver_store::read_file(filesystem_,published));
                const auto class_name=driver_store::expand(inf_sections,driver_store::value(inf_sections,u"version",u"class"));
                for(auto& [handle,node]:nodes){
                    if(node.pending_set || !node.present)continue;
                    // 硬體 ID 優先於相容 ID，愈前面的 ID 等級愈高。
                    const auto hardware=multi(node,2),compatible=multi(node,3);std::optional<std::pair<std::size_t,const driver_store::Model*>> best;
                    for(const auto& model:models){
                        if(!node.setup_created && model.class_guid!=node.guid)continue; // 原生裝置的類別由宿主功能決定。
                        for(std::size_t rank=0;rank<hardware.size()+compatible.size();++rank){
                            const auto& id=rank<hardware.size()?hardware[rank]:compatible[rank-hardware.size()];const auto score=rank<hardware.size()?rank:0x1000+rank;
                            if(std::find(model.ids.begin(),model.ids.end(),id)!=model.ids.end()){if(!best || score<best->first)best=std::pair{score,&model};break;}
                        }
                    }
                    if(!best)continue;const auto& model=*best->second;
                    const auto current=installed_inf(node);if(current==folded(oem) || (!current.empty() && !(flags&2U)))continue;
                    // 驅動軟體鍵：原生裝置沿用既有 Driver 鍵；root 虛擬裝置配置 Control\Class\{guid}\NNNN。
                    std::u16string driver=property_text(node,10);
                    if(node.setup_created){
                        const auto guid_text=guid_name(model.class_guid);
                        if(driver.empty() || folded(driver.substr(0,driver.find(u'\\')))!=guid_text){
                            RegistryTransaction view(runtime_.registry,registry_file_,nullptr,true);
                            for(unsigned number=0;number<10000;++number){const auto digits=std::to_string(number);auto candidate=guid_text+u"\\"+std::u16string(4-digits.size(),u'0');candidate.append(digits.begin(),digits.end());
                                if(!runtime_.registry.keys.contains(folded(class_root+candidate))){driver=std::move(candidate);break;}}
                            if(driver.empty())return failed(8);
                        }
                        node.guid=model.class_guid;node.class_guid=guid_text;
                        for(const auto property:{1U,8U,9U,10U,12U})node.properties.erase(property);
                        text_property(node,1,u"DeviceDesc",model.description);text_property(node,9,u"ClassGUID",guid_text);if(!class_name.empty())text_property(node,8,u"Class",class_name);
                        text_property(node,10,u"Driver",driver);if(!model.manufacturer.empty())text_property(node,12,u"Mfg",model.manufacturer);
                        if(const auto error=write_enum_key(runtime_.registry,registry_file_,node,output_limit))return failed(error);
                        node.problem=setup_problem(node);node.status=0x0401U;
                    }else if(driver.empty())continue;
                    // DriverDate 為 M-D-YYYY、DriverVersion 為 a.b.c.d，與 INF DriverVer 一致。
                    std::u16string date_text,version_text;
                    if(model.date){const auto days=std::chrono::sys_days{std::chrono::days{static_cast<long>(model.date/UINT64_C(864000000000))-134774}};const std::chrono::year_month_day ymd{days};
                        date_text=utf8_to_utf16(std::to_string(static_cast<unsigned>(ymd.month()))+"-"+std::to_string(static_cast<unsigned>(ymd.day()))+"-"+std::to_string(static_cast<int>(ymd.year())));}
                    if(model.version)version_text=utf8_to_utf16(std::to_string(model.version>>48)+"."+std::to_string((model.version>>32)&0xffff)+"."+std::to_string((model.version>>16)&0xffff)+"."+std::to_string(model.version&0xffff));
                    const auto matched=best->first<hardware.size()?hardware[best->first]:compatible[best->first-0x1000-hardware.size()];
                    std::vector<std::pair<std::u16string,RegistryValue>> values{text_value(u"DriverDesc",model.description),text_value(u"ProviderName",model.provider),
                        text_value(u"MatchingDeviceId",matched),text_value(u"InfPath",oem),text_value(u"InfSection",model.section)};
                    if(!date_text.empty())values.push_back(text_value(u"DriverDate",date_text));if(!version_text.empty())values.push_back(text_value(u"DriverVersion",version_text));
                    if(const auto error=write_registry_key(runtime_.registry,registry_file_,class_root+driver,values,{},output_limit))return failed(error);
                    (void)handle;
                }
            }else{
                for(auto& [handle,node]:nodes){
                    (void)handle;if(node.pending_set || installed_inf(node)!=folded(oem))continue;const auto driver=property_text(node,10);
                    if(node.setup_created){
                        if(const auto error=erase_registry_tree(runtime_.registry,registry_file_,class_root+driver))return failed(error);
                        node.properties.erase(10);node.properties.erase(12);
                        if(const auto error=write_enum_key(runtime_.registry,registry_file_,node,output_limit))return failed(error);
                        node.problem=setup_problem(node);
                    }else if(const auto error=write_registry_key(runtime_.registry,registry_file_,class_root+driver,{},driver_values,output_limit))return failed(error);
                }
                if(!(flags&1U))driver_store::remove_package(filesystem_,published); // DIURFLAG_NO_REMOVE_INF 時保留套件。
            }
            if(arg(3))memory.store(GuestAddress{arg(3)},32,0); // 不需重新開機。
            return finish(1);
        }
        // SetupCopyOEMInf／SetupUninstallOEMInf：驅動套件放入或移出 sandbox DriverStore；需系統管理員權限。
        if(name.starts_with("SetupCopyOEMInf") || name.starts_with("SetupUninstallOEMInf")){
            const auto failed=[&](std::uint32_t error){last_error_=error;return finish(0);};
            const auto administrator=[&]{
                initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());
                return token.integrity>=0x3000U && std::any_of(token.groups.begin(),token.groups.end(),[](const auto& group){return group.sid==security::account_sid(*security::find_account("Administrators")) && (group.attributes&0x14U)==4U;});
            };
            if(name.starts_with("SetupUninstallOEMInf")){
                // SetupUninstallOEMInf(InfFileName, Flags, Reserved)：只接受 %windir%\INF 下的 oemN.inf 檔名。
                if(arg(2) || (arg(1)&~1U))return failed(87);const auto file=read(arg(0));const auto lower=folded(file);
                if(!lower.starts_with(u"oem") || !lower.ends_with(u".inf") || lower.size()<8 || !std::all_of(lower.begin()+3,lower.end()-4,[](char16_t c){return c>=u'0' && c<=u'9';}))return failed(87);
                if(!administrator())return failed(5);
                const auto published=std::u16string(driver_store::inf_directory)+u"\\"+file;
                if(const auto info=filesystem_.information(utf16_to_utf8(published));info.error)return failed(info.error);
                // 仍有 devnode 的驅動軟體鍵指向此 INF 時，未強制則不可移除。
                if(!(arg(1)&1U)){
                    refresh_device_configuration(false);RegistryTransaction view(runtime_.registry,registry_file_,nullptr,true);
                    for(const auto& [handle,node]:device_configuration_->nodes){(void)handle;const auto driver=node.properties.find(10);if(driver==node.properties.end())continue;
                        std::u16string key;const auto& bytes=driver->second.second.bytes;for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(std::uint16_t{bytes[i+1]}<<8));if(!c)break;key.push_back(c);}
                        const auto values=runtime_.registry.keys.find(folded(std::u16string(1,2)+u"\\system\\currentcontrolset\\control\\class\\"+key));if(values==runtime_.registry.keys.end())continue;
                        const auto inf=values->second.find(u"infpath");if(inf!=values->second.end() && folded(std::u16string(reinterpret_cast<const char16_t*>(inf->second.bytes.data()),inf->second.bytes.size()/2)).starts_with(lower))return failed(0xe000023dU); // ERROR_INF_IN_USE_BY_DEVICES
                    }
                }
                driver_store::remove_package(filesystem_,published);return finish(1);
            }
            // SetupCopyOEMInf(Source, MediaLocation, MediaType, CopyStyle, Destination, DestinationSize, RequiredSize, DestinationComponent)
            if(!arg(0) || arg(2)>2 || (!arg(4) && arg(5)))return failed(87);
            if(!administrator())return failed(5);
            std::u16string source;try{source=normalized_windows_path(read(arg(0)),runtime_.current_directory);}catch(const std::invalid_argument&){return failed(87);}
            const auto style=static_cast<std::uint32_t>(arg(3));const auto staged=driver_store::stage_package(filesystem_,source,style);
            const auto ansi=utf16_to_utf8(staged.published);const auto length=(wide?staged.published.size():ansi.size())+1;
            const auto leaf=wide?staged.published.find_last_of(u'\\')+1:ansi.find_last_of('\\')+1;
            if(arg(6))memory.store(GuestAddress{arg(6)},32,length);
            const bool fits=arg(4) && arg(5)>=length;
            if(fits){auto bytes=wide?utf16_bytes(staged.published):std::vector<std::uint8_t>(ansi.begin(),ansi.end());if(!wide)bytes.push_back(0);memory.write(GuestAddress{arg(4)},bytes);
                if(arg(7))memory.store(GuestAddress{arg(7)},x86_?32:64,arg(4)+leaf*(wide?2U:1U));}
            if(staged.existed && (style&8U))return failed(80); // SP_COPY_NOOVERWRITE：已發佈，回傳既有名稱。
            if(arg(4) && !fits)return failed(122);
            return finish(1);
        }
