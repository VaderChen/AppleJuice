#include "x64arm/network_registry.hpp"
#include "x64arm/unicode.hpp"
#include <arpa/inet.h>
#include <array>
#include <stdexcept>
#include <utility>

namespace x64arm {
namespace {
const std::array<std::u16string,3> parameters{
    u"\x0002\\system\\currentcontrolset\\services\\tcpip\\parameters",
    u"\x0002\\system\\currentcontrolset\\services\\tcpip6\\parameters",
    u"\x0002\\system\\currentcontrolset\\services\\dnscache\\parameters"};
std::u16string nameservers(const NetworkConfiguration& config, unsigned family) {
    std::u16string result;
    for (const auto& address : config.dns) {
        const auto offset=family==2?4U:8U, size=family==2?16U:28U;
        if (address.size()!=size || address[0]!=family || address[1]) continue;
        std::array<char,INET6_ADDRSTRLEN> text{};
        if (!::inet_ntop(family==2?AF_INET:AF_INET6,address.data()+offset,text.data(),static_cast<socklen_t>(text.size()))) continue;
        if (!result.empty()) result.push_back(u' ');
        result+=utf8_to_utf16(text.data());
        if(family==23) {
            std::uint32_t scope{};
            for(unsigned i=0;i<4;++i) scope|=std::uint32_t{address[24+i]}<<(8*i);
            if(scope) result+=u"%"+utf8_to_utf16(std::to_string(scope));
        }
    }
    return result;
}
}
bool network_registry_present(const RegistryState& state) {
    for (const auto& path : parameters) {
        for (auto end=path.find(u'\\');;end=path.find(u'\\',end+1)) {
            if (!state.keys.contains(path.substr(0,end))) return false;
            if (end==path.npos) break;
        }
    }
    return true;
}
bool initialize_network_registry(RegistryState& state, const NetworkConfiguration& config) {
    if (network_registry_present(state)) return false;
    auto next=state;
    for (std::size_t service=0;service<parameters.size();++service) {
        const auto& path=parameters[service];
        for (auto end=path.find(u'\\');;end=path.find(u'\\',end+1)) {
            next.keys.try_emplace(path.substr(0,end));
            if (end==path.npos) break;
        }
        if (state.keys.contains(path)) continue;
        const auto string=[&](std::u16string name, std::u16string_view value) {
            auto bytes=utf16_bytes(value);
            next.bytes+=bytes.size();++next.values;
            next.keys.at(path).emplace(std::move(name),RegistryValue{1,std::move(bytes)});
        };
        // 設定值來自與 IP Helper 共用的網路快照；未知值保持不存在。
        if (service==0 && config.metadata_available) {
            string(u"hostname",utf8_to_utf16(config.host_name));
            string(u"domain",utf8_to_utf16(config.domain_name));
        }
        if (service<2) {
            const auto servers=nameservers(config,service==0?2U:23U);
            if (!servers.empty()) string(u"nameserver",servers);
        }
    }
    if (next.keys.size()>4096 || next.values>4096 || next.bytes>16U*1024U*1024U)
        throw std::length_error("網路系統登錄初始化超過儲存範圍");
    state=std::move(next);
    return true;
}
}
