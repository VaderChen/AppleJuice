// 公開 query 控制與 provider ledger 共用真正的回呼、RPC 與清理流程。
namespace {
constexpr std::size_t perflib_batch_width=32,perflib_query_endpoints=1024;
constexpr std::size_t perflib_query_schemas=4096,perflib_schema_bytes=64U*1024U*1024U;
std::size_t perflib_schema_charge(const PerflibState::Query::Schema& schema){
    return 40+32*schema.registration->counters.size()+32*schema.remote.size();
}
void forget_perflib_schema_endpoint(PerflibState::Query& query,const PerflibState::RemoteEndpoint& endpoint){
    for(auto& [id,schema]:query.known_schemas){(void)id;const auto removed=std::erase(schema.remote,endpoint);query.schema_bytes-=32*removed;}
}
void begin_perflib_batch(PerflibState::RemoteConsumer& consumer,std::uint32_t code){
    consumer.batched=true;consumer.batch_code=code;consumer.next_submit=0;
    for(auto& target:consumer.targets){
        target.channel.reset();target.sequence=0;target.consumed=false;
        target.deadline={};target.retry_at={};target.retry_delay_ms=1;target.retry_pending=false;
        target.ready=code==perflib::rpc::undo_counter && !target.add_sequence;
    }
}
template<class Submit>void schedule_perflib_batch(PerflibState::RemoteConsumer& consumer,Submit&& submit){
    if(!consumer.batched)return;
    std::size_t pending=0;
    for(const auto& target:consumer.targets)if(consumer.batch_code==perflib::rpc::collect?
        !target.consumed && (target.sequence || target.ready || target.retry_pending):
        !target.ready && (target.sequence || target.retry_pending))++pending;
    while(consumer.next_submit<consumer.targets.size() && pending<perflib_batch_width){
        const auto index=consumer.next_submit++;auto& target=consumer.targets[index];if(target.ready || target.sequence || target.retry_pending)continue;
        submit(index,consumer.batch_code);
        // 資料錯誤也占用一個待消費 slot，必須交由 API continuation 處理才繼續下一批。
        if(consumer.batch_code==perflib::rpc::collect || !target.ready)++pending;
    }
}
}

std::uint32_t WindowsServices::submit_perflib_consumer_target(std::uint64_t id,std::size_t index,std::uint32_t code){
    auto& operation=perflib_state().operations.at(id);auto& target=operation.consumer->targets.at(index);
    target.ready=false;target.error=target.mode=0;target.result={};target.sequence=0;target.request_code=code;
    target.retry_pending=false;
    if(target.deadline==std::chrono::steady_clock::time_point{})target.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(5000);
    try{
        if(!runtime_.network.external_io)throw perflib::Error(50);
        if(code==perflib::rpc::collect && target.specifications.empty()){
            std::size_t size=perflib::rpc::header_size;target.specifications.reserve(target.indices.size());
            for(const auto position:target.indices){const auto& spec=operation.identifiers.at(position);
                const auto encoded=perflib::rpc::encoded_identifier(spec);
                if(encoded.size()>maximum_data-size)throw perflib::Error(8);size+=encoded.size();target.specifications.push_back(spec);
            }
        }
        const auto request=perflib::rpc::request(code,target.specifications,code==perflib::rpc::undo_counter?target.add_sequence:0);
        const auto capacity=perflib::rpc::no_output(code)?0U:code==perflib::rpc::collect?maximum_data:
            code==perflib::rpc::enumerate?operation.capacity:request.size()-perflib::rpc::header_size;
        target.channel=perflib::rpc::Channel::create(mapping_session(),request,capacity);
        const auto& token=operation.consumer->token;if(!token)throw perflib::Error(13);
        const auto& endpoint=target.endpoint;
        const std::array<std::uint64_t,5> values{request.size(),capacity,endpoint.publication,endpoint.handle,operation.query};
        const auto now=std::chrono::steady_clock::now();if(now>=target.deadline)throw perflib::Error(121);
        const auto timeout=static_cast<std::uint32_t>(std::max<std::int64_t>(1,std::chrono::duration_cast<std::chrono::milliseconds>(target.deadline-now).count()));
        const auto error=begin_remote_family_request(endpoint.origin.pid,endpoint.origin.birth,perflib::rpc::family,code,values,0,
            target.channel->descriptor(),token->descriptor(),target.sequence,timeout);
        if(error){target.error=error;target.ready=true;return error;}
        if(perflib::rpc::subscribing(code)){
            target.add_sequence=target.sequence;auto& contacts=perflib_state().queries.at(operation.query).contacts;
            const auto contact=std::find_if(contacts.begin(),contacts.end(),[&](const auto& value){return value.endpoint==endpoint;});
            if(contact!=contacts.end())contact->attempted=true;
        }
        return 0;
    }catch(const perflib::Error& error){target.error=error.code;}
     catch(const SecurityDescriptorError& error){target.error=error.code;}
     catch(const std::bad_alloc&){target.error=8;}
    target.ready=true;return target.error;
}

