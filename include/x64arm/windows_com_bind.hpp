#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace x64arm {
struct ComBindReference {
    std::uint64_t owner{},object{};
    std::optional<std::uint64_t> apartment;
    bool operator==(const ComBindReference&)const=default;
};
struct ComBindObject {
    std::uint32_t references{1},pins{};
    std::array<std::uint8_t,48> options{};
    std::vector<ComBindReference> bound;
    std::map<std::u16string,ComBindReference> parameters;
    bool operator==(const ComBindObject&)const=default;
};
struct ComBindEnum {
    std::uint32_t references{1};std::vector<std::u16string> names;std::size_t cursor{};
    bool operator==(const ComBindEnum&)const=default;
};
struct ComBindOperation {
    enum class Kind {Bound,Set,Get} kind{};
    std::uint64_t context{},output{},scratch{},delivered{},caller{};
    ComBindReference reference;std::u16string key;
    bool awaiting{},acquired{},borrowed{},proxy{};
    bool operator==(const ComBindOperation&)const=default;
};
struct ComBindBridge {
    using ReferenceKey=std::pair<std::uint64_t,std::uint64_t>;
    std::map<std::uint64_t,ComBindObject> objects;
    std::map<std::uint64_t,ComBindEnum> enumerators;
    std::map<std::uint64_t,ComBindOperation> operations;
    std::map<ReferenceKey,std::size_t> borrowed;
    std::map<ReferenceKey,std::vector<ComBindReference>> deferred;
    std::uint64_t table{},enum_table{},next{1};
    bool operator==(const ComBindBridge&)const=default;
};
}
