        if(command==mci::open && !video_installable && !wave_installable){
            const bool named_type=flags&0x2000U;
            const auto type=named_type?field(p+4,p):0;
            const auto element=flags&mci::open_element && !(flags&mci::open_element_id)?
                string(field(2*p+4,p)):std::u16string{};
            const auto lower=fold(element);
            const bool inferred=lower.ends_with(u".mid") || lower.ends_with(u".midi") ||
                lower.ends_with(u".rmi") || lower.ends_with(u".smf");
            const bool sequencer=named_type?
                (flags&0x1000U?type==mci::sequencer_device:fold(string(type))==u"sequencer"):
                sequencer_installable || inferred;
            if(sequencer){
                check_flags(0x3700U);
                block(4*p+4);
                if(flags&0x100U)throw mci::Error(mci::hardware);
                memory.check(GuestAddress{parameters+p},4,Access::Write);
                auto media=std::make_shared<mci::MidiSequence>();
                media->creator=threads_.current;
                if(flags&0x400U){
                    media->alias=string(field(3*p+4,p));
                    if(media->alias.empty())throw mci::Error(mci::missing_parameter);
                    if(find_device(bridge,media->alias))throw mci::Error(mci::duplicate_alias);
                }
                if(flags&mci::open_element){
                    if(element.empty())throw mci::Error(mci::file_missing);
                    for(const auto& [other,item]:bridge.sequences){
                        (void)other;
                        if(fold(item->file)==lower)throw mci::Error(265);
                    }
                    media->file=element;
                    try{
                        media->sequence=std::make_shared<midi::Sequence>(midi::parse_midi_file(read_file(element,16U*1024U*1024U)));
                    }catch(const std::invalid_argument&){throw mci::Error(mci::invalid_file);}
                    media->offset=media->sequence->smpte_offset;
                }
                const auto created=sequencer_installable?id:bridge.allocate_device();
                bridge.sequences.emplace(created,std::move(media));
                memory.store(GuestAddress{parameters+p},32,created);
                if(flags&mci::notify)notify_mci(created,callback,1);
                return finish(0);
            }
            if(sequencer_installable)throw mci::Error(mci::not_installed);
        }
        if(command!=mci::sysinfo){
            const auto entry=bridge.sequences.find(id);
            if(entry!=bridge.sequences.end()){
                auto& device=*entry->second;
                const auto now=runtime_.clock_ticks;
                const auto succeeded=[&](unsigned result=0){
                    if(flags&mci::notify){
                        notify_mci(id,std::exchange(device.notify_window,0),2);
                        notify_mci(id,callback,1);
                    }
                    return finish(result);
                };
                if(command==mci::close || command==mci::stop){
                    check_flags(0);
                    complete_mci_transport(id,4);
                    succeeded();
                    if(command==mci::close)bridge.sequences.erase(entry);
                    return {};
                }
                if(command==mci::pause || command==mci::resume){
                    check_flags(0);
                    if(command==mci::pause && device.mode==mci::Mode::Playing)device.finish(now,mci::Mode::Paused);
                    if(command==mci::resume && device.mode==mci::Mode::Paused){
                        auto next=device.prepare(device.position,device.end,device.port,runtime_.network.external_io);
                        device.start(std::move(next),device.position,device.end,now);
                        if(device.mode==mci::Mode::Stopped)complete_mci_transport(id,1);
                    }
                    return succeeded();
                }
                if(command==mci::play){
                    check_flags(mci::from|mci::to);
                    if(!device.sequence)throw mci::Error(mci::file_missing);
                    const double first=flags&mci::from?static_cast<double>(device.from_time(static_cast<std::uint32_t>(field(p)))):device.current(now);
                    const auto last=flags&mci::to?device.from_time(static_cast<std::uint32_t>(field(p+4))):device.length();
                    if(first>static_cast<double>(last) || last>device.length())throw mci::Error(mci::out_of_range);
                    const bool continuing=!(flags&mci::from) && device.mode!=mci::Mode::Stopped && device.end==last;
                    if(!continuing || device.mode==mci::Mode::Paused){
                        auto next=device.prepare(first,last,device.port,runtime_.network.external_io);
                        if(!continuing)complete_mci_transport(id,4);
                        device.start(std::move(next),first,last,now);
                    }
                    if(flags&mci::notify){
                        notify_mci(id,std::exchange(device.notify_window,callback),2);
                    }
                    if(device.mode==mci::Mode::Stopped){complete_mci_transport(id,1);return finish(0);}
                    if(flags&mci::wait)return wait_transport(device.waiters);
                    return finish(0);
                }
                if(command==mci::seek){
                    check_flags(mci::to|0x300U);
                    const auto choices=flags&(mci::to|0x300U);
                    if(!choices)throw mci::Error(mci::missing_parameter);
                    if(choices&(choices-1))throw mci::Error(mci::conflicting_flags);
                    const auto target=flags&0x100U?0:flags&0x200U?device.length():
                        device.from_time(static_cast<std::uint32_t>(field(p)));
                    if(target>device.length())throw mci::Error(mci::out_of_range);
                    complete_mci_transport(id,4);
                    device.position=static_cast<double>(target);
                    return succeeded();
                }
                if(command==mci::status || command==mci::getcaps){
                    const bool status=command==mci::status;
                    check_flags(status?0x310U:0x100U);
                    if(!(flags&0x100U))throw mci::Error(mci::missing_parameter);
                    const auto item=static_cast<unsigned>(field(status?2*p:p+4));
                    unsigned value=0,resource=0;
                    bool timecode=false;
                    const auto logical=[&](bool present){value=present;resource=mci::boolean_resource(present);};
                    if(status){
                        if((flags&0x10U) && field(2*p+4)!=1)throw mci::Error(mci::out_of_range);
                        const auto position=device.current(now);
                        switch(item){
                        case 1:case 2:{
                            const auto frame=item==1?device.length():flags&0x200U?0:static_cast<std::uint64_t>(position);
                            value=device.as_time(frame,item==2);
                            timecode=device.format>=mci::seq_smpte_24 && device.format<=mci::seq_smpte_drop;
                            break;
                        }
                        case 3:case 8:value=device.sequence?1:0;break;
                        case 4:value=device.status_mode();resource=mci::resource(value,value);break;
                        case 5:logical(bool(device.sequence));break;
                        case 6:
                            value=device.format;
                            resource=mci::resource(value,value==mci::seq_song_pointer?mci::seq_division_base+9:mci::format_resource_base+value);
                            break;
                        case 7:logical(true);break;
                        case 0x4002:value=device.current_tempo(position);break;
                        case 0x4003:
                            value=device.port;
                            if(value==mci::seq_mapper_port || value==mci::seq_no_port)
                                resource=mci::resource(value,mci::seq_division_base+(value==mci::seq_mapper_port?5:10));
                            break;
                        case 0x4007:value=0x4002;resource=mci::resource(value,mci::seq_division_base+6);break;
                        case 0x4008:value=mci::seq_no_port;resource=mci::resource(value,mci::seq_division_base+10);break;
                        case 0x4009:value=device.offset;timecode=true;break;
                        case 0x400a:
                            value=mci::sequencer_division(device.sequence?device.sequence->division:0);
                            resource=mci::resource(value,value);break;
                        default:throw mci::Error(mci::unsupported);
                        }
                    }else{
                        switch(item){
                        case 1:case 3:case 7:case 9:logical(false);break;
                        case 2:case 5:case 6:case 8:logical(true);break;
                        case 4:value=mci::sequencer_device;resource=mci::resource(value,value);break;
                        default:throw mci::Error(mci::unsupported);
                        }
                    }
                    const auto width=status?bits:32;
                    memory.store(GuestAddress{parameters+p},static_cast<std::uint8_t>(width),
                        sequencer_installable && resource?resource:value);
                    return succeeded(sequencer_installable?(resource?mci::resource_returned:timecode?0x40000U:0):0);
                }
                if(command==mci::info){
                    check_flags(0x3300U);
                    const auto kind=flags&0x3300U;
                    std::u16string text;
                    if(kind==0x100)text=u"MIDI Sequencer";
                    else if(kind==0x200)text=device.file;
                    else if(kind==0x1000 || kind==0x2000){
                        if(device.sequence)text=utf8_to_utf16_lossy(kind==0x1000?device.sequence->title:device.sequence->copyright);
                    }else throw mci::Error(mci::missing_parameter);
                    write_string(field(p,p),static_cast<std::uint32_t>(field(2*p)),text);
                    return succeeded();
                }
                if(command==mci::set){
                    check_flags(0x6c00U|mci::seq_tempo|mci::seq_port|mci::seq_slave|mci::seq_master|mci::seq_offset);
                    block(p);
                    if(flags&(mci::seq_slave|mci::seq_master))throw mci::Error(mci::unsupported);
                    const auto format=flags&0x400U?static_cast<unsigned>(field(p)):device.format;
                    if(format && format!=mci::seq_song_pointer && (format<mci::seq_smpte_24 || format>mci::seq_smpte_drop))
                        throw mci::Error(mci::bad_time);
                    if(format==mci::seq_song_pointer && device.has_smpte_division())throw mci::Error(mci::seq_division_incompatible);
                    auto offset=device.offset;
                    if(flags&mci::seq_offset){
                        if(!device.has_smpte_division())throw mci::Error(mci::seq_division_incompatible);
                        offset=static_cast<unsigned>(field(p+24));
                        if((offset&255U)>23)throw mci::Error(mci::out_of_range);
                        (void)mci::MidiSequence::from_smpte(device.offset_format(),offset);
                    }
                    const auto tempo=flags&mci::seq_tempo?static_cast<unsigned>(field(p+8)):device.tempo;
                    if((flags&mci::seq_tempo) && !tempo)throw mci::Error(mci::out_of_range);
                    auto port=flags&mci::seq_port?static_cast<unsigned>(field(p+12)):device.port;
                    if(port==UINT32_MAX)port=mci::seq_mapper_port;
                    if(port && port!=mci::seq_mapper_port && port!=mci::seq_no_port)throw mci::Error(mci::seq_port_missing);
                    bool left=device.left,right=device.right;
                    if(flags&0x800U){
                        const auto channel=field(p+4);
                        const auto toggle=flags&0x6000U;
                        if(channel>2 || (toggle!=0x2000U && toggle!=0x4000U))throw mci::Error(mci::conflicting_flags);
                        if(channel!=2)left=toggle==0x2000U;
                        if(channel!=1)right=toggle==0x2000U;
                    }else if(flags&0x6000U)throw mci::Error(mci::conflicting_flags);

                    if(device.mode==mci::Mode::Playing && (port==mci::seq_no_port)!=(device.port==mci::seq_no_port)){
                        const auto first=device.current(now);
                        auto next=device.prepare(first,device.end,port,runtime_.network.external_io);
                        device.finish(now);
                        device.start(std::move(next),first,device.end,now);
                        if(device.mode==mci::Mode::Stopped)complete_mci_transport(id,1);
                    }else if(device.mode==mci::Mode::Playing && !device.playback && tempo!=device.tempo){
                        device.position=device.current(now);
                        device.anchor=now;
                    }
                    device.format=format;
                    device.offset=offset;
                    device.tempo=tempo;
                    device.port=port;
                    device.left=left;
                    device.right=right;
                    device.update_audio();
                    return succeeded();
                }
                throw mci::Error(mci::unsupported);
            }
        }
