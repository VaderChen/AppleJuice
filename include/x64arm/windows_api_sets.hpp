#pragma once
#include <string_view>

namespace x64arm {
// Loader 模組辨識與函式解析共用同一份契約；未列出的 API-set 不冒用其他 DLL。
constexpr std::string_view windows_api_set_host(std::string_view name){
    if(name=="kernelbase.dll")return "kernel32.dll";
    if(name=="api-ms-win-downlevel-kernel32-l2-1-0.dll")return "kernel32.dll";
    if(name=="api-ms-win-eventing-provider-l1-1-0.dll" || name=="api-ms-win-security-base-l1-1-0.dll")return "advapi32.dll";
    if(name.starts_with("api-ms-win-security-systemfunctions-"))return "advapi32.dll"; // SystemFunction036／040／041
    if(name=="api-ms-win-power-base-l1-1-0.dll" || name=="api-ms-win-power-setting-l1-1-0.dll")return "powrprof.dll";
    if(name.starts_with("api-ms-win-core-path-"))return "pathcch.dll";
    if(name.starts_with("api-ms-win-core-localregistry-"))return "advapi32.dll";
    if(name.starts_with("api-ms-win-core-winrt-"))return "combase.dll";
    if(name.starts_with("api-ms-win-core-") || name.starts_with("api-ms-win-appmodel-runtime-"))return "kernel32.dll";
    if(name.starts_with("ext-ms-win-ntuser-powermanagement-"))return "user32.dll";
    if(name.starts_with("api-ms-win-devices-config-"))return "cfgmgr32.dll";
    if(name=="api-ms-win-shcore-scaling-l1-1-1.dll")return "shcore.dll";
    if(name=="api-ms-win-downlevel-shell32-l1-1-0.dll")return "shell32.dll";
    return name;
}
}
