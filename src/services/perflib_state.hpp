#pragma once
#include "perflib_data.hpp"
#include "perflib_catalog.hpp"
#include "perflib_rpc.hpp"
#include "x64arm/shared_token.hpp"
#include "x64arm/guest_callback.hpp"
#include <chrono>
#include <optional>
#include <tuple>

namespace x64arm {
struct PerflibState {
    struct Instance {
        perflib::Guid set{};
        std::u16string name;
        std::uint32_t id{};
        std::size_t size{};
        bool custom{};
    };
    struct Provider {
        perflib::Guid id{};
        std::uint64_t callback{},allocate{},release{},context{};
        std::map<perflib::Guid,perflib::CounterSet> sets;
        std::map<std::uint64_t,Instance> instances;
        std::uint32_t active{};
        bool stopping{};
        perflib::Bytes security;
        std::shared_ptr<perflib::ProviderPublication> publication;
    };
    struct RemoteEndpoint {
        ProcessIdentity origin;
        std::uint64_t publication{},handle{};
        bool operator==(const RemoteEndpoint&)const=default;
    };
    struct Binding {
        perflib::Identifier specification;
        std::uint64_t local_provider{};
        std::vector<RemoteEndpoint> remote;
        std::shared_ptr<const perflib::CounterSet> registration;
        bool multi_instance{};
    };
    struct Query {
        struct Contact {RemoteEndpoint endpoint;bool attempted{};};
        struct Schema {std::shared_ptr<const perflib::CounterSet> registration;std::vector<RemoteEndpoint> remote;};
        using Schemas=std::map<perflib::Guid,Schema>;
        std::vector<perflib::Identifier> counters;
        std::vector<Binding> bindings;
        std::vector<Contact> contacts; // 包含回覆遺失的 Add，關閉時仍須回收 cookie。
        Schemas known_schemas; // 僅保存本 query 成功取得的 schema；不冒充 manifest 安裝。
        std::size_t schema_bytes{};
        std::uint32_t active{};
    };
    enum class Action {Create,Delete,Stop,Add,Remove,Close,Collect,Enumerate,Cleanup,Subscribe,Unsubscribe,Probe};
    enum class Await {None,Allocate,Release,Control};
    struct Notification {
        std::uint64_t provider{};
        std::uint32_t code{};
        std::optional<perflib::Identifier> counter;
        std::size_t item{};
    };
    struct Release {std::uint64_t address{},function{},context{};bool custom{};};
    using SubscriptionKey=std::tuple<std::uint32_t,std::uint64_t,std::uint64_t,std::uint64_t>; // caller、birth、query cookie、provider
    struct Subscription {
        struct Counter {perflib::Identifier specification;std::uint64_t reservation{},sequence{};bool committed{},cancelled{};};
        std::vector<Counter> counters;
        bool busy{},retiring{};
    };
    struct RemoteRequest {
        std::uint32_t caller{},code{},error{};
        std::uint64_t birth{},sequence{};
        std::chrono::steady_clock::time_point deadline;
        std::shared_ptr<perflib::rpc::Channel> channel;
        std::shared_ptr<SharedToken> token;
        std::array<std::uint64_t,5> request{};
        std::array<std::uint64_t,5> result{};
        std::pair<std::uint64_t,std::uint64_t> payload_identity{};
        bool cancelled{},finished{},replied{},response_written{};
    };
    using RemoteRequestKey=std::tuple<std::uint32_t,std::uint64_t,std::uint64_t>; // caller、birth、sequence
    static constexpr std::size_t maximum_remote_records=65536;
    struct RemoteConsumer {
        struct Target {
            std::uint32_t provider_pid{},error{},mode{},request_code{};
            std::uint64_t sequence{},add_sequence{};
            std::chrono::steady_clock::time_point deadline{},retry_at{};
            std::uint32_t retry_delay_ms{1};
            RemoteEndpoint endpoint;
            std::vector<perflib::Identifier> specifications;
            std::vector<std::size_t> indices;
            std::shared_ptr<perflib::rpc::Channel> channel;
            std::array<std::uint64_t,5> result{};
            bool ready{},consumed{},retry_pending{};
        };
        struct Collection {
            struct Item {
                const perflib::CounterSet* registration{};std::uint32_t error{};
                std::vector<perflib::Instance> instances;std::size_t dynamic_bytes{};
                bool wildcard_counters{},layout_charged{};
            };
            std::vector<Item> items;
            std::size_t budget{48};
        };
        std::string symbol;
        std::uint64_t stack{};
        std::vector<Target> targets;
        std::shared_ptr<SharedToken> token;
        std::shared_ptr<Collection> collection;
        std::size_t next_submit{};
        std::uint32_t batch_code{};
        bool submitted{},merged{},batched{};
    };
    struct Control {
        enum class Phase {Probe,Reconcile,Prepare,Local,Remote,RollbackLocal,RollbackRemote,Commit,CloseRemote};
        Phase phase{Phase::Prepare};
        Binding staged;
        Query::Schemas::node_type schema_node;
        std::size_t schema_charge{};
        perflib::Identifier counter_copy;
        std::vector<perflib::CatalogEntry> candidates;
        std::vector<RemoteEndpoint> reconcile_endpoints;
        std::size_t reconcile_position{};
        std::uint32_t status{},callback_result{};
        bool callback_complete{},local_added{},refresh{},probes_started{},reconcile_started{};
    };
    struct Operation {
        Action action{};
        Await awaiting{};
        std::uint64_t owner{},worker{},provider{},query{},output{},actual{},scratch{};
        std::uint32_t capacity{},status{};
        std::size_t position{},notification{},release_position{},output_offset{};
        perflib::Bytes bytes;
        perflib::Guid set{};
        std::u16string name;
        std::uint32_t instance_id{};
        std::vector<perflib::Identifier> identifiers;
        std::vector<std::uint64_t> pinned;
        std::vector<Notification> notifications;
        std::vector<Release> releases;
        std::map<std::uint64_t,std::uint32_t> collection_status;
        std::map<std::uint64_t,std::uint32_t> collection_begin_status;
        std::shared_ptr<RemoteRequest> remote;
        std::shared_ptr<RemoteConsumer> consumer;
        std::shared_ptr<Control> control;
        std::optional<SubscriptionKey> subscription;
        std::uint64_t reservation{};
        std::uint64_t undo_sequence{};
        bool subscription_active{};
        bool needs_cleanup{};
        bool collected{},allocated{},retired{},retire_provider{};
    };
    std::map<std::uint64_t,Provider> providers;
    std::shared_ptr<perflib::ProviderCatalog> catalog;
    bool endpoint_listener{}; // 曾發佈 provider 的程序須繼續回答已保存的 endpoint 探測。
    std::map<std::uint64_t,Query> queries;
    std::map<std::uint64_t,Operation> operations;
    // 已完成的傳輸結果保留至原期限；不占用客體工作、provider pin 或資料通道。
    std::map<RemoteRequestKey,std::shared_ptr<RemoteRequest>> completed_requests;
    std::map<SubscriptionKey,Subscription> subscriptions;
    // 由 registered-wait worker 執行的清理或遠端 provider 工作。
    std::vector<std::uint64_t> cleanup;
    // 與一般 Win32 handle 命名空間分開；兩種 handle 不可互換或用 CloseHandle 關閉。
    std::uint64_t next_handle{0x5f000000},next_operation{1};
    std::u16string machine{u"X64ARM"};
    PerflibState(){cleanup.reserve(1024);}
    std::uint64_t provider_for(const perflib::Guid& id)const{
        for(const auto& [handle,provider]:providers)if(!provider.stopping && provider.sets.contains(id))return handle;
        return 0;
    }
};
}
