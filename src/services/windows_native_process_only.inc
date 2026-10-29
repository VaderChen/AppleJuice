// SEC_IMAGE 建立真實零執行緒程序；建立、啟動握手及失敗回收沿用共用程序 transport。
if(service.symbol=="NtCreateProcess" || service.symbol=="ZwCreateProcess" ||
   service.symbol=="NtCreateProcessEx" || service.symbol=="ZwCreateProcessEx")
return service_callback::invoke<std::optional<std::uint32_t>>(
    this,threads_.current,state,memory,"NtCreateProcess",
    [this](GuestState& s,GuestMemory& m,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
        start_callbacks(s,m,{{target,std::move(args)}},Continuation::NestedResult,id);
    },
    [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto saved_error=last_error_;
        const auto done=[&](std::uint32_t status){
            last_error_=saved_error;threads_.items.at(threads_.current).last_native_status=status;
            call.return_to(s,status);return std::optional<std::uint32_t>{};
        };
#if defined(_WIN32)
        co_return done(0xc00000bbU);
#else
        using detail::NativeProcessError;
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto pointer=[&](std::uint64_t at){return m.load(GuestAddress{at},bits);};
        const auto put=[&](std::uint64_t at,std::uint64_t value){m.store(GuestAddress{at},bits,value);};
        struct Resources{
            WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t owner;
            std::uint64_t scratch{},process{};bool delivered{};
            ~Resources(){
                if(self.processes_){
                    self.processes_->native_images.erase(owner);
                    if(process && !delivered){
                        std::shared_ptr<Child> child;
                        if(const auto found=self.processes_->handles.find(process);found!=self.processes_->handles.end())child=found->second.child;
                        self.processes_->handles.erase(process);self.runtime_.handle_flags.erase(process);
                        if(child){child->independent=false;std::erase(self.processes_->children,child);}
                    }
                }
                if(scratch)(void)self.free_heap(memory,scratch,state);
            }
        } resources{*this,s,m,threads_.current};
        try{
            const auto output=call(0),parent=call(3),section=call(5);
            if(output%unit)co_return done(0x80000002U);
            m.check(GuestAddress{output},unit,Access::Write);
            if(!processes_ || processes_->options.engine.empty())co_return done(0xc00000bbU);
            const bool extended=service.symbol.ends_with("Ex");
            const auto flags=extended?static_cast<std::uint32_t>(call(4)):(static_cast<std::uint8_t>(call(4))?4U:0U);
            if(extended && static_cast<std::uint32_t>(call(8)))co_return done(0xc000000dU);
            if(flags&~7U)co_return done(0xc00000bbU);
            auto access=security::creation_access(static_cast<std::uint32_t>(call(1)),security::process_access_mapping);
            if(access&0x400U)access|=0x1000U;if(access&0x200U)access|=0x2000U;
            const auto parent_type=object_type(parent);
            if(parent_type!=u"Process")co_return done(parent_type.empty()?0xc0000008U:0xc0000024U);
            if(!process_handle_pid(parent,0x80U))co_return done(0xc0000022U);
            if(!current_process_handle(parent))co_return done(0xc00000bbU);
            if(call(6) || call(7) || (!(flags&2U) && m.load(GuestAddress{peb_base_.value+2},8)))co_return done(0xc00000bbU);
            // 空 section 要複製父位址空間；不能用重新載入 EXE 冒充 clone。
            if(!section)co_return done(0xc00000bbU);
            const auto section_type=object_type(section);
            if(section_type!=u"Section")co_return done(section_type.empty()?0xc0000008U:0xc0000024U);
            ProcessImageSource image;const auto source_error=process_image_source(section,image);
            if(source_error)co_return done(detail::process_native_status(source_error));
            if(image.machine!=(x86_?0x14cU:0x8664U))co_return done(0xc00000bbU);
            std::uint64_t descriptor=0;bool inherit=false;
            if(const auto attributes=call(2)){
                if(attributes%unit)co_return done(0x80000002U);
                m.check(GuestAddress{attributes},6*unit,Access::Read);
                if(m.load(GuestAddress{attributes},32)!=6*unit)co_return done(0xc000000dU);
                const auto attributes_flags=static_cast<std::uint32_t>(m.load(GuestAddress{attributes+3*unit},32));
                if(attributes_flags&~0x42U)co_return done(0xc000000dU);
                if(pointer(attributes+unit) || pointer(attributes+2*unit))co_return done(0xc0000033U);
                if(pointer(attributes+5*unit))co_return done(0xc00000bbU);
                inherit=(attributes_flags&2U)!=0;descriptor=pointer(attributes+4*unit);
            }
            const auto security_bytes=descriptor?
                object_security_descriptor(security::read_descriptor(m,descriptor,x86_,true),true,true):synchronization_descriptor(m,0).bytes;
            const auto image_size=(image.path.size()+1)*2;
            resources.scratch=allocate_heap(m,256+image_size+security_bytes.size());
            if(!resources.scratch)co_return done(0xc0000017U);
            const auto base=resources.scratch,startup=base+32,pi=base+144,attributes=base+192;
            m.write(GuestAddress{base},std::vector<std::uint8_t>(256));
            const auto image_address=base+256,security_address=image_address+image_size;
            for(std::size_t i=0;i<=image.path.size();++i)m.store(GuestAddress{image_address+i*2},16,i<image.path.size()?image.path[i]:0);
            m.write(GuestAddress{security_address},security_bytes);
            m.store(GuestAddress{attributes},32,3*unit);put(attributes+unit,security_address);m.store(GuestAddress{attributes+2*unit},32,inherit);
            m.store(GuestAddress{startup},32,x86_?68U:104U);
            if(!processes_->native_images.emplace(threads_.current,std::move(image)).second)co_return done(0xc000000dU);
            const auto created=co_await service_callback::call(resolve("internal-native-process-only","CreateProcessW").value,
                {image_address,0,attributes,0,(flags&4U)?1U:0U,0x40cU|((flags&1U)?0x1000000U:0U),0,0,startup,pi});
            if(!created)co_return done(detail::process_native_status(last_error_));
            resources.process=pointer(pi);
            if(pointer(pi+unit) || !processes_->handles.contains(resources.process))co_return done(0xc0000008U);
            // 等候可讓其他客體執行緒修改輸出頁；所有輸出重新探測後才交付 handle。
            m.check(GuestAddress{output},unit,Access::Write);
            processes_->handles.at(resources.process).access=access;put(output,resources.process);
            resources.delivered=true;co_return done(0);
        }catch(const NativeProcessError& error){co_return done(error.status);}
        catch(const MemoryFault&){co_return done(0xc0000005U);}
        catch(const SecurityDescriptorError& error){co_return done(detail::process_native_status(error.code));}
        catch(const std::bad_alloc&){co_return done(0xc0000017U);}
        catch(const std::invalid_argument&){co_return done(0xc000000dU);}
        catch(const HostCallFault&){co_return done(0xc000009aU);}
#endif
    });
