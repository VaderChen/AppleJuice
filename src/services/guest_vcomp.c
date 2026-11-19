// 平行區域完全使用客體執行緒。區域堆疊持有團隊，所有工作者結束後才返回。
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>

#define API __declspec(dllexport)
typedef uintptr_t Word;
typedef uint64_t U64;
typedef struct Loop Loop;
struct Loop {
    U64 first,last,stride,remaining,chunk;
    unsigned kind,users;
    U64 generation;
    Loop *next;
};
typedef struct {
    U64 generation;
    unsigned count,next;
} Sections;
typedef struct {
    void *body;
    Word *args;
    int nargs,size;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE changed;
    unsigned waiting,phase;
    Loop *loops;
    Sections sections;
    uint32_t single_generation;
} Team;
typedef struct {
    int threads,nested,dynamic;
} Settings;
typedef struct {
    Settings config;
    Team *team;
    int rank,parallel,next_threads;
    U64 generation;
    Loop local,*loop;
    U64 section_generation;
    Sections serial_sections;
    uint32_t single_generation;
} Member;
typedef struct {Member member; HANDLE handle;} Worker;

extern void vcomp_call(void *body,unsigned count,const Word *args);
static INIT_ONCE once=INIT_ONCE_STATIC_INIT;
static DWORD slot;
static int processors;
static void *allocate(SIZE_T size){
    void *p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,size);
    if(!p)ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}
