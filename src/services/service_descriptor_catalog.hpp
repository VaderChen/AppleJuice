#pragma once
#include "x64arm/windows_abi.hpp"
#include <functional>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace x64arm::detail {
struct ServiceDescriptorGroup {
    std::span<const WindowsServiceDescriptor> descriptors;
    bool unique{};
};

inline std::vector<WindowsServiceDescriptor> combine_service_descriptors(
    std::span<const ServiceDescriptorGroup> groups) {
    using Key=std::pair<std::string_view,std::string_view>;
    struct Hash {
        std::size_t operator()(const Key& key) const noexcept {
            const auto family=std::hash<std::string_view>{}(key.first);
            const auto symbol=std::hash<std::string_view>{}(key.second);
            return family ^ (symbol+0x9e3779b9U+(family<<6)+(family>>2));
        }
    };
    std::size_t capacity=0;
    for(const auto& group:groups)capacity+=group.descriptors.size();
    std::vector<WindowsServiceDescriptor> result;
    result.reserve(capacity);
    std::unordered_set<Key,Hash> seen;
    seen.reserve(capacity);
    // 索引只用於排重，不排序輸出。原本直接附加的 provider 保留重複項；
    // 排重的 provider 維持先出現的家族／名稱與完整 ABI 優先權。
    for(const auto& group:groups)for(const auto& item:group.descriptors){
        const auto first=seen.emplace(item.family,item.symbol).second;
        if(!group.unique || first)result.push_back(item);
    }
    return result;
}
}
