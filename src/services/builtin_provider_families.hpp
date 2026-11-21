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
    // FidelityFX API 的三個 DLL（載入器、升頻提供者、3.1.0～3.1.3 的單一 DLL）匯出同一組五個函式，共用內建實作。
    if(name=="amd_fidelityfx_upscaler_dx12.dll" || name=="amd_fidelityfx_dx12.dll")return "amd_fidelityfx_loader_dx12.dll";
    // VC8（msvcp80）與 VC9（msvcp90）的 STL 物件版面相同：basic_string 為 allocator（補齊到指標大小）、16-byte SSO、size、capacity，
    // iostream／locale 物件也相同；兩版共有的匯出名稱（含完整參數型別的修飾名）共用 VC9 實作。只在 VC8 出現的修飾名（如單參數 _Getcat）
    // 與 VC9 名稱不同，不會誤配。VC10 起 allocator 位置改變，不在此列。
    if(name=="msvcp80.dll")return "msvcp90.dll";
    return name;
}
// ImageHlp 的符號／堆疊家族轉送 DbgHelp；憑證等本身的服務仍留在 ImageHlp。
constexpr std::string_view builtin_provider_export_family(std::string_view family,std::string_view symbol){
    // VC10～VC12 把 ConcRT 匯出放在 msvcr100／110／120，並行容器基底放在 msvcp100／110／120；同名匯出的物件版面與 concrt140 相同。
    if((family=="msvcr100.dll" || family=="msvcr110.dll" || family=="msvcr120.dll" || family=="msvcp100.dll" || family=="msvcp110.dll" || family=="msvcp120.dll") &&
       symbol.find("@Concurrency@@")!=symbol.npos && symbol.find("@platform@details@Concurrency@@")==symbol.npos)return "concrt140.dll";
    // Concurrency::details::platform（執行緒識別碼等）在 VC14 移到 msvcp140，不屬 concrt140；由各自的別名對應。
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
static_assert(builtin_provider_family("msvcp80.dll")=="msvcp90.dll" && builtin_provider_family("msvcp100.dll")=="msvcp100.dll");
static_assert(builtin_provider_export_family("imagehlp.dll","ImageAddCertificate")=="imagehlp.dll");
static_assert(builtin_provider_export_family("msvcr120.dll","?GetCurrentThreadId@platform@details@Concurrency@@YAJXZ")=="msvcr120.dll");
}
