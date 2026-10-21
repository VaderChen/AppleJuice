#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "perflib_state.hpp"
#include "perflib_query_reply.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/security_access.hpp"
#include <ctime>

namespace x64arm {
namespace {
constexpr std::size_t maximum_data=16U*1024U*1024U;
perflib::Bytes read_bytes(const GuestMemory& memory,std::uint64_t address,std::size_t size){
    if(size>maximum_data)throw perflib::Error(8);
    if(!address && size)throw perflib::Error(87);
    perflib::Bytes result(size);if(size)memory.read(GuestAddress{address},result);return result;
}
perflib::Guid read_guid(const GuestMemory& memory,std::uint64_t address){
    if(!address)throw perflib::Error(87);perflib::Guid result{};memory.read(GuestAddress{address},result);return result;
}
std::u16string read_name(const GuestMemory& memory,std::uint64_t address,bool optional=false){
    if(!address){if(optional)return {};throw perflib::Error(87);}
    std::u16string result;
    for(std::size_t i=0;i<=1024;++i){
        if(address>UINT64_MAX-i*2)throw perflib::Error(87);
        const auto value=static_cast<char16_t>(memory.load(GuestAddress{address+i*2},16));
        if(!value)return result;if(i==1024)throw perflib::Error(87);result+=value;
    }
    throw perflib::Error(87);
}
bool local_machine(std::u16string name,std::u16string_view local){
    if(name.starts_with(u"\\\\"))name.erase(0,2);
    for(auto& c:name)c=unicode_case_unit(c,false);
    std::u16string expected(local);for(auto& c:expected)c=unicode_case_unit(c,false);
    return name.empty() || name==u"." || name==u"localhost" || name==expected;
}
perflib::Timestamp timestamp(const RuntimeState& runtime){
    perflib::Timestamp result;result.counter=runtime.clock_ticks;result.filetime=runtime.system_filetime();result.frequency=10000000;
    constexpr std::uint64_t epoch=116444736000000000ULL;
    if(result.filetime>=epoch){const auto seconds=static_cast<std::time_t>((result.filetime-epoch)/10000000);std::tm value{};
#if defined(_WIN32)
        if(!gmtime_s(&value,&seconds))
#else
        if(gmtime_r(&seconds,&value))
#endif
            result.systemtime={static_cast<std::uint16_t>(value.tm_year+1900),static_cast<std::uint16_t>(value.tm_mon+1),static_cast<std::uint16_t>(value.tm_wday),static_cast<std::uint16_t>(value.tm_mday),static_cast<std::uint16_t>(value.tm_hour),static_cast<std::uint16_t>(value.tm_min),static_cast<std::uint16_t>(value.tm_sec),static_cast<std::uint16_t>((result.filetime/10000)%1000)};
    }
    return result;
}
std::uint32_t copy_output(GuestMemory& memory,std::uint64_t output,std::uint32_t capacity,std::uint64_t actual,std::span<const std::uint8_t> bytes){
    if(!actual || (capacity && !output))return 87;
    memory.check(GuestAddress{actual},4,Access::Write);
    memory.store(GuestAddress{actual},32,bytes.size());
    if(capacity<bytes.size())return 8;
    if(!bytes.empty())memory.write(GuestAddress{output},bytes);return 0;
}
std::uint32_t validate_identifier(const perflib::Identifier& id,const perflib::CounterSet& set){
    // 單實例不附名稱；多實例必須有非空 filter，全選使用真正的 "*"。
    if(set.multi()?(id.size<=40 || id.name.empty()):(id.size!=40 || !id.name.empty()))return 87;
    if(id.counter!=perflib::wildcard && std::none_of(set.counters.begin(),set.counters.end(),[&](const auto& counter){return counter.id==id.counter;}))return 1168;
    return 0;
}
std::uint64_t local_query_provider(const PerflibState& perf,const PerflibState::Query& query,const perflib::Identifier& spec){
    const auto binding=std::find_if(query.bindings.begin(),query.bindings.end(),[&](const auto& value){return value.specification.same_query(spec);});
    if(binding==query.bindings.end())return perf.provider_for(spec.set);
    const auto provider=perf.providers.find(binding->local_provider);
    return provider==perf.providers.end() || provider->second.stopping || !provider->second.sets.contains(spec.set)?0:provider->first;
}
std::vector<perflib::Instance> collect_instances(const PerflibState::Provider& provider,const perflib::CounterSet& set,const perflib::Identifier& spec,const GuestMemory& memory,unsigned pointer_bytes){
    std::vector<perflib::Instance> result;std::size_t bytes=0;
    for(const auto& [address,instance]:provider.instances){
        if(instance.set!=set.id || (spec.instance!=perflib::wildcard && instance.id!=spec.instance) || (!spec.name.empty() && !perflib::matches(spec.name,instance.name)))continue;
        const auto overhead=perflib::align8(8+2*(instance.name.size()+1));if(overhead>maximum_data-bytes)throw perflib::Error(8);bytes+=overhead;
        perflib::Instance value;value.id=instance.id;value.name=instance.name;
        for(const auto& counter:set.counters){
            if(spec.counter!=perflib::wildcard && counter.id!=spec.counter)continue;
            const auto extent=perflib::align8(8+counter.size);if(extent>maximum_data-bytes)throw perflib::Error(8);bytes+=extent;
            perflib::Value data;data.counter=counter.id;
            try{
                const auto source=counter.reference()?memory.load(GuestAddress{address+counter.offset},static_cast<std::uint8_t>(pointer_bytes*8)):address+counter.offset;
                // NULL reference 是尚無資料；非 NULL 的無效客體地址仍回報存取錯誤。
                if(!source)data.status=counter.reference()?232U:998U;else data.bytes=read_bytes(memory,source,counter.size);
            }catch(const MemoryFault&){data.status=998;}
            value.values.push_back(std::move(data));
        }
        result.push_back(std::move(value));
    }
    return result;
}
}

std::span<const WindowsServiceDescriptor> perflib_service_descriptors(){
    static const std::vector<WindowsServiceDescriptor> entries=[] {
        std::vector<WindowsServiceDescriptor> result;
        for(const auto family:{"advapi32.dll","sechost.dll","kernelbase.dll"}){
            const WindowsServiceDescriptor provider[]{
                {family,"PerfStartProvider",3},{family,"PerfStartProviderEx",3},{family,"PerfStopProvider",1},
                {family,"PerfSetCounterSetInfo",3,0,0,4},{family,"PerfCreateInstance",4,0,0,8},{family,"PerfDeleteInstance",2},{family,"PerfQueryInstance",4,0,0,8},
                {family,"PerfSetCounterRefValue",4,0,0,4},{family,"PerfSetULongCounterValue",4,0,0,12},{family,"PerfSetULongLongCounterValue",4,8,0,4},
                {family,"PerfIncrementULongCounterValue",4,0,0,12},{family,"PerfIncrementULongLongCounterValue",4,8,0,4},
                {family,"PerfDecrementULongCounterValue",4,0,0,12},{family,"PerfDecrementULongLongCounterValue",4,8,0,4},
                {family,"PerfOpenQueryHandle",2},{family,"PerfCloseQueryHandle",1},{family,"PerfAddCounters",3,0,0,4},{family,"PerfDeleteCounters",3,0,0,4},
                {family,"PerfQueryCounterInfo",4,0,0,4},{family,"PerfQueryCounterData",4,0,0,4},{family,"PerfEnumerateCounterSet",4,0,0,4},
                {family,"PerfEnumerateCounterSetInstances",5,0,0,8},{family,"PerfQueryCounterSetRegistrationInfo",7,0,0,44}
            };
            result.insert(result.end(),std::begin(provider),std::end(provider));
        }
        return result;
    }();return entries;
}

PerflibState& WindowsServices::perflib_state(){if(!perflib_)perflib_=std::make_shared<PerflibState>();return *perflib_;}

std::uint32_t WindowsServices::queue_perflib_remote(std::uint32_t caller,std::uint64_t birth,std::uint64_t sequence,
    std::uint32_t code,std::uint32_t timeout,const std::array<std::uint64_t,5>& values,std::uint32_t mode,int& payload,int token){
    try{
        if(mode || !caller || !birth || !sequence || !timeout || timeout>60000 || !values[2] || !values[3] || payload<0 || token<0)return 87;
        if(!perflib::rpc::supported(code))return 50;
        if((perflib::rpc::subscribing(code) || perflib::rpc::removing(code) || perflib::rpc::empty_request(code) || code==perflib::rpc::reconcile_query) && !values[4])return 87;
        if(code==perflib::rpc::reconcile_query && values[1])return 87;
        auto& perf=perflib_state();std::size_t pending=0,records=perf.completed_requests.size();
        const auto duplicate=[&](PerflibState::RemoteRequest& remote){
            if(remote.code!=code || remote.request!=values)return 87U;
            auto channel=std::make_shared<perflib::rpc::Channel>(payload,values[0],values[1]);payload=-1;
            if(channel->identity()!=remote.payload_identity)return 13U;
            remote.replied=false;return 0U; // 使用既有結果，不重複 provider 回呼。
        };
        if(const auto cached=perf.completed_requests.find({caller,birth,sequence});cached!=perf.completed_requests.end())return duplicate(*cached->second);
        for(auto& [id,operation]:perf.operations){
            (void)id;if(!operation.remote)continue;auto& remote=*operation.remote;
            ++records;
            if(remote.caller==caller && remote.birth==birth && remote.sequence==sequence)return duplicate(remote);
            if(!remote.finished)++pending;
        }
        const auto busy=[&]{
            // 拒絕接受前即回覆專用標記，不建立去重紀錄、不執行回呼，也不變更 ledger。
            (void)complete_remote_family_request(caller,sequence,birth,170,{0,0,0,perflib::rpc::unaccepted_busy,0},0);
            return 0U; // 回覆已交給 transport；不是客體 API 的成功結果。
        };
        // 接受前保留未來完成紀錄的名額；快取滿載時不得丟棄尚在去重期限內的結果。
        if(pending>=64 || perf.operations.size()>=1024 || records>=PerflibState::maximum_remote_records)return busy();
        if(!perf.next_operation || perf.next_operation==UINT64_MAX)return 8;
        const auto found=perf.providers.find(values[3]);
        const bool live=found!=perf.providers.end() && !found->second.stopping && found->second.publication &&
            found->second.publication->publication()==values[2];
        if(code==perflib::rpc::probe_endpoint || (code==perflib::rpc::reconcile_query && !live)){
            if((code==perflib::rpc::probe_endpoint && values[0]!=perflib::rpc::header_size) || values[1])return 87;
            const auto shared_token=SharedToken::inherit(token);const auto caller_token=shared_token->read();
            if(code==perflib::rpc::probe_endpoint && caller_token.type==2 && caller_token.level<2)return 1346;
            auto channel=std::make_shared<perflib::rpc::Channel>(payload,values[0],values[1]);payload=-1;
            const auto identifiers=perflib::rpc::parse_request(channel->read(),code);
            if(code==perflib::rpc::probe_endpoint && !identifiers.empty())return 87;
            if(live)(void)security::object_access(caller_token,security::parse_relative_descriptor(found->second.security),1,{1,2,4,7});
            PerflibState::Operation operation;operation.action=PerflibState::Action::Probe;
            operation.remote=std::make_shared<PerflibState::RemoteRequest>();auto& remote=*operation.remote;
            remote.caller=caller;remote.birth=birth;remote.sequence=sequence;remote.code=code;remote.request=values;
            remote.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);remote.payload_identity=channel->identity();
            remote.result={0,0,0,live?perflib::rpc::endpoint_live:perflib::rpc::endpoint_retired,0};remote.finished=true;
            const auto id=perf.next_operation++;perf.operations.emplace(id,std::move(operation));
            // 只觀察 endpoint，不改 ledger、不 pin provider，也不建立客體 callback worker。
            remote.replied=!complete_remote_family_request(caller,sequence,birth,0,remote.result,0);return 0;
        }
        if(!live)return 1168;
        auto& provider=found->second;auto shared_token=SharedToken::inherit(token);const auto caller_token=shared_token->read();
        if(!perflib::rpc::removing(code) && code!=perflib::rpc::close_query && code!=perflib::rpc::reconcile_query){
            if(caller_token.type==2 && caller_token.level<2)return 1346;
            (void)security::object_access(caller_token,security::parse_relative_descriptor(provider.security),1,{1,2,4,7});
        }
        auto channel=std::make_shared<perflib::rpc::Channel>(payload,values[0],values[1]);payload=-1;
        const auto request_bytes=channel->read();auto identifiers=perflib::rpc::parse_request(request_bytes,code);
        for(const auto& spec:identifiers){
            if(perflib::rpc::subscribing(code) || perflib::rpc::removing(code) || code==perflib::rpc::reconcile_query)continue;
            const auto set=provider.sets.find(spec.set);if(set==provider.sets.end())return 1168;
            if(code==perflib::rpc::collect){if(const auto error=validate_identifier(spec,set->second))return error;}
            else if(code==perflib::rpc::enumerate && (spec.counter!=perflib::wildcard || spec.instance!=perflib::wildcard || !spec.name.empty()))return 87;
        }
        const auto id=perf.next_operation++;
        PerflibState::Operation operation;
        operation.action=perflib::rpc::subscribing(code)?PerflibState::Action::Subscribe:perflib::rpc::removing(code)?PerflibState::Action::Unsubscribe:
            code==perflib::rpc::close_query || code==perflib::rpc::reconcile_query?PerflibState::Action::Cleanup:
            code==perflib::rpc::collect?PerflibState::Action::Collect:PerflibState::Action::Enumerate;
        operation.provider=values[3];operation.pinned={values[3]};if(!identifiers.empty())operation.set=identifiers.front().set;operation.identifiers=std::move(identifiers);
        if(code==perflib::rpc::undo_counter)operation.undo_sequence=perflib::Reader{request_bytes}.integer(24,8);
        if(code==perflib::rpc::enumerate || code==perflib::rpc::collect)operation.notifications.push_back({values[3],code,{},0});
        operation.remote=std::make_shared<PerflibState::RemoteRequest>();
        auto& remote=*operation.remote;remote.caller=caller;remote.birth=birth;remote.sequence=sequence;remote.code=code;remote.request=values;
        remote.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);remote.payload_identity=channel->identity();remote.channel=std::move(channel);remote.token=std::move(shared_token);
        std::optional<PerflibState::Subscription> next;
        if(values[4] && code!=perflib::rpc::enumerate){
            operation.subscription=PerflibState::SubscriptionKey{caller,birth,values[4],values[3]};operation.reservation=code==perflib::rpc::close_query || code==perflib::rpc::reconcile_query?0:id;
            const auto ledger=perf.subscriptions.find(*operation.subscription);
            if(ledger!=perf.subscriptions.end() && ledger->second.retiring)return 995;
            if(ledger!=perf.subscriptions.end() && ledger->second.busy)return busy();
            if(code==perflib::rpc::close_query){
                if(ledger!=perf.subscriptions.end())for(const auto& counter:ledger->second.counters)if(counter.committed)
                    operation.notifications.push_back({values[3],2,counter.specification,0});
            }else if(code==perflib::rpc::reconcile_query){
                if(ledger!=perf.subscriptions.end())for(const auto& counter:ledger->second.counters)if(counter.committed &&
                    (counter.cancelled || std::none_of(operation.identifiers.begin(),operation.identifiers.end(),[&](const auto& retained){return counter.specification.same_query(retained);}))){
                    operation.notifications.push_back({values[3],2,counter.specification,0});
                }
            }else if(code==perflib::rpc::collect){
                if(ledger==perf.subscriptions.end())return 1168;
                for(const auto& spec:operation.identifiers)if(std::none_of(ledger->second.counters.begin(),ledger->second.counters.end(),[&](const auto& counter){return counter.committed && !counter.cancelled && counter.specification.same_query(spec);}))return 1168;
            }else{
                next=ledger==perf.subscriptions.end()?PerflibState::Subscription{}:ledger->second;
                if(perflib::rpc::subscribing(code)){
                    std::size_t total=0;for(const auto& [key,value]:perf.subscriptions){(void)key;total+=value.counters.size();}
                    for(const auto& spec:operation.identifiers)if(std::none_of(next->counters.begin(),next->counters.end(),[&](const auto& counter){return counter.specification.same_query(spec);})){
                        if(total>=65536)throw perflib::Error(8);next->counters.push_back({spec,id,sequence,false,false});++total;
                    }
                }
            }
        }
        bool created=false;
        if(operation.subscription && !perf.subscriptions.contains(*operation.subscription)){
            if(perf.subscriptions.size()>=1024)return 8;
            created=perf.subscriptions.emplace(*operation.subscription,PerflibState::Subscription{}).second;
        }
        const auto key=operation.subscription;
        try{perf.cleanup.push_back(id);try{perf.operations.emplace(id,std::move(operation));}catch(...){perf.cleanup.pop_back();throw;}}
        catch(...){if(created)perf.subscriptions.erase(*key);throw;}
        if(key){auto& ledger=perf.subscriptions.at(*key);if(next)ledger.counters.swap(next->counters);ledger.busy=true;perf.operations.at(id).subscription_active=true;}
        ++provider.active;return 0;
    }catch(const perflib::Error& error){return error.code;}
     catch(const SecurityDescriptorError& error){return error.code;}
     catch(const std::bad_alloc&){return 8;}
}
void WindowsServices::cancel_perflib_remote(std::uint32_t caller,std::uint64_t birth,std::uint64_t sequence){
    if(!perflib_)return;
    // 完成紀錄已過期時，仍能撤銷這個 Add 序號留下的訂閱。
    for(auto& [key,ledger]:perflib_->subscriptions)if(std::get<0>(key)==caller && std::get<1>(key)==birth)
        for(auto& counter:ledger.counters)if(counter.sequence==sequence)counter.cancelled=true;
    if(const auto cached=perflib_->completed_requests.find({caller,birth,sequence});cached!=perflib_->completed_requests.end()){
        auto& remote=*cached->second;
        if(perflib::rpc::subscribing(remote.code)){remote.cancelled=true;remote.error=995;remote.result[1]=0;remote.replied=false;}
        return;
    }
    for(auto& [id,operation]:perflib_->operations){(void)id;if(!operation.remote)continue;auto& remote=*operation.remote;
        if(remote.caller==caller && remote.birth==birth && remote.sequence==sequence){
            if(!remote.finished){remote.cancelled=true;remote.error=995;}
            else if(perflib::rpc::subscribing(remote.code)){remote.cancelled=true;remote.error=995;remote.result[1]=0;remote.replied=false;}
            return;
        }}
}
bool WindowsServices::perflib_remote_waiting()const{
    if(!perflib_)return false;
    for(const auto& [id,operation]:perflib_->operations){(void)id;if(operation.needs_cleanup)return true;
        if(operation.consumer && std::any_of(operation.consumer->targets.begin(),operation.consumer->targets.end(),
            [](const auto& target){return target.retry_pending;}))return true;
    }
    if(!runtime_.network.external_io)return false;
    if(perflib_->endpoint_listener && handle_transfers_)return true;
    for(const auto& [key,remote]:perflib_->completed_requests){(void)key;if(!remote->replied)return true;}
    for(const auto& [handle,provider]:perflib_->providers){(void)handle;if(!provider.stopping && provider.publication)return true;}
    for(const auto& [id,operation]:perflib_->operations){(void)id;if(operation.remote && (!operation.remote->finished || !operation.remote->replied))return true;}
    return false;
}
bool WindowsServices::perflib_consumer_ready(std::uint64_t id)const{
    if(!perflib_)return true;const auto found=perflib_->operations.find(id);
    if(found==perflib_->operations.end() || !found->second.consumer)return true;
    const auto& consumer=*found->second.consumer;
    if(consumer.batched && consumer.batch_code==perflib::rpc::collect &&
       std::any_of(consumer.targets.begin(),consumer.targets.end(),[](const auto& target){return target.ready && !target.consumed;}))return true;
    return std::all_of(consumer.targets.begin(),consumer.targets.end(),[](const auto& target){return target.ready;});
}
void WindowsServices::release_perflib_subscription(std::uint64_t id){
    auto& perf=perflib_state();auto& operation=perf.operations.at(id);
    if(!operation.subscription || !operation.subscription_active)return;operation.subscription_active=false;
    const auto found=perf.subscriptions.find(*operation.subscription);if(found==perf.subscriptions.end())return;auto& ledger=found->second;
    const bool commit=operation.action==PerflibState::Action::Subscribe && operation.remote && !operation.remote->error && !operation.remote->cancelled;
    if(commit)for(auto& counter:ledger.counters)if(counter.reservation==operation.reservation && counter.committed)counter.reservation=0;
    if(operation.reservation)std::erase_if(ledger.counters,[&](const auto& counter){return counter.reservation==operation.reservation;});
    ledger.busy=false;if(ledger.counters.empty())perf.subscriptions.erase(found);
}
std::uint64_t WindowsServices::finish_perflib_remote(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint32_t status){
    auto& perf=perflib_state();auto& operation=perf.operations.at(id);auto& remote=*operation.remote;const auto worker=operation.worker;
    remote.error=status?status:remote.cancelled?(remote.error?remote.error:995U):0U;
    remote.result={operation.bytes.size(),0,operation.identifiers.size(),0,0};
    if(remote.code==perflib::rpc::reconcile_query){remote.result[2]=0;remote.result[3]=perflib::rpc::endpoint_live;}
    // Collect 的第 4 個 accounting 值是 provider BEGIN 回呼結果；與「沒有符合的 instance」分開。
    if(remote.code==perflib::rpc::collect)if(const auto begin=operation.collection_begin_status.find(operation.provider);begin!=operation.collection_begin_status.end())remote.result[3]=begin->second;
    if(remote.code==perflib::rpc::ensure_counter && operation.subscription){
        const auto ledger=perf.subscriptions.find(*operation.subscription);
        if(ledger!=perf.subscriptions.end())remote.result[4]=std::any_of(ledger->second.counters.begin(),ledger->second.counters.end(),
            [&](const auto& counter){return counter.reservation==operation.reservation && counter.committed;});
    }
    if(!remote.error){
        try{if(!remote.response_written)remote.error=remote.channel->write(operation.bytes);if(!remote.error)remote.result[1]=operation.bytes.size();}
        catch(const perflib::Error& error){remote.error=error.code;}
    }
    release_perflib_subscription(id);
    if(operation.scratch)(void)free_heap(memory,operation.scratch,state);operation.scratch=0;
    for(auto handle:operation.pinned)if(const auto provider=perf.providers.find(handle);provider!=perf.providers.end() && provider->second.active)--provider->second.active;
    operation.pinned.clear();operation.bytes=perflib::Bytes{};operation.identifiers=std::vector<perflib::Identifier>{};
    operation.notifications=std::vector<PerflibState::Notification>{};operation.collection_status.clear();operation.collection_begin_status.clear();
    operation.worker=operation.owner=0;remote.finished=true;remote.channel.reset();remote.token.reset();
    remote.replied=!complete_remote_family_request(remote.caller,remote.sequence,remote.birth,remote.error,remote.result,0);
    // 保留至原期限以去除重複封包；worker 已完成，不再持有 provider 或資料檔。
    return worker;
}

