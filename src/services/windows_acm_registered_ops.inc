                        std::shared_ptr<acm::DriverInstance> instance;
                        std::shared_ptr<acm::DriverStream> stream;
                        if(const auto open_stream=catalog.registered_streams.find(id_arg);
                           open_stream!=catalog.registered_streams.end()){
                            stream=open_stream->second;
                            instance=stream->instance;
                        }
                        if(const auto open_driver=catalog.registered_instances.find(id_arg);
                           open_driver!=catalog.registered_instances.end())instance=open_driver->second;
                        const auto registration=instance?catalog.registered.end():catalog.registered.find(id_arg);
                        const auto driver=instance?instance->driver:
                            registration!=catalog.registered.end()?registration->second:nullptr;
                        if(!driver)co_return finish(5);
                        if(driver->busy || (instance && instance->busy) || (stream && stream->busy))co_return finish(513);
                        if(symbol=="acmDriverRemove"){
                            if(word(1))co_return finish(10);
                            if(instance)co_return finish(5);
                            if(driver->open_count)co_return finish(513);
                            if(driver->registrations>1){--driver->registrations;co_return finish(0);}
                            driver->busy=true;
                            (void)co_await unload(*driver);
                            std::erase(catalog.order,driver->token);
                            catalog.registered.erase(driver->token);
                            (void)co_await broadcast();
                            co_return finish(0);
                        }
                        if(driver->window)co_return finish(5);
