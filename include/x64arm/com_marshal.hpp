#pragma once
#include "x64arm/windows_guid.hpp"
#include "x64arm/com_rpc_wire.hpp"
#include <map>
#include <functional>
#include <optional>
#include <span>
#include <vector>
namespace x64arm::com_marshal {
constexpr bool valid_flags(std::uint32_t flags){return !(flags&~7U) && (flags&3U)!=3;}
constexpr std::uint32_t mode_flags(std::uint32_t flags){return flags&3U;}
constexpr bool weak_flags(std::uint32_t flags){return mode_flags(flags)==2;}
struct Packet {
    std::uint64_t token{},object{},owner{};
    WindowsGuid iid{};
    std::uint32_t flags{};
    bool standard{};
    bool operator==(const Packet&)const=default;
};
struct Wire {std::uint64_t token{},object{};std::uint32_t pid{},flags{};bool operator==(const Wire&)const=default;};
struct RemoteWire {
    com_rpc::Interface interface;std::uint64_t packet{};std::uint32_t flags{},context{};
    bool operator==(const RemoteWire&)const=default;
};
inline constexpr std::size_t remote_wire_size=112;
std::vector<std::uint8_t> encode(Wire);
std::optional<Wire> decode(std::span<const std::uint8_t>);
std::vector<std::uint8_t> encode(const RemoteWire&);
std::optional<RemoteWire> decode_remote(std::span<const std::uint8_t>);
bool remote_prefix(std::span<const std::uint8_t>);
// 核心只記錄引用的所有權；實際 AddRef／Release 由客體 callback driver 執行。
class Registry {
public:
    std::uint64_t publish(Packet);
    const Packet* find(std::uint64_t)const;
    std::optional<Packet> consume(std::uint64_t); // NORMAL 移轉引用；table 保留索引。
    std::optional<Packet> release(std::uint64_t);
    std::vector<Packet> retire_standard(std::uint64_t owner,const std::function<bool(std::uint64_t)>& retained={});
    void move_standard_owner(std::uint64_t from,std::uint64_t to);
    std::size_t size()const noexcept{return packets_.size();}
    bool operator==(const Registry&)const=default;
private:
    std::uint64_t next_{1};std::map<std::uint64_t,Packet> packets_;
};
}