#include "perflib_consumer_control.inc"
#include "perflib_consumer_data.inc"

std::optional<std::uint32_t> WindowsServices::invoke_perflib(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned p=x86_?4:8;const auto bits=static_cast<std::uint8_t>(p*8);
    const bool pointer_result=name=="PerfCreateInstance" || name=="PerfQueryInstance";
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t code){if(pointer_result){last_error_=code;return finish(0);}return finish(code);};
    try{
        auto& perf=perflib_state();
        for(const auto& [id,operation]:perf.operations)if(operation.consumer && operation.owner==threads_.current &&
            operation.awaiting==PerflibState::Await::None && operation.consumer->stack==state.reg(Gpr::Rsp)){
            if(operation.consumer->symbol!=name)throw HostCallFault("Perflib consumer 與客體呼叫框架不一致");
            return advance_perflib(state,memory,id);
        }
        const auto begin=[&](PerflibState::Operation operation){
            const auto consumer=operation.consumer;operation.owner=threads_.current;std::uint64_t id{};
            try{
                // 後續批次與忙碌重試可由 poll 送出，必須沿用此客體呼叫的身分。
                if(consumer && runtime_.network.external_io && !consumer->token){initialize_tokens();consumer->token=share_token(effective_token_id());}
                if(!perf.next_operation || perf.next_operation==UINT64_MAX || perf.operations.size()>=1024)throw perflib::Error(8);id=perf.next_operation++;
                perf.operations.emplace(id,std::move(operation));
            }catch(...){if(consumer)for(const auto& target:consumer->targets)cancel_remote_family_request(target.provider_pid,target.sequence);throw;}
            const auto& saved=perf.operations.at(id);
            if(saved.query)++perf.queries.at(saved.query).active;
            for(auto provider:saved.pinned)++perf.providers.at(provider).active;
            return advance_perflib(state,memory,id);
        };
        const auto provider=[&](std::uint64_t handle)->PerflibState::Provider&{
            const auto found=perf.providers.find(handle);if(found==perf.providers.end() || found->second.stopping)throw perflib::Error(6);return found->second;
        };
        const auto query=[&](std::uint64_t handle)->PerflibState::Query&{
            const auto found=perf.queries.find(handle);if(found==perf.queries.end())throw perflib::Error(6);if(found->second.active)throw perflib::Error(170);return found->second;
        };
        const auto registrations=[&]{
            std::map<perflib::Guid,perflib::CounterSet> result;
            for(const auto& [handle,value]:perf.providers){(void)handle;if(value.stopping)continue;for(const auto& [id,set]:value.sets)result.emplace(id,set);}
            if(runtime_.network.external_io){
                if(!perf.catalog)perf.catalog=perflib::ProviderCatalog::open(mapping_session());
                if(perf.catalog)for(const auto& entry:perf.catalog->snapshot([&](const auto& sd){
                    try{(void)object_security_access(sd,1,{1,2,4,7});return true;}catch(const SecurityDescriptorError&){return false;}
                })){
                    for(const auto& [id,set]:entry.sets){
                        const auto found=result.find(id);
                        if(found!=result.end()){
                            // 登錄 ABI 不含客體 offset；跨 x86／x64 provider 不比對私人配置。
                            if(!perflib::registration_compatible(found->second,set))throw perflib::Error(13);
                        }else{if(result.size()>=65536)throw perflib::Error(8);result.emplace(id,set);}
                    }
                }
            }return result;
        };
        if(name=="PerfStartProvider" || name=="PerfStartProviderEx"){
            if(!arg(2))return fail(87);memory.check(GuestAddress{arg(2)},p,Access::Write);memory.store(GuestAddress{arg(2)},bits,0);
            PerflibState::Provider value;value.id=read_guid(memory,arg(0));
            if(runtime_.network.external_io)value.security=object_security_descriptor({},true);
            if(name=="PerfStartProvider")value.callback=arg(1);
            else if(arg(1)){
                const auto context=read_bytes(memory,arg(1),8+4*p);const perflib::Reader reader{context};
                if(reader.u32(0)!=context.size() || reader.u32(4))return fail(87);
                value.callback=reader.integer(8,p);value.allocate=reader.integer(8+p,p);value.release=reader.integer(8+2*p,p);value.context=reader.integer(8+3*p,p);
                if(bool(value.allocate)!=bool(value.release))return fail(87);
            }
            for(auto target:{value.callback,value.allocate,value.release})if(target && !handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
            if(perf.providers.size()>=1024)return fail(8);
            for(const auto& [handle,current]:perf.providers){(void)handle;if(current.id==value.id)return fail(183);}
            const auto handle=perf.next_handle++;perf.providers.emplace(handle,std::move(value));memory.store(GuestAddress{arg(2)},bits,handle);return finish(0);
        }
        if(name=="PerfSetCounterSetInfo"){
            auto& current=provider(arg(0));if(!arg(1))return fail(87);
            const auto bytes=read_bytes(memory,arg(1),static_cast<std::uint32_t>(arg(2)));auto set=perflib::counter_set(bytes,p);
            if(set.provider!=current.id)return fail(87);
            if(perf.provider_for(set.id))return fail(183);if(current.sets.size()>=1024)return fail(8);
            auto next=current.sets;next.emplace(set.id,std::move(set));
            if(runtime_.network.external_io){
                if(!perf.catalog)perf.catalog=perflib::ProviderCatalog::open(mapping_session());
                if(perf.catalog){
                    if(current.publication)current.publication->update(next);
                    else current.publication=perf.catalog->publish(arg(0),current.id,p,next,current.security);
                }
            }
            current.sets.swap(next);if(current.publication)perf.endpoint_listener=true;return finish(0);
        }
        if(name=="PerfCreateInstance" || name=="PerfQueryInstance"){
            auto& current=provider(arg(0));const auto id=read_guid(memory,arg(1));const auto found=current.sets.find(id);if(found==current.sets.end())return fail(1168);
            const auto instance_name=read_name(memory,arg(2));const auto instance_id=static_cast<std::uint32_t>(arg(3));
            for(const auto& [address,item]:current.instances)if(item.set==id && ((name=="PerfCreateInstance" && !found->second.multi()) || (item.name==instance_name && item.id==instance_id)))
                return name=="PerfQueryInstance"?finish(address):fail(183);
            if(name=="PerfQueryInstance")return fail(1168);if(current.instances.size()>=65536)return fail(8);
            PerflibState::Operation operation;operation.action=PerflibState::Action::Create;operation.provider=arg(0);operation.pinned={arg(0)};
            operation.set=id;operation.name=instance_name;operation.instance_id=instance_id;operation.bytes=perflib::instance(found->second,instance_name,instance_id);return begin(std::move(operation));
        }
        if(name=="PerfDeleteInstance" || name=="PerfStopProvider"){
            auto& current=provider(arg(0));const bool stop=name=="PerfStopProvider";if(stop && current.active)return fail(170);
            PerflibState::Operation operation;operation.action=stop?PerflibState::Action::Stop:PerflibState::Action::Delete;operation.provider=arg(0);operation.pinned={arg(0)};
            if(stop){for(const auto& [address,item]:current.instances)operation.releases.push_back({address,current.release,current.context,item.custom});}
            else{const auto found=current.instances.find(arg(1));if(found==current.instances.end())return fail(87);operation.releases.push_back({arg(1),current.release,current.context,found->second.custom});}
            return begin(std::move(operation));
        }
        if(name=="PerfSetCounterRefValue" || name.starts_with("PerfSetULong") || name.starts_with("PerfIncrementULong") || name.starts_with("PerfDecrementULong")){
            auto& current=provider(arg(0));const auto instance=current.instances.find(arg(1));if(instance==current.instances.end())return fail(87);
            const auto& set=current.sets.at(instance->second.set);const auto counter=std::find_if(set.counters.begin(),set.counters.end(),[&](const auto& item){return item.id==static_cast<std::uint32_t>(arg(2));});if(counter==set.counters.end())return fail(1168);
            const auto storage=arg(1)+counter->offset;
            if(name=="PerfSetCounterRefValue"){
                if(!counter->reference())return fail(87);memory.store(GuestAddress{storage},static_cast<std::uint8_t>(bits),arg(3));return finish(0);
            }
            const unsigned width=name.find("ULongLong")!=name.npos?8:4;if(counter->size!=width)return fail(87);
            const auto address=counter->reference()?memory.load(GuestAddress{storage},static_cast<std::uint8_t>(bits)):storage;if(!address)return fail(87);
            const auto previous=name.starts_with("PerfSet")?0:memory.load(GuestAddress{address},static_cast<std::uint8_t>(width*8));
            const auto value=name.starts_with("PerfDecrement")?previous-arg(3):previous+arg(3);
            memory.store(GuestAddress{address},static_cast<std::uint8_t>(width*8),value);return finish(0);
        }
        if(name=="PerfOpenQueryHandle"){
            if(!arg(1))return fail(87);memory.check(GuestAddress{arg(1)},p,Access::Write);memory.store(GuestAddress{arg(1)},bits,0);
            if(!local_machine(read_name(memory,arg(0),true),perf.machine))return fail(53);
            if(perf.queries.size()>=1024)return fail(8);const auto handle=perf.next_handle++;perf.queries.emplace(handle,PerflibState::Query{});memory.store(GuestAddress{arg(1)},bits,handle);return finish(0);
        }
        if(name=="PerfAddCounters" || name=="PerfDeleteCounters" || name=="PerfCloseQueryHandle"){
            auto& current=query(arg(0));PerflibState::Operation operation;operation.query=arg(0);
            operation.control=std::make_shared<PerflibState::Control>();operation.consumer=std::make_shared<PerflibState::RemoteConsumer>();
            operation.consumer->symbol=std::string(name);operation.consumer->stack=state.reg(Gpr::Rsp);
            if(name=="PerfCloseQueryHandle"){operation.action=PerflibState::Action::Close;operation.identifiers=current.counters;}
            else{operation.action=name=="PerfAddCounters"?PerflibState::Action::Add:PerflibState::Action::Remove;operation.output=arg(1);
                const auto size=static_cast<std::uint32_t>(arg(2));if(!size || !arg(1))return fail(87);memory.check(GuestAddress{arg(1)},size,Access::Write);operation.identifiers=perflib::identifiers(read_bytes(memory,arg(1),size));
                if(name=="PerfAddCounters" && std::any_of(operation.identifiers.begin(),operation.identifiers.end(),[&](const auto& spec){
                    const auto schema=current.known_schemas.find(spec.set);
                    return schema!=current.known_schemas.end() && !validate_identifier(spec,*schema->second.registration) &&
                        std::none_of(current.counters.begin(),current.counters.end(),[&](const auto& counter){return counter.same_query(spec);});
                }))operation.control->phase=PerflibState::Control::Phase::Probe;
                if(name=="PerfAddCounters" && runtime_.network.external_io){
                    if(!perf.catalog)perf.catalog=perflib::ProviderCatalog::open(mapping_session());
                    if(perf.catalog)operation.control->candidates=perf.catalog->snapshot([&](const auto& sd){
                        try{(void)object_security_access(sd,1,{1,2,4,7});return true;}catch(const SecurityDescriptorError&){return false;}
                    });
#if !defined(_WIN32)
                    std::erase_if(operation.control->candidates,[](const auto& entry){return entry.origin.pid==static_cast<std::uint32_t>(::getpid());});
#endif
                }
            }
            return begin(std::move(operation));
        }
        if(name=="PerfQueryCounterInfo"){
            auto& current=query(arg(0));perflib::Bytes bytes;for(std::size_t i=0;i<current.counters.size();++i){auto spec=current.counters[i];spec.index=static_cast<std::uint32_t>(i);spec.status=0;perflib::append(bytes,perflib::identifier(spec));}
            return finish(copy_output(memory,arg(1),static_cast<std::uint32_t>(arg(2)),arg(3),bytes));
        }
        if(name=="PerfQueryCounterData"){
            auto& current=query(arg(0));if(!arg(3) || (arg(2) && !arg(1)) || arg(2)>maximum_data)return fail(87);
            memory.check(GuestAddress{arg(3)},4,Access::Write);if(arg(2))memory.check(GuestAddress{arg(1)},static_cast<std::uint32_t>(arg(2)),Access::Write);
            PerflibState::Operation operation;operation.action=PerflibState::Action::Collect;operation.query=arg(0);operation.identifiers=current.counters;operation.output=arg(1);operation.capacity=static_cast<std::uint32_t>(arg(2));operation.actual=arg(3);
            auto consumer=std::make_shared<PerflibState::RemoteConsumer>();consumer->symbol=std::string(name);consumer->stack=state.reg(Gpr::Rsp);
            operation.control=std::make_shared<PerflibState::Control>();operation.control->refresh=true;operation.control->phase=PerflibState::Control::Phase::Probe;
            if(runtime_.network.external_io){
                if(!perf.catalog)perf.catalog=perflib::ProviderCatalog::open(mapping_session());
                if(perf.catalog)operation.control->candidates=perf.catalog->snapshot([&](const auto& sd){
                    try{(void)object_security_access(sd,1,{1,2,4,7});return true;}catch(const SecurityDescriptorError&){return false;}
                });
#if !defined(_WIN32)
                std::erase_if(operation.control->candidates,[](const auto& entry){return entry.origin.pid==static_cast<std::uint32_t>(::getpid());});
#endif
            }
            // 即使本輪只有本地 provider，也保留 consumer continuation，以平衡異常離開的 BEGIN／END。
            operation.consumer=std::move(consumer);
            return begin(std::move(operation));
        }
        if(name=="PerfEnumerateCounterSet" || name=="PerfEnumerateCounterSetInstances" || name=="PerfQueryCounterSetRegistrationInfo"){
            if(!local_machine(read_name(memory,arg(0),true),perf.machine))return fail(53);
            if(name=="PerfEnumerateCounterSet"){
                if(!arg(3) || (arg(2) && !arg(1)))return fail(87);const auto sets=registrations();
                memory.store(GuestAddress{arg(3)},32,sets.size());if(arg(2)<sets.size())return fail(8);
                if(!sets.empty())memory.check(GuestAddress{arg(1)},sets.size()*16,Access::Write);
                std::uint64_t at=arg(1);for(const auto& [id,set]:sets){(void)set;memory.write(GuestAddress{at},id);at+=16;}return finish(0);
            }
            const auto id=read_guid(memory,arg(1));const auto handle=perf.provider_for(id);
            if(name=="PerfEnumerateCounterSetInstances"){
                if(!arg(4) || (arg(3) && !arg(2)) || arg(3)>maximum_data)return fail(87);
                memory.check(GuestAddress{arg(4)},4,Access::Write);if(arg(3))memory.check(GuestAddress{arg(2)},static_cast<std::uint32_t>(arg(3)),Access::Write);
                PerflibState::Operation operation;operation.action=PerflibState::Action::Enumerate;operation.provider=handle;operation.set=id;
                operation.output=arg(2);operation.capacity=static_cast<std::uint32_t>(arg(3));operation.actual=arg(4);
                if(handle){operation.pinned={handle};operation.notifications.push_back({handle,3,{},0});}
                auto consumer=std::make_shared<PerflibState::RemoteConsumer>();consumer->symbol=std::string(name);consumer->stack=state.reg(Gpr::Rsp);
                std::optional<perflib::CounterSet> registration;if(handle)registration=perf.providers.at(handle).sets.at(id);
                try{
                    if(runtime_.network.external_io){
                        if(!perf.catalog)perf.catalog=perflib::ProviderCatalog::open(mapping_session());
                        if(perf.catalog){
                            const auto entries=perf.catalog->snapshot([&](const auto& sd){
                                try{(void)object_security_access(sd,1,{1,2,4,7});return true;}catch(const SecurityDescriptorError&){return false;}
                            });
                            perflib::Identifier specification;specification.set=id;specification.size=40;
                            const auto request=perflib::rpc::request(perflib::rpc::enumerate,std::span{&specification,1});
                            for(const auto& entry:entries){
                                if(!entry.sets.contains(id))continue;
#if !defined(_WIN32)
                                if(entry.origin.pid==static_cast<std::uint32_t>(::getpid()))continue;
#endif
                                if(registration){if(!perflib::registration_compatible(*registration,entry.sets.at(id)))throw perflib::Error(13);}
                                else registration=entry.sets.at(id);
                                if(consumer->targets.size()>=64)throw perflib::Error(8);
                                auto channel=perflib::rpc::Channel::create(mapping_session(),request,operation.capacity);
                                if(!consumer->token){initialize_tokens();consumer->token=share_token(effective_token_id());}
                                consumer->targets.push_back({});auto& target=consumer->targets.back();
                                target.provider_pid=entry.origin.pid;target.channel=std::move(channel);
                                target.endpoint={entry.origin,entry.publication,entry.handle};target.specifications.push_back(specification);
                                target.request_code=perflib::rpc::enumerate;target.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(5000);
                                const std::array<std::uint64_t,5> values{request.size(),operation.capacity,entry.publication,entry.handle,0};
                                if(const auto error=begin_remote_family_request(entry.origin.pid,entry.origin.birth,perflib::rpc::family,perflib::rpc::enumerate,values,0,target.channel->descriptor(),consumer->token->descriptor(),target.sequence))throw perflib::Error(error);
                            }
                        }
                    }
                    if(!handle && consumer->targets.empty())return fail(1168);
                    if(!consumer->targets.empty())operation.consumer=consumer;
                    return begin(std::move(operation));
                }catch(...){for(const auto& target:consumer->targets)cancel_remote_family_request(target.provider_pid,target.sequence);throw;}
            }
            const auto sets=registrations();const auto found=sets.find(id);if(found==sets.end())return fail(1168);const auto& set=found->second;
            perflib::Bytes data;const auto code=static_cast<std::uint32_t>(arg(2));
            if(code==1)data=perflib::set_registration(set);
            else if(code==2){const auto counter=std::find_if(set.counters.begin(),set.counters.end(),[&](const auto& item){return item.id==static_cast<std::uint32_t>(arg(3));});if(counter==set.counters.end())return fail(1168);data=perflib::counter_registration(*counter);}
            else if(code==8)data.assign(set.provider.begin(),set.provider.end());
            else if(code>=3 && code<=10)return fail(1168);else return fail(87);
            return finish(copy_output(memory,arg(4),static_cast<std::uint32_t>(arg(5)),arg(6),data));
        }
        throw HostCallFault("未知 Perflib 入口");
    }catch(const perflib::Error& error){return fail(error.code);}catch(const SecurityDescriptorError& error){return fail(error.code);}catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(8);}
}

