// iphlpapi 回呼式變更通知的輪詢與派送（windows_network_config.cpp 尾端引入）：每 500 ms 依（種類, 家族）各取一次快照，與各註冊的基準比較，
// 新增列為 MibAddInstance（1）、消失列為 MibDeleteInstance（2）、穩定欄位改變為 MibParameterNotification（0）。每個註冊同一時間只有一個回呼在工作執行緒上執行，
// 列資料放在 heap，回呼返回後由 finish_mib_notification 釋放；與裝置通知共用 RegisteredWait 派送與 RegisteredWaitCompletion 取消等待。
void WindowsServices::finish_mib_notification(GuestState& state,GuestMemory& memory,std::uint64_t owner){
    const auto found=mib_notifications_.find(owner);if(found==mib_notifications_.end())return;
    auto& n=found->second;if(n.payload)(void)free_heap(memory,n.payload,state);n.payload=0;n.work=0;
    if(n.closing && !n.waiter)mib_notifications_.erase(found);
}
void WindowsServices::poll_mib_notifications(GuestMemory& memory){
    const auto saved_error=last_error_;
    const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if(runtime_.network.external_io && now>=mib_poll_time_){
        mib_poll_time_=now+500;auto& config=*runtime_.network.configuration;
        if(refresh_configuration(config,true,false)){
            std::map<std::pair<std::uint32_t,std::uint16_t>,std::map<std::vector<std::uint8_t>,std::vector<std::uint8_t>>> snapshots;
            for(auto& [owner,n]:mib_notifications_){
                (void)owner;if(n.closing)continue;
                auto current=snapshots.find({n.kind,n.family});
                if(current==snapshots.end()){std::map<std::vector<std::uint8_t>,std::vector<std::uint8_t>> rows;if(mib_snapshot(config,n.kind,n.family,rows))continue;current=snapshots.emplace(std::pair{n.kind,n.family},std::move(rows)).first;}
                const auto& rows=current->second;
                for(const auto& [key,row]:n.rows)if(!rows.contains(key))n.pending.push_back({2,row});
                for(const auto& [key,row]:rows){
                    const auto old=n.rows.find(key);
                    if(old==n.rows.end())n.pending.push_back({1,row});
                    else if(mib_stable(n.kind,old->second)!=mib_stable(n.kind,row))n.pending.push_back({0,row});
                }
                n.rows=rows;
                // 回呼長時間未返回時只保留最近的事件，避免無上限累積。
                if(n.pending.size()>256)n.pending.erase(n.pending.begin(),n.pending.end()-256);
            }
        }
    }
    for(auto& [owner,n]:mib_notifications_){
        if(n.closing || n.work || n.pending.empty())continue;
        if(registered_waits_.size()>=64)break;
        const auto& event=n.pending.front();
        if(!event.row.empty()){n.payload=allocate_heap(memory,event.row.size());if(!n.payload)break;memory.write(GuestAddress{n.payload},event.row);}
        const auto worker=acquire_wait_worker(threads_.items.at(threads_.current).context,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));
        if(!worker){if(n.payload)(void)free_heap(memory,n.payload,threads_.items.at(threads_.current).context);n.payload=0;break;}
        RegisteredWait work;work.worker=worker;work.mib_owner=owner;work.armed=false;work.ready=true;work.closing=true;
        work.callback_batch={{n.callback,{n.context,n.payload,event.type}}};n.work=next_registered_wait_++;
        registered_waits_.emplace(n.work,std::move(work));wait_workers_[worker]=n.work;
        auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};
        n.pending.erase(n.pending.begin());
    }
    last_error_=saved_error;
}
