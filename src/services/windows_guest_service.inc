// Windows 服務的設定、控制與狀態共用持久登錄交易。原生 launchd 查詢仍由原流程處理。
std::optional<std::uint32_t> WindowsServices::invoke_guest_service(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    namespace gs=guest_service;
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto read=[&](std::uint64_t at){return at?(wide?read_utf16(memory,GuestAddress{at},32768):utf8_to_utf16(read_ansi(memory,at,32768))):std::u16string{};};
    const auto finish=[&](std::uint64_t value,std::uint32_t error=0){if(error)last_error_=error;arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto handle=[&](std::uint64_t id)->ServiceControlHandle&{const auto it=service_control_handles_.find(id);if(it==service_control_handles_.end() || it->second.owner!=process_id())throw SecurityDescriptorError{6};return it->second;};
    const auto require=[&](const ServiceControlHandle& object,std::uint32_t access){if(!object.guest || object.manager || object.status_handle)throw SecurityDescriptorError{6};if((object.access&access)!=access)throw SecurityDescriptorError{5};};
    auto& registry=runtime_.registry;
    std::unique_ptr<RegistryTransaction> transaction;
    const auto reload=[&]{transaction=std::make_unique<RegistryTransaction>(registry,registry_file_,&registry_store_cache_);if(transaction->error)throw SecurityDescriptorError{transaction->error};};
    const auto save=[&]{if(const auto error=gs::commit(registry,*transaction))throw SecurityDescriptorError{error};};
    const auto find_record=[&](const ServiceControlHandle& object)->gs::Values&{const auto it=registry.keys.find(gs::path(object.label));if(it==registry.keys.end() || !gs::installed(it->second) || gs::number(it->second,u"x64arm_instance")!=object.instance)throw SecurityDescriptorError{1060};return it->second;};
    const auto live_handle=[&](const std::u16string& label,gs::Values& values,std::uint32_t access){
        const auto id=runtime_.allocate_wait_handle();auto refs=gs::references(values);if(refs.size()>=8192)throw SecurityDescriptorError{8};
        const auto owner=process_identity::query(process_id());if(owner.error || !owner.identity)throw SecurityDescriptorError{owner.error?owner.error:6U};
        refs.push_back({owner.identity,id});gs::put_references(values,refs);save();
        service_control_handles_.emplace(id,ServiceControlHandle{label,access,process_id(),false,false,true,false,gs::number(values,u"x64arm_instance")});return id;
    };
    const auto descriptor=[&](const gs::Values& values){const auto bytes=gs::bytes(values,u"security");if(bytes.empty())throw SecurityDescriptorError{13};return bytes;};
    const auto put_status=[&](std::uint64_t output,const std::array<std::uint32_t,9>& words,unsigned count){if(!output)throw SecurityDescriptorError{87};memory.check(GuestAddress{output},count*4,Access::Write);for(unsigned i=0;i<count;++i)memory.store(GuestAddress{output+i*4},32,words[i]);};
    const auto suspend=[&](const std::u16string& label,unsigned kind,std::uint64_t sequence=0){
        // 重入同一個同步 API 時保留原始期限，避免永遠延長等待。
        guest_service_waits_.try_emplace(threads_.current,GuestServiceWait{label,sequence,gs::now()+30000,kind});
        auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::ServiceControl;
        transaction.reset();schedule(state,memory);return std::optional<std::uint32_t>{};
    };
    try {
        if(!runtime_.network.external_io)throw SecurityDescriptorError{50};
        if(name=="ServiceMainDispatch"){
            const auto id=arg(0);const auto it=guest_service_dispatchers_.find(id);if(it==guest_service_dispatchers_.end())throw SecurityDescriptorError{6};const auto& dispatch=it->second;
            start_callbacks(state,memory,{{dispatch.main,{dispatch.argc,dispatch.storage}}},Continuation::ServiceMain,id);return {};
        }
        reload();
        if(name.starts_with("CreateService")){
            const auto& manager=handle(arg(0));if(!manager.manager)throw SecurityDescriptorError{6};if(!(manager.access&2U) || !filesystem_.writable())throw SecurityDescriptorError{5};
            const auto label=read(arg(1)),display=arg(2)?read(arg(2)):label,binary=read(arg(7)),group=read(arg(8)),account=arg(11)?read(arg(11)):u"LocalSystem";
            const auto type=static_cast<std::uint32_t>(arg(4)),start=static_cast<std::uint32_t>(arg(5)),control=static_cast<std::uint32_t>(arg(6));
            if(!gs::valid_name(label) || display.size()>256)throw SecurityDescriptorError{123};if(binary.empty() || start>4 || control>3)throw SecurityDescriptorError{87};
            // SERVICE_KERNEL_DRIVER 由程序內的核心驅動宿主載入；ObjectName 為驅動物件名稱，不是登入帳戶。
            const bool kernel_driver=type==1;
            if(type!=0x10 && type!=0x110 && !kernel_driver)throw SecurityDescriptorError{50};if(start<2 && !kernel_driver)throw SecurityDescriptorError{87};
            if(!kernel_driver && gs::fold(account)!=u"localsystem" && gs::fold(account)!=u"nt authority\\system")throw SecurityDescriptorError{1057};
            if(arg(12) && !read(arg(12)).empty())throw SecurityDescriptorError{87};
            if(arg(9))memory.check(GuestAddress{arg(9)},4,Access::Write);
            const auto key=gs::path(label);
            if(const auto old=registry.keys.find(key);old!=registry.keys.end() && gs::installed(old->second)){
                if(gs::number(old->second,u"deleteflag") && gs::references(old->second).empty() && gs::status(old->second)[1]==1){gs::erase(registry,key);save();}
                else throw SecurityDescriptorError{gs::number(old->second,u"deleteflag")?1072U:1073U};
            }
            for(const auto& [other,values]:registry.keys)if(other.starts_with(gs::root+u"\\") && gs::installed(values) && gs::fold(gs::text(values,u"displayname"))==gs::fold(display))throw SecurityDescriptorError{1078};
            gs::Values values;gs::put_text(values,u"name",label);gs::put_text(values,u"displayname",display);gs::put_text(values,u"imagepath",binary,2);gs::put_text(values,u"group",group);gs::put_text(values,u"objectname",account);
            gs::put_number(values,u"type",type);gs::put_number(values,u"start",start);gs::put_number(values,u"errorcontrol",control);gs::put_number(values,u"tag",0);
            std::u16string dependencies;if(arg(10)){auto at=arg(10);for(unsigned count=0;count<1024;++count){auto item=read(at);if(item.empty()){dependencies.push_back(0);break;}if(!gs::valid_name(item) || gs::fold(item)==gs::fold(label))throw SecurityDescriptorError{1059};dependencies+=item;dependencies.push_back(0);at+=wide?(item.size()+1)*2:utf16_to_utf8(item).size()+1;if(count==1023)throw SecurityDescriptorError{87};}}
            if(dependencies.empty())dependencies.push_back(0);values[u"dependonservice"]={7,utf16_bytes(dependencies),{}};
            initialize_tokens();security::SecurityDescriptor sd;sd.control=4;sd.data[0]=runtime_.token_objects.at(effective_token_id()).user.sid;sd.data[1]=security::account_sid(*security::find_account("Administrators"));
            sd.data[3]=security::encode_acl({security::encode_ace({0,0,0xf01ffU,sd.data[0]}),security::encode_ace({0,0,0xf01ffU,sd.data[1]}),security::encode_ace({0,0,0xf01ffU,security::account_sid(*security::find_account("SYSTEM"))}),security::encode_ace({0,0,0x2008dU,security::account_sid(*security::find_account("Authenticated Users"))})});
            const auto bytes=security::relative_descriptor(sd);const auto access=object_security_access(bytes,static_cast<std::uint32_t>(arg(3)),gs::mapping);values[u"security"]={3,bytes,{}};
            gs::put_number(values,u"x64arm_instance",(gs::now()<<20)^process_identity::query(process_id()).identity.birth^runtime_.allocate_wait_handle(),true);
            gs::create_path(registry,key);registry.keys[key]=std::move(values);registry.display_names[key]=label;
            const auto id=live_handle(label,registry.keys.at(key),access);if(arg(9))memory.store(GuestAddress{arg(9)},32,0);return finish(id);
        }
        if(name.starts_with("OpenService")){
            const auto& manager=handle(arg(0));if(!manager.manager)throw SecurityDescriptorError{6};if(!(manager.access&1))throw SecurityDescriptorError{5};
            const auto label=read(arg(1));if(!gs::valid_name(label))throw SecurityDescriptorError{123};const auto it=registry.keys.find(gs::path(label));if(it==registry.keys.end() || !gs::installed(it->second))throw SecurityDescriptorError{1060};
            auto& values=it->second;if(gs::number(values,u"deleteflag")){
                if(gs::references(values).empty() && gs::status(values)[1]==1){gs::erase(registry,it->first);save();throw SecurityDescriptorError{1060};}
                throw SecurityDescriptorError{1072};
            }
            const auto access=object_security_access(descriptor(values),static_cast<std::uint32_t>(arg(2)),gs::mapping);return finish(live_handle(gs::text(values,u"name",label),values,access));
        }
        if(name.starts_with("StartServiceCtrlDispatcher")){
            if(const auto waiting=guest_service_waits_.find(threads_.current);waiting!=guest_service_waits_.end()){
                const auto label=waiting->second.label;auto it=std::find_if(guest_service_dispatchers_.begin(),guest_service_dispatchers_.end(),[&](const auto& entry){return entry.second.dispatcher==threads_.current;});if(it==guest_service_dispatchers_.end())throw SecurityDescriptorError{6};
                auto& dispatch=it->second;
                const auto record=registry.keys.find(gs::path(label));
                // STOPPED 之後 SCM 可以立刻啟動新程序；舊 dispatcher 不得接收新程序的控制。
                if(dispatch.stopped || record==registry.keys.end() ||
                   gs::identity(record->second)!=process_identity::query(process_id()).identity ||
                   gs::number(record->second,u"x64arm_instance")!=service_control_handles_.at(it->first).instance){
                    guest_service_waits_.erase(waiting);return finish(1);
                }
                auto& values=record->second;const auto words=gs::status(values);
                if(words[1]==1){dispatch.stopped=true;guest_service_waits_.erase(waiting);return finish(1);}
                const auto sequence=gs::number(values,u"x64arm_control_sequence");
                if(sequence>gs::number(values,u"x64arm_control_done") && dispatch.handler){dispatch.control_sequence=sequence;const auto control=gs::number(values,u"x64arm_control");transaction.reset();start_callbacks(state,memory,{{dispatch.handler,dispatch.extended?std::vector<std::uint64_t>{control,0,0,dispatch.context}:std::vector<std::uint64_t>{control}}},Continuation::ServiceControl,it->first);return {};}
                return suspend(label,2);
            }
            if(!guest_service_dispatchers_.empty())throw SecurityDescriptorError{1056};if(!arg(0))throw SecurityDescriptorError{87};
            const auto owner=process_identity::query(process_id()).identity;auto record=registry.keys.end();
            for(auto it=registry.keys.begin();it!=registry.keys.end();++it)if(gs::installed(it->second) && gs::identity(it->second)==owner && gs::status(it->second)[1]==2){record=it;break;}
            if(record==registry.keys.end())throw SecurityDescriptorError{1063};
            const auto table_name=memory.load(GuestAddress{arg(0)},bits),main=memory.load(GuestAddress{arg(0)+unit},bits);if(!table_name || !main || memory.load(GuestAddress{arg(0)+2*unit},bits) || memory.load(GuestAddress{arg(0)+3*unit},bits))throw SecurityDescriptorError{87};memory.check(GuestAddress{main},1,Access::Execute);
            auto& values=record->second;const auto label=gs::text(values,u"name");std::vector<std::u16string> arguments{label};const auto raw=gs::bytes(values,u"x64arm_arguments");
            if(raw.size()%2)throw SecurityDescriptorError{13};std::u16string item;for(std::size_t i=0;i<raw.size();i+=2){const auto c=static_cast<char16_t>(raw[i]|(unsigned{raw[i+1]}<<8));if(c)item.push_back(c);else{arguments.push_back(item);item.clear();}}if(!item.empty())throw SecurityDescriptorError{13};
            std::vector<std::uint8_t> buffer((arguments.size()+1)*unit);std::vector<std::size_t> offsets;for(const auto& value:arguments){offsets.push_back(buffer.size());if(wide){const auto encoded=utf16_bytes(value);buffer.insert(buffer.end(),encoded.begin(),encoded.end());}else{const auto encoded=utf16_to_utf8(value);buffer.insert(buffer.end(),encoded.begin(),encoded.end());buffer.push_back(0);}}
            const auto storage=allocate_heap(memory,buffer.size());if(!storage)throw SecurityDescriptorError{8};memory.write(GuestAddress{storage},buffer);for(unsigned i=0;i<offsets.size();++i)memory.store(GuestAddress{storage+i*unit},bits,storage+offsets[i]);
            const auto id=runtime_.allocate_wait_handle();service_control_handles_.emplace(id,ServiceControlHandle{label,0,process_id(),false,false,true,true,gs::number(values,u"x64arm_instance")});
            const auto worker=create_guest_thread(state,memory,resolve("internal-service","ServiceMainDispatch").value,id,0,0);if(!worker){(void)free_heap(memory,storage,state);service_control_handles_.erase(id);throw SecurityDescriptorError{last_error_};}
            guest_service_dispatchers_.emplace(id,GuestServiceDispatch{label,main,0,0,thread_id(worker),threads_.current,storage,0,static_cast<std::uint32_t>(arguments.size()),wide,false});
            runtime_.process_thread_handles.erase(worker);
            gs::put_number(values,u"x64arm_connected",1);save();return suspend(label,2);
        }
        if(name.starts_with("RegisterServiceCtrlHandler")){
            const auto label=read(arg(0));if(!arg(1))throw SecurityDescriptorError{87};memory.check(GuestAddress{arg(1)},1,Access::Execute);
            const auto found=std::find_if(guest_service_dispatchers_.begin(),guest_service_dispatchers_.end(),[&](const auto& item){return gs::fold(item.second.label)==gs::fold(label);});if(found==guest_service_dispatchers_.end())throw SecurityDescriptorError{1083};
            found->second.handler=arg(1);found->second.extended=name.find("HandlerEx")!=name.npos;found->second.context=found->second.extended?arg(2):0;return finish(found->first);
        }
        auto& object=handle(arg(0));auto& values=find_record(object);const auto key=gs::path(object.label);
        if(name=="SetServiceStatus"){
            if(!object.status_handle || !guest_service_dispatchers_.contains(arg(0)) || !arg(1))throw SecurityDescriptorError{6};
            auto& dispatch=guest_service_dispatchers_.at(arg(0));
            if(dispatch.stopped || gs::identity(values)!=process_identity::query(process_id()).identity)throw SecurityDescriptorError{6};
            std::array<std::uint32_t,7> words{};for(unsigned i=0;i<7;++i)words[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+i*4},32));
            if(words[0]!=gs::number(values,u"type") || words[1]<1 || words[1]>7 || (words[2]&~0xfffU) || (words[3]!=1066 && words[4]))throw SecurityDescriptorError{13};
            if(gs::status(values)[1]==1)throw SecurityDescriptorError{6};gs::put_status(values,words);save();dispatch.stopped=words[1]==1;return finish(1);
        }
        if(name=="CloseServiceHandle"){
            require(object,0);auto refs=gs::references(values);const auto owner=process_identity::query(process_id()).identity;
            std::erase_if(refs,[&](const auto& ref){return ref.owner==owner && ref.handle==arg(0);});gs::put_references(values,refs);
            if(gs::number(values,u"deleteflag") && refs.empty() && gs::status(values)[1]==1)gs::erase(registry,key);save();service_control_handles_.erase(arg(0));return finish(1);
        }
        if(name=="DeleteService"){require(object,0x10000);if(!filesystem_.writable())throw SecurityDescriptorError{5};if(gs::number(values,u"deleteflag"))throw SecurityDescriptorError{1072};gs::put_number(values,u"deleteflag",1);save();return finish(1);}
        if(name=="QueryServiceStatus" || name=="QueryServiceStatusEx"){
            require(object,4);const bool ex=name.ends_with("Ex");if(ex){if(static_cast<std::uint32_t>(arg(1)))throw SecurityDescriptorError{124};if(!arg(4))throw SecurityDescriptorError{87};memory.store(GuestAddress{arg(4)},32,36);if(static_cast<std::uint32_t>(arg(3))<36)throw SecurityDescriptorError{122};}
            put_status(arg(ex?2:1),gs::status(values),ex?9:7);return finish(1);
        }
        if(name.starts_with("QueryServiceConfig") && !name.starts_with("QueryServiceConfig2")){
            require(object,1);if(!arg(3))throw SecurityDescriptorError{87};const unsigned header=x86_?36:64;std::vector<std::uint8_t> buffer(header);std::vector<std::pair<unsigned,std::size_t>> offsets;
            const auto put=[&](unsigned at,std::uint64_t value,unsigned count=4){for(unsigned i=0;i<count;++i)buffer[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
            const auto add=[&](unsigned at,std::u16string value){offsets.emplace_back(at,buffer.size());if(wide){auto data=utf16_bytes(value);buffer.insert(buffer.end(),data.begin(),data.end());}else{auto data=utf16_to_utf8(value);buffer.insert(buffer.end(),data.begin(),data.end());buffer.push_back(0);}};
            put(0,gs::number(values,u"type"));put(4,gs::number(values,u"start"));put(8,gs::number(values,u"errorcontrol"));put(x86_?20:32,gs::number(values,u"tag"));
            add(x86_?12:16,gs::text(values,u"imagepath"));add(x86_?16:24,gs::text(values,u"group"));std::u16string dependencies;const auto data=gs::bytes(values,u"dependonservice");for(std::size_t i=0;i+2<data.size();i+=2)dependencies.push_back(static_cast<char16_t>(data[i]|(unsigned{data[i+1]}<<8)));add(x86_?24:40,dependencies.empty()?std::u16string(1,0):dependencies);add(x86_?28:48,gs::text(values,u"objectname"));add(x86_?32:56,gs::text(values,u"displayname"));
            memory.store(GuestAddress{arg(3)},32,buffer.size());if(!arg(1) || static_cast<std::uint32_t>(arg(2))<buffer.size())throw SecurityDescriptorError{122};for(const auto& [at,offset]:offsets)put(at,arg(1)+offset,unit);memory.write(GuestAddress{arg(1)},buffer);return finish(1);
        }
        if(name.starts_with("ChangeServiceConfig") && !name.starts_with("ChangeServiceConfig2")){
            require(object,2);if(!filesystem_.writable())throw SecurityDescriptorError{5};auto next=values;
            for(unsigned i=1;i<=3;++i)if(static_cast<std::uint32_t>(arg(i))!=UINT32_MAX)gs::put_number(next,i==1?u"type":i==2?u"start":u"errorcontrol",static_cast<std::uint32_t>(arg(i)));
            if((gs::number(next,u"type")!=0x10 && gs::number(next,u"type")!=0x110) || gs::number(next,u"start")<2 || gs::number(next,u"start")>4 || gs::number(next,u"errorcontrol")>3)throw SecurityDescriptorError{87};
            if(arg(4)){auto binary=read(arg(4));if(binary.empty())throw SecurityDescriptorError{87};gs::put_text(next,u"imagepath",binary,2);}if(arg(5))gs::put_text(next,u"group",read(arg(5)));
            if(arg(8)){auto account=read(arg(8));if(gs::fold(account)!=u"localsystem" && gs::fold(account)!=u"nt authority\\system")throw SecurityDescriptorError{1057};gs::put_text(next,u"objectname",account);}if(arg(9) && !read(arg(9)).empty())throw SecurityDescriptorError{87};
            if(arg(10)){auto display=read(arg(10));if(display.size()>256)throw SecurityDescriptorError{87};gs::put_text(next,u"displayname",display);}
            if(arg(7)){std::u16string list;auto at=arg(7);for(unsigned count=0;;++count){if(count>=1024)throw SecurityDescriptorError{87};auto item=read(at);list+=item;list.push_back(0);if(item.empty())break;if(gs::fold(item)==gs::fold(object.label))throw SecurityDescriptorError{1059};at+=wide?(item.size()+1)*2:utf16_to_utf8(item).size()+1;}next[u"dependonservice"]={7,utf16_bytes(list),{}};}
            if(arg(6))memory.check(GuestAddress{arg(6)},4,Access::Write);values=std::move(next);save();if(arg(6))memory.store(GuestAddress{arg(6)},32,gs::number(values,u"tag"));return finish(1);
        }
        if(name=="QueryServiceObjectSecurity" || name=="SetServiceObjectSecurity"){
            const bool setting=name.starts_with("Set");const auto flags=static_cast<std::uint32_t>(arg(1));
            if(!flags || (flags&~(setting?0xa000001fU:0x1fU)) || (flags&0xa0000000U)==0xa0000000U)throw SecurityDescriptorError{87};
            if(flags&8U)throw SecurityDescriptorError{1314};
            require(object,setting?((flags&0x13U)?0x80000U:0U)|((flags&0xa0000004U)?0x40000U:0U):0x20000U);
            auto sd=security::parse_relative_descriptor(descriptor(values));
            if(setting){
                if(!filesystem_.writable())throw SecurityDescriptorError{5};if(!arg(2))throw SecurityDescriptorError{87};
                const auto incoming=security::read_descriptor(memory,arg(2),x86_,true);
                if(((flags&1U) && incoming.data[0].empty()) || ((flags&2U) && incoming.data[1].empty()))throw SecurityDescriptorError{1337};
                if((flags&4U) && !(incoming.control&4U))throw SecurityDescriptorError{1338};
                if(flags&1U){sd.data[0]=incoming.data[0];sd.control&=static_cast<std::uint16_t>(~1U);}
                if(flags&2U){sd.data[1]=incoming.data[1];sd.control&=static_cast<std::uint16_t>(~2U);}
                if(flags&4U){sd.data[3]=incoming.data[3];sd.control=static_cast<std::uint16_t>((sd.control&~12U)|(incoming.control&12U));}
                if(flags&16U){sd.data[2]=incoming.data[2];sd.control=static_cast<std::uint16_t>((sd.control&~48U)|(incoming.control&48U));}
                if(flags&0x80000000U)sd.control|=0x1000U;if(flags&0x20000000U)sd.control&=static_cast<std::uint16_t>(~0x1000U);
                const auto bytes=object_security_descriptor(sd,(flags&1U)!=0,false);values[u"security"]={3,bytes,{}};save();return finish(1);
            }
            if(!arg(4))throw SecurityDescriptorError{87};
            if(!(flags&1U))sd.data[0].clear();if(!(flags&2U))sd.data[1].clear();
            if(!(flags&4U)){sd.data[3].clear();sd.control&=static_cast<std::uint16_t>(~12U);}
            if(!(flags&16U)){sd.data[2].clear();sd.control&=static_cast<std::uint16_t>(~48U);}
            const auto bytes=security::relative_descriptor(sd);memory.store(GuestAddress{arg(4)},32,bytes.size());
            if(!arg(2) || static_cast<std::uint32_t>(arg(3))<bytes.size())throw SecurityDescriptorError{122};memory.write(GuestAddress{arg(2)},bytes);return finish(1);
        }

        if(name.starts_with("ChangeServiceConfig2") || name.starts_with("QueryServiceConfig2")){
            const bool setting=name.starts_with("Change");require(object,setting?2U:1U);const auto level=static_cast<std::uint32_t>(arg(1));if(setting && !filesystem_.writable())throw SecurityDescriptorError{5};
            const auto property=level==1?u"description":level==3?u"delayedautostart":level==4?u"failureactionsonnoncrashfailures":level==5?u"servicesidtype":level==7?u"preshutdowntimeout":u"";
            if(!*property)throw SecurityDescriptorError{124};
            if(setting){if(!arg(2))return finish(1);if(level==1){const auto at=memory.load(GuestAddress{arg(2)},bits);if(at)gs::put_text(values,property,read(at));}else{const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));if((level==3 || level==4) && value>1)throw SecurityDescriptorError{87};if(level==5 && value!=0)throw SecurityDescriptorError{50};gs::put_number(values,property,value);}save();return finish(1);}
            if(!arg(4))throw SecurityDescriptorError{87};std::vector<std::uint8_t> bytes(level==1?unit:4);
            if(level==1){const auto value=gs::text(values,property);if(!value.empty()){const auto address=arg(2)+unit;for(unsigned i=0;i<unit;++i)bytes[i]=static_cast<std::uint8_t>(address>>(i*8));if(wide){auto data=utf16_bytes(value);bytes.insert(bytes.end(),data.begin(),data.end());}else{auto data=utf16_to_utf8(value);bytes.insert(bytes.end(),data.begin(),data.end());bytes.push_back(0);}}}else{const auto value=gs::number(values,property,level==7?180000U:0U);for(unsigned i=0;i<4;++i)bytes[i]=static_cast<std::uint8_t>(value>>(i*8));}
            memory.store(GuestAddress{arg(4)},32,bytes.size());if(!arg(2) || static_cast<std::uint32_t>(arg(3))<bytes.size())throw SecurityDescriptorError{122};memory.write(GuestAddress{arg(2)},bytes);return finish(1);
        }
        if(name.starts_with("StartService")){
            require(object,0x10);if(gs::number(values,u"deleteflag"))throw SecurityDescriptorError{1072};
            if(gs::number(values,u"type")==1){
                if(gs::number(values,u"start")==4)throw SecurityDescriptorError{1058};if(gs::status(values)[1]!=1)throw SecurityDescriptorError{1056};
                const auto label=gs::text(values,u"name",object.label),image=gs::text(values,u"imagepath");transaction.reset();return start_kernel_driver(state,memory,label,image);
            }
            if(const auto waiting=guest_service_waits_.find(threads_.current);waiting!=guest_service_waits_.end()){
                const auto wait=waiting->second;const bool dependency=wait.kind==3;
                const auto record=registry.keys.find(gs::path(wait.label));
                const auto words=record==registry.keys.end()?std::array<std::uint32_t,9>{0,1,0,1060}:gs::status(record->second);
                const bool ready=dependency?words[1]==4:record!=registry.keys.end() && gs::number(record->second,u"x64arm_connected");
                const bool expired=gs::now()>=wait.deadline;
                if(!ready && words[1]!=1 && !expired)return suspend(wait.label,wait.kind,wait.sequence);
                guest_service_waits_.erase(waiting);
                if(!ready)throw SecurityDescriptorError{dependency?1068U:expired?1053U:words[3]?words[3]:1067U};
                if(!dependency)return finish(1);
            }
            if(gs::number(values,u"start")==4)throw SecurityDescriptorError{1058};if(gs::status(values)[1]!=1)throw SecurityDescriptorError{1056};
            const auto count=static_cast<std::uint32_t>(arg(1));if(count>1024 || (count && !arg(2)))throw SecurityDescriptorError{87};
            std::u16string arguments;for(unsigned i=0;i<count;++i){const auto at=memory.load(GuestAddress{arg(2)+i*unit},bits);if(!at)throw SecurityDescriptorError{87};arguments+=read(at);arguments.push_back(0);if(arguments.size()>32768)throw SecurityDescriptorError{87};}
            // 依拓撲順序啟動相依服務；相依服務必須回報 RUNNING 才啟動後續服務。
            std::vector<std::u16string> order;std::set<std::u16string> visited,visiting;
            std::function<void(const std::u16string&)> visit=[&](const auto& label){
                const auto path=gs::path(label);if(visited.contains(path))return;if(!visiting.insert(path).second)throw SecurityDescriptorError{1059};
                const auto found=registry.keys.find(path);if(found==registry.keys.end() || !gs::installed(found->second) || gs::number(found->second,u"deleteflag"))throw SecurityDescriptorError{1068};
                if(visited.size()+visiting.size()>128)throw SecurityDescriptorError{1068};
                const auto raw=gs::bytes(found->second,u"dependonservice");if(raw.size()%2)throw SecurityDescriptorError{13};
                std::u16string item;for(std::size_t i=0;i<raw.size();i+=2){const auto ch=static_cast<char16_t>(raw[i]|(unsigned{raw[i+1]}<<8));if(ch){item.push_back(ch);continue;}if(item.empty())break;
                    if(item.front()==u'+'){
                        bool present=false;for(const auto& [candidate_key,candidate]:registry.keys)if(candidate_key.starts_with(gs::root+u"\\") && gs::installed(candidate) && gs::fold(gs::text(candidate,u"group"))==gs::fold(item.substr(1))){present=true;visit(gs::text(candidate,u"name"));}if(!present)throw SecurityDescriptorError{1068};
                    }else visit(item);item.clear();}
                visiting.erase(path);visited.insert(path);order.push_back(label);
            };
            visit(object.label);
            auto label=object.label;auto* starting=&values;unsigned wait_kind=0;
            for(const auto& dependency:order){if(gs::fold(dependency)==gs::fold(object.label))break;auto& candidate=registry.keys.at(gs::path(dependency));const auto words=gs::status(candidate);if(words[1]==4)continue;
                if(words[1]==2)return suspend(dependency,3);if(words[1]!=1 || gs::number(candidate,u"start")==4)throw SecurityDescriptorError{1068};label=dependency;starting=&candidate;arguments.clear();wait_kind=3;break;}
            const auto instance=gs::number(*starting,u"x64arm_instance");const auto command=gs::text(*starting,u"imagepath");auto parts=parse_windows_arguments(command);if(parts.empty())throw SecurityDescriptorError{2};
            std::vector<std::uint8_t> buffer(128);const auto add=[&](const std::u16string& value){const auto offset=buffer.size();const auto data=utf16_bytes(value);buffer.insert(buffer.end(),data.begin(),data.end());return offset;};const auto application=add(parts.front()),line=add(command);
            const auto storage=allocate_heap(memory,buffer.size());if(!storage)throw SecurityDescriptorError{8};memory.write(GuestAddress{storage},buffer);memory.store(GuestAddress{storage},32,x86_?68:104);
            std::vector<std::uint8_t> raw;for(const auto ch:arguments){raw.push_back(static_cast<std::uint8_t>(ch));raw.push_back(static_cast<std::uint8_t>(ch>>8));}(*starting)[u"x64arm_arguments"]={3,std::move(raw),{}};
            const auto owner=process_identity::query(process_id()).identity;
            gs::put_number(*starting,u"x64arm_pid",owner.pid);gs::put_number(*starting,u"x64arm_birth",owner.birth,true);gs::put_number(*starting,u"x64arm_connected",0);gs::put_number(*starting,u"x64arm_control_sequence",0,true);gs::put_number(*starting,u"x64arm_control_done",0,true);gs::put_status(*starting,std::array<std::uint32_t,7>{static_cast<std::uint32_t>(gs::number(*starting,u"type")),2,0,0,0,0,30000});
            try{save();}catch(...){(void)free_heap(memory,storage,state);throw;}transaction.reset();
            const bool launched=create_guest_process(memory,{storage+application,storage+line,0,0,0,4,0,0,storage,storage+104},true,false,0,2);
            const auto error=last_error_;const auto process=launched?memory.load(GuestAddress{storage+104},bits):0,thread=launched?memory.load(GuestAddress{storage+104+unit},bits):0;const auto pid=launched?static_cast<std::uint32_t>(memory.load(GuestAddress{storage+104+2*unit},32)):0;(void)free_heap(memory,storage,state);
            reload();auto found=registry.keys.find(gs::path(label));if(found==registry.keys.end() || gs::number(found->second,u"x64arm_instance")!=instance)throw SecurityDescriptorError{1060};auto& current=found->second;
            if(!launched){gs::put_status(current,std::array<std::uint32_t,7>{static_cast<std::uint32_t>(gs::number(current,u"type")),1,0,error,0,0,0});save();throw SecurityDescriptorError{wait_kind==3?1068U:error};}
            const auto born=process_identity::query(pid);gs::put_number(current,u"x64arm_pid",pid);gs::put_number(current,u"x64arm_birth",born.identity.birth,true);save();transaction.reset();std::uint32_t previous{};const auto resumed=process_thread_control(memory,thread,3,previous);(void)close_process(thread);(void)close_process(process);
            if(!resumed || *resumed){reload();auto& failed=registry.keys.at(gs::path(label));gs::put_status(failed,std::array<std::uint32_t,7>{static_cast<std::uint32_t>(gs::number(failed,u"type")),1,0,resumed?*resumed:6U,0,0,0});save();throw SecurityDescriptorError{resumed?*resumed:6U};}
            return suspend(label,wait_kind);
        }
        if(name=="ControlService"){
            const auto control=static_cast<std::uint32_t>(arg(1));const auto access=control==1?0x20U:control==4?0x80U:(control==2 || control==3)?0x40U:(control>=128 && control<=255)?0x100U:0U;if(!access)throw SecurityDescriptorError{87};require(object,access);if(!arg(2))throw SecurityDescriptorError{87};
            const auto words=gs::status(values);
            if(gs::number(values,u"type")==1){
                // 驅動只接受 STOP 與 INTERROGATE；STOP 呼叫 DriverUnload。
                if(words[1]==1)throw SecurityDescriptorError{1062};if(control==4){put_status(arg(2),words,7);return finish(1);}if(control!=1)throw SecurityDescriptorError{1052};
                if(!(words[2]&1U))throw SecurityDescriptorError{1052};const auto label=gs::text(values,u"name",object.label);transaction.reset();return stop_kernel_driver(state,memory,label,arg(2));
            }
            if(const auto wait=guest_service_waits_.find(threads_.current);wait!=guest_service_waits_.end()){
                const bool done=gs::number(values,u"x64arm_control_done")>=wait->second.sequence;const bool expired=gs::now()>=wait->second.deadline;const bool alive=process_identity::query(gs::identity(values).pid).live(gs::identity(values));if(done || !alive || expired){guest_service_waits_.erase(wait);put_status(arg(2),words,7);return finish(done?1:0,done?0:expired?1053:1062);}return suspend(object.label,1,wait->second.sequence);
            }
            if(words[1]==1)throw SecurityDescriptorError{1062};if(words[1]==2 || words[1]==3)throw SecurityDescriptorError{1061};if(control==1 && !(words[2]&1U))throw SecurityDescriptorError{1052};if((control==2 || control==3) && !(words[2]&2U))throw SecurityDescriptorError{1052};
            if(gs::number(values,u"x64arm_control_sequence")!=gs::number(values,u"x64arm_control_done"))throw SecurityDescriptorError{1061};const auto sequence=gs::number(values,u"x64arm_control_sequence")+1;gs::put_number(values,u"x64arm_control",control);gs::put_number(values,u"x64arm_control_sequence",sequence,true);save();return suspend(object.label,1,sequence);
        }
        throw SecurityDescriptorError{50};
    }catch(const SecurityDescriptorError& error){return finish(0,error.code);}
    catch(const MemoryFault&){return finish(0,998);}
    catch(const std::invalid_argument&){return finish(0,87);}
    catch(const std::bad_alloc&){return finish(0,8);}
}