std::optional<std::uint32_t> WindowsServices::advance_perflib(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& perf=perflib_state();const auto p=x86_?4U:8U;
    const auto done=[&](std::uint64_t value){
        const auto found=perf.operations.find(id);if(found==perf.operations.end())throw HostCallFault("Perflib operation 已消失");
        if(found->second.remote){
            const auto worker=finish_perflib_remote(state,memory,id,static_cast<std::uint32_t>(value));
            if(worker)return resume_registered_wait(state,memory);return std::optional<std::uint32_t>{};
        }
        release_perflib_subscription(id);
        const auto operation=std::move(found->second);perf.operations.erase(found);
        if(operation.consumer)for(const auto& target:operation.consumer->targets)cancel_remote_family_request(target.provider_pid,target.sequence);
        if(operation.scratch)(void)free_heap(memory,operation.scratch,state);
        for(auto handle:operation.pinned)if(const auto provider=perf.providers.find(handle);provider!=perf.providers.end() && provider->second.active)--provider->second.active;
        if(operation.query)if(const auto query=perf.queries.find(operation.query);query!=perf.queries.end() && query->second.active)--query->second.active;
        if(operation.action==PerflibState::Action::Stop || operation.retire_provider)perf.providers.erase(operation.provider);
        if(operation.action==PerflibState::Action::Close && !value)perf.queries.erase(operation.query);
        if(operation.worker)return resume_registered_wait(state,memory);
        complete_call(state,memory,value);return std::optional<std::uint32_t>{};
    };
    const auto callback=[&](std::uint64_t target,std::vector<std::uint64_t> arguments,PerflibState::Await awaiting){
        perf.operations.at(id).awaiting=awaiting;
        try{start_callbacks(state,memory,{{target,std::move(arguments)}},Continuation::PerflibOperation,id);}
        catch(...){auto& operation=perf.operations.at(id);operation.awaiting=PerflibState::Await::None;if(awaiting==PerflibState::Await::Release && operation.release_position)--operation.release_position;throw;}
        return std::optional<std::uint32_t>{};
    };
    const auto abort=[&](std::uint32_t code){
        const auto& operation=perf.operations.at(id);const bool pointer=operation.action==PerflibState::Action::Create;const auto worker=operation.worker;
        if(operation.remote)operation.remote->error=code;
        if(pointer)last_error_=code;discard_perflib(state,memory,id);
        if(worker)return resume_registered_wait(state,memory);complete_call(state,memory,pointer?0:code);return std::optional<std::uint32_t>{};
    };
    try{
        auto& operation=perf.operations.at(id);
        if(operation.control){
            if(!advance_perflib_control(state,memory,id))return std::nullopt;
            if(!operation.control->refresh || operation.status)return done(operation.status);
            prepare_perflib_collection(perf,operation);operation.control.reset();
        }
        if(operation.remote && !operation.remote->finished && operation.action!=PerflibState::Action::Cleanup){
            auto& remote=*operation.remote;const auto gone=process_identity::query(remote.caller).gone({remote.caller,remote.birth});
            if(gone || std::chrono::steady_clock::now()>=remote.deadline){remote.cancelled=true;if(!remote.error)remote.error=gone?995U:121U;}
        }
        if(operation.remote && operation.remote->cancelled && operation.action!=PerflibState::Action::Cleanup){
            // 不在回呼執行途中釋放 scratch；回呼返回後才送出必要的 COLLECT_END。
            operation.action=PerflibState::Action::Cleanup;operation.status=operation.remote->error?operation.remote->error:995U;
            operation.notifications.clear();operation.notification=0;operation.bytes.clear();
            for(const auto& [handle,status]:operation.collection_status)if(!status)operation.notifications.push_back({handle,6,{},0});
            if(operation.subscription && operation.reservation){
                const auto ledger=perf.subscriptions.find(*operation.subscription);
                if(ledger!=perf.subscriptions.end())for(const auto& counter:ledger->second.counters)if(counter.reservation==operation.reservation && counter.committed)
                    operation.notifications.push_back({operation.provider,2,counter.specification,0});
            }
        }
        if(operation.remote && operation.action!=PerflibState::Action::Cleanup && operation.action!=PerflibState::Action::Unsubscribe){
            const auto& provider=perf.providers.at(operation.provider);const auto token=operation.remote->token->read();
            if(token.type==2 && token.level<2)throw perflib::Error(1346);
            (void)security::object_access(token,security::parse_relative_descriptor(provider.security),1,{1,2,4,7});
        }
        if(operation.action==PerflibState::Action::Create){
            auto& provider=perf.providers.at(operation.provider);
            if(!operation.allocated && provider.allocate)return callback(provider.allocate,{operation.bytes.size(),provider.context},PerflibState::Await::Allocate);
            const auto address=operation.allocated?operation.output:allocate_heap(memory,operation.bytes.size());
            if(!address){last_error_=8;return done(0);}
            try{
                if(provider.instances.contains(address)){operation.output=0;throw perflib::Error(87);}
                const auto& set=provider.sets.at(operation.set);
                for(const auto& [existing,item]:provider.instances){(void)existing;if(item.set==operation.set && (!set.multi() || (item.name==operation.name && item.id==operation.instance_id)))throw perflib::Error(183);}
                memory.write(GuestAddress{address},operation.bytes);provider.instances.emplace(address,PerflibState::Instance{operation.set,operation.name,operation.instance_id,operation.bytes.size(),provider.allocate!=0});
            }
            catch(...){if(!provider.allocate)(void)free_heap(memory,address,state);throw;}
            return done(address);
        }
        if((operation.action==PerflibState::Action::Stop || operation.action==PerflibState::Action::Delete) && !operation.retired){
            auto& provider=perf.providers.at(operation.provider);if(operation.action==PerflibState::Action::Stop){
                provider.stopping=true;provider.publication.reset();
                std::erase_if(perf.subscriptions,[&](const auto& entry){return std::get<3>(entry.first)==operation.provider;});
            }
            for(const auto& release:operation.releases)provider.instances.erase(release.address);operation.retired=true;
        }
        while(operation.release_position<operation.releases.size()){
            const auto release=operation.releases[operation.release_position++];
            if(release.custom && release.function)return callback(release.function,{release.address,release.context},PerflibState::Await::Release);
            if(!release.custom)(void)free_heap(memory,release.address,state);
        }
        if(operation.action==PerflibState::Action::Stop || operation.action==PerflibState::Action::Delete)return done(0);
        if(operation.action==PerflibState::Action::Subscribe || operation.action==PerflibState::Action::Unsubscribe){
            auto& ledger=perf.subscriptions.at(*operation.subscription);auto& provider=perf.providers.at(operation.provider);
            const bool add=operation.action==PerflibState::Action::Subscribe;
            while(operation.position<operation.identifiers.size()){
                auto& spec=operation.identifiers[operation.position];const auto set=provider.sets.find(spec.set);
                auto previous=std::find_if(ledger.counters.begin(),ledger.counters.end(),[&](const auto& counter){return counter.specification.same_query(spec) && (!operation.undo_sequence || counter.sequence==operation.undo_sequence);});
                spec.status=add?(set==provider.sets.end()?1168U:validate_identifier(spec,set->second)):
                    previous==ledger.counters.end() || !previous->committed?1168U:0U;
                if(add && !spec.status && previous!=ledger.counters.end() && previous->committed){
                    if(operation.remote->code==perflib::rpc::ensure_counter){
                        if(!previous->cancelled){++operation.position;continue;}
                        spec.status=170; // 先完成真正 Remove，再允許下一輪恢復 Add。
                    }else spec.status=183;
                }
                if(!spec.status){
                    if(provider.callback){
                        const auto bytes=perflib::notification(add?1:2,provider.id,&spec,perf.machine,runtime_.system_filetime());
                        operation.scratch=allocate_heap(memory,bytes.size());if(!operation.scratch)throw perflib::Error(8);memory.write(GuestAddress{operation.scratch},bytes);
                        return callback(provider.callback,{add?1U:2U,operation.scratch,bytes.size()},PerflibState::Await::Control);
                    }
                    if(add){if(previous==ledger.counters.end())throw perflib::Error(13);previous->committed=true;}
                    else ledger.counters.erase(previous);
                }
                ++operation.position;
            }
            for(const auto& spec:operation.identifiers)perflib::append(operation.bytes,perflib::rpc::encoded_identifier(spec));
            if(add){if(const auto error=operation.remote->channel->write(operation.bytes))throw perflib::Error(error);operation.remote->response_written=true;}
            return done(0);
        }
        if(operation.action==PerflibState::Action::Collect && operation.consumer && !operation.consumer->submitted){
            operation.consumer->submitted=true;
            begin_perflib_batch(*operation.consumer,perflib::rpc::collect);
            schedule_perflib_batch(*operation.consumer,[&](std::size_t index,std::uint32_t code){(void)submit_perflib_consumer_target(id,index,code);});
        }
        while(operation.notification<operation.notifications.size()){
            const auto& notification=operation.notifications[operation.notification];const auto provider=perf.providers.find(notification.provider);
            if(provider==perf.providers.end() || !provider->second.callback){
                if(notification.code==2 && operation.subscription && notification.counter){
                    const auto ledger=perf.subscriptions.find(*operation.subscription);if(ledger!=perf.subscriptions.end())
                        std::erase_if(ledger->second.counters,[&](const auto& counter){return counter.specification.same_query(*notification.counter);});
                }
                ++operation.notification;continue;
            }
            const auto bytes=perflib::notification(notification.code,provider->second.id,notification.counter?&*notification.counter:nullptr,perf.machine,runtime_.system_filetime());
            operation.scratch=allocate_heap(memory,bytes.size());if(!operation.scratch)throw perflib::Error(8);memory.write(GuestAddress{operation.scratch},bytes);
            return callback(provider->second.callback,{notification.code,operation.scratch,bytes.size()},PerflibState::Await::Control);
        }
        if(operation.action==PerflibState::Action::Collect && !operation.collected){
            std::vector<perflib::Bytes> blocks;std::size_t total=48;
            const auto append_block=[&](perflib::Bytes bytes){
                if(bytes.size()>maximum_data-total)throw perflib::Error(8);total+=bytes.size();blocks.push_back(std::move(bytes));
            };
            for(const auto& spec:operation.identifiers){
                const auto handle=operation.remote?operation.provider:operation.query?local_query_provider(perf,perf.queries.at(operation.query),spec):perf.provider_for(spec.set);
                if(!handle){append_block(perflib::error_block(1168));continue;}
                const auto error=operation.collection_status.find(handle);if(error!=operation.collection_status.end() && error->second){append_block(perflib::error_block(error->second));continue;}
                const auto& provider=perf.providers.at(handle);const auto& set=provider.sets.at(spec.set);std::vector<std::uint32_t> counters;for(const auto& counter:set.counters)counters.push_back(counter.id);
                perflib::Bytes block;
                try{
                    if(set.instance_type!=0 && set.instance_type!=2)throw perflib::Error(50);
                    const auto instances=collect_instances(provider,set,spec,memory,p);
                    if(!set.multi() && instances.size()>1)throw perflib::Error(13);
                    block=perflib::query_block(spec,instances,counters,set.multi());
                }
                catch(const perflib::Error& failure){block=perflib::error_block(failure.code);}
                append_block(std::move(block));
            }
            operation.bytes=perflib::query_data(timestamp(runtime_),blocks);operation.collected=true;
            for(auto handle:operation.pinned)if(!operation.collection_status[handle])operation.notifications.push_back({handle,6,{},0});
            return advance_perflib(state,memory,id);
        }
        if(operation.action==PerflibState::Action::Collect && operation.consumer && !merge_perflib_consumer_data(id)){
            auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};
            thread.wait.kind=ThreadWaitKind::Perflib;thread.wait.handles={id};threads_.yield=true;notify_concurrency_changed();return std::nullopt;
        }
        if(operation.action==PerflibState::Action::Enumerate){
            if(operation.status)return done(operation.status);
            if(!operation.collected){
                if(operation.provider){const auto& provider=perf.providers.at(operation.provider);
                    for(const auto& [address,instance]:provider.instances){(void)address;if(instance.set==operation.set)perflib::append(operation.bytes,perflib::instance_header(instance.id,instance.name));}}
                operation.collected=true;
            }
            if(operation.consumer){
                if(!perflib_consumer_ready(id)){
                    auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};
                    thread.wait.kind=ThreadWaitKind::Perflib;thread.wait.handles={id};threads_.yield=true;notify_concurrency_changed();return std::nullopt;
                }
                std::size_t total=operation.bytes.size();bool too_small=false;
                for(const auto& target:operation.consumer->targets){
                    if(target.mode || target.result[3] || target.result[4] || target.result[0]>maximum_data)throw perflib::Error(13);
                    if(target.error && !(target.error==8 && target.result[0]>operation.capacity))return done(target.error);
                    if(target.result[2]!=1 || target.result[1]!=(target.error?0:target.result[0]))throw perflib::Error(13);
                    if(target.result[0]>maximum_data-total)throw perflib::Error(8);total+=static_cast<std::size_t>(target.result[0]);
                    too_small=too_small || target.error==8;
                }
                if(too_small || total>operation.capacity){memory.store(GuestAddress{operation.actual},32,total);return done(8);}
                for(const auto& target:operation.consumer->targets){
                    auto bytes=target.channel->read_output(target.result[1]);perflib::rpc::validate_instances(bytes);perflib::append(operation.bytes,bytes);
                }
            }
        }
        if(operation.action==PerflibState::Action::Cleanup)return done(operation.remote?operation.status:0);
        if(operation.remote)return done(0);
        return done(copy_output(memory,operation.output,operation.capacity,operation.actual,operation.bytes));
    }catch(const perflib::Error& error){return abort(error.code);}
     catch(const SecurityDescriptorError& error){return abort(error.code);}
     catch(const MemoryFault&){return abort(998);}
     catch(const std::bad_alloc&){return abort(8);}
}

