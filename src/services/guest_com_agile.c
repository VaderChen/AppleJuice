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

/* SHLWAPI 序號 23／24；先在區域緩衝區格式化，容許來源與目的重疊。 */
static void guid_text(const unsigned char *guid,char output[39]){
    static const unsigned char order[]={3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    static const char hex[]="0123456789ABCDEF";
    unsigned pos=0;
    output[pos++]='{';
    for(unsigned i=0;i<16;++i){
        if(i==4 || i==6 || i==8 || i==10)output[pos++]='-';
        unsigned byte=guid[order[i]];
        output[pos++]=hex[byte>>4];output[pos++]=hex[byte&15];
    }
    output[pos++]='}';output[pos]=0;
}
EXPORT SHStringFromGUIDA(const unsigned char *guid,char *dest,int capacity){
    char text[39];guid_text(guid,text);
    if(capacity<39)return 0;
    for(unsigned i=0;i<39;++i)dest[i]=text[i];
    return 39;
}
EXPORT SHStringFromGUIDW(const unsigned char *guid,unsigned short *dest,int capacity){
    char text[39];guid_text(guid,text);
    if(capacity<39)return 0;
    for(unsigned i=0;i<39;++i)dest[i]=(unsigned short)text[i];
    return 39;
}

/* SHLWAPI 的 C 定序只折疊 ASCII 大寫。遇到首個差異、NUL 或指定長度即停止讀取。 */
static inline int canonical_compare(const void *left,const void *right,Pointer count,int wide,int fold){
    if(!left || !right)return left==right?0:left?1:-1;
    for(Pointer i=0;i<count;++i){
        unsigned a=wide?((const unsigned short*)left)[i]:((const unsigned char*)left)[i];
        unsigned b=wide?((const unsigned short*)right)[i]:((const unsigned char*)right)[i];
        if(fold){
            if(a>='A' && a<='Z')a+=32;
            if(b>='A' && b<='Z')b+=32;
        }
        if(a!=b)return a<b?-1:1;
        if(!a)break;
    }
    return 0;
}
#define FULL_COMPARE(name,type,fold) \
    __declspec(dllexport) int CALL name(const type* a,const type* b){ \
        return canonical_compare(a,b,(Pointer)-1,sizeof(type)==2,fold); \
    }
#define PREFIX_COMPARE(name,type,fold) \
    __declspec(dllexport) int CALL name(const type* a,const type* b,int n){ \
        return canonical_compare(a,b,n>0?(Pointer)n:0,sizeof(type)==2,fold); \
    }
FULL_COMPARE(StrCmpCA,unsigned char,0)
FULL_COMPARE(StrCmpCW,unsigned short,0)
FULL_COMPARE(StrCmpICA,unsigned char,1)
FULL_COMPARE(StrCmpICW,unsigned short,1)
PREFIX_COMPARE(StrCmpNCA,unsigned char,0)
PREFIX_COMPARE(StrCmpNCW,unsigned short,0)
PREFIX_COMPARE(StrCmpNICA,unsigned char,1)
PREFIX_COMPARE(StrCmpNICW,unsigned short,1)
