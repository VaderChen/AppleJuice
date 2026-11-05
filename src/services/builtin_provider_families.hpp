#pragma once
#include <array>
#include <string_view>

namespace x64arm::detail {
// D3DX9 的版本 DLL 共用已實作的匯出 ABI；不載入系統目錄的可再散布二進位。
// 只辨識正式的兩位數 SDK 版本，其他名稱仍交既有載入規則。
constexpr std::string_view builtin_provider_family(std::string_view name){
    if(name.size()==12 && name.starts_with("d3dx9_") && name.ends_with(".dll") &&
       name[6]>='0' && name[6]<='9' && name[7]>='0' && name[7]<='9'){
        const auto version=(name[6]-'0')*10+(name[7]-'0');
        if(version>=24 && version<=43)return "d3dx9_43.dll";
    }
    return name;
}
// ImageHlp 的符號／堆疊家族轉送 DbgHelp；憑證等本身的服務仍留在 ImageHlp。
constexpr std::string_view builtin_provider_export_family(std::string_view family,std::string_view symbol){
    if(family=="imagehlp.dll"){
        constexpr std::array shared{"EnumerateLoadedModules","EnumerateLoadedModules64",
            "FindDebugInfoFile","FindDebugInfoFileEx","FindExecutableImage","FindExecutableImageEx",
            "GetTimestampForLoadedLibrary","ImageDirectoryEntryToData","ImageDirectoryEntryToDataEx",
            "ImageNtHeader","ImageRvaToSection","ImageRvaToVa","ImagehlpApiVersion","ImagehlpApiVersionEx",
            "MakeSureDirectoryPathExists","SearchTreeForFile","StackWalk","StackWalk64","UnDecorateSymbolName"};
        if(symbol.starts_with("Sym"))return "dbghelp.dll";
        for(const auto name:shared)if(symbol==name)return "dbghelp.dll";
    }
    return builtin_provider_family(family);
}
static_assert(builtin_provider_family("d3dx9_29.dll")=="d3dx9_43.dll");
static_assert(builtin_provider_family("d3dx9_23.dll")=="d3dx9_23.dll");
static_assert(builtin_provider_export_family("imagehlp.dll","ImageAddCertificate")=="imagehlp.dll");
}
