#pragma once
#include "windows_process_parameter_layout.hpp"
#include "x64arm/process_startup_data.hpp"
#include "x64arm/memory.hpp"
#include <algorithm>
#include <stdexcept>

namespace x64arm::detail {
struct NativeProcessError {std::uint32_t status;};
inline NativeProcessParameters capture_native_process_parameters(const GuestMemory& memory,std::uint64_t address,bool x86){
    NativeProcessParameters result;
    if(!address)return result;
    const auto layout=ProcessParameterLayout::for_x86(x86);const auto bits=static_cast<std::uint8_t>(layout.unit*8);
    const auto length=memory.load(GuestAddress{address+4},32),maximum=memory.load(GuestAddress{address},32);
    if(length<layout.strings[7]+2*layout.unit || length>1024*1024 || maximum<length)throw NativeProcessError{0xc000000dU};
    const auto header=std::min<std::uint64_t>(length,layout.header);
    memory.check(GuestAddress{address},static_cast<std::size_t>(header),Access::Read);
    result.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{address+8},32));
    if(result.flags&~1U)throw NativeProcessError{0xc00000bbU};
    if(memory.load(GuestAddress{address+12},32) || memory.load(GuestAddress{address+16},bits) ||
       memory.load(GuestAddress{address+layout.strings[0]+2*layout.unit},bits))throw NativeProcessError{0xc00000bbU};
    result.console_flags=static_cast<std::uint32_t>(memory.load(GuestAddress{address+layout.console_flags},32));
    for(unsigned i=0;i<3;++i)result.standard_handles[i]=memory.load(GuestAddress{address+(x86?0x18U:0x20U)+i*layout.unit},bits);
    for(unsigned i=0;i<result.strings.size();++i){
        const auto offset=layout.strings[i];if(offset+2*layout.unit>length)continue;
        const auto size=memory.load(GuestAddress{address+offset},16),capacity=memory.load(GuestAddress{address+offset+2},16);
        auto pointer=memory.load(GuestAddress{address+offset+layout.unit},bits);
        if(size>capacity || size>65533 || (i!=7 && size%2) || (size && !pointer))throw NativeProcessError{0xc000000dU};
        if(pointer && !(result.flags&1U)){if(pointer>UINT64_MAX-address || (x86 && pointer+address>UINT32_MAX))throw NativeProcessError{0xc000000dU};pointer+=address;}
        auto& bytes=result.strings[i];bytes.resize(static_cast<std::size_t>(size));if(size)memory.read(GuestAddress{pointer},bytes);
        if(i!=7)for(std::size_t at=0;at<bytes.size();at+=2)if(!bytes[at] && !bytes[at+1])throw NativeProcessError{0xc000000dU};
    }
    for(unsigned i=0;i<result.window.size();++i)result.window[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{address+(x86?0x4cU:0x88U)+4*i},32));
    // 擴充啟動政策尚無對應契約；不能把非零政策當作一般字串資料忽略。
    if(!result.strings[8].empty())throw NativeProcessError{0xc00000bbU};
    const auto tail=x86?0x2acU:0x420U;
    for(std::uint64_t at=tail;at<header;++at)if(memory.load(GuestAddress{address+at},8))throw NativeProcessError{0xc00000bbU};
    return result;
}
inline std::uint32_t process_native_status(std::uint32_t error){
    switch(error){case 0:return 0;case 2:return 0xc0000034U;case 3:return 0xc000003aU;case 5:case 367:return 0xc0000022U;
    case 6:return 0xc0000008U;case 8:case 14:return 0xc0000017U;case 32:return 0xc0000043U;case 50:return 0xc00000bbU;
    case 87:return 0xc000000dU;case 123:return 0xc0000033U;case 193:return 0xc000007bU;case 267:return 0xc0000103U;
    case 740:return 0xc000042cU;case 998:return 0xc0000005U;case 1314:return 0xc0000061U;case 1336:return 0xc0000077U;
    case 1337:return 0xc0000078U;case 1338:return 0xc0000079U;case 14001:return 0xc0150002U;default:return 0xc0000001U;}
}
}
