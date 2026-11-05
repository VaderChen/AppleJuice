/* 緩衝區格式化家族的客體常駐快路徑。先在有界堆疊完成整份輸出，才提交客體
 * 緩衝區；浮點、跨編碼、位置參數、大輸出與錯誤均在提交前回既有 provider。
 * 不快取格式字串或參數；正常客體載入／儲存與 unwind 承接記憶體故障。 */
#include "crt_format_contract.h"
INLINE unsigned format_unit(const void* p,Size i,int wide) {
    return wide?((const Wide*)p)[i]:((const Byte*)p)[i];
}
INLINE Word format_argument(const Byte** arguments,unsigned size) {
    Word value;
    if(size==8)__builtin_memcpy(&value,*arguments,8);
    else {unsigned narrow;__builtin_memcpy(&narrow,*arguments,4);value=narrow;}
    *arguments+=8;return value;
}
/* 同一份 parser 依 ABI 字元寬度各產一份本體；字元寬度不是客體資料，
 * 不應在每個字元載入與輸出處反覆判斷。回傳高 32 位全一表示零提交回退。 */
#define FORMAT_BUFFER_NAME format_buffer_narrow
#define FORMAT_OUTPUT Byte
#define FORMAT_WIDE 0
#include "guest_crt_format_core.inc"
#undef FORMAT_BUFFER_NAME
#undef FORMAT_OUTPUT
#undef FORMAT_WIDE
#define FORMAT_BUFFER_NAME format_buffer_wide
#define FORMAT_OUTPUT Wide
#define FORMAT_WIDE 1
#include "guest_crt_format_core.inc"
#undef FORMAT_BUFFER_NAME
#undef FORMAT_OUTPUT
#undef FORMAT_WIDE
INLINE int format_buffer(void* destination,Size capacity,Size limit,const void* specification,
        const void* arguments,Word options,unsigned mode,int* result) {
    const Word value=mode&X64ARM_FMT_WIDE?
        format_buffer_wide(destination,capacity,limit,specification,arguments,options,mode):
        format_buffer_narrow(destination,capacity,limit,specification,arguments,options,mode);
    if(value==~0ULL)return 0;
    *result=(int)value;return 1;
}
/* v 入口與變動參數入口共用同一 parser，回退固定指向原 v provider。 */
#define FORMAT_PAIR(name,vname,mode) \
EXPORT int (*volatile host_##vname)(void*,Size,const void*,const void*); \
EXPORT int guest_format_##vname(void* out,Size size,const void* fmt,const void* args) { \
    int result;if(format_buffer(out,size,size,fmt,args,0,mode,&result))return result;return host_##vname(out,size,fmt,args); } \
EXPORT int guest_format_##name(void* out,Size size,const void* fmt,...) { \
    __builtin_va_list args;__builtin_va_start(args,fmt);int result=guest_format_##vname(out,size,fmt,args);__builtin_va_end(args);return result; }
FORMAT_PAIR(snprintf,vsnprintf,0)
FORMAT_PAIR(_snprintf,_vsnprintf,X64ARM_FMT_LEGACY)
FORMAT_PAIR(_snwprintf,_vsnwprintf,X64ARM_FMT_LEGACY|X64ARM_FMT_WIDE)
FORMAT_PAIR(sprintf_s,vsprintf_s,X64ARM_FMT_SECURE)
FORMAT_PAIR(swprintf_s,vswprintf_s,X64ARM_FMT_SECURE|X64ARM_FMT_WIDE)
#undef FORMAT_PAIR
#define FORMAT_LIMIT(name,vname,mode) \
EXPORT int (*volatile host_##vname)(void*,Size,Size,const void*,const void*); \
EXPORT int guest_format_##vname(void* out,Size size,Size limit,const void* fmt,const void* args) { \
    int result;if(format_buffer(out,size,limit,fmt,args,0,X64ARM_FMT_SECURE|X64ARM_FMT_LIMITED|mode,&result))return result;return host_##vname(out,size,limit,fmt,args); } \
EXPORT int guest_format_##name(void* out,Size size,Size limit,const void* fmt,...) { \
    __builtin_va_list args;__builtin_va_start(args,fmt);int result=guest_format_##vname(out,size,limit,fmt,args);__builtin_va_end(args);return result; }
FORMAT_LIMIT(_snprintf_s,_vsnprintf_s,0)
FORMAT_LIMIT(_snwprintf_s,_vsnwprintf_s,X64ARM_FMT_WIDE)
#undef FORMAT_LIMIT
EXPORT int (*volatile host_vsprintf)(void*,const void*,const void*);
EXPORT int guest_format_vsprintf(void* out,const void* fmt,const void* args) {
    int result;if(format_buffer(out,16U<<20,16U<<20,fmt,args,0,0,&result))return result;return host_vsprintf(out,fmt,args);
}
EXPORT int guest_format_sprintf(void* out,const void* fmt,...) {
    __builtin_va_list args;__builtin_va_start(args,fmt);int result=guest_format_vsprintf(out,fmt,args);__builtin_va_end(args);return result;
}
#define FORMAT_COUNT(name,vname,mode) \
EXPORT int (*volatile host_##vname)(const void*,const void*); \
EXPORT int guest_format_##vname(const void* fmt,const void* args) { \
    int result;if(format_buffer(0,0,0,fmt,args,0,X64ARM_FMT_COUNT|mode,&result))return result;return host_##vname(fmt,args); } \
EXPORT int guest_format_##name(const void* fmt,...) { \
    __builtin_va_list args;__builtin_va_start(args,fmt);int result=guest_format_##vname(fmt,args);__builtin_va_end(args);return result; }
FORMAT_COUNT(_scprintf,_vscprintf,0)
FORMAT_COUNT(_scwprintf,_vscwprintf,X64ARM_FMT_WIDE)
#undef FORMAT_COUNT
#define FORMAT_COMMON(name,mode) \
EXPORT int (*volatile host_##name)(Word,void*,Size,const void*,void*,const void*); \
EXPORT int guest_format_##name(Word options,void* out,Size size,const void* fmt,void* locale,const void* args) { \
    int result;if(!locale && format_buffer(out,size,size,fmt,args,options,X64ARM_FMT_COMMON|mode,&result))return result;return host_##name(options,out,size,fmt,locale,args); }
FORMAT_COMMON(__stdio_common_vsprintf,0)
FORMAT_COMMON(__stdio_common_vsprintf_s,X64ARM_FMT_SECURE)
FORMAT_COMMON(__stdio_common_vswprintf,X64ARM_FMT_WIDE)
FORMAT_COMMON(__stdio_common_vswprintf_s,X64ARM_FMT_SECURE|X64ARM_FMT_WIDE)
#undef FORMAT_COMMON
#define FORMAT_COMMON_LIMIT(name,mode) \
EXPORT int (*volatile host_##name)(Word,void*,Size,Size,const void*,void*,const void*); \
EXPORT int guest_format_##name(Word options,void* out,Size size,Size limit,const void* fmt,void* locale,const void* args) { \
    int result;if(!locale && format_buffer(out,size,limit,fmt,args,options,X64ARM_FMT_COMMON|X64ARM_FMT_SECURE|X64ARM_FMT_LIMITED|mode,&result))return result;return host_##name(options,out,size,limit,fmt,locale,args); }
FORMAT_COMMON_LIMIT(__stdio_common_vsnprintf_s,0)
FORMAT_COMMON_LIMIT(__stdio_common_vsnwprintf_s,X64ARM_FMT_WIDE)
#undef FORMAT_COMMON_LIMIT