void WindowsServices::poll_guest_services(GuestMemory& memory){
    if(registry_io_busy_)return; // 不可重入尚未完成的登錄交易；其他 I/O／音訊輪詢照常進行。
    (void)memory;namespace gs=guest_service;if(guest_service_waits_.empty() || gs::now()<guest_service_poll_time_+10)return;guest_service_poll_time_=gs::now();
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return;
    for(const auto& [id,wait]:guest_service_waits_){auto it=threads_.items.find(id);if(it==threads_.items.end() || it->second.status!=ThreadStatus::Waiting || it->second.wait.kind!=ThreadWaitKind::ServiceControl)continue;
        try{
            const auto record=runtime_.registry.keys.find(gs::path(wait.label));bool ready=record==runtime_.registry.keys.end();
            if(wait.kind==2){
                const auto dispatch=std::find_if(guest_service_dispatchers_.begin(),guest_service_dispatchers_.end(),[&](const auto& entry){return entry.second.dispatcher==id;});
                ready|=dispatch==guest_service_dispatchers_.end() || dispatch->second.stopped;
                if(!ready)ready=gs::identity(record->second)!=process_identity::query(process_id()).identity;
            }
            if(!ready){const auto& values=record->second;const auto words=gs::status(values);ready=(words[1]==1 && wait.kind!=1) || (wait.kind==3 && words[1]==4) || (wait.kind==0 && gs::number(values,u"x64arm_connected")) || (wait.kind==1 && (gs::number(values,u"x64arm_control_done")>=wait.sequence || !process_identity::query(gs::identity(values).pid).live(gs::identity(values)))) || (wait.kind==2 && gs::number(values,u"x64arm_control_sequence")>gs::number(values,u"x64arm_control_done"));}
            if(ready || (wait.kind!=2 && gs::now()>=wait.deadline))it->second.status=ThreadStatus::Runnable;
        }catch(const SecurityDescriptorError&){it->second.status=ThreadStatus::Runnable;}
    }
}

