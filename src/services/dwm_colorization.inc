        if(symbol=="DwmGetColorizationColor" || symbol=="#127" || symbol=="#131"){
            const auto colorization=[&]() -> std::uint32_t {
                constexpr std::array<std::u16string_view,7> names{
                    u"colorizationcolor",u"colorizationafterglow",u"colorizationcolorbalance",
                    u"colorizationafterglowbalance",u"colorizationblurbalance",
                    u"colorizationglassreflectionintensity",u"colorizationopaqueblend"};
                constexpr std::uint32_t invalid=0x80070057U;
                const bool set=symbol=="#131",full=symbol!="DwmGetColorizationColor";
                const auto address=arg(0);
                if(!address || (!full && !arg(1)))return invalid;
                const auto failure=[](std::uint32_t error){return 0x80070000U|(error&0xffffU);};
                const auto colorref=gui_system_color(2);
                const auto accent=0xff000000U|((colorref&0xffU)<<16)|(colorref&0xff00U)|((colorref>>16)&0xffU);
                std::array<std::uint32_t,7> data{accent,accent,100,0,0,0,1};
                std::array<std::uint32_t,7> changes{};
                std::u16string key;
                try {
                    memory.check(GuestAddress{address},full?28:4,set?Access::Read:Access::Write);
                    if(!full)memory.check(GuestAddress{arg(1)},4,Access::Write);
                    if(set)for(unsigned i=0;i<changes.size();++i)
                        changes[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{address+4*i},32));
                    {
                        auto& registry=runtime_.registry;
                        RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_,!set);
                        if(transaction.error)return failure(transaction.error);
                        key=registry_current_user_path()+u"\\software\\microsoft\\windows\\dwm";
                        if(const auto entry=registry.keys.find(key);entry!=registry.keys.end()){
                            for(unsigned i=0;i<names.size();++i){
                                const auto found=entry->second.find(std::u16string(names[i]));
                                if(found==entry->second.end() || found->second.type!=4 || found->second.bytes.size()!=4)continue;
                                data[i]=0;
                                for(unsigned n=0;n<4;++n)data[i]|=std::uint32_t{found->second.bytes[n]}<<(n*8);
                            }
                        }
                        if(set){
                            // 一次提交整份設定；各程序及 WOW64 檢視共用同一個沙盒使用者的色彩。
                            auto next=registry;
                            for(std::size_t pos=1;;){
                                const auto slash=key.find(u'\\',pos);
                                next.keys.try_emplace(key.substr(0,slash));
                                if(slash==key.npos)break;
                                pos=slash+1;
                            }
                            auto& values=next.keys.at(key);
                            for(unsigned i=0;i<changes.size();++i){
                                const std::u16string name(names[i]);
                                if(const auto found=values.find(name);found!=values.end())next.bytes-=found->second.bytes.size();
                                else ++next.values;
                                std::vector<std::uint8_t> bytes(4);
                                for(unsigned n=0;n<4;++n)bytes[n]=static_cast<std::uint8_t>(changes[i]>>(8*n));
                                values[name]={4,std::move(bytes),{}};
                                next.bytes+=4;
                            }
                            next.modified_times[key]=runtime_.system_filetime();
                            if(next.keys.size()>4096 || next.values>4096 || next.bytes>16*1024*1024)return 0x8007000eU;
                            std::swap(registry,next);
                            try {
                                if(const auto error=transaction.commit()){
                                    std::swap(registry,next);
                                    return failure(error);
                                }
                            }catch(...){std::swap(registry,next);throw;}
                        }
                    }
                    if(set){
                        poll_registry_notifications(false,key,4,&memory);
                        if(changes[0]!=data[0] || bool(changes[6])!=bool(data[6]))
                            for(const auto window:gui_broadcast_targets())
                                enqueue_gui_message({window,changes[0],changes[6]!=0,0x0320});
                    }else if(full){
                        for(unsigned i=0;i<data.size();++i)memory.store(GuestAddress{address+4*i},32,data[i]);
                    }else{
                        memory.store(GuestAddress{address},32,data[0]);
                        memory.store(GuestAddress{arg(1)},32,data[6]!=0);
                    }
                    return 0;
                }catch(const MemoryFault&){return invalid;}
                catch(const std::bad_alloc&){return 0x8007000eU;}
            };
            result=colorization();
        }
