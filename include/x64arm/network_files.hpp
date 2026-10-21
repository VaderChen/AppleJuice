#pragma once
#include "x64arm/network_resources.hpp"
#include <atomic>
#include <map>
#include <mutex>

namespace x64arm {
struct NetworkFileBinding {
    std::u16string remote,user;
    char drive{};
    bool readonly{};
    std::shared_ptr<network_resources::Mount> mount;
    std::atomic<unsigned> leases{};
};
struct NetworkFileLocation {
    std::shared_ptr<NetworkFileBinding> binding;
    std::u16string suffix;
    std::uint32_t error{};
    bool matched{};
    std::shared_ptr<void> lease;
};
class NetworkFiles {
public:
    std::vector<std::shared_ptr<NetworkFileBinding>> connections(const std::string&)const;
    NetworkFileLocation locate(const std::string&,std::u16string_view)const;
    std::uint32_t add(const std::string&,std::shared_ptr<NetworkFileBinding>);
    std::uint32_t remove(const std::string&,std::u16string_view,bool force);
    static std::shared_ptr<void> lease(std::shared_ptr<NetworkFileBinding>);
private:
    mutable std::mutex mutex_;
    std::map<std::string,std::vector<std::shared_ptr<NetworkFileBinding>>> sessions_;
};
}
