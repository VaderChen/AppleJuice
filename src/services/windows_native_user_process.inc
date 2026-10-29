// Native 建立與 Win32 共用實際宿主子程序、身分、安全、啟動握手及排程；
// 區域狀態由 coroutine 持有，等候 IPC 時不阻塞其他客體執行緒。
if(service.symbol=="NtCreateUserProcess" || service.symbol=="ZwCreateUserProcess")return service_callback::invoke<std::optional<std::uint32_t>>(
    this,threads_.current,state,memory,"NtCreateUserProcess",
    [this](GuestState& s,GuestMemory& m,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
        start_callbacks(s,m,{{target,std::move(args)}},Continuation::NestedResult,id);
    },
    [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto saved_error=last_error_;
        const auto done=[&](std::uint32_t status){last_error_=saved_error;threads_.items.at(threads_.current).last_native_status=status;call.return_to(s,status);return std::optional<std::uint32_t>{};};
#if defined(_WIN32)
        co_return done(0xc00000bbU);
#else
        using detail::NativeProcessError;
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const unsigned info_size=x86_?72U:88U,info_union=x86_?8U:16U;
        const auto create_info=call(9),process_output=call(0),thread_output=call(1);
        const auto process_flags=static_cast<std::uint32_t>(call(6)),thread_flags=static_cast<std::uint32_t>(call(7));
        const auto probe=[&](std::uint64_t at,std::size_t size){m.check(GuestAddress{at},size,Access::Write);};
        const auto pointer=[&](std::uint64_t at){return m.load(GuestAddress{at},bits);};
        const auto put=[&](std::uint64_t at,std::uint64_t value){m.store(GuestAddress{at},bits,value);};
        struct Attribute{std::uint64_t kind,address,returned;std::size_t size;};
        struct Security{std::vector<std::uint8_t> bytes;bool inherit{};};
        struct Resources {
            WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t owner;
            std::uint64_t scratch{},file{},section{},process{},thread{};bool delivered{};
            ~Resources(){
                if(self.processes_){
                    self.processes_->native_parameters.erase(owner);
                    if(!delivered && process){
                        std::shared_ptr<Child> child;
                        if(const auto found=self.processes_->handles.find(process);found!=self.processes_->handles.end())child=found->second.child;
                        self.processes_->handles.erase(process);self.processes_->handles.erase(thread);
                        self.runtime_.handle_flags.erase(process);self.runtime_.handle_flags.erase(thread);
                        if(child){child->independent=false;std::erase(self.processes_->children,child);}
                    }
                }
                if(section)(void)self.close_mapping(section);
                if(file){self.files_.erase(file);self.runtime_.win_file_positions.erase(file);self.runtime_.handle_flags.erase(file);}
                if(scratch)(void)self.free_heap(memory,scratch,state);
            }
        } resources{*this,s,m,threads_.current};
        try{
            if(!processes_ || processes_->options.engine.empty())co_return done(0xc00000bbU);
            if((process_output%unit) || (thread_output%unit) || (create_info%unit))co_return done(0x80000002U);
            probe(process_output,unit);probe(thread_output,unit);probe(create_info,info_size);
            if(pointer(create_info)!=info_size || m.load(GuestAddress{create_info+unit},32))co_return done(0xc000000dU);
            const auto init_flags=static_cast<std::uint32_t>(m.load(GuestAddress{create_info+info_union},32));
            const auto additional_access=static_cast<std::uint32_t>(m.load(GuestAddress{create_info+info_union+4},32));
            if(init_flags&0x0000fff0U)co_return done(0xc000000dU);
            if(process_flags&~0x207U || thread_flags&~1U)co_return done(0xc00000bbU);
            // 現有建立器不傳遞 debugger；不能在需要繼承時靜默遺漏。
            if(!(process_flags&2U) && m.load(GuestAddress{peb_base_.value+2},8))co_return done(0xc00000bbU);
            auto process_access=security::creation_access(static_cast<std::uint32_t>(call(2)),security::process_access_mapping);
            auto thread_access=security::creation_access(static_cast<std::uint32_t>(call(3)),security::thread_access_mapping);
            if(process_access&0x400U)process_access|=0x1000U;
            if(process_access&0x200U)process_access|=0x2000U;
            if(thread_access&0x40U)thread_access|=0x800U;
            if(thread_access&0x20U)thread_access|=0x400U;
            const auto capture_security=[&](std::uint64_t address){
                Security result;std::uint64_t descriptor=0;
                if(address){
                    if(address%unit)throw NativeProcessError{0x80000002U};m.check(GuestAddress{address},6*unit,Access::Read);
                    if(m.load(GuestAddress{address},32)!=6*unit)throw NativeProcessError{0xc000000dU};
                    const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{address+3*unit},32));
                    if(flags&~0x42U)throw NativeProcessError{0xc000000dU};
                    if(pointer(address+unit) || pointer(address+2*unit))throw NativeProcessError{0xc0000033U};
                    if(pointer(address+5*unit))throw NativeProcessError{0xc00000bbU};
                    result.inherit=(flags&2U)!=0;descriptor=pointer(address+4*unit);
                }
                result.bytes=descriptor?object_security_descriptor(security::read_descriptor(m,descriptor,x86_,true),true,true):synchronization_descriptor(m,0).bytes;
                return result;
            };
            auto process_security=capture_security(call(4)),thread_security=capture_security(call(5));
            auto parameters=detail::capture_native_process_parameters(m,call(8),x86_);
            const auto layout=detail::ProcessParameterLayout::for_x86(x86_);
            const auto text=[](const std::vector<std::uint8_t>& bytes){std::u16string value;for(std::size_t i=0;i<bytes.size();i+=2)value.push_back(static_cast<char16_t>(bytes[i]|(unsigned{bytes[i+1]}<<8)));return value;};
            const auto encode=[](std::u16string_view value){std::vector<std::uint8_t> bytes;for(const auto c:value){bytes.push_back(static_cast<std::uint8_t>(c));bytes.push_back(static_cast<std::uint8_t>(c>>8));}return bytes;};
            auto image=text(parameters.strings[2]);std::vector<Attribute> outputs;std::set<std::uint64_t> kinds;
            if(const auto list=call(10)){
                if(list%unit)co_return done(0x80000002U);const auto size=pointer(list);
                if(size<unit || size>unit+64*4*unit || (size-unit)%(4*unit))co_return done(0xc000000dU);
                m.check(GuestAddress{list},size,Access::Read);
                for(std::uint64_t offset=unit;offset<size;offset+=4*unit){
                    const auto row=list+offset,kind=pointer(row),length=pointer(row+unit),value=pointer(row+2*unit),returned=pointer(row+3*unit);
                    if(!kinds.insert(kind).second)co_return done(0xc000000dU);
                    if(returned){if(returned%unit)co_return done(0x80000002U);probe(returned,unit);}
                    if(kind==0x20005){
                        if(!length || length>65532 || length%2 || !value)co_return done(0xc000000dU);
                        std::vector<std::uint8_t> bytes(length);m.read(GuestAddress{value},bytes);image=text(bytes);
                        if(image.find(char16_t{})!=image.npos)co_return done(0xc000000dU);
                    }else if(kind==0x60000){
                        if(length!=unit)co_return done(0xc000000dU);
                        const auto type=object_type(value);if(type!=u"Process")co_return done(type.empty()?0xc0000008U:0xc0000024U);
                        if(!process_handle_pid(value,0x80U))co_return done(0xc0000022U);
                        if(!current_process_handle(value))co_return done(0xc00000bbU);
                    }else if(kind==0x10003 || kind==0x10004 || kind==6){
                        const auto expected=kind==0x10003?2*unit:kind==0x10004?unit:(x86_?48U:64U);
                        if(length!=expected)co_return done(0xc000000dU);
                        if(value%unit)co_return done(0x80000002U);probe(value,expected);outputs.push_back({kind,value,returned,expected});
                    }else co_return done(0xc00000bbU);
                }
            }
            if(image.empty())co_return done(0xc000000dU);
            if(image.starts_with(u"\\??\\"))image.erase(0,4);
            if(image.starts_with(u"UNC\\"))image=u"\\\\"+image.substr(4);
            image=normalized_windows_path(image,runtime_.current_directory);
            if(!call(8))parameters.strings[2]=encode(image);
            if(parameters.strings[3].empty())parameters.strings[3]=encode(u"\""+image+u"\"");
            if(parameters.strings[0].empty())parameters.strings[0]=encode(runtime_.current_directory);
            std::vector<std::uint8_t> environment;
            const auto standard=parameters.standard_handles;
            if(call(8)){
                const auto env=pointer(call(8)+layout.environment);
                if(env){
                    bool zero=false,terminated=false;
                    for(std::uint64_t offset=0;offset<16*1024*1024;offset+=2){
                        const auto c=m.load(GuestAddress{env+offset},16);environment.push_back(static_cast<std::uint8_t>(c));environment.push_back(static_cast<std::uint8_t>(c>>8));
                        if(!c && (zero || !offset)){terminated=true;break;}zero=!c;
                    }
                    if(!terminated)co_return done(0xc000000dU);
                    if(environment.size()==2){environment.push_back(0);environment.push_back(0);}
                }
            }
            const auto image_bytes=encode(image);
            std::size_t capacity=1024+image_bytes.size()+2+environment.size()+process_security.bytes.size()+thread_security.bytes.size();
            for(const auto& bytes:parameters.strings)capacity+=bytes.size()+16;
            resources.scratch=allocate_heap(m,capacity);if(!resources.scratch)co_return done(0xc0000017U);
            const auto scratch=resources.scratch;std::size_t used=512;m.write(GuestAddress{scratch},std::vector<std::uint8_t>(512));
            const auto save=[&](const std::vector<std::uint8_t>& bytes,bool terminate){const auto address=scratch+used;used+=bytes.size();m.write(GuestAddress{address},bytes);if(terminate){m.store(GuestAddress{scratch+used},16,0);used+=2;}used=(used+7)&~std::size_t{7};return address;};
            const auto image_address=save(image_bytes,true),command_address=save(parameters.strings[3],true),directory_address=save(parameters.strings[0],true);
            const auto environment_address=environment.empty()?0:save(environment,false);
            const auto process_sd=save(process_security.bytes,false),thread_sd=save(thread_security.bytes,false);
            const auto startup=scratch+32,pi=scratch+144,basic=scratch+176,thread_basic=scratch+224,peb_data=scratch+280,image_info=scratch+352;
            for(unsigned i=0;i<2;++i){const auto at=scratch+416+3*unit*i;m.store(GuestAddress{at},32,3*unit);put(at+unit,i?thread_sd:process_sd);m.store(GuestAddress{at+2*unit},32,i?thread_security.inherit:process_security.inherit);}
            m.store(GuestAddress{startup},32,x86_?68U:104U);
            for(unsigned i=0;i<8;++i)m.store(GuestAddress{startup+(x86_?16U:32U)+4*i},32,parameters.window[i]);
            m.store(GuestAddress{startup+(x86_?48U:64U)},16,parameters.window[8]);
            for(unsigned i=0;i<3;++i)put(startup+(x86_?56U:80U)+i*unit,standard[i]);
            const auto creation_state=[&](unsigned value){probe(create_info,info_size);m.store(GuestAddress{create_info+unit},32,value);m.write(GuestAddress{create_info+info_union},std::vector<std::uint8_t>(info_size-info_union));};
            const auto file_access=security::mapped_access(0xa0000000U|additional_access,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
            const auto opened=co_await service_callback::call_value(resolve("kernel32.dll","CreateFileW").value,{image_address,file_access,5,0,3,0x80,0});
            if(opened==(x86_?UINT64_C(0xffffffff):UINT64_MAX)){creation_state(1);co_return done(detail::process_native_status(last_error_));}
            resources.file=opened;
            auto status=static_cast<std::uint32_t>(co_await service_callback::call(resolve("ntdll.dll","NtCreateSection").value,{scratch,0xf001f,0,0,0x20,0x1000000,opened}));
            if(status&0x80000000U){creation_state(2);put(create_info+info_union,opened);resources.file=0;co_return done(status);}
            resources.section=pointer(scratch);
            status=co_await service_callback::call(resolve("ntdll.dll","NtQuerySection").value,{resources.section,1,image_info,x86_?48U:64U,0});
            if(status&0x80000000U)co_return done(status);
            const auto characteristics=m.load(GuestAddress{image_info+(x86_?28U:44U)},16),machine=m.load(GuestAddress{image_info+(x86_?32U:48U)},16);
            if(machine!=(x86_?0x14cU:0x8664U)){creation_state(4);co_return done(0xc00000bbU);}
            if((characteristics&0x2000U) || (characteristics&(init_flags>>16))){creation_state(3);m.store(GuestAddress{create_info+info_union},16,m.load(GuestAddress{image_info+(x86_?30U:46U)},16));co_return done(0xc000007bU);}
            std::optional<image_file::PeMetadata::ResourceLocation> manifest;
            if(init_flags&2U){
                const auto found=files_.find(opened);if(found==files_.end() || !found->second.native)co_return done(0xc0000008U);
                image_file::Reader reader(*found->second.native);image_file::PeMetadata metadata(reader);manifest=metadata.resource_location(24,1,1024*1024);reader.finish();
            }
            if(!processes_->native_parameters.emplace(threads_.current,parameters).second)co_return done(0xc000000dU);
            const auto created=co_await service_callback::call(resolve("internal-native-process","CreateProcessW").value,
                {image_address,command_address,scratch+416,scratch+416+3*unit,(process_flags&4U)?1U:0U,0x404U|((process_flags&1U)?0x1000000U:0U),environment_address,directory_address,startup,pi});
            processes_->native_parameters.erase(threads_.current);
            if(!created)co_return done(detail::process_native_status(last_error_));
            resources.process=pointer(pi);resources.thread=pointer(pi+unit);
            if(!processes_->handles.contains(resources.process) || !processes_->handles.contains(resources.thread))co_return done(0xc0000008U);
            status=co_await service_callback::call(resolve("ntdll.dll","NtQueryInformationProcess").value,{resources.process,0,basic,unit*6,0});
            if(status&0x80000000U)co_return done(status);
            const auto peb=pointer(basic+unit),pid=pointer(basic+4*unit);
            const auto read=co_await service_callback::call(resolve("kernel32.dll","ReadProcessMemory").value,{resources.process,peb,peb_data,x86_?0x14U:0x28U,0});
            if(!read)co_return done(detail::process_native_status(last_error_));
            const auto image_base=pointer(peb_data+(x86_?8U:16U)),remote_parameters=pointer(peb_data+(x86_?16U:32U));
            status=co_await service_callback::call(resolve("ntdll.dll","NtQueryInformationThread").value,{resources.thread,0,thread_basic,x86_?28U:48U,0});
            if(status&0x80000000U)co_return done(status);
            const auto teb=pointer(thread_basic+unit),tid=pointer(thread_basic+3*unit);
            probe(process_output,unit);probe(thread_output,unit);probe(create_info,info_size);
            for(const auto& attr:outputs){probe(attr.address,attr.size);if(attr.returned)probe(attr.returned,unit);}
            if(!(process_flags&0x200U) && !(thread_flags&1U)){
                const auto resumed=co_await service_callback::call(resolve("kernel32.dll","ResumeThread").value,{resources.thread});
                if(static_cast<std::uint32_t>(resumed)==UINT32_MAX)co_return done(detail::process_native_status(last_error_));
            }
            // 等候期間其他執行緒可能改變記憶體；發布前再次探測，失敗由 RAII 回收未交付子程序。
            probe(process_output,unit);probe(thread_output,unit);probe(create_info,info_size);
            for(const auto& attr:outputs){probe(attr.address,attr.size);if(attr.returned)probe(attr.returned,unit);}
            creation_state(6);m.store(GuestAddress{create_info+info_union},32,manifest?8U:0U);
            put(create_info+(x86_?12U:24U),resources.file);put(create_info+(x86_?16U:32U),resources.section);
            // x86 引擎只有 PEB32，WOW64 欄位交付真實的 32 位元指標，不捏造 PEB64。
            m.store(GuestAddress{create_info+(x86_?24U:40U)},64,x86_?0:remote_parameters);
            m.store(GuestAddress{create_info+(x86_?32U:48U)},32,x86_?remote_parameters:0);
            m.store(GuestAddress{create_info+(x86_?36U:52U)},32,parameters.flags|1U);
            m.store(GuestAddress{create_info+(x86_?40U:56U)},64,x86_?0:peb);
            m.store(GuestAddress{create_info+(x86_?48U:64U)},32,x86_?peb:0);
            m.store(GuestAddress{create_info+(x86_?56U:72U)},64,manifest?image_base+manifest->rva:0);
            m.store(GuestAddress{create_info+(x86_?64U:80U)},32,manifest?manifest->size:0);
            for(const auto& attr:outputs){
                if(attr.kind==0x10003){put(attr.address,pid);put(attr.address+unit,tid);}
                else if(attr.kind==0x10004)put(attr.address,teb);
                else {std::vector<std::uint8_t> bytes(attr.size);m.read(GuestAddress{image_info},bytes);m.write(GuestAddress{attr.address},bytes);}
                if(attr.returned)put(attr.returned,attr.size);
            }
            processes_->handles.at(resources.process).access=process_access;processes_->handles.at(resources.thread).access=thread_access;
            put(process_output,resources.process);put(thread_output,resources.thread);
            resources.file=0;resources.section=0;resources.delivered=true;
            co_return done(0);
        }catch(const NativeProcessError& error){co_return done(error.status);}
        catch(const MemoryFault&){co_return done(0xc0000005U);}
        catch(const SecurityDescriptorError& error){co_return done(detail::process_native_status(error.code));}
        catch(const std::bad_alloc&){co_return done(0xc0000017U);}
        catch(const std::invalid_argument&){co_return done(0xc000000dU);}
        catch(const HostCallFault&){co_return done(0xc000009aU);}
#endif
    });
