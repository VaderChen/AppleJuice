#pragma once
#include "x64arm/decoder.hpp"
namespace x64arm {
// 固定 16-bit 描述供原生 helper／AOT 重定位及發布驗證共同使用。
// bit 15 標記；0–1 元素、2–3 地址、4 SCAS、5 REP、6 REPE、7 RIP64、
// 8–9 segment、10–13 長度、14 TSO。沒有宿主指標或可變呼叫目標。
struct StringCompareForm {
    std::uint32_t control{};
    explicit operator bool()const noexcept{return valid(control);}
    unsigned width()const noexcept{return 8U<<(control&3U);}
    unsigned address_width()const noexcept{return 16U<<((control>>2)&3U);}
    unsigned stack_width()const noexcept{return control&128U?64U:32U;}
    unsigned length()const noexcept{return (control>>10)&15U;}
    unsigned segment()const noexcept{return (control>>8)&3U;}
    bool scan()const noexcept{return (control&16U)!=0;}
    bool repeat()const noexcept{return (control&32U)!=0;}
    bool equal()const noexcept{return (control&64U)!=0;}
    static constexpr bool valid(std::uint32_t value)noexcept{
        const auto addr=(value>>2)&3U;const bool wide=(value&128U)!=0;
        return (value&0xffff8000U)==0x8000U && ((value>>10)&15U) && addr<3 && ((value>>8)&3U)<3 &&
            (wide?addr!=0:addr!=2) && (wide || (value&3U)!=3U) && (!(value&16U) || !(value&0x300U));
    }
    static StringCompareForm of(const Instruction&);
};
// 單元素 CPU 契約；失敗時恢復 REP 開始前旗標後重新拋出精確 MemoryFault。
void execute_string_compare(StringCompareForm,GuestState&,GuestMemory&,std::uint64_t rip);
// 私有穩定頁的有界批次；呼叫者保證 Relaxed 及允許延後排程回呼。
std::uint64_t execute_rep_compare(StringCompareForm,GuestState&,GuestMemory&,std::uint64_t budget);
// 常駐 REP 家族共用的穩定頁 lowering。多客體使用逐元素 acquire 讀取，不跨越第一個停止元素；
// 呼叫者持有映射讀鎖。正常路徑只受 CX、頁界線與位址回繞限制，metered 僅供獨立觀察模式。
// 亦接受不帶 REP 的單元素形式；零計數完成一次空指令。無可用視窗時不提交，回傳零交回精確慢路徑。
std::uint64_t execute_resident_compare(StringCompareForm,GuestState&,GuestMemory&,std::uint64_t budget,bool metered,bool shared);
// 原生單元素路徑：記憶體檢查失敗時完全不提交，回到 CPU 故障派送。
extern "C" std::uint64_t jit_string_compare(GuestState*,GuestMemory*,unsigned control,std::uint64_t rip) noexcept;
void restore_rep_compare_fault(GuestState&) noexcept;
inline void discard_abandoned_rep_compare(GuestState& state) noexcept {
    if(state.rep_compare.control && state.rep_compare.rip!=state.rip.value)state.rep_compare={};
}
}