bool WindowsServices::advance_perflib_control(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& perf=perflib_state();auto& operation=perf.operations.at(id);auto& control=*operation.control;
    auto& query=perf.queries.at(operation.query);const bool add=operation.action==PerflibState::Action::Add || control.refresh;
    using Phase=PerflibState::Control::Phase;
    const auto waiting=[&]{
        if(perflib_consumer_ready(id))return false;
        auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};
        thread.wait.kind=ThreadWaitKind::Perflib;thread.wait.handles={id};threads_.yield=true;notify_concurrency_changed();return true;
    };
    const auto pin=[&](std::uint64_t handle){
        const auto provider=perf.providers.find(handle);if(provider==perf.providers.end() || provider->second.stopping)return;
        if(handle && std::find(operation.pinned.begin(),operation.pinned.end(),handle)==operation.pinned.end()){
            operation.pinned.push_back(handle);++provider->second.active;
        }
    };
    const auto callback=[&](std::uint32_t code){
        const auto provider=perf.providers.find(control.staged.local_provider);
        if(provider==perf.providers.end() || provider->second.stopping || !provider->second.callback){control.callback_result=0;control.callback_complete=true;return false;}
        pin(provider->first);const auto bytes=perflib::notification(code,provider->second.id,&control.staged.specification,perf.machine,runtime_.system_filetime());
        operation.scratch=allocate_heap(memory,bytes.size());if(!operation.scratch)throw perflib::Error(8);memory.write(GuestAddress{operation.scratch},bytes);
        operation.awaiting=PerflibState::Await::Control;
        try{start_callbacks(state,memory,{{provider->second.callback,{code,operation.scratch,bytes.size()}}},Continuation::PerflibOperation,id);}
        catch(...){operation.awaiting=PerflibState::Await::None;throw;}
        return true;
    };
    const auto reply=[&](const PerflibState::RemoteConsumer::Target& target,const perflib::Identifier* spec,bool removing){
        if(target.error){
            if(removing && (target.error==1168 || process_identity::query(target.endpoint.origin.pid).gone(target.endpoint.origin)))return 0U;
            return target.error;
        }
        if(target.mode || target.result[3] ||
           (target.request_code==perflib::rpc::ensure_counter?target.result[4]>1:target.result[4]!=0) || target.result[0]!=target.result[1])return 13U;
        if(!spec)return target.result[0] || target.result[2]?13U:0U;
        if(target.result[0]!=perflib::rpc::encoded_identifier(*spec).size() || target.result[2]!=1)return 13U;
        try{
            const auto values=perflib::identifiers(target.channel->read_output(target.result[1]));
            if(values.size()!=1 || !values[0].same_query(*spec) || values[0].index!=spec->index)return 13U;
            if(target.request_code==perflib::rpc::ensure_counter && values[0].status && target.result[4])return 13U;
            return removing && values[0].status==1168?0U:values[0].status;
        }catch(const perflib::Error&){return 13U;}
    };
    const auto binding=[&](const perflib::Identifier& spec){
        return std::find_if(query.bindings.begin(),query.bindings.end(),[&](const auto& value){return value.specification.same_query(spec);});
    };
    const auto build_targets=[&](const std::vector<PerflibState::RemoteEndpoint>& endpoints,const perflib::Identifier* spec){
        operation.consumer->batched=false;operation.consumer->next_submit=0;operation.consumer->batch_code=0;
        operation.consumer->targets.clear();operation.consumer->targets.reserve(endpoints.size());
        for(const auto& endpoint:endpoints){PerflibState::RemoteConsumer::Target target;
            target.provider_pid=endpoint.origin.pid;target.endpoint=endpoint;if(spec)target.specifications.push_back(*spec);
            operation.consumer->targets.push_back(std::move(target));
        }
    };
    const auto batch=[&](std::uint32_t code){
        begin_perflib_batch(*operation.consumer,code);
        schedule_perflib_batch(*operation.consumer,[&](std::size_t index,std::uint32_t request){(void)submit_perflib_consumer_target(id,index,request);});
    };
    for(;;){
        if(control.phase==Phase::Probe){
            if(!control.probes_started){
                std::vector<PerflibState::RemoteEndpoint> endpoints;
                const auto wanted=[&](const perflib::Guid& set){return std::any_of(operation.identifiers.begin(),operation.identifiers.end(),[&](const auto& spec){
                    if(spec.set!=set)return false;if(control.refresh)return true;
                    const auto schema=query.known_schemas.find(set);
                    return schema!=query.known_schemas.end() && !validate_identifier(spec,*schema->second.registration) &&
                        std::none_of(query.counters.begin(),query.counters.end(),[&](const auto& counter){return counter.same_query(spec);});
                });};
                const auto probe=[&](const auto& endpoint){if(std::find(endpoints.begin(),endpoints.end(),endpoint)==endpoints.end()){
                    if(endpoints.size()>=perflib_query_endpoints)throw perflib::Error(8);endpoints.push_back(endpoint);
                }};
                std::erase_if(query.contacts,[](const auto& contact){return process_identity::query(contact.endpoint.origin.pid).gone(contact.endpoint.origin);});
                std::erase_if(control.candidates,[](const auto& entry){return process_identity::query(entry.origin.pid).gone(entry.origin);});
                for(auto& current:query.bindings){
                    std::erase_if(current.remote,[](const auto& endpoint){return process_identity::query(endpoint.origin.pid).gone(endpoint.origin);});
                    if(wanted(current.specification.set))for(const auto& endpoint:current.remote)probe(endpoint);
                }
                for(auto& [schema_id,schema]:query.known_schemas){
                    const auto removed=std::erase_if(schema.remote,[](const auto& endpoint){return process_identity::query(endpoint.origin.pid).gone(endpoint.origin);});
                    query.schema_bytes-=32*removed;
                    if(wanted(schema_id))for(const auto& endpoint:schema.remote)probe(endpoint);
                }
                build_targets(endpoints,nullptr);control.probes_started=true;
                batch(perflib::rpc::probe_endpoint);
            }
            if(waiting())return false;
            for(const auto& target:operation.consumer->targets){
                bool retired=process_identity::query(target.endpoint.origin.pid).gone(target.endpoint.origin);
                if(!retired){
                    if(target.error){operation.status=target.error;return true;}
                    if(target.mode || target.result[0] || target.result[1] || target.result[2] || target.result[4] ||
                       (target.result[3]!=perflib::rpc::endpoint_live && target.result[3]!=perflib::rpc::endpoint_retired)){
                        operation.status=13;return true;
                    }
                    retired=target.result[3]==perflib::rpc::endpoint_retired;
                }
                if(retired){
                    for(auto& current:query.bindings)std::erase(current.remote,target.endpoint);
                    forget_perflib_schema_endpoint(query,target.endpoint);
                    std::erase_if(control.candidates,[&](const auto& entry){return entry.origin==target.endpoint.origin && entry.publication==target.endpoint.publication && entry.handle==target.endpoint.handle;});
                    // 退役／程序死亡已清除該 endpoint 的 ledger，Close 不再重送已失效的 cookie。
                    std::erase_if(query.contacts,[&](const auto& contact){return contact.endpoint==target.endpoint;});
                }
            }
            control.reconcile_endpoints.clear();control.reconcile_endpoints.reserve(query.contacts.size());
            for(const auto& contact:query.contacts)if(contact.attempted)control.reconcile_endpoints.push_back(contact.endpoint);
            operation.consumer->targets.clear();control.phase=Phase::Reconcile;continue;
        }
        if(control.phase==Phase::Reconcile){
            if(control.reconcile_position==control.reconcile_endpoints.size()){
                operation.consumer->targets.clear();control.phase=Phase::Prepare;continue;
            }
            const auto& endpoint=control.reconcile_endpoints[control.reconcile_position];
            if(!control.reconcile_started){
                build_targets(std::vector<PerflibState::RemoteEndpoint>{endpoint},nullptr);
                auto& target=operation.consumer->targets.front();
                for(const auto& current:query.bindings)if(std::find(current.remote.begin(),current.remote.end(),endpoint)!=current.remote.end())target.specifications.push_back(current.specification);
                control.reconcile_started=true;(void)submit_perflib_consumer_target(id,0,perflib::rpc::reconcile_query);
            }
            if(waiting())return false;
            const auto& target=operation.consumer->targets.front();bool retired=process_identity::query(endpoint.origin.pid).gone(endpoint.origin);
            if(!retired){
                if(target.error){operation.status=target.error;return true;}
                if(target.mode || target.result[0] || target.result[1] || target.result[2] || target.result[4] ||
                   (target.result[3]!=perflib::rpc::endpoint_live && target.result[3]!=perflib::rpc::endpoint_retired)){
                    operation.status=13;return true;
                }
                retired=target.result[3]==perflib::rpc::endpoint_retired;
            }
            if(retired){
                for(auto& current:query.bindings)std::erase(current.remote,endpoint);
                forget_perflib_schema_endpoint(query,endpoint);
                std::erase_if(control.candidates,[&](const auto& entry){return entry.origin==endpoint.origin && entry.publication==endpoint.publication && entry.handle==endpoint.handle;});
            }
            const bool referenced=std::any_of(query.bindings.begin(),query.bindings.end(),[&](const auto& current){return std::find(current.remote.begin(),current.remote.end(),endpoint)!=current.remote.end();});
            if(retired || !referenced)std::erase_if(query.contacts,[&](const auto& contact){return contact.endpoint==endpoint;});
            ++control.reconcile_position;control.reconcile_started=false;operation.consumer->targets.clear();continue;
        }
        if(control.phase==Phase::CloseRemote){
            if(waiting())return false;
            for(const auto& target:operation.consumer->targets)if(const auto error=reply(target,nullptr,true)){operation.status=error;return true;}
            operation.status=0;return true;
        }
        if(operation.position==operation.identifiers.size()){
            if(operation.action!=PerflibState::Action::Close){if(!control.refresh)operation.status=0;return true;}
            std::vector<PerflibState::RemoteEndpoint> endpoints;
            for(const auto& contact:query.contacts)if(contact.attempted)endpoints.push_back(contact.endpoint);
            build_targets(endpoints,nullptr);control.phase=Phase::CloseRemote;
            batch(perflib::rpc::close_query);
            continue;
        }
        auto& specification=operation.identifiers[operation.position];
        if(control.phase==Phase::Prepare){
            control.status=0;control.callback_complete=control.local_added=false;control.staged={};
            control.schema_node={};control.schema_charge=0;
            control.staged.specification=specification;control.counter_copy=specification;operation.consumer->targets.clear();
            const auto previous=std::find_if(query.counters.begin(),query.counters.end(),[&](const auto& value){return value.same_query(specification);});
            if(add){
                const auto current=control.refresh?binding(specification):query.bindings.end();
                if(control.refresh && (previous==query.counters.end() || current==query.bindings.end() || !current->registration))control.status=13;
                else if(!control.refresh && previous!=query.counters.end())control.status=183;
                else if(!control.refresh && query.counters.size()>=65536)control.status=8;
                else{
                    const auto remembered=query.known_schemas.find(specification.set);
                    const perflib::CounterSet* registration=control.refresh?current->registration.get():
                        remembered==query.known_schemas.end()?nullptr:remembered->second.registration.get();
                    if(control.refresh){
                        if(current->local_provider){const auto provider=perf.providers.find(current->local_provider);
                            if(provider==perf.providers.end() || provider->second.stopping)current->local_provider=0;
                        }
                        // 查詢失敗／權限錯誤不等於死亡；只移除已確認離開或 PID 重用的參照。
                        std::erase_if(current->remote,[](const auto& endpoint){return process_identity::query(endpoint.origin.pid).gone(endpoint.origin);});
                        control.staged.remote=current->remote; // Ensure 核對每個既有 selector，而不是只確認 provider 存活。
                    }
                    if(remembered!=query.known_schemas.end())for(const auto& endpoint:remembered->second.remote)
                        if(std::find(control.staged.remote.begin(),control.staged.remote.end(),endpoint)==control.staged.remote.end())control.staged.remote.push_back(endpoint);
                    const auto local=perf.provider_for(specification.set);
                    if(local){const auto& set=perf.providers.at(local).sets.at(specification.set);
                        if(registration && !perflib::registration_compatible(*registration,set))control.status=13;
                        else{
                            if(!registration)registration=&set;
                            if(!control.refresh || current->local_provider!=local)control.staged.local_provider=local;
                        }
                    }
                    for(const auto& entry:control.candidates){
                        if(control.status)break;
                        const auto set=entry.sets.find(specification.set);if(set==entry.sets.end())continue;
                        if(registration && !perflib::registration_compatible(*registration,set->second)){control.status=13;break;}
                        if(!registration)registration=&set->second;
                        const PerflibState::RemoteEndpoint endpoint{entry.origin,entry.publication,entry.handle};
                        if(std::find(control.staged.remote.begin(),control.staged.remote.end(),endpoint)!=control.staged.remote.end())continue;
                        if(control.staged.remote.size()>=64){control.status=8;break;}
                        control.staged.remote.push_back(endpoint);
                    }
                    if(!control.status)control.status=registration?validate_identifier(specification,*registration):1168U;
                    if(registration){control.staged.multi_instance=registration->multi();
                        if(!control.status){
                            const auto known=std::find_if(query.bindings.begin(),query.bindings.end(),[&](const auto& value){return value.registration && value.specification.set==specification.set;});
                            if(remembered!=query.known_schemas.end()){
                                if(!perflib::registration_compatible(*remembered->second.registration,*registration))control.status=13;
                                else control.staged.registration=remembered->second.registration;
                            }else if(known!=query.bindings.end()){
                                if(!perflib::registration_compatible(*known->registration,*registration))control.status=13;
                                else control.staged.registration=known->registration;
                            }else control.staged.registration=std::make_shared<const perflib::CounterSet>(*registration);
                        }
                    }
                    if(!control.status){
                        PerflibState::Query::Schema schema{control.staged.registration,control.staged.remote};
                        const auto charge=perflib_schema_charge(schema),old_charge=remembered==query.known_schemas.end()?0:perflib_schema_charge(remembered->second);
                        if((remembered==query.known_schemas.end() && query.known_schemas.size()>=perflib_query_schemas) ||
                           query.schema_bytes<old_charge || query.schema_bytes-old_charge>perflib_schema_bytes ||
                           charge>perflib_schema_bytes-(query.schema_bytes-old_charge))control.status=8;
                        else{
                            // 回呼前備妥 map 節點；真正成功時移交，不在 Add 成功後再配置。
                            PerflibState::Query::Schemas pending;pending.emplace(specification.set,std::move(schema));
                            control.schema_node=pending.extract(specification.set);control.schema_charge=charge;
                        }
                    }
                    if(!control.status){
                        if(control.refresh)current->remote.reserve(current->remote.size()+control.staged.remote.size());
                        else{query.counters.reserve(query.counters.size()+1);query.bindings.reserve(query.bindings.size()+1);}
                        for(const auto& endpoint:control.staged.remote)if(std::none_of(query.contacts.begin(),query.contacts.end(),[&](const auto& contact){return contact.endpoint==endpoint;})){
                            if(query.contacts.size()>=1024)throw perflib::Error(8);query.contacts.push_back({endpoint,false});
                        }
                    }
                }
            }else{
                const auto current=binding(specification);
                if(previous==query.counters.end())control.status=1168;
                else if(current!=query.bindings.end()){
                    control.staged=*current;
                    if(current->multi_instance?specification.size<=40:specification.size!=40)control.status=87;
                }
                else control.staged.local_provider=perf.provider_for(specification.set);
            }
            if(control.status){control.phase=Phase::Commit;continue;}
            build_targets(control.staged.remote,&specification);pin(control.staged.local_provider);control.phase=Phase::Local;
        }
        if(control.phase==Phase::Local){
            if(!control.callback_complete && callback(add?1:2))return false;
            if(add){control.status=control.callback_result;control.local_added=!control.status && control.staged.local_provider;}
            else if(const auto current=binding(specification);current!=query.bindings.end())current->local_provider=0;
            control.callback_complete=false;
            if(control.status){control.phase=Phase::Commit;continue;}
            control.phase=Phase::Remote;
            batch(add?(control.refresh?perflib::rpc::ensure_counter:perflib::rpc::add):perflib::rpc::remove);
        }
        if(control.phase==Phase::Remote){
            if(waiting())return false;
            for(auto& target:operation.consumer->targets){const auto status=reply(target,&specification,!add);
                if(!status && target.request_code==perflib::rpc::ensure_counter && !target.result[4])target.add_sequence=0;
                if(status && !control.status)control.status=status;
                if(!add && !status)if(const auto current=binding(specification);current!=query.bindings.end())std::erase(current->remote,target.endpoint);
            }
            if(add && control.status){control.phase=Phase::RollbackLocal;control.callback_complete=false;}
            else control.phase=Phase::Commit;
        }
        if(control.phase==Phase::RollbackLocal){
            if(control.local_added && !control.callback_complete && callback(2))return false;
            control.local_added=false;control.callback_complete=false;
            batch(perflib::rpc::undo_counter);
            control.phase=Phase::RollbackRemote;
        }
        if(control.phase==Phase::RollbackRemote){
            if(waiting())return false;
            for(const auto& target:operation.consumer->targets)if(target.add_sequence && reply(target,&specification,true))
                (void)queue_remote_family_cancel(target.endpoint.origin.pid,target.endpoint.origin.birth,perflib::rpc::family,target.add_sequence);
            control.phase=Phase::Commit;
        }
        if(control.phase==Phase::Commit){
            if(add && !control.status){
                if(!control.schema_node.empty()){
                    const auto known=query.known_schemas.find(specification.set);
                    if(known!=query.known_schemas.end()){
                        query.schema_bytes-=perflib_schema_charge(known->second);known->second=std::move(control.schema_node.mapped());
                        control.schema_node={};query.schema_bytes+=control.schema_charge;
                    }else{
                        const auto saved=query.known_schemas.insert(std::move(control.schema_node));
                        if(saved.inserted)query.schema_bytes+=control.schema_charge;
                    }
                }
                control.counter_copy.status=0;control.staged.specification.status=0;
                if(control.refresh){auto current=binding(specification);if(current==query.bindings.end())throw perflib::Error(13);
                    if(control.staged.local_provider)current->local_provider=control.staged.local_provider;
                    for(const auto& endpoint:control.staged.remote)if(std::find(current->remote.begin(),current->remote.end(),endpoint)==current->remote.end())current->remote.push_back(endpoint);
                }else{query.counters.push_back(std::move(control.counter_copy));query.bindings.push_back(std::move(control.staged));}
                control.local_added=false;
                for(auto& target:operation.consumer->targets)target.add_sequence=0; // 參照已移交 query，異常清理不可撤銷已提交項目。
            }else if(!add){
                const auto current=binding(specification);
                if(!control.status || (current!=query.bindings.end() && !current->local_provider && current->remote.empty())){
                    std::erase_if(query.counters,[&](const auto& value){return value.same_query(specification);});
                    std::erase_if(query.bindings,[&](const auto& value){return value.specification.same_query(specification);});
                    control.status=0;
                }
            }
            if(control.refresh && control.status && !operation.status)operation.status=control.status;
            specification.status=control.status;
            std::erase_if(query.contacts,[](const auto& contact){return !contact.attempted;});
            if(operation.output && !control.refresh)memory.store(GuestAddress{operation.output+operation.output_offset+16},32,control.status);
            operation.output_offset+=specification.size;++operation.position;control.phase=Phase::Prepare;
        }
    }
}
