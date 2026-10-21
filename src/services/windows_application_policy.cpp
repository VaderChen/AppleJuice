#include "x64arm/windows_services.hpp"
#include <algorithm>

namespace x64arm {
namespace {
struct DesktopPolicy {WindowsServiceDescriptor service;std::uint32_t value;};
// 此表描述一般 Win32 PE 的契約，並非依客體名稱選擇回傳值。
constexpr DesktopPolicy desktop_policies[]{
    {{"kernel32.dll","AppPolicyGetClrCompat",2},1},
    {{"kernel32.dll","AppPolicyGetCreateFileAccess",2},0},
    {{"kernel32.dll","AppPolicyGetLifecycleManagement",2},0},
    {{"kernel32.dll","AppPolicyGetMediaFoundationCodecLoading",2},0},
    {{"kernel32.dll","AppPolicyGetProcessTerminationMethod",2},0},
    {{"kernel32.dll","AppPolicyGetShowDeveloperDiagnostic",2},1},
    {{"kernel32.dll","AppPolicyGetThreadInitializationType",2},0},
    {{"kernel32.dll","AppPolicyGetWindowingModel",2},2}
};
}
std::span<const WindowsServiceDescriptor> application_policy_service_descriptors(){
    static const auto table=[] {std::array<WindowsServiceDescriptor,std::size(desktop_policies)> result{};
        for(std::size_t i=0;i<result.size();++i)result[i]=desktop_policies[i].service;return result;}();
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_application_policy(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);std::uint32_t error=0;initialize_tokens();
    const auto handle=arg(0),mask=x86_?UINT64_C(0xffffffff):UINT64_MAX;
    std::uint64_t id=0;
    if(!handle || !arg(1))error=87;
    else if(handle==mask-3)id=1; // GetCurrentProcessToken() 是 -4 的 inline pseudo handle。
    else if(handle==mask-4){
        const auto thread=threads_.items.find(threads_.current);
        if(thread==threads_.items.end() || !thread->second.impersonation_token)error=1008;
        else id=thread->second.impersonation_token;
    }else if(handle==mask-5)id=effective_token_id();
    else {
        const auto access=runtime_.token_handles.find(handle);
        if(access==runtime_.token_handles.end())error=6;
        else if(!(access->second&8U))error=5;
        else id=runtime_.token_handle_objects.at(handle);
    }
    if(!error)try{
        memory.check(GuestAddress{arg(1)},4,Access::Write);
        const auto policy=std::find_if(std::begin(desktop_policies),std::end(desktop_policies),[&](const auto& p){return p.service.symbol==service.symbol;});
        if(runtime_.token_objects.at(id).application_model!=ApplicationModel::ClassicDesktop || policy==std::end(desktop_policies)){
            // 無 app-model claims 的權杖遵守 ASSERTION_FAILURE + ERROR_NOT_FOUND 契約。
            auto caller=state;arg.return_to(caller,1168);begin_exception(caller,memory,0xc0000420U,0,{});state=caller;return std::nullopt;
        }
        memory.store(GuestAddress{arg(1)},32,policy->value);
    }catch(const MemoryFault&){error=998;}
    // LONG 錯誤碼直接回傳；保留 GetLastError。
    arg.return_to(state,error);return std::nullopt;
}
}
