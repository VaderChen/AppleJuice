        if(command==mci::open && !video_installable && !wave_installable && !sequencer_installable){
            const auto typed=flags&0x2000U;
            const auto raw_type=typed?field(p+4,p):0;
            const auto requested=flags&mci::open_element && !(flags&mci::open_element_id)?
                string(field(2*p+4,p)):std::u16string{};
            const auto lower=fold(requested);
            const bool cd=cd_installable || (typed?(flags&0x1000U?raw_type==mci::cd_device:fold(string(raw_type))==u"cdaudio"):
                lower.ends_with(u".cue") || lower.ends_with(u".cda"));
            if(cd){
                check_flags(0x3700U);
                block(4*p+4);
                memory.check(GuestAddress{parameters+p},4,Access::Write);
                const auto open_cue=[&](const std::string& filename){
                    try{return cdda::Disc::open(filesystem_,filename);}
                    catch(const std::invalid_argument&){throw mci::Error(mci::invalid_file);}
                    catch(const std::runtime_error&){throw mci::Error(mci::file_read);}
                };
                std::string cue;
                std::u16string drive;
                const auto find_cue=[&](char letter){
                    const std::string root{letter,':','\\'};
                    auto found=cdda::volume_cue(filesystem_,root);
                    if(found.ambiguous)throw mci::Error(mci::bad_constant);
                    return std::move(found.path);
                };
                if(lower.ends_with(u".cue")){
                    try{cue=utf16_to_utf8(normalized_windows_path(requested,runtime_.current_directory));}
                    catch(const std::invalid_argument&){throw mci::Error(mci::file_missing);}
                }else if(!requested.empty()){
                    if(requested.size()<2 || requested[1]!=u':' || lower[0]<u'a' || lower[0]>u'z')
                        throw mci::Error(mci::bad_constant);
                    const char letter=static_cast<char>('A'+lower[0]-u'a');
                    cue=find_cue(letter);
                    drive={static_cast<char16_t>(letter),u':'};
                }else{
                    for(const auto& [letter,volume]:filesystem_.volume_state().drives){
                        (void)volume;
                        if(letter<'D')continue;
                        cue=find_cue(letter);
                        if(!cue.empty()){drive={static_cast<char16_t>(letter),u':'};break;}
                    }
                }
                if(cue.empty())throw mci::Error(mci::not_installed);
                const auto key=fold(utf8_to_utf16_lossy(cue));
                auto device=std::make_shared<mci::CdaDevice>();
                device->creator=threads_.current;
                device->shareable=flags&0x100U;
                device->file=!requested.empty()?requested:drive;
                if(flags&0x400U){
                    device->alias=string(field(3*p+4,p));
                    if(device->alias.empty())throw mci::Error(mci::missing_parameter);
                    if(find_device(bridge,device->alias))throw mci::Error(mci::duplicate_alias);
                }
                for(const auto& [other,item]:bridge.compact_discs){
                    (void)other;
                    if(fold(utf8_to_utf16_lossy(item->tray->cue))!=key)continue;
                    if(!device->shareable || !item->shareable)throw mci::Error(265);
                    device->tray=item->tray;
                    break;
                }
                if(!device->tray){
                    auto disc=open_cue(cue);
                    if(!disc)throw mci::Error(mci::file_missing);
                    device->tray=std::make_shared<mci::CdTray>(mci::CdTray{std::move(cue),std::move(disc)});
                }
                if(device->media_present())
                    device->position=std::uint64_t{device->disc().tracks().front().sector}*cdda::samples_per_sector;
                const auto created=cd_installable?id:bridge.allocate_device();
                bridge.compact_discs.emplace(created,std::move(device));
                memory.store(GuestAddress{parameters+p},32,created);
                if(flags&mci::notify)notify_mci(created,callback,1);
                return finish(0);
            }
        }
        if(command!=mci::sysinfo){
            const auto entry=bridge.compact_discs.find(id);
            if(entry!=bridge.compact_discs.end()){
                auto& device=*entry->second;
                const auto succeeded=[&](unsigned code=0){
                    if(flags&mci::notify){
                        notify_mci(id,std::exchange(device.notify_window,0),2);
                        notify_mci(id,callback,1);
                    }
                    return finish(code);
                };
                if(command==mci::close || command==mci::stop){
                    check_flags(0);
                    complete_mci_transport(id,4);
                    const auto result=succeeded();
                    if(command==mci::close)bridge.compact_discs.erase(entry);
                    return result;
                }
                if(command==mci::pause || command==mci::resume){
                    check_flags(0);
                    if(command==mci::pause)device.pause();else device.resume();
                    return succeeded();
                }
                if(command==mci::play){
                    check_flags(mci::from|mci::to);
                    const auto& disc=device.disc();
                    const auto first=flags&mci::from?std::uint64_t{device.next_audio(device.from_time(static_cast<std::uint32_t>(field(p))))}*cdda::samples_per_sector:
                        std::max(device.current(),std::uint64_t{device.next_audio(device.current_sector())}*cdda::samples_per_sector);
                    const auto last=flags&mci::to?device.next_audio(device.from_time(static_cast<std::uint32_t>(field(p+4)))):disc.sectors();
                    const auto stop=std::uint64_t{last}*cdda::samples_per_sector;
                    if(first>stop || first>std::uint64_t{disc.sectors()}*cdda::samples_per_sector)
                        throw mci::Error(mci::out_of_range);
                    auto next=device.prepare(first,stop,runtime_.network.external_io);
                    complete_mci_transport(id,4);
                    device.start(std::move(next));
                    device.notify_window=callback;
                    if(device.mode==mci::Mode::Stopped){complete_mci_transport(id,1);return finish(0);}
                    if(flags&mci::wait)return wait_transport(device.waiters);
                    return finish(0);
                }
                if(command==mci::seek){
                    check_flags(0x300U|mci::to);
                    const auto selector=flags&(0x300U|mci::to);
                    if(!selector)throw mci::Error(mci::missing_parameter);
                    if(selector&(selector-1))throw mci::Error(mci::conflicting_flags);
                    const auto& disc=device.disc();
                    const auto sector=selector==0x100U?disc.tracks().front().sector:selector==0x200U?disc.sectors()-1:
                        device.from_time(static_cast<std::uint32_t>(field(p)));
                    const auto seek=std::min(disc.sectors()-1,device.next_audio(sector));
                    complete_mci_transport(id,4);
                    device.position=std::uint64_t{seek}*cdda::samples_per_sector;
                    return succeeded();
                }
                if(command==mci::getcaps || command==mci::status){
                    const bool status=command==mci::status;
                    check_flags(status?0x310U:0x100U);
                    if(!(flags&0x100U))throw mci::Error(mci::missing_parameter);
                    const auto item=static_cast<unsigned>(field(status?2*p:p+4));
                    unsigned value=0,resource=0,colonized=0;
                    const auto logical=[&](bool result){value=result;resource=mci::boolean_resource(result);};
                    if(!status){
                        switch(item){
                        case 1:case 3:case 5:case 6:case 9:logical(false);break;
                        case 2:case 7:case 8:logical(true);break;
                        case 4:value=mci::cd_device;resource=mci::resource(value,value);break;
                        default:throw mci::Error(mci::unsupported);
                        }
                    }else{
                        switch(item){
                        case 1:{
                            const auto& disc=device.disc();
                            auto frames=disc.sectors()-disc.tracks().front().sector-1;
                            if(flags&0x10U){
                                const auto& track=device.track(static_cast<unsigned>(field(2*p+4)));
                                frames=device.next_track(track.number)-track.sector-(track.number==disc.tracks().back().number?1:0);
                            }
                            value=device.to_time(frames,true);
                            if(device.format)colonized=0x20000U;
                            break;
                        }
                        case 2:{
                            if((flags&0x210U)==0x210U)throw mci::Error(mci::conflicting_flags);
                            const auto& disc=device.disc();
                            const auto sector=flags&0x10U?device.track(static_cast<unsigned>(field(2*p+4))).sector:
                                flags&0x200U?disc.tracks().front().sector:device.current_sector();
                            value=device.to_time(sector);
                            if(device.format)colonized=device.format==mci::cd_tmsf?0x40000U:0x20000U;
                            break;
                        }
                        case 3:value=static_cast<unsigned>(device.disc().tracks().size());break;
                        case 4:value=device.status_mode();resource=mci::resource(value,value);break;
                        case 5:case 7:logical(device.media_present());break;
                        case 6:value=device.format;resource=mci::resource(value,mci::format_resource_base+value);break;
                        case 8:value=device.disc().track_at(device.current_sector()).number;break;
                        case mci::cd_track_type:{
                            if(!(flags&0x10U))throw mci::Error(mci::missing_parameter);
                            value=device.track(static_cast<unsigned>(field(2*p+4))).audio?mci::cd_track_audio:mci::cd_track_data;
                            resource=mci::resource(value,value);
                            break;
                        }
                        default:throw mci::Error(mci::unsupported);
                        }
                    }
                    memory.store(GuestAddress{parameters+p},static_cast<std::uint8_t>(status?bits:32),cd_installable && resource?resource:value);
                    return succeeded(cd_installable?(resource?mci::resource_returned:colonized):0);
                }
                if(command==mci::info){
                    check_flags(0xd00U);
                    const auto item=flags&0xd00U;
                    std::u16string text;
                    if(item==0x100U)text=u"CD audio";
                    else if(item==0x400U){
                        const auto& catalog=device.disc().catalog();
                        if(catalog.empty())throw mci::Error(mci::cd_no_identity);
                        text=utf8_to_utf16_lossy(catalog);
                    }else if(item==0x800U){
                        char identity[17];
                        std::snprintf(identity,sizeof identity,"%016llX",static_cast<unsigned long long>(device.disc().identity()));
                        text=utf8_to_utf16_lossy(identity);
                    }else throw mci::Error(mci::missing_parameter);
                    write_string(field(p,p),static_cast<std::uint32_t>(field(2*p)),text);
                    return succeeded();
                }
                if(command==mci::set){
                    check_flags(0x6f00U);
                    const auto format=flags&0x400U?static_cast<unsigned>(field(p)):device.format;
                    if(format && format!=mci::cd_msf && format!=mci::cd_tmsf)throw mci::Error(mci::bad_time);
                    if((flags&0x300U)==0x300U)throw mci::Error(mci::conflicting_flags);
                    bool left=device.left,right=device.right;
                    if(flags&0x800U){
                        const auto channel=field(p+4),toggle=flags&0x6000U;
                        if(channel>2 || (toggle!=0x2000U && toggle!=0x4000U))throw mci::Error(mci::conflicting_flags);
                        if(channel!=2)left=toggle==0x2000U;
                        if(channel!=1)right=toggle==0x2000U;
                    }else if(flags&0x6000U)throw mci::Error(mci::conflicting_flags);
                    if(flags&0x100U){
                        for(const auto& [other,disc]:bridge.compact_discs)
                            if(disc->tray==device.tray)complete_mci_transport(other,4);
                        device.tray->open=true;
                        device.tray->disc.reset();
                    }else if((flags&0x200U) && device.tray->open){
                        try{device.tray->disc=cdda::Disc::open(filesystem_,device.tray->cue);}
                        catch(const std::invalid_argument&){throw mci::Error(mci::invalid_file);}
                        catch(const std::runtime_error&){throw mci::Error(mci::file_read);}
                        device.tray->open=false;
                        for(const auto& [other,disc]:bridge.compact_discs){
                            (void)other;
                            if(disc->tray==device.tray)disc->position=disc->media_present()?
                                std::uint64_t{disc->disc().tracks().front().sector}*cdda::samples_per_sector:0;
                        }
                    }
                    device.format=format;
                    device.left=left;
                    device.right=right;
                    device.update_volume();
                    return succeeded();
                }
                throw mci::Error(mci::unsupported);
            }
        }
