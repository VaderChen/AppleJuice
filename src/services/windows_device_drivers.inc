// psapi 的裝置驅動程式列舉（由 windows_process.cpp 的 invoke_process 引入；kernel32 的 K32* 為同名別名）。
// 與 NtQuerySystemInformation(SystemModuleInformation) 共用 capture_kernel_modules：同一 sandbox 已載入的核心驅動及映射依賴，
// 中等完整性以下拒絕存取。32 位元程序取得的位址截為 32 位元。EnumDeviceDrivers 寫出 ImageBase 陣列與所需位元組數；GetDeviceDriverBaseName／FileName 以 ImageBase 查詢，
// 依 nSize 截斷並回傳複製的字元數（不含結尾 NUL），找不到時回報 ERROR_INVALID_HANDLE。完整路徑與模組資訊相同：磁碟機路徑加上「\??\」。
if(service.family=="psapi.dll" && (service.symbol=="EnumDeviceDrivers" || service.symbol.starts_with("GetDeviceDriver"))){
    const WindowsCall arg(state,memory,service);const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    const auto finish=[&](std::uint64_t value,std::uint32_t error=0){if(error)last_error_=error;arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    try{
        initialize_tokens();if(runtime_.token_objects.at(effective_token_id()).integrity<0x2000U)return finish(0,5);
        std::vector<KernelModuleRecord> modules;
        if(const auto error=capture_kernel_modules(memory,modules))return finish(0,error==8?8U:error==50?50U:31U);
        if(service.symbol=="EnumDeviceDrivers"){
            if(!arg(2))return finish(0,87);
            const auto capacity=static_cast<std::uint32_t>(arg(1))/unit;
            for(std::size_t i=0;i<modules.size() && i<capacity;++i)memory.store(GuestAddress{arg(0)+i*unit},bits,modules[i].base);
            memory.store(GuestAddress{arg(2)},32,modules.size()*unit);return finish(1);
        }
        // 32 位元程序看到的 ImageBase 已截成 32 位元（與 SystemModuleInformation 相同），查詢時以同樣寬度比對。
        const auto found=std::find_if(modules.begin(),modules.end(),[&](const KernelModuleRecord& module){return (x86_?module.base&UINT32_MAX:module.base)==arg(0);});
        if(found==modules.end())return finish(0,6);
        auto text=found->path;
        if(service.symbol.starts_with("GetDeviceDriverBaseName")){const auto slash=text.find_last_of(u"\\/");if(slash!=text.npos)text.erase(0,slash+1);}
        else if(text.size()>2 && text[1]==u':')text=u"\\??\\"+text;
        const auto size=static_cast<std::uint32_t>(arg(2));if(!size)return finish(0,122);if(!arg(1))return finish(0,87);
        if(service.symbol.ends_with('W')){
            const auto count=std::min<std::size_t>(text.size(),size-1);auto bytes=utf16_bytes(std::u16string_view(text).substr(0,count));
            memory.write(GuestAddress{arg(1)},bytes);return finish(count);
        }
        auto narrow=utf16_to_utf8(text);if(narrow.size()>size-1)narrow.resize(size-1);narrow.push_back(0);
        memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(narrow.data()),narrow.size()));return finish(narrow.size()-1);
    }catch(const MemoryFault&){return finish(0,998);}catch(const std::bad_alloc&){return finish(0,8);}
}
