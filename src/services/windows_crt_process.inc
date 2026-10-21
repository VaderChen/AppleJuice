// CRT 的 vector 程序家族共用 Windows 程序、環境與 lowio 繼承契約。
const WindowsCall crt_call(state,memory,service);
const auto symbol=service.symbol;
const auto invalid=x86_?UINT64_C(0xffffffff):UINT64_MAX;
const auto signed_result=[&](std::uint32_t value){return x86_?std::uint64_t(value):static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));};
const auto error=[&](unsigned value,unsigned os=0){
    memory.store(GuestAddress{runtime_.errno_cell},32,value);
    memory.store(GuestAddress{crt_os_error::cell(runtime_.errno_cell)},32,os);
};
const auto os_error=[&](unsigned value){error(value==193?8U:value==206?7U:crt_os_error::mapped(value),value);};
const auto close=[&](std::uint64_t handle){
    if(const auto flags=runtime_.handle_flags.find(handle);flags!=runtime_.handle_flags.end() && (flags->second&2U))return;
    (void)close_process(handle);runtime_.handle_flags.erase(handle);
};
const auto invalid_parameter=[&]() -> std::optional<std::uint32_t>{
    error(22);
    if(const auto handler=current_invalid_parameter_handler()){
        start_callbacks(state,memory,{{handler,{0,0,0,0,0}}},Continuation::Return,invalid);
        return std::nullopt;
    }
    return finish_process(state,memory,0xc0000417U);
};
#if !defined(_WIN32)
const auto finish_wait=[&](const ProcessState::CrtWait& pending) -> std::optional<std::uint32_t>{
    const auto handle=pending.handle;
    const auto found=processes_->handles.find(handle);
    if(found==processes_->handles.end() || found->second.thread){
        error(10,6);if(pending.status)memory.store(GuestAddress{pending.status},32,UINT32_MAX);
        if(found!=processes_->handles.end())close(handle);
        processes_->crt_waits.erase(threads_.current);crt_call.return_to(state,invalid);return std::nullopt;
    }
    const auto rights=found->second.access;
    if(!(rights&0x100000U) || !(rights&0x1400U)){
        if(pending.cwait)error(10,5);else os_error(5);
        if(pending.status)memory.store(GuestAddress{pending.status},32,UINT32_MAX);
        close(handle);processes_->crt_waits.erase(threads_.current);crt_call.return_to(state,invalid);return std::nullopt;
    }
    const auto signaled=process_signaled(handle);
    if(signaled && !*signaled){
        auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};
        thread.wait.kind=ThreadWaitKind::CrtProcess;thread.wait.handles={handle};
        schedule(state,memory);return std::nullopt;
    }
    std::optional<std::uint32_t> code;
    const auto& object=found->second;
    if(object.debug_life && object.debug_life->exited)code=object.debug_life->code;
    else if(object.child && object.child->exited)code=object.child->exit_code;
    else if(object.record && object.record->exited())code=object.record->data->code;
    const bool cwait=pending.cwait;
    if(pending.status)memory.store(GuestAddress{pending.status},32,code.value_or(UINT32_MAX));
    if(!code)error(10,6);
    close(handle);processes_->crt_waits.erase(threads_.current);
    threads_.items.at(threads_.current).wait={};
    crt_call.return_to(state,code?(cwait?handle:signed_result(*code)):invalid);
    return std::nullopt;
};
if(processes_){
    if(const auto pending=processes_->crt_waits.find(threads_.current);pending!=processes_->crt_waits.end())
        return finish_wait(pending->second);
}
if(symbol=="_cwait"){
    if(crt_call(0))memory.check(GuestAddress{crt_call(0)},4,Access::Write);
    if(!processes_ || crt_call(1)==invalid || crt_call(1)==invalid-1 || !processes_->handles.contains(crt_call(1))){
        error(10,6);if(crt_call(0))memory.store(GuestAddress{crt_call(0)},32,UINT32_MAX);
        crt_call.return_to(state,invalid);return std::nullopt;
    }
    auto& pending=processes_->crt_waits[threads_.current];pending={crt_call(1),crt_call(0),true};
    return finish_wait(pending); // action 在 Windows 上不使用。
}
#else
if(symbol=="_cwait"){error(10,6);crt_call.return_to(state,invalid);return std::nullopt;}
#endif
const bool wide=symbol.starts_with("_w");
const bool system=symbol=="system" || symbol=="_wsystem";
const bool exec=symbol.starts_with("_exec") || symbol.starts_with("_wexec");
const bool search=symbol.ends_with("vp") || symbol.ends_with("vpe");
const bool explicit_environment=symbol.ends_with("ve") || symbol.ends_with("vpe");
const auto text=[&](std::uint64_t pointer){return wide?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16(read_ansi(memory,pointer));};
const auto comspec=runtime_.environment.find(u"COMSPEC");
if(system && !crt_call(0)){
    const bool exists=comspec!=runtime_.environment.end() && !filesystem_.information(utf16_to_utf8(normalized_windows_path(comspec->second.second,runtime_.current_directory))).error;
    crt_call.return_to(state,exists?1U:0U);return std::nullopt;
}
const auto mode=system?0U:exec?2U:static_cast<unsigned>(crt_call(0));
const unsigned first=exec?0U:1U;
if(!system && (mode>4 || !crt_call(first) || !crt_call(first+1)))return invalid_parameter();
try{
    std::u16string filename,command;
    GuestEnvironment environment;
    const auto env_pointer=!system && explicit_environment?crt_call(first+2):0;
    if(system){command=text(crt_call(0));}
    else{
        filename=text(crt_call(first));
        const auto vector=crt_call(first+1);const unsigned pointer=x86_?4U:8U;
        if(filename.empty() || !memory.load(GuestAddress{vector},static_cast<std::uint8_t>(pointer*8)))return invalid_parameter();
        for(unsigned i=0;;++i){
            if(i>=32767)throw std::length_error("CRT 程序引數過多");
            const auto address=memory.load(GuestAddress{vector+std::uint64_t(i)*pointer},static_cast<std::uint8_t>(pointer*8));
            if(!address)break;
            auto value=text(address);if(!i && value.empty())return invalid_parameter();
            if(i)command.push_back(u' ');command+=value;
            if(command.size()>=32767)throw std::length_error("CRT 程序命令列過長");
        }
    }
    if(env_pointer){
        for(const auto& [key,value]:runtime_.environment)if(key.starts_with(u"="))environment[key]=value;
        const unsigned pointer=x86_?4U:8U;
        for(unsigned i=0;;++i){
            if(i>=1048576)throw std::length_error("CRT 程序環境過大");
            const auto address=memory.load(GuestAddress{env_pointer+std::uint64_t(i)*pointer},static_cast<std::uint8_t>(pointer*8));
            if(!address)break;
            const auto value=text(address);const auto equals=value.find(u'=',value.starts_with(u"=")?1U:0U);
            if(equals==value.npos || !equals){error(22);crt_call.return_to(state,invalid);return std::nullopt;}
            auto name=value.substr(0,equals),key=name;for(auto& c:key)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
            environment[key]={std::move(name),value.substr(equals+1)};
        }
        if(!environment.contains(u"SYSTEMROOT"))if(const auto root=runtime_.environment.find(u"SYSTEMROOT");root!=runtime_.environment.end())environment[root->first]=root->second;
    }
    const auto environment_bytes=env_pointer?encode_guest_environment(environment):std::vector<std::uint8_t>{};
    const auto resolve=[&](std::u16string_view name,bool path_search) -> std::optional<std::u16string>{
        std::vector<std::u16string> bases{std::u16string(name)};
        if(path_search && name.find_first_of(u"\\/:")==name.npos){
            if(const auto found=runtime_.environment.find(u"PATH");found!=runtime_.environment.end()){
                const auto& path=found->second.second;std::size_t begin=0;
                do{const auto end=path.find(u';',begin);auto part=path.substr(begin,end==path.npos?path.size()-begin:end-begin);
                    if(part.size()>=2 && part.front()==u'"' && part.back()==u'"')part=part.substr(1,part.size()-2);
                    if(!part.empty())bases.push_back(part+u"\\"+std::u16string(name));
                    if(end==path.npos)break;begin=end+1;
                }while(begin<path.size());
            }
        }
        for(const auto& base:bases){
            const auto leaf=base.substr(base.find_last_of(u"\\/:")==base.npos?0:base.find_last_of(u"\\/:")+1);
            const bool extension=leaf.find(u'.')!=leaf.npos;
            for(const auto suffix:{u".com",u".exe",u".bat",u".cmd"}){
                const auto candidate=normalized_windows_path(base+(extension?u"":suffix),runtime_.current_directory);
                const auto info=filesystem_.information(utf16_to_utf8(candidate));
                if(!info.error)return candidate;
                if(info.error!=2 && info.error!=3){os_error(info.error);return std::nullopt;}
                if(extension)break;
            }
        }
        os_error(2);return std::nullopt;
    };
    const auto launch=[&](std::u16string_view name,std::u16string_view line,bool path_search) -> std::optional<std::uint64_t>{
        const auto image=resolve(name,path_search);if(!image)return std::nullopt;
        std::map<unsigned,std::pair<std::uint64_t,std::uint8_t>> descriptors;
        for(unsigned fd=0;fd<crt_lowio::descriptor_limit;++fd){
            const auto stream=crt_descriptor_stream(fd);auto* file=crt_file(stream);
            if(!file || !import_crt_lowio(memory,*file))continue;
            const auto handle=crt_lowio_handle(*file);
            if(file->noinherit || (mode==4 && fd<3) || (fd<3 && !standard_open_[fd]))descriptors[fd]={invalid,0};
            else{publish_crt_lowio(memory,*file);descriptors[fd]={handle,file->lowio_flags};}
        }
        const unsigned count=descriptors.empty()?0U:descriptors.rbegin()->first+1U,pointer=x86_?4U:8U;
        if(count>=(65535U-4)/(pointer+1)){error(12);return std::nullopt;}
        std::vector<std::uint8_t> reserved(4+count*(pointer+1));
        for(unsigned i=0;i<4;++i)reserved[i]=static_cast<std::uint8_t>(count>>(8*i));
        for(unsigned fd=0;fd<count;++fd){
            const auto entry=descriptors.find(fd);const auto handle=entry==descriptors.end()?invalid:entry->second.first;
            reserved[4+fd]=entry==descriptors.end()?0:entry->second.second;
            for(unsigned i=0;i<pointer;++i)reserved[4+count+fd*pointer+i]=static_cast<std::uint8_t>(handle>>(8*i));
        }
        const auto image_bytes=utf16_bytes(*image),line_bytes=utf16_bytes(line);
        const auto size=image_bytes.size()+line_bytes.size()+environment_bytes.size()+reserved.size()+256;
        const auto scratch=allocate_heap(memory,size);if(!scratch){error(12);return std::nullopt;}
        const auto application=scratch+256,command_line=application+image_bytes.size(),env=command_line+line_bytes.size(),payload=env+environment_bytes.size();
        const auto write=[&](std::uint64_t address,const std::vector<std::uint8_t>& bytes){for(std::size_t i=0;i<bytes.size();++i)memory.store(GuestAddress{address+i},8,bytes[i]);};
        std::optional<std::uint64_t> handle;
        try{
            for(unsigned i=0;i<256;++i)memory.store(GuestAddress{scratch+i},8,0);
            write(application,image_bytes);write(command_line,line_bytes);write(env,environment_bytes);write(payload,reserved);
            memory.store(GuestAddress{scratch},32,x86_?68U:104U);
            memory.store(GuestAddress{scratch+(x86_?50U:66U)},16,reserved.size());
            memory.store(GuestAddress{scratch+(x86_?52U:72U)},static_cast<std::uint8_t>(pointer*8),payload);
            if(mode==4)memory.store(GuestAddress{scratch+(x86_?44U:60U)},32,0x100U);
            memory.store(GuestAddress{crt_os_error::cell(runtime_.errno_cell)},32,0);
            const bool created=create_guest_process(memory,{application,command_line,0,0,1,0x400U|(mode==4?8U:0U),env_pointer?env:0,0,scratch,scratch+128},true);
            if(created){handle=memory.load(GuestAddress{scratch+128},static_cast<std::uint8_t>(pointer*8));close(memory.load(GuestAddress{scratch+128+pointer},static_cast<std::uint8_t>(pointer*8)));}
            else os_error(last_error_);
        }catch(...){(void)free_heap(memory,scratch,state);throw;}
        (void)free_heap(memory,scratch,state);return handle;
    };
    const auto saved_errno=memory.load(GuestAddress{runtime_.errno_cell},32);
    std::optional<std::uint64_t> handle;
    if(system){
        if(comspec!=runtime_.environment.end()){
            const auto& processor=comspec->second.second;
            handle=launch(processor,quote_windows_argument(processor)+u" /c "+command,false);
            const auto code=memory.load(GuestAddress{runtime_.errno_cell},32);
            if(!handle && code!=2 && code!=13){crt_call.return_to(state,invalid);return std::nullopt;}
        }
        if(!handle)handle=launch(u"cmd.exe",u"cmd.exe /c "+command,true);
        if(handle)memory.store(GuestAddress{runtime_.errno_cell},32,saved_errno);
    }else handle=launch(filename,command,search);
    if(!handle){crt_call.return_to(state,invalid);return std::nullopt;}
    if(mode==2){close(*handle);return finish_process(state,memory,0);}
    if(mode==4){close(*handle);crt_call.return_to(state,0);return std::nullopt;}
    if(mode!=0){crt_call.return_to(state,*handle);return std::nullopt;}
#if !defined(_WIN32)
    auto& pending=processes_->crt_waits[threads_.current];pending={*handle,0,false};return finish_wait(pending);
#else
    close(*handle);os_error(50);crt_call.return_to(state,invalid);return std::nullopt;
#endif
}catch(const std::bad_alloc&){error(12);}
catch(const std::length_error&){error(7);}
catch(const std::invalid_argument&){error(22);}
crt_call.return_to(state,invalid);return std::nullopt;
