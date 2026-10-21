#include "x64arm/windows_services.hpp"

namespace x64arm {
namespace { constexpr std::uint32_t bstr_tag=0x52545342U; }
void WindowsServices::check_bstr(const GuestMemory& memory,std::uint64_t pointer) const {
    if(!pointer)return;
    const auto found=pointer>=8?allocations_.find(pointer-8):allocations_.end();
    if(found==allocations_.end() || found->second<10 || memory.load(GuestAddress{pointer-8},32)!=bstr_tag ||
       memory.load(GuestAddress{pointer-4},32)>found->second-10)
        throw HostCallFault("BSTR 不是有效的 SysAllocString 配置");
}
void WindowsServices::free_bstr(GuestState& state,GuestMemory& memory,std::uint64_t pointer){
    check_bstr(memory,pointer);
    if(pointer && !free_heap(memory,pointer-8,state))throw HostCallFault("BSTR 配置已失效");
}
std::uint64_t WindowsServices::allocate_bstr(GuestMemory& memory,std::uint64_t bytes,std::span<const std::uint8_t> contents){
    if(bytes>UINT32_MAX || bytes+10>memory.remaining_capacity())return 0;
    const auto base=allocate_heap(memory,bytes+10);if(!base)return 0;
    memory.store(GuestAddress{base},32,bstr_tag);memory.store(GuestAddress{base+4},32,bytes);
    if(!contents.empty())memory.write(GuestAddress{base+8},contents);
    memory.store(GuestAddress{base+8+bytes},16,0);return base+8;
}
}
