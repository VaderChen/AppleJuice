            if(name=="SetupDiBuildDriverInfoList" || name=="SetupDiDestroyDriverInfoList" || name.starts_with("SetupDiEnumDriverInfo")){
                const auto type=static_cast<std::uint32_t>(arg(2));if(type!=1 && type!=2)return failed(87);
                std::uint32_t device=0;if(arg(1)){const auto node=read_node(arg(1));if(!node)return failed(87);device=*node;}else if(type==2)return failed(87);
                const auto key=std::pair{device,type};
                if(name=="SetupDiDestroyDriverInfoList"){info.driver_lists.erase(key);return finish(1);}
                if(name=="SetupDiBuildDriverInfoList"){
                    std::set<std::u16string> hardware;WindowsGuid guid=device?nodes.at(device).guid:info.class_guid;const bool filter=device || info.filtered;
                    if(device)for(const auto property:{2U,3U})if(const auto entry=nodes.at(device).properties.find(property);entry!=nodes.at(device).properties.end()){
                        std::u16string id;const auto& bytes=entry->second.second.bytes;
                        for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(std::uint16_t{bytes[i+1]}<<8));if(c)id+=c;else if(!id.empty()){hardware.insert(driver_store::fold(std::move(id)));id.clear();}}
                    }
                    std::vector<driver_store::Model> list;auto next=configuration.next_driver;
                    for(const auto& file:driver_store::inf_files(filesystem_,std::u16string(driver_store::inf_directory)))for(auto model:driver_store::models(filesystem_,file,false)){
                        if(filter && model.class_guid!=guid)continue;
                        if(type==2 && std::none_of(model.ids.begin(),model.ids.end(),[&](const auto& id){return hardware.contains(id);}))continue;
                        if(type==1 && model.excluded)continue;if(list.size()>=4096 || next>UINT32_MAX)return failed(8);
                        model.token=next++;list.push_back(std::move(model));
                    }
                    info.driver_lists.insert_or_assign(key,std::move(list));configuration.next_driver=next;return finish(1);
                }
                const auto found=info.driver_lists.find(key);const auto index=static_cast<std::uint32_t>(arg(3));
                if(found==info.driver_lists.end() || index>=found->second.size())return failed(259);
                const auto unit=x86_?4U:8U,start=8+unit,string_size=wide?512U:256U,v1=start+string_size*3,v2=v1+16;
                if(!arg(4))return failed(1784);const auto size=memory.load(GuestAddress{arg(4)},32);if(size!=v1 && size!=v2)return failed(1784);
                memory.check(GuestAddress{arg(4)},size,Access::Write);const auto& driver=found->second[index];std::vector<std::uint8_t> bytes(size);
                const auto put=[&](unsigned at,std::uint64_t value,unsigned length){for(unsigned i=0;i<length;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
                put(0,size,4);put(4,type,4);put(8,driver.token,unit);
                const std::array texts{driver.description,driver.manufacturer,driver.provider};for(unsigned i=0;i<3;++i){
                    std::vector<std::uint8_t> text;if(wide)text=utf16_bytes(texts[i].substr(0,255));else {const auto value=utf16_to_utf8(texts[i]);text.assign(value.begin(),value.begin()+static_cast<std::ptrdiff_t>(std::min<std::size_t>(255,value.size())));text.push_back(0);}
                    std::copy(text.begin(),text.end(),bytes.begin()+start+string_size*i);
                }
                if(size==v2){put(v1,driver.date,8);put(v1+8,driver.version,8);}memory.write(GuestAddress{arg(4)},bytes);return finish(1);
            }
            if(name.starts_with("SetupDiGetDriverInfoDetail")){
                // SetupDiGetDriverInfoDetail(set, device, SP_DRVINFO_DATA, SP_DRVINFO_DETAIL_DATA, size, required)：依列舉時的 token 找回驅動節點。
                std::uint32_t device=0;if(arg(1)){const auto node=read_node(arg(1));if(!node)return failed(87);device=*node;}
                const auto unit=x86_?4U:8U,start=8+unit,string_size=wide?512U:256U,v1=start+string_size*3,v2=v1+16;
                if(!arg(2))return failed(87);const auto data_size=memory.load(GuestAddress{arg(2)},32);if(data_size!=v1 && data_size!=v2)return failed(1784);
                const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+4},32));const auto token=memory.load(GuestAddress{arg(2)+8},static_cast<std::uint8_t>(unit*8));
                const auto list=info.driver_lists.find(std::pair{device,type});if(list==info.driver_lists.end())return failed(87);
                const auto driver=std::find_if(list->second.begin(),list->second.end(),[&](const auto& item){return item.token==token;});if(driver==list->second.end())return failed(87);
                // HardwareID 欄位為 MULTI_SZ：第一個為硬體 ID，其後為相容 ID，清單以額外 NUL 結尾。
                std::u16string ids;for(const auto& id:driver->ids){ids+=id;ids.push_back(0);}ids.push_back(0);
                const auto encoded=utf16_to_utf8(ids);const auto unit_size=wide?2U:1U;
                const auto first=driver->ids.empty()?0U:static_cast<std::uint32_t>((wide?driver->ids[0].size():utf16_to_utf8(driver->ids[0]).size())+1);
                const auto compat=driver->ids.size()>1?static_cast<std::uint32_t>((wide?ids.size():encoded.size())-first):0U;
                const unsigned section_at=x86_?24U:32U,name_size=wide?2U:1U,file_at=section_at+256*name_size,description_at=file_at+260*name_size,hardware_at=description_at+256*name_size;
                const auto structure=x86_?(wide?1570U:797U):(wide?1584U:808U);
                const auto list_bytes=wide?ids.size()*2:encoded.size();const auto required=hardware_at+list_bytes;
                if(arg(5))memory.store(GuestAddress{arg(5)},32,required);
                if(!arg(3)){if(arg(4))return failed(87);return failed(122);}
                if(memory.load(GuestAddress{arg(3)},32)!=structure || arg(4)<structure)return failed(1784);
                const bool fits=arg(4)>=required;const auto written=fits?required:structure;memory.check(GuestAddress{arg(3)},written,Access::Write);
                std::vector<std::uint8_t> bytes(written);
                const auto put=[&](std::size_t at,std::uint64_t value,unsigned length){for(unsigned i=0;i<length;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
                const auto text=[&](std::size_t at,const std::u16string& value,std::size_t limit){
                    std::vector<std::uint8_t> encoded_text;if(wide)encoded_text=utf16_bytes(value.substr(0,limit-1));else {const auto narrow=utf16_to_utf8(value);encoded_text.assign(narrow.begin(),narrow.begin()+static_cast<std::ptrdiff_t>(std::min(narrow.size(),limit-1)));}
                    std::copy(encoded_text.begin(),encoded_text.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));};
                put(0,structure,4);put(4,driver->date,8);put(12,first,4);put(16,compat,4);
                text(section_at,driver->section,256);text(file_at,driver->filename,260);text(description_at,driver->description,256);
                if(fits){if(wide){const auto raw=utf16_bytes(ids);std::copy(raw.begin(),raw.end(),bytes.begin()+hardware_at);}else std::copy(encoded.begin(),encoded.end(),bytes.begin()+hardware_at);}
                (void)unit_size;memory.write(GuestAddress{arg(3)},bytes);if(!fits)return failed(122);return finish(1);
            }
