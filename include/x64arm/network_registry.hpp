#pragma once
#include "x64arm/network.hpp"
#include "x64arm/registry.hpp"

namespace x64arm {
bool network_registry_present(const RegistryState&);
// 建立已提供的 TCP/IP 與 DNS 系統命名空間；不新增未設定的群組政策。
// 只初始化缺少的 Parameters，既有鍵與其自訂值保持原有內容。
bool initialize_network_registry(RegistryState&, const NetworkConfiguration&);
}
