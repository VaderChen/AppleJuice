#include "shell_window_interfaces.h"
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

#include "windows_path_character.h"

EXPORT PathIsValidCharA(unsigned char c,Word flags){return flags & path_character_class(c);}
EXPORT PathIsValidCharW(unsigned short c,Word flags){return flags & path_character_class(c);}
EXPORT PathGetCharTypeA(unsigned char c){return path_character_type(c);}
EXPORT PathGetCharTypeW(unsigned short c){return path_character_type(c);}

/* UTF-8 與 UTF-16 的 '&' 都是單一碼元，只處理第一次出現的位置。 */
static Word strip_mnemonic(void *buffer, unsigned wide){
#define UNIT(i) (wide ? ((unsigned short *)buffer)[i] : ((unsigned char *)buffer)[i])
    Pointer marker = 0;
    Word result = UNIT(0);
    while(UNIT(marker) && UNIT(marker) != '&') ++marker;
    if(UNIT(marker) && UNIT(marker + 1)){
        Word next = UNIT(marker + 1);
        if(next != '&') result = next;
        Pointer end = marker + 1;
        while(UNIT(end)) ++end;
        for(; marker < end; ++marker){
            if(wide) ((unsigned short *)buffer)[marker] = ((unsigned short *)buffer)[marker + 1];
            else ((unsigned char *)buffer)[marker] = ((unsigned char *)buffer)[marker + 1];
        }
    }
#undef UNIT
    return result;
}
EXPORT SHStripMneumonicA(unsigned char *text){return strip_mnemonic(text, 0);}
EXPORT SHStripMneumonicW(unsigned short *text){return strip_mnemonic(text, 1);}

/* SHLWAPI #198 只讀取第一筆符合鍵的對應值。 */
EXPORT SHSearchMapInt(const Word *keys,const Word *values,int count,Word key){
    if(keys && values){
        for(int i=0;i<count;++i){
            if(keys[i]==key)return values[i];
        }
    }
    return (Word)-1;
}

/* Release 回呼可讀取舊槽位，AddRef 回呼則在清空與發布新值之間執行。 */
static void release_slot(void **slot){
    void *previous = slot ? *slot : 0;
    if(previous){
        ((One)(*(Pointer **)previous)[2])(previous);
        *slot = 0;
    }
}
__declspec(dllexport) void CALL IUnknown_AtomicRelease(void **slot){
    release_slot(slot);
}
__declspec(dllexport) void CALL IUnknown_Set(void **slot, void *replacement){
    release_slot(slot);
    if(replacement){
        ((One)(*(Pointer **)replacement)[1])(replacement);
        *slot = replacement;
    }
}

EXPORT SHIsSameObject(void *left, void *right){
    if(!left || !right)return 0;
    if(left==right)return 1;
    void *first, *second;
    if(((Three)(*(Pointer **)left)[0])(left,com_agile_iunknown,&first))return 0;
    Word status=((Three)(*(Pointer **)right)[0])(right,com_agile_iunknown,&second);
    Word same=!status && first==second;
    ((One)(*(Pointer **)first)[2])(first);
    if(!status)((One)(*(Pointer **)second)[2])(second);
    return same;
}

typedef Word (CALL *ShellMethod)(void *,void *);
static Word shell_interface_method(void *object,void *argument,const struct ShellInterfaceQuery *query){
    if(!object)return 0x80004005U;
    Word hr=0x80004005U;
    for(unsigned i=0;i<query->count;++i){
        void *target;
        const unsigned char *iid=query->iids[i];
        hr=((Three)(*(Pointer **)object)[0])(object,iid,&target);
        if(query->require_s_ok?hr!=0:(hr&0x80000000U)!=0)continue;
        hr=((ShellMethod)(*(Pointer **)target)[3])(target,argument);
        ((One)(*(Pointer **)target)[2])(target);
        break;
    }
    return hr;
}
EXPORT IUnknown_GetWindow(void *object,void *output){return shell_interface_method(object,output,&window_query);}
EXPORT IUnknown_SetOwner(void *object,void *owner){return shell_interface_method(object,owner,&owner_query);}
EXPORT IUnknown_GetClassID(void *object,unsigned char *clsid){
    if(!object){
        for(unsigned i=0;i<16;++i)clsid[i]=0;
        return 0x80004005U;
    }
    return shell_interface_method(object,clsid,&class_id_query);
}

