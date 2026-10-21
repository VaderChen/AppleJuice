#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> activation_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","QueryActCtxW",7},{"kernel32.dll","CreateActCtxA",1},{"kernel32.dll","CreateActCtxW",1},
        {"kernel32.dll","ActivateActCtx",2},{"kernel32.dll","DeactivateActCtx",2},
        {"kernel32.dll","GetCurrentActCtx",1},{"kernel32.dll","AddRefActCtx",1},{"kernel32.dll","ReleaseActCtx",1},
        {"kernel32.dll","FindActCtxSectionStringA",5},{"kernel32.dll","FindActCtxSectionStringW",5}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_activation(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto dword=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto stack=runtime_.activation_stacks.find(threads_.current);
    const auto active=stack==runtime_.activation_stacks.end() || stack->second.empty()?0:stack->second.back().second;
    std::uint64_t result=0;const auto fail=[&](unsigned error){last_error_=error;};
    if(symbol=="QueryActCtxW") {
        const auto flags=dword(0),mode=flags&0x1c;auto context=arg(1);unsigned error=0;
        if((flags&~0x8000001cU) || (mode && (mode&(mode-1))) || (mode==4 && context) || (!arg(4) && arg(5)))error=87;
        else if(mode==8 || mode==16) {
            const auto* module=module_image(context,mode==16);
            if(!context)error=87;
            else if(!module)error=126;
            else if(gui_resource(memory,module->base,2,24,true))error=50;
            else context=0; // 未含 isolation-aware RT_MANIFEST #2 的模組沒有 associated context。
        } else if(mode==4)context=active;
        if(!error && context)error=6;
        if(!error && dword(3)!=1)error=50;
        if(error)fail(error);
        else {
            if(arg(6))memory.store(GuestAddress{arg(6)},64,16);
            if(!arg(4) || arg(5)<16)fail(122);
            else {memory.write(GuestAddress{arg(4)},std::array<std::uint8_t,16>{});result=1;}
        }
    } else if(symbol.starts_with("CreateActCtx")) {
        result=UINT64_MAX;
        if(!arg(0) || memory.load(GuestAddress{arg(0)},32)!=56)fail(87);
        else {
            memory.check(GuestAddress{arg(0)},56,Access::Read);
            const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+4},32));
            const auto source=memory.load(GuestAddress{arg(0)+8},64),resource=memory.load(GuestAddress{arg(0)+32},64),module=memory.load(GuestAddress{arg(0)+48},64);
            if((flags&~0xffU) || ((flags&0x80) && !(flags&8)))fail(87);
            else if(flags&0x80) {
                if(!module_image(module))fail(126);
                else if(!gui_resource(memory,module,resource,24,symbol.ends_with('W')))fail(1814);
                else fail(50); // SxS manifest／assembly redirection 尚未建立，不能回假 context。
            } else if(!source)fail(87);
            else {
                const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{source})):read_ansi(memory,source);
                const auto file=filesystem_.open(path);fail(file.error?file.error:50);
            }
        }
    } else if(symbol=="ActivateActCtx") {
        if(arg(0))fail(6);
        else if(!arg(1))fail(87);
        else {
            memory.check(GuestAddress{arg(1)},8,Access::Write);auto& frames=runtime_.activation_stacks[threads_.current];
            if(frames.size()>=1024 || runtime_.next_activation_cookie==UINT64_MAX)fail(8);
            else {const auto cookie=runtime_.next_activation_cookie++;frames.emplace_back(cookie,0);memory.store(GuestAddress{arg(1)},64,cookie);result=1;}
        }
    } else if(symbol=="DeactivateActCtx") {
        if(dword(0)&~1U)fail(87);
        else {
            if(stack==runtime_.activation_stacks.end())throw CpuFault(0xc0150010U,"Activation context cookie 不屬於目前執行緒");
            auto& frames=stack->second;
            const auto found=std::find_if(frames.begin(),frames.end(),[&](const auto& frame){return frame.first==arg(1);});
            if(found==frames.end())throw CpuFault(0xc0150010U,"Activation context cookie 不在堆疊中");
            const bool top=std::next(found)==frames.end();
            if(!dword(0) && !top)throw CpuFault(0xc015000fU,"Activation context 必須按相反次序停用");
            if(dword(0) && top)fail(87);
            else {frames.erase(found,frames.end());result=1;}
        }
    } else if(symbol=="GetCurrentActCtx") {
        if(!arg(0))fail(87);else {memory.store(GuestAddress{arg(0)},64,active);result=1;}
    } else if(symbol=="AddRefActCtx" || symbol=="ReleaseActCtx") {
        if(arg(0))fail(6); // NULL default context 不需要 reference count。
    } else {
        if((dword(0)&~1U) || arg(1) || !arg(3) || !arg(4))fail(87);
        else {memory.check(GuestAddress{arg(4)},4,Access::Read);fail(14007);} // Empty/default context 沒有可重導向的 section key。
    }
    arg.return_to(state,result);return std::nullopt;
}
}
