/* Concurrency::details::_Concurrent_queue_base_v4、_Concurrent_queue_iterator_base_v4 與 _Concurrent_vector_base_v4（concrt140.dll）的客體常式。
 * 與 ConcRT 相同的演算法：全域 head／tail ticket 以原子指令配號，依 ticket*3 mod 8 分到 8 條微佇列；
 * 每條微佇列以自己的 head／tail 計數排序同佇列的 push／pop，分頁鏈結只在短暫 spin lock 下修改。
 * 元素搬移與分頁配置直接呼叫衍生類別的虛擬函式（客體對客體，不離開 resident AOT、不經宿主協調者）。
 * 映像沒有匯入表；配置、讓出執行與拋出例外的入口由宿主在映射前填入 guest_concrt_host[]。 */
#include "concrt_queue_layout.h"
typedef __SIZE_TYPE__ Size;
typedef __UINTPTR_TYPE__ Pointer;
typedef __UINT32_TYPE__ Word;
#if defined(CONCRT_QUEUE_NATIVE_TEST)
/* 宿主原生單元測試：同一份演算法以原生 ABI 編譯，沒有 SEH 展開表。 */
#define MEMBER
#define CALL
#define EXPORT
#elif defined(__i386__)
#define MEMBER __attribute__((thiscall))
#define CALL __attribute__((stdcall))
#define EXPORT __declspec(dllexport)
#else
#define MEMBER
#define CALL
#define EXPORT __declspec(dllexport)
#endif
#define INLINE static __attribute__((always_inline)) inline
#define QUEUES ((Size)CONCRT_QUEUE_MICRO_QUEUES)

typedef struct Page {struct Page* next;Size mask;} Page;
typedef struct Queue Queue;
typedef void (MEMBER *ItemCall)(Queue*,Pointer,Pointer,Pointer);
typedef Page* (MEMBER *AllocatePage)(Queue*);
typedef void (MEMBER *DeallocatePage)(Queue*,Page*);
typedef struct {Page* head_page;Size head_counter;Page* tail_page;Size tail_counter;Word page_lock;} Micro;
typedef struct {
    Size head_counter;char head_padding[64-sizeof(Size)];
    Size tail_counter;char tail_padding[64-sizeof(Size)];
    Micro array[CONCRT_QUEUE_MICRO_QUEUES];
} Rep;
struct Queue {void* const* vtable;Rep* rep;Size items_per_page,item_size;};
_Static_assert(sizeof(Queue)==4*sizeof(Pointer),"_Concurrent_queue_base_v4 佈局");
_Static_assert(__builtin_offsetof(Queue,rep)==CONCRT_QUEUE_REP*sizeof(Pointer) &&
    __builtin_offsetof(Queue,items_per_page)==CONCRT_QUEUE_ITEMS_PER_PAGE*sizeof(Pointer) &&
    __builtin_offsetof(Queue,item_size)==CONCRT_QUEUE_ITEM_SIZE*sizeof(Pointer),"_Concurrent_queue_base_v4 欄位");
_Static_assert(sizeof(Page)==2*sizeof(Pointer),"_Page 前綴");

/* 初值非零讓陣列留在已初始化資料區；宿主映射前覆寫，之後整個映像唯讀。 */
EXPORT Pointer guest_concrt_host[CONCRT_QUEUE_HOST_COUNT]={1,1,1,1,1,1,1};
typedef void* (*HostAllocate)(Size,Size);
typedef void (*HostFree)(void*);
typedef int (CALL *HostYield)(void);
typedef void (MEMBER *HostThrow)(const Queue*);
typedef void (*HostPurecall)(void);

#define LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)

/* 短暫自旋後讓出時間片：持有 ticket 的執行緒可能未被排上 vCPU（含單核設定），不可無限佔用處理器。 */
INLINE void backoff(unsigned* spins){
    if(++*spins<32){__asm__ volatile("pause");return;}
    *spins=0;(void)((HostYield)guest_concrt_host[CONCRT_QUEUE_HOST_YIELD])();
}
INLINE void wait_until_equal(const Size* location,Size value){unsigned spins=0;while(LOAD(location)!=value)backoff(&spins);}
INLINE void wait_while_equal(const Size* location,Size value){unsigned spins=0;while(LOAD(location)==value)backoff(&spins);}
INLINE void lock_pages(Micro* queue){unsigned spins=0;while(__atomic_exchange_n(&queue->page_lock,1,__ATOMIC_ACQUIRE))backoff(&spins);}
INLINE void unlock_pages(Micro* queue){STORE(&queue->page_lock,0);}

