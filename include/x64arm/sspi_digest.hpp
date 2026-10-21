#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm::sspi_digest {
constexpr std::uint32_t invalid_handle=0x80090301,unsupported=0x80090302,invalid_token=0x80090308;
constexpr std::uint32_t logon_denied=0x8009030c,no_credentials=0x8009030e,altered=0x8009030f,out_of_sequence=0x80090310;
constexpr std::uint32_t continuing=0x90312,complete_needed=0x90313,complete_continue=0x90314;
constexpr std::uint64_t cookie=UINT64_C(0x5744494745535431);
struct Error {std::uint32_t status;};
struct Secret {
    explicit Secret(std::string value):value(std::move(value)){}
    ~Secret();
    Secret(const Secret&)=delete;Secret& operator=(const Secret&)=delete;
    std::string value;
};
struct Credentials {
    std::string user,domain;
    std::shared_ptr<const Secret> password;
    std::uint32_t use{};
    bool operator==(const Credentials&) const;
};
struct Context {
    enum class Phase {Initial,Challenge,ClientPending,ServerPending,PeerProof,ServerAck,Established};
    Credentials credentials;Phase phase{Phase::Initial};
    bool server{},http{},utf8{},mutual{};
    std::uint32_t flags{},nc{};
    std::string realm,nonce,cnonce,qop,algorithm,uri,opaque,method,entity;
    std::shared_ptr<const Secret> key;
    bool operator==(const Context&) const;
};
struct State {
    std::map<std::uint64_t,Credentials> credentials;
    std::map<std::uint64_t,Context> contexts;
    std::uint64_t next{0xd1000000};
    bool operator==(const State&) const = default;
};
using Directives=std::map<std::string,std::string>;
Directives parse(std::string_view,bool repeated_realms=false);
std::string encode(const Directives&);
std::string hash(std::string_view);
bool equal(std::string_view,std::string_view);
struct Result {std::string token;std::uint32_t status{};};
// 相容層使用同一狀態副本提交；短 buffer 或驗證失敗不推進握手。
Result client(Context&,std::string_view challenge,std::string_view target,std::string_view method,std::string_view entity,std::string_view random);
Result server(Context&,std::string_view response,std::string_view realm,std::string_view method,std::string_view entity,std::string_view random);
Result complete(Context&,std::string_view token,std::string_view method,std::string_view entity);
}
