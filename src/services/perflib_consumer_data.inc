namespace {
// 刷新訂閱完成後，才以已提交的 binding 建立這一輪快照與資料請求。
void prepare_perflib_collection(PerflibState& perf,PerflibState::Operation& operation){
    const auto& query=perf.queries.at(operation.query);
    std::vector<PerflibState::RemoteConsumer::Target> targets;std::set<std::uint64_t> providers;
    for(std::size_t i=0;i<operation.identifiers.size();++i){const auto& spec=operation.identifiers[i];
        if(const auto handle=local_query_provider(perf,query,spec))providers.insert(handle);
        const auto binding=std::find_if(query.bindings.begin(),query.bindings.end(),[&](const auto& value){return value.specification.same_query(spec);});
        if(binding==query.bindings.end())throw perflib::Error(13);
        for(const auto& endpoint:binding->remote){
            auto target=std::find_if(targets.begin(),targets.end(),[&](const auto& value){return value.endpoint==endpoint;});
            if(target==targets.end()){
                if(targets.size()>=perflib_query_endpoints)throw perflib::Error(8);
                targets.push_back({});target=std::prev(targets.end());target->endpoint=endpoint;target->provider_pid=endpoint.origin.pid;
            }
            target->indices.push_back(i); // selector 名稱只在該批真正送出時複製。
        }
    }
    std::vector<std::uint64_t> pinned(providers.begin(),providers.end());std::vector<PerflibState::Notification> notifications;
    notifications.reserve(pinned.size());for(const auto handle:pinned)notifications.push_back({handle,5,{},0});
    // 所需配置全數成功後才交接 pin；控制階段已取得的 pin 不重複增加。
    for(const auto handle:operation.pinned)if(!providers.contains(handle))
        if(const auto provider=perf.providers.find(handle);provider!=perf.providers.end() && provider->second.active)--provider->second.active;
    for(const auto handle:pinned)if(std::find(operation.pinned.begin(),operation.pinned.end(),handle)==operation.pinned.end())++perf.providers.at(handle).active;
    operation.pinned=std::move(pinned);operation.notifications=std::move(notifications);operation.notification=operation.position=0;
    operation.consumer->targets=std::move(targets);operation.consumer->submitted=operation.consumer->merged=false;
    operation.consumer->collection.reset();operation.consumer->batched=false;
}
}

