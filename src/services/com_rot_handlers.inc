namespace com_rot_detail {
constexpr std::uint32_t pointer_error=0x80004003U,invalid=0x80070057U,no_memory=0x8007000eU,
    no_interface=0x80004002U,unavailable=0x800401e3U,duplicate=0x401e7U,failed=0x80004005U;
constexpr WindowsGuid rot_data_iid{0xc0,0x6b,0x9f,0xf2,0x21,0x50,0xce,0x11,0xaa,0x15,0,0,0x69,1,0x29,0x3f};
constexpr std::size_t scratch_size=4096,wire_limit=1024U*1024U;
bool failure(std::uint64_t value){return (value&0x80000000U)!=0;}
std::uint32_t status(std::uint32_t code){return code==8?no_memory:0x80070000U|(code&0xffffU);}
}

rot::SharedRot& WindowsServices::com_running_table(){
    if(com_running_table_)return *com_running_table_;
    if(!runtime_.network.external_io || !filesystem_.writable())throw rot::Error{5};
#if !defined(_WIN32)
    initialize_tokens();const auto primary=runtime_.token_objects.at(1);
    const auto caller=runtime_.token_objects.at(effective_token_id());
    // ROT 是 Windows 系統服務資料；客體不取得資料檔案的寫入權限。
    // 虛擬 LocalSystem 只用於受限制的 C 槽目錄，取得描述元後即恢復呼叫者。
    VirtualToken service;service.user.sid=sddl_sid("SY");service.owner=service.user.sid;service.primary_group=service.user.sid;service.integrity=0x4000;
    service.privileges={{17,2},{18,2},{9,2},{23,2}};
    const auto descriptor=sddl_descriptor("O:SYG:SYD:P(A;OICI;FA;;;SY)");
    const auto restore=[&]{filesystem_.set_security_context(caller,primary.user.sid,primary.primary_group);};
    filesystem_.set_security_context(service,service.user.sid,service.primary_group);
    int directory=-1;
    try{
        std::string path="C:";
        for(const auto* part:{"ProgramData","MacWin","COM","v1","ROT"}){
            path+="/";path+=part;auto info=filesystem_.information(path);
            if(info.error==2 || info.error==3){const auto error=filesystem_.create_directory(path,[&]{return descriptor;});if(error && error!=183)throw rot::Error{error};info=filesystem_.information(path);}
            if(info.error)throw rot::Error{info.error};if((info.metadata.attributes&0x410U)!=0x10U)throw rot::Error{5};
        }
        const auto sd=security::file_descriptor(filesystem_.security_information(path,true),0x17);
        const auto expected=security::parse_relative_descriptor(descriptor);
        if(sd.data[0]!=expected.data[0] || sd.data[3]!=expected.data[3])throw rot::Error{5};
        auto opened=filesystem_.open_directory_observer(path);if(opened.error)throw rot::Error{opened.error};
        directory=opened.file->duplicate_descriptor();if(directory<0)throw rot::Error{5};
        if(::fchmod(directory,0700))throw rot::Error{5};
        std::array<std::uint8_t,16> nonce{};if(RAND_bytes(nonce.data(),static_cast<int>(nonce.size()))!=1)throw rot::Error{8};
        const auto private_file=security::inherit_descriptor({},sd,service.user.sid,service.primary_group,false);
        const auto initialize_file=[private_file](int descriptor){
            const auto copy=::fcntl(descriptor,F_DUPFD_CLOEXEC,0);if(copy<0)throw rot::Error{5};HostFile file(copy);
            const auto native=security::read_native_file_security(file,true);
            if(native.mapped_descriptor){const auto existing=security::file_descriptor(native,0x17);if(existing.data[0]!=private_file.data[0] || existing.data[1]!=private_file.data[1] || existing.data[3]!=private_file.data[3])throw rot::Error{5};return;}
            const auto information=file.information();if(information.error)throw rot::Error{information.error};if(information.metadata.size)throw rot::Error{5};
            security::write_virtual_file_security(file,private_file,7U|(private_file.control&16U?16U:0U));
        };
        auto shared=std::make_shared<rot::SharedRot>(directory,nonce,initialize_file);::close(directory);directory=-1;
        restore();com_running_table_=std::move(shared);return *com_running_table_;
    }catch(const SecurityDescriptorError& error){if(directory>=0)::close(directory);restore();throw rot::Error{error.code};}
    catch(...){if(directory>=0)::close(directory);restore();throw;}
#else
    throw rot::Error{50};
#endif
}
rot::Scope WindowsServices::com_rot_scope(){
    initialize_tokens();const auto token=effective_token_id();
    rot::Scope scope{utf8_to_utf16(token_user_sid_string(token)),u"winsta0",runtime_.token_objects.at(token).session};
    if(gui_.station_handle){const auto handle=gui_.user_handles.find(gui_.station_handle);
        if(handle==gui_.user_handles.end())throw rot::Error{6};const auto object=gui_.user_objects.find(handle->second.object);
        if(object==gui_.user_objects.end())throw rot::Error{6};scope.station=object->second.name;
        for(auto& unit:scope.station)unit=unicode_case_unit(unit,false);
    }return scope;
}
bool WindowsServices::com_rot_allow_any(){
    initialize_tokens();RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
    if(transaction.error)throw rot::Error{transaction.error};
    // ALLOWANYCLIENT 依機器 AppID 安裝資料授權，不讀使用者可寫的 HKCU 視圖。
    RegistryTextView lookup{runtime_.registry,{std::u16string(1,2)+u"\\software\\classes"},registry_view(0,x86_),false};
    auto name=runtime_.image.path;const auto slash=name.find_last_of(u"/\\");if(slash!=name.npos)name.erase(0,slash+1);
    if(name.empty())return false;const auto app=lookup.value(u"AppID\\"+name,u"AppID");
    if(!app || !parse_windows_guid(*app,true))return false;
    const auto base=u"AppID\\"+*app;const auto service=lookup.value(base,u"LocalService"),run_as=lookup.value(base,u"RunAs");
    return (service && !service->empty()) || (run_as && !run_as->empty());
}

