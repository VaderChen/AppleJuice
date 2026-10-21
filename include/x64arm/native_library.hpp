#pragma once
#include "x64arm/windows_abi.hpp"
#include <set>
#include <string>

namespace x64arm {
// 代理只按函式庫契約選擇，不以應用程式、位址或檔案雜湊識別。
std::span<const WindowsServiceDescriptor> native_library_service_descriptors();
bool native_library_module(std::string_view dll);
class NativeLibraries {
public:
    void configure(std::string_view names);
    bool enabled(std::string_view dll) const;
    const std::set<std::string>& configuration() const noexcept{return enabled_;}
    std::uint64_t invoke(GuestMemory&,const WindowsCall&,const WindowsServiceDescriptor&);
    std::string diagnostics() const;
private:
    std::set<std::string> enabled_;
    std::uint64_t calls_{},input_bytes_{},output_bytes_{};
};
}
