#include "x64arm/etw_state.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <set>

namespace x64arm::etw {
namespace {
constexpr std::uint64_t control_base=UINT64_C(0x4554000000000000),consumer_base=UINT64_C(0x4555000000000000),logger_base=UINT64_C(0x4556000000000000);
constexpr std::uint32_t filter_schematized=0x80000000U,filter_pid=0x80000004U,filter_event_id=0x80000200U;
std::u16string folded(std::u16string_view text){std::u16string result(text);for(auto& c:result)c=unicode_case_unit(c,true);return result;}
bool valid_name(std::u16string_view text,std::size_t max){return text.size()<=max && text.find(char16_t{})==text.npos;}
std::uint32_t number(std::span<const std::uint8_t> data,std::size_t at,unsigned n){std::uint32_t result=0;for(unsigned i=0;i<n;++i)result|=std::uint32_t{data[at+i]}<<(i*8);return result;}
Status validate(const Enable& config){
    if(config.filters.size()>13)return Status::InvalidParameter;std::set<std::uint32_t> seen;
    for(const auto& filter:config.filters){
        if(!seen.insert(filter.type).second)return Status::InvalidParameter;
        if(filter.type==0 || filter.type==filter_schematized){if(filter.data.size()>1024)return Status::InvalidParameter;}
        else if(filter.type==filter_pid){if(filter.data.empty() || filter.data.size()%4 || filter.data.size()>32)return Status::InvalidParameter;}
        else if(filter.type==filter_event_id){
            if(filter.data.size()<4 || filter.data[0]>1)return Status::InvalidParameter;
            const auto count=number(filter.data,2,2);if(count>64 || filter.data.size()!=4+count*2)return Status::InvalidParameter;
        }else return Status::Unsupported;
    }return Status::Success;
}
bool matches(std::uint8_t level,std::uint64_t keyword,const Enable& enable){
    if(level && level>enable.level)return false;
    if(!keyword && !enable.ignore_keyword_zero)return true;
    return keyword && (!enable.any || (keyword&enable.any)) && (keyword&enable.all)==enable.all;
}
bool filtered(const Record& record,const Enable& config){
    if(!matches(record.descriptor.level,record.descriptor.keyword,config))return false;
    for(const auto& filter:config.filters){
        if(filter.type==filter_pid){bool found=false;for(std::size_t at=0;at<filter.data.size();at+=4)found|=number(filter.data,at,4)==record.process;if(!found)return false;}
        else if(filter.type==filter_event_id){bool found=false;for(std::size_t at=4;at<filter.data.size();at+=2)found|=number(filter.data,at,2)==record.descriptor.id;if(found!=(filter.data[0]!=0))return false;}
        // Schematized filter 的資料交給 provider callback；ETW 不猜測其格式。
    }return true;
}
void lost(std::uint32_t& count,std::size_t amount=1){count+=static_cast<std::uint32_t>(std::min(amount,std::size_t{UINT32_MAX-count}));}
}
Result TraceState::start(SessionOptions options){
    if(options.header.session_name.empty() || !valid_name(options.header.session_name,1024) || !valid_name(options.header.file_name,32767) ||
       (!options.file && !options.realtime) || !options.buffer_limit || (options.file && options.header.file_name.empty()) ||
       (options.header.clock!=1 && options.header.clock!=2 && options.header.clock!=3) ||
       (options.header.clock==1 && !options.header.frequency) || (options.header.clock==3 && !options.header.cpu_mhz))return {Status::InvalidParameter};
    if(options.buffer_limit>65536)return {Status::NoResources};
    if(find_session(options.header.session_name))return {Status::AlreadyExists};
    for(const auto& [id,value]:sessions_)if(value.active && value.options.id==options.id)return {Status::AlreadyExists};
    if(sessions_.size()>=limits_.sessions || limits_.file_records==SIZE_MAX)return {Status::NoResources};
    try{(void)serialize(options.header,{},Limits{limits_.file_bytes,limits_.file_records+1,65536,1048576});}
    catch(const Error& error){return {error.failure==Failure::TooLarge?Status::BufferTooSmall:Status::InvalidParameter};}
    // ETL context 的 LoggerId 為 16-bit，controller handle 則為另一個完整 64-bit 命名空間。
    std::uint16_t logger=0;
    for(unsigned candidate=1;candidate<=UINT16_MAX;++candidate){bool used=false;for(const auto& [id,value]:sessions_)used|=value.logger_id==candidate;if(!used){logger=static_cast<std::uint16_t>(candidate);break;}}
    if(!logger || next_session_>=UINT64_C(0x1000000000000))return {Status::NoResources};
    const auto handle=control_base|next_session_++;Session value;value.options=std::move(options);value.logger_id=logger;
    value.file_bytes=value.options.header.buffer_size;
    sessions_.emplace(handle,std::move(value));return {Status::Success,handle};
}
const Session* TraceState::session(std::uint64_t handle)const{const auto found=sessions_.find(handle);return found==sessions_.end()?nullptr:&found->second;}
std::uint64_t TraceState::find_session(std::u16string_view name)const{const auto key=folded(name);for(const auto& [id,value]:sessions_)if(value.active && folded(value.options.header.session_name)==key)return id;return 0;}
Aggregate TraceState::aggregate_id(const Guid& guid)const{
    Aggregate result;
    for(const auto& [id,value]:sessions_){if(!value.active)continue;const auto found=value.bindings.find(guid);if(found==value.bindings.end())continue;
        const auto& config=found->second.enable;if(!result.enabled)result.all=UINT64_MAX;result.enabled=true;
        result.level=std::max(result.level,config.level);result.any|=config.any?config.any:UINT64_MAX;result.all&=config.all;
    }return result;
}
Aggregate TraceState::aggregate(std::uint64_t registration)const{const auto found=providers_.find(registration);return found==providers_.end() || found->second.classic?Aggregate{}:aggregate_id(found->second.id);}
bool TraceState::provider_enabled(std::uint64_t registration,std::uint8_t level,std::uint64_t keyword)const{
    const auto value=aggregate(registration);return value.enabled && (!level || level<=value.level) && (!keyword || ((keyword&value.any) && (keyword&value.all)==value.all));
}
void TraceState::notify(const Guid& guid,std::uint32_t code,const Enable& config,std::uint64_t handle,bool classic_changed,bool late,std::uint64_t only){
    const auto combined=aggregate_id(guid);
    for(const auto& [id,provider]:providers_){if(provider.id!=guid || !provider.callback || (only && only!=id) || (provider.classic && !classic_changed))continue;
        Notification item;item.id=next_notification_++;item.registration=id;item.classic=provider.classic;item.late=late;item.session=handle;item.enable=config;
        item.aggregate=combined;item.code=provider.classic?code:code==2?2U:combined.enabled?1U:0U;item.source=late?Guid{}:config.source;
        if(provider.classic && code!=0){const auto value=sessions_.find(handle);if(value!=sessions_.end()){const auto binding=value->second.bindings.find(guid);if(binding!=value->second.bindings.end())item.logger=binding->second.logger;}}
        notifications_.emplace(item.id,std::move(item));
    }
}
Status TraceState::register_provider(std::uint64_t handle,Guid guid,bool classic,bool callback){
    if(!handle)return Status::InvalidParameter;if(providers_.contains(handle))return Status::AlreadyExists;
    if(providers_.size()>=limits_.providers)return Status::NoResources;
    const auto combined=aggregate_id(guid);std::uint64_t selected=0;
    if(classic){const auto found=classic_sessions_.find(guid);if(found!=classic_sessions_.end())selected=found->second;}
    else {std::size_t count=0;for(const auto& [id,value]:sessions_)if(value.active && value.bindings.contains(guid))++count;if(count>8)return Status::NoResources;}
    if(callback && (classic?selected!=0:combined.enabled) && notifications_.size()>=limits_.notifications)return Status::NoResources;
    providers_.emplace(handle,Provider{guid,classic,callback});
    if(callback && (classic?selected!=0:combined.enabled)){
        Enable config;if(selected)config=sessions_.at(selected).bindings.at(guid).enable;
        // Late registration 沒有 SourceId；modern 的 level／keyword 是所有 session 的彙整。
        notify(guid,1,config,selected,classic,true,handle);
    }return Status::Success;
}
Status TraceState::unregister_provider(std::uint64_t handle,bool classic){
    if(!handle && !classic)return Status::Success;const auto found=providers_.find(handle);
    if(found==providers_.end() || found->second.classic!=classic)return Status::InvalidHandle;
    providers_.erase(found);
    // 正在執行的 callback 有自己的 snapshot；尚未派送的 callback 不再指向失效 registration。
    std::erase_if(notifications_,[&](const auto& entry){return entry.second.registration==handle && !entry.second.running;});return Status::Success;
}
Status TraceState::enable(std::uint64_t handle,Guid guid,std::uint32_t code,Enable config){
    const auto found=sessions_.find(handle);if(found==sessions_.end() || !found->second.active)return Status::InvalidHandle;
    if(code>2)return Status::InvalidParameter;const auto validation=validate(config);if(validation!=Status::Success)return validation;
    auto& value=found->second;const auto previous=value.bindings.find(guid);if(code==2 && previous==value.bindings.end())return Status::NotFound;
    if(code==1 && previous==value.bindings.end()){
        std::size_t count=0;for(const auto& [id,other]:sessions_)if(other.active && other.bindings.contains(guid))++count;
        // 事先啟用尚未註冊的 provider 仍須限制 modern session 數，不能靠未註冊繞過。
        bool classic_only=false,modern=false;for(const auto& [id,provider]:providers_)if(provider.id==guid){classic_only|=provider.classic;modern|=!provider.classic;}
        if(count>=8 && (!classic_only || modern))return Status::NoResources;
    }
    const auto selected=classic_sessions_.find(guid);const bool was_selected=selected!=classic_sessions_.end() && selected->second==handle;
    const bool classic_changed=code==1 || was_selected;
    std::size_t callbacks=0;for(const auto& [id,provider]:providers_)if(provider.id==guid && provider.callback && (!provider.classic || classic_changed))++callbacks;
    if(callbacks>limits_.notifications-notifications_.size())return Status::NoResources;
    if(code==1){
        if(next_logger_>=UINT64_C(0x1000000000000))return Status::NoResources;
        const auto logger=previous==value.bindings.end()?(logger_base|next_logger_++):previous->second.logger;
        value.bindings[guid]={config,logger,next_enable_++};classic_sessions_[guid]=handle;
    }else if(code==0){value.bindings.erase(guid);if(was_selected)classic_sessions_.erase(guid);}
    notify(guid,code,config,handle,classic_changed);return Status::Success;
}
std::optional<Binding> TraceState::classic_info(std::uint64_t logger)const{
    for(const auto& [guid,id]:classic_sessions_){const auto found=sessions_.find(id);if(found==sessions_.end() || !found->second.active)continue;
        const auto binding=found->second.bindings.find(guid);if(binding!=found->second.bindings.end() && binding->second.logger==logger)return binding->second;
    }return {};
}
std::optional<Notification> TraceState::take_notification(std::uint64_t registration){for(auto& [id,item]:notifications_)if(!item.running && (!registration || item.registration==registration)){item.running=true;return item;}return {};}
std::vector<std::uint64_t> TraceState::notification_ids()const{std::vector<std::uint64_t> result;for(const auto& [id,item]:notifications_)result.push_back(id);return result;}
Status TraceState::complete_notification(std::uint64_t id){const auto found=notifications_.find(id);if(found==notifications_.end() || !found->second.running)return Status::InvalidHandle;notifications_.erase(found);return Status::Success;}
bool TraceState::notification_pending(std::uint64_t id)const{return notifications_.contains(id);}
Status TraceState::stop(std::uint64_t handle,std::uint64_t end_time){
    const auto found=sessions_.find(handle);if(found==sessions_.end() || !found->second.active)return Status::InvalidHandle;
    auto& value=found->second;std::size_t callbacks=0;for(const auto& [id,provider]:providers_)if(provider.callback && value.bindings.contains(provider.id)){
        const auto selected=classic_sessions_.find(provider.id);if(!provider.classic || (selected!=classic_sessions_.end() && selected->second==handle))++callbacks;
    }
    if(callbacks>limits_.notifications-notifications_.size())return Status::NoResources;
    const auto result=publish(value,handle);value.active=false;value.options.header.end_time=end_time;
    const auto bindings=std::move(value.bindings);value.bindings.clear();
    for(const auto& [guid,binding]:bindings){const auto selected=classic_sessions_.find(guid);const bool classic_changed=selected!=classic_sessions_.end() && selected->second==handle;
        if(classic_changed)classic_sessions_.erase(selected);Enable disabled;notify(guid,0,disabled,handle,classic_changed);
    }
    for(auto& [id,consumer]:consumers_)if(consumer.session==handle){
        if(!consumer.processing && !consumer.closing)for(const auto& batch:value.backlog){BufferNotice notice;notice.sequence=batch.sequence;notice.size=value.options.header.buffer_size;
            notice.filled=batch.filled;notice.processor=batch.records[0].processor;notice.logger=value.logger_id;
            if(queue(consumer,notice,batch.records,notice.sequence<<32)!=Status::Success)lost(consumer.events_lost,batch.records.size());}
        std::stable_sort(consumer.records.begin(),consumer.records.end(),[](const auto& a,const auto& b){return a.time<b.time;});consumer.ended=true;
    }
    value.backlog.clear();
    return result;
}
Status TraceState::update(std::uint64_t handle,const SessionOptions& options,bool rollover){
    const auto found=sessions_.find(handle);if(found==sessions_.end() || !found->second.active)return Status::InvalidHandle;auto& value=found->second;const auto& old=value.options;
    auto immutable=options;immutable.header.mode=old.header.mode;immutable.header.maximum_file_size=old.header.maximum_file_size;
    immutable.header.file_name=old.header.file_name;immutable.file=old.file;immutable.realtime=old.realtime;immutable.buffer_limit=old.buffer_limit;
    if(immutable!=old || !options.buffer_limit || (!options.file && !options.realtime) || (options.file && options.header.file_name.empty()) ||
       !valid_name(options.header.file_name,32767) || (rollover && !options.file))return Status::InvalidParameter;
    if(options.buffer_limit>65536)return Status::NoResources;
    try{(void)serialize(options.header,{},Limits{limits_.file_bytes,limits_.file_records+1,65536,1048576});}
    catch(const Error& error){return error.failure==Failure::TooLarge?Status::BufferTooSmall:Status::InvalidParameter;}
    const auto result=publish(value,handle);if(old.realtime && !options.realtime){for(auto& [id,consumer]:consumers_)if(consumer.session==handle)consumer.ended=true;value.backlog.clear();}
    value.options=options;for(auto& [id,consumer]:consumers_)if(consumer.session==handle)consumer.buffer_limit=options.buffer_limit;
    if(rollover){value.file_records.clear();value.file_buffer_fill=0;value.file_buffers=1;value.file_bytes=options.header.buffer_size;}return result;
}
Status TraceState::forget_session(std::uint64_t handle){const auto found=sessions_.find(handle);if(found==sessions_.end())return Status::InvalidHandle;if(found->second.active)return Status::Busy;sessions_.erase(found);return Status::Success;}
Status TraceState::append(Session& value,std::uint64_t handle,const Record& input){
    auto record=input;record.logger=value.logger_id;
    if(record.kind==Kind::Modern && (value.options.header.mode&0x800U))record.flags|=2;const auto bytes=serialize_record(record).size();
    (void)filetime(value.options.header,record.timestamp);
    if(bytes>value.options.header.buffer_size-72U){lost(value.events_lost);return Status::BufferTooSmall;}
    const bool new_buffer=value.file_records.empty() || value.file_processor!=record.processor || bytes>value.options.header.buffer_size-value.file_buffer_fill;
    const auto capacity=std::min(limits_.file_bytes,value.options.header.maximum_file_size?std::size_t{value.options.header.maximum_file_size}*1048576:limits_.file_bytes);
    if(value.options.file && (value.file_records.size()>=limits_.file_records || (new_buffer &&
       (value.file_buffers>=65536 || value.options.header.buffer_size>capacity || value.file_bytes>capacity-value.options.header.buffer_size)))){
        lost(value.events_lost);return Status::FileFull;
    }
    Status result=Status::Success;
    if(!value.pending.empty() && (value.pending[0].processor!=record.processor || bytes>value.options.header.buffer_size-value.pending_bytes))result=publish(value,handle);
    if(value.options.file){if(new_buffer){++value.file_buffers;value.file_bytes+=value.options.header.buffer_size;value.file_buffer_fill=72;value.file_processor=record.processor;}
        value.file_buffer_fill+=bytes;value.file_records.push_back(record);}
    value.pending_bytes+=bytes;value.pending.push_back(std::move(record));return result;
}
Status TraceState::write(std::uint64_t registration,const Record& record){return write_impl(registration,record,{});}
Status TraceState::write_timed(std::uint64_t registration,const Record& record,std::uint64_t time){return write_impl(registration,record,time);}
Status TraceState::write_impl(std::uint64_t registration,const Record& record,std::optional<std::uint64_t> time){
    if(!registration)return Status::Success;const auto found=providers_.find(registration);if(found==providers_.end() || found->second.classic)return Status::InvalidHandle;
    // Disabled fast path 不驗證／序列化 payload；Windows ABI 層亦先判斷是否讀取 guest 記憶體。
    if(!provider_enabled(registration,record.descriptor.level,record.descriptor.keyword))return Status::Success;
    if(record.kind!=Kind::Modern || record.provider!=found->second.id)return Status::InvalidParameter;
    std::vector<std::pair<std::uint64_t,Record>> routed;
    try{(void)serialize_record(record);for(const auto& [id,value]:sessions_){if(!value.active)continue;const auto binding=value.bindings.find(record.provider);
        if(binding==value.bindings.end() || !filtered(record,binding->second.enable))continue;
        auto timed=record;if(time){const auto& header=value.options.header;if(header.clock==2)timed.timestamp=*time;
            else{const auto delta=static_cast<__int128>(*time)-header.start_time;
                const auto raw=static_cast<__int128>(header.start_timestamp)+(header.clock==1?delta*header.frequency/10000000:delta*header.cpu_mhz/10);
                if(raw<0 || raw>UINT64_MAX)return Status::InvalidTime;timed.timestamp=static_cast<std::uint64_t>(raw);}}
        // 先驗證所有 session 的時基，避免後面的非法時間留下部分寫入。
        (void)filetime(value.options.header,timed.timestamp);routed.emplace_back(id,std::move(timed));
    }
    Status result=Status::Success;for(const auto& [id,timed]:routed){const auto status=append(sessions_.at(id),id,timed);if(status!=Status::Success)result=status;}return result;
    }catch(const Error& error){return error.failure==Failure::TooLarge?Status::EventTooLarge:Status::InvalidParameter;}
}
Status TraceState::write_classic(std::uint64_t logger,const Record& record){
    if(record.kind!=Kind::Classic)return Status::InvalidParameter;
    for(const auto& [guid,id]:classic_sessions_){auto found=sessions_.find(id);if(found==sessions_.end() || !found->second.active)continue;
        const auto binding=found->second.bindings.find(guid);if(binding==found->second.bindings.end() || binding->second.logger!=logger)continue;
        // classic 由 provider 自行判斷 level／flags，TraceEvent 沒有 control GUID；不做 modern 篩選。
        try{return append(found->second,id,record);}catch(const Error& error){return error.failure==Failure::TooLarge?Status::EventTooLarge:Status::InvalidParameter;}
    }return Status::InvalidHandle;
}
Status TraceState::queue(Consumer& consumer,const BufferNotice& notice,std::span<const Record> records,std::uint64_t order){
    if(consumer.buffer_limit && consumer.buffers.size()+consumer.ready_buffers.size()>=consumer.buffer_limit)return Status::QueueFull;
    std::size_t bytes=0;for(const auto& record:records){const auto size=serialize_record(record).size();if(size>limits_.queue_bytes-bytes)return Status::QueueFull;bytes+=size;}
    if(bytes>limits_.queue_bytes-consumer.queued_bytes || records.size()>limits_.queue_records-consumer.records.size())return Status::QueueFull;
    // 先驗證所有時間再改變 consumer；錯誤 ETL 不留下半個 buffer。
    std::vector<QueuedRecord> pending;pending.reserve(records.size());const auto identity=consumer.next_buffer;
    for(const auto& record:records)pending.push_back({record,filetime(consumer.header,record.timestamp),identity,order++,serialize_record(record).size()});
    auto buffer=notice;buffer.identity=consumer.next_buffer++;buffer.remaining=records.size();consumer.buffers.emplace(buffer.identity,buffer);
    consumer.queued_bytes+=bytes;for(auto& item:pending)consumer.records.push_back(std::move(item));
    if(records.empty()){consumer.ready_buffers.push_back(buffer);consumer.buffers.erase(buffer.identity);}return Status::Success;
}
Status TraceState::publish(Session& value,std::uint64_t handle){
    if(value.pending.empty())return Status::Success;
    BufferNotice notice;notice.sequence=value.next_buffer++;notice.size=value.options.header.buffer_size;notice.filled=static_cast<std::uint32_t>(value.pending_bytes);
    notice.processor=value.pending[0].processor;notice.logger=value.logger_id;
    Status result=Status::Success;bool lost_buffer=false;
    bool receiving=false;
    if(value.options.realtime)for(auto& [id,consumer]:consumers_)if(consumer.session==handle && consumer.processing && !consumer.closing && !consumer.ended){
        receiving=true;const auto records=std::span(value.pending);
        if(!records.empty()){
            const auto status=queue(consumer,notice,records,notice.sequence<<32);if(status!=Status::Success){result=status;lost(consumer.events_lost,records.size());lost_buffer=true;}
            else std::stable_sort(consumer.records.begin(),consumer.records.end(),[](const auto& a,const auto& b){return a.time<b.time;});
        }
    }
    if(value.options.realtime && !receiving){if(value.backlog.size()>=std::min(std::size_t{value.options.buffer_limit},limits_.queue_bytes/value.options.header.buffer_size)){result=Status::RealtimeFull;lost(value.events_lost,value.pending.size());lost_buffer=true;}
        else value.backlog.push_back({value.pending,notice.sequence,notice.filled});}
    if(lost_buffer)lost(value.realtime_buffers_lost);value.pending.clear();value.pending_bytes=72;return result;
}
Status TraceState::flush(std::uint64_t handle){const auto found=sessions_.find(handle);if(found==sessions_.end() || !found->second.active)return Status::InvalidHandle;return publish(found->second,handle);}
Bytes TraceState::file_bytes(std::uint64_t handle)const{
    const auto found=sessions_.find(handle);if(found==sessions_.end() || !found->second.options.file)throw Error(Failure::InvalidParameter,"ETW 沒有此檔案 session");
    auto header=found->second.options.header;header.events_lost=found->second.events_lost;header.buffers_lost=found->second.realtime_buffers_lost;
    return serialize(header,found->second.file_records,Limits{limits_.file_bytes,limits_.file_records+1,65536,1048576});
}
Result TraceState::open_realtime(std::u16string_view name){
    const auto handle=find_session(name);const auto found=sessions_.find(handle);if(found==sessions_.end() || !found->second.options.realtime)return {Status::NotFound};
    if(consumers_.size()>=limits_.consumers || next_consumer_>=UINT64_C(0x1000000000000))return {Status::NoResources};
    Consumer consumer;consumer.header=found->second.options.header;consumer.realtime=true;consumer.session=handle;
    consumer.buffer_limit=found->second.options.buffer_limit;const auto id=consumer_base|next_consumer_++;consumers_.emplace(id,std::move(consumer));return {Status::Success,id};
}
Result TraceState::open_file(const File& file){
    if(consumers_.size()>=limits_.consumers || next_consumer_>=UINT64_C(0x1000000000000))return {Status::NoResources};
    Consumer consumer;consumer.header=file.header;consumer.ended=true;std::uint64_t order=0;
    try{for(const auto& buffer:file.buffers){
        BufferNotice notice;notice.sequence=buffer.sequence;notice.size=buffer.size;notice.filled=buffer.filled;notice.processor=buffer.processor;notice.logger=buffer.logger;notice.type=buffer.type;
        const auto status=queue(consumer,notice,buffer.records,order);
        if(status!=Status::Success)return {status};order+=buffer.records.size();
    }}catch(const Error& error){return {error.failure==Failure::Unsupported?Status::Unsupported:Status::InvalidParameter};}
    std::stable_sort(consumer.records.begin(),consumer.records.end(),[](const auto& a,const auto& b){return a.time<b.time;});
    const auto id=consumer_base|next_consumer_++;consumers_.emplace(id,std::move(consumer));return {Status::Success,id};
}
const Consumer* TraceState::consumer(std::uint64_t handle)const{const auto found=consumers_.find(handle);return found==consumers_.end()?nullptr:&found->second;}
Status TraceState::begin(std::span<const std::uint64_t> handles,std::optional<std::uint64_t> start,std::optional<std::uint64_t> end){
    if(handles.empty() || handles.size()>64)return Status::InvalidParameter;if(start && end && *end<*start)return Status::InvalidTime;
    bool realtime=false;std::set<std::uint64_t> seen;
    for(auto id:handles){const auto found=consumers_.find(id);if(found==consumers_.end() || found->second.closing)return Status::InvalidHandle;
        if(!seen.insert(id).second)return Status::InvalidParameter;if(found->second.processing)return Status::Busy;realtime|=found->second.realtime;
    }if(realtime && handles.size()!=1)return Status::InvalidParameter;
    if(realtime){auto& value=consumers_.at(handles[0]);const auto found=sessions_.find(value.session);
        if(found!=sessions_.end()){auto& source=found->second;auto pending=value;
            for(const auto& batch:source.backlog){BufferNotice notice;notice.sequence=batch.sequence;notice.size=source.options.header.buffer_size;notice.filled=batch.filled;
                notice.processor=batch.records[0].processor;notice.logger=source.logger_id;const auto status=queue(pending,notice,batch.records,notice.sequence<<32);if(status!=Status::Success)return status;}
            auto capacity=pending;if(!source.pending.empty()){BufferNotice notice;notice.sequence=source.next_buffer;notice.size=source.options.header.buffer_size;notice.filled=static_cast<std::uint32_t>(source.pending_bytes);
                notice.processor=source.pending[0].processor;notice.logger=source.logger_id;const auto status=queue(capacity,notice,source.pending,notice.sequence<<32);if(status!=Status::Success)return status;}
            std::stable_sort(pending.records.begin(),pending.records.end(),[](const auto& a,const auto& b){return a.time<b.time;});
            value=std::move(pending);value.processing=true;source.backlog.clear();(void)publish(source,value.session);
        }
    }
    for(auto id:handles){auto& value=consumers_.at(id);value.processing=true;value.start=start;value.end=end;}return Status::Success;
}
void TraceState::retire_record(Consumer& value,const QueuedRecord& item){
    value.queued_bytes-=item.bytes;auto found=value.buffers.find(item.buffer);if(found==value.buffers.end() || !found->second.remaining)throw Error(Failure::InvalidFormat,"ETW consumer buffer 計數不符");
    if(--found->second.remaining==0){value.ready_buffers.push_back(found->second);value.buffers.erase(found);}
}
Delivery TraceState::take(std::span<const std::uint64_t> handles){
    if(handles.empty())throw Error(Failure::InvalidParameter,"ETW 沒有 consumer");
    for(auto id:handles){const auto found=consumers_.find(id);if(found==consumers_.end() || !found->second.processing)throw Error(Failure::InvalidParameter,"ETW consumer 尚未開始處理");}
    for(auto id:handles){auto& value=consumers_.at(id);
        while(!value.records.empty()){const auto& item=value.records.front();if((!value.start || item.time>=*value.start) && (!value.end || item.time<=*value.end))break;
            auto skipped=std::move(value.records.front());value.records.pop_front();retire_record(value,skipped);
        }
        if(!value.ready_buffers.empty()){Delivery result;result.kind=DeliveryKind::Buffer;result.consumer=id;result.buffer=value.ready_buffers.front();value.ready_buffers.pop_front();return result;}
    }
    std::uint64_t selected=0;bool ended=true;
    for(auto id:handles){const auto& value=consumers_.at(id);ended&=value.ended;
        if(!value.records.empty() && (!selected || value.records.front().time<consumers_.at(selected).records.front().time))selected=id;
    }
    if(!selected){Delivery result;result.kind=ended?DeliveryKind::End:DeliveryKind::Wait;return result;}
    auto& value=consumers_.at(selected);auto item=std::move(value.records.front());value.records.pop_front();retire_record(value,item);
    Delivery result;result.kind=DeliveryKind::Event;result.consumer=selected;result.time=item.time;result.record=std::move(item.record);return result;
}
bool TraceState::ready(std::uint64_t id)const{const auto found=consumers_.find(id);return found!=consumers_.end() && (!found->second.records.empty() || !found->second.ready_buffers.empty() || found->second.ended);}
Status TraceState::finish(std::span<const std::uint64_t> handles){
    std::set<std::uint64_t> seen;for(auto id:handles){const auto found=consumers_.find(id);if(!seen.insert(id).second)return Status::InvalidParameter;if(found==consumers_.end() || !found->second.processing)return Status::InvalidHandle;}
    for(auto id:handles){auto found=consumers_.find(id);found->second.processing=false;if(found->second.closing)consumers_.erase(found);}return Status::Success;
}
Status TraceState::close(std::uint64_t id){
    const auto found=consumers_.find(id);if(found==consumers_.end() || found->second.closing)return Status::InvalidHandle;
    auto& value=found->second;if(value.processing){if(!value.realtime)return Status::Busy;value.closing=true;value.ended=true;return Status::ClosePending;}
    consumers_.erase(found);return Status::Success;
}
}
