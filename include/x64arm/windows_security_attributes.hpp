#pragma once
#include "x64arm/security_descriptor.hpp"

namespace x64arm {
struct SynchronizationDescriptor {bool inherit{};std::vector<std::uint8_t> bytes;};
struct CreationSecurity {bool inherit{};security::SecurityDescriptor descriptor;};
// check_length=false：BaseFormatObjectAttributes 路徑（事件、號誌、Job 等核心物件）不讀取 nLength。
inline CreationSecurity read_security_attributes(const GuestMemory& memory,std::uint64_t address,bool x86,bool read_sd=true,bool check_length=true) {
    CreationSecurity result;if(!address)return result;
    const unsigned size=x86?12:24;memory.check(GuestAddress{address},size,Access::Read);
    if(check_length && memory.load(GuestAddress{address},32)!=size)throw SecurityDescriptorError{87};
    result.inherit=memory.load(GuestAddress{address+(x86?8:16)},32)!=0;
    const auto pointer=memory.load(GuestAddress{address+(x86?4:8)},x86?32:64);
    if(pointer && read_sd)result.descriptor=security::read_descriptor(memory,pointer,x86,true);return result;
}
inline constexpr std::uint32_t virtual_primary_integrity=0x2000;
struct SynchronizationSecurity {
    bool inherit{};
    std::uint32_t error{};
    std::uint32_t integrity{virtual_primary_integrity}, policy{1}, access_mask{UINT32_MAX};
};
// 同步物件共用的 SECURITY_ATTRIBUTES 解碼；NULL DACL 不等於空 ACL。
// 目前的 session 隔離物件支援預設描述元／不限制存取的 DACL。
// 管道可套用基本 DACL 與 integrity label；其餘物件尚未支援的 ACL 明確拒絕。
inline SynchronizationSecurity synchronization_security(const GuestMemory& memory,
                                                        std::uint64_t address, bool x86, bool allow_integrity = false) {
    if (!address) return {};
    const std::uint8_t width = x86 ? 32 : 64;
    const unsigned size = x86 ? 12 : 24;
    memory.check(GuestAddress{address}, size, Access::Read);
    if (memory.load(GuestAddress{address}, 32) != size) return {false, 87};
    const bool inherit = memory.load(GuestAddress{address + (x86 ? 8 : 16)}, 32) != 0;
    const auto descriptor = memory.load(GuestAddress{address + (x86 ? 4 : 8)}, width);
    if (!descriptor) return {inherit, 0};
    memory.check(GuestAddress{descriptor}, 4, Access::Read);
    if (memory.load(GuestAddress{descriptor}, 8) != 1) return {false, 1305};
    const auto control = memory.load(GuestAddress{descriptor + 2}, 16);
    const bool relative = (control & 0x8000) != 0;
    const std::uint8_t pointer_width = relative ? 32 : width;
    const unsigned first = relative || x86 ? 4 : 8, step = pointer_width / 8;
    memory.check(GuestAddress{descriptor}, relative || x86 ? 20 : 40, Access::Read);
    const auto owner = memory.load(GuestAddress{descriptor + first}, pointer_width);
    const auto group = memory.load(GuestAddress{descriptor + first + step}, pointer_width);
    const auto sacl = memory.load(GuestAddress{descriptor + first + 2 * step}, pointer_width);
    if (owner || group) return {false, 50};
    SynchronizationSecurity result{inherit,0};
    if((control&4) && memory.load(GuestAddress{descriptor+first+3*step},pointer_width)) {
        if(!allow_integrity)return {false,50}; // 目前只有管道後端保留並執行此權限集合。
        const auto pointer=memory.load(GuestAddress{descriptor+first+3*step},pointer_width),acl=relative?descriptor+pointer:pointer;
        memory.check(GuestAddress{acl},8,Access::Read);const auto dacl_size=memory.load(GuestAddress{acl+2},16),count=memory.load(GuestAddress{acl+4},16),revision=memory.load(GuestAddress{acl},8);
        if(dacl_size<8 || (revision!=2 && revision!=4))return {false,1336};memory.check(GuestAddress{acl},dacl_size,Access::Read);
        std::uint64_t cursor=8;std::uint32_t allowed=0,denied=0;
        for(std::uint64_t i=0;i<count;++i){
            if(cursor+8>dacl_size)return {false,1336};const auto ace=acl+cursor,bytes=memory.load(GuestAddress{ace+2},16),type=memory.load(GuestAddress{ace},8),flags=memory.load(GuestAddress{ace+1},8);
            if(bytes<16 || bytes>dacl_size-cursor)return {false,1336};if(type>1)return {false,50};
            const auto sid=ace+8,subcount=memory.load(GuestAddress{sid+1},8);if(memory.load(GuestAddress{sid},8)!=1 || subcount>15 || 16+subcount*4>bytes)return {false,1337};
            std::uint64_t authority=0;for(unsigned j=0;j<6;++j)authority=(authority<<8)|memory.load(GuestAddress{sid+2+j},8);
            const auto sub0=subcount?memory.load(GuestAddress{sid+8},32):UINT32_MAX,sub1=subcount>1?memory.load(GuestAddress{sid+12},32):UINT32_MAX;
            // 同一沙箱的未提升本機使用者：Everyone、Authenticated Users、Interactive、Users，以及預設物件 owner。
            const bool match=(subcount==1 && ((authority==1 && sub0==0) || (authority==3 && sub0==4) || (authority==5 && (sub0==11 || sub0==4)))) || (authority==5 && subcount==2 && sub0==32 && sub1==545);
            if(match && !(flags&8)) {
                auto mask=static_cast<std::uint32_t>(memory.load(GuestAddress{ace+4},32));
                if(mask&0x10000000U)mask|=0x1f01ff;if(mask&0x80000000U)mask|=0x120089;if(mask&0x40000000U)mask|=0x120116;if(mask&0x20000000U)mask|=0x1200a0;mask&=0x0fffffffU;
                if(type==1)denied|=mask&~allowed;else allowed|=mask&~denied;
            }
            cursor+=bytes;
        }
        result.access_mask=allowed;
    }
    if((control&0x10) || sacl) {
        if(!allow_integrity)return {false,50};
        if(!(control&0x10) || !sacl)return {false,1338};
        const auto acl=relative?descriptor+sacl:sacl;
        memory.check(GuestAddress{acl},8,Access::Read);
        const auto revision=memory.load(GuestAddress{acl},8),acl_size=memory.load(GuestAddress{acl+2},16),count=memory.load(GuestAddress{acl+4},16);
        if((revision!=2 && revision!=4) || acl_size<8)return {false,1336};
        memory.check(GuestAddress{acl},acl_size,Access::Read);
        if(count>1)return {false,50};
        if(count) {
            if(acl_size<28 || memory.load(GuestAddress{acl+10},16)!=20)return {false,1336};
            if(memory.load(GuestAddress{acl+8},8)!=0x11 || memory.load(GuestAddress{acl+9},8))return {false,50};
            const auto policy=memory.load(GuestAddress{acl+12},32),sid=acl+16;
            if((policy&~7U) || memory.load(GuestAddress{sid},16)!=0x101 || memory.load(GuestAddress{sid+2},32) || memory.load(GuestAddress{sid+6},16)!=0x1000)return {false,1336};
            const auto level=memory.load(GuestAddress{sid+8},32);if(level>0x5000 || (level&0xfff))return {false,1337};
            result.integrity=static_cast<std::uint32_t>(level);result.policy=static_cast<std::uint32_t>(policy);
        }
    }
    return result;
}
}
