/* VC9 純讀取字串常式。以客體 ABI 編譯後交 resident AOT lowering，
 * 正常查詢／比較／搜尋不經宿主服務；配置、例外與其餘操作仍有完整 provider。 */
typedef __SIZE_TYPE__ Size;
#if defined(__i386__)
#define MEMBER __attribute__((thiscall))
#else
#define MEMBER
#endif
#define EXPORT __declspec(dllexport)
#define INLINE static __attribute__((always_inline)) inline
#define STRINGS(C,T) \
typedef struct {void* allocator;union {T small[16/sizeof(T)];T* ptr;} data;Size size,capacity;} C##_String; \
typedef struct {const C##_String* owner;const T* position;} C##_Iterator; \
INLINE const T* C##_data(const C##_String* s){return s->capacity<16/sizeof(T)?s->data.small:s->data.ptr;} \
INLINE Size C##_length(const T* p){Size n=0;while(p[n])++n;return n;} \
INLINE int C##_compare(const T* a,Size an,const T* b,Size bn){Size n=an<bn?an:bn;for(Size i=0;i<n;++i)if(a[i]!=b[i])return a[i]<b[i]?-1:1;return an<bn?-1:an>bn;} \
EXPORT Size MEMBER guest_##C##_size(const C##_String* s){return s->size;} \
EXPORT Size MEMBER guest_##C##_capacity(const C##_String* s){return s->capacity;} \
EXPORT unsigned char MEMBER guest_##C##_empty(const C##_String* s){return !s->size;} \
EXPORT Size MEMBER guest_##C##_maximum(const C##_String* s){(void)s;return (Size)-1/sizeof(T)-1;} \
EXPORT const T* MEMBER guest_##C##_data(const C##_String* s){return C##_data(s);} \
EXPORT C##_Iterator* MEMBER guest_##C##_begin(const C##_String* s,C##_Iterator* out){out->owner=s;out->position=C##_data(s);return out;} \
EXPORT C##_Iterator* MEMBER guest_##C##_end(const C##_String* s,C##_Iterator* out){out->owner=s;out->position=C##_data(s)+s->size;return out;} \
EXPORT unsigned char MEMBER guest_##C##_inside(const C##_String* s,const T* p){return (Size)p>=(Size)C##_data(s) && (Size)p-(Size)C##_data(s)<s->size*sizeof(T);} \
EXPORT int MEMBER guest_##C##_compare_text(const C##_String* s,const T* rhs){return C##_compare(C##_data(s),s->size,rhs,C##_length(rhs));} \
EXPORT int MEMBER guest_##C##_compare_string(const C##_String* s,const C##_String* rhs){return C##_compare(C##_data(s),s->size,C##_data(rhs),rhs->size);} \
INLINE Size C##_find(const C##_String* s,const T* needle,Size pos,Size count,int reverse,int set,int negate){ \
    const T* value=C##_data(s);const Size n=s->size;if(!set){if(count>n)return (Size)-1;const Size last=n-count;if(!reverse && pos>last)return (Size)-1; \
        Size i=reverse?(pos<last?pos:last):pos;for(;;){Size k=0;while(k<count && value[i+k]==needle[k])++k;if(k==count)return i;if(reverse){if(!i)break;--i;}else{if(i==last)break;++i;}} \
    }else if(n){if(!reverse && pos>=n)return (Size)-1;Size i=reverse?(pos<n?pos:n-1):pos;for(;;){Size k=0;while(k<count && value[i]!=needle[k])++k;if((k<count)!=negate)return i;if(reverse){if(!i)break;--i;}else if(++i==n)break;}}return (Size)-1; \
}
STRINGS(D,unsigned char)
STRINGS(W,unsigned short)
#define SEARCH(C,T,N,R,S,V) \
EXPORT Size MEMBER guest_##C##_##N##_text(const C##_String* s,const T* p,Size at){return C##_find(s,p,at,C##_length(p),R,S,V);} \
EXPORT Size MEMBER guest_##C##_##N##_span(const C##_String* s,const T* p,Size at,Size count){return C##_find(s,p,at,count,R,S,V);} \
EXPORT Size MEMBER guest_##C##_##N##_string(const C##_String* s,const C##_String* p,Size at){return C##_find(s,C##_data(p),at,p->size,R,S,V);} \
EXPORT Size MEMBER guest_##C##_##N##_character(const C##_String* s,T c,Size at){return C##_find(s,&c,at,1,R,S,V);}
#define SEARCHES(C,T) \
SEARCH(C,T,find,0,0,0) SEARCH(C,T,rfind,1,0,0) SEARCH(C,T,first_of,0,1,0) \
SEARCH(C,T,last_of,1,1,0) SEARCH(C,T,first_not_of,0,1,1) SEARCH(C,T,last_not_of,1,1,1)
SEARCHES(D,unsigned char)
SEARCHES(W,unsigned short)
#define RELATION(C,T,N,OP) \
EXPORT unsigned char guest_##C##_##N##_string(const C##_String* a,const C##_String* b){return C##_compare(C##_data(a),a->size,C##_data(b),b->size) OP 0;} \
EXPORT unsigned char guest_##C##_##N##_text(const C##_String* a,const T* b){return C##_compare(C##_data(a),a->size,b,C##_length(b)) OP 0;} \
EXPORT unsigned char guest_##C##_##N##_text_lhs(const T* a,const C##_String* b){return C##_compare(a,C##_length(a),C##_data(b),b->size) OP 0;}
#define RELATIONS(C,T) \
RELATION(C,T,equal,==) RELATION(C,T,not_equal,!=) RELATION(C,T,less,<) \
RELATION(C,T,greater,>) RELATION(C,T,less_equal,<=) RELATION(C,T,greater_equal,>=)
RELATIONS(D,unsigned char)
RELATIONS(W,unsigned short)

