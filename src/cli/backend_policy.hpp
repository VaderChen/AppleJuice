#pragma once
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace x64arm::cli {
inline void reject_retired_backends() {
    const auto reject=[](const char* key){
        throw std::invalid_argument(std::string(key)+" 要求已封閉的舊 JIT／LLVM 後端；請移除該設定並使用 resident AOT。");
    };
    if(const auto* value=std::getenv("X64ARM_JIT_MODULE_AOT");value && std::string_view(value)!="1")
        reject("X64ARM_JIT_MODULE_AOT");
    for(const auto* key:{"X64ARM_JIT_LLVM","X64ARM_JIT_LLVM_TIER","X64ARM_AOT_LLVM"})
        if(const auto* value=std::getenv(key);value && *value && std::string_view(value)!="0")reject(key);
}
}
