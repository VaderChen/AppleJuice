        const bool add_driver=name=="acmDriverAddA" || name=="acmDriverAddW";
        const auto id_arg=(name=="acmDriverOpen" || name=="acmStreamOpen")?arg(1):arg(0);
        const bool dispatch_driver=name.starts_with("acmDriver") || name.starts_with("acmStream") ||
                                   name=="acmMetrics" || name=="acmFormatSuggest";
        const bool owned_driver=acm_ && dispatch_driver && (acm_->registered.contains(id_arg) ||
            acm_->registered_instances.contains(id_arg) || acm_->registered_streams.contains(id_arg));
        // 關閉與移除會先從物件表釋放；客體回呼結束後仍須取回原呼叫的結果。
        const bool eligible_for_auto=service.family=="msacm32.dll" && !id_arg && (name=="acmStreamOpen" || name=="acmFormatSuggest");
        const bool automatic_driver=eligible_for_auto && acm_ && std::any_of(acm_->registered.begin(),acm_->registered.end(),
            [](const auto& item){const auto& driver=*item.second;return !driver.window && !driver.busy && driver.enabled;});
        const bool completed_driver=!owned_driver && (eligible_for_auto || name=="acmDriverRemove" ||
            name=="acmDriverClose" || name=="acmStreamClose") &&
            service_callback::completed(this,threads_.current,"ACM 驅動程式",state);
        if(add_driver || owned_driver || automatic_driver || completed_driver){
            if(!acm_)acm_=std::make_shared<AcmState>();
            return nested_continuation(state,memory,"ACM 驅動程式",
                [this,service,add_driver,id_arg,automatic_driver](GuestState& s,GuestMemory& m)
                    ->service_callback::Task<std::optional<std::uint32_t>>{
                    const WindowsCall call(s,m,service);
                    const auto symbol=service.symbol;
                    auto& catalog=*acm_;
                    const unsigned unit=x86_?4U:8U;
                    const auto bits=static_cast<std::uint8_t>(unit*8);
                    const auto word=[&](unsigned index){return static_cast<std::uint32_t>(call(index));};
                    const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
                    GuestScratch scratch(*this,m,s);
                    const auto api=[&](std::string_view module,std::string_view function,std::vector<std::uint64_t> args)
                        ->service_callback::Task<std::uint64_t>{
                        co_return co_await service_callback::call_value(resolve(module,function).value,std::move(args));
                    };
                    const auto buffer=[&](std::size_t size){
                        const auto pointer=scratch.allocate(size);
                        if(!pointer)throw std::bad_alloc{};
                        m.fill(GuestAddress{pointer},0,size);
                        return pointer;
                    };
                    const auto text=[&](std::u16string_view value){
                        const auto pointer=scratch.text(value);
                        if(!pointer)throw std::bad_alloc{};
                        return pointer;
                    };
                    const auto load32=[&](std::uint64_t pointer,unsigned offset){
                        return static_cast<std::uint32_t>(m.load(GuestAddress{pointer+offset},32));
                    };
                    const auto send=[&](const acm::RegisteredDriver& driver,std::uint64_t instance,
                                        unsigned message,std::uint64_t first,std::uint64_t second)
                        ->service_callback::Task<std::uint64_t>{
                        if(driver.procedure)
                            co_return co_await service_callback::call_value(driver.procedure,
                                {instance,driver.token,message,first,second});
                        co_return co_await api("winmm.dll","SendDriverMessage",{instance,message,first,second});
                    };
                    const auto open=[&](const acm::RegisteredDriver& driver)->service_callback::Task<std::uint64_t>{
                        const unsigned size=unit==4?36U:48U;
                        const auto descriptor=buffer(size);
                        m.store(GuestAddress{descriptor},32,size);
                        m.store(GuestAddress{descriptor+4},32,0x63647561);
                        m.store(GuestAddress{descriptor+12},32,0x04000000);
                        if(driver.procedure)co_return co_await send(driver,0,3,0,descriptor);
                        const auto alias=text(driver.alias);
                        m.store(GuestAddress{descriptor+24},bits,text(u"Drivers32"));
                        m.store(GuestAddress{descriptor+24+unit},bits,alias);
                        co_return co_await api("winmm.dll","OpenDriver",{alias,0,descriptor});
                    };
                    const auto close=[&](const acm::RegisteredDriver& driver,std::uint64_t instance)
                        ->service_callback::Task<std::uint64_t>{
                        if(driver.procedure)co_return co_await send(driver,instance,4,0,0);
                        co_return co_await api("winmm.dll","CloseDriver",{instance,0,0});
                    };
                    const auto unload=[&](acm::RegisteredDriver& driver)->service_callback::Task<bool>{
                        const auto loaded=std::exchange(driver.loaded,false);
                        const auto module=std::exchange(driver.module,0);
                        if(loaded){
                            (void)co_await send(driver,0,5,0,0);
                            (void)co_await send(driver,0,6,0,0);
                        }
                        if(module)(void)co_await api("kernel32.dll","FreeLibrary",{module});
                        co_return loaded || module!=0;
                    };
                    const auto broadcast=[&]()->service_callback::Task<bool>{
                        std::vector<std::pair<std::uint64_t,unsigned>> subscribers;
                        for(const auto& [token,driver]:catalog.registered){
                            (void)token;
                            if(driver->window && gui_.windows.contains(driver->window))
                                subscribers.emplace_back(driver->window,driver->message);
                        }
                        for(const auto& [window,message]:subscribers)
                            (void)co_await api("user32.dll","PostMessageW",{window,message,0,0});
                        co_return true;
                    };
                    const auto copy_details=[&](const acm::RegisteredDriver& driver,std::uint64_t target,bool wide)
                        ->service_callback::Task<std::uint32_t>{
                        if(!target || load32(target,0)<4)co_return 11;
                        const unsigned header=unit==4?40U:44U;
                        const auto capacity=load32(target,0);
                        const auto amount=std::min<unsigned>(capacity,header+880*(wide?2U:1U));
                        std::vector<std::uint8_t> details(header+880*(wide?2U:1U));
                        std::copy_n(driver.details.begin(),header,details.begin());
                        if(wide)std::copy(driver.details.begin()+header,driver.details.end(),details.begin()+header);
                        else{
                            constexpr unsigned lengths[]{32,128,80,128,512};
                            unsigned at=0;
                            for(const auto length:lengths){
                                const auto source=buffer(length*2),destination=buffer(length);
                                auto input=std::span(driver.details).subspan(header+at*2,length*2);
                                m.write(GuestAddress{source},input);
                                m.store(GuestAddress{source+(length-1)*2},16,0);
                                const auto produced=co_await api("kernel32.dll","WideCharToMultiByte",
                                    {0,0,source,UINT32_MAX,destination,length,0,0});
                                if(produced && produced<=length)m.read(GuestAddress{destination},
                                    std::span(details).subspan(header+at,static_cast<std::size_t>(produced)));
                                at+=length;
                            }
                        }
                        const auto store=[&](unsigned offset,unsigned value){
                            for(unsigned i=0;i<4;++i)details[offset+i]=static_cast<std::uint8_t>(value>>(8*i));
                        };
                        store(0,amount);
                        store(24,acm::registered_support(driver));
                        m.write(GuestAddress{target},std::span(details).first(amount));
                        co_return 0;
                    };
                    try{
#include "windows_acm_registered_add.inc"
#include "windows_acm_registered_auto.inc"
#include "windows_acm_registered_ops.inc"
                    }catch(const AcmError& error){co_return finish(error.code);}
                    catch(const MemoryFault&){co_return finish(11);}
                    catch(const std::bad_alloc&){co_return finish(7);}
                    co_return finish(8);
                });
        }
