#pragma once
#include "x64arm/com_context.hpp"

namespace x64arm {
struct ComContextEnumerator {
    com_context::Enumeration snapshot;
    std::uint32_t references{1};
    bool operator==(const ComContextEnumerator&) const = default;
};
enum class ComContextOperationKind { Set, Get, Enumerate, Clone, Next };
struct ComContextOperation {
    ComContextOperationKind kind{};
    std::uint64_t context{}, enumerator{}, output{}, flags_output{}, fetched_output{}, result_object{};
    std::uint32_t requested{};
    com_context::Property property{};
    std::vector<com_context::Property> properties;
    std::size_t acquired{};
    bool awaiting_reference{};
    bool operator==(const ComContextOperation&) const = default;
};
struct ComContextBridge {
    com_context::State contexts;
    std::map<std::uint64_t,ComContextEnumerator> enumerators;
    std::map<std::uint64_t,ComContextOperation> operations;
    // 尚在取得快照引用時，來源持有的引用不能被重入 Remove／Uninitialize 提早釋放。
    std::map<std::uint64_t,std::size_t> borrowed, deferred_releases;
    std::uint64_t table{}, enum_table{}, next_operation{1};
    bool operator==(const ComContextBridge&) const = default;
};
}
