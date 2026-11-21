#include "shell_window_interfaces.h"
#include "mapi_property_tag.h"
#include "mapi_scalar_compare.h"
#include "mapi_hex.h"
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
/* 此指標槽由程序內所有 Shell 入口以及 SHCreateThread 共用。 */
__declspec(dllexport) void *ShellProcessReferenceSlot;
__declspec(dllexport) void CALL SHSetInstanceExplorer(void *object){
    __atomic_store_n(&ShellProcessReferenceSlot,object,__ATOMIC_RELEASE);
}
EXPORT SHGetInstanceExplorer(void **output){
    if(!output)return 0x80070057U;
    void *object=__atomic_load_n(&ShellProcessReferenceSlot,__ATOMIC_ACQUIRE);
    *output=object;
    if(!object)return 0x80004005U;
    ((One)(*(Pointer**)object)[1])(object);
    return 0;
}
static int guid_equal(const unsigned char* a,const unsigned char* b){
    unsigned different=0;for(unsigned i=0;i<16;++i)different|=a[i]^b[i];return different==0;
}
EXPORT AddRef(void* self){return __atomic_add_fetch((Word*)((unsigned char*)self+COM_AGILE_REFS),1,__ATOMIC_SEQ_CST);}
EXPORT Release(void* self){
    Word n=__atomic_sub_fetch((Word*)((unsigned char*)self+COM_AGILE_REFS),1,__ATOMIC_SEQ_CST);
    if(!n)((One)*(Pointer*)((unsigned char*)self+COM_AGILE_DESTROY))(self);
    return n;
}
#include "shell_defclass_layout.h"
EXPORT ShellFactoryQuery(void *self,const unsigned char *iid,void **out){
    if(!out)return 0x80004003U;
    *out=0;
    if(!iid)return 0x80070057U;
    if(!guid_equal(iid,com_agile_iunknown) && !guid_equal(iid,shell_factory_iid))return 0x80004002U;
    AddRef(self);*out=self;return 0;
}
EXPORT ShellFactoryCreate(unsigned char *self,void *outer,const unsigned char *iid,void **out){
    if(!out)return 0x80004003U;
    *out=0;
    if(!iid)return 0x80070057U;
    const unsigned char *filter=*(const unsigned char**)(self+SHELL_FACTORY_FILTER);
    if(filter && !guid_equal(iid,com_agile_iunknown) && !guid_equal(iid,filter))return 0x80004002U;
    return (*(Three*)(self+SHELL_FACTORY_CALLBACK))(outer,iid,out);
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
static const unsigned char guid_order[]={3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
static void guid_text(const unsigned char *guid,char output[39]){
    static const char hex[]="0123456789ABCDEF";
    unsigned pos=0;
    output[pos++]='{';
    for(unsigned i=0;i<16;++i){
        if(i==4 || i==6 || i==8 || i==10)output[pos++]='-';
        unsigned byte=guid[guid_order[i]];
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

/* ProgID 需要查詢登錄服務，映射時將對應的宿主入口填入唯讀表。 */
typedef Word (CALL *GuidParser)(const void*,unsigned char*);
__declspec(dllexport) const volatile Pointer GuestServiceFallbacks[23]={0};
/* ASCII 碼元可直接轉換；非 ASCII 使用與宿主服務相同的 Unicode 與 ACP 規則。 */
static unsigned ascii_case(unsigned unit,int upper){
    unsigned first=upper?'a':'A';
    return unit-first<26 ? unit^32 : unit;
}
typedef Pointer (CALL *CaseString)(Pointer);
typedef Word (CALL *CaseBuffer)(Pointer,Word);
static Pointer case_string(Pointer value,int upper,int wide){
    if(value<=65535){
        unsigned unit=wide?(unsigned)value:(unsigned char)value;
        if(unit<128)return ascii_case(unit,upper);
    }
    return ((CaseString)GuestServiceFallbacks[(wide?7:9)+upper])(value);
}
static Word case_buffer(Pointer address,Word count,int upper,int wide){
    if(!address || !count)return 0;
    unsigned short *words=(unsigned short *)address;
    unsigned char *bytes=(unsigned char *)address;
    /* 先判斷整段編碼，再寫入，保留多位元組轉換失敗時的原始資料。 */
    if(count<=256 && count*(wide?2U:1U)<=4096U-(address&4095U)){
        Word at=0;
        while(at<count && (wide?words[at]:bytes[at])<128)++at;
        if(at==count){
            for(at=0;at<count;++at){
                if(wide)words[at]=(unsigned short)ascii_case(words[at],upper);
                else bytes[at]=(unsigned char)ascii_case(bytes[at],upper);
            }
            return count;
        }
    }
    return ((CaseBuffer)GuestServiceFallbacks[(wide?11:13)+upper])(address,count);
}
#define CASE_EXPORTS(direction,upper,suffix,wide) \
    __declspec(dllexport) Pointer CALL Char##direction##suffix(Pointer v){return case_string(v,upper,wide);} \
    EXPORT Char##direction##Buff##suffix(Pointer p,Word n){return case_buffer(p,n,upper,wide);}
CASE_EXPORTS(Lower,0,W,1)
CASE_EXPORTS(Upper,1,W,1)
CASE_EXPORTS(Lower,0,A,0)
CASE_EXPORTS(Upper,1,A,0)
#undef CASE_EXPORTS

/* ACP／OEMCP 共用 UTF-8。單頁 ASCII 字串可直接複製，重疊或轉碼時沿用宿主快照。 */
static Word oem_convert(Pointer src,Pointer dst,Word count,int from_oem,int wide,int counted){
    unsigned src_unit=wide && !from_oem?2:1, dst_unit=wide && from_oem?2:1;
    Word limit=(Word)((4096-(src&4095))/src_unit);
    if(src && dst && src!=dst && (!counted || count<=limit)){
        Word n=0;
        int terminated=0;
        if(counted)limit=count;
        while(n<limit){
            unsigned c=src_unit==2?((const unsigned short *)src)[n]:((const unsigned char *)src)[n];
            if(c>=128)break;
            ++n;
            if(!counted && !c){terminated=1;break;}
        }
        if((terminated || (counted && n==count)) && n<=(4096-(dst&4095))/dst_unit &&
           (src<dst?dst-src>=n*src_unit:src-dst>=n*dst_unit)){
            for(Word i=0;i<n;++i){
                unsigned c=src_unit==2?((const unsigned short *)src)[i]:((const unsigned char *)src)[i];
                if(dst_unit==2)((unsigned short *)dst)[i]=(unsigned short)c;
                else ((unsigned char *)dst)[i]=(unsigned char)c;
            }
            return 1;
        }
    }
    unsigned slot=15+2*(unsigned)from_oem+(unsigned)wide;
    typedef Word (CALL *Terminated)(Pointer,Pointer);
    typedef Word (CALL *Counted)(Pointer,Pointer,Word);
    if(counted)return ((Counted)GuestServiceFallbacks[slot+4])(src,dst,count);
    return ((Terminated)GuestServiceFallbacks[slot])(src,dst);
}
#define OEM_EXPORTS(op,dir,suffix,wide) \
    EXPORT op##suffix(Pointer s,Pointer d){return oem_convert(s,d,0,dir,wide,0);} \
    EXPORT op##Buff##suffix(Pointer s,Pointer d,Word n){return oem_convert(s,d,n,dir,wide,1);}
OEM_EXPORTS(CharToOem,0,A,0)
OEM_EXPORTS(CharToOem,0,W,1)
OEM_EXPORTS(OemToChar,1,A,0)
OEM_EXPORTS(OemToChar,1,W,1)
#undef OEM_EXPORTS

/* ACP 與 OEM 在客體預設為 UTF-8；其他碼頁共用 NLS 的 lead-byte 表。 */
static int walk_lead(unsigned short page,unsigned char byte){
    if(page==0 || page==1 || page==3 || page==65001 || page==1252)return 0;
    typedef int (CALL *Lead)(Word,Word);
    return ((Lead)GuestServiceFallbacks[6])(page,byte);
}
#define WALK_EXPORT __declspec(dllexport) Pointer CALL
WALK_EXPORT CharNextA(const unsigned char *p){return (Pointer)p+(*p!=0);}
WALK_EXPORT CharNextW(const unsigned short *p){return (Pointer)p+(*p?2:0);}
WALK_EXPORT CharPrevA(Pointer start,Pointer p){return p-(p>start);}
WALK_EXPORT CharPrevW(Pointer start,Pointer p){return p-(p>start?2:0);}
WALK_EXPORT CharNextExA(unsigned short page,const unsigned char *p,Word flags){
    (void)flags;
    if(!*p)return (Pointer)p;
    return (Pointer)p+(walk_lead(page,*p) && p[1]?2:1);
}
WALK_EXPORT CharPrevExA(unsigned short page,Pointer start,Pointer p,Word flags){
    (void)flags;
    if(p<=start)return p;
    return p-(p-start>=2 && walk_lead(page,*(const unsigned char *)(p-2))?2:1);
}
WALK_EXPORT CharNextExW(unsigned short page,Pointer p,Word flags){
    (void)page;(void)p;(void)flags;return 0;
}
WALK_EXPORT CharPrevExW(unsigned short page,Pointer start,Pointer p,Word flags){
    (void)page;(void)start;(void)p;(void)flags;return 0;
}

static unsigned guid_unit(const void *text,unsigned pos,int wide){
    return wide?((const unsigned short*)text)[pos]:((const unsigned char*)text)[pos];
}
static int guid_digit(unsigned ch){
    if(ch>='0' && ch<='9')return ch-'0';
    if(ch>='a' && ch<='f')return ch-'a'+10;
    if(ch>='A' && ch<='F')return ch-'A'+10;
    return -1;
}
static Word parse_guid(const void *text,unsigned char *out,unsigned kind){
    const int wide=kind!=1;
    if(!out)return kind?0:0x80070057U;
    unsigned char value[16]={0};
    if(text){
        if(guid_unit(text,0,wide)!='{')return ((GuidParser)GuestServiceFallbacks[kind])(text,out);
        unsigned pos=1;
        for(unsigned i=0;i<16;++i){
            if((i==4 || i==6 || i==8 || i==10) && guid_unit(text,pos++,wide)!='-')return kind?0:0x800401f3U;
            int hi=guid_digit(guid_unit(text,pos++,wide));
            if(hi<0)return kind?0:0x800401f3U;
            int lo=guid_digit(guid_unit(text,pos++,wide));
            if(lo<0)return kind?0:0x800401f3U;
            value[guid_order[i]]=(unsigned char)((hi<<4)|lo);
        }
        if(guid_unit(text,pos,wide)!='}' || guid_unit(text,pos+1,wide))return kind?0:0x800401f3U;
    }
    for(unsigned i=0;i<16;++i)out[i]=value[i];
    return kind?1:0;
}
EXPORT CLSIDFromString(const unsigned short *text,unsigned char *out){return parse_guid(text,out,0);}
EXPORT GUIDFromStringA(const char *text,unsigned char *out){return parse_guid(text,out,1);}
EXPORT GUIDFromStringW(const unsigned short *text,unsigned char *out){return parse_guid(text,out,2);}

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
typedef struct {
    const unsigned char *guid;
    Word kind;
    union { Word id; const unsigned short *name; } value;
} MapiNameId;

EXPORT FEqualNames(const MapiNameId *left,const MapiNameId *right){
    if(!left || !right || !guid_equal(left->guid,right->guid) || left->kind!=right->kind)return 0;
    if(left->kind!=1)return left->value.id==right->value.id;
    if(!left->value.name || !right->value.name)return left->value.name==right->value.name;
    if(!canonical_compare(left->value.name,right->value.name,(Pointer)-1,1,0))return 1;
    /* 字碼不同仍可能定序等價，由宿主依客體執行緒的地區設定判斷。 */
    typedef Word (CALL *Compare)(const MapiNameId*,const MapiNameId*);
    return ((Compare)GuestServiceFallbacks[3])(left,right);
}
static uint8_t mapi_read_byte(void *memory,uint64_t at){(void)memory;return *(const uint8_t*)(Pointer)at;}
static void mapi_write_byte(void *memory,uint64_t at,uint8_t byte){(void)memory;*(uint8_t*)(Pointer)at=byte;}
EXPORT HexFromBin(Pointer src,Word count,Pointer dst){
    return mapi_hex_convert(MapiHexEncode,src,dst,count,0,mapi_read_byte,mapi_write_byte);
}
EXPORT FBinFromHex(Pointer src,Pointer dst){
    return mapi_hex_convert(MapiHexDecode,src,dst,0,0,mapi_read_byte,mapi_write_byte);
}
EXPORT ScBinFromHexBounded(Pointer src,Pointer dst,Word count){
    return mapi_hex_convert(MapiHexBounded,src,dst,count,0,mapi_read_byte,mapi_write_byte);
}
__declspec(dllexport) uint64_t CALL FtAddFt(uint64_t left,uint64_t right){return left+right;}
__declspec(dllexport) uint64_t CALL FtSubFt(uint64_t minuend,uint64_t subtrahend){return minuend-subtrahend;}
__declspec(dllexport) uint64_t CALL FtNegFt(uint64_t value){return 0-value;}
__declspec(dllexport) uint64_t CALL FtMulDw(Word factor,uint64_t value){return factor*value;}
__declspec(dllexport) uint64_t CALL FtMulDwDw(Word left,Word right){return (uint64_t)left*right;}
EXPORT UFromSz(Pointer src){return mapi_unsigned_decimal(src,0,mapi_read_byte);}
EXPORT UlFromSzHex(Pointer src){
    return mapi_hex_convert(MapiHexNumber,src,0,0,0,mapi_read_byte,mapi_write_byte);
}
EXPORT LPropCompareProp(const unsigned char *left,const unsigned char *right){
    unsigned type=*(const uint16_t*)left,other=*(const uint16_t*)right;
    if(type!=other)return type-other;
    if(type==30 || type==31 || type==72 || type==258){
        typedef Word (CALL *Order)(const unsigned char*,const unsigned char*);
        return ((Order)GuestServiceFallbacks[5])(left,right);
    }
    uint64_t a,b;
    __builtin_memcpy(&a,left+8,8); __builtin_memcpy(&b,right+8,8);
    return mapi_scalar_order(type,a,b);
}
EXPORT FBadPropTag(Word tag){return !mapi_valid_property_tag(tag);}
static inline void *mapi_find_property(unsigned char *props,Word count,Word tag,int id_only){
    if(!props)return 0;
    for(Word i=0;i<count;++i,props+=mapi_property_stride(sizeof(Pointer))){
        if(mapi_property_matches(*(const Word*)props,tag,id_only))return props;
    }
    return 0;
}
__declspec(dllexport) void *CALL PpropFindProp(void *props,Word count,Word tag){
    return mapi_find_property(props,count,tag,0);
}
__declspec(dllexport) void *CALL LpValFindProp(Word tag,Word count,void *props){
    return mapi_find_property(props,count,tag,1);
}
/* MAPI 的長度以元素為單位，原地變更每個元素的位元組順序。 */
__declspec(dllexport) void CALL SwapPlong(Word *values,Word count){
    for(Word i=0;i<count;++i)values[i]=__builtin_bswap32(values[i]);
}
__declspec(dllexport) void CALL SwapPword(unsigned short *values,Word count){
    for(Word i=0;i<count;++i)values[i]=__builtin_bswap16(values[i]);
}
/* 參考實作 的 #64 回傳含終止字元的位元組數；#65 的修飾名稱為 @24，但入口接受三個引數。 */
EXPORT MapiCopyW(unsigned short *dest,const unsigned short *src){
    Word bytes=0;
    do { bytes+=2; } while((*dest++=*src++)!=0);
    return bytes;
}
EXPORT MapiCompareW(Word codepage,const unsigned short *left,const unsigned short *right){
    static const unsigned short empty=0;
    (void)codepage;
    return 2+canonical_compare(left?left:&empty,right?right:&empty,(Pointer)-1,1,0);
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

#include "shell_quote_text.h"
static unsigned read_quote_unit(void *context, ShellQuoteAddress address, unsigned unit){
    (void)context;
    return unit==1 ? *(const unsigned char*)(Pointer)address : *(const unsigned short*)(Pointer)address;
}
static void write_quote_unit(void *context, ShellQuoteAddress address, unsigned unit, unsigned value){
    (void)context;
    if(unit==1) *(unsigned char*)(Pointer)address=(unsigned char)value;
    else *(unsigned short*)(Pointer)address=(unsigned short)value;
}
#define SHELL_QUOTE_ENTRY(name,type,action) \
    __declspec(dllexport) Pointer CALL name(type *text){ \
        return (Pointer)shell_quote_text(0,(Pointer)text,sizeof(type),action,read_quote_unit,write_quote_unit); \
    }
SHELL_QUOTE_ENTRY(SheRemoveQuotesA,unsigned char,ShellExtractQuoted)
SHELL_QUOTE_ENTRY(SheRemoveQuotesW,unsigned short,ShellExtractQuoted)
SHELL_QUOTE_ENTRY(PathQuoteSpacesA,unsigned char,ShellQuoteSpaces)
SHELL_QUOTE_ENTRY(PathQuoteSpacesW,unsigned short,ShellQuoteSpaces)
SHELL_QUOTE_ENTRY(PathUnquoteSpacesA,unsigned char,ShellUnquoteSpaces)
SHELL_QUOTE_ENTRY(PathUnquoteSpacesW,unsigned short,ShellUnquoteSpaces)
SHELL_QUOTE_ENTRY(PathGetArgsA,unsigned char,ShellFindArguments)
SHELL_QUOTE_ENTRY(PathGetArgsW,unsigned short,ShellFindArguments)
SHELL_QUOTE_ENTRY(PathRemoveArgsA,unsigned char,ShellRemoveArguments)
SHELL_QUOTE_ENTRY(PathRemoveArgsW,unsigned short,ShellRemoveArguments)
#undef SHELL_QUOTE_ENTRY

#include "shell_unc_path.h"
#define UNC_ENTRY(name,type,query) \
    EXPORT name(const type *path){return (Word)shell_unc_query(0,(Pointer)path,sizeof(type),query,read_quote_unit);}
UNC_ENTRY(PathIsRelativeA,unsigned char,ShellRelative)
UNC_ENTRY(PathIsRelativeW,unsigned short,ShellRelative)
UNC_ENTRY(PathIsUNCA,unsigned char,ShellUNC)
UNC_ENTRY(PathIsUNCW,unsigned short,ShellUNC)
UNC_ENTRY(PathIsUNCServerA,unsigned char,ShellServer)
UNC_ENTRY(PathIsUNCServerW,unsigned short,ShellServer)
UNC_ENTRY(PathIsUNCServerShareA,unsigned char,ShellShare)
UNC_ENTRY(PathIsUNCServerShareW,unsigned short,ShellShare)
#undef UNC_ENTRY
EXPORT PathIsUNCEx(const unsigned short *path,const unsigned short **output){
    Pointer server=(Pointer)shell_unc_query(0,(Pointer)path,2,ShellUNCServerAddress,read_quote_unit);
    if(output)*output=(const unsigned short*)server;
    return server!=0;
}

#include "shell_root_query.h"
#define ROOT_QUERY(path,action,count) shell_root_query((struct ShellRootInput){0,(Pointer)(path),sizeof(*(path)),read_quote_unit},action,count,write_quote_unit)
#define ROOT_ENTRIES(suffix,type) \
    EXPORT PathIsRoot##suffix(const type *p){return (Word)ROOT_QUERY(p,ShellIsRoot,0);} \
    __declspec(dllexport) Pointer CALL PathSkipRoot##suffix(const type *p){return (Pointer)ROOT_QUERY(p,ShellSkipLegacyRoot,0);} \
    EXPORT PathStripToRoot##suffix(type *p){return ROOT_QUERY(p,ShellStripRoot,32768)<=1;}
ROOT_ENTRIES(A,unsigned char)
ROOT_ENTRIES(W,unsigned short)
EXPORT PathCchIsRoot(const unsigned short *p){return (Word)ROOT_QUERY(p,ShellIsRoot,0);}
EXPORT PathCchStripToRoot(unsigned short *p,Pointer count){return (Word)ROOT_QUERY(p,ShellStripRoot,count);}
EXPORT PathCchSkipRoot(const unsigned short *p,Pointer *out){
    if(!out)return SHELL_ROOT_INVALID;
    ShellRootAddress value=ROOT_QUERY(p,ShellSkipCchRoot,0);
    if(value==SHELL_ROOT_KEEP_OUTPUT)return SHELL_ROOT_INVALID;
    *out=(Pointer)value;
    return value?0:SHELL_ROOT_INVALID;
}
#undef ROOT_QUERY
#undef ROOT_ENTRIES

#include "shell_end_slash.h"
#define SLASH_INPUT(p) (struct ShellRootInput){0,(Pointer)(p),sizeof(*(p)),read_quote_unit}
#define SLASH_ENTRIES(name,add) \
    __declspec(dllexport) Pointer CALL Path##name##A(unsigned char *p){return (Pointer)shell_legacy_end_slash(SLASH_INPUT(p),add,write_quote_unit);} \
    __declspec(dllexport) Pointer CALL Path##name##W(unsigned short *p){return (Pointer)shell_legacy_end_slash(SLASH_INPUT(p),add,write_quote_unit);} \
    EXPORT PathCch##name##Ex(unsigned short *p,Pointer cap,Pointer *end,Pointer *remaining){ \
        ShellRootAddress e=SHELL_ROOT_KEEP_OUTPUT,r=SHELL_ROOT_KEEP_OUTPUT; \
        Word status=shell_cch_end_slash(SLASH_INPUT(p),cap,add,end?&e:0,remaining?&r:0,write_quote_unit); \
        if(add || status!=SHELL_ROOT_INVALID || !cap || !end || !remaining){ \
            if(end)*end=(Pointer)e; \
            if(remaining)*remaining=(Pointer)r; \
        } \
        return status; \
    } \
    EXPORT PathCch##name(unsigned short *p,Pointer cap){ \
        ShellRootAddress end,remaining; \
        return shell_cch_end_slash(SLASH_INPUT(p),cap,add,&end,&remaining,write_quote_unit); \
    }
SLASH_ENTRIES(AddBackslash,1)
SLASH_ENTRIES(RemoveBackslash,0)
#undef SLASH_ENTRIES
#include "shell_remove_file_spec.h"
EXPORT PathRemoveFileSpecA(unsigned char *p){return shell_remove_file_spec(SLASH_INPUT(p),0,0,write_quote_unit);}
EXPORT PathRemoveFileSpecW(unsigned short *p){return shell_remove_file_spec(SLASH_INPUT(p),0,0,write_quote_unit);}
EXPORT PathCchRemoveFileSpec(unsigned short *p,Pointer size){return shell_remove_file_spec(SLASH_INPUT(p),1,size,write_quote_unit);}
#include "shell_ole_string.h"
EXPORT OleStrToStrN(unsigned short *dst,Word capacity,const unsigned short *src,Word source_length){
    (void)source_length;
    return shell_ole_string(0,(Pointer)dst,capacity,(Pointer)src,read_quote_unit,write_quote_unit);
}
EXPORT StrToOleStr(unsigned short *dst,const unsigned short *src){
    return shell_ole_string(0,(Pointer)dst,~(Word)0,(Pointer)src,read_quote_unit,write_quote_unit);
}
#include "shell_parse_field.h"
EXPORT ParseFieldW(const unsigned short *src,Word field,unsigned short *dst,Word len){
    return shell_parse_field(0,(Pointer)src,field,(Pointer)dst,len,read_quote_unit,write_quote_unit);
}
#include "shell_path_join.h"
__declspec(dllexport) Pointer CALL PathCombineA(unsigned char *out,const unsigned char *dir,const unsigned char *file){
    return (Pointer)shell_path_join(SLASH_INPUT(out),(Pointer)dir,(Pointer)file,0,write_quote_unit);
}
EXPORT PathAppendA(unsigned char *dir,const unsigned char *file){
    return (Word)shell_path_join(SLASH_INPUT(dir),(Pointer)dir,(Pointer)file,1,write_quote_unit);
}
__declspec(dllexport) Pointer CALL PathCombineW(unsigned short *out,const unsigned short *dir,const unsigned short *file){
    return (Pointer)shell_path_join(SLASH_INPUT(out),(Pointer)dir,(Pointer)file,0,write_quote_unit);
}
EXPORT PathAppendW(unsigned short *dir,const unsigned short *file){
    return (Word)shell_path_join(SLASH_INPUT(dir),(Pointer)dir,(Pointer)file,1,write_quote_unit);
}
#undef SLASH_INPUT

#include "shell_pidl_walk.h"
static unsigned pidl_length(void *ctx,unsigned long long at){
    (void)ctx;
    const unsigned char *p=(const unsigned char *)(Pointer)at;
    return (unsigned)p[0] | ((unsigned)p[1]<<8);
}
EXPORT ILGetSize(const void *p){return (Word)shell_pidl_walk(0,(Pointer)p,ShellPidlSize,pidl_length);}
__declspec(dllexport) Pointer CALL ILGetNext(const void *p){return (Pointer)shell_pidl_walk(0,(Pointer)p,ShellPidlNext,pidl_length);}
__declspec(dllexport) Pointer CALL ILFindLastID(const void *p){return (Pointer)shell_pidl_walk(0,(Pointer)p,ShellPidlLast,pidl_length);}

#include "shell_icon_location.h"
EXPORT PathParseIconLocationA(unsigned char *path){return shell_parse_icon_location(0,(Pointer)path,1,read_quote_unit,write_quote_unit);}
EXPORT PathParseIconLocationW(unsigned short *path){return shell_parse_icon_location(0,(Pointer)path,2,read_quote_unit,write_quote_unit);}
__declspec(dllexport) void CALL PathRemoveBlanksA(unsigned char *path){shell_remove_blanks(0,(Pointer)path,1,read_quote_unit,write_quote_unit);}
__declspec(dllexport) void CALL PathRemoveBlanksW(unsigned short *path){shell_remove_blanks(0,(Pointer)path,2,read_quote_unit,write_quote_unit);}

#include "shell_drive_root.h"
__declspec(dllexport) Pointer CALL PathBuildRootA(unsigned char *path, Word drive){
    return (Pointer)shell_build_drive_root(0,(Pointer)path,1,drive,write_quote_unit);
}
__declspec(dllexport) Pointer CALL PathBuildRootW(unsigned short *path, Word drive){
    return (Pointer)shell_build_drive_root(0,(Pointer)path,2,drive,write_quote_unit);
}
__declspec(dllexport) int CALL PathGetDriveNumberA(const unsigned char *path){
    return shell_drive_number(0,(Pointer)path,1,read_quote_unit);
}
__declspec(dllexport) int CALL PathGetDriveNumberW(const unsigned short *path){
    return shell_drive_number(0,(Pointer)path,2,read_quote_unit);
}

#include "shell_file_name.h"
#define FILE_NAME_ENTRY(name,type,strip) \
    __declspec(dllexport) Pointer CALL name(type *text){ \
        return (Pointer)shell_file_name(0,(Pointer)text,sizeof(type),strip,read_quote_unit,write_quote_unit); \
    }
FILE_NAME_ENTRY(PathFindFileNameA,unsigned char,0)
FILE_NAME_ENTRY(PathFindFileNameW,unsigned short,0)
FILE_NAME_ENTRY(PathStripPathA,unsigned char,1)
FILE_NAME_ENTRY(PathStripPathW,unsigned short,1)
#undef FILE_NAME_ENTRY

#include "shell_extension_text.h"
EXPORT PathIsExe(const unsigned short *path){return shell_is_exe(0,(Pointer)path,read_quote_unit);}
#define EXT_CALL(text,ext,action) (Pointer)shell_extension_text(0,(Pointer)(text),(Pointer)(ext),sizeof(*(text)),action,read_quote_unit,write_quote_unit)
#define EXT_ENTRY(name,type,action) __declspec(dllexport) Pointer CALL name(type *text){return EXT_CALL(text,0,action);}
#define EXT_EDIT(name,type,action) __declspec(dllexport) Word CALL name(type *text,const type *ext){return (Word)EXT_CALL(text,ext,action);}
EXT_ENTRY(PathFindExtensionA,unsigned char,ShellFindExtension)
EXT_ENTRY(PathFindExtensionW,unsigned short,ShellFindExtension)
EXT_ENTRY(PathRemoveExtensionA,unsigned char,ShellRemoveExtension)
EXT_ENTRY(PathRemoveExtensionW,unsigned short,ShellRemoveExtension)
EXT_EDIT(PathAddExtensionA,unsigned char,ShellAddExtension)
EXT_EDIT(PathAddExtensionW,unsigned short,ShellAddExtension)
EXT_EDIT(PathRenameExtensionA,unsigned char,ShellRenameExtension)
EXT_EDIT(PathRenameExtensionW,unsigned short,ShellRenameExtension)
__declspec(dllexport) Pointer CALL PathGetExtension(unsigned short *text,Word reserved1,Word reserved2){
    (void)reserved1; (void)reserved2;
    return EXT_CALL(text,0,ShellGetExtension);
}
#undef EXT_ENTRY
#undef EXT_EDIT
#undef EXT_CALL

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

typedef Word (CALL *GetProps)(void*,Word*,Word,Word*,void**);
typedef Word (CALL *GetPropList)(void*,Word,Word**);
typedef Word (CALL *SetProps)(void*,Word,void*,void*);
typedef Word (CALL *AllocTags)(Word,Word**);
typedef Word (CALL *SetColumns)(void*,Word*,Word);
typedef void (*FilterTags)(Word*);
EXPORT HrAddColumnsEx(void *table,const Word *added,AllocTags alloc,One release,FilterTags filter){
    if(!table || !added || !alloc || !release)return 0x80070057U;
    Word *old=0,*merged=0;
    Word hr=((GetPropList)(*(Pointer**)table)[8])(table,0,&old);
    if(hr&0x80000000U)return hr;
    if(!old)return 0x80004005U;
    const Word capacity=old[0];
    if(filter)filter(old);
    const Word extra=added[0],previous=old[0];
    if(previous>capacity)hr=0x80070057U;
    else if(previous>0x3ffffffeU || extra>0x3ffffffeU-previous)hr=0x8007000eU;
    else{
        hr=alloc((extra+previous+1)*4,&merged);
        if(!(hr&0x80000000U)){
            if(!merged)hr=0x8007000eU;
            else{
                Word count=extra;
                for(Word i=1;i<=extra;++i)merged[i]=added[i];
                for(Word i=1;i<=previous;++i){
                    Word j=1;
                    while(j<=extra && merged[j]!=old[i])++j;
                    if(j>extra)merged[++count]=old[i];
                }
                merged[0]=count;
                hr=((SetColumns)(*(Pointer**)table)[7])(table,merged,0);
                release(merged);
            }
        }
    }
    release(old);
    return hr;
}
EXPORT HrAddColumns(void *table,const Word *added,AllocTags alloc,One release){
    return HrAddColumnsEx(table,added,alloc,release,0);
}
EXPORT HrGetOneProp(void *object,Word tag,void **output){
    Word tags[2]={1,tag},count=0;
    Word hr=((GetProps)(*(Pointer**)object)[5])(object,tags,0,&count,output);
    if(hr==0x40380U){
        ((One)GuestServiceFallbacks[4])(*output);
        *output=0;
        return 0x8004010fU;
    }
    return hr;
}
EXPORT HrSetOneProp(void *object,void *prop){
    return ((SetProps)(*(Pointer**)object)[8])(object,1,prop,0);
}
EXPORT FPropExists(void *object,Word tag){
    if(!object)return 0;
    Word *tags=0;
    Word hr=((GetPropList)(*(Pointer**)object)[6])(object,0,&tags);
    if(hr&0x80000000U)return 0;
    Word found=0;
    for(Word i=0;i<tags[0];++i){
        if(mapi_property_matches(tags[1+(Pointer)i],tag,0)){found=1;break;}
    }
    ((One)GuestServiceFallbacks[4])(tags);
    return found;
}
EXPORT UlAddRef(void *object){
    return object?((One)(*(Pointer **)object)[1])(object):0;
}
EXPORT UlRelease(void *object){
    return object?((One)(*(Pointer **)object)[2])(object):0;
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
