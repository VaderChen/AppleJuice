#include "x64arm/windows_services.hpp"
#include <algorithm>
namespace x64arm {
WinrtString WindowsServices::winrt_string(const GuestMemory& memory,std::uint64_t handle) const {
    if(!handle)return {};
    if(const auto found=runtime_.winrt_strings.find(handle);found!=runtime_.winrt_strings.end())return found->second;
    memory.check(GuestAddress{handle},24,Access::Read);
    if(memory.load(GuestAddress{handle},32)!=0x52534858U)throw HostCallFault("HSTRING handle 不合法");
    return {memory.load(GuestAddress{handle+8},x86_?32:64),static_cast<std::uint32_t>(memory.load(GuestAddress{handle+4},32)),0};
}
std::u16string WindowsServices::winrt_content(const GuestMemory& memory,const WinrtString& value,std::uint32_t maximum) const {
    const auto length=std::min(value.length,maximum);const auto size=std::uint64_t{length}*2;
    if(size>memory.limit())throw HostCallFault("HSTRING 長度超出記憶體容量");
    std::u16string result(length,u'\0');memory.check(GuestAddress{value.data},static_cast<std::size_t>(size),Access::Read);
    for(std::uint64_t i=0;i<length;++i)result[i]=static_cast<char16_t>(memory.load(GuestAddress{value.data+i*2},16));
    return result;
}
}
