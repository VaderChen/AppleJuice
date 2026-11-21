/* D3D9 的 F/I/B 兩階段常數由 resident AOT 操作同一份客體狀態。 */
#include "d3d9_constant_layout.h"
typedef __UINT32_TYPE__ Word;
typedef __UINTPTR_TYPE__ Pointer;
struct __attribute__((packed,may_alias)) UnalignedWord {Word value;};
#if defined(__i386__)
#define CALL __attribute__((stdcall))
#else
#define CALL
#endif
#define EXPORT __declspec(dllexport) Word CALL
typedef Word (CALL *Host)(void*,Word,void*,Word);
static Word constants(void* self,Word first,void* data,Word count,unsigned stage,unsigned kind,unsigned write){
    unsigned char* object=self;
    /* stateblock 記錄與診斷保留共用宿主服務。多執行緒裝置（位元 2）留在客體：寫入者以鎖互斥並遞增序號，宿主讀取依序號取一致快照；
       原本每次都回宿主服務（FINAL FANTASY XIII 戰鬥每幀約 1.2 萬次）。 */
    const Word flags=*(Word*)(object+D9_CONSTANT_FLAGS);
    if(flags&~4U){
        const unsigned method=stage*6+kind*2+(write?0:1);
        return ((Host)*(Pointer*)(object+D9_CONSTANT_HOST+method*8))(self,first,data,count);
    }
    const unsigned limit=d9_constant_limit(stage,kind),words=d9_constant_stride(kind)/4;
    if(first>limit || count>limit-first)return 0x8876086cU;
    if(!count)return 0;
    Word temporary[1024];
    Word* state=(Word*)(object+D9_CONSTANT_DATA+d9_constant_offset(stage,kind))+first*words;
    struct UnalignedWord* values=data;
    const unsigned length=count*words;
    /* 先讀完來源，存取例外不會留下部分更新的裝置狀態。 */
    Word* lock=(Word*)(object+D9_CONSTANT_LOCK);
    Word* sequence=(Word*)(object+D9_CONSTANT_SEQUENCE);
    if(write){for(unsigned i=0;i<length;++i)temporary[i]=values[i].value;}
    if(flags){while(__atomic_exchange_n(lock,1U,__ATOMIC_ACQUIRE))__asm__ __volatile__("pause");}
    if(!write){for(unsigned i=0;i<length;++i)temporary[i]=state[i];}
    __asm__ __volatile__("" ::: "memory");
    if(write){
        if(flags)__atomic_fetch_add(sequence,1U,__ATOMIC_RELEASE);
        for(unsigned i=0;i<length;++i)state[i]=temporary[i];
        if(flags)__atomic_fetch_add(sequence,1U,__ATOMIC_RELEASE);
    }
    if(flags)__atomic_store_n(lock,0U,__ATOMIC_RELEASE);
    if(write){}
    else{
        /* 先探測整份輸出；跨頁存取例外保留呼叫端原內容。 */
        volatile struct UnalignedWord* probe=data;
        for(unsigned i=0;i<length;++i){Word old=probe[i].value;probe[i].value=old;}
        for(unsigned i=0;i<length;++i)values[i].value=temporary[i];
    }
    return 0;
}
#define METHODS(prefix,stage,suffix,kind) \
EXPORT Set##prefix##ShaderConstant##suffix(void* self,Word first,void* data,Word count){return constants(self,first,data,count,stage,kind,1);} \
EXPORT Get##prefix##ShaderConstant##suffix(void* self,Word first,void* data,Word count){return constants(self,first,data,count,stage,kind,0);}
METHODS(Vertex,0,F,0)
METHODS(Vertex,0,I,1)
METHODS(Vertex,0,B,2)
METHODS(Pixel,1,F,0)
METHODS(Pixel,1,I,1)
METHODS(Pixel,1,B,2)