// 本地 COLLECT_END 完成後才等待遠端；等待期間不持有本地 provider 的資料鎖。
bool WindowsServices::merge_perflib_consumer_data(std::uint64_t id){
    auto& perf=perflib_state();auto& operation=perf.operations.at(id);auto& consumer=*operation.consumer;
    if(consumer.merged)return true;
    const auto& query=perf.queries.at(operation.query);
    using Collected=PerflibState::RemoteConsumer::Collection::Item;
    const bool initialize=!consumer.collection;
    if(initialize){consumer.collection=std::make_shared<PerflibState::RemoteConsumer::Collection>();consumer.collection->items.resize(operation.identifiers.size());}
    auto& collected=consumer.collection->items;auto& budget=consumer.collection->budget;
    const auto fail=[&](Collected& item,std::uint32_t error){
        if(item.error)return;item.error=error;budget-=item.dynamic_bytes;item.dynamic_bytes=0;item.layout_charged=false;
        std::vector<perflib::Instance>{}.swap(item.instances);
    };
    const auto add=[&](Collected& item,perflib::CounterReply reply){
        if(reply.status){if(reply.status!=1168)fail(item,reply.status);return;}
        if(item.error)return;
        if(!item.registration->multi() && item.instances.size()+reply.instances.size()>1){fail(item,13);return;}
        if(reply.instances.size()>65536-item.instances.size())throw perflib::Error(8);
        if(!reply.instances.empty() && !item.layout_charged){
            const auto extra=(item.wildcard_counters?perflib::align8(8+4*item.registration->counters.size()):0)+(item.registration->multi()?8:0);
            if(extra>maximum_data-budget)throw perflib::Error(8);budget+=extra;item.dynamic_bytes+=extra;item.layout_charged=true;
        }
        for(auto& instance:reply.instances){
            const auto header=item.registration->multi()?perflib::align8(8+2*(instance.name.size()+1)):0;
            if(header>maximum_data-budget)throw perflib::Error(8);budget+=header;item.dynamic_bytes+=header;
            for(const auto& value:instance.values){const auto size=perflib::align8(8+value.bytes.size());
                if(size>maximum_data-budget)throw perflib::Error(8);budget+=size;item.dynamic_bytes+=size;
            }
            item.instances.push_back(std::move(instance));
        }
    };
    if(initialize){
      const auto local=perflib::query_reply(operation.bytes,operation.identifiers.size());
      for(std::size_t i=0;i<collected.size();++i){
        auto& item=collected[i];const auto& spec=operation.identifiers[i];
        if(16>maximum_data-budget)throw perflib::Error(8);budget+=16;item.wildcard_counters=spec.counter==perflib::wildcard;
        const auto binding=std::find_if(query.bindings.begin(),query.bindings.end(),[&](const auto& value){return value.specification.same_query(spec);});
        if(binding!=query.bindings.end())item.registration=binding->registration.get();
        if(!item.registration){const auto handle=local_query_provider(perf,query,spec);
            if(handle)item.registration=&perf.providers.at(handle).sets.at(spec.set);
        }
        if(!item.registration){item.error=1168;continue;}
        const auto& set=*item.registration;
        if(set.instance_type!=0 && set.instance_type!=2){item.error=50;continue;}
        if(const auto handle=local_query_provider(perf,query,spec))
            if(const auto begin=operation.collection_begin_status.find(handle);begin!=operation.collection_begin_status.end() && begin->second)item.error=begin->second;
        add(item,perflib::counter_reply(local.blocks[i],spec,set));
      }
      // 結構化快照已保留；不在等待各批期間另留一份本地 wire payload。
      perflib::Bytes{}.swap(operation.bytes);
    }
    for(auto& target:consumer.targets)if(target.ready && !target.consumed){
        std::vector<perflib::CounterReply> replies;std::uint32_t error=target.error;
        if(!error)try{
            if(target.mode || target.result[3]>UINT32_MAX || target.result[4] || target.result[0]!=target.result[1] ||
               target.result[1]>maximum_data || target.result[2]!=target.specifications.size() || target.specifications.size()!=target.indices.size())throw perflib::Error(13);
            const auto bytes=target.channel->read_output(target.result[1]);const auto frame=perflib::query_reply(bytes,target.specifications.size());
            replies.reserve(target.indices.size());
            for(std::size_t i=0;i<target.indices.size();++i){const auto index=target.indices[i];
                if(index>=collected.size() || !target.specifications[i].same_query(operation.identifiers[index]) || !collected[index].registration)throw perflib::Error(13);
                replies.push_back(perflib::counter_reply(frame.blocks[i],operation.identifiers[index],*collected[index].registration));
                if(target.result[3] && replies.back().status!=target.result[3])throw perflib::Error(13);
            }
            if(target.result[3])error=static_cast<std::uint32_t>(target.result[3]);
        }catch(const perflib::Error&){error=13;}
        // 整份回覆核對完成才合併；畸形 block 不留下同一 provider 的部分成功資料。
        for(std::size_t i=0;i<target.indices.size();++i){auto& item=collected.at(target.indices[i]);
            if(error)fail(item,error);else add(item,std::move(replies[i]));
        }
        target.channel.reset();target.consumed=true;
        std::vector<perflib::Identifier>{}.swap(target.specifications);std::vector<std::size_t>{}.swap(target.indices);
    }
    schedule_perflib_batch(consumer,[&](std::size_t index,std::uint32_t code){(void)submit_perflib_consumer_target(id,index,code);});
    if(!std::all_of(consumer.targets.begin(),consumer.targets.end(),[](const auto& target){return target.consumed;}))return false;
    std::vector<perflib::Bytes> blocks;blocks.reserve(collected.size());
    for(std::size_t i=0;i<collected.size();++i){auto& item=collected[i];
        if(!item.error && !item.registration->multi() && item.instances.size()>1)item.error=13;
        if(item.error){blocks.push_back(perflib::error_block(item.error));continue;}
        std::vector<std::uint32_t> counters;for(const auto& counter:item.registration->counters)counters.push_back(counter.id);
        blocks.push_back(perflib::query_block(operation.identifiers[i],item.instances,counters,item.registration->multi()));
    }
    // block 順序沿用 PerfQueryCounterInfo 的 Index；PERF_COUNTER_HEADER.Reserved 維持零。
    operation.bytes=perflib::query_data(timestamp(runtime_),blocks);consumer.collection.reset();consumer.merged=true;return true;
}
