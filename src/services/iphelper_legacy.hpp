#pragma once
#include <span>
#include "x64arm/network.hpp"
#include <string_view>
#include <span>

namespace x64arm::iphelper {
struct Blob {
    std::vector<std::uint8_t> bytes;
    std::vector<std::size_t> pointers;
    void relocate(std::uint64_t base,unsigned unit);
};
bool capture_metadata(NetworkConfiguration&,bool* dns_captured=nullptr);
// 介面的本地化顯示名稱（SCNetworkInterfaceCopyAll 約 15 ms）；只在輸出描述字串的 API 第一次需要時取得。
void capture_interface_descriptions(NetworkConfiguration&);
std::uint32_t dhcp_inform(const NetworkInterface&,std::span<const std::uint8_t> requested,const std::map<std::uint8_t,std::vector<std::uint8_t>>& send,std::span<const std::uint8_t> class_id,std::map<std::uint8_t,std::vector<std::uint8_t>>& result);
Blob adapters(const NetworkConfiguration&,bool x86);
Blob fixed_info(const NetworkConfiguration&,bool x86);
std::vector<std::uint8_t> interface_row(const NetworkInterface&);
Blob interface_table(const NetworkConfiguration&,bool sorted);
}
