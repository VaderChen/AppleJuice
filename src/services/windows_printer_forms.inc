// 此分支由既有 spooler dispatcher 呼叫，與列印工作共用 handle、通知及登錄後台。
if(name.starts_with("EnumForms") || name.starts_with("GetForm") || name.starts_with("AddForm") || name.starts_with("SetForm") || name.starts_with("DeleteForm")){
    const auto handle=printing_->handles.find(arg(0));if(handle==printing_->handles.end())return fail(6);
    const bool enumerate=name.starts_with("EnumForms"),get=name.starts_with("GetForm"),add=name.starts_with("AddForm"),remove=name.starts_with("DeleteForm");
    const bool query=enumerate || get;const auto level=remove?1U:d(enumerate || add?1:2);
    if(level!=1 && level!=2)return fail(124);
    if(!query && !(handle->second.access&4U))return fail(5);
    const auto target=remove?0:query?arg(enumerate?2:3):arg(add?2:3),needed=query?arg(enumerate?4:5):0;
    const auto capacity=query?d(enumerate?3:4):0;
    if(query){
        if(!needed || (enumerate && !arg(5)))return fail(87);
        memory.check(GuestAddress{needed},4,Access::Write);if(enumerate){memory.check(GuestAddress{arg(5)},4,Access::Write);memory.store(GuestAddress{arg(5)},32,0);}
        memory.store(GuestAddress{needed},32,0);
    }
    const auto key=!enumerate && !add?printer_forms::fold(string(arg(1))):std::u16string{};
    if(!enumerate && !add && !printer_forms::valid_name(key))return fail(1902);
    try {
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,query);
        if(transaction.error)return fail(transaction.error);
        auto forms=printer_forms::all(runtime_.registry);
        const auto found=std::find_if(forms.begin(),forms.end(),[&](const auto& f){return printer_forms::fold(f.name)==key;});
        if(!enumerate && !add && found==forms.end())return fail(1902);
        if(query){
            if(get)forms={*found};
            const unsigned record=level==1?unit*2+24:(x86_?56:88),tail=unit*2+24;
            PrinterBuffer buffer(static_cast<unsigned>(forms.size())*record,target,x86_,wide);
            for(unsigned i=0;i<forms.size();++i){
                const auto& form=forms[i];const auto base=i*record;
                buffer.put(base,form.flags);buffer.text(base+unit,form.name);
                for(unsigned j=0;j<6;++j)buffer.put(base+unit*2+j*4,static_cast<std::uint32_t>(form.geometry[j]));
                if(level==2){
                    // pKeyword 不受 A／W 影響，始終是 LPCSTR。
                    if(!form.keyword.empty()){std::vector<std::uint8_t> bytes(form.keyword.begin(),form.keyword.end());bytes.push_back(0);buffer.data(base+tail,bytes);}
                    unsigned type=0;
                    if(!form.mui.empty()){type|=2;buffer.text(base+tail+unit*2,form.mui);buffer.put(base+tail+unit*3,form.resource);}
                    if(!form.languages.empty()){
                        type|=4;auto language=form.languages.find(0x409);if(language==form.languages.end())language=form.languages.begin();
                        buffer.text(base+tail+unit*4,language->second);buffer.put(base+tail+unit*5,language->first,2);
                    }
                    buffer.put(base+tail+unit,type?type:1);
                }
            }
            if(target && capacity>=buffer.bytes.size()){
                memory.check(GuestAddress{target},buffer.bytes.size(),Access::Write);
                if(enumerate)memory.store(GuestAddress{arg(5)},32,forms.size());
            }
            return copy(buffer,target,capacity,needed);
        }
        if(!add && found->flags==1)return fail(5);
        printer_forms::Form form;
        if(!remove){
            if(!target)return fail(87);
            const unsigned size=level==1?unit*2+24:(x86_?56:88),tail=unit*2+24;
            memory.check(GuestAddress{target},size,Access::Read);
            const auto pointer=[&](unsigned offset){return memory.load(GuestAddress{target+offset},width);};
            form.flags=static_cast<unsigned>(memory.load(GuestAddress{target},32));form.name=string(pointer(unit));
            if((form.flags!=0 && form.flags!=2) || !printer_forms::valid_name(form.name))return fail(87);
            for(unsigned j=0;j<6;++j)form.geometry[j]=static_cast<std::int32_t>(memory.load(GuestAddress{target+unit*2+j*4},32));
            if(!printer_forms::valid_geometry(form))return fail(1903);
            if(!add){form.paper=found->paper;form.mui=found->mui;form.resource=found->resource;form.languages=found->languages;form.keyword=found->keyword;}
            if(level==2){
                const auto keyword=pointer(tail),mui=pointer(tail+unit*2),display=pointer(tail+unit*4);
                const auto type=static_cast<unsigned>(memory.load(GuestAddress{target+tail+unit},32));
                const auto resource=static_cast<unsigned>(memory.load(GuestAddress{target+tail+unit*3},32));
                const auto lang=static_cast<std::uint16_t>(memory.load(GuestAddress{target+tail+unit*5},16));
                if(type!=1 && type!=2 && type!=4)return fail(87);
                if((type==1 && (mui || display || resource || lang)) || (type==2 && (!mui || display || lang)) || (type==4 && (mui || resource || !display)))return fail(87);
                form.keyword=keyword?read_ansi(memory,keyword):std::string{};if(form.keyword.size()>63)return fail(87);
                if(type==2){form.mui=string(mui);form.resource=resource;if(form.mui.empty())return fail(87);}
                if(type==4){auto text=string(display);if(text.empty())return fail(87);form.languages[lang]=std::move(text);}
                if(type==1){form.mui.clear();form.resource=0;form.languages.clear();}
            }
            for(const auto& other:forms){
                if(!add && &other==&*found)continue;
                if(printer_forms::fold(other.name)==printer_forms::fold(form.name) || (!form.keyword.empty() && other.keyword==form.keyword))return fail(183);
            }
            if(add){
                for(form.paper=256;form.paper<=32767;++form.paper)if(std::none_of(forms.begin(),forms.end(),[&](const auto& f){return f.paper==form.paper;}))break;
                if(form.paper>32767)return fail(8);
            }
        }
        // 建立／更新先在快照上完成。任何存取、配額或磁碟錯誤均不留下半份表單。
        auto& registry=runtime_.registry;auto before=registry;bool accepted=false;
        struct Rollback {RegistryState& state;RegistryState& before;bool& accepted;~Rollback(){if(!accepted)state=std::move(before);}} rollback{registry,before,accepted};
        const auto& path=printer_forms::path;
        if(!registry.keys.contains(path)){
            for(std::size_t end=1;;){
                const auto part=path.substr(0,end);
                if(!registry.keys.contains(part)){
                    if(registry.keys.size()>=4096)return fail(8);
                    const auto slash=part.find_last_of(u'\\');
                    if(slash!=part.npos){const auto parent=part.substr(0,slash);(void)registry_security_access(parent,4);
                        registry.security_descriptors[part]=registry_creation_descriptor(registry_security_descriptor(parent),{});}
                    registry.keys.try_emplace(part);registry.display_names[part]=part.substr(slash==part.npos?0:slash+1);registry.modified_times[part]=runtime_.system_filetime();
                }
                if(end==path.size())break;end=path.find(u'\\',end+1);if(end==path.npos)end=path.size();
            }
        }
        (void)registry_security_access(path,2);
        auto& values=registry.keys.at(path);
        if(!add){const auto old=values.find(key);if(old==values.end())return fail(1902);registry.bytes-=old->second.bytes.size();--registry.values;values.erase(old);}
        if(!remove){auto bytes=printer_forms::encode(form);registry.bytes+=bytes.size();++registry.values;values.emplace(printer_forms::fold(form.name),RegistryValue{3,std::move(bytes),form.name});}
        auto revisions=printer_forms::revisions(registry);++revisions[add?0:remove?2:1];
        const bool had_revision=values.contains(u"");auto& revision=values[u""];registry.bytes-=revision.bytes.size();if(!had_revision)++registry.values;revision.type=3;revision.bytes.resize(24);
        for(unsigned i=0;i<24;++i)revision.bytes[i]=static_cast<std::uint8_t>(revisions[i/8]>>((i%8)*8));registry.bytes+=24;
        if(registry.bytes>output_limit || registry.values>4096)return fail(8);
        registry.modified_times[path]=runtime_.system_filetime();
        if(const auto error=commit_registry_transaction(transaction))return fail(error);
        accepted=true;poll_registry_notifications(false,path,4,&memory);return finish(1);
    }catch(const SecurityDescriptorError& error){return fail(error.code);}catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::invalid_argument&){return fail(87);}
}