std::optional<std::uint32_t> WindowsServices::resume_perflib(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t result){
    auto& perf=perflib_state();const auto found=perf.operations.find(id);if(found==perf.operations.end())throw HostCallFault("Perflib callback 缺少 operation");auto& operation=found->second;
    const auto awaiting=operation.awaiting;operation.awaiting=PerflibState::Await::None;
    if(operation.scratch){(void)free_heap(memory,operation.scratch,state);operation.scratch=0;}
    if(awaiting==PerflibState::Await::Allocate){operation.output=x86_?static_cast<std::uint32_t>(result):result;operation.allocated=true;}
    else if(awaiting==PerflibState::Await::Control){
        if(operation.control){operation.control->callback_result=static_cast<std::uint32_t>(result);operation.control->callback_complete=true;}
        else if(operation.action==PerflibState::Action::Subscribe || operation.action==PerflibState::Action::Unsubscribe){
            auto& spec=operation.identifiers.at(operation.position++);auto& ledger=perf.subscriptions.at(*operation.subscription);
            auto previous=std::find_if(ledger.counters.begin(),ledger.counters.end(),[&](const auto& counter){return counter.specification.same_query(spec) && (!operation.undo_sequence || counter.sequence==operation.undo_sequence);});
            spec.status=operation.action==PerflibState::Action::Subscribe?static_cast<std::uint32_t>(result):0U;
            if(!spec.status && previous!=ledger.counters.end()){
                if(operation.action==PerflibState::Action::Subscribe)previous->committed=true;else ledger.counters.erase(previous);
            }
        }
        else{const auto& notification=operation.notifications.at(operation.notification++);
            if(notification.code==5){operation.collection_status[notification.provider]=static_cast<std::uint32_t>(result);operation.collection_begin_status[notification.provider]=static_cast<std::uint32_t>(result);}
            else if(notification.code==6)operation.collection_status[notification.provider]=UINT32_MAX;
            else if(notification.code==3 && result)operation.status=static_cast<std::uint32_t>(result);
            else if(notification.code==2 && operation.subscription && notification.counter){
                const auto ledger=perf.subscriptions.find(*operation.subscription);if(ledger!=perf.subscriptions.end())
                    std::erase_if(ledger->second.counters,[&](const auto& counter){return counter.specification.same_query(*notification.counter);});
            }
        }
    }
    return advance_perflib(state,memory,id);
}

