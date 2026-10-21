#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace x64arm::network_resources {
inline constexpr std::u16string_view provider_name=u"Microsoft Windows Network";
struct Path {
    std::u16string server,share,system;
    std::u16string remote()const;
    bool operator==(const Path&)const=default;
};
std::optional<Path> parse_path(std::u16string_view);
std::u16string canonical(std::u16string);
struct Resource {
    std::uint32_t scope{2},type{1},display{3},usage{1};
    std::optional<std::u16string> local,remote,comment;
    std::u16string provider{provider_name};
    bool operator==(const Resource&)const=default;
};
// NETRESOURCE 的指標皆指向呼叫者的同一輸出區塊，A/W 字串由服務層提供。
struct PackedResource {Resource value;std::vector<std::vector<std::uint8_t>> strings;};
std::size_t packed_size(std::span<const PackedResource>,unsigned pointer_size);
std::vector<std::uint8_t> pack(std::span<const PackedResource>,unsigned pointer_size,std::uint64_t address);
struct Credentials {std::u16string user,password;bool guest{};};
struct Server {std::u16string name,comment;};
struct Share {std::u16string name,comment;bool printer{},hidden{};};
struct Servers {std::vector<Server> values;std::uint32_t error{};};
struct Shares {std::vector<Share> values;std::uint32_t error{};};
class Mount {
public:
    virtual ~Mount()=default;
    virtual const std::filesystem::path& root()const=0;
    virtual std::uint32_t disconnect(bool force)=0;
};
struct Connection {std::shared_ptr<Mount> mount;std::uint32_t error{};};
class Backend {
public:
    virtual ~Backend()=default;
    virtual Servers servers()=0;
    virtual Shares shares(std::u16string_view server,const Credentials&)=0;
    virtual Connection connect(const Path&,const Credentials&,const std::filesystem::path& destination,bool readonly)=0;
};
std::shared_ptr<Backend> make_native_backend();
}
