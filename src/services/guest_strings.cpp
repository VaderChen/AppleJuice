#include "x64arm/windows_abi.hpp"
#include <algorithm>

namespace x64arm {
std::string read_ansi(const GuestMemory& memory, std::uint64_t pointer, std::size_t limit) {
    std::string value;
    for (std::size_t i = 0; i <= limit;) {
        if (i > UINT64_MAX - pointer) throw MemoryFault(GuestAddress{pointer}, Access::Read, i, "字串地址溢位");
        const auto bytes=memory.readable_prefix(GuestAddress{pointer+i},std::min<std::size_t>(limit-i,255)+1);
        const auto end=std::find(bytes.begin(),bytes.end(),0);const auto count=static_cast<std::size_t>(end-bytes.begin());
        if(count>limit-i)throw HostCallFault("字串超過服務上限");
        value.append(reinterpret_cast<const char*>(bytes.data()),count);
        if(end!=bytes.end())return value;
        i+=count;
    }
    throw HostCallFault("無效字串");
}
}