typedef struct {void* table;char* cached;unsigned char decorated[1];} TypeInfo;
INLINE int type_compare(const TypeInfo* a,const TypeInfo* b){
    if(a==b)return 0;const unsigned char* x=a->decorated+1;const unsigned char* y=b->decorated+1;
    while(*x && *x==*y){++x;++y;}return *x<*y?-1:*x>*y;
}
EXPORT unsigned char MEMBER guest_type_equal(const TypeInfo* a,const TypeInfo* b){return type_compare(a,b)==0;}
EXPORT unsigned char MEMBER guest_type_not_equal(const TypeInfo* a,const TypeInfo* b){return type_compare(a,b)!=0;}
EXPORT unsigned char MEMBER guest_type_before(const TypeInfo* a,const TypeInfo* b){return type_compare(a,b)<0;}
EXPORT const unsigned char* MEMBER guest_type_raw_name(const TypeInfo* a){return a->decorated;}

/* VC9 的無副作用串流查詢共用此常駐映像；開檔與回呼仍由宿主 provider 承接。 */
INLINE unsigned stream_state(const void* object){return *(const unsigned*)((const char*)object+2*sizeof(void*));}
EXPORT Size MEMBER guest_stream_count(const void* object){return ((const Size*)object)[1];}
EXPORT unsigned MEMBER guest_stream_state(const void* object){return stream_state(object);}
EXPORT unsigned char MEMBER guest_stream_good(const void* object){return stream_state(object)==0;}
EXPORT unsigned char MEMBER guest_stream_eof(const void* object){return (stream_state(object)&1)!=0;}
EXPORT unsigned char MEMBER guest_stream_fail(const void* object){return (stream_state(object)&6)!=0;}
EXPORT unsigned char MEMBER guest_stream_bad(const void* object){return (stream_state(object)&4)!=0;}
EXPORT unsigned MEMBER guest_stream_exceptions(const void* object){return *(const unsigned*)((const char*)object+2*sizeof(void*)+4);}
#if defined(__i386__)
#define VC9_IOS_SIZE 40
#define VC9_FILE_POINTER 76
#else
#define VC9_IOS_SIZE 72
#define VC9_FILE_POINTER 136
#endif
EXPORT void* MEMBER guest_stream_buffer(const void* object){return *(void* const*)((const char*)object+VC9_IOS_SIZE);}
EXPORT void* MEMBER guest_ifstream_buffer(const void* object){return (char*)object+2*sizeof(void*);}
EXPORT unsigned char MEMBER guest_filebuf_open(const void* object){return *(void* const*)((const char*)object+VC9_FILE_POINTER)!=0;}
EXPORT unsigned char MEMBER guest_ifstream_open(const void* object){return guest_filebuf_open((const char*)object+2*sizeof(void*));}

#include "msvcp_numeric_values.h"
/* Windows 的 long double 回傳使用 double ABI；不可採用 MinGW 的 80-bit 型別。 */
#define NUMERIC(C,T,B,F,S,D,D10,MIN,MAX,EPS,ROUND,INF,NAN,SNAN,DENORM) {MIN,MAX,EPS,ROUND,INF,NAN,SNAN,DENORM},
EXPORT const unsigned long long guest_numeric_values[][8]={MSVCP_NUMERIC_TYPES(NUMERIC)};
#undef NUMERIC
#define NUMERIC(C,T,B,F,S,D,D10,MIN,MAX,EPS,ROUND,INF,NAN,SNAN,DENORM) NumericIndex_##C,
enum {MSVCP_NUMERIC_TYPES(NUMERIC)};
#undef NUMERIC
/* 保留真正的浮點載入，避免編譯器先把 signaling NaN 靜態轉成 quiet NaN。 */
#define NUMERIC_FUNCTION(C,T,B,F,OP,V,INDEX) \
EXPORT T guest_numeric_##C##_##OP(void){ \
    if(F)return *(const volatile T*)&guest_numeric_values[NumericIndex_##C][INDEX];return (T)(V);}
#define NUMERIC(C,T,B,F,S,D,D10,MIN,MAX,EPS,ROUND,INF,NAN,SNAN,DENORM) \
NUMERIC_FUNCTION(C,T,B,F,min,MIN,0) NUMERIC_FUNCTION(C,T,B,F,max,MAX,1) \
NUMERIC_FUNCTION(C,T,B,F,epsilon,EPS,2) NUMERIC_FUNCTION(C,T,B,F,round_error,ROUND,3) \
NUMERIC_FUNCTION(C,T,B,F,infinity,INF,4) NUMERIC_FUNCTION(C,T,B,F,quiet_NaN,NAN,5) \
NUMERIC_FUNCTION(C,T,B,F,signaling_NaN,SNAN,6) NUMERIC_FUNCTION(C,T,B,F,denorm_min,DENORM,7)
MSVCP_NUMERIC_TYPES(NUMERIC)
#undef NUMERIC
EXPORT void* MEMBER guest_numeric_assign(void* object,const void* source){(void)source;return object;}
