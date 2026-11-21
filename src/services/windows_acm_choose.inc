    if(name.starts_with("acmFormatChoose") || name.starts_with("acmFilterChoose")){
        const auto address=arg(0);
        const x64arm::acm::ChooseAbi abi{pointer,cu};
        if(!address || memory.load(GuestAddress{address},32)<abi.size() || acm.dialogs.contains(address))
            throw AcmError{11};
        const auto field=[&](unsigned offset){return static_cast<unsigned>(memory.load(GuestAddress{address+offset},32));};
        const auto slot=[&](unsigned offset){return read_pointer(address+offset);};
        const auto source=slot(8+pointer);
        const auto style=field(4);
        const auto capacity=field(8+2*pointer);
        if(!source || !capacity)throw AcmError{11};
        if(style&~0xfcU || (style&0x30U)==0x30U)throw AcmError{10};
        auto dialog=std::make_shared<x64arm::acm::ChoiceDialog>();
        dialog->filter=name.starts_with("acmFilter");
        dialog->style=style;
        dialog->owner=slot(8);
        if(dialog->owner && !gui_.windows.contains(dialog->owner))throw AcmError{5};
        if(style&8U){
            dialog->hook=slot(abi.hook());
            dialog->custom=slot(abi.custom());
            if(!dialog->hook)throw AcmError{11};
            memory.check(GuestAddress{dialog->hook},1,Access::Execute);
        }
        const auto read_text=[&](std::uint64_t ptr,unsigned limit){
            return !ptr?std::u16string{}:wide_out?read_utf16(memory,GuestAddress{ptr},limit):
                                                   utf8_to_utf16_lossy(read_ansi(memory,ptr,limit));
        };
        const auto name_buffer=slot(abi.name());
        if(name_buffer){
            dialog->name_capacity=field(abi.name_capacity());
            if(!dialog->name_capacity)throw AcmError{11};
            dialog->name=read_text(name_buffer,std::min(dialog->name_capacity,1024U));
        }
        auto title=read_text(slot(abi.title()),1024);
        if(title.empty())title=dialog->filter?u"音訊濾波器":u"音訊格式";
        const auto flags=field(abi.enum_flags());
        const auto filter_format=slot(abi.enum_format());
        auto offered=dialog->filter?filters_matching(filter_format,flags,0):formats_matching(filter_format,flags,0);
        if(offered.empty())throw AcmError{6};
        unsigned initial_tag=0;
        std::vector<std::uint8_t> initial;
        if(style&0x40U){
            unsigned size=0;
            if(dialog->filter){
                if(capacity<32)throw AcmError{512};
                size=static_cast<unsigned>(memory.load(GuestAddress{source},32));
                initial_tag=static_cast<unsigned>(memory.load(GuestAddress{source+4},32));
                if(size<32 || size>65536)throw AcmError{11};
            }else{
                if(capacity<16)throw AcmError{512};
                initial_tag=static_cast<unsigned>(memory.load(GuestAddress{source},16));
                size=initial_tag==1?16U:capacity>=18?18U+static_cast<unsigned>(memory.load(GuestAddress{source+16},16)):0U;
            }
            if(size && size<=capacity){
                initial.resize(size);
                memory.read(GuestAddress{source},initial);
            }
        }
        bool initial_selected=false;
        for(auto& offer:offered){
            auto group=std::find_if(dialog->groups.begin(),dialog->groups.end(),[&](const auto& item){return item.tag==offer.tag;});
            if(group==dialog->groups.end()){
                dialog->groups.push_back({offer.tag,std::u16string(offer.label),{}});
                group=std::prev(dialog->groups.end());
                if(offer.tag==initial_tag)dialog->group=static_cast<unsigned>(group-dialog->groups.begin());
            }
            if(std::any_of(group->options.begin(),group->options.end(),[&](const auto& item){return item.bytes==offer.standard.bytes;}))
                continue;
            if(!initial_selected && !initial.empty() && offer.standard.bytes==initial){
                dialog->group=static_cast<unsigned>(group-dialog->groups.begin());
                dialog->option=static_cast<unsigned>(group->options.size());
                initial_selected=true;
            }
            group->options.push_back({std::move(offer.standard.text),std::move(offer.standard.bytes)});
        }
        if(dialog->filter && !initial_selected && !initial.empty() && initial_tag==x64arm::acm::volume_tag){
            auto& group=dialog->groups.at(dialog->group);
            if(group.tag==initial_tag && acm.enabled[0]){
                const auto gain=read_volume(memory,source);
                dialog->option=static_cast<unsigned>(group.options.size());
                group.options.push_back({volume_description(gain),x64arm::acm::volume_format(gain)});
            }
        }
        const auto instance=(style&0x30U)?slot(abi.instance()):0;
        const auto template_name=(style&0x10U)?slot(abi.template_name()):0;
        if((style&0x30U) && (!instance || ((style&0x10U) && !template_name)))throw AcmError{11};
        return nested_continuation(state,memory,"ACM 音訊選擇",
            [this,service,address,abi,dialog,source,capacity,instance,template_name,name_buffer,
             title=std::move(title)](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                const auto finish=[&](std::uint32_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
                auto& dialogs=acm_->dialogs;
                dialogs.emplace(address,dialog);
                struct Remove {decltype(dialogs)& items;std::uint64_t id;~Remove(){items.erase(id);}} remove{dialogs,address};
                GuestScratch scratch(*this,m,s);
                const auto api=[&](std::string_view symbol,std::vector<std::uint64_t> args)
                    ->service_callback::Task<std::uint64_t>{
                    co_return co_await service_callback::call_value(resolve("user32.dll",symbol).value,std::move(args));
                };
                try{
                    const auto proc=resolve("internal-acm-choice","DialogProc").value;
                    std::uint64_t answer=0;
                    if(dialog->style&0x10U){
                        answer=co_await api(abi.character==2?"DialogBoxParamW":"DialogBoxParamA",
                                            {instance,template_name,dialog->owner,proc,address});
                    }else{
                        auto template_data=instance;
                        if(!(dialog->style&0x20U)){
                            DialogTemplate item;
                            item.item(0,8,9,52,12,-1,0x82,u"",dialog->filter?u"濾波器":u"格式");
                            item.item(0x10203,66,7,168,75,x64arm::acm::tag_control,0x85,u"",u"");
                            item.item(0,8,29,52,12,-1,0x82,u"",u"選項");
                            item.item(0x10203,66,27,168,90,x64arm::acm::value_control,0x85,u"",u"");
                            int y=48;
                            if(dialog->name_capacity){
                                item.item(0,8,49,52,12,-1,0x82,u"",u"名稱");
                                item.item(0x810080,66,47,168,14,x64arm::acm::name_control,0x81,u"",dialog->name);
                                y+=20;
                            }
                            if(dialog->style&4U)item.item(0x10000,8,y,58,16,x64arm::acm::help_control,0x80,u"",u"說明");
                            item.item(0x10001,106,y,61,16,1,0x80,u"",u"確定");
                            item.item(0x10000,173,y,61,16,2,0x80,u"",u"取消");
                            DialogTemplate window;
                            window.dword(0x90c80040);window.dword(dialog->style&0x80U?0x400U:0);
                            window.word(item.count);window.word(10);window.word(10);window.word(241);window.word(static_cast<unsigned>(y+23));
                            window.word(0);window.word(0);window.text(title);window.word(9);window.text(u"Segoe UI");window.align();
                            window.bytes.insert(window.bytes.end(),item.bytes.begin(),item.bytes.end());
                            template_data=scratch.allocate(window.bytes.size());
                            if(!template_data)co_return finish(7);
                            m.write(GuestAddress{template_data},window.bytes);
                        }
                        answer=co_await api("DialogBoxIndirectParamW",{0,template_data,dialog->owner,proc,address});
                    }
                    if(static_cast<std::uint32_t>(answer)==UINT32_MAX)co_return finish(11);
                    if(answer!=1 || !dialog->confirmed)co_return finish(515);
                    const auto& group=dialog->groups.at(dialog->group);
                    const auto& selection=group.options.at(dialog->option);
                    if(selection.bytes.size()>capacity)co_return finish(512);
                    const auto encode=[&](std::u16string_view text,unsigned limit)->service_callback::Task<std::vector<std::uint8_t>>{
                        if(abi.character==2)co_return utf16_bytes(text.substr(0,limit-1));
                        const auto wide=scratch.text(text);
                        const auto buffer=scratch.allocate(limit);
                        if(!wide || !buffer)throw std::bad_alloc{};
                        const auto convert=resolve("kernel32.dll","WideCharToMultiByte").value;
                        auto count=text.size();
                        while(count){
                            const auto actual=co_await service_callback::call_value(convert,
                                {0,0,wide,count,buffer,limit-1,0,0});
                            if(actual && actual<limit){
                                std::vector<std::uint8_t> bytes(actual+1);
                                m.read(GuestAddress{buffer},std::span(bytes).first(actual));
                                co_return bytes;
                            }
                            --count;
                        }
                        co_return std::vector<std::uint8_t>{0};
                    };
                    auto tag=co_await encode(group.label,48);
                    auto value=co_await encode(selection.description,128);
                    std::vector<std::uint8_t> custom_name;
                    if(name_buffer)custom_name=co_await encode(dialog->name,std::min(dialog->name_capacity,1024U));
                    m.check(GuestAddress{address},abi.size(),Access::Write);
                    m.check(GuestAddress{source},selection.bytes.size(),Access::Write);
                    if(name_buffer)m.check(GuestAddress{name_buffer},custom_name.size(),Access::Write);
                    m.write(GuestAddress{source},selection.bytes);
                    m.store(GuestAddress{address+8+2*abi.pointer},32,selection.bytes.size());
                    tag.resize(48*abi.character);
                    value.resize(128*abi.character);
                    m.write(GuestAddress{address+abi.tag_text()},tag);
                    m.write(GuestAddress{address+abi.value_text()},value);
                    if(name_buffer)m.write(GuestAddress{name_buffer},custom_name);
                    co_return finish(0);
                }catch(const MemoryFault&){co_return finish(11);}
                catch(const std::bad_alloc&){co_return finish(7);}
            });
    }
