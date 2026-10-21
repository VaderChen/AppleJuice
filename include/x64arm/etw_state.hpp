#pragma once
#include "x64arm/etw_file.hpp"
#include <deque>
#include <map>
#include <optional>

namespace x64arm::etw {
// 與 guest ABI／HostFile 分開；所有可變資料由 RuntimeState 值擁有，可獨立複製。
enum class Status {Success,InvalidParameter,InvalidHandle,AlreadyExists,NotFound,
    NoResources,Unsupported,EventTooLarge,BufferTooSmall,QueueFull,FileFull,RealtimeFull,Busy,ClosePending,InvalidTime};
struct Result {Status status{Status::Success};std::uint64_t handle{};};
struct StateLimits {
    std::size_t sessions{64},providers{1024},consumers{128},notifications{4096};
    std::size_t file_bytes{512U*1024U*1024U},file_records{262143};
    std::size_t queue_bytes{64U*1024U*1024U},queue_records{262144};
    bool operator==(const StateLimits&)const=default;
};
struct Filter {
    std::uint32_t type{};Bytes data;
    bool operator==(const Filter&)const=default;
};
struct Enable {
    std::uint8_t level{};std::uint64_t any{},all{};bool ignore_keyword_zero{};
    Guid source{};std::vector<Filter> filters;
    bool operator==(const Enable&)const=default;
};
struct Aggregate {
    bool enabled{};std::uint8_t level{};std::uint64_t any{},all{};
    bool operator==(const Aggregate&)const=default;
};
struct SessionOptions {
    Guid id{};Header header;bool realtime{},file{};std::uint32_t buffer_limit{64};
    bool operator==(const SessionOptions&)const=default;
};
struct Binding {
    Enable enable;std::uint64_t logger{},order{};
    bool operator==(const Binding&)const=default;
};
struct Session {
    struct Buffered {
        std::vector<Record> records;std::uint64_t sequence{};std::uint32_t filled{};
        bool operator==(const Buffered&)const=default;
    };
    SessionOptions options;bool active{true};std::uint16_t logger_id{};
    std::map<Guid,Binding> bindings;
    std::vector<Record> file_records,pending;
    std::deque<Buffered> backlog;
    std::size_t file_bytes{},file_buffer_fill{},pending_bytes{72};std::uint64_t next_buffer{1};
    std::size_t file_buffers{1};std::uint16_t file_processor{};
    std::uint32_t events_lost{},realtime_buffers_lost{};
    bool operator==(const Session&)const=default;
};
struct Provider {
    Guid id{};bool classic{},callback{};
    bool operator==(const Provider&)const=default;
};
struct Notification {
    std::uint64_t id{},registration{},session{},logger{};std::uint32_t code{};
    bool classic{},late{},running{};Guid source{};
    Aggregate aggregate;Enable enable;
    bool operator==(const Notification&)const=default;
};
struct BufferNotice {
    std::uint64_t sequence{},identity{};std::uint32_t size{},filled{};
    std::uint16_t processor{},logger{},type{};
    std::size_t remaining{};
    bool operator==(const BufferNotice&)const=default;
};
struct QueuedRecord {
    Record record;std::uint64_t time{},buffer{},order{};std::size_t bytes{};
    bool operator==(const QueuedRecord&)const=default;
};
struct Consumer {
    Header header;std::uint64_t session{};bool realtime{},processing{},closing{},ended{};
    std::optional<std::uint64_t> start,end;
    std::deque<QueuedRecord> records;std::map<std::uint64_t,BufferNotice> buffers;
    std::deque<BufferNotice> ready_buffers;
    std::size_t queued_bytes{};std::uint64_t next_buffer{1};std::uint32_t events_lost{},buffer_limit{};
    bool operator==(const Consumer&)const=default;
};
enum class DeliveryKind {Event,Buffer,Wait,End};
struct Delivery {
    DeliveryKind kind{DeliveryKind::Wait};std::uint64_t consumer{},time{};
    Record record;BufferNotice buffer;
};
class TraceState {
public:
    explicit TraceState(StateLimits limits={}):limits_(limits){}
    Result start(SessionOptions);
    const Session* session(std::uint64_t)const;
    std::uint64_t find_session(std::u16string_view)const;
    Status stop(std::uint64_t,std::uint64_t end_time);
    Status update(std::uint64_t,const SessionOptions&,bool rollover=false);
    Status forget_session(std::uint64_t);
    Status register_provider(std::uint64_t,Guid,bool classic,bool callback);
    Status unregister_provider(std::uint64_t,bool classic);
    Status enable(std::uint64_t,Guid,std::uint32_t code,Enable={});
    Aggregate aggregate(std::uint64_t registration)const;
    bool provider_enabled(std::uint64_t,std::uint8_t,std::uint64_t)const;
    std::optional<Binding> classic_info(std::uint64_t logger)const;
    std::optional<Notification> take_notification(std::uint64_t registration=0);
    std::vector<std::uint64_t> notification_ids()const;
    Status complete_notification(std::uint64_t);
    bool notification_pending(std::uint64_t)const;
    std::size_t notification_count()const{return notifications_.size();}
    Status write(std::uint64_t registration,const Record&);
    Status write_timed(std::uint64_t registration,const Record&,std::uint64_t filetime);
    Status write_classic(std::uint64_t logger,const Record&);
    Status flush(std::uint64_t);
    Bytes file_bytes(std::uint64_t)const;
    Result open_realtime(std::u16string_view);
    Result open_file(const File&);
    const Consumer* consumer(std::uint64_t)const;
    Status begin(std::span<const std::uint64_t>,std::optional<std::uint64_t> start={},std::optional<std::uint64_t> end={});
    Delivery take(std::span<const std::uint64_t>);
    bool ready(std::uint64_t)const;
    Status finish(std::span<const std::uint64_t>);
    Status close(std::uint64_t);
    bool operator==(const TraceState&)const=default;
private:
    StateLimits limits_;
    std::map<std::uint64_t,Session> sessions_;
    std::map<std::uint64_t,Provider> providers_;
    std::map<std::uint64_t,Consumer> consumers_;
    std::map<std::uint64_t,Notification> notifications_;
    std::map<Guid,std::uint64_t> classic_sessions_;
    std::uint64_t next_session_{1},next_consumer_{1},next_logger_{1},next_notification_{1},next_enable_{1};
    Aggregate aggregate_id(const Guid&)const;
    void notify(const Guid&,std::uint32_t,const Enable&,std::uint64_t,bool classic_changed,bool late=false,std::uint64_t only=0);
    Status write_impl(std::uint64_t,const Record&,std::optional<std::uint64_t>);
    Status append(Session&,std::uint64_t,const Record&);
    Status publish(Session&,std::uint64_t);
    Status queue(Consumer&,const BufferNotice&,std::span<const Record>,std::uint64_t order);
    void retire_record(Consumer&,const QueuedRecord&);
};
}
