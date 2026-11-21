                        if(add_driver){
                            if(!call(0))co_return finish(11);
                            m.check(GuestAddress{call(0)},unit,Access::Write);
                            m.store(GuestAddress{call(0)},bits,0);
                            const auto flags=word(4);
                            const auto kind=flags&acm::driver_type_mask;
                            if(flags&~0x0fU || (kind!=acm::driver_name && kind!=acm::driver_function &&
                               kind!=acm::driver_notification) || (kind==acm::driver_notification && (flags&acm::driver_global)))
                                co_return finish(10);
                            if(!call(2) || (kind!=acm::driver_notification && word(3)))co_return finish(11);
                            if(catalog.registered.size()>=256)co_return finish(7);
                            auto driver=std::make_shared<acm::RegisteredDriver>();
                            driver->token=runtime_.allocate_wait_handle();
                            driver->global=(flags&acm::driver_global)!=0;
                            if(kind==acm::driver_notification){
                                driver->window=call(2);
                                driver->message=word(3);
                                if(!driver->message || !gui_.windows.contains(driver->window))co_return finish(11);
                            }else if(kind==acm::driver_function){
                                driver->procedure=call(2);
                                m.check(GuestAddress{driver->procedure},1,Access::Execute);
                                const auto existing=std::find_if(catalog.registered.begin(),catalog.registered.end(),
                                    [&](const auto& item){return item.second->procedure==driver->procedure && item.second->global==driver->global;});
                                if(existing!=catalog.registered.end()){
                                    if(existing->second->busy)co_return finish(513);
                                    if(existing->second->registrations==UINT32_MAX)co_return finish(7);
                                    ++existing->second->registrations;
                                    m.store(GuestAddress{call(0)},bits,existing->first);
                                    co_return finish(0);
                                }
                            }else{
                                driver->alias=symbol.ends_with('W')?read_utf16(m,GuestAddress{call(2)},1024):
                                                                   utf8_to_utf16_lossy(read_ansi(m,call(2),1024));
                                if(driver->alias.empty() || driver->alias.size()>=1024)co_return finish(11);
                                const auto machine=x86_?UINT64_C(0x80000002):UINT64_C(0xffffffff80000002);
                                const auto key_slot=buffer(unit),size=buffer(4),type=buffer(4);
                                const auto alias=text(driver->alias);
                                if(co_await api("advapi32.dll","RegOpenKeyExW",
                                    {machine,text(u"Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32"),0,1,key_slot}))
                                    co_return finish(11);
                                const auto key=m.load(GuestAddress{key_slot},bits);
                                const auto check=co_await api("advapi32.dll","RegQueryValueExW",{key,alias,0,type,0,size});
                                (void)co_await api("advapi32.dll","RegCloseKey",{key});
                                const auto value_type=load32(type,0);
                                if(check || load32(size,0)<=2 || (value_type!=1 && value_type!=2))co_return finish(11);
                            }
                            if(!driver->window){
                                driver->busy=true;
                                catalog.registered.emplace(driver->token,driver);
                                std::uint32_t result=0;
                                std::uint64_t instance=0;
                                try{
                                    if(driver->procedure){
                                        if(call(1)){
                                            const auto module=buffer(unit);
                                            if(!co_await api("kernel32.dll","GetModuleHandleExW",{4,driver->procedure,module}))
                                                throw AcmError{11};
                                            driver->module=m.load(GuestAddress{module},bits);
                                            if(driver->module!=call(1))throw AcmError{11};
                                        }
                                        if((co_await send(*driver,0,1,0,0))!=1)throw AcmError{6};
                                        driver->loaded=true;
                                        (void)co_await send(*driver,0,2,0,0);
                                    }
                                    instance=co_await open(*driver);
                                    if(!instance)throw AcmError{6};
                                    const auto details_size=(unit==4?40U:44U)+880U*2U;
                                    const auto details=buffer(details_size);
                                    m.store(GuestAddress{details},32,details_size);
                                    m.store(GuestAddress{details+4},32,0x63647561);
                                    const auto reply=static_cast<std::uint32_t>(co_await send(*driver,instance,acm::driver_details,details,0));
                                    if(reply)throw AcmError{reply};
                                    driver->support=load32(details,24)&0x1fU;
                                    driver->details.resize(details_size);
                                    m.read(GuestAddress{details},driver->details);
                                    std::size_t remaining_standard_bytes=16U*1024*1024;
                                    const auto collect=[&](std::vector<acm::RegisteredDriver::Tag>& tags,unsigned count,unsigned message,
                                                           unsigned standard_message)
                                        ->service_callback::Task<bool>{
                                        constexpr unsigned bytes=24+48*2;
                                        const auto standard_head=20+unit;
                                        const auto standard_bytes=standard_head+128*2;
                                        const auto source=buffer(bytes);
                                        for(unsigned index=0;index<std::min(128U,count);++index){
                                            m.fill(GuestAddress{source},0,bytes);
                                            m.store(GuestAddress{source},32,bytes);
                                            m.store(GuestAddress{source+4},32,index);
                                            if(co_await send(*driver,instance,message,source,0))continue;
                                            const auto tag=load32(source,8),size=load32(source,12),standard_count=load32(source,20);
                                            if(!tag || !size || size>65536 || standard_count>4096)continue;
                                            auto& item=tags.emplace_back(acm::RegisteredDriver::Tag{
                                                read_utf16(m,GuestAddress{source+24},48),tag,size,standard_count,index,{}});
                                            if(!standard_count)continue;
                                            GuestScratch per_tag(*this,m,s);
                                            const auto details_at=per_tag.allocate(standard_bytes);
                                            const auto format_at=per_tag.allocate(size);
                                            if(!details_at || !format_at)throw std::bad_alloc{};
                                            for(unsigned standard=0;standard<standard_count && remaining_standard_bytes>=size;++standard){
                                                m.fill(GuestAddress{details_at},0,standard_bytes);
                                                m.fill(GuestAddress{format_at},0,size);
                                                m.store(GuestAddress{details_at},32,standard_bytes);
                                                m.store(GuestAddress{details_at+4},32,standard);
                                                m.store(GuestAddress{details_at+8},32,tag);
                                                m.store(GuestAddress{details_at+16},bits,format_at);
                                                m.store(GuestAddress{details_at+16+unit},32,size);
                                                if(co_await send(*driver,instance,standard_message,details_at,0))continue;
                                                const auto used=load32(details_at,16+unit);
                                                if(!used || used>size)continue;
                                                acm::RegisteredDriver::Tag::Standard choice{
                                                    std::vector<std::uint8_t>(used),
                                                    read_utf16(m,GuestAddress{details_at+standard_head},128),standard};
                                                m.read(GuestAddress{format_at},choice.bytes);
                                                item.standards.push_back(std::move(choice));
                                                remaining_standard_bytes-=used;
                                            }
                                        }
                                        co_return true;
                                    };
                                    (void)co_await collect(driver->formats,load32(details,28),acm::format_tag_details,acm::format_details);
                                    (void)co_await collect(driver->filters,load32(details,32),acm::filter_tag_details,acm::filter_details);
                                    m.check(GuestAddress{call(0)},unit,Access::Write);
                                }catch(const AcmError& error){result=error.code;}
                                catch(const MemoryFault&){result=11;}
                                catch(const std::bad_alloc&){result=7;}
                                if(instance)(void)co_await close(*driver,instance);
                                if(result){
                                    (void)co_await unload(*driver);
                                    catalog.registered.erase(driver->token);
                                    co_return finish(result);
                                }
                                try{
                                    if(driver->global)catalog.order.push_back(driver->token);
                                    else catalog.order.insert(catalog.order.begin(),driver->token);
                                }catch(const std::bad_alloc&){result=7;}
                                if(result){
                                    (void)co_await unload(*driver);
                                    catalog.registered.erase(driver->token);
                                    co_return finish(result);
                                }
                                driver->busy=false;
                            }else catalog.registered.emplace(driver->token,driver);
                            m.store(GuestAddress{call(0)},bits,driver->token);
                            (void)co_await broadcast();
                            co_return finish(0);
                        }
