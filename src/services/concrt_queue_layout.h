/* Concurrency::details::_Concurrent_queue_base_v4（VC14 concurrent_queue.h）的 ABI 佈局。
 * 客體常式（guest_concrt_queue.c）與宿主初始化共用；欄位以指標寬度為單位。
 * 物件：vptr、_My_rep、_Items_per_page、_Item_size；衍生類別的內嵌程式碼直接讀後兩欄。
 * vtable：_Move_item、_Copy_item、_Assign_and_destroy_item、解構、_Allocate_page、_Deallocate_page。 */
#ifndef X64ARM_CONCRT_QUEUE_LAYOUT_H
#define X64ARM_CONCRT_QUEUE_LAYOUT_H
#define CONCRT_QUEUE_VTABLE 0
#define CONCRT_QUEUE_REP 1
#define CONCRT_QUEUE_ITEMS_PER_PAGE 2
#define CONCRT_QUEUE_ITEM_SIZE 3
#define CONCRT_QUEUE_SLOT_MOVE_ITEM 0
#define CONCRT_QUEUE_SLOT_COPY_ITEM 1
#define CONCRT_QUEUE_SLOT_ASSIGN_AND_DESTROY 2
#define CONCRT_QUEUE_SLOT_DESTRUCTOR 3
#define CONCRT_QUEUE_SLOT_ALLOCATE_PAGE 4
#define CONCRT_QUEUE_SLOT_DEALLOCATE_PAGE 5
/* 微佇列數量與分配乘數（ticket*3 mod 8），與 ConcRT 的 _N_queue／_Phi 相同。 */
#define CONCRT_QUEUE_MICRO_QUEUES 8
#define CONCRT_QUEUE_PHI 3
/* 宿主在映射前填入的入口（guest_concrt_host[]）；映像本身沒有匯入表。 */
#define CONCRT_QUEUE_HOST_ALLOCATE 0 /* crt!_aligned_malloc(size,alignment) */
#define CONCRT_QUEUE_HOST_FREE 1     /* crt!_aligned_free(pointer) */
#define CONCRT_QUEUE_HOST_YIELD 2    /* kernel32!SwitchToThread() */
#define CONCRT_QUEUE_HOST_BAD_ALLOC 3 /* concrt140!_Internal_throw_exception：拋出 std::bad_alloc */
#define CONCRT_QUEUE_HOST_PURECALL 4 /* crt!_purecall */
#define CONCRT_QUEUE_HOST_LENGTH_ERROR 5 /* msvcp140!std::_Xlength_error(const char*) */
#define CONCRT_QUEUE_HOST_XBAD_ALLOC 6   /* msvcp140!std::_Xbad_alloc() */
#define CONCRT_QUEUE_HOST_COUNT 7
/* _Concurrent_vector_base_v4（VC14 concurrent_vector.h，TBB 2.x 衍生）：配置函式指標、_My_first_block、_My_early_size、
 * _My_segment、內嵌 _My_storage[3]；衍生類別的內嵌程式碼直接讀分段表（分段 k 起於 (1<<k)&~1、長度 1<<k，分段 0 長度 2）。 */
#define CONCRT_VECTOR_SHORT_TABLE 3
#define CONCRT_VECTOR_WORDS 7
#endif