std::uint64_t WindowsServices::create_com_rot_enum(GuestState& state,GuestMemory& memory,ComRotEnumeration value){
    auto& bridge=runtime_.com_rot;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(bridge.enumerators.size()>=4096)throw std::bad_alloc();
    if(!bridge.enum_table){const auto table=runtime_allocate(memory,{},7*unit);unsigned i=0;
        for(const auto& method:com_context_service_descriptors())if(method.family=="internal-com-rot-enum")
            memory.store(GuestAddress{table+unit*i++},bits,resolve(method.family,method.symbol).value);
        bridge.enum_table=table;
    }
    const auto address=allocate_heap(memory,unit);if(!address)throw std::bad_alloc();
    try{memory.store(GuestAddress{address},bits,bridge.enum_table);bridge.enumerators.emplace(address,std::move(value));}
    catch(...){(void)free_heap(memory,address,state);throw;}return address;
}
std::vector<GuestCallback> WindowsServices::dispose_com_rot_enum(GuestState& state,GuestMemory& memory,std::uint64_t address){
    auto& enumerators=runtime_.com_rot.enumerators;const auto found=enumerators.find(address);
    if(found==enumerators.end() || found->second.references || found->second.pins)return {};
    auto callbacks=release_com_bind_references(memory,found->second.monikers);
    enumerators.erase(found);(void)free_heap(memory,address,state);return callbacks;
}
void WindowsServices::discard_com_rot(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& bridge=runtime_.com_rot;const auto found=bridge.operations.find(id);if(found==bridge.operations.end())return;
    auto op=std::move(found->second);bridge.operations.erase(found);
    const auto retire=[&](std::uint64_t object){if(object)if(const auto pending=release_com_proxy_owned(state,memory,{threads_.current,object}))queue_com_proxy_retirement(memory,pending);};
    if(op.object_held)retire(op.object);for(const auto object:{op.context,op.reduced,op.rot_data,op.stream})retire(object);
    if(op.candidate_owned)retire(op.delivered);for(const auto object:op.delivered_objects)retire(object);
    const auto retire_enum=[&](std::uint64_t address,bool newly_created){const auto record=bridge.enumerators.find(address);if(record==bridge.enumerators.end())return;
        if(newly_created)record->second.references=0;else if(record->second.pins)--record->second.pins;
        if(!record->second.pins && !record->second.references){auto value=std::move(record->second);bridge.enumerators.erase(record);(void)free_heap(memory,address,state);
            for(const auto& reference:value.monikers){const auto owner=com_bind_detail::same_apartment(reference,threads_.current,com_apartment(threads_.current))?threads_.current:reference.owner;
                if(const auto pending=release_com_proxy_owned(state,memory,{owner,reference.object}))queue_com_proxy_retirement(memory,pending);}}};
    if(op.enumerator)retire_enum(op.enumerator,false);if(op.created_enumerator)retire_enum(op.created_enumerator,true);
    if(op.buffer)(void)free_heap(memory,op.buffer,state);if(op.scratch)(void)free_heap(memory,op.scratch,state);
    // 正常撤銷交由 CoReleaseMarshalData；異常離開則加入共同引用退役佇列。
    if(!op.published)for(const auto* bytes:{&op.object_wire,&op.moniker_wire}){
        if(const auto wire=com_marshal::decode(*bytes);wire && wire->pid==process_id())if(const auto packet=runtime_.com_marshal.packets.release(wire->token))
            if(!com_marshal::weak_flags(packet->flags))if(const auto pending=release_com_proxy_owned(state,memory,{packet->owner,packet->object}))queue_com_proxy_retirement(memory,pending);
    }
}

