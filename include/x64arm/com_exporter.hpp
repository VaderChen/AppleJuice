#pragma once
#include "x64arm/com_marshal.hpp"
#include "x64arm/com_rpc_wire.hpp"
#include "x64arm/windows_com_bind.hpp"
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace x64arm::com_exporter {
enum class Status {Ok,Missing,Invalid,Disconnected,Busy,Limit,Denied};
struct Source {
    ComBindReference canonical,interface;
    std::uint64_t generation{};
    bool operator==(const Source&)const=default;
};
struct Client {
    com_rpc::Endpoint endpoint;std::uint64_t request{};
    auto operator<=>(const Client&)const=default;
};
struct Retirement {
    enum class Kind {Packet,Reference} kind{};
    std::uint64_t packet{};ComBindReference source;bool started{};
    bool operator==(const Retirement&)const=default;
};
struct Result {
    Status status{Status::Ok};com_rpc::Interface wire{};Source source{};
    std::uint64_t operation{};std::vector<std::uint64_t> retirements{};
};

// 此 ledger 不執行客體函式。強 packet 由既有 com_marshal::Registry 持有；
// driver 完成真正的 QI／AddRef 才可 commit_acquire。退役以工作 ID 保存，
// driver 將它交到原有 ReleaseAt／packet Release 流程，不可直接丟棄引用。
class Registry {
    using Canonical=std::tuple<std::uint64_t,std::uint64_t,std::uint64_t>;
    using InterfaceKey=std::tuple<Canonical,WindowsGuid,std::uint64_t>;
    struct Interface {
        com_rpc::Interface wire;Source source;
        struct References{std::uint32_t tracked{},untracked{};bool operator==(const References&)const=default;};
        std::map<com_rpc::Endpoint,References> clients;
        bool connected{true};std::uint32_t pins{};
        bool operator==(const Interface&)const=default;
    };
    struct Packet {
        std::uint64_t interface{},operation{};std::uint32_t flags{},pins{};bool retired{};
        bool operator==(const Packet&)const=default;
    };
    struct Operation {
        enum class Kind {Acquire,Method} kind{};
        std::uint64_t interface{},packet{};Client client;
        bool operator==(const Operation&)const=default;
    };
    struct State {
        std::map<std::uint64_t,Interface> interfaces;
        std::map<InterfaceKey,std::uint64_t> index;
        std::map<std::uint64_t,Packet> packets;
        std::map<std::uint64_t,Operation> operations;
        std::map<Client,std::uint64_t> requests;
        std::map<std::uint64_t,Retirement> retirements;
        std::map<Canonical,std::uint32_t> pins;
        std::map<Canonical,std::vector<Retirement>> deferred;
        std::uint64_t next_interface{1},next_operation{1},next_retirement{1};
        bool operator==(const State&)const=default;
    } state_;
    com_rpc::Endpoint endpoint_;
    static constexpr std::size_t limit=4096;
    static Canonical canonical(const Source& source){const auto owner=source.canonical.apartment && !*source.canonical.apartment?0:source.canonical.owner;return {owner,source.canonical.object,source.generation};}
    static InterfaceKey key(const Interface& value){return {canonical(value.source),value.wire.iid,value.source.interface.object};}
    static bool nonzero(const std::array<std::uint8_t,16>& bytes){return !com_rpc::wire_detail::zero(bytes);}
    static bool matching(const Interface& value,const com_rpc::Interface& wire){
        if(value.wire.endpoint!=wire.endpoint || value.wire.id!=wire.id || value.wire.iid!=wire.iid)return false;
        std::uint8_t different=0;for(std::size_t i=0;i<wire.capability.size();++i)different|=value.wire.capability[i]^wire.capability[i];return different==0;
    }
    static std::uint64_t identifier(std::uint64_t& next){if(!next || next==UINT64_MAX)throw std::bad_alloc();return next++;}
    template<class F> auto change(F&& function){
        // API 家族實作階段先保證 OOM 不留下半份 ownership；效能驗收後再縮小複製。
        auto replacement=state_;auto result=function(replacement);std::swap(state_,replacement);return result;
    }
    static void retirement(State& state,Retirement value,std::vector<std::uint64_t>& output){
        if(state.retirements.size()>=limit)throw std::bad_alloc();const auto id=identifier(state.next_retirement);state.retirements.emplace(id,std::move(value));output.push_back(id);
    }
    static void defer(State& state,Canonical identity,Retirement value,std::vector<std::uint64_t>& output){
        if(state.pins.contains(identity)){auto& pending=state.deferred[identity];if(pending.size()>=limit)throw std::bad_alloc();pending.push_back(std::move(value));}
        else retirement(state,std::move(value),output);
    }
    static void finish(State& state,std::uint64_t operation,std::vector<std::uint64_t>& output){
        const auto found=state.operations.find(operation);if(found==state.operations.end())return;
        const auto value=found->second;auto& entry=state.interfaces.at(value.interface);const auto identity=canonical(entry.source);
        if(entry.pins)--entry.pins;
        auto pin=state.pins.find(identity);if(pin==state.pins.end() || !pin->second)throw std::logic_error("COM exporter 缺少引用 pin");
        if(!--pin->second){state.pins.erase(pin);if(auto pending=state.deferred.find(identity);pending!=state.deferred.end()){
            for(auto& action:pending->second)retirement(state,std::move(action),output);state.deferred.erase(pending);}}
        if(value.packet){auto packet=state.packets.find(value.packet);if(packet!=state.packets.end()){
            if(packet->second.pins)--packet->second.pins;if(packet->second.operation==operation)packet->second.operation=0;
            if(packet->second.retired && !packet->second.pins){defer(state,identity,{Retirement::Kind::Packet,value.packet,{}},output);state.packets.erase(packet);}}}
        state.requests.erase(value.client);state.operations.erase(found);
    }
    static void collect(State& state){
        if(!state.retirements.empty() || !state.deferred.empty())return;
        for(auto item=state.interfaces.begin();item!=state.interfaces.end();){const auto id=item->first;
            const bool retained=std::any_of(state.packets.begin(),state.packets.end(),[&](const auto& packet){return packet.second.interface==id;});
            if(item->second.pins || !item->second.clients.empty() || retained){++item;continue;}
            const auto indexed=state.index.find(key(item->second));if(indexed!=state.index.end() && indexed->second==id)state.index.erase(indexed);
            item=state.interfaces.erase(item);
        }
    }
    static Result begin(State& state,const com_rpc::Interface& wire,std::uint64_t packet,Client client,Operation::Kind kind){
        if(!com_rpc::wire_detail::valid(client.endpoint) || !client.request)return {Status::Invalid};
        const auto found=state.interfaces.find(wire.id);if(found==state.interfaces.end())return {Status::Missing};
        auto& entry=found->second;if(!matching(entry,wire))return {Status::Denied};if(!entry.connected)return {Status::Disconnected};
        if(state.requests.contains(client))return {Status::Busy};if(state.operations.size()>=limit || entry.pins==UINT32_MAX)return {Status::Limit};
        auto identity=canonical(entry.source);if(state.pins.contains(identity) && state.pins.at(identity)==UINT32_MAX)return {Status::Limit};
        if(packet){const auto p=state.packets.find(packet);if(p==state.packets.end() || p->second.interface!=wire.id)return {Status::Missing};
            if(p->second.retired)return {Status::Disconnected};if(p->second.pins==UINT32_MAX)return {Status::Limit};
            if(com_marshal::mode_flags(p->second.flags)==0 && p->second.operation)return {Status::Busy};}
        else if(!entry.clients.contains(client.endpoint))return {Status::Denied};
        const auto operation=identifier(state.next_operation);state.operations.emplace(operation,Operation{kind,wire.id,packet,client});state.requests.emplace(client,operation);
        ++entry.pins;++state.pins[identity];if(packet){auto& value=state.packets.at(packet);++value.pins;if(com_marshal::mode_flags(value.flags)==0)value.operation=operation;}
        Result result;result.wire=entry.wire;result.source=entry.source;result.operation=operation;return result;
    }
    template<class Match> static Result disconnect_matching(State& state,Match&& matches){
        Result result;
        for(auto& [id,entry]:state.interfaces){
            if(!matches(entry.source))continue;entry.connected=false;const auto identity=canonical(entry.source);
            for(const auto& [client,refs]:entry.clients){(void)client;
                for(std::uint64_t i=0,count=std::uint64_t{refs.tracked}+refs.untracked;i<count;++i)
                    defer(state,identity,{Retirement::Kind::Reference,0,entry.source.interface},result.retirements);
            }
            entry.clients.clear();
            for(auto packet=state.packets.begin();packet!=state.packets.end();){
                if(packet->second.interface!=id){++packet;continue;}
                packet->second.retired=true;if(packet->second.pins){++packet;continue;}
                defer(state,identity,{Retirement::Kind::Packet,packet->first,{}},result.retirements);packet=state.packets.erase(packet);
            }
        }return result;
    }
public:
    void configure(com_rpc::Endpoint endpoint){if(!com_rpc::wire_detail::valid(endpoint) || (com_rpc::wire_detail::valid(endpoint_) && endpoint_!=endpoint))throw std::invalid_argument("COM exporter endpoint 不合法");endpoint_=endpoint;}
    Result publish(std::uint64_t packet,Source source,WindowsGuid iid,std::uint32_t flags,const std::array<std::uint8_t,16>& capability){
        if(!com_rpc::wire_detail::valid(endpoint_) || !packet || !source.canonical.owner || !source.canonical.object || !source.interface.owner || !source.interface.object || !source.generation || !com_marshal::valid_flags(flags) || !nonzero(capability))return {Status::Invalid};
        return change([&](State& state){
            if(state.packets.contains(packet))return Result{Status::Invalid};if(state.packets.size()>=limit)return Result{Status::Limit};
            const InterfaceKey lookup{canonical(source),iid,source.interface.object};std::uint64_t id{};
            if(const auto existing=state.index.find(lookup);existing!=state.index.end()){
                if(state.interfaces.at(existing->second).connected)id=existing->second;else state.index.erase(existing);
            }
            if(!id){if(state.interfaces.size()>=limit)return Result{Status::Limit};id=identifier(state.next_interface);Interface value;value.wire={endpoint_,id,iid,capability};value.source=source;
                state.interfaces.emplace(id,std::move(value));state.index.emplace(lookup,id);}
            state.packets.emplace(packet,Packet{id,0,flags});Result result;result.wire=state.interfaces.at(id).wire;result.source=state.interfaces.at(id).source;return result;
        });
    }
    Result begin_acquire(const com_rpc::Interface& wire,std::uint64_t packet,Client client){return change([&](State& state){return begin(state,wire,packet,client,Operation::Kind::Acquire);});}
    Result begin_method(const com_rpc::Interface& wire,Client client){return change([&](State& state){return begin(state,wire,0,client,Operation::Kind::Method);});}
    Result inspect_packet(const com_rpc::Interface& wire,std::uint64_t packet,std::uint32_t flags)const{
        const auto p=state_.packets.find(packet);if(p==state_.packets.end())return {Status::Missing};
        const auto found=state_.interfaces.find(p->second.interface);if(found==state_.interfaces.end())return {Status::Missing};
        const auto& entry=found->second;
        if(!matching(entry,wire) || p->second.flags!=flags)return {Status::Denied};
        if(!entry.connected || p->second.retired)return {Status::Disconnected};
        Result result;result.wire=entry.wire;result.source=entry.source;return result;
    }
    bool owns_packet(std::uint64_t packet)const{
        if(state_.packets.contains(packet))return true;
        const auto matches=[&](const Retirement& action){return action.kind==Retirement::Kind::Packet && action.packet==packet;};
        for(const auto& [id,action]:state_.retirements){(void)id;if(matches(action))return true;}
        for(const auto& [identity,actions]:state_.deferred){(void)identity;if(std::any_of(actions.begin(),actions.end(),matches))return true;}
        return false;
    }
    // 同程序取得的引用由既有 apartment proxy 或客體介面直接持有。
    // 不在遠端 client ledger 再保留一份，也不對實際 AddRef 的 ULONG 判斷 HRESULT。
    Result complete_local(std::uint64_t operation){
        return change([&](State& state){const auto found=state.operations.find(operation);
            if(found==state.operations.end() || found->second.kind!=Operation::Kind::Acquire)return Result{Status::Missing};
            const auto value=found->second;const auto& entry=state.interfaces.at(value.interface);
            Result result;result.wire=entry.wire;result.source=entry.source;
            if(!entry.connected)result.status=Status::Disconnected;
            else if(auto packet=state.packets.find(value.packet);packet!=state.packets.end()){
                if(com_marshal::mode_flags(packet->second.flags)==0)packet->second.retired=true;
            }else result.status=Status::Disconnected;
            finish(state,operation,result.retirements);return result;
        });
    }
    Result complete(std::uint64_t operation,bool acquired=false){
        return change([&](State& state){const auto found=state.operations.find(operation);if(found==state.operations.end())return Result{Status::Missing};
            const auto value=found->second;auto& entry=state.interfaces.at(value.interface);Result result;result.wire=entry.wire;result.source=entry.source;
            if(value.kind==Operation::Kind::Acquire && acquired){
                if(!entry.connected){defer(state,canonical(entry.source),{Retirement::Kind::Reference,0,entry.source.interface},result.retirements);result.status=Status::Disconnected;finish(state,operation,result.retirements);return result;}
                auto client=entry.clients.find(value.client.endpoint);const auto packet=state.packets.find(value.packet);
                if(packet==state.packets.end())return Result{Status::Disconnected};const bool no_ping=(packet->second.flags&4U)!=0;
                if(client!=entry.clients.end() && (no_ping?client->second.untracked:client->second.tracked)==UINT32_MAX)return Result{Status::Limit};
                if(client==entry.clients.end() && entry.clients.size()>=limit)return Result{Status::Limit};
                auto& owned=entry.clients[value.client.endpoint];++(no_ping?owned.untracked:owned.tracked);
                if(auto acquired_packet=state.packets.find(value.packet);acquired_packet!=state.packets.end() && com_marshal::mode_flags(acquired_packet->second.flags)==0)acquired_packet->second.retired=true;
            }
            finish(state,operation,result.retirements);return result;
        });
    }
    Result release_packet(std::uint64_t packet){
        return change([&](State& state){const auto found=state.packets.find(packet);if(found==state.packets.end())return Result{Status::Missing};
            found->second.retired=true;Result result;const auto& entry=state.interfaces.at(found->second.interface);result.wire=entry.wire;result.source=entry.source;
            if(!found->second.pins){defer(state,canonical(entry.source),{Retirement::Kind::Packet,packet,{}},result.retirements);state.packets.erase(found);}return result;
        });
    }
    Result release_client(const com_rpc::Interface& wire,com_rpc::Endpoint client,bool no_ping=false){
        return change([&](State& state){const auto found=state.interfaces.find(wire.id);if(found==state.interfaces.end())return Result{Status::Missing};auto& entry=found->second;
            if(!matching(entry,wire))return Result{Status::Denied};auto owned=entry.clients.find(client);if(owned==entry.clients.end())return Result{Status::Missing};
            auto& count=no_ping?owned->second.untracked:owned->second.tracked;if(!count)return Result{Status::Missing};--count;
            if(!owned->second.tracked && !owned->second.untracked)entry.clients.erase(owned);Result result;result.source=entry.source;defer(state,canonical(entry.source),{Retirement::Kind::Reference,0,entry.source.interface},result.retirements);return result;
        });
    }
    Result abort(std::uint64_t operation,bool acquired=false){
        return change([&](State& state){const auto found=state.operations.find(operation);if(found==state.operations.end())return Result{Status::Missing};const auto& entry=state.interfaces.at(found->second.interface);
            Result result;result.wire=entry.wire;result.source=entry.source;
            if(acquired && found->second.kind==Operation::Kind::Acquire)defer(state,canonical(entry.source),{Retirement::Kind::Reference,0,entry.source.interface},result.retirements);
            finish(state,operation,result.retirements);return result;
        });
    }
    Result retire_client(com_rpc::Endpoint client,bool include_no_ping=false){
        return change([&](State& state){Result result;
            for(auto& [id,entry]:state.interfaces){(void)id;const auto owned=entry.clients.find(client);if(owned==entry.clients.end())continue;
                const auto count=std::uint64_t{owned->second.tracked}+(include_no_ping?owned->second.untracked:0U);
                owned->second.tracked=0;if(include_no_ping)owned->second.untracked=0;
                for(std::uint64_t i=0;i<count;++i)defer(state,canonical(entry.source),{Retirement::Kind::Reference,0,entry.source.interface},result.retirements);
                if(!owned->second.untracked)entry.clients.erase(owned);
            }return result;
        });
    }
    void move_mta_owner(std::uint64_t from,std::uint64_t to){
        if(!from || !to)throw std::invalid_argument("COM exporter MTA owner 不合法");
        const auto move=[&](ComBindReference& source){if(source.owner==from && source.apartment && !*source.apartment)source.owner=to;};
        for(auto& [id,entry]:state_.interfaces){(void)id;move(entry.source.interface);move(entry.source.canonical);}
        for(auto& [id,entry]:state_.retirements){(void)id;if(!entry.started)move(entry.source);}
        for(auto& [identity,entries]:state_.deferred){(void)identity;for(auto& entry:entries)move(entry.source);}
    }
    Result disconnect(const ComBindReference& canonical_source,std::uint64_t generation){
        Source lookup;lookup.canonical=canonical_source;lookup.generation=generation;const auto identity=canonical(lookup);
        return change([&](State& state){return disconnect_matching(state,[&](const Source& source){return canonical(source)==identity;});});
    }
    Result disconnect_owner(std::uint64_t owner){
        return change([&](State& state){return disconnect_matching(state,[&](const Source& source){return source.canonical.owner==owner || source.interface.owner==owner;});});
    }
    std::optional<Retirement> start_retirement(std::uint64_t id){const auto found=state_.retirements.find(id);if(found==state_.retirements.end() || found->second.started)return std::nullopt;found->second.started=true;return found->second;}
    bool finish_retirement(std::uint64_t id){const auto found=state_.retirements.find(id);if(found==state_.retirements.end() || !found->second.started)return false;state_.retirements.erase(found);collect(state_);return true;}
    const std::map<std::uint64_t,Retirement>& retirements()const noexcept{return state_.retirements;}
    std::size_t packets()const noexcept{return state_.packets.size();}
    std::size_t operations()const noexcept{return state_.operations.size();}
    std::size_t interfaces()const noexcept{return state_.interfaces.size();}
    bool operator==(const Registry&)const=default;
};
}
