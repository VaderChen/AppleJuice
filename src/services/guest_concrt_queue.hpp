#pragma once
#include "concrt_queue_layout.h"
#include "x64arm/memory.hpp"
#include "x64arm/windows_runtime.hpp"
#include <array>
#include <string_view>
// Concurrency::details::_Concurrent_queue_base_v4 的客體常式映像（guest_concrt_queue.c）。
// 佇列演算法與衍生類別的虛擬函式都在客體端執行，多條客體執行緒以原子指令協調，不經宿主協調者；
// 因此不受追蹤服務或啟動剖析設定影響，一律映射。
namespace x64arm::concrt_queue {
inline constexpr std::uint64_t image_offset=0x20000,image_capacity=0x10000;
struct Binding {std::string_view x64,x86,native;};
// MSVC 修飾名（x64／x86 thiscall）對應映像內的 C 入口；_Internal_throw_exception 由宿主服務拋出 std::bad_alloc。
inline constexpr std::array<Binding,19> bindings{{
    {"??0_Concurrent_queue_base_v4@details@Concurrency@@IEAA@_K@Z","??0_Concurrent_queue_base_v4@details@Concurrency@@IAE@I@Z","guest_concrt_queue_construct"},
    {"??1_Concurrent_queue_base_v4@details@Concurrency@@MEAA@XZ","??1_Concurrent_queue_base_v4@details@Concurrency@@MAE@XZ","guest_concrt_queue_destroy"},
    {"?_Internal_push@_Concurrent_queue_base_v4@details@Concurrency@@IEAAXPEBX@Z","?_Internal_push@_Concurrent_queue_base_v4@details@Concurrency@@IAEXPBX@Z","guest_concrt_queue_push"},
    {"?_Internal_move_push@_Concurrent_queue_base_v4@details@Concurrency@@IEAAXPEAX@Z","?_Internal_move_push@_Concurrent_queue_base_v4@details@Concurrency@@IAEXPAX@Z","guest_concrt_queue_move_push"},
    {"?_Internal_pop_if_present@_Concurrent_queue_base_v4@details@Concurrency@@IEAA_NPEAX@Z","?_Internal_pop_if_present@_Concurrent_queue_base_v4@details@Concurrency@@IAE_NPAX@Z","guest_concrt_queue_pop_if_present"},
    {"?_Internal_size@_Concurrent_queue_base_v4@details@Concurrency@@IEBA_KXZ","?_Internal_size@_Concurrent_queue_base_v4@details@Concurrency@@IBEIXZ","guest_concrt_queue_size"},
    {"?_Internal_empty@_Concurrent_queue_base_v4@details@Concurrency@@IEBA_NXZ","?_Internal_empty@_Concurrent_queue_base_v4@details@Concurrency@@IBE_NXZ","guest_concrt_queue_empty"},
    {"?_Internal_finish_clear@_Concurrent_queue_base_v4@details@Concurrency@@IEAAXXZ","?_Internal_finish_clear@_Concurrent_queue_base_v4@details@Concurrency@@IAEXXZ","guest_concrt_queue_finish_clear"},
    {"?_Internal_swap@_Concurrent_queue_base_v4@details@Concurrency@@IEAAXAEAV123@@Z","?_Internal_swap@_Concurrent_queue_base_v4@details@Concurrency@@IAEXAAV123@@Z","guest_concrt_queue_swap"},
    // _Concurrent_queue_iterator_base_v4。
    {"??0_Concurrent_queue_iterator_base_v4@details@Concurrency@@IEAA@AEBV_Concurrent_queue_base_v4@12@@Z","??0_Concurrent_queue_iterator_base_v4@details@Concurrency@@IAE@ABV_Concurrent_queue_base_v4@12@@Z","guest_concrt_iterator_construct"},
    {"?_Assign@_Concurrent_queue_iterator_base_v4@details@Concurrency@@IEAAXAEBV123@@Z","?_Assign@_Concurrent_queue_iterator_base_v4@details@Concurrency@@IAEXABV123@@Z","guest_concrt_iterator_assign"},
    {"?_Advance@_Concurrent_queue_iterator_base_v4@details@Concurrency@@IEAAXXZ","?_Advance@_Concurrent_queue_iterator_base_v4@details@Concurrency@@IAEXXZ","guest_concrt_iterator_advance"},
    {"??1_Concurrent_queue_iterator_base_v4@details@Concurrency@@IEAA@XZ","??1_Concurrent_queue_iterator_base_v4@details@Concurrency@@IAE@XZ","guest_concrt_iterator_destroy"},
    // _Concurrent_vector_base_v4（_Segment_index_of 為 static __cdecl）。
    {"??1_Concurrent_vector_base_v4@details@Concurrency@@IEAA@XZ","??1_Concurrent_vector_base_v4@details@Concurrency@@IAE@XZ","guest_concrt_vector_destroy"},
    {"?_Segment_index_of@_Concurrent_vector_base_v4@details@Concurrency@@KA_K_K@Z","?_Segment_index_of@_Concurrent_vector_base_v4@details@Concurrency@@KAII@Z","guest_concrt_vector_segment_index_of"},
    {"?_Internal_reserve@_Concurrent_vector_base_v4@details@Concurrency@@IEAAX_K00@Z","?_Internal_reserve@_Concurrent_vector_base_v4@details@Concurrency@@IAEXIII@Z","guest_concrt_vector_reserve"},
    {"?_Internal_capacity@_Concurrent_vector_base_v4@details@Concurrency@@IEBA_KXZ","?_Internal_capacity@_Concurrent_vector_base_v4@details@Concurrency@@IBEIXZ","guest_concrt_vector_capacity"},
    {"?_Internal_push_back@_Concurrent_vector_base_v4@details@Concurrency@@IEAAPEAX_KAEA_K@Z","?_Internal_push_back@_Concurrent_vector_base_v4@details@Concurrency@@IAEPAXIAAI@Z","guest_concrt_vector_push_back"},
    {"?_Internal_clear@_Concurrent_vector_base_v4@details@Concurrency@@IEAA_KP6AXPEAX_K@Z@Z","?_Internal_clear@_Concurrent_vector_base_v4@details@Concurrency@@IAEIP6AXPAXI@Z@Z","guest_concrt_vector_clear"}}};
inline constexpr std::string_view throw_exception_x64="?_Internal_throw_exception@_Concurrent_queue_base_v4@details@Concurrency@@IEBAXXZ",
    throw_exception_x86="?_Internal_throw_exception@_Concurrent_queue_base_v4@details@Concurrency@@IBEXXZ";
// host：依 CONCRT_QUEUE_HOST_* 順序的客體入口（配置、釋放、讓出、bad_alloc、_purecall、length_error、std::_Xbad_alloc），映射前寫入映像。
void initialize(GuestMemory&,std::map<std::uint64_t,RuntimeImage>&,bool x86,std::uint64_t gateway,
                const std::array<std::uint64_t,CONCRT_QUEUE_HOST_COUNT>& host);
}
