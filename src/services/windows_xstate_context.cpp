// XState 延伸內容家族：kernel32 的 InitializeContext／InitializeContext2、CopyContext、LocateXStateFeature、GetXStateFeaturesMask、
// SetXStateFeaturesMask，以及 ntdll 的 RtlGetExtendedContextLength(2)、RtlInitializeExtendedContext(2)、RtlLocateExtendedFeature(2)、
// RtlLocateLegacyContext、RtlGetExtendedFeaturesMask、RtlSetExtendedFeaturesMask、RtlCopyContext、RtlCopyExtendedContext。
// 緩衝區版面：[CONTEXT（x64 16 位元組對齊）][CONTEXT_EX{All, Legacy, XState}][對齊 64][XSAVE：legacy 512＋header 64＋AVX 256]；
// CONTEXT_EX 的 Offset 皆相對於 CONTEXT_EX 本身。可用特徵與 SharedUserData 的 XSTATE_CONFIGURATION、GetEnabledXStateFeatures 相同。
#include "x64arm/windows_services.hpp"
#include "x64arm/cpu_features.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t chunk_bytes=24,xsave_header=512,xsave_align=64;
struct ContextAbi {
    bool x86;std::uint32_t size,flags_at,arch,xstate_flag,float_flag;
    explicit ContextAbi(bool is_x86):x86(is_x86),size(is_x86?0x2ccU:0x4d0U),flags_at(is_x86?0U:0x30U),arch(is_x86?0x10000U:0x100000U),
        xstate_flag(arch|0x40U),float_flag(arch|8U){}
    std::uint32_t alignment()const{return x86?4U:16U;}
    // 需要的長度：CONTEXT＋CONTEXT_EX，含 XSTATE 時再加 64 位元組對齊與 XSAVE 區，最後保留緩衝區起點的對齊餘量。
    std::uint32_t length(std::uint32_t flags)const{
        std::uint32_t total=size+chunk_bytes;
        if((flags&xstate_flag)==xstate_flag)total=((total+xsave_align-1)&~(xsave_align-1))+virtual_xsave_bytes+xsave_align-1;
        return total+alignment()-1;
    }
};
}
std::span<const WindowsServiceDescriptor> xstate_context_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","InitializeContext",4,0,0,0x2},{"kernel32.dll","InitializeContext2",5,0x10,0,0x2},{"kernel32.dll","CopyContext",3,0,0,0x2},
        {"kernel32.dll","LocateXStateFeature",3,0,0,0x2},{"kernel32.dll","GetXStateFeaturesMask",2},{"kernel32.dll","SetXStateFeaturesMask",2,0x2},
        {"ntdll.dll","RtlGetExtendedContextLength",2,0,0,0x1},{"ntdll.dll","RtlGetExtendedContextLength2",3,0x4,0,0x1},
        {"ntdll.dll","RtlInitializeExtendedContext",3,0,0,0x2},{"ntdll.dll","RtlInitializeExtendedContext2",4,0x8,0,0x2},
        {"ntdll.dll","RtlLocateExtendedFeature",3,0,0,0x2},{"ntdll.dll","RtlLocateExtendedFeature2",4,0,0,0x2},{"ntdll.dll","RtlLocateLegacyContext",2},
        {"ntdll.dll","RtlGetExtendedFeaturesMask",1},{"ntdll.dll","RtlSetExtendedFeaturesMask",2,0x2},
        {"ntdll.dll","RtlCopyContext",3,0,0,0x2},{"ntdll.dll","RtlCopyExtendedContext",3,0,0,0x2}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_xstate_context(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const ContextAbi abi(x86_);
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint64_t value,bool wide=false){arg.return_to(state,value,wide);return std::optional<std::uint32_t>{};};
    const auto win32=[&](unsigned error){if(error)last_error_=error;return finish(error?0:1);};
    constexpr std::uint32_t ok=0,invalid=0xc000000dU,not_supported=0xc00000bbU;
    const auto flags_of=[&](std::uint64_t context){return static_cast<std::uint32_t>(memory.load(GuestAddress{context+abi.flags_at},32));};
    const auto chunk=[&](std::uint64_t ex,unsigned index){return std::pair{static_cast<std::int32_t>(memory.load(GuestAddress{ex+index*8},32)),static_cast<std::uint32_t>(memory.load(GuestAddress{ex+index*8+4},32))};};
    const auto legacy_of=[&](std::uint64_t ex){return ex+static_cast<std::uint64_t>(static_cast<std::int64_t>(chunk(ex,1).first));};
    const auto xsave_of=[&](std::uint64_t ex)->std::uint64_t{const auto [offset,length]=chunk(ex,2);return length?ex+static_cast<std::uint64_t>(static_cast<std::int64_t>(offset)):0;};
    // 在已對齊的 CONTEXT 位址建立 CONTEXT_EX 與 XSAVE header，回傳 CONTEXT_EX 位址。
    const auto initialize=[&](std::uint64_t context,std::uint32_t flags){
        const auto ex=context+abi.size;std::uint64_t xsave=0;std::uint32_t xsave_length=0;
        if((flags&abi.xstate_flag)==abi.xstate_flag){xsave=(ex+chunk_bytes+xsave_align-1)&~std::uint64_t{xsave_align-1};xsave_length=virtual_xsave_bytes;}
        const auto end=xsave?xsave+xsave_length:ex+chunk_bytes;
        memory.check(GuestAddress{context},static_cast<std::size_t>(end-context),Access::Write);
        memory.fill(GuestAddress{context},0,abi.size);memory.store(GuestAddress{context+abi.flags_at},32,flags);
        const auto put=[&](unsigned index,std::int64_t offset,std::uint32_t length){memory.store(GuestAddress{ex+index*8},32,static_cast<std::uint32_t>(offset));memory.store(GuestAddress{ex+index*8+4},32,length);};
        put(0,-static_cast<std::int64_t>(abi.size),static_cast<std::uint32_t>(end-context));put(1,-static_cast<std::int64_t>(abi.size),abi.size);
        put(2,xsave?static_cast<std::int64_t>(xsave-ex):chunk_bytes,xsave_length);
        if(xsave)memory.fill(GuestAddress{xsave},0,xsave_length);
        return ex;
    };
    const auto valid_flags=[&](std::uint32_t flags){return (flags&abi.arch)==abi.arch && !(flags&~(abi.arch|0x7fU|0x80000000U|0x40000000U));};
    // 特徵 2（AVX 高半部）位於 XSAVE 的 virtual_ymm_offset；0／1 屬於 legacy CONTEXT。
    const auto feature=[&](std::uint64_t ex,std::uint32_t id,std::uint32_t& length)->std::uint64_t{
        length=0;const auto xsave=xsave_of(ex);if(!xsave || id!=2 || !((virtual_xstate_mask>>id)&1))return 0;
        length=virtual_ymm_bytes;return xsave+virtual_ymm_offset;
    };
    const auto copy_range=[&](std::uint64_t to,std::uint64_t from,std::uint32_t offset,std::uint32_t size){std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{from+offset},bytes);memory.write(GuestAddress{to+offset},bytes);};
    // 依 CONTEXT_* 旗標複製；回傳 NTSTATUS。
    const auto copy=[&](std::uint64_t to,std::uint32_t flags,std::uint64_t from)->std::uint32_t{
        if(!to || !from || !valid_flags(flags))return invalid;
        const auto source_flags=flags_of(from);const auto wanted=flags&source_flags&0x7fU;
        if(x86_){
            if(wanted&1){copy_range(to,from,0xb4,12);copy_range(to,from,0xc0,12);} // Ebp、Eip、SegCs；EFlags、Esp、SegSs
            if(wanted&2)copy_range(to,from,0x9c,24);
            if(wanted&4)copy_range(to,from,0x8c,16);
            if(wanted&8)copy_range(to,from,0x1c,112);
            if(wanted&0x10)copy_range(to,from,0x04,24);
            if(wanted&0x20)copy_range(to,from,0xcc,512);
        }else {
            if(wanted&1){copy_range(to,from,0x38,2);copy_range(to,from,0x42,6);copy_range(to,from,0x98,8);copy_range(to,from,0xf8,8);} // SegCs、SegSs＋EFlags、Rsp、Rip
            if(wanted&2){copy_range(to,from,0x78,32);copy_range(to,from,0xa0,88);} // Rax～Rbx、Rbp～R15
            if(wanted&4)copy_range(to,from,0x3a,8);
            if(wanted&8){copy_range(to,from,0x34,4);copy_range(to,from,0x100,512);}
            if(wanted&0x10){copy_range(to,from,0x48,32);copy_range(to,from,0x68,16);}
        }
        if(wanted&0x40){
            const auto to_xsave=xsave_of(to+abi.size),from_xsave=xsave_of(from+abi.size);
            if(!to_xsave || !from_xsave)return invalid;
            const auto mask=memory.load(GuestAddress{from_xsave+xsave_header},64)&virtual_xstate_mask;
            memory.store(GuestAddress{to_xsave+xsave_header},64,mask);
            if(mask&4)copy_range(to_xsave,from_xsave,virtual_ymm_offset,virtual_ymm_bytes);
        }
        memory.store(GuestAddress{to+abi.flags_at},32,flags_of(to)|abi.arch|wanted);return ok;
    };
    if(name=="InitializeContext" || name=="InitializeContext2"){
        // (Buffer, ContextFlags, CONTEXT** Context, DWORD* ContextLength[, XStateCompactionMask])：容量不足時回報所需長度。
        const auto flags=static_cast<std::uint32_t>(arg(1));const auto length_at=arg(3);
        if(!length_at || !valid_flags(flags))return win32(87);
        if(name=="InitializeContext2" && (arg(4)&~virtual_xstate_mask))return win32(87);
        const auto needed=abi.length(flags);const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{length_at},32));
        memory.store(GuestAddress{length_at},32,needed);
        if(!arg(0) || capacity<needed)return win32(122);
        if(!arg(2))return win32(87);
        const auto context=(arg(0)+abi.alignment()-1)&~std::uint64_t{abi.alignment()-1};(void)initialize(context,flags);
        memory.store(GuestAddress{arg(2)},bits,context);return win32(0);
    }
    if(name=="RtlGetExtendedContextLength" || name=="RtlGetExtendedContextLength2"){
        const auto flags=static_cast<std::uint32_t>(arg(0));if(!arg(1) || !valid_flags(flags))return finish(invalid);
        if(name.ends_with('2') && (arg(2)&~virtual_xstate_mask))return finish(not_supported);
        memory.store(GuestAddress{arg(1)},32,abi.length(flags));return finish(ok);
    }
    if(name=="RtlInitializeExtendedContext" || name=="RtlInitializeExtendedContext2"){
        // (緩衝區, ContextFlags, CONTEXT_EX** ContextEx[, CompactionMask])：緩衝區須至少 RtlGetExtendedContextLength 回報的長度。
        const auto flags=static_cast<std::uint32_t>(arg(1));if(!arg(0) || !arg(2) || !valid_flags(flags))return finish(invalid);
        if(name.ends_with('2') && (arg(3)&~virtual_xstate_mask))return finish(not_supported);
        const auto context=(arg(0)+abi.alignment()-1)&~std::uint64_t{abi.alignment()-1};
        memory.store(GuestAddress{arg(2)},bits,initialize(context,flags));return finish(ok);
    }
    if(name=="LocateXStateFeature"){
        // (CONTEXT*, FeatureId, DWORD* Length)：0／1 指向 legacy 浮點區（x64 FltSave／Xmm0，x86 ExtendedRegisters）。
        const std::uint64_t context=arg(0);const auto id=static_cast<std::uint32_t>(arg(1));if(!context)return finish(0);
        std::uint64_t at=0;std::uint32_t length=0;
        if(id==0){at=context+(x86_?0xccU:0x100U);length=160;}
        else if(id==1){at=context+(x86_?0xccU+160U:0x1a0U);length=x86_?128U:256U;}
        else if((flags_of(context)&abi.xstate_flag)==abi.xstate_flag)at=feature(context+abi.size,id,length);
        if(arg(2))memory.store(GuestAddress{arg(2)},32,length);return finish(at);
    }
    if(name=="RtlLocateExtendedFeature" || name=="RtlLocateExtendedFeature2"){
        const auto ex=arg(0);if(!ex)return finish(0);std::uint32_t length=0;const auto at=feature(ex,static_cast<std::uint32_t>(arg(1)),length);
        const auto out=name.ends_with('2')?arg(3):arg(2);if(out)memory.store(GuestAddress{out},32,length);return finish(at);
    }
    if(name=="RtlLocateLegacyContext"){const auto ex=arg(0);if(!ex)return finish(0);if(arg(1))memory.store(GuestAddress{arg(1)},32,chunk(ex,1).second);return finish(legacy_of(ex));}
    if(name=="GetXStateFeaturesMask" || name=="RtlGetExtendedFeaturesMask"){
        // kernel32 版：浮點旗標代表特徵 0／1；XSTATE 旗標時加上 XSAVE header 的遮罩。ntdll 版只回 XSAVE 遮罩。
        if(name=="RtlGetExtendedFeaturesMask"){const auto xsave=arg(0)?xsave_of(arg(0)):0;return finish(xsave?memory.load(GuestAddress{xsave+xsave_header},64):0,true);}
        const auto context=arg(0);if(!context || !arg(1))return win32(87);const auto flags=flags_of(context);std::uint64_t mask=0;
        if((flags&abi.float_flag)==abi.float_flag)mask|=3;
        if((flags&abi.xstate_flag)==abi.xstate_flag)if(const auto xsave=xsave_of(context+abi.size))mask|=memory.load(GuestAddress{xsave+xsave_header},64)&~std::uint64_t{3};
        memory.store(GuestAddress{arg(1)},64,mask);return win32(0);
    }
    if(name=="SetXStateFeaturesMask" || name=="RtlSetExtendedFeaturesMask"){
        const bool native=name.starts_with("Rtl");const auto target=arg(0),mask=arg(1);
        if(!target){if(native){arg.return_to(state,0);return std::nullopt;}return win32(87);}
        const auto ex=native?target:target+abi.size;
        if(!native && (flags_of(target)&abi.xstate_flag)!=abi.xstate_flag)return win32(87);
        const auto xsave=xsave_of(ex);if(!xsave || (mask&~virtual_xstate_mask)){if(native){arg.return_to(state,0);return std::nullopt;}return win32(87);}
        memory.store(GuestAddress{xsave+xsave_header},64,mask);
        if(native){arg.return_to(state,0);return std::nullopt;}return win32(0);
    }
    if(name=="CopyContext" || name=="RtlCopyContext"){const auto status=copy(arg(0),static_cast<std::uint32_t>(arg(1)),arg(2));return name=="RtlCopyContext"?finish(status):win32(status?87:0);}
    if(name=="RtlCopyExtendedContext"){
        if(!arg(0) || !arg(2))return finish(invalid);return finish(copy(legacy_of(arg(0)),static_cast<std::uint32_t>(arg(1)),legacy_of(arg(2))));
    }
    return std::nullopt;
}
}