void WindowsServices::discard_perflib(GuestState& state,GuestMemory& memory,std::uint64_t id){
    if(!perflib_)return;auto& perf=*perflib_;const auto found=perf.operations.find(id);if(found==perf.operations.end())return;
    auto& operation=found->second;
    try{
        if(operation.scratch)(void)free_heap(memory,operation.scratch,state);operation.scratch=0;
        if(operation.consumer)for(const auto& target:operation.consumer->targets){
            cancel_remote_family_request(target.provider_pid,target.sequence);
            if(target.add_sequence)(void)queue_remote_family_cancel(target.endpoint.origin.pid,target.endpoint.origin.birth,perflib::rpc::family,target.add_sequence);
        }
        PerflibState::Operation cleanup;cleanup.action=PerflibState::Action::Cleanup;cleanup.owner=threads_.current;cleanup.provider=operation.provider;cleanup.retire_provider=operation.action==PerflibState::Action::Stop || operation.retire_provider;
        if(operation.control && (operation.action==PerflibState::Action::Add || operation.control->refresh) && operation.control->staged.local_provider &&
           (operation.control->local_added || (operation.control->phase==PerflibState::Control::Phase::Local && operation.awaiting==PerflibState::Await::Control)))
            cleanup.notifications.push_back({operation.control->staged.local_provider,2,operation.control->staged.specification,0});
        cleanup.remote=operation.remote;if(cleanup.remote)cleanup.status=cleanup.remote->error?cleanup.remote->error:995U;
        cleanup.subscription=operation.subscription;cleanup.subscription_active=operation.subscription_active;cleanup.reservation=operation.reservation;
        if(operation.subscription){
            const auto ledger=perf.subscriptions.find(*operation.subscription);
            if(ledger!=perf.subscriptions.end()){
                if(operation.action==PerflibState::Action::Subscribe && operation.awaiting==PerflibState::Await::Control && operation.position<operation.identifiers.size()){
                    for(auto& counter:ledger->second.counters)if(counter.reservation==operation.reservation && counter.specification.same_query(operation.identifiers[operation.position]))counter.committed=true;
                }
                if(operation.reservation)for(const auto& counter:ledger->second.counters)if(counter.reservation==operation.reservation && counter.committed)
                    cleanup.notifications.push_back({operation.provider,2,counter.specification,0});
            }
        }
        // 本地 query 補償亦可能在清理回呼中再次中止，不能因沒有遠端 ledger 而遺失尚未完成的 Remove。
        if(operation.action==PerflibState::Action::Cleanup && !operation.reservation)
            for(std::size_t i=operation.notification;i<operation.notifications.size();++i)if(operation.notifications[i].code==2)cleanup.notifications.push_back(operation.notifications[i]);
        std::set<std::uint64_t> unlock;
        // 已成功鎖定資料的 Provider 必須收到實際 COLLECT_END，不能在 callback 中止後永久保留鎖。
        for(const auto& [handle,status]:operation.collection_status)if(!status)unlock.insert(handle);
        if(operation.action==PerflibState::Action::Collect && operation.awaiting==PerflibState::Await::Control && operation.notification<operation.notifications.size()){
            const auto& pending=operation.notifications[operation.notification];if(pending.code==5 || pending.code==6)unlock.insert(pending.provider);
        }
        if(operation.action==PerflibState::Action::Cleanup)for(std::size_t i=operation.notification;i<operation.notifications.size();++i)if(operation.notifications[i].code==6)unlock.insert(operation.notifications[i].provider);
        for(auto handle:unlock)cleanup.notifications.push_back({handle,6,{},0});
        for(std::size_t i=operation.release_position;i<operation.releases.size();++i)cleanup.releases.push_back(operation.releases[i]);
        if(operation.action==PerflibState::Action::Create && operation.allocated && operation.output){const auto provider=perf.providers.find(operation.provider);if(provider!=perf.providers.end())cleanup.releases.push_back({operation.output,provider->second.release,provider->second.context,provider->second.allocate!=0});}
        if(cleanup.remote || cleanup.subscription || !cleanup.notifications.empty() || !cleanup.releases.empty()){
            // 沿用原 map 節點；清理內容配置成功前，不搬走原請求或失去 provider／query pin。
            cleanup.pinned=std::move(operation.pinned);cleanup.query=operation.query;
            found->second=std::move(cleanup);perf.cleanup.push_back(id);return;
        }
        for(auto handle:operation.pinned)if(const auto provider=perf.providers.find(handle);provider!=perf.providers.end() && provider->second.active)--provider->second.active;
        if(operation.query)if(const auto query=perf.queries.find(operation.query);query!=perf.queries.end() && query->second.active)--query->second.active;
        if(operation.pinned.size() && (operation.action==PerflibState::Action::Stop || operation.retire_provider))perf.providers.erase(operation.provider);
        perf.operations.erase(found);
    }catch(const std::bad_alloc&){
        // 回呼已中止，但原資料仍完整；等宿主能配置清理內容時，再排入新的 worker。
        operation.needs_cleanup=true;operation.owner=operation.worker=0;
    }
}

