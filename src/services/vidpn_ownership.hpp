#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace x64arm {
struct GuiState;
// type 無值時查詢；0 釋放，device 為 0 時釋放整個程序。pid 為 0 的查詢涵蓋所有程序。
std::uint32_t vidpn_ownership(GuiState&,const std::string& directory,std::uint32_t self,std::uint32_t pid,
                              std::uint32_t device,std::optional<std::uint32_t> type,std::uint32_t& current,std::uint32_t* owner_pid=nullptr);
}
