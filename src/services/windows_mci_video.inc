        const auto open_video=[&](std::u16string requested,bool quartz){
            std::u16string path;
            try{path=normalized_windows_path(std::move(requested),runtime_.current_directory);}
            catch(const std::invalid_argument&){throw mci::Error(mci::file_missing);}
            const auto utf8=utf16_to_utf8(path);
            const auto file=filesystem_.information(utf8);
            if(file.error)throw mci::Error(file_error(file.error));
            std::unique_ptr<VideoPreview> clip;
            try{
                if(quartz){
                    const auto source=filesystem_.open_reader(utf8);
                    if(source.error)throw mci::Error(file_error(source.error));
                    clip=Mp4Preview::open(source);
                    if(!clip)clip=MpegPreview::open(source);
                }
                if(!clip)clip=AviPreviewClip::open(filesystem_,utf8,true);
            }
            catch(const mci::Error&){throw;}
            catch(const std::length_error&){throw mci::Error(mci::memory);}
            catch(const std::bad_alloc&){throw;}
            catch(const std::exception& error){
                if(quartz && std::getenv("X64ARM_TRACE_MCI_VIDEO"))std::fprintf(stderr,"[mci-video] open: %s\n",error.what());
                throw mci::Error(mci::invalid_file);
            }
            if(!clip)throw mci::Error(mci::invalid_file);
            auto media=std::make_shared<mci::AviDevice>(std::move(clip));
            media->quartz=quartz;
            if(quartz)media->format=0;
            media->file=std::move(path);
            return media;
        };
        if(!wave_installable && !sequencer_installable && command==mci::open && (flags&mci::open_element) && !(flags&mci::open_element_id)){
            block(4*p+4);
            const auto requested=string(field(2*p+4,p));
            const auto type=flags&0x2000U?field(p+4,p):0;
            const auto type_name=(flags&0x2000U) && !(flags&0x1000U)?fold(string(type)):std::u16string{};
            const auto lower=fold(requested);
            constexpr std::u16string_view quartz_extensions[]{u".mp4",u".m4v",u".mov",u".m4a",u".mpg",u".mpeg",u".m1v",u".m2v",u".vob",u".mp3"};
            const bool quartz_extension=std::any_of(std::begin(quartz_extensions),std::end(quartz_extensions),
                [&](std::u16string_view extension){return lower.ends_with(extension);});
            const bool quartz=quartz_installable || (!avi_installable &&
                (type_name==u"mpegvideo" || (quartz_extension && type_name!=u"avivideo")));
            const bool video=video_installable || (flags&0x2000U?
                (flags&0x1000U?type==mci::avi_device:(type_name==u"avivideo" || type_name==u"digitalvideo" || type_name==u"mpegvideo")):
                quartz_extension || lower.ends_with(u".avi"));
            if(video){
                check_flags(0x3700|mci::video_open_style|mci::video_open_parent|mci::video_open_nostatic);
                if(flags&mci::video_open_style)throw mci::Error(mci::unsupported);
                auto media=open_video(requested,quartz);
                media->creator=threads_.current;
                media->shareable=(flags&0x100U)!=0;
                if(flags&mci::video_open_parent){
                    const auto parent=field(4*p+8,p);
                    if(!parent || !gui_.windows.contains(parent))throw mci::Error(mci::bad_constant);
                    media->window=parent;
                }
                if(flags&0x400U){
                    media->alias=string(field(3*p+4,p));
                    if(media->alias.empty())throw mci::Error(mci::missing_parameter);
                    if(find_device(bridge,media->alias))throw mci::Error(mci::duplicate_alias);
                }
                for(const auto& [other,item]:bridge.videos){
                    (void)other;
                    if(fold(item->file)==fold(media->file) && (!item->shareable || !media->shareable))throw mci::Error(265);
                }
                memory.check(GuestAddress{parameters+p},4,Access::Write);
                const auto created=video_installable?id:bridge.allocate_device();
                if(video_installable || quartz)media->presentation=bridge.next_presentation++;
                bridge.videos.emplace(created,std::move(media));
                memory.store(GuestAddress{parameters+p},32,created);
                if(flags&mci::notify)notify_mci(created,callback,1);
                return finish(0);
            }
        }
        if(command!=mci::sysinfo){
            const auto entry=bridge.videos.find(id);
            if(entry!=bridge.videos.end()){
                auto& video=*entry->second;
                const auto now=runtime_.clock_ticks;
                const auto succeeded=[&]{
                    if(flags&mci::notify)notify_mci(id,callback,1);
                    return finish(0);
                };
                if(command==mci::close || command==mci::stop){
                    check_flags(0);
                    complete_mci_transport(id,4);
                    if(command==mci::close)bridge.videos.erase(entry);
                    return succeeded();
                }
                if(command==mci::play){
                    check_flags(mci::from|mci::to|mci::video_reverse|mci::video_repeat);
                    const auto backwards=bool(flags&mci::video_reverse);
                    const auto first=flags&mci::from?video.from_time(static_cast<std::uint32_t>(field(p))):
                        backwards?0:video.current(now);
                    const auto last=flags&mci::to?video.from_time(static_cast<std::uint32_t>(field(p+4))):
                        backwards?std::min(video.length(),video.current(now)+1):video.length();
                    if(first>last || last>video.length())throw mci::Error(mci::out_of_range);
                    if(video.audible() && !output_available())throw mci::Error(mci::hardware);
                    complete_mci_transport(id,4);
                    video.play(first,last,backwards,flags&mci::video_repeat,now);
                    if(video.mode==mci::Mode::Stopped)return succeeded();
                    video.notify_window=callback;
                    if(flags&mci::wait)return wait_transport(video.waiters);
                    return finish(0);
                }
                if(command==mci::pause || command==mci::resume){
                    check_flags(0);
                    if(command==mci::pause && video.mode==mci::Mode::Playing)video.pause(now);
                    else if(command==mci::resume && video.mode==mci::Mode::Paused)video.resume(now);
                    else throw mci::Error(mci::not_ready);
                    return succeeded();
                }
                if(command==mci::seek || command==mci::video_step){
                    const auto stepping=command==mci::video_step;
                    check_flags(stepping?0x30000U:mci::to|0x300U);
                    std::uint64_t position=0;
                    if(stepping){
                        const auto count=(flags&0x20000U)?field(p):1;
                        const auto current=video.current(now);
                        position=(flags&0x10000U)?current-std::min(current,count):std::min(video.length(),current+count);
                    }else{
                        const auto choices=flags&(mci::to|0x300U);
                        if(!choices || (choices&(choices-1)))throw mci::Error(mci::conflicting_flags);
                        position=flags&0x100U?0:flags&0x200U?video.length():
                            video.from_time(static_cast<std::uint32_t>(field(p)));
                    }
                    if(position>video.length())throw mci::Error(mci::out_of_range);
                    complete_mci_transport(id,4);
                    video.position=position;
                    return succeeded();
                }
                if(command==mci::status || command==mci::getcaps){
                    const bool status=command==mci::status;
                    check_flags((status?0xc00310U:0x100U)|mci::test_flag);
                    if(!(flags&0x100U))throw mci::Error(mci::missing_parameter);
                    const auto item=field(status?2*p:p+4);
                    if(status && (flags&0x10U) && field(2*p+4)!=1)throw mci::Error(mci::out_of_range);
                    if(flags&mci::test_flag)return finish(0);
                    std::uint64_t result=0;
                    if(status){
                        switch(item){
                        case 1: result=video.as_time(video.length());break;
                        case 2: result=video.as_time(flags&0x200U?0:video.current(now));break;
                        case 3:case 5:case 7:case 8:result=1;break;
                        case 4: result=video.status_mode();break;
                        case 6: result=video.format;break;
                        case 0x4001: result=video.window;break;
                        case 0x4003: result=video.speed;break;
                        case 0x4014: result=video.audible();break;
                        case 0x4019: result=(video.left+video.right)/2;break;
                        default:throw mci::Error(mci::unsupported);
                        }
                    }else{
                        switch(item){
                        case 1:case 7:case 9:break;
                        case 2: result=video.audible();break;
                        case 3: result=video.clip->visual();break;
                        case 5:case 6:case 8: result=1;break;
                        case 4: result=mci::avi_device;break;
                        default:throw mci::Error(mci::unsupported);
                        }
                    }
                    bool resource=false;
                    if(installable){
                        const auto value=static_cast<unsigned>(result);
                        if(!status || item==5 || item==7){
                            result=!status && item==4?mci::resource(value,value):mci::boolean_resource(value!=0);
                            resource=true;
                        }else if(item==4 || item==6){
                            result=mci::resource(value,item==6?value+533:value);
                            resource=true;
                        }
                    }
                    memory.store(GuestAddress{parameters+p},static_cast<std::uint8_t>(status?bits:32),result);
                    if(resource){
                        if(flags&mci::notify)notify_mci(id,callback,1);
                        return finish(mci::resource_returned);
                    }
                    return succeeded();
                }
                if(command==mci::info){
                    check_flags(0x300U);
                    const auto item=flags&0x300U;
                    if(item!=0x100U && item!=0x200U)throw mci::Error(mci::conflicting_flags);
                    write_string(field(p,p),static_cast<std::uint32_t>(field(2*p)),
                                 item==0x100U?std::u16string_view(video.quartz?u"MPEG digital video":u"AVI digital video"):std::u16string_view(video.file));
                    return succeeded();
                }
                if(command==mci::load){
                    check_flags(0x100U);
                    if(!(flags&0x100U))throw mci::Error(mci::missing_parameter);
                    auto replacement=open_video(string(field(p,p)),video.quartz);
                    for(const auto& [other,item]:bridge.videos)
                        if(other!=id && fold(item->file)==fold(replacement->file) && (!item->shareable || !video.shareable))
                            throw mci::Error(265);
                    static_cast<mci::DeviceControl&>(*replacement)=video;
                    replacement->window=video.window;
                    replacement->alias=video.alias;
                    replacement->shareable=video.shareable;
                    replacement->format=video.format;
                    replacement->speed=video.speed;
                    replacement->left=video.left;
                    replacement->right=video.right;
                    replacement->fixed_target=video.fixed_target;
                    if(video.fixed_target)replacement->target=video.target;
                    if(video.presentation)replacement->presentation=bridge.next_presentation++;
                    complete_mci_transport(id,4);
                    entry->second=std::move(replacement);
                    return succeeded();
                }
                if(command==mci::cue){
                    check_flags(0x70000U);
                    if(flags&0x10000U)throw mci::Error(mci::unsupported);
                    const auto frame=static_cast<std::size_t>(std::min(video.current(now),video.length()-1));
                    try{if(video.clip->visual() && !video.clip->at(frame))throw mci::Error(mci::invalid_file);}
                    catch(const std::runtime_error&){throw mci::Error(mci::invalid_file);}
                    return succeeded();
                }
                if(command==mci::set){
                    check_flags(0x20400U);
                    const auto format=(flags&0x400U)?field(p):video.format;
                    const auto speed=(flags&0x20000U)?field(p+12):video.speed;
                    if(format && format!=mci::time_frames)throw mci::Error(mci::bad_time);
                    if(!speed || speed>10000)throw mci::Error(mci::out_of_range);
                    if(speed!=video.speed){
                        video.position=video.current(now);
                        video.anchor=now;
                        video.speed=static_cast<unsigned>(speed);
                        if(video.wave.buffer){
                            std::lock_guard lock(video.wave.buffer->mutex);
                            video.wave.buffer->frequency=std::max(1U,video.wave.format.rate*video.speed/1000U);
                        }
                    }
                    video.format=static_cast<unsigned>(format);
                    return succeeded();
                }
                if(command==mci::video_set_audio){
                    check_flags(0x1e00000U);
                    if((flags&0x1800000U)!=0x1800000U || field(p)!=0x4002)throw mci::Error(mci::unsupported);
                    const auto value=field(p+4);
                    if(value>1000)throw mci::Error(mci::out_of_range);
                    const auto channels=flags&0x600000U;
                    if(!channels || (channels&0x200000U))video.left=static_cast<unsigned>(value);
                    if(!channels || (channels&0x400000U))video.right=static_cast<unsigned>(value);
                    if(video.wave.buffer){
                        std::lock_guard lock(video.wave.buffer->mutex);
                        video.wave.buffer->left=static_cast<float>(video.left)/1000.F;
                        video.wave.buffer->right=static_cast<float>(video.right)/1000.F;
                    }
                    return succeeded();
                }
                if(command==mci::video_window){
                    check_flags(0x10000U);
                    if(!(flags&0x10000U))throw mci::Error(mci::missing_parameter);
                    const auto hwnd=field(p,p);
                    if(hwnd && !gui_.windows.contains(hwnd))throw mci::Error(mci::bad_constant);
                    video.window=hwnd;
                    video.presented_frame=UINT64_MAX;
                    return succeeded();
                }
                if(command==mci::video_put || command==mci::video_where){
                    const bool putting=command==mci::video_put;
                    check_flags(putting?0x70000U:0x60000U);
                    const auto choice=flags&0x60000U;
                    if(choice!=0x20000U && choice!=0x40000U)throw mci::Error(mci::conflicting_flags);
                    const bool source=choice==0x20000U;
                    auto& rect=source?video.source:video.target;
                    block(p+16);
                    if(putting){
                        if(!(flags&0x10000U))throw mci::Error(mci::missing_parameter);
                        GuiRect requested{};
                        for(unsigned i=0;i<4;++i)requested[i]=static_cast<std::int32_t>(field(p+4*i));
                        if(requested[0]>=requested[2] || requested[1]>=requested[3] ||
                           (source && (requested[0]<0 || requested[1]<0 ||
                            requested[2]>static_cast<std::int32_t>(video.clip->width()) ||
                            requested[3]>static_cast<std::int32_t>(video.clip->height()))))throw mci::Error(mci::out_of_range);
                        rect=requested;
                        video.presented_frame=UINT64_MAX;
                        if(!source)video.fixed_target=true;
                    }else{
                        memory.check(GuestAddress{parameters+p},16,Access::Write);
                        for(unsigned i=0;i<4;++i)
                            memory.store(GuestAddress{parameters+p+4*i},32,static_cast<std::uint32_t>(rect[i]));
                    }
                    return succeeded();
                }
                if(command==mci::video_update){
                    check_flags(0x30000U);
                    std::uint64_t dc=flags&0x20000U?field(p+16,p):0;
                    const auto target=gui_.windows.find(video.window);
                    if(!dc && target!=gui_.windows.end()){
                        dc=video.window+0x10000000U;
                        gui_.contexts.try_emplace(dc);
                    }
                    auto rect=video.target;
                    if(flags&0x10000U)
                        for(unsigned i=0;i<4;++i)rect[i]=static_cast<std::int32_t>(field(p+i*4));
                    else if(!(flags&0x20000U) && !video.fixed_target && target!=gui_.windows.end())
                        rect={0,0,static_cast<std::int32_t>(target->second.width),static_cast<std::int32_t>(target->second.height)};
                    const auto frame=std::min(video.current(now),video.length()-1);
                    if(const auto error=paint_video(video,dc,rect,frame))throw mci::Error(error);
                    return succeeded();
                }
                throw mci::Error(mci::unrecognized);
            }
        }