#include "windows_acm_registered_stream.inc"
                        if(symbol=="acmFormatSuggest"){
                            if(!instance || stream)co_return finish(5);
                            if(word(4)&~0xf0000U)co_return finish(10);
                            const auto source=call(1),destination=call(2);
                            if(!source || !destination || word(3)<16)co_return finish(11);
                            const auto tag=m.load(GuestAddress{source},16);
                            const auto source_size=tag==1?16U:18U+static_cast<unsigned>(m.load(GuestAddress{source+16},16));
                            m.check(GuestAddress{source},source_size,Access::Read);
                            m.check(GuestAddress{destination},word(3),Access::Write);
                            const unsigned destination_at=unit==4?16U:24U;
                            const auto size=unit==4?24U:40U;
                            const auto request=buffer(size);
                            m.store(GuestAddress{request},32,size);
                            m.store(GuestAddress{request+4},32,word(4));
                            m.store(GuestAddress{request+8},bits,source);
                            m.store(GuestAddress{request+8+unit},32,source_size);
                            m.store(GuestAddress{request+destination_at},bits,destination);
                            m.store(GuestAddress{request+destination_at+unit},32,word(3));
                            co_return finish(co_await send(*driver,instance->endpoint,acm::format_suggest,request,0));
                        }
                        if(symbol=="acmDriverOpen"){
                            if(!call(0))co_return finish(11);
                            if(word(2))co_return finish(10);
                            if(instance)co_return finish(5);
                            if(!driver->enabled)co_return finish(3);
                            if(catalog.drivers.size()+catalog.registered_instances.size()>=256)co_return finish(4);
                            m.check(GuestAddress{call(0)},unit,Access::Write);
                            m.store(GuestAddress{call(0)},bits,0);
                            const auto handle=runtime_.allocate_wait_handle();
                            auto item=std::make_shared<acm::DriverInstance>(acm::DriverInstance{driver,0,true});
                            catalog.registered_instances.emplace(handle,item);
                            ++driver->open_count;
                            struct UndoOpen {
                                AcmState& state;acm::RegisteredDriver& driver;std::uint64_t handle;bool successful{};
                                ~UndoOpen(){if(!successful){state.registered_instances.erase(handle);--driver.open_count;}}
                            } undo{catalog,*driver,handle};
                            item->endpoint=co_await open(*driver);
                            if(!item->endpoint)co_return finish(6);
                            bool writable=true;
                            try{m.check(GuestAddress{call(0)},unit,Access::Write);}
                            catch(const MemoryFault&){writable=false;}
                            if(!writable){
                                (void)co_await close(*driver,item->endpoint);
                                co_return finish(11);
                            }
                            m.store(GuestAddress{call(0)},bits,handle);
                            item->busy=false;
                            undo.successful=true;
                            co_return finish(0);
                        }
                        if(symbol=="acmDriverClose"){
                            if(word(1))co_return finish(10);
                            if(!instance || stream)co_return finish(5);
                            for(const auto& [stream_handle,item]:catalog.streams){
                                (void)stream_handle;
                                if(item->driver==id_arg)co_return finish(513);
                            }
                            for(const auto& [stream_handle,item]:catalog.registered_streams){
                                (void)stream_handle;
                                if(item->driver_handle==id_arg)co_return finish(513);
                            }
                            instance->busy=true;
                            const auto closed=co_await close(*driver,instance->endpoint);
                            catalog.registered_instances.erase(id_arg);
                            --driver->open_count;
                            co_return finish(closed?0:1);
                        }
                        if(symbol=="acmDriverID"){
                            if(word(2))co_return finish(10);
                            if(!call(1))co_return finish(11);
                            m.store(GuestAddress{call(1)},bits,driver->token);
                            co_return finish(0);
                        }
                        if(symbol=="acmDriverDetailsA" || symbol=="acmDriverDetailsW"){
                            if(word(2))co_return finish(10);
                            co_return finish(co_await copy_details(*driver,call(1),symbol.ends_with('W')));
                        }
                        if(symbol=="acmDriverMessage"){
                            const auto message=word(1);
                            const bool config=message==7 || message==8 || message==acm::driver_about;
                            if(!config && (message<0x4000U || message>=0x6000U))co_return finish(11);
                            if(instance && !stream)co_return finish(co_await send(*driver,instance->endpoint,message,call(2),call(3)));
                            if(!config)co_return finish(5);
                            driver->busy=true;
                            struct Reset {bool& flag;~Reset(){flag=false;}} reset{driver->busy};
                            const auto endpoint=co_await open(*driver);
                            if(!endpoint)co_return finish(6);
                            const auto result=co_await send(*driver,endpoint,message,call(2),call(3));
                            (void)co_await close(*driver,endpoint);
                            co_return finish(result);
                        }
                        if(symbol=="acmDriverPriority"){
                            const auto flags=word(2),priority=word(1);
                            if((flags&~3U) || (flags&3U)==3U)co_return finish(10);
                            if(instance || !driver->global)co_return finish(5);
                            const auto global_begin=std::find_if(catalog.order.begin(),catalog.order.end(),[&](std::uint64_t token){
                                const auto item=catalog.registered.find(token);
                                return item==catalog.registered.end() || item->second->global;
                            });
                            if(priority!=UINT32_MAX && static_cast<std::size_t>(catalog.order.end()-global_begin)<priority)
                                co_return finish(11);
                            const auto locals=global_begin-catalog.order.begin();
                            if(flags&1U)driver->enabled=true;
                            if(flags&2U)driver->enabled=false;
                            if(priority){
                                std::erase(catalog.order,driver->token);
                                catalog.order.insert(priority==UINT32_MAX?catalog.order.end():
                                    catalog.order.begin()+locals+static_cast<std::ptrdiff_t>(priority-1),driver->token);
                            }
                            (void)co_await broadcast();
                            co_return finish(0);
                        }
                        if(symbol=="acmMetrics"){
                            if(!call(2))co_return finish(11);
                            std::uint32_t value=0;
                            switch(word(1)){
                            case 50:case 51:
                                for(const auto& item:word(1)==50?driver->formats:driver->filters)
                                    value=std::max(value,item.size);
                                break;
                            case 100:value=acm::registered_support(*driver);break;
                            case 101:value=static_cast<std::uint32_t>(std::find(catalog.order.begin(),catalog.order.end(),
                                                                              driver->token)-catalog.order.begin())+1;break;
                            case 30:case 31:co_return finish(512);
                            default:co_return finish(5);
                            }
                            m.store(GuestAddress{call(2)},32,value);
                            co_return finish(0);
                        }
