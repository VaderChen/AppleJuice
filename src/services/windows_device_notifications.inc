// 與 SetupAPI／CM 列舉共用 devnode；註冊建立基準快照，之後才通知真正的狀態轉換。
void WindowsServices::finish_device_notification(GuestState& state,GuestMemory& memory,std::uint64_t owner){
    if(!device_configuration_)return;
    const auto found=device_configuration_->notifications.find(owner);if(found==device_configuration_->notifications.end())return;
    auto& n=found->second;if(n.payload)(void)free_heap(memory,n.payload,state);n.payload=0;n.work=0;
}
void WindowsServices::poll_device_notifications(GuestMemory& memory){
    if(!device_configuration_ || device_configuration_->notifications.empty())return;
    auto& config=*device_configuration_;const auto saved_error=last_error_;
    if(!registry_io_busy_ && runtime_.clock_ticks>=config.poll_deadline){
        // 每次重新列舉 HID 等裝置約 4～5 ms；插拔通知以 500 ms 為間隔（原 100 ms，Steam.exe 主執行緒約 5%）。
        // X64ARM_DEVICE_POLL_MS 調整，100 為舊行為。
        static const std::uint64_t interval=[] {
            const auto* value=std::getenv("X64ARM_DEVICE_POLL_MS");if(!value)return UINT64_C(5000000);
            const auto parsed=std::strtoull(value,nullptr,10);return std::clamp<std::uint64_t>(parsed,10,10000)*10000;
        }();
        config.poll_deadline=runtime_.clock_ticks+interval;
        refresh_device_configuration(true);
        for(auto& [owner,n]:config.notifications){
            (void)owner;if(n.closing)continue;
            for(const auto& [id,node]:config.nodes){
                const bool matches=n.type==0?(!node.interface_path.empty() && ((n.flags&1U) || supports_interface(node,n.guid))):
                    ((n.flags&2U) || folded(n.instance)==folded(node.id));
                if(!matches)continue;
                const auto previous=n.present.find(id);const bool old=previous!=n.present.end() && previous->second;
                n.present[id]=node.present;if(old==node.present)continue;
                const auto make_event=[&](std::uint32_t action){
                    DeviceConfigurationState::Event e;e.action=action;const auto interface_guid=n.type==0 && !(n.flags&1U)?n.guid:node.interface_guid;
                    if(n.window){
                        // DEV_BROADCAST_DEVICEINTERFACE A/W；WM_DEVICECHANGE 的資料在接收者返回後回收。
                        auto text=n.wide?utf16_bytes(node.interface_path):std::vector<std::uint8_t>{};
                        if(!n.wide){auto encoded=utf16_to_utf8(node.interface_path);encoded.push_back(0);text.assign(encoded.begin(),encoded.end());}
                        e.bytes.resize(28+text.size());std::copy(interface_guid.begin(),interface_guid.end(),e.bytes.begin()+12);
                        for(unsigned i=0;i<4;++i)e.bytes[i]=static_cast<std::uint8_t>(e.bytes.size()>>(8*i));e.bytes[4]=5;
                        std::copy(text.begin(),text.end(),e.bytes.begin()+28);
                    }else{
                        const auto text=n.type==0?utf16_bytes(node.interface_path):n.type==2?utf16_bytes(node.id):std::vector<std::uint8_t>{};
                        const auto offset=n.type==0?24U:8U;e.bytes.resize(std::max<std::size_t>(36,offset+text.size()));e.bytes[0]=static_cast<std::uint8_t>(n.type);
                        if(n.type==0)std::copy(interface_guid.begin(),interface_guid.end(),e.bytes.begin()+8);
                        if(n.type==1)for(unsigned i=24;i<28;++i)e.bytes[i]=0xff; // 無附帶名稱的移除事件。
                        else std::copy(text.begin(),text.end(),e.bytes.begin()+offset);
                    }
                    n.pending.push_back(std::move(e));
                };
                if(n.type==0)make_event(node.present?0:1);
                else if(n.type==2){if(node.present){make_event(7);make_event(8);}else make_event(9);}
                else if(!node.present)make_event(5); // 宿主意外拔除沒有可否決的 query-remove 階段。
            }
        }
    }
    for(auto it=config.notifications.begin();it!=config.notifications.end();){
        const auto owner=it->first;auto& n=it->second;
        if(n.window && n.work && !gui_.sent_messages.contains(n.work))finish_device_notification(threads_.items.at(threads_.current).context,memory,owner);
        if(n.window && !gui_.windows.contains(n.window)){n.closing=true;n.pending.clear();}
        if(n.closing){if(n.window && !n.work){it=config.notifications.erase(it);continue;}++it;continue;}
        if(n.work || n.pending.empty()){++it;continue;}
        const auto& e=n.pending.front();
        if(n.window){
            if(gui_.sent_messages.size()>=8192){++it;continue;}
            const auto w=gui_.windows.find(n.window);if(w==gui_.windows.end()){++it;continue;}
            n.payload=allocate_heap(memory,e.bytes.size());if(!n.payload){++it;continue;}memory.write(GuestAddress{n.payload},e.bytes);
            GuiSentMessage request;request.receiver=w->second.thread;request.wide=n.wide;request.device_owner=owner;
            request.message={n.window,e.action==0?0x8000U:0x8004U,n.payload,0x219};n.work=gui_.next_object++;
            gui_.sent_messages.emplace(n.work,std::move(request));
        }else{
            if(registered_waits_.size()>=64){++it;continue;}
            const auto worker=acquire_wait_worker(threads_.items.at(threads_.current).context,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));
            if(!worker){++it;continue;}
            n.payload=allocate_heap(memory,e.bytes.size());if(!n.payload){++it;continue;}memory.write(GuestAddress{n.payload},e.bytes);
            RegisteredWait work;work.worker=worker;work.device_owner=owner;work.armed=false;work.ready=true;work.closing=true;
            work.callback_batch={{n.callback,{owner,n.context,e.action,n.payload,e.bytes.size()}}};n.work=next_registered_wait_++;
            registered_waits_.emplace(n.work,std::move(work));wait_workers_[worker]=n.work;
            auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};
        }
        n.pending.pop_front();++it;
    }
    last_error_=saved_error;
}
std::optional<std::uint32_t> WindowsServices::invoke_device_notification(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool legacy=name.starts_with("RegisterDeviceNotification") || name=="UnregisterDeviceNotification";
    const bool remove=name=="CM_Unregister_Notification" || name=="UnregisterDeviceNotification";
    const auto finish=[&](std::uint64_t value,std::uint32_t error=0){if(legacy && error)last_error_=error;arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto invalid=[&](std::uint32_t cm,std::uint32_t win){return finish(legacy?0:cm,win);};
    if(!runtime_.network.external_io)throw HostCallFault("原生裝置通知不支援 deterministic compare");
    try{
        if(remove){
            if(!device_configuration_)return invalid(31,6);
            const auto found=device_configuration_->notifications.find(arg(0));
            if(found==device_configuration_->notifications.end() || static_cast<bool>(found->second.window)!=legacy)return invalid(31,6);
            auto& n=found->second;
            if(n.work){
                if(n.window){
                    const auto request=gui_.sent_messages.find(n.work);
                    if(request!=gui_.sent_messages.end() && !request->second.active)gui_.sent_messages.erase(request);
                    else if(request!=gui_.sent_messages.end()){n.closing=true;n.pending.clear();return finish(1);}
                    finish_device_notification(state,memory,arg(0));
                }else{
                    const auto work=registered_waits_.find(n.work);
                    if(work!=registered_waits_.end() && work->second.active){
                        if(work->second.worker==threads_.current)return invalid(19,87);
                        n.closing=true;n.pending.clear();auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;
                        thread.wait={};thread.wait.kind=ThreadWaitKind::RegisteredWaitCompletion;thread.wait.handles={n.work};schedule(state,memory);return std::nullopt;
                    }
                    const auto token=n.work;finish_device_notification(state,memory,arg(0));release_registered_wait(token);
                }
            }
            device_configuration_->notifications.erase(found);threads_.items.at(threads_.current).wait={};return finish(legacy?1:0);
        }
        DeviceConfigurationState::Notification n;
        const auto pointer=arg(legacy?1:0);if(!pointer)return invalid(3,87);
        if(legacy){
            const auto flags=static_cast<std::uint32_t>(arg(2));if(flags&~5U)return invalid(4,87);
            if(flags&1U)return invalid(19,50); // 服務接收者仍需實際服務控制管理員的事件派送。
            if(!gui_.windows.contains(arg(0)))return invalid(3,1400);
            const auto size=memory.load(GuestAddress{pointer},32),kind=memory.load(GuestAddress{pointer+4},32);
            if(size<28 || size>65536 || kind!=5 || memory.load(GuestAddress{pointer+8},32))return invalid(31,87);
            memory.check(GuestAddress{pointer},size,Access::Read);n.window=arg(0);n.wide=name.ends_with('W');n.flags=(flags&4U)?1U:0U;
            memory.read(GuestAddress{pointer+12},n.guid);
        }else{
            if(!arg(2) || !arg(3))return invalid(3,87);
            if(memory.load(GuestAddress{pointer},32)!=416)return invalid(31,87);
            memory.check(GuestAddress{pointer},416,Access::Read);n.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+4},32));
            n.type=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+8},32));
            if(n.type>2 || memory.load(GuestAddress{pointer+12},32))return invalid(31,87);
            if(n.flags&~3U || (n.type==0 && n.flags&~1U) || (n.type==1 && n.flags) || (n.type==2 && n.flags&~2U))return invalid(4,87);
            if(n.type==0){memory.read(GuestAddress{pointer+16},n.guid);if((n.flags&1U) && n.guid!=std::array<std::uint8_t,16>{})return invalid(31,87);}
            else if(n.type==2){
                bool terminated=false;for(unsigned i=0;i<200;++i){const auto ch=static_cast<char16_t>(memory.load(GuestAddress{pointer+16+2*i},16));if(!ch){terminated=true;break;}n.instance.push_back(ch);}
                if(!terminated || ((n.flags&2U)?!n.instance.empty():n.instance.empty()))return invalid(31,87);
            }
            n.callback=arg(2);n.context=arg(1);memory.check(GuestAddress{n.callback},1,Access::Execute);memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);
        }
        refresh_device_configuration(true);
        if(n.type==1){
            const auto handle=memory.load(GuestAddress{pointer+16},x86_?32:64);const auto file=files_.find(handle);
            if(file==files_.end() || (!file->second.serial && !file->second.hid))return invalid(31,6);
            auto node=device_configuration_->nodes.end();
            if(file->second.hid){const auto id=device_configuration_->hid_nodes.find(file->second.hid->device.instance);if(id!=device_configuration_->hid_nodes.end())node=device_configuration_->nodes.find(id->second);}
            else for(const auto& [number,path]:serial_ports_)if(path==file->second.path){node=device_configuration_->nodes.find(0x10000U+number);break;}
            if(node==device_configuration_->nodes.end() || !node->second.present)return invalid(31,6);n.instance=node->second.id;
        }
        auto& config=*device_configuration_;if(config.notifications.size()>=4096)return invalid(2,8);
        for(const auto& [id,node]:config.nodes)n.present.emplace(id,node.present);
        const auto token=config.next_notification++;
        if(!legacy)memory.store(GuestAddress{arg(3)},x86_?32:64,token);
        config.notifications.emplace(token,std::move(n));return finish(legacy?token:0);
    }catch(const MemoryFault&){return invalid(3,998);}catch(const std::bad_alloc&){return invalid(2,8);}
}
