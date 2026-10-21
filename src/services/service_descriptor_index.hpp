#pragma once
#include "x64arm/windows_abi.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <mutex>
#include <utility>
#include <vector>

namespace x64arm::detail {
// catalog 在程序期間不可變；索引只保存名稱，不保存服務實體或客體地址。
// 各 provider 仍依原分類順序查詢，重疊 API 不因排序而改變所屬 handler。
template<auto Catalog>
bool service_descriptor_index_contains(const WindowsServiceDescriptor& service) {
    using Key=std::pair<std::string_view,std::string_view>;
    struct Index {
        std::atomic<unsigned> queries{};std::atomic<bool> ready{};
        std::once_flag once;std::vector<Key> keys;
    };
    static Index index;
    if(!index.ready.load(std::memory_order_acquire)) {
        const auto descriptors=Catalog();
        // 一次性冷查詢沿用無配置掃描；累積線性工作足以攤平排序才建索引。
        // 小表不排序，避免比直接比較更高的初始化與間接存取成本。
        const auto threshold=static_cast<unsigned>(2*std::bit_width(descriptors.size()));
        if(descriptors.size()<=8 || index.queries.fetch_add(1,std::memory_order_relaxed)<threshold)
            return std::any_of(descriptors.begin(),descriptors.end(),[&](const auto& item){return item.family==service.family && item.symbol==service.symbol;});
        std::call_once(index.once,[&] {
            index.keys.reserve(descriptors.size());
            for(const auto& descriptor:descriptors)index.keys.emplace_back(descriptor.family,descriptor.symbol);
            std::sort(index.keys.begin(),index.keys.end());
            index.keys.erase(std::unique(index.keys.begin(),index.keys.end()),index.keys.end());
            index.ready.store(true,std::memory_order_release);
        });
    }
    return std::binary_search(index.keys.begin(),index.keys.end(),Key{service.family,service.symbol});
}
template<auto Catalog>
bool service_descriptor_contains(const WindowsServiceDescriptor& service,bool reuse=true) {
    // 首批 API 分類維持可內嵌的線性路徑，不初始化各 provider 的索引狀態。
    if(reuse)return service_descriptor_index_contains<Catalog>(service);
    const auto descriptors=Catalog();
    return std::any_of(descriptors.begin(),descriptors.end(),[&](const auto& item){return item.family==service.family && item.symbol==service.symbol;});
}
}
