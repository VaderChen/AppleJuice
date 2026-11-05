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
    /* stateblock 記錄、多執行緒裝置與診斷保留共用宿主服務。 */
    if(*(Word*)(object+D9_CONSTANT_FLAGS)){
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
    for(unsigned i=0;i<length;++i)temporary[i]=write?values[i].value:state[i];
    __asm__ __volatile__("" ::: "memory");
    if(write){for(unsigned i=0;i<length;++i)state[i]=temporary[i];}
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
