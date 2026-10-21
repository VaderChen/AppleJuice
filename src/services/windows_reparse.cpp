#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
namespace x64arm {
std::span<const WindowsServiceDescriptor> reparse_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{{"kernel32.dll","CreateSymbolicLinkW",3},{"kernel32.dll","CreateSymbolicLinkA",3}};
    return entries;
}
std::optional<std::uint32_t> WindowsServices::invoke_reparse(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許符號連結宿主寫入");
    const WindowsCall args(state,memory,service);std::uint32_t error=0;const bool wide=service.symbol.ends_with('W');
    try{const auto flags=static_cast<std::uint32_t>(args(2));if(flags&~3U || !args(0) || !args(1))error=87;
        else{const auto text=[&](unsigned i){return wide?read_utf16(memory,GuestAddress{args(i)}):utf8_to_utf16(read_ansi(memory,args(i),32767));};
            const auto link=text(0),target=text(1);const auto long_path=[](std::u16string_view value){return value.size()>=260 && !value.starts_with(u"\\\\?\\");};
            if(link.empty() || target.empty())error=2;else if(link.size()>32767 || target.size()>32767 || long_path(link) || long_path(target))error=206;
            // 沿用可寫映射的原生建立授權；完整 Windows token／Developer Mode policy 尚待接入。
            else error=filesystem_.symbolic_link(utf16_to_utf8(link),utf16_to_utf8(target),(flags&1U)!=0);
        }
    }catch(const MemoryFault&){error=998;}catch(const std::invalid_argument&){error=87;}catch(const std::bad_alloc&){error=8;}
    if(error)last_error_=error;args.return_to(state,error?0U:1U);return {};
}
}