static void dispose(void *p){HeapFree(GetProcessHeap(),0,p);}
/* 呼叫端持有共用的指標槽，進入時才配置，離開時直接使用已發布的鎖。 */
API void _vcomp_enter_critsect(CRITICAL_SECTION **slot_address){
    CRITICAL_SECTION *lock=__atomic_load_n(slot_address,__ATOMIC_ACQUIRE);
    if(!lock){
        CRITICAL_SECTION *candidate=allocate(sizeof(*candidate));
        if(!InitializeCriticalSectionEx(candidate,0,RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO)){
            DWORD error=GetLastError();
            dispose(candidate);
            ExitProcess(error);
        }
        if(__atomic_compare_exchange_n(slot_address,&lock,candidate,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
            lock=candidate;
        }else{
            DeleteCriticalSection(candidate);
            dispose(candidate);
        }
    }
    EnterCriticalSection(lock);
}
API void _vcomp_leave_critsect(CRITICAL_SECTION *lock){
    LeaveCriticalSection(lock);
}
static void CALLBACK free_member(void *p){if(p)dispose(p);}
static BOOL CALLBACK initialize(PINIT_ONCE unused,void *arg,void **context){
    (void)unused;(void)arg;(void)context;
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    processors=(int)info.dwNumberOfProcessors;
    slot=FlsAlloc(free_member);
    return slot!=FLS_OUT_OF_INDEXES;
}
// FLS 的擁有者永遠是堆積配置；即使工作者提前離開也不會釋放堆疊上的團隊框架。
typedef struct {Member root;Member *active;} Local;
static Local *local(void){
    if(!InitOnceExecuteOnce(&once,initialize,0,0))ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
    Local *l=FlsGetValue(slot);
    if(!l){
        l=allocate(sizeof(*l));l->root.config.threads=processors;l->active=&l->root;
        if(!FlsSetValue(slot,l)){dispose(l);ExitProcess(ERROR_NOT_ENOUGH_MEMORY);}
    }
    return l;
}
static Member *current(void){return local()->active;}
static void install(Member *m){Local *l=local();l->active=m?m:&l->root;}
API int omp_get_num_procs(void){current();return processors;}
API int omp_get_max_threads(void){return current()->config.threads;}
API void omp_set_num_threads(int n){if(n>0)current()->config.threads=n;}
API void _vcomp_set_num_threads(int n){if(n>0)current()->next_threads=n;}
API int omp_get_num_threads(void){Team *team=current()->team;return team?team->size:1;}
API int omp_get_thread_num(void){return current()->rank;}
API int _vcomp_get_thread_num(void){return omp_get_thread_num();}
API int omp_in_parallel(void){return current()->parallel;}
API int omp_get_nested(void){return current()->config.nested;}
API void omp_set_nested(int n){current()->config.nested=n!=0;}
API int omp_get_dynamic(void){return current()->config.dynamic;}
API void omp_set_dynamic(int n){current()->config.dynamic=n!=0;}
API void _vcomp_flush(void){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
API void _vcomp_barrier(void){
    Team *t=current()->team;
    if(!t || t->size==1)return;
    EnterCriticalSection(&t->lock);
    unsigned phase=t->phase;
    if(++t->waiting==(unsigned)t->size){
        t->waiting=0;++t->phase;
        WakeAllConditionVariable(&t->changed);
    }else while(t->phase==phase)SleepConditionVariableCS(&t->changed,&t->lock,INFINITE);
    LeaveCriticalSection(&t->lock);
}
/* 每個成員按遇到 single 的次序編號。只有第一個將團隊推進至該輪的成員能進入。 */
API int _vcomp_single_begin(int flags){
    (void)flags;
    Member *m=current();
    if(!m->team || m->team->size==1)return 1;
    uint32_t round=++m->single_generation;
    uint32_t *latest=&m->team->single_generation;
    uint32_t observed=__atomic_load_n(latest,__ATOMIC_RELAXED);
    while((int32_t)(round-observed)>0){
        if(__atomic_compare_exchange_n(latest,&observed,round,0,__ATOMIC_SEQ_CST,__ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}
API void _vcomp_single_end(void){
    /* 下一輪可與本輪重疊；需要等待時，呼叫端會另行呼叫 barrier。 */
}
/* nowait 允許較快的成員先開始下一個區段群組。落後的成員不可領取新一輪的索引。 */
API void _vcomp_sections_init(int count){
    Member *m=current();
    Team *team=m->team;
    Sections *s=team?&team->sections:&m->serial_sections;
    ++m->section_generation;
    if(team)EnterCriticalSection(&team->lock);
    if(m->section_generation>s->generation){
        s->generation=m->section_generation;
        s->count=(unsigned)count;
        s->next=0;
    }
    if(team)LeaveCriticalSection(&team->lock);
}
API int _vcomp_sections_next(void){
    Member *m=current();
    Team *team=m->team;
    Sections *s=team?&team->sections:&m->serial_sections;
    int index=-1;
    if(team)EnterCriticalSection(&team->lock);
    if(s->generation==m->section_generation && s->next!=s->count)
        index=(int)s->next++;
    if(team)LeaveCriticalSection(&team->lock);
    return index;
}
static DWORD WINAPI run_worker(void *param){
    Member *m=param;
    Team *t=m->team;
    // 建立者完成所有執行緒的配置後才公布最終隊伍大小。
    EnterCriticalSection(&t->lock);
    LeaveCriticalSection(&t->lock);
    install(m);
    vcomp_call(t->body,t->nargs,t->args);
    install(0);
    return 0;
}
static void fork_region(int parallel,int count,void *body,Word *args){
    Member *parent=current();
    int requested=parent->next_threads?parent->next_threads:parent->config.threads;
    parent->next_threads=0;
    parallel=parallel && (!parent->parallel || parent->config.nested);
    if(!parallel)requested=1;
    if(parent->config.dynamic && requested>processors)requested=processors;
    if(count<0 || !body || (SIZE_T)requested>SIZE_MAX/sizeof(Worker))ExitProcess(ERROR_INVALID_PARAMETER);
    Team team={.body=body,.args=args,.nargs=count,.size=1};
    InitializeCriticalSection(&team.lock);
    InitializeConditionVariable(&team.changed);
    Member primary={.config=parent->config,.team=&team,.parallel=parallel||parent->parallel};
    Worker *workers=requested>1?allocate((requested-1)*sizeof(*workers)):0;
    EnterCriticalSection(&team.lock);
    for(int i=1;i<requested;++i){
        Worker *w=&workers[i-1];
        w->member=primary; w->member.rank=i;
        w->handle=CreateThread(0,0,run_worker,&w->member,0,0);
        if(!w->handle)break;
        ++team.size;
    }
    LeaveCriticalSection(&team.lock);
    install(&primary);
    vcomp_call(body,count,args);
    install(parent);
    for(int i=1;i<team.size;++i){
        HANDLE handle=workers[i-1].handle;
        if(WaitForSingleObject(handle,INFINITE)!=WAIT_OBJECT_0)ExitProcess(ERROR_INVALID_HANDLE);
        CloseHandle(handle);
    }
    if(workers)dispose(workers);
    while(team.loops){Loop *next=team.loops->next;dispose(team.loops);team.loops=next;}
    DeleteCriticalSection(&team.lock);
}
API void _vcomp_fork(int parallel,int nargs,void *body,...){
    va_list args;va_start(args,body);
    fork_region(parallel,nargs,body,(Word*)args);
    va_end(args);
}

// 對包含端點的範圍以「最後迭代索引」作除法，避免迭代數加一時溢位。
static void partition(U64 final,unsigned size,unsigned rank,U64 *offset,U64 *count){
    U64 base=final/size;
    unsigned extra=(unsigned)(final%size)+1;
    *offset=rank*base+(rank<extra?rank:extra);
    *count=base+(rank<extra);
}
static void simple(U64 first,U64 last,U64 step,int up,U64 mask,U64 *begin,U64 *end){
    unsigned size=omp_get_num_threads();
    if(size==1){*begin=first;*end=last;return;}
    if(!step || step>mask/2){*begin=0;*end=up?mask:1;return;}
    U64 index=((up?last-first:first-last)&mask)/step,offset,n;
    partition(index,size,omp_get_thread_num(),&offset,&n);
    U64 stride=up?step:0-step;
    *begin=n?first+offset*stride:first;
    *end=*begin+(n-1)*stride;
}
API void _vcomp_for_static_simple_init(unsigned a,unsigned b,int step,int up,unsigned *lo,unsigned *hi){
    U64 l,h;simple(a,b,(unsigned)step,up,UINT32_MAX,&l,&h);*lo=(unsigned)l;*hi=(unsigned)h;
}
API void _vcomp_for_static_simple_init_i8(U64 a,U64 b,int64_t step,int up,U64 *lo,U64 *hi){
    simple(a,b,(U64)step,up,UINT64_MAX,lo,hi);
}
typedef struct {U64 loops,begin,end,next,lastchunk;int bounds;} Chunks;
static Chunks chunks(int64_t a,int64_t b,int64_t step,int64_t chunk){
    Chunks r={0};
    unsigned size=omp_get_num_threads(),rank=omp_get_thread_num();
    if((size==1 && chunk!=1) || a==b){
        if(!rank){r.loops=1;r.bounds=1;r.begin=a;r.end=b;r.lastchunk=a;}
    }else if(step>0){
        U64 span=a<b?(U64)b-(U64)a:(U64)a-(U64)b;
        U64 stride=a<b?(U64)step:0-(U64)step;
        U64 width=chunk>0?(U64)chunk:1;
        U64 final=span/(U64)step/width;
        r.bounds=1;r.loops=final/size+(rank<=final%size);
        r.begin=(U64)a+rank*width*stride;
        r.end=r.begin+(width-1)*stride;
        r.next=width*size*stride;r.lastchunk=(U64)a+final*width*stride;
    }
    return r;
}
#define STATIC_INIT(name,T,S) \
API void name(S a,S b,S step,S chunk,T *loops,T *begin,T *end,T *next,T *lastchunk){ \
    Chunks r=chunks(a,b,step,chunk);*loops=(T)r.loops; \
    if(r.bounds){if(begin){*begin=(T)r.begin;*lastchunk=(T)r.lastchunk;}*end=(T)r.end;*next=(T)r.next;} \
}
STATIC_INIT(_vcomp_for_static_init,unsigned,int)
STATIC_INIT(_vcomp_for_static_init_i8,U64,int64_t)
// 靜態區塊在 init 即計算完畢；結束沒有隱含的團隊屏障。
API void _vcomp_for_static_end(void){}

static void dynamic_init(unsigned flags,U64 a,U64 b,U64 step,U64 chunk,U64 mask){
    Member *m=current();Team *t=m->team;
    int up=(flags&0x40)!=0;
    unsigned kind=flags&~0x40U;
    if(kind!=1 && kind!=2)kind=3;
    m->loop=0;
    if(!step || step>mask/2)return;
    U64 final=((up?b-a:a-b)&mask)/step;
    U64 stride=up?step:0-step;
    Loop init={.first=a,.last=b,.stride=stride,.remaining=final,.chunk=chunk?chunk:1,.kind=kind};
    if(kind==1){
        if((t?t->size:1)>1){
            U64 offset,n;
            partition(final,t->size,m->rank,&offset,&n);
            if(!n)return;
            init.first+=offset*stride;init.remaining=n-1;
        }
        init.last=init.first+init.remaining*stride;
    }
    if(!t || kind==1){
        m->local=init;m->loop=&m->local;
    }else{
        ++m->generation;
        EnterCriticalSection(&t->lock);
        Loop *p=t->loops;
        while(p && p->generation!=m->generation)p=p->next;
        if(!p){
            p=allocate(sizeof(*p));*p=init;p->generation=m->generation;
            p->users=t->size;p->next=t->loops;t->loops=p;
        }
        m->loop=p;
        LeaveCriticalSection(&t->lock);
    }
}
static int dynamic_next(U64 *lo,U64 *hi){
    Member *m=current();Loop *p=m->loop;
    if(!p)return 0;
    Team *shared=p!=&m->local?m->team:0;
    if(shared)EnterCriticalSection(&shared->lock);
    int available=p->kind!=0;
    if(available){
        U64 take=p->chunk;
        if(p->kind==3){
            U64 guided=p->remaining/(shared?shared->size:1);
            if(guided>=take)take=guided+1;
        }
        *lo=p->first;
        if(p->kind==1 || take==0 || take>p->remaining){*hi=p->last;p->kind=0;}
        else{*hi=*lo+(take-1)*p->stride;p->first=*hi+p->stride;p->remaining-=take;}
    }else{
        m->loop=0;
        if(shared && !--p->users){
            Loop **link=&shared->loops;
            while(*link!=p)link=&(*link)->next;
            *link=p->next;dispose(p);
        }
    }
    if(shared)LeaveCriticalSection(&shared->lock);
    return available;
}
API void _vcomp_for_dynamic_init(unsigned f,unsigned a,unsigned b,int step,unsigned chunk){
    dynamic_init(f,a,b,(unsigned)step,chunk,UINT32_MAX);
}
API void _vcomp_for_dynamic_init_i8(unsigned f,U64 a,U64 b,int64_t step,U64 chunk){
    dynamic_init(f,a,b,(U64)step,chunk,UINT64_MAX);
}
API int _vcomp_for_dynamic_next(unsigned *lo,unsigned *hi){
    U64 l,h;if(!dynamic_next(&l,&h))return 0;*lo=(unsigned)l;*hi=(unsigned)h;return 1;
}
API int _vcomp_for_dynamic_next_i8(U64 *lo,U64 *hi){return dynamic_next(lo,hi);}

// 編譯器向量化入口的前兩個回呼參數是每次分配的範圍，後面保留原有捕捉值。
typedef struct {
    int first,last,step,inclusive,dynamic,count;
    void *body;
    Word *captures;
    U64 iterations;
} VectorLoop;
static void invoke_range(VectorLoop *v,int first,int last){
    Word args[128];
    args[0]=(Word)(intptr_t)first;args[1]=(Word)(intptr_t)last;
    for(int i=2;i<v->count;++i)args[i]=v->captures[i-2];
    vcomp_call(v->body,v->count,args);
}
static void vector_worker(VectorLoop *v){
    U64 lo,hi;
    unsigned size=omp_get_num_threads(),rank=omp_get_thread_num();
    if(v->dynamic)dynamic_init(0x43,0,v->iterations-1,1,1,UINT64_MAX);
    do{
        if(v->dynamic){if(!dynamic_next(&lo,&hi))break;}
        else{
            U64 n;partition(v->iterations-1,size,rank,&lo,&n);
            if(!n){
                if(!v->inclusive && (v->step==1 || v->step==-1))invoke_range(v,v->first,v->first);
                return;
            }
            hi=lo+n-1;
        }
        int a=(int)((U64)v->first+lo*(U64)v->step);
        int b=(int)((U64)v->first+hi*(U64)v->step);
        if(!v->inclusive)b=(int)((unsigned)b+(v->step>0?1U:UINT32_MAX));
        if(v->dynamic && hi==v->iterations-1)b=v->last;
        invoke_range(v,a,b);
    }while(v->dynamic);
}
API void C2VectParallel(int first,int last,int step,int inclusive,int threads,int dynamic,void *body,int nargs,...){
    if(nargs<2 || nargs>128 || !step)return;
    va_list args;va_start(args,nargs);
    int64_t span=(int64_t)last-first;
    VectorLoop loop={first,last,step,inclusive,dynamic,nargs,body,(Word*)args,0};
    if(span>INT32_MAX || span<INT32_MIN || span/step<2 || threads<0)invoke_range(&loop,first,last);
    else{
        U64 distance=span<0?-span:span;
        U64 stride=step<0?-(int64_t)step:step;
        loop.iterations=inclusive?1+distance/stride:1+(distance-1)/stride;
        Member *m=current();int saved=m->next_threads;m->next_threads=threads;
        Word arg=(Word)&loop;
        fork_region(1,1,vector_worker,&arg);m->next_threads=saved;
    }
    va_end(args);
}

/* 歸約的運算碼位於旗標的第 8～11 位。整數在目的寬度內回繞；
 * 邏輯 OR 保留既有的非零累積值，邏輯 AND 則正規化為 0 或 1。 */
static U64 reduce_integer(unsigned op,U64 old,U64 value){
    switch(op){
    case 0:case 1:return old+value;
    case 2:return old*value;
    case 3:return old&value;
    case 4:return old|value;
    case 5:return old^value;
    case 6:return old && value;
    default:return old?old:(value!=0);
    }
}
#define INTEGER_REDUCTION(width,T) \
API void _vcomp_reduction_i##width(unsigned flags,T *dest,T value){ \
    unsigned op=(flags>>8)&15; \
    T old=__atomic_load_n(dest,__ATOMIC_RELAXED),next; \
    do{next=(T)reduce_integer(op,old,value);} \
    while(!__atomic_compare_exchange_n(dest,&old,next,0,__ATOMIC_SEQ_CST,__ATOMIC_RELAXED)); \
} \
API void _vcomp_reduction_u##width(unsigned,T*,T) __attribute__((alias("_vcomp_reduction_i" #width)));
INTEGER_REDUCTION(1,uint8_t)
INTEGER_REDUCTION(2,uint16_t)
INTEGER_REDUCTION(4,uint32_t)
INTEGER_REDUCTION(8,uint64_t)

/* 原子比較採用物件的位元表示；遇到 NaN 也能確認同一筆觀測值。 */
#define REAL_REDUCTION(width,T) \
API void _vcomp_reduction_r##width(unsigned flags,T *dest,T value){ \
    unsigned op=(flags>>8)&15; \
    T old,next; \
    __atomic_load(dest,&old,__ATOMIC_RELAXED); \
    do{ \
        if(op<=1)next=old+value; \
        else if(op==2)next=old*value; \
        else if(op==6)next=old && value; \
        else next=old!=0?old:(T)(value!=0); \
    }while(!__atomic_compare_exchange(dest,&old,&next,0,__ATOMIC_SEQ_CST,__ATOMIC_RELAXED)); \
}
REAL_REDUCTION(4,float)
REAL_REDUCTION(8,double)

/* CAS 失敗時，使用同一次比較傳回的觀測值重新計算。浮點也以位元比較。 */
#define ATOMIC_UPDATE(op,kind,T,A,expr) \
API void _vcomp_atomic_##op##_##kind(T *dest,A value){ \
    T old,next; \
    __atomic_load(dest,&old,__ATOMIC_RELAXED); \
    do{next=(T)(expr);} \
    while(!__atomic_compare_exchange(dest,&old,&next,0,__ATOMIC_SEQ_CST,__ATOMIC_RELAXED)); \
}

/* Win32 的雙字移位在計數超出 63 時移出全部位元。 */
static inline int shift_out(unsigned bytes,unsigned count){
    return sizeof(void*)==4 && bytes==8 && (uint8_t)count>=64;
}
#define INTEGER_ATOMICS(n,S,U,W,mask) \
ATOMIC_UPDATE(add,i##n,U,U,(W)old+value) \
ATOMIC_UPDATE(sub,i##n,U,U,(W)old-value) \
ATOMIC_UPDATE(mul,i##n,U,U,(W)old*value) \
ATOMIC_UPDATE(and,i##n,U,U,old&value) \
ATOMIC_UPDATE(or,i##n,U,U,old|value) \
ATOMIC_UPDATE(xor,i##n,U,U,old^value) \
ATOMIC_UPDATE(div,i##n,S,S,old/value) \
ATOMIC_UPDATE(div,ui##n,U,U,old/value) \
ATOMIC_UPDATE(shl,i##n,U,unsigned,shift_out(n,value)?0:(W)old<<(value&mask)) \
ATOMIC_UPDATE(shr,i##n,S,unsigned,shift_out(n,value)?-(old<0):old>>(value&mask)) \
ATOMIC_UPDATE(shr,ui##n,U,unsigned,shift_out(n,value)?0:old>>(value&mask))

INTEGER_ATOMICS(1,int8_t,uint8_t,uint32_t,31)
INTEGER_ATOMICS(2,int16_t,uint16_t,uint32_t,31)
INTEGER_ATOMICS(4,int32_t,uint32_t,uint32_t,31)
INTEGER_ATOMICS(8,int64_t,uint64_t,uint64_t,63)

#define REAL_ATOMICS(kind,T) \
ATOMIC_UPDATE(add,kind,T,T,old+value) \
ATOMIC_UPDATE(sub,kind,T,T,old-value) \
ATOMIC_UPDATE(mul,kind,T,T,old*value) \
ATOMIC_UPDATE(div,kind,T,T,old/value)
REAL_ATOMICS(r4,float)
REAL_ATOMICS(r8,double)