std::optional<std::uint32_t> WindowsServices::resume_com_rot(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t returned){
    using namespace com_rot_detail;using Kind=ComRotOperation::Kind;auto& bridge=runtime_.com_rot;auto& op=bridge.operations.at(id);
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);const unsigned unit=bits/8;
    const auto cell=[&](unsigned offset){return memory.load(GuestAddress{op.scratch+offset},bits);};
    const auto callback=[&](GuestCallback call,unsigned next){op.step=next;start_callbacks(state,memory,{std::move(call)},Continuation::ComRotOperation,id);return std::optional<std::uint32_t>{};};
    const auto api=[&](std::string_view name,std::vector<std::uint64_t> arguments,unsigned next){return callback({resolve("ole32.dll",name).value,std::move(arguments)},next);};
    const auto transfer=[&](const rot::Bytes& bytes,WindowsGuid iid,unsigned next,bool release=false){
        if(bytes.empty() || bytes.size()>wire_limit)throw rot::Error{13};
        if(op.buffer){(void)free_heap(memory,op.buffer,state);op.buffer=0;}
        op.buffer=allocate_heap(memory,bytes.size());if(!op.buffer)throw std::bad_alloc();memory.write(GuestAddress{op.buffer},bytes);
        memory.write(GuestAddress{op.scratch+64},iid);memory.store(GuestAddress{op.scratch+32},bits,0);op.resume_step=next;op.release_wire=release;op.transfer_failed=false;
        return callback({resolve("shlwapi.dll","SHCreateMemStream").value,{op.buffer,bytes.size()}},100);
    };
    const auto cleanup=[&](std::uint32_t result){
        op.result=result;
        if(op.stream_marshaled){op.stream_marshaled=false;GuestCallback seek{context_method(memory,op.stream,5),{op.stream,0,0,0}};seek.wide_arguments=2;return callback(std::move(seek),91);}
        if(!op.published && op.object_marshaled){op.object_marshaled=false;return transfer(op.object_wire,context_iid(0),90,true);}
        if(!op.published && op.moniker_marshaled){op.moniker_marshaled=false;return transfer(op.moniker_wire,context_iid(0xf),90,true);}
        if(op.object_held){op.object_held=false;return callback({context_method(memory,op.object,2),{op.object}},90);}
        for(auto* object:{&op.delivered,&op.rot_data,&op.stream,&op.reduced,&op.context}){
            if(!*object || (object==&op.delivered && !op.candidate_owned))continue;
            const auto address=*object;*object=0;if(object==&op.delivered)op.candidate_owned=false;
            return callback({context_method(memory,address,2),{address}},90);
        }
        if(!op.delivered_objects.empty()){const auto object=op.delivered_objects.back();op.delivered_objects.pop_back();return callback({context_method(memory,object,2),{object}},90);}
        std::vector<GuestCallback> releases;
        if(op.enumerator){const auto address=op.enumerator;auto& value=bridge.enumerators.at(address);if(value.pins)--value.pins;op.enumerator=0;releases=dispose_com_rot_enum(state,memory,address);}
        if(op.created_enumerator){const auto address=op.created_enumerator;bridge.enumerators.at(address).references=0;op.created_enumerator=0;
            auto calls=dispose_com_rot_enum(state,memory,address);releases.insert(releases.end(),std::make_move_iterator(calls.begin()),std::make_move_iterator(calls.end()));}
        discard_com_rot(state,memory,id);
        if(releases.empty())complete_call(state,memory,result);else start_callbacks(state,memory,std::move(releases),Continuation::Return,result);return std::optional<std::uint32_t>{};
    };
    try{
        if(op.step==90 || op.step==92)return cleanup(op.result);
        if(op.step==91){if(failure(returned))return cleanup(op.result);return api("CoReleaseMarshalData",{op.stream},92);}
        if(op.step==100){op.stream=returned;if(!op.stream)return cleanup(no_memory);
            if(op.release_wire)return api("CoReleaseMarshalData",{op.stream},101);
            return api("CoUnmarshalInterface",{op.stream,op.scratch+64,op.scratch+32},101);
        }
        if(op.step==101){op.transfer_failed=failure(returned);op.transfer_result=static_cast<std::uint32_t>(returned);
            if(!op.release_wire){op.delivered=cell(32);op.candidate_owned=op.delivered!=0;if(!op.transfer_failed && !op.delivered){op.transfer_result=failed;op.transfer_failed=true;}}
            const auto stream=op.stream;op.stream=0;return callback({context_method(memory,stream,2),{stream}},102);
        }
        if(op.step==102){if(op.buffer){(void)free_heap(memory,op.buffer,state);op.buffer=0;}
            if(op.resume_step==90)return cleanup(op.result);
            if(op.transfer_failed)return cleanup(op.transfer_result);op.step=op.resume_step;
        }
        if(op.kind==Kind::Revoke){
            if(op.step==0){auto revoked=com_running_table().revoke(op.cookie);if(!revoked)return cleanup(invalid);op.object_wire=std::move(revoked->object);op.moniker_wire=std::move(revoked->moniker);
                if(!op.object_wire.empty())return transfer(op.object_wire,context_iid(0),1,true);op.step=1;}
            if(op.step==1){if(!op.moniker_wire.empty())return transfer(op.moniker_wire,context_iid(0xf),2,true);op.step=2;}
            return cleanup(0);
        }
        if(op.kind==Kind::Snapshot || op.kind==Kind::Clone || op.kind==Kind::Enumerate){
            if(op.step==30){
                if(op.kind!=Kind::Snapshot){if(op.proxy_transfer){op.delivered=returned;op.candidate_owned=op.delivered!=0;const auto error=static_cast<std::uint32_t>(memory.load(GuestAddress{op.scratch+32},32));if(failure(error) || !op.delivered)return cleanup(failure(error)?error:failed);}else op.candidate_owned=true;}
                op.delivered_objects.push_back(op.delivered);op.delivered=0;op.candidate_owned=false;
            }
            if(op.kind==Kind::Snapshot){if(op.cursor<op.snapshot.size())return transfer(op.snapshot[op.cursor++].moniker,context_iid(0xf),30);}
            else{
                auto& enumeration=bridge.enumerators.at(op.enumerator);
                if(op.delivered_objects.size()<op.requested && op.cursor<enumeration.monikers.size()){
                    const auto& reference=enumeration.monikers[op.cursor++];
                    op.proxy_transfer=!com_bind_detail::same_apartment(reference,threads_.current,com_apartment(threads_.current));
                    if(op.proxy_transfer){memory.store(GuestAddress{op.scratch+32},32,0);
                        return callback({resolve("internal-com-proxy","Marshal").value,{reference.object,reference.owner,threads_.current,static_cast<unsigned>(com_proxy::Kind::Moniker),op.scratch+32}},30);}
                    op.delivered=reference.object;return callback({context_method(memory,reference.object,1),{reference.object}},30);
                }
            }
            if(op.kind==Kind::Snapshot || op.kind==Kind::Clone){
                ComRotEnumeration value;value.monikers.reserve(op.delivered_objects.size());
                for(const auto object:op.delivered_objects)value.monikers.push_back({threads_.current,object,com_apartment(threads_.current)});
                if(op.kind==Kind::Clone)value.cursor=bridge.enumerators.at(op.enumerator).cursor;
                const auto created=create_com_rot_enum(state,memory,std::move(value));op.created_enumerator=created;op.delivered_objects.clear();
                memory.store(GuestAddress{op.output},bits,created);op.created_enumerator=0;return cleanup(0);
            }
            auto& enumeration=bridge.enumerators.at(op.enumerator);
            for(std::size_t i=0;i<op.delivered_objects.size();++i)memory.store(GuestAddress{op.output+i*unit},bits,op.delivered_objects[i]);
            if(op.count_output)memory.store(GuestAddress{op.count_output},32,op.delivered_objects.size());
            const auto result=op.delivered_objects.size()==op.requested?0U:1U;
            enumeration.cursor=op.cursor;op.delivered_objects.clear();return cleanup(result);
        }
        if(op.step==0){memory.store(GuestAddress{op.scratch},bits,0);return api("CreateBindCtx",{0,op.scratch},1);}
        if(op.step==1){if(failure(returned))return cleanup(static_cast<std::uint32_t>(returned));op.context=cell(0);if(!op.context)return cleanup(failed);
            memory.store(GuestAddress{op.scratch+8},bits,0);return callback({context_method(memory,op.moniker,10),{op.moniker,op.context,0,0,op.scratch+8}},2);}
        if(op.step==2){op.reduced=cell(8);if(failure(returned))return cleanup(static_cast<std::uint32_t>(returned));if(!op.reduced)return cleanup(invalid);
            memory.write(GuestAddress{op.scratch+64},rot_data_iid);memory.store(GuestAddress{op.scratch+16},bits,0);
            return callback({context_method(memory,op.reduced,0),{op.reduced,op.scratch+64,op.scratch+16}},3);
        }
        if(op.step==3){op.rot_data=cell(16);if(!failure(returned) && op.rot_data){memory.store(GuestAddress{op.scratch+56},32,0);return callback({context_method(memory,op.rot_data,3),{op.rot_data,op.scratch+80,2048,op.scratch+56}},4);}
            if(returned!=no_interface && failure(returned))return cleanup(static_cast<std::uint32_t>(returned));op.step=5;
        }
        if(op.step==4){if(failure(returned))return cleanup(static_cast<std::uint32_t>(returned));const auto size=memory.load(GuestAddress{op.scratch+56},32);if(!size || size>2048)return cleanup(invalid);
            op.comparison.resize(size);memory.read(GuestAddress{op.scratch+80},op.comparison);op.step=5;
        }
        if(op.step==5){op.snapshot=com_running_table().snapshot(com_rot_scope());op.candidate=0;op.step=6;}
        if(op.step==7)return callback({context_method(memory,op.reduced,13),{op.reduced,op.delivered}},8);
        if(op.step==8){const auto object=op.delivered;op.delivered=0;op.candidate_owned=false;
            if(!returned){op.duplicate=true;op.step=9;}else{if(failure(returned))return cleanup(static_cast<std::uint32_t>(returned));++op.candidate;op.step=6;}
            return callback({context_method(memory,object,2),{object}},op.step);
        }
        if(op.step==6){
            while(op.candidate<op.snapshot.size()){
                const auto& candidate=op.snapshot[op.candidate];
                if(!op.comparison.empty() && !candidate.comparison.empty()){
                    if(op.comparison==candidate.comparison){op.duplicate=true;op.step=9;break;}++op.candidate;continue;
                }
                return transfer(candidate.moniker,context_iid(0xf),7);
            }
            if(op.step==6)op.step=9;
        }
        if(op.step==9){
            if(op.kind!=Kind::Register){
                if(!op.duplicate)return cleanup(op.kind==Kind::GetObject?unavailable:1);
                return transfer(op.snapshot.at(op.candidate).object,context_iid(0),31);
            }
            memory.store(GuestAddress{op.scratch+48},64,0);return callback({context_method(memory,op.reduced,16),{op.reduced,op.context,0,op.scratch+48}},10);
        }
        if(op.step==31){
            if(op.kind==Kind::GetObject){memory.store(GuestAddress{op.output},bits,op.delivered);op.delivered=0;op.candidate_owned=false;}
            else if(op.kind==Kind::GetTime)memory.store(GuestAddress{op.output},64,op.snapshot.at(op.candidate).changed);
            return cleanup(0);
        }
        if(op.step==10){if(failure(returned) || returned==1)return callback({resolve("kernel32.dll","GetSystemTimeAsFileTime").value,{op.scratch+48}},11);op.step=11;}
        if(op.step==11){op.changed=memory.load(GuestAddress{op.scratch+48},64);return callback({context_method(memory,op.object,1),{op.object}},12);}
        if(op.step==12){op.object_held=true;memory.store(GuestAddress{op.scratch+24},bits,0);return api("CreateStreamOnHGlobal",{0,1,op.scratch+24},13);}
        if(op.step==13 || op.step==18){op.stream=cell(24);if(failure(returned))return cleanup(static_cast<std::uint32_t>(returned));if(!op.stream)return cleanup(failed);
            const bool moniker=op.step==18;memory.write(GuestAddress{op.scratch+64},context_iid(moniker?0xf:0));
            // MSHCTX_LOCAL 必須經過跨程序共同封送機制，不能改用 INPROC 冒充機器範圍。
            return api("CoMarshalInterface",{op.stream,op.scratch+64,moniker?op.reduced:op.object,0,0,moniker?1U:((op.flags&1U)?1U:2U)},moniker?19:14);
        }
        if(op.step==14 || op.step==19){if(failure(returned))return cleanup(static_cast<std::uint32_t>(returned));op.stream_marshaled=true;memory.store(GuestAddress{op.scratch+40},bits,0);return api("GetHGlobalFromStream",{op.stream,op.scratch+40},op.step==14?15:20);}
        if(op.step==15 || op.step==20){if(failure(returned))return cleanup(static_cast<std::uint32_t>(returned));const auto global=gui_.global_memory.find(cell(40));
            if(global==gui_.global_memory.end() || !global->second.size || global->second.size>wire_limit)return cleanup(invalid);
            const bool moniker=op.step==20;auto& bytes=moniker?op.moniker_wire:op.object_wire;bytes.resize(global->second.size);memory.read(GuestAddress{global->second.address},bytes);
            if(moniker)op.moniker_marshaled=true;else op.object_marshaled=true;op.stream_marshaled=false;
            const auto stream=op.stream;op.stream=0;return callback({context_method(memory,stream,2),{stream}},moniker?21:16);
        }
        if(op.step==16){op.object_held=false;return callback({context_method(memory,op.object,2),{op.object}},17);}
        if(op.step==17){memory.store(GuestAddress{op.scratch+24},bits,0);return api("CreateStreamOnHGlobal",{0,1,op.scratch+24},18);}
        if(op.step==21){rot::Entry entry;entry.pid=process_id();entry.thread=threads_.current;entry.flags=op.flags;entry.scope=com_rot_scope();entry.comparison=op.comparison;entry.object=op.object_wire;entry.moniker=op.moniker_wire;entry.changed=op.changed;
            const auto published=com_running_table().publish(std::move(entry),(op.flags&2U)?com_rot_allow_any():false);
            op.published=true;op.cookie=published.first.cookie;
            try{memory.store(GuestAddress{op.output},32,op.cookie);}catch(...){(void)com_running_table().revoke(op.cookie);op.published=false;throw;}
            return cleanup(op.duplicate || published.second?duplicate:0);
        }
        throw HostCallFault("ROT 延續狀態不合法");
    }catch(const MemoryFault&){return cleanup(pointer_error);}catch(const rot::Error& error){return cleanup(status(error.code));}
    catch(const std::bad_alloc&){return cleanup(no_memory);}catch(...){discard_com_rot(state,memory,id);throw;}
}

