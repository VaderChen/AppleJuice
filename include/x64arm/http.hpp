#pragma once
#include "x64arm/process_io.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace x64arm {
using HttpHeaders=std::vector<std::pair<std::string,std::string>>;
struct HttpResult {
    std::uint32_t error{},status{};
    std::string status_line;
    std::string effective_url;
    HttpHeaders headers;
    std::vector<std::uint8_t> body;
    bool operator==(const HttpResult&) const = default;
};
struct HttpInput {
    std::string url,method,proxy,bypass,username,password;
    bool follow_redirects{true};
    std::uint32_t redirect_policy{1},max_redirects{10},disabled_features{},decompression{},secure_protocols{};
    HttpHeaders headers;
    std::vector<std::uint8_t> body;
    std::array<std::int32_t,4> timeouts{0,60000,30000,30000};
    bool operator==(const HttpInput&) const = default;
};
class NativeHttp {
public:
    virtual ~NativeHttp()=default;
    virtual bool done() const=0;
    virtual HttpResult take()=0;
    // 記憶體快取沒有網路傳輸；真正的宿主傳輸覆寫快照與取消。
    virtual process_io::Counters io_counters() const {return {};}
    virtual void cancel() noexcept {}

};
std::shared_ptr<NativeHttp> start_native_http(HttpInput);
struct HttpHandle {
    enum class Kind {Session,Connection,Request,Resolver,Probe} kind{Kind::Session};
    std::uint64_t parent{},context{};
    std::uint64_t callback{};
    std::uint32_t callback_flags{};
    bool asynchronous{};
    std::map<std::uint64_t,std::uint64_t> opening; // 每個客體執行緒各自保留同步 InternetOpenUrl 的未完成請求
    std::string agent,host,path;
    std::uint16_t port{};
    bool secure{},sent{},received{};
    bool inet{},cache_hit{};
    std::uint32_t cache_flags{};
    std::uint32_t total{};
    std::size_t cursor{};
    HttpInput input;
    HttpResult response;
    std::shared_ptr<NativeHttp> pending;
    process_io::Counters io_accounted;
    bool operator==(const HttpHandle&) const = default;
};
struct HttpState {
    bool cache_active{};
    std::uint64_t next{0x68000000};
    std::map<std::uint64_t,HttpHandle> handles;
    std::map<std::uint64_t,std::uint64_t> connection_probes;
    std::string nearest_server;
    HttpState()=default;
    HttpState(const HttpState&);
    HttpState& operator=(const HttpState&);
    HttpState(HttpState&&) noexcept=default;
    HttpState& operator=(HttpState&&) noexcept=default;
    bool operator==(const HttpState&) const = default;
};
}
