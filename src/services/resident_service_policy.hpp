#pragma once
#include <string_view>

namespace x64arm::detail {
// 同一分類供 runnable 選取與協調 provider 使用；表示可在一致邊界處理，
// 不表示 worker 可以直接修改映射。跨程序資格仍由 provider 依 handle 核對。
enum class ResidentMappingService {None,Local,Process,Allocation2};
inline ResidentMappingService resident_mapping_service(std::string_view symbol) noexcept {
    if(symbol=="VirtualAlloc" || symbol=="VirtualFree" || symbol=="VirtualProtect" || symbol=="VirtualQuery")
        return ResidentMappingService::Local;
    if(symbol=="VirtualAllocEx" || symbol=="VirtualFreeEx" || symbol=="VirtualProtectEx" ||
       symbol=="VirtualQueryEx" || symbol=="VirtualAllocExNuma" || symbol=="FlushInstructionCache")
        return ResidentMappingService::Process;
    if(symbol=="VirtualAlloc2" || symbol=="VirtualAlloc2FromApp")return ResidentMappingService::Allocation2;
    return ResidentMappingService::None;
}
}
