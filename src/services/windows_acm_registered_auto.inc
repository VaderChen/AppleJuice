                        if(automatic_driver){
                            const bool converting=symbol=="acmStreamOpen";
                            const bool query=converting && (word(7)&1U);
                            if(converting && !query && !call(0))co_return finish(11);
                            if(converting && call(0))m.store(GuestAddress{call(0)},bits,0);
                            if(converting?word(7)&~7U:word(4)&~0xf0000U)co_return finish(10);
                            const auto source=call(converting?2:1),destination=call(converting?3:2);
                            if(!source || !destination || (!converting && word(3)<16))co_return finish(11);
                            const auto input=m.load(GuestAddress{source},16);
                            const auto output=(converting || (word(4)&0x10000U))?
                                m.load(GuestAddress{destination},16):0;
                            const auto built_in=acm::builtin_driver_base+
                                builtin_codec_for(static_cast<unsigned>(input),static_cast<unsigned>(output));
                            const auto handle_slot=buffer(unit);
                            const auto order=catalog.order;
                            std::vector<std::uint64_t> parameters;
                            for(unsigned i=0;i<(converting?8U:5U);++i)parameters.push_back(call(i));
                            const auto filter=converting?call(4):0;
                            const auto filter_tag=filter?load32(filter,4):0;
                            std::vector<std::uint8_t> previous_destination;
                            if(!converting){
                                const auto capacity=word(3);
                                m.check(GuestAddress{destination},capacity,Access::Write);
                                previous_destination.resize(std::min(capacity,65553U));
                                m.read(GuestAddress{destination},previous_destination);
                            }
                            std::uint64_t result=512;
                            for(const auto token:order){
                                if(token>=acm::builtin_driver_base && token<acm::builtin_driver_base+catalog.enabled.size()){
                                    if(token!=built_in || (filter && filter_tag!=acm::volume_tag) ||
                                       !builtin_conversion_source(static_cast<unsigned>(input)))continue;
                                    result=co_await api("internal-acm-builtin",symbol,parameters);
                                    if(!result)co_return finish(0);
                                }else{
                                    const auto entry=catalog.registered.find(token);
                                    if(entry==catalog.registered.end() || !entry->second->enabled || entry->second->busy)continue;
                                    const auto& format_tags=filter?entry->second->filters:entry->second->formats;
                                    const auto wanted=filter?filter_tag:static_cast<std::uint32_t>(input);
                                    if(!format_tags.empty() && std::none_of(format_tags.begin(),format_tags.end(),
                                       [&](const auto& tag){return tag.tag==wanted;}))continue;
                                    if(co_await api("msacm32.dll","acmDriverOpen",{handle_slot,token,0}))continue;
                                    const auto opened=m.load(GuestAddress{handle_slot},bits);
                                    auto request=parameters;
                                    request[converting?1:0]=opened;
                                    result=co_await api("msacm32.dll",symbol,std::move(request));
                                    if(!result && converting && !query){
                                        const auto stream_handle=m.load(GuestAddress{call(0)},bits);
                                        catalog.registered_streams.at(stream_handle)->owns_driver=true;
                                        co_return finish(0);
                                    }
                                    (void)co_await api("msacm32.dll","acmDriverClose",{opened,0});
                                    if(!result)co_return finish(0);
                                }
                                if(!converting)m.write(GuestAddress{destination},previous_destination);
                            }
                            co_return finish(result);
                        }