/* ConcRT 的 push／pop finalizer：不論元素建構或指派是否拋出例外，都要推進微佇列計數並釋放用完的分頁，
 * 否則後續同一微佇列的 ticket 會永遠等待。 */
typedef struct {Micro* queue;Size ticket;Page* page;Queue* base;int pop;} Guard;
static __attribute__((noinline)) void finalize(Guard* guard){
    Micro* queue=guard->queue;
    if(!guard->pop){STORE(&queue->tail_counter,guard->ticket);return;}
    Page* page=guard->page;
    if(page){
        lock_pages(queue);
        Page* next=page->next;queue->head_page=next;if(!next)queue->tail_page=0;
        unlock_pages(queue);
    }
    STORE(&queue->head_counter,guard->ticket);
    if(page)((DeallocatePage)guard->base->vtable[CONCRT_QUEUE_SLOT_DEALLOCATE_PAGE])(guard->base,page);
}

#if defined(__x86_64__) && !defined(CONCRT_QUEUE_NATIVE_TEST)
/* x64：經過帶有 unwind handler 的跳板呼叫衍生類別；C++ 例外展開穿越時執行 finalizer（等同 ConcRT 的 RAII 物件）。
 * 跳板框架 56 bytes，guard 存在 establisher frame+32（被呼叫者只有四個暫存器參數，不使用該位置）。 */
typedef struct {Word code,flags;} ExceptionRecord;
__attribute__((used)) int guest_concrt_unwind(ExceptionRecord* record,void* frame,void* context,void* dispatcher){
    (void)context;(void)dispatcher;
    /* EXCEPTION_UNWINDING｜EXIT_UNWIND｜TARGET_UNWIND｜COLLIDED_UNWIND */
    if(record->flags&0x66U)finalize(*(Guard**)((char*)frame+32));
    return 1; /* ExceptionContinueSearch */
}
void guest_concrt_guarded(Guard*,void*,Pointer,Pointer,Pointer,Pointer);
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl guest_concrt_guarded\n"
    ".def guest_concrt_guarded; .scl 2; .type 32; .endef\n"
    ".seh_proc guest_concrt_guarded\n"
    "guest_concrt_guarded:\n"
    "    subq $56, %rsp\n"
    "    .seh_stackalloc 56\n"
    "    .seh_endprologue\n"
    "    movq %rcx, 32(%rsp)\n"
    "    movq %rdx, %rax\n"
    "    movq %r8, %rcx\n"
    "    movq %r9, %rdx\n"
    "    movq 96(%rsp), %r8\n"
    "    movq 104(%rsp), %r9\n"
    "    callq *%rax\n"
    "    nop\n"
    "    addq $56, %rsp\n"
    "    retq\n"
    "    .seh_handler guest_concrt_unwind, @unwind\n"
    "    .seh_endproc\n");
INLINE void call_item(Guard* guard,void* target,Queue* self,Pointer a,Pointer b,Pointer c){
    guest_concrt_guarded(guard,target,(Pointer)self,a,b,c);
}
#else
/* Win32 沒有表格式 unwind；例外穿越時不補做 finalizer（與未加例外處理的實作相同，僅正常返回路徑完整）。 */
INLINE void call_item(Guard* guard,void* target,Queue* self,Pointer a,Pointer b,Pointer c){
    (void)guard;((ItemCall)target)(self,a,b,c);
}
#endif

INLINE Micro* choose(Rep* rep,Size ticket){return &rep->array[ticket*CONCRT_QUEUE_PHI%QUEUES];}

static void push(Queue* self,Pointer source,unsigned slot){
    Rep* rep=self->rep;
    Size ticket=__atomic_fetch_add(&rep->tail_counter,1,__ATOMIC_SEQ_CST);
    Micro* queue=choose(rep,ticket);
    ticket&=~(QUEUES-1);
    const Size index=ticket/QUEUES&(self->items_per_page-1);
    Page* page=0;
    if(!index){
        /* 衍生類別的 _Allocate_page 配置失敗時自行呼叫 _Internal_throw_exception。 */
        page=((AllocatePage)self->vtable[CONCRT_QUEUE_SLOT_ALLOCATE_PAGE])(self);
        page->mask=0;page->next=0;
    }
    wait_until_equal(&queue->tail_counter,ticket);
    if(page){
        lock_pages(queue);
        Page* tail=queue->tail_page;
        if(tail)tail->next=page;else queue->head_page=page;
        queue->tail_page=page;
        unlock_pages(queue);
    }else page=queue->tail_page;
    Guard guard={queue,ticket+QUEUES,0,self,0};
    call_item(&guard,self->vtable[slot],self,(Pointer)page,index,source);
    /* 建構成功才標記該格有效；pop 依 mask 決定是否指派並解構。 */
    STORE(&page->mask,page->mask|(Size)1<<index);
    finalize(&guard);
}

