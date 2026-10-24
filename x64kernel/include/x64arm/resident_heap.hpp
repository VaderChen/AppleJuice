#pragma once
#include <cstddef>
#include <cstdint>

namespace x64arm {
// 有界級距 heap 的 resident 契約；服務決定入口及 magic，不依賴 Windows 符號。
// 首版為 32-bit 位址，256 KiB chunk、64-byte 標頭及 64-bit 空閒串列頭。
// 級距 1..256 每級 16 bytes；其後每級 256 bytes。標頭依序為 magic/class/size/owner。
enum class ResidentHeapOperation : std::uint32_t {None=0,Allocate=1,Free=2,Size=3};
inline constexpr std::uint32_t resident_heap_chunk_bits=18, resident_heap_chunk_count=1U<<(32-resident_heap_chunk_bits);
inline constexpr std::uint32_t resident_heap_chunk_size=1U<<resident_heap_chunk_bits, resident_heap_header_size=64;
inline constexpr std::uint32_t resident_heap_small_limit=4096, resident_heap_small_class_count=256;
inline constexpr std::uint32_t resident_heap_small_unit=16, resident_heap_large_unit=256;
inline constexpr std::uint32_t resident_heap_entry_ready=1;
struct ResidentHeapEntry {
    std::uint64_t heap{};
    std::uint64_t* heads{};
    std::uint32_t head_count{},flags{};
};
// entries[0] 為 process heap，其餘依 heap 遞增。chunks[guest>>18] 是整段已驗證的私人 RW backing。
// 空項目／未 ready／不支援的旗標或大小均在提交前回退原 provider；不得補充 chunk 或提前 promotion。
// 描述元與表只於全體 worker 停止時更新；heads、mutations 及所有 chunk 資料由 guard 保護。
// native 成功只改 EAX/RSP/RIP，保留 LastError、FS、其餘 GPR、XMM 與 lazy flags。
struct ResidentHeap {
    std::uint32_t* guard{};
    std::uint64_t* mutations{};
    std::uint8_t* const* chunks{};
    const ResidentHeapEntry* entries{};
    std::uint64_t process_heap{},magic{};
    std::uint32_t chunk_count{},entry_count{},max_request{},max_class{};
    std::uint64_t generation{};
    // 客體返回槽與狀態頁重疊時必須在 CAS 前回退；宿主 backing 指標不可拿來比較 MMU alias。
    std::uint64_t guest_state_begin{},guest_state_end{};
};
// 與 ResidentRendezvous 的 C++ Retry 等待共用同一組 atomic_ref 格位。
// 解鎖後先無條件以 seq_cst 遞增 sequence，再 seq_cst 讀 waiters；非零才呼叫 notify。
// notify 持相同信箱 mutex 通知，不能再遞增序號；不物化 GuestState、不更改客體狀態。
struct alignas(8) ResidentServiceProgressState {
    std::uint64_t sequence{};
    std::uint32_t waiters{},reserved{};
};
struct ResidentServiceProgressView {
    ResidentServiceProgressState* state{};
    void* context{};
    void (*notify)(void*) noexcept{};
};
static_assert(sizeof(void*)==8);
static_assert(sizeof(ResidentHeapEntry)==24 && offsetof(ResidentHeapEntry,heap)==0 &&
              offsetof(ResidentHeapEntry,heads)==8 && offsetof(ResidentHeapEntry,head_count)==16 &&
              offsetof(ResidentHeapEntry,flags)==20);
static_assert(sizeof(ResidentHeap)==88 && offsetof(ResidentHeap,guard)==0 &&
              offsetof(ResidentHeap,mutations)==8 && offsetof(ResidentHeap,chunks)==16 &&
              offsetof(ResidentHeap,entries)==24 && offsetof(ResidentHeap,process_heap)==32 &&
              offsetof(ResidentHeap,magic)==40 && offsetof(ResidentHeap,chunk_count)==48 &&
              offsetof(ResidentHeap,entry_count)==52 && offsetof(ResidentHeap,max_request)==56 &&
              offsetof(ResidentHeap,max_class)==60 &&
              offsetof(ResidentHeap,generation)==64 && offsetof(ResidentHeap,guest_state_begin)==72 &&
              offsetof(ResidentHeap,guest_state_end)==80);
static_assert(sizeof(ResidentServiceProgressState)==16 && offsetof(ResidentServiceProgressState,sequence)==0 &&
              offsetof(ResidentServiceProgressState,waiters)==8);
static_assert(sizeof(ResidentServiceProgressView)==24 && offsetof(ResidentServiceProgressView,state)==0 &&
              offsetof(ResidentServiceProgressView,context)==8 && offsetof(ResidentServiceProgressView,notify)==16);
}
