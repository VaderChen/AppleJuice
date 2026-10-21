#include "x64arm/thread_context.hpp"
#include "x64arm/windows_unwind.hpp"
#include <algorithm>
#include <stdexcept>

namespace x64arm::thread_context {
std::uint32_t validate_flags(ExecutionMode mode,std::uint32_t flags) {
    const auto architecture=mode==ExecutionMode::X86?0x10000U:0x100000U;
    if((flags&0x00ff0000U)!=architecture || flags&~(architecture|0x3fU))return 87;
    // AMD64 的 0x20 沒有 i386 ExtendedRegisters 意義；XSTATE 另外需要可變長緩衝區。
    if(mode==ExecutionMode::X64 && (flags&0x20U))return 50;
    return 0;
}
std::vector<std::uint8_t> capture(const GuestState& state,ExecutionMode mode,std::uint32_t flags) {
    if(validate_flags(mode,flags))throw std::invalid_argument("執行緒 CONTEXT flags 無效");
    GuestMemory scratch;scratch.map(GuestAddress{0x10000},4096,read_write);
    write_context(scratch,0x10000,state,mode);
    scratch.store(GuestAddress{0x10000+flags_offset(mode)},32,flags);
    std::vector<std::uint8_t> bytes(size(mode));scratch.read(GuestAddress{0x10000},bytes);return bytes;
}
void write_selected(GuestMemory& memory,std::uint64_t address,ExecutionMode mode,std::span<const std::uint8_t> context) {
    if(context.size()!=size(mode))throw std::invalid_argument("執行緒 CONTEXT 大小無效");
    const auto at=flags_offset(mode);std::uint32_t flags=0;
    for(unsigned i=0;i<4;++i)flags|=std::uint32_t{context[at+i]}<<(8*i);
    if(validate_flags(mode,flags))throw std::invalid_argument("執行緒 CONTEXT flags 無效");
    // 先驗證整個 in/out 結構，再提交所選欄位，跨頁失敗不留下部分輸出。
    std::vector<std::uint8_t> output(context.size());memory.read(GuestAddress{address},output);
    memory.check(GuestAddress{address},output.size(),Access::Write);
    const auto copy=[&](std::size_t begin,std::size_t count){std::copy_n(context.data()+begin,count,output.data()+begin);};
    if(mode==ExecutionMode::X86) {
        if(flags&0x10U)copy(4,24);
        if(flags&8U)copy(28,112);
        if(flags&4U)copy(140,16);
        if(flags&2U)copy(156,24);
        if(flags&1U)copy(180,24);
        if(flags&0x20U)copy(204,512);
    } else {
        if(flags&0x10U)copy(72,48);
        if(flags&8U){copy(52,4);copy(256,512);}
        if(flags&4U)copy(58,8);
        if(flags&2U){copy(120,32);copy(160,88);}
        if(flags&1U){copy(56,2);copy(66,6);copy(152,8);copy(248,8);}
    }
    copy(at,4);memory.write(GuestAddress{address},output);
}
GuestState restore(const GuestState& previous,ExecutionMode mode,std::span<const std::uint8_t> context) {
    if(context.size()!=size(mode))throw std::invalid_argument("執行緒 CONTEXT 大小無效");
    std::uint32_t flags=0;const auto at=flags_offset(mode);
    for(unsigned i=0;i<4;++i)flags|=std::uint32_t{context[at+i]}<<(8*i);
    if(validate_flags(mode,flags))throw std::invalid_argument("執行緒 CONTEXT flags 無效");
    // 按目標架構解碼，共用既有 CPU 欄位選取及硬體能力檢查。
    GuestMemory scratch;scratch.set_execution_mode(mode);scratch.map(GuestAddress{0x10000},4096,read_write);scratch.write(GuestAddress{0x10000},context);
    return read_context(scratch,0x10000,previous);
}
}
