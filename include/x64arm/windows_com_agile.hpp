#pragma once
#include "x64arm/windows_guid.hpp"
#include <map>

namespace x64arm {
struct ComAgileObject {
    std::uint64_t source{},stream{},context{},owner{};
    WindowsGuid iid{};
    bool agile{},delayed{};
    bool operator==(const ComAgileObject&)const=default;
};
struct ComAgileOperation {
    enum class Kind { Create,Resolve,Prepare,Dispose,Cleanup } kind{};
    unsigned step{};
    std::uint64_t object{},output{},scratch{},temporary{},stream{},source{},context{},owner{},parent{};
    std::uint32_t result{},options{};
    WindowsGuid iid{};
    bool agile{},source_owned{},stream_owned{},packet_owned{},context_owned{};
    bool operator==(const ComAgileOperation&)const=default;
};
struct ComAgileBridge {
    std::map<std::uint64_t,ComAgileObject> objects;
    std::map<std::uint64_t,ComAgileOperation> operations;
    std::uint64_t table{},next{1};
    bool operator==(const ComAgileBridge&)const=default;
};
}