std::optional<std::uint32_t> WindowsServices::resume_guest_service(GuestState& state,GuestMemory& memory,std::uint64_t id,bool main){
    namespace gs=guest_service;const auto it=guest_service_dispatchers_.find(id);if(it==guest_service_dispatchers_.end())throw HostCallFault("服務回呼的執行狀態遺失");
    const auto dispatch=it->second;
    {
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_);if(transaction.error)throw HostCallFault("服務狀態交易失敗");
        const auto record=runtime_.registry.keys.find(gs::path(dispatch.label));
        if(record!=runtime_.registry.keys.end() && gs::identity(record->second)==process_identity::query(process_id()).identity &&
           gs::number(record->second,u"x64arm_instance")==service_control_handles_.at(id).instance){
            auto& values=record->second;
            if(main){const auto words=gs::status(values);if(!dispatch.stopped && words[1]!=1)gs::put_status(values,std::array<std::uint32_t,7>{words[0],1,0,1067,0,0,0});}
            else gs::put_number(values,u"x64arm_control_done",dispatch.control_sequence,true);
            if(gs::commit(runtime_.registry,transaction))throw HostCallFault("無法發布服務回呼結果");
        }
    }
    if(main){(void)free_heap(memory,dispatch.storage,state);it->second.storage=0;it->second.stopped=true;complete_call(state,memory,0);return {};}
    auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::ServiceControl;schedule(state,memory);return {};
}
