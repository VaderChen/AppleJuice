#pragma once
#include "x64arm/com_proxy.hpp"
#include <string>

namespace x64arm {
struct ComProxyInput {
    std::uint64_t original{},acquired{},cell{};
    unsigned argument{};
    com_proxy::Kind kind{};
    bool transfer{};
    bool operator==(const ComProxyInput&) const = default;
};
struct ComProxyOutput {
    std::uint64_t destination{},buffer{},size{},count{1},fetched{};
    com_proxy::Kind kind{};
    int iid_argument{-1};
    enum class Layout { Interface,Medium,StatData,InMedium,Plain } layout{Layout::Interface};
    bool operator==(const ComProxyOutput&) const = default;
};
struct ComProxyField {
    std::uint64_t cell{};
    std::optional<com_proxy::Kind> kind{};
    bool operator==(const ComProxyField&) const = default;
};
struct ComProxyOperation {
    enum class Type { Method,Marshal,Retire } type{Type::Method};
    enum class Stage { Begin,Borrow,Invoke,Fields,Export,Release,Done } stage{Stage::Begin};
    std::uint64_t sender{},receiver{},object{},proxy{},retirement{},error_pointer{},rpc{},frame_stack{},frame_rip{};
    std::uint64_t result{},scratch_error{},marshal_recipient{},pin_apartment{},marshal_query_output{};
    std::uint32_t error{};
    com_proxy::Kind kind{};
    std::string symbol;
    std::vector<std::uint64_t> arguments,scratch,retirements;
    std::vector<com_proxy::Origin> exported;
    std::vector<ComProxyInput> inputs;
    std::vector<ComProxyOutput> outputs;
    std::vector<ComProxyField> fields;
    std::size_t input{},field{},release{};
    bool active{},abandoned{},acquired{},retirement_started{},success{};
    bool operator==(const ComProxyOperation&) const = default;
};
struct ComProxyBridge {
    com_proxy::Registry objects;
    std::map<com_proxy::Kind,std::uint64_t> vtables;
    std::map<std::uint64_t,ComProxyOperation> operations;
    std::uint64_t next{1};
    bool operator==(const ComProxyBridge&) const = default;
};
}