static int pop(Micro* queue,void* destination,Size ticket,Queue* self){
    ticket&=~(QUEUES-1);
    wait_until_equal(&queue->head_counter,ticket);
    wait_while_equal(&queue->tail_counter,ticket);
    Page* page=queue->head_page;
    const Size index=ticket/QUEUES&(self->items_per_page-1);
    Guard guard={queue,ticket+QUEUES,index==self->items_per_page-1?page:0,self,1};
    int success=0;
    if(LOAD(&page->mask)>>index&1){
        success=1;
        call_item(&guard,self->vtable[CONCRT_QUEUE_SLOT_ASSIGN_AND_DESTROY],self,(Pointer)destination,(Pointer)page,index);
    }
    finalize(&guard);
    return success;
}

/* 抽象基底的 vtable：建構與解構期間 vptr 指向這裡；純虛擬格轉 _purecall。 */
static void pure_virtual(void){((HostPurecall)guest_concrt_host[CONCRT_QUEUE_HOST_PURECALL])();}
EXPORT void MEMBER guest_concrt_queue_destroy(Queue* self);
static void* MEMBER deleting_destructor(Queue* self,unsigned flags){
    /* 抽象類別不會以基底 vptr 成為完整物件；儲存區由衍生類別的 deleting destructor 釋放。 */
    (void)flags;guest_concrt_queue_destroy(self);return self;
}
static void* const base_vtable[1+6]={0,
    (void*)pure_virtual,(void*)pure_virtual,(void*)pure_virtual,(void*)deleting_destructor,(void*)pure_virtual,(void*)pure_virtual};

EXPORT Queue* MEMBER guest_concrt_queue_construct(Queue* self,Size item_size){
    self->vtable=&base_vtable[1];
    self->items_per_page=item_size<=8?32:item_size<=16?16:item_size<=32?8:item_size<=64?4:item_size<=128?2:1;
    Rep* rep=(Rep*)((HostAllocate)guest_concrt_host[CONCRT_QUEUE_HOST_ALLOCATE])(sizeof(Rep),64);
    if(!rep){
        ((HostThrow)guest_concrt_host[CONCRT_QUEUE_HOST_BAD_ALLOC])(self);
        __builtin_trap();
    }
    volatile Size* words=(volatile Size*)rep;
    for(Size i=0;i<sizeof(Rep)/sizeof(Size);++i)words[i]=0;
    self->rep=rep;self->item_size=item_size;
    return self;
}
EXPORT void MEMBER guest_concrt_queue_destroy(Queue* self){
    self->vtable=&base_vtable[1];
    ((HostFree)guest_concrt_host[CONCRT_QUEUE_HOST_FREE])(self->rep);
}
EXPORT void MEMBER guest_concrt_queue_push(Queue* self,const void* source){push(self,(Pointer)source,CONCRT_QUEUE_SLOT_COPY_ITEM);}
EXPORT void MEMBER guest_concrt_queue_move_push(Queue* self,void* source){push(self,(Pointer)source,CONCRT_QUEUE_SLOT_MOVE_ITEM);}
EXPORT unsigned char MEMBER guest_concrt_queue_pop_if_present(Queue* self,void* destination){
    Rep* rep=self->rep;Size ticket;
    do{
        for(;;){
            ticket=LOAD(&rep->head_counter);
            /* 佇列在觀察當下是空的。 */
            if((__PTRDIFF_TYPE__)(LOAD(&rep->tail_counter)-ticket)<=0)return 0;
            /* 取得 ticket；被其他執行緒搶先就重試。 */
            Size expected=ticket;
            if(__atomic_compare_exchange_n(&rep->head_counter,&expected,ticket+1,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))break;
        }
        /* 該 ticket 的元素建構曾失敗（mask 未設）時取下一個。 */
    }while(!pop(choose(rep,ticket),destination,ticket,self));
    return 1;
}
EXPORT Size MEMBER guest_concrt_queue_size(const Queue* self){
    const Rep* rep=self->rep;
    return (Size)(__PTRDIFF_TYPE__)(LOAD(&rep->tail_counter)-LOAD(&rep->head_counter));
}
EXPORT unsigned char MEMBER guest_concrt_queue_empty(const Queue* self){
    const Rep* rep=self->rep;
    const Size tail=LOAD(&rep->tail_counter),head=LOAD(&rep->head_counter);
    /* 兩次讀取間 tail 改變，表示其間佇列曾經非空。 */
    return tail==LOAD(&rep->tail_counter) && (__PTRDIFF_TYPE__)(tail-head)<=0;
}
EXPORT void MEMBER guest_concrt_queue_finish_clear(Queue* self){
    Rep* rep=self->rep;
    for(Size i=0;i<QUEUES;++i){
        Micro* queue=&rep->array[i];
        Page* tail=queue->tail_page;
        if(!tail)continue;
        /* 清空後每條微佇列至多留下一頁（尾頁）；交還衍生類別的配置器。 */
        ((DeallocatePage)self->vtable[CONCRT_QUEUE_SLOT_DEALLOCATE_PAGE])(self,tail);
        if(queue->head_page==tail)queue->head_page=0;
        queue->tail_page=0;
    }
}
EXPORT void MEMBER guest_concrt_queue_swap(Queue* self,Queue* other){
    Rep* rep=self->rep;self->rep=other->rep;other->rep=rep;
}

