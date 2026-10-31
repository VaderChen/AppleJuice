#pragma once
#include "x64arm/memory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <ostream>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace x64arm::detail {
// 故障後的有界唯讀觀測；不建立／刷新映射，也不修改原本的拒絕或例外結果。
inline void trace_fault_memory_views(std::ostream& out,const GuestMemory& memory,
                                    GuestAddress address,const char* reason) noexcept {
    if(!std::getenv("X64ARM_TRACE_FAULT_VIEWS"))return;
    static std::atomic<unsigned> samples{};
    const auto sample=samples.fetch_add(1,std::memory_order_relaxed);if(sample>=64)return;
    try{
        struct FormatRestore {std::ostream& stream;std::ios_base::fmtflags flags;
            ~FormatRestore(){stream.flags(flags);}} restore{out,out.flags()};
        const auto mapping=memory.query(address);if(!mapping)return;
        constexpr std::uint64_t extent=16384;
        const auto begin=std::max(address.value&~(extent-1),mapping->base.value);
        const auto count=static_cast<std::size_t>(std::min(extent-(begin&(extent-1)),
            mapping->base.value+mapping->size-begin));
        std::array<std::uint8_t,extent> backing{};
        memory.read(GuestAddress{begin},std::span(backing).first(count));
        out<<"[fault_view] reason="<<reason<<" sample="<<std::dec<<sample
            <<" address=0x"<<std::hex<<address.value<<" begin=0x"<<begin<<" size=0x"<<count
            <<" native_mmu_pending="<<memory.native_mmu_update_pending()
            <<" borrow_generation=0x"<<memory.native_borrow_generation()<<'\n';
        const auto describe=[&](const char* source,const auto& bytes){
            out<<"[fault_view_data] source="<<source<<" begin=0x"<<begin
                <<" zero_bytes=0x"<<std::count(bytes.begin(),bytes.begin()+count,0)<<" zero_runs=";
            unsigned runs=0;
            for(std::size_t i=0;i<count;){
                if(bytes[i]){++i;continue;}
                const auto first=i;while(i<count && !bytes[i])++i;
                if(i-first>=16){if(runs++==32){out<<"truncated";break;}out<<"[0x"<<begin+first<<",0x"<<begin+i<<")";}
            }
            const auto focus=static_cast<std::size_t>(address.value-begin)&~std::size_t{7};
            const auto first=focus>=16?focus-16:0;
            out<<" words_begin=0x"<<begin+first;
            for(auto i=first;i+8<=count && i<first+80;i+=8){
                std::uint64_t word=0;for(unsigned n=0;n<8;++n)word|=std::uint64_t{bytes[i+n]}<<(n*8);
                out<<" 0x"<<word;
            }
            out<<'\n';
        };
        describe("backing",backing);
#if defined(__APPLE__)
        std::array<std::uint8_t,extent> alias{};
        const auto base=memory.native_mmu_base_address()[0];
        if(!base || begin>=GuestMemory::native_mmu_window || count>GuestMemory::native_mmu_window-begin ||
           begin>UINTPTR_MAX-base || count>UINTPTR_MAX-base-begin){out<<"[fault_view_alias] unavailable\n";return;}
        // 透過核心安全讀取目前別名；未映射或無讀權限時只記錄錯誤，不觸發宿主 SIGSEGV。
        mach_vm_size_t copied=0;
        const auto result=mach_vm_read_overwrite(mach_task_self(),base+begin,count,
            reinterpret_cast<mach_vm_address_t>(alias.data()),&copied);
        out<<"[fault_view_alias] result="<<std::dec<<result<<" copied="<<copied;
        if(result!=KERN_SUCCESS || copied!=count){out<<'\n';return;}
        std::size_t mismatches=0,first=count,last=0;
        for(std::size_t i=0;i<count;++i)if(backing[i]!=alias[i]){++mismatches;first=std::min(first,i);last=i;}
        out<<" mismatches="<<mismatches;
        if(mismatches)out<<" first=0x"<<std::hex<<begin+first<<" last=0x"<<begin+last;
        out<<'\n'<<std::hex;describe("native",alias);
#endif
    }catch(...){/* 診斷失敗不得取代原本的故障。 */}
}
}
