// 啟用內容（activation context）家族：kernel32 的 *ActCtx* 與 ntdll 的 Rtl*ActivationContext* 共用同一份每執行緒堆疊與錯誤判定，
// 前者回 BOOL＋last error，後者回 NTSTATUS。沙箱尚未建立 SxS manifest／assembly 重導向，只有預設（NULL）內容：
// CreateActCtx 不回假內容，查詢與 section 搜尋依預設內容回報「沒有對應鍵」。x86 的 ULONG_PTR／HANDLE／SIZE_T 與 ACTCTX 版面依位元數處理。
#include "ansi_codepage.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
// Win32 錯誤 → NTSTATUS（本家族會用到的部分）。
std::uint32_t activation_status(unsigned error){
    switch(error){
    case 0:return 0;case 6:return 0xc0000008U;case 8:return 0xc0000017U;case 87:return 0xc000000dU;case 122:return 0xc0000023U;
    case 126:return 0xc0000135U;case 1814:return 0xc000008bU;case 14004:return 0xc0150001U;case 14007:return 0xc0150008U;case 2:return 0xc0000034U;
    default:return 0xc00000bbU;
    }
}
// QueryActCtxSettingsW 接受的 WindowsSettings 命名空間。
constexpr std::u16string_view settings_namespaces[]{u"http://schemas.microsoft.com/SMI/2005/WindowsSettings",u"http://schemas.microsoft.com/SMI/2011/WindowsSettings",
    u"http://schemas.microsoft.com/SMI/2016/WindowsSettings",u"http://schemas.microsoft.com/SMI/2017/WindowsSettings",
    u"http://schemas.microsoft.com/SMI/2019/WindowsSettings",u"http://schemas.microsoft.com/SMI/2020/WindowsSettings"};
}
std::span<const WindowsServiceDescriptor> activation_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","QueryActCtxW",7},{"kernel32.dll","CreateActCtxA",1},{"kernel32.dll","CreateActCtxW",1},
        {"kernel32.dll","ActivateActCtx",2},{"kernel32.dll","DeactivateActCtx",2},
        {"kernel32.dll","GetCurrentActCtx",1},{"kernel32.dll","AddRefActCtx",1},{"kernel32.dll","ReleaseActCtx",1},{"kernel32.dll","ZombifyActCtx",1},
        {"kernel32.dll","FindActCtxSectionStringA",5},{"kernel32.dll","FindActCtxSectionStringW",5},{"kernel32.dll","FindActCtxSectionGuid",5},
        {"kernel32.dll","QueryActCtxSettingsW",7},
        {"ntdll.dll","RtlActivateActivationContext",3,0,0,0x1},{"ntdll.dll","RtlActivateActivationContextEx",4,0,0,0x1},{"ntdll.dll","RtlDeactivateActivationContext",2,0,0,0x1},
        {"ntdll.dll","RtlAddRefActivationContext",1},{"ntdll.dll","RtlReleaseActivationContext",1},{"ntdll.dll","RtlZombifyActivationContext",1},
        {"ntdll.dll","RtlGetActiveActivationContext",1},{"ntdll.dll","RtlIsActivationContextActive",1},
        {"ntdll.dll","RtlQueryInformationActivationContext",7,0,0,0x9},{"ntdll.dll","RtlQueryInformationActiveActivationContext",4,0,0,0x1},
        {"ntdll.dll","RtlFindActivationContextSectionString",5,0,0,0x5},{"ntdll.dll","RtlFindActivationContextSectionGuid",5,0,0,0x5},
        {"ntdll.dll","RtlFreeThreadActivationContextStack",0},{"ntdll.dll","RtlFreeActivationContextStack",1},
        {"ntdll.dll","RtlQueryActivationContextApplicationSettings",7,0,0,0x1}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_activation(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);auto symbol=service.symbol;
    const bool native=service.family=="ntdll.dll";const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto dword=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    auto stack=runtime_.activation_stacks.find(threads_.current);
    const auto active=stack==runtime_.activation_stacks.end() || stack->second.empty()?0:stack->second.back().second;
    // ---- 共用核心：回傳 Win32 錯誤碼（0 為成功） ----
    const auto activate=[&](std::uint64_t context,std::uint64_t cookie_out)->unsigned{
        if(context)return 6;if(!cookie_out)return 87;
        memory.check(GuestAddress{cookie_out},unit,Access::Write);auto& frames=runtime_.activation_stacks[threads_.current];
        if(frames.size()>=1024 || runtime_.next_activation_cookie==(x86_?UINT32_MAX:UINT64_MAX))return 8;
        const auto cookie=runtime_.next_activation_cookie++;frames.emplace_back(cookie,0);memory.store(GuestAddress{cookie_out},bits,cookie);return 0;
    };
    const auto deactivate=[&](std::uint32_t flags,std::uint64_t cookie)->unsigned{
        if(flags&~1U)return 87;
        if(stack==runtime_.activation_stacks.end())throw CpuFault(0xc0150010U,"Activation context cookie 不屬於目前執行緒");
        auto& frames=stack->second;
        const auto found=std::find_if(frames.begin(),frames.end(),[&](const auto& frame){return frame.first==cookie;});
        if(found==frames.end())throw CpuFault(0xc0150010U,"Activation context cookie 不在堆疊中");
        const bool top=std::next(found)==frames.end();
        if(!flags && !top)throw CpuFault(0xc015000fU,"Activation context 必須按相反次序停用");
        if(flags && top)return 87;
        frames.erase(found,frames.end());return 0;
    };
    // QueryActCtxW 的旗標：4 使用目前內容、8 內容為 HMODULE、16 內容為位址；ntdll 版為 1／2／4。
    const auto query=[&](std::uint32_t flags,std::uint64_t context,std::uint32_t info_class,std::uint64_t buffer,std::uint64_t size,std::uint64_t written)->unsigned{
        const auto mode=flags&0x1c;
        if((flags&~0x8000001cU) || (mode && (mode&(mode-1))) || (mode==4 && context) || (!buffer && size))return 87;
        if(mode==8 || mode==16) {
            const auto* module=module_image(context,mode==16);
            if(!context)return 87;if(!module)return 126;
            if(gui_resource(memory,module->base,2,24,true))return 50;
            context=0; // 未含 isolation-aware RT_MANIFEST #2 的模組沒有 associated context。
        } else if(mode==4)context=active;
        if(context)return 6;
        if(info_class!=1)return 50;
        // ACTIVATION_CONTEXT_BASIC_INFORMATION：{HANDLE hActCtx, DWORD dwFlags}。
        const auto needed=x86_?8U:16U;if(written)memory.store(GuestAddress{written},bits,needed);
        if(!buffer || size<needed)return 122;
        memory.fill(GuestAddress{buffer},0,needed);return 0;
    };
    // 預設內容沒有任何 section；ExtensionGuid 必須為 NULL。
    const auto find_section=[&](std::uint32_t flags,std::uint64_t extension,std::uint64_t key,std::uint64_t data)->unsigned{
        if((flags&~1U) || extension || !key || !data)return 87;
        memory.check(GuestAddress{data},4,Access::Read);return 14007;
    };
    const auto settings=[&](std::uint32_t flags,std::uint64_t context,std::uint64_t name_space,std::uint64_t setting,std::uint64_t written)->unsigned{
        if(flags || context || !setting)return 87;
        if(name_space){const auto text=read_utf16(memory,GuestAddress{name_space});
            if(std::none_of(std::begin(settings_namespaces),std::end(settings_namespaces),[&](auto item){return item==text;}))return 87;}
        if(written)memory.store(GuestAddress{written},bits,0);
        return 14007; // 預設內容沒有 manifest 的 windowsSettings。
    };
    const auto win32=[&](unsigned error,std::uint64_t success=1){if(error){last_error_=error;arg.return_to(state,0);}else arg.return_to(state,success);return std::optional<std::uint32_t>{};};
    const auto status=[&](unsigned error){arg.return_to(state,activation_status(error));return std::optional<std::uint32_t>{};};
    if(native){
        if(symbol=="RtlActivateActivationContext")return status(activate(arg(1),arg(2)));
        if(symbol=="RtlActivateActivationContextEx"){
            // Teb 只接受目前執行緒（其他執行緒的堆疊不能由此改動）。
            if(arg(1) && arg(1)!=(x86_?state.fs_base:state.gs_base))return status(87);
            return status(activate(arg(2),arg(3)));
        }
        if(symbol=="RtlDeactivateActivationContext"){
            if(const auto error=deactivate(dword(0),arg(1)))throw CpuFault(activation_status(error),"RtlDeactivateActivationContext 參數錯誤");
            arg.return_to(state,0);return std::nullopt;
        }
        if(symbol=="RtlAddRefActivationContext" || symbol=="RtlReleaseActivationContext"){arg.return_to(state,0);return std::nullopt;}
        if(symbol=="RtlZombifyActivationContext")return status(arg(0)?6:87);
        if(symbol=="RtlGetActiveActivationContext"){if(!arg(0))return status(87);memory.store(GuestAddress{arg(0)},bits,active);return status(0);}
        if(symbol=="RtlIsActivationContextActive"){
            const bool found=stack!=runtime_.activation_stacks.end() && std::any_of(stack->second.begin(),stack->second.end(),[&](const auto& frame){return frame.second==arg(0);});
            arg.return_to(state,found?1:0);return std::nullopt;
        }
        if(symbol=="RtlQueryInformationActivationContext"){
            const auto flags=dword(0);std::uint32_t mapped=flags&0x80000000U;
            if(flags&~0x80000007U)return status(87);if(flags&1)mapped|=4;if(flags&2)mapped|=8;if(flags&4)mapped|=16;
            return status(query(mapped,arg(1),dword(3),arg(4),arg(5),arg(6)));
        }
        if(symbol=="RtlQueryInformationActiveActivationContext")return status(query(4,0,dword(0),arg(1),arg(2),arg(3)));
        if(symbol=="RtlFindActivationContextSectionString" || symbol=="RtlFindActivationContextSectionGuid")return status(find_section(dword(0),arg(1),arg(3),arg(4)));
        if(symbol=="RtlFreeThreadActivationContextStack"){runtime_.activation_stacks.erase(threads_.current);arg.return_to(state,0);return std::nullopt;}
        if(symbol=="RtlFreeActivationContextStack"){if(arg(0))runtime_.activation_stacks.erase(threads_.current);arg.return_to(state,0);return std::nullopt;}
        if(symbol=="RtlQueryActivationContextApplicationSettings")return status(settings(dword(0),arg(1),arg(2),arg(3),arg(6)));
        return std::nullopt;
    }
    if(symbol=="QueryActCtxW")return win32(query(dword(0),arg(1),dword(3),arg(4),arg(5),arg(6)));
    if(symbol.starts_with("CreateActCtx")) {
        // ACTCTX：x64 {cbSize 0, dwFlags 4, lpSource 8, ..., lpResourceName 32, ..., hModule 48}（56）；x86 {0, 4, 8, ..., 20, ..., 28}（32）。
        const unsigned size=x86_?32U:56U,resource_at=x86_?20U:32U,module_at=x86_?28U:48U;
        const auto fail=[&](unsigned error){last_error_=error;arg.return_to(state,x86_?UINT32_MAX:UINT64_MAX);return std::optional<std::uint32_t>{};};
        if(!arg(0) || memory.load(GuestAddress{arg(0)},32)!=size)return fail(87);
        memory.check(GuestAddress{arg(0)},size,Access::Read);
        const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+4},32));
        const auto source=memory.load(GuestAddress{arg(0)+8},bits),resource=memory.load(GuestAddress{arg(0)+resource_at},bits),module=memory.load(GuestAddress{arg(0)+module_at},bits);
        if((flags&~0xffU) || ((flags&0x80) && !(flags&8)))return fail(87);
        if(flags&0x80) {
            if(!module_image(module))return fail(126);
            if(!gui_resource(memory,module,resource,24,symbol.ends_with('W')))return fail(1814);
            return fail(50); // SxS manifest／assembly redirection 尚未建立，不能回假 context。
        }
        if(!source)return fail(87);
        const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{source})):ansi_to_utf8(read_ansi(memory,source));
        const auto file=filesystem_.open(path);return fail(file.error?file.error:50);
    }
    if(symbol=="ActivateActCtx")return win32(activate(arg(0),arg(1)));
    if(symbol=="DeactivateActCtx")return win32(deactivate(dword(0),arg(1)));
    if(symbol=="GetCurrentActCtx"){if(!arg(0))return win32(87);memory.store(GuestAddress{arg(0)},bits,active);return win32(0);}
    if(symbol=="AddRefActCtx" || symbol=="ReleaseActCtx"){if(arg(0))last_error_=6;arg.return_to(state,0);return std::nullopt;} // NULL 預設內容不需要參考計數。
    if(symbol=="ZombifyActCtx")return win32(arg(0)?6:87);
    if(symbol=="QueryActCtxSettingsW")return win32(settings(dword(0),arg(1),arg(2),arg(3),arg(6)));
    // FindActCtxSectionString(A／W)／FindActCtxSectionGuid(dwFlags, lpExtensionGuid, ulSectionId, 鍵, ReturnedData)。
    return win32(find_section(dword(0),arg(1),arg(3),arg(4)));
}
}
