#include "x64arm/windows_services.hpp"
namespace x64arm {
std::optional<std::uint32_t> WindowsServices::invoke_native_library(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!native_libraries_.enabled(service.family))throw HostCallFault("原生函式庫代理未啟用");
    if(service.family=="x64arm_v8.dll")return invoke_native_v8(state,memory,service);
    const WindowsCall call(state,memory,service);
    const auto result=native_libraries_.invoke(memory,call,service);
    call.return_to(state,result);return {};
}
}
