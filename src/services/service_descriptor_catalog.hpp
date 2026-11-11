#pragma once
#include "x64arm/windows_abi.hpp"
#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace x64arm::detail {
struct ServiceDescriptorGroup {
    std::span<const WindowsServiceDescriptor> descriptors;
    bool unique{};
    std::string_view name{};
};
// 稽核用的合併紀錄：每筆保留項目的來源群組，以及因排重被捨棄的項目（連同它讓位給哪一群組的哪一筆）。
struct ServiceDescriptorDropped {WindowsServiceDescriptor descriptor;std::uint16_t group{};std::size_t kept{};};
struct ServiceDescriptorAudit {
    std::vector<std::string_view> groups;std::vector<bool> unique;
    std::vector<std::uint16_t> origin;std::vector<ServiceDescriptorDropped> dropped;
};

inline std::vector<WindowsServiceDescriptor> combine_service_descriptors(
    std::span<const ServiceDescriptorGroup> groups,ServiceDescriptorAudit* audit=nullptr) {
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
    std::unordered_map<Key,std::size_t,Hash> seen;
    seen.reserve(capacity);
    // 索引只用於排重，不排序輸出。原本直接附加的 provider 保留重複項；
    // 排重的 provider 維持先出現的家族／名稱與完整 ABI 優先權。
    std::uint16_t index=0;
    for(const auto& group:groups){
        if(audit){audit->groups.push_back(group.name);audit->unique.push_back(group.unique);}
        for(const auto& item:group.descriptors){
            const auto [found,first]=seen.try_emplace(Key{item.family,item.symbol},result.size());
            if(!group.unique || first){result.push_back(item);if(audit)audit->origin.push_back(index);}
            else if(audit)audit->dropped.push_back({item,index,found->second});
        }
        ++index;
    }
    return result;
}
}