/* ---- _Concurrent_queue_iterator_base_v4：{_My_rep, _My_item}。rep 記錄目前 ticket 與 8 條微佇列的目前頁，
 * 與 ConcRT 相同：ticket 等於佇列 tail 時為結尾（item NULL），元素建構失敗（mask 未設）的格子略過。 */
typedef struct {Size head_counter;const Queue* queue;Page* array[CONCRT_QUEUE_MICRO_QUEUES];} IteratorRep;
typedef struct {IteratorRep* rep;void* item;} Iterator;
_Static_assert(sizeof(Iterator)==2*sizeof(Pointer),"_Concurrent_queue_iterator_base_v4 佈局");
static int iterator_get(IteratorRep* rep,void** item,Size ticket){
    const Queue* queue=rep->queue;
    if(ticket==LOAD(&queue->rep->tail_counter)){*item=0;return 1;}
    Page* page=rep->array[ticket*CONCRT_QUEUE_PHI%QUEUES];
    const Size index=ticket/QUEUES&(queue->items_per_page-1);
    *item=(unsigned char*)(page+1)+queue->item_size*index;
    return (LOAD(&page->mask)>>index&1)!=0;
}
static IteratorRep* iterator_rep_allocate(const Queue* owner){
    IteratorRep* rep=(IteratorRep*)((HostAllocate)guest_concrt_host[CONCRT_QUEUE_HOST_ALLOCATE])(sizeof(IteratorRep),sizeof(Pointer));
    if(!rep){((HostThrow)guest_concrt_host[CONCRT_QUEUE_HOST_BAD_ALLOC])(owner);__builtin_trap();}
    return rep;
}
EXPORT void MEMBER guest_concrt_iterator_advance(Iterator* self);
EXPORT Iterator* MEMBER guest_concrt_iterator_construct(Iterator* self,const Queue* queue){
    IteratorRep* rep=iterator_rep_allocate(queue);
    rep->head_counter=LOAD(&queue->rep->head_counter);rep->queue=queue;
    for(Size i=0;i<QUEUES;++i)rep->array[i]=queue->rep->array[i].head_page;
    self->rep=rep;self->item=0;
    if(!iterator_get(rep,&self->item,rep->head_counter))guest_concrt_iterator_advance(self);
    return self;
}
EXPORT void MEMBER guest_concrt_iterator_assign(Iterator* self,const Iterator* other){
    if(self->rep!=other->rep){
        if(self->rep){((HostFree)guest_concrt_host[CONCRT_QUEUE_HOST_FREE])(self->rep);self->rep=0;}
        if(other->rep){IteratorRep* rep=iterator_rep_allocate(other->rep->queue);*rep=*other->rep;self->rep=rep;}
    }
    self->item=other->item;
}
EXPORT void MEMBER guest_concrt_iterator_advance(Iterator* self){
    IteratorRep* rep=self->rep;
    for(;;){
        Size ticket=rep->head_counter;const Queue* queue=rep->queue;
        /* 離開頁面最後一格時，該微佇列改指下一頁。 */
        if((ticket/QUEUES&(queue->items_per_page-1))==queue->items_per_page-1){Page** root=&rep->array[ticket*CONCRT_QUEUE_PHI%QUEUES];*root=(*root)->next;}
        rep->head_counter=++ticket;
        if(iterator_get(rep,&self->item,ticket))return;
    }
}
EXPORT void MEMBER guest_concrt_iterator_destroy(Iterator* self){
    if(self->rep)((HostFree)guest_concrt_host[CONCRT_QUEUE_HOST_FREE])(self->rep);
    self->rep=0;
}

