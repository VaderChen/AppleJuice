/* 純介面查詢、原子引用與 agile 物件直接 Resolve，不離開 resident AOT。 */
#include "com_agile_layout.h"
typedef __UINT32_TYPE__ Word;
typedef __UINTPTR_TYPE__ Pointer;
#if defined(__i386__)
#define CALL __attribute__((stdcall))
#else
#define CALL
#endif
#define EXPORT __declspec(dllexport) Word CALL
typedef Word (CALL *One)(void*);
typedef Word (CALL *Three)(void*,const unsigned char*,void**);
static int guid_equal(const unsigned char* a,const unsigned char* b){
    unsigned different=0;for(unsigned i=0;i<16;++i)different|=a[i]^b[i];return different==0;
}
EXPORT AddRef(void* self){return __atomic_add_fetch((Word*)((unsigned char*)self+COM_AGILE_REFS),1,__ATOMIC_SEQ_CST);}
EXPORT Release(void* self){
    Word n=__atomic_sub_fetch((Word*)((unsigned char*)self+COM_AGILE_REFS),1,__ATOMIC_SEQ_CST);
    if(!n)((One)*(Pointer*)((unsigned char*)self+COM_AGILE_DESTROY))(self);
    return n;
}
EXPORT QueryInterface(void* self,const unsigned char* iid,void** output){
    if(!output)return 0x80004003U;
    if(!iid){*output=0;return 0x80070057U;}
    const int known=guid_equal(iid,com_agile_iunknown)||guid_equal(iid,com_agile_iagile)||guid_equal(iid,com_agile_ireference);
    *output=0;if(!known)return 0x80004002U;AddRef(self);*output=self;return 0;
}
EXPORT Resolve(void* self,const unsigned char* iid,void** output){
    if(!output)return 0x80004003U;
    if(!iid){*output=0;return 0x80070057U;}
    unsigned char requested[16];for(unsigned i=0;i<16;++i)requested[i]=iid[i];*output=0;
    if(*(Word*)((unsigned char*)self+COM_AGILE_FLAGS)&1){
        void* target=(void*)*(Pointer*)((unsigned char*)self+COM_AGILE_TARGET);
        return ((Three)(*(Pointer**)target)[0])(target,requested,output);
    }
    return ((Three)*(Pointer*)((unsigned char*)self+COM_AGILE_RESOLVE))(self,requested,output);
}