std::optional<std::uint32_t> WindowsServices::invoke_com_rot(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using namespace com_rot_detail;using Kind=ComRotOperation::Kind;const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    auto& bridge=runtime_.com_rot;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    try{
        if(name=="GetRunningObjectTable"){
            if(!arg(1))return done(pointer_error);memory.store(GuestAddress{arg(1)},bits,0);if(static_cast<std::uint32_t>(arg(0)))return done(0x8000ffffU);
            (void)com_running_table();if(bridge.objects.size()>=4096)return done(no_memory);
            if(!bridge.table){const auto table=runtime_allocate(memory,{},10*unit);unsigned i=0;for(const auto& method:com_context_service_descriptors())if(method.family=="internal-com-rot")memory.store(GuestAddress{table+unit*i++},bits,resolve(method.family,method.symbol).value);bridge.table=table;}
            const auto object=allocate_heap(memory,unit);if(!object)return done(no_memory);
            try{memory.store(GuestAddress{object},bits,bridge.table);bridge.objects.emplace(object,1);memory.store(GuestAddress{arg(1)},bits,object);}catch(...){bridge.objects.erase(object);(void)free_heap(memory,object,state);throw;}return done(0);
        }
        const auto object=arg(0);const bool enumeration=service.family=="internal-com-rot-enum";
        const auto found=bridge.objects.find(object);const auto enumerator=bridge.enumerators.find(object);
        if(enumeration?enumerator==bridge.enumerators.end():found==bridge.objects.end())return done(name=="AddRef" || name=="Release"?0:pointer_error);
        auto& references=enumeration?enumerator->second.references:found->second;
        if(name=="QueryInterface"){
            if(!arg(2))return done(pointer_error);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(pointer_error);WindowsGuid iid{};memory.read(GuestAddress{arg(1)},iid);
            if(iid!=context_iid(0) && iid!=context_iid(enumeration?0x102:0x10))return done(no_interface);if(references==UINT32_MAX)return done(no_memory);++references;memory.store(GuestAddress{arg(2)},bits,object);return done(0);
        }
        if(name=="AddRef"){if(references==UINT32_MAX)return done(0);return done(++references);}
        if(name=="Release"){
            if(!references)return done(0);if(--references)return done(references);
            if(enumeration){auto calls=dispose_com_rot_enum(state,memory,object);if(calls.empty())return done(0);start_callbacks(state,memory,std::move(calls),Continuation::Return,0);return std::nullopt;}
            bridge.objects.erase(found);(void)free_heap(memory,object,state);return done(0);
        }
        ComRotOperation op;
        if(enumeration){auto& value=enumerator->second;
            if(name=="Reset"){if(value.pins)return done(0x8001010dU);value.cursor=0;return done(0);}
            if(name=="Skip"){if(value.pins)return done(0x8001010dU);const auto requested=static_cast<std::uint32_t>(arg(1));const auto count=std::min<std::size_t>(requested,value.monikers.size()-value.cursor);value.cursor+=count;return done(count==requested?0:1);}
            if(value.pins)return done(0x8001010dU);
            if(name=="Clone"){
                if(!arg(1))return done(pointer_error);memory.store(GuestAddress{arg(1)},bits,0);op.kind=Kind::Clone;op.output=arg(1);op.requested=static_cast<std::uint32_t>(value.monikers.size());op.enumerator=object;op.cursor=0;op.step=29;op.delivered_objects.reserve(value.monikers.size());
            }else{
                if(name!="Next")throw HostCallFault("未知 IEnumMoniker 方法");
                op.requested=static_cast<std::uint32_t>(arg(1));if(!arg(3) && op.requested>1)return done(pointer_error);if(arg(3))memory.store(GuestAddress{arg(3)},32,0);
                if(op.requested && !arg(2))return done(pointer_error);const auto count=std::min<std::size_t>(op.requested,value.monikers.size()-value.cursor);memory.check(GuestAddress{arg(2)},count*unit,Access::Write);
                op.kind=Kind::Enumerate;op.output=arg(2);op.count_output=arg(3);op.enumerator=object;op.cursor=value.cursor;op.step=29;op.delivered_objects.reserve(count);
            }
        }else if(name=="NoteChangeTime"){
            if(!arg(2))return done(pointer_error);return done(com_running_table().change(static_cast<std::uint32_t>(arg(1)),memory.load(GuestAddress{arg(2)},64))?0:invalid);
        }else if(name=="EnumRunning"){
            if(!arg(1))return done(pointer_error);memory.store(GuestAddress{arg(1)},bits,0);op.kind=Kind::Snapshot;op.snapshot=com_running_table().snapshot(com_rot_scope());
            op.output=arg(1);op.step=29;op.delivered_objects.reserve(op.snapshot.size());
        }else if(name=="Revoke"){
            op.kind=Kind::Revoke;op.cookie=static_cast<std::uint32_t>(arg(1));if(!op.cookie)return done(invalid);
        }else if(name=="Register"){
            if(!arg(4))return done(pointer_error);memory.store(GuestAddress{arg(4)},32,0);op.flags=static_cast<std::uint32_t>(arg(1));if((op.flags&~3U) || !arg(2) || !arg(3))return done(invalid);
            if((op.flags&2U) && !com_rot_allow_any())return done(0x80070005U);op.kind=Kind::Register;op.object=arg(2);op.moniker=arg(3);op.output=arg(4);
        }else{
            if(!arg(1))return done(invalid);op.moniker=arg(1);
            if(name=="IsRunning")op.kind=Kind::IsRunning;
            else if(name=="GetObject" || name=="GetTimeOfLastChange"){
                if(!arg(2))return done(pointer_error);op.output=arg(2);op.kind=name=="GetObject"?Kind::GetObject:Kind::GetTime;
                if(op.kind==Kind::GetObject)memory.store(GuestAddress{op.output},bits,0);else memory.check(GuestAddress{op.output},8,Access::Write);
            }else throw HostCallFault("未知 IRunningObjectTable 方法");
        }
        if(bridge.operations.size()>=4096)return done(no_memory);op.scratch=allocate_heap(memory,scratch_size);if(!op.scratch)return done(no_memory);
        const auto scratch=op.scratch;auto id=bridge.next;while(!id || bridge.operations.contains(id))++id;bridge.next=id+1;
        try{bridge.operations.emplace(id,std::move(op));}catch(...){(void)free_heap(memory,scratch,state);throw;}
        if(bridge.operations.at(id).enumerator)++bridge.enumerators.at(object).pins;
        return resume_com_rot(state,memory,id,0);
    }catch(const MemoryFault&){return done(pointer_error);}catch(const rot::Error& error){return done(status(error.code));}catch(const std::bad_alloc&){return done(no_memory);}
}