/* ---- _Concurrent_vector_base_v4（TBB 2.x 演算法）：push_back 以原子遞增 _My_early_size 取得索引，分段首格的取得者配置分段，
 * 其他執行緒等待分段發布；_My_first_block 個分段共用一次配置；超過內嵌 3 格時改用 64 格長分段表（以 CAS 發布）。
 * 配置失敗的分段以 _BAD_ALLOC_MARKER（63）標記。配置由衍生類別的 _My_vector_allocator_ptr 提供（元素數）。 */
typedef struct {void* array;} Segment;
typedef struct Vector Vector;
typedef void* (*VectorAllocate)(Vector*,Size);
typedef void (*VectorArrayOp)(void*,Size);
struct Vector {VectorAllocate allocate;Size first_block;Size early_size;Segment* segment;Segment storage[CONCRT_VECTOR_SHORT_TABLE];};
_Static_assert(sizeof(Vector)==CONCRT_VECTOR_WORDS*sizeof(Pointer),"_Concurrent_vector_base_v4 佈局");
#define VECTOR_LONG_TABLE (sizeof(Size)*8)
#define BAD_ALLOC_MARKER ((Pointer)63)
typedef void (*HostLengthError)(const char*);
typedef void (*HostBadAlloc)(void);
static __attribute__((noreturn)) void vector_bad_alloc(void){((HostBadAlloc)guest_concrt_host[CONCRT_QUEUE_HOST_XBAD_ALLOC])();__builtin_trap();}
INLINE Size segment_index_of(Size index){index|=1;return (Size)(sizeof(Size)*8-1)-(Size)(sizeof(Size)==8?__builtin_clzll((unsigned long long)index):__builtin_clz((unsigned)index));}
INLINE Size segment_base(Size k){return ((Size)1<<k)&~(Size)1;}
INLINE Size segment_size(Size k){return (Size)1<<k;}
INLINE void wait_while_null(void* const* location){unsigned spins=0;while(!LOAD(location))backoff(&spins);}
static Size find_segment_end(Vector* v){
    Segment* s=LOAD(&v->segment);const Size limit=s==v->storage?CONCRT_VECTOR_SHORT_TABLE:VECTOR_LONG_TABLE;
    Size k=0;while(k<limit && (Pointer)LOAD(&s[k].array)>BAD_ALLOC_MARKER)++k;
    return k;
}
INLINE void assign_first_segment(Vector* v,Size k){
    if(!LOAD(&v->first_block)){Size expected=0;__atomic_compare_exchange_n(&v->first_block,&expected,k+1,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);}
}
INLINE void* allocate_segment(Vector* v,Size count){void* p=v->allocate(v,count);if(!p)vector_bad_alloc();return p;}
static Size enable_segment(Vector* v,Size k,Size element_size){
    Segment* s=LOAD(&v->segment);
    if(!k){
        assign_first_segment(v,0);
        STORE(&s[0].array,allocate_segment(v,segment_size(LOAD(&v->first_block))));
        return 2;
    }
    if(!LOAD(&v->first_block)){unsigned spins=0;while(!LOAD(&v->first_block))backoff(&spins);}
    if(k<LOAD(&v->first_block)){
        /* 首區塊內的分段指向分段 0 那次配置的對應位置。 */
        wait_while_null(&s[0].array);
        void* first=LOAD(&s[0].array);
        if((Pointer)first<=BAD_ALLOC_MARKER){STORE(&s[k].array,(void*)BAD_ALLOC_MARKER);return 0;}
        STORE(&s[k].array,(void*)((unsigned char*)first+segment_base(k)*element_size));
    }else STORE(&s[k].array,allocate_segment(v,segment_size(k)));
    return segment_size(k);
}
static void extend_segment_table(Vector* v,Size start){
    if(start>segment_size(CONCRT_VECTOR_SHORT_TABLE))start=segment_size(CONCRT_VECTOR_SHORT_TABLE);
    /* 其他執行緒仍在寫內嵌表時，等它們發布後才複製到長表。 */
    for(Size i=0;segment_base(i)<start && LOAD(&v->segment)==v->storage;++i)
        if(!LOAD(&v->storage[i].array)){unsigned spins=0;while(LOAD(&v->segment)==v->storage && !LOAD(&v->storage[i].array))backoff(&spins);}
    if(LOAD(&v->segment)!=v->storage)return;
    Segment* table=(Segment*)((HostAllocate)guest_concrt_host[CONCRT_QUEUE_HOST_ALLOCATE])(VECTOR_LONG_TABLE*sizeof(Segment),64);
    if(!table)vector_bad_alloc();
    for(Size i=0;i<VECTOR_LONG_TABLE;++i)table[i].array=i<CONCRT_VECTOR_SHORT_TABLE?LOAD(&v->storage[i].array):0;
    Segment* expected=v->storage;
    if(!__atomic_compare_exchange_n(&v->segment,&expected,table,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))((HostFree)guest_concrt_host[CONCRT_QUEUE_HOST_FREE])(table);
}
INLINE void extend_table_if_necessary(Vector* v,Size k,Size start){if(k>=CONCRT_VECTOR_SHORT_TABLE && LOAD(&v->segment)==v->storage)extend_segment_table(v,start);}
EXPORT Size guest_concrt_vector_segment_index_of(Size index){return segment_index_of(index);}
EXPORT void MEMBER guest_concrt_vector_destroy(Vector* self){
    Segment* table=self->segment;
    if(table!=self->storage){
        for(Size i=0;i<CONCRT_VECTOR_SHORT_TABLE;++i)self->storage[i].array=0;
        self->segment=self->storage;((HostFree)guest_concrt_host[CONCRT_QUEUE_HOST_FREE])(table);
    }
}
EXPORT void MEMBER guest_concrt_vector_reserve(Vector* self,Size count,Size element_size,Size max_size){
    static const char message[]="Invalid reservation size";
    if(count>max_size){((HostLengthError)guest_concrt_host[CONCRT_QUEUE_HOST_LENGTH_ERROR])(message);__builtin_trap();}
    if(!count)return;
    assign_first_segment(self,segment_index_of(count-1));
    for(Size k=find_segment_end(self);segment_base(k)<count;++k){
        extend_table_if_necessary(self,k,0);
        if((Pointer)LOAD(&LOAD(&self->segment)[k].array)<=BAD_ALLOC_MARKER)enable_segment(self,k,element_size);
    }
}
EXPORT Size MEMBER guest_concrt_vector_capacity(Vector* self){return segment_base(find_segment_end(self));}
EXPORT void* MEMBER guest_concrt_vector_push_back(Vector* self,Size element_size,Size* index){
    const Size item=__atomic_fetch_add(&self->early_size,1,__ATOMIC_SEQ_CST);
    *index=item;
    const Size k=segment_index_of(item),base=segment_base(k);
    extend_table_if_necessary(self,k,item);
    /* 分段首格的取得者負責配置；其他執行緒等分段發布。配置失敗的分段不可再使用。 */
    if((Pointer)LOAD(&LOAD(&self->segment)[k].array)<=BAD_ALLOC_MARKER){
        if(base==item)enable_segment(self,k,element_size);
        else {unsigned spins=0;while(!LOAD(&LOAD(&self->segment)[k].array))backoff(&spins);}
    }
    void* array=LOAD(&LOAD(&self->segment)[k].array);
    if((Pointer)array<=BAD_ALLOC_MARKER)vector_bad_alloc();
    return (unsigned char*)array+element_size*(item-base);
}
EXPORT Size MEMBER guest_concrt_vector_clear(Vector* self,VectorArrayOp destroy){
    /* 先把大小歸零，同時存取的下標即失敗；之後由高到低解構各分段已使用的元素。回傳使用中的分段數，供衍生類別釋放分段。 */
    const Size end=find_segment_end(self);
    Size finish=self->early_size;self->early_size=0;
    while(finish>0){
        const Size k=segment_index_of(finish-1),base=segment_base(k),count=finish-base;
        finish=base;
        if(k<=end){void* array=self->segment[k].array;if((Pointer)array>BAD_ALLOC_MARKER)destroy(array,count);}
    }
    return end;
}