void WindowsServices::poll_perflib(GuestMemory& memory){
    if(!perflib_)return;auto& perf=*perflib_;const auto now=std::chrono::steady_clock::now();
    for(auto it=perf.completed_requests.begin();it!=perf.completed_requests.end();){
        auto& remote=*it->second;
        if(now>=remote.deadline || process_identity::query(remote.caller).gone({remote.caller,remote.birth})){
            it=perf.completed_requests.erase(it);continue;
        }
        if(!remote.replied)remote.replied=!complete_remote_family_request(remote.caller,remote.sequence,remote.birth,remote.error,remote.result,0);
        ++it;
    }
    for(auto it=perf.operations.begin();it!=perf.operations.end();){
        auto& operation=it->second;
        if(operation.needs_cleanup){
            const auto id=it->first;++it;auto context=threads_.items.at(threads_.current).context;
            discard_perflib(context,memory,id);continue;
        }
        if(operation.consumer)for(std::size_t index=0;index<operation.consumer->targets.size();++index){
            auto& target=operation.consumer->targets[index];if(target.ready)continue;
            if(target.sequence){
                const auto result=poll_remote_family_request(memory,target.provider_pid,target.sequence);
                if(result.ready){
                    if(result.error==170 && !result.mode && result.values==std::array<std::uint64_t,5>{0,0,0,perflib::rpc::unaccepted_busy,0}){
                        // 僅未接受的請求可改用新序號；已接受後的錯誤／遺失回覆仍依原取消與補償流程。
                        target.sequence=0;target.channel.reset();if(perflib::rpc::subscribing(target.request_code))target.add_sequence=0;
                        target.retry_pending=true;target.retry_at=std::min(target.deadline,now+std::chrono::milliseconds(target.retry_delay_ms));
                        target.retry_delay_ms=std::min(100U,target.retry_delay_ms*2);
                    }else{target.ready=true;target.error=result.error;target.mode=result.mode;target.result=result.values;}
                }
            }
            if(target.retry_pending){
                if(now>=target.deadline){target.retry_pending=false;target.ready=true;target.error=121;target.mode=0;target.result={};}
                else if(now>=target.retry_at)(void)submit_perflib_consumer_target(it->first,index,target.request_code);
            }
        }
        if(operation.consumer)schedule_perflib_batch(*operation.consumer,[&](std::size_t index,std::uint32_t code){(void)submit_perflib_consumer_target(it->first,index,code);});
        if(!operation.remote){++it;continue;}auto& remote=*operation.remote;
        const auto gone=process_identity::query(remote.caller).gone({remote.caller,remote.birth});
        if(remote.finished){
            if(gone || now>=remote.deadline){it=perf.operations.erase(it);continue;}
            if(!remote.replied)remote.replied=!complete_remote_family_request(remote.caller,remote.sequence,remote.birth,remote.error,remote.result,0);
            if(!operation.worker && !operation.owner && !operation.scratch && !operation.subscription_active &&
               operation.pinned.empty() && operation.awaiting==PerflibState::Await::None && !remote.channel && !remote.token){
                // 回呼與 worker 已離開後才移出工作節點；結果與去重期限保持不變。
                try{
                    const auto saved=perf.completed_requests.emplace(PerflibState::RemoteRequestKey{remote.caller,remote.birth,remote.sequence},operation.remote);
                    if(saved.second){it=perf.operations.erase(it);continue;}
                }catch(const std::bad_alloc&){} // 保留完整節點，下一輪再嘗試，不能先丟掉原結果。
            }
        }else{
            if(gone || now>=remote.deadline){remote.cancelled=true;if(!remote.error)remote.error=gone?995U:121U;}
            if(remote.cancelled && !operation.worker && operation.action!=PerflibState::Action::Cleanup &&
               operation.collection_status.empty() && operation.awaiting==PerflibState::Await::None){
                // worker 尚未取得時即可撤銷；不因註冊等待配額已滿而永久保留 pin。
                auto context=threads_.items.at(threads_.current).context;
                (void)finish_perflib_remote(context,memory,it->first,remote.error?remote.error:995U);
                std::erase(perf.cleanup,it->first);
            }
        }
        ++it;
    }
    for(auto it=perf.subscriptions.begin();it!=perf.subscriptions.end();){
        auto& ledger=it->second;const auto& key=it->first;
        if(process_identity::query(std::get<0>(key)).gone({std::get<0>(key),std::get<1>(key)}))ledger.retiring=true;
        if(ledger.busy){++it;continue;}
        const auto provider=perf.providers.find(std::get<3>(key));
        if(provider==perf.providers.end() || ledger.counters.empty()){it=perf.subscriptions.erase(it);continue;}
        try{
            PerflibState::Operation cleanup;cleanup.action=PerflibState::Action::Cleanup;cleanup.provider=provider->first;cleanup.pinned={provider->first};
            cleanup.subscription=key;cleanup.subscription_active=true;
            for(const auto& counter:ledger.counters)if(counter.committed && (ledger.retiring || counter.cancelled))
                cleanup.notifications.push_back({provider->first,2,counter.specification,0});
            if(!cleanup.notifications.empty() && perf.operations.size()<1024 && perf.next_operation && perf.next_operation!=UINT64_MAX){
                const auto id=perf.next_operation++;perf.cleanup.push_back(id);
                try{perf.operations.emplace(id,std::move(cleanup));}catch(...){perf.cleanup.pop_back();throw;}
                ledger.busy=true;++provider->second.active;
            }
        }catch(const std::bad_alloc&){} // 保留 ledger，下一輪再取得清理 worker。
        ++it;
    }
    while(!perf.cleanup.empty() && registered_waits_.size()<64){
        const auto id=perf.cleanup.front();const auto found=perf.operations.find(id);
        if(found==perf.operations.end()){perf.cleanup.erase(perf.cleanup.begin());continue;}
        const auto current=threads_.items.at(threads_.current).context;
        const auto worker=acquire_wait_worker(current,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));if(!worker)return;
        const auto handle=next_registered_wait_++;RegisteredWait wait;wait.worker=worker;wait.perflib_operation=id;wait.armed=false;wait.ready=true;wait.closing=true;wait.work_item=true;
        registered_waits_.emplace(handle,std::move(wait));wait_workers_[worker]=handle;
        found->second.worker=worker;found->second.owner=worker;auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};perf.cleanup.erase(perf.cleanup.begin());
    }
}

void WindowsServices::cleanup_perflib_thread(GuestState& state,GuestMemory& memory,std::uint64_t thread){
    if(!perflib_)return;std::vector<std::uint64_t> cancelled;
    for(const auto& [id,operation]:perflib_->operations)if(operation.owner==thread && (operation.worker || operation.consumer))cancelled.push_back(id);
    for(auto id:cancelled)discard_perflib(state,memory,id);
}
}
