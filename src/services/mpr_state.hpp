#pragma once
#include "x64arm/network_resources.hpp"
#include <map>

namespace x64arm {
struct MprState {
    struct Enumeration {std::string scope;std::vector<network_resources::Resource> values;std::size_t position{};};
    std::shared_ptr<network_resources::Backend> backend;
    std::map<std::uint64_t,Enumeration> enumerations;
    std::map<std::string,std::map<std::u16string,network_resources::Credentials>> credentials;
    std::uint64_t next_enumeration{0x620000};
};
}
