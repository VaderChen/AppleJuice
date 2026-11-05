/* IMFMediaBuffer 的計數、長度與純記憶體鎖定由 resident AOT 執行。 */
#include "mf_buffer_layout.h"
typedef __UINT32_TYPE__ Word;
typedef __UINTPTR_TYPE__ Pointer;
#if defined(__i386__)
#define CALL __attribute__((stdcall))
#else
#define CALL
#endif
#define EXPORT __declspec(dllexport) Word CALL
#define CELL(self,offset) ((Word*)((unsigned char*)(self)+(offset)))
#define PTR(self,offset) (*(Pointer*)((unsigned char*)(self)+(offset)))
typedef Word (CALL *One)(void*);
typedef Word (CALL *Lock)(void*,void**,Word*,Word*);
typedef Word (CALL *Query)(void*,const unsigned char*,void**);
static int equal(const unsigned char* a,const unsigned char* b){
    unsigned n=0;for(unsigned i=0;i<16;++i)n|=a[i]^b[i];return !n;
}
EXPORT BufferAddRef(void* self){return __atomic_add_fetch(CELL(self,MF_BUFFER_REFS),1,__ATOMIC_SEQ_CST);}
EXPORT BufferRelease(void* self){
    Word refs=__atomic_sub_fetch(CELL(self,MF_BUFFER_REFS),1,__ATOMIC_SEQ_CST);
    if(!refs)((One)PTR(self,MF_BUFFER_DESTROY))(self);return refs;
}
EXPORT BufferQueryInterface(void* self,const unsigned char* iid,void** output){
    if(!*CELL(self,MF_BUFFER_PLAIN))return ((Query)PTR(self,MF_BUFFER_QUERY_HOST))(self,iid,output);
    if(!output)return 0x80004003U;
    if(!iid)return 0x80004003U;
    const int known=equal(iid,mf_buffer_iunknown)||equal(iid,mf_buffer_interface);
    *output=0;if(!known)return 0x80004002U;
    BufferAddRef(self);*output=self;return 0;
}
EXPORT B_Lock(void* self,void** data,Word* maximum,Word* length){
    if(!*CELL(self,MF_BUFFER_PLAIN))return ((Lock)PTR(self,MF_BUFFER_LOCK_HOST))(self,data,maximum,length);
    if(!data)return 0x80004003U;
    /* 寫入探測先於計數變更，保留錯誤指標造成存取例外時的狀態。 */
    void* old=*(void* volatile*)data;*(void* volatile*)data=old;
    if(maximum){Word old_max=*(volatile Word*)maximum;*(volatile Word*)maximum=old_max;}
    if(length){Word old_length=*(volatile Word*)length;*(volatile Word*)length=old_length;}
    Word old_locks=__atomic_load_n(CELL(self,MF_BUFFER_LOCKS),__ATOMIC_SEQ_CST);
    do {if(old_locks==0xffffffffU)return 0xc00d36b2U;}
    while(!__atomic_compare_exchange_n(CELL(self,MF_BUFFER_LOCKS),&old_locks,old_locks+1,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST));
    *data=(void*)PTR(self,MF_BUFFER_DATA);
    if(maximum)*maximum=*CELL(self,MF_BUFFER_MAXIMUM);
    if(length)*length=__atomic_load_n(CELL(self,MF_BUFFER_LENGTH),__ATOMIC_SEQ_CST);
    return 0;
}
EXPORT B_Unlock(void* self){
    if(!*CELL(self,MF_BUFFER_PLAIN))return ((One)PTR(self,MF_BUFFER_UNLOCK_HOST))(self);
    Word locks=__atomic_load_n(CELL(self,MF_BUFFER_LOCKS),__ATOMIC_SEQ_CST);
    do {if(!locks)return 0x80070057U;}
    while(!__atomic_compare_exchange_n(CELL(self,MF_BUFFER_LOCKS),&locks,locks-1,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST));
    return 0;
}
EXPORT B_GetCurrentLength(void* self,Word* result){
    if(!result)return 0x80004003U;*result=__atomic_load_n(CELL(self,MF_BUFFER_LENGTH),__ATOMIC_SEQ_CST);return 0;
}
EXPORT B_GetMaxLength(void* self,Word* result){
    if(!result)return 0x80004003U;*result=*CELL(self,MF_BUFFER_MAXIMUM);return 0;
}
EXPORT B_SetCurrentLength(void* self,Word length){
    if(length>*CELL(self,MF_BUFFER_MAXIMUM))return 0x80070057U;
    __atomic_store_n(CELL(self,MF_BUFFER_LENGTH),length,__ATOMIC_SEQ_CST);return 0;
}
