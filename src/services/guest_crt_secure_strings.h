/* 與宿主 provider 共用容量／計數契約。錯誤與大型輸出在任何提交前回原 provider；
 * 一般短字串直接由客體常式執行，保留原生 MMU 的故障及寫入通知。 */
#include "crt_secure_string_contract.h"
INLINE unsigned secure_string_unit(const void* source,Size index,unsigned wide){
    return wide?((const Wide*)source)[index]:((const Byte*)source)[index];
}
__attribute__((noinline)) static int secure_string_copy(void* destination,Size capacity,
        const void* source,Size count,unsigned append,unsigned bounded,unsigned wide,int* result){
    if(!destination || !capacity || capacity>x64arm_crt_secure_rsize_max(8) || (!source && (!bounded || count)))return 0;
    Wide text[512];Size used=0;
    if(append){
        while(used<capacity){
            const unsigned c=secure_string_unit(destination,used,wide);
            if(!c)break;
            if(used==511)return 0;text[used++]=(Wide)c;
        }
        if(used==capacity)return 0;
    }
    const Size room=capacity-used-1;
    const Size limit=(Size)x64arm_crt_secure_scan_limit(capacity,used,count,~0ULL,bounded);
    int status=0;
    for(Size index=0;index<limit;++index){
        const unsigned c=secure_string_unit(source,index,wide);
        if(!c)break;
        if(index==room){if(bounded && count==~(Size)0){status=80;break;}return 0;}
        if(used==511)return 0;text[used++]=(Wide)c;
    }
    text[used]=0;
    if(wide){for(Size index=0;index<=used;++index)((Wide*)destination)[index]=text[index];}
    else {for(Size index=0;index<=used;++index)((Byte*)destination)[index]=(Byte)text[index];}
    *result=status;return 1;
}
#define SECURE_STRING_COPY(name,append,wide) \
EXPORT int (*volatile host_##name)(void*,Size,const void*); \
EXPORT int guest_text_##name(void* destination,Size capacity,const void* source){ \
    int result;if(secure_string_copy(destination,capacity,source,0,append,0,wide,&result))return result; \
    return host_##name(destination,capacity,source); }
SECURE_STRING_COPY(strcpy_s,0,0)
SECURE_STRING_COPY(strcat_s,1,0)
SECURE_STRING_COPY(wcscpy_s,0,1)
SECURE_STRING_COPY(wcscat_s,1,1)
#undef SECURE_STRING_COPY
#define SECURE_STRING_LIMIT(name,append,wide) \
EXPORT int (*volatile host_##name)(void*,Size,const void*,Size); \
EXPORT int guest_text_##name(void* destination,Size capacity,const void* source,Size count){ \
    int result;if(secure_string_copy(destination,capacity,source,count,append,1,wide,&result))return result; \
    return host_##name(destination,capacity,source,count); }
SECURE_STRING_LIMIT(strncpy_s,0,0)
SECURE_STRING_LIMIT(strncat_s,1,0)
SECURE_STRING_LIMIT(wcsncpy_s,0,1)
SECURE_STRING_LIMIT(wcsncat_s,1,1)
#undef SECURE_STRING_LIMIT
