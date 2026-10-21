#pragma once
#include "x64arm/http.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace x64arm {
struct AutoProxyInput {
    HttpInput http;
    std::string target_url,script_url;
    std::uint32_t detect_flags{},flags{};
    bool discovery_only{};
};
struct AutoProxyEntry {
    std::string host;
    std::uint32_t scheme{};
    std::uint16_t port{};
    bool proxy{},bypass{};
    bool operator==(const AutoProxyEntry&) const = default;
};
struct AutoProxyResult {
    std::uint32_t error{};
    std::string script_url,proxy;
    std::vector<AutoProxyEntry> entries;
};
class NativeAutoProxy {
public:
    virtual ~NativeAutoProxy()=default;
    virtual bool done() const=0;
    virtual AutoProxyResult take()=0;
    virtual void cancel() noexcept {}
};
std::shared_ptr<NativeAutoProxy> start_native_autoproxy(AutoProxyInput);
struct PendingAutoProxy {
    std::uint64_t session{};
    std::shared_ptr<NativeAutoProxy> task;
};
}