__declspec(dllexport) const char *CALL PathSkipLeadingSlashesA(const char *path){
    return path && path[0]=='/' && path[1]=='/' ? path+2 : path;
}
__declspec(dllexport) void CALL FixSlashesAndColonA(char *path){
    for(;*path;++path)if(*path=='/')*path='\\';
}
__declspec(dllexport) void CALL FixSlashesAndColonW(unsigned short *path){
    for(;*path;++path)if(*path=='/')*path='\\';
}

typedef Word (CALL *AdviseMethod)(void *,void *,Word *);
typedef Word (CALL *UnadviseMethod)(void *,Word);
EXPORT ConnectToConnectionPoint(void *sink,const unsigned char *iid,Word connect,void *source,Word *cookie,void **output){
    static const unsigned char container_iid[16]={0x84,0xb2,0x96,0xb1,0xb4,0xba,0x1a,0x10,0xb6,0x9c,0,0xaa,0,0x34,0x1d,7};
    if(!source || (connect && !sink))return 0x80004005U;
    if(output)*output=0;
    void *container, *point;
    Word hr=((Three)(*(Pointer **)source)[0])(source,container_iid,&container);
    if(hr&0x80000000U)return hr;
    hr=((Three)(*(Pointer **)container)[4])(container,iid,&point);
    if(!(hr&0x80000000U)){
        if(connect)hr=((AdviseMethod)(*(Pointer **)point)[5])(point,sink,cookie);
        else hr=((UnadviseMethod)(*(Pointer **)point)[6])(point,*cookie);
        if(hr&0x80000000U)*cookie=0;
        if(output && !(hr&0x80000000U))*output=point;
        else ((One)(*(Pointer **)point)[2])(point);
    }
    ((One)(*(Pointer **)container)[2])(container);
    return hr;
}

struct ShellFDSA {
    Word count;
    unsigned char *data;
    Word capacity;
    unsigned char increment, width, flags;
};
EXPORT FDSA_Initialize(Word width,Word increment,struct ShellFDSA *info,unsigned char *data,Word capacity){
    if(data)for(Word i=0;i<width*capacity;++i)data[i]=0;
    info->count=0;
    info->data=data;
    info->capacity=capacity;
    info->increment=(unsigned char)(increment?increment:1);
    info->width=(unsigned char)width;
    info->flags=0;
    return 1;
}
EXPORT FDSA_DeleteItem(struct ShellFDSA *info,Word index){
    if(index>=info->count)return 0;
    Word width=info->width;
    Pointer tail=(Pointer)(info->count-index-1)*width;
    unsigned char *dest=info->data+(Pointer)index*width;
    for(Pointer i=0;i<tail;++i)dest[i]=dest[i+width];
    for(Word i=0;i<width;++i)dest[tail+i]=0;
    --info->count;
    return 1;
}

#include "shell_dispatch_layout.h"
typedef struct {
    unsigned short type, reserved[3];
    union { void *pointers[2]; Word integer; unsigned short boolean; } value;
} ShellVariant;
typedef struct {
    ShellVariant *args;
    void *names;
    Word count, named_count;
} ShellDispParams;

EXPORT SHPackDispParamsV(ShellDispParams *params, ShellVariant *args, Word count, __builtin_va_list values)
{
    params->args = args;
    params->names = 0;
    params->count = count;
    params->named_count = 0;
    while (count) {
        ShellVariant *arg = &args[--count];
        arg->type = shell_dispatch_type((unsigned short)__builtin_va_arg(values, int));
        if (arg->type == 11)
            arg->value.boolean = (unsigned short)__builtin_va_arg(values, int);
        else if (arg->type == 3)
            arg->value.integer = __builtin_va_arg(values, Word);
        else
            arg->value.pointers[0] = __builtin_va_arg(values, void *);
    }
    return 0;
}

__declspec(dllexport) Word __cdecl SHPackDispParams(ShellDispParams *params, ShellVariant *args, Word count, ...)
{
    __builtin_va_list values;
    __builtin_va_start(values, count);
    Word result = SHPackDispParamsV(params, args, count, values);
    __builtin_va_end(values);
    return result;
}
