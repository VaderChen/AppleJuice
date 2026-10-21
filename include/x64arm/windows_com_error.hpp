#pragma once
#include <cstdint>
#include <map>

namespace x64arm {
enum class ComErrorViewKind { Info, Create, Support, Restricted };
struct ComErrorView {
    std::uint64_t object{};
    ComErrorViewKind kind{};
    bool operator==(const ComErrorView&) const = default;
};
struct ComErrorOperation {
    std::uint64_t thread{}, object{};
    bool operator==(const ComErrorOperation&) const = default;
};
struct RestrictedErrorOperation {
    enum class Phase { Query, Set, Cleanup } phase{Phase::Query};
    std::uint64_t original{}, queried{}, scratch{}, output{};
    std::uint32_t result{};
    bool setting{};
    bool operator==(const RestrictedErrorOperation&) const = default;
};
struct WinrtOriginOperation {
    std::uint64_t object{}, payload{};
    std::uint32_t hresult{}, length{};
    bool exception{};
    bool operator==(const WinrtOriginOperation&) const = default;
};
struct ComErrorBridge {
    std::map<std::uint64_t,ComErrorView> views;
    std::map<std::uint64_t,ComErrorOperation> operations;
    std::map<std::uint64_t,RestrictedErrorOperation> restricted_operations;
    std::map<std::uint64_t,WinrtOriginOperation> origins;
    std::uint64_t info_table{}, create_table{}, support_table{}, restricted_table{}, next_operation{1};
    bool operator==(const ComErrorBridge&) const = default;
};
}
