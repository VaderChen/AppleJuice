/* CRT 格式輸出的共用純運算契約；宿主與客體常駐常式使用同一份規則。 */
#pragma once
typedef unsigned long long X64armCrtFormatWord;
enum {
    X64ARM_FMT_LEFT=1, X64ARM_FMT_PLUS=2, X64ARM_FMT_SPACE=4,
    X64ARM_FMT_ALT=8, X64ARM_FMT_ZERO=16,
    X64ARM_FMT_COMMON=1, X64ARM_FMT_SECURE=2, X64ARM_FMT_LIMITED=4,
    X64ARM_FMT_LEGACY=8, X64ARM_FMT_WIDE=16, X64ARM_FMT_COUNT=32
};
static inline unsigned x64arm_crt_format_flag(unsigned c) {
    return c=='-'?X64ARM_FMT_LEFT:c=='+'?X64ARM_FMT_PLUS:c==' '?X64ARM_FMT_SPACE:
           c=='#'?X64ARM_FMT_ALT:c=='0'?X64ARM_FMT_ZERO:0;
}
typedef struct {
    char digits[24],prefix[2]; /* digits 以最低位起存放，避免另一次搬移。 */
    unsigned count,prefix_count,spaces,zeros,trailing;
} X64armCrtInteger;
static inline X64armCrtInteger x64arm_crt_integer(X64armCrtFormatWord value,unsigned bits,
        unsigned conversion,unsigned flags,unsigned width,int precision,unsigned pointer_bytes) {
    X64armCrtInteger out;
    out.count=out.prefix_count=out.spaces=out.zeros=out.trailing=0;
    if(conversion=='p') {
        conversion='X';bits=pointer_bytes*8;precision=(int)(pointer_bytes*2);
        if(width<pointer_bytes*2)width=pointer_bytes*2;
    }
    const X64armCrtFormatWord mask=bits==64?~0ULL:(1ULL<<bits)-1;
    value&=mask;
    if(conversion=='d' || conversion=='i') {
        if(value&(1ULL<<(bits-1))) {out.prefix[out.prefix_count++]='-';value=(0ULL-value)&mask;}
        else if(flags&X64ARM_FMT_PLUS)out.prefix[out.prefix_count++]='+';
        else if(flags&X64ARM_FMT_SPACE)out.prefix[out.prefix_count++]=' ';
    }
    const X64armCrtFormatWord original=value;
    if(value || precision!=0) {
        if(conversion=='x' || conversion=='X') {
            const unsigned letter=conversion=='x'?'a':'A';
            do {unsigned d=(unsigned)(value&15);out.digits[out.count++]=(char)(d<10?'0'+d:letter+d-10);value>>=4;}while(value);
        } else if(conversion=='o') {
            do {out.digits[out.count++]=(char)('0'+(value&7));value>>=3;}while(value);
        } else {
            do {const X64armCrtFormatWord next=value/10;out.digits[out.count++]=(char)('0'+value-next*10);value=next;}while(value);
        }
    }
    if(precision>0 && (unsigned)precision>out.count)out.zeros=(unsigned)precision-out.count;
    if(flags&X64ARM_FMT_ALT) {
        if(conversion=='o' && !out.zeros && (!out.count || out.digits[out.count-1]!='0'))out.zeros=1;
        if(original && (conversion=='x' || conversion=='X')) {out.prefix[0]='0';out.prefix[1]=(char)conversion;out.prefix_count=2;}
    }
    const unsigned used=out.prefix_count+out.zeros+out.count;
    if(width>used) {
        if(flags&X64ARM_FMT_LEFT)out.trailing=width-used;
        else if((flags&X64ARM_FMT_ZERO) && precision<0)out.zeros+=width-used;
        else out.spaces=width-used;
    }
    return out;
}
typedef struct {
    X64armCrtFormatWord length,result;
    unsigned terminate,invalid_parameter,errno_after_write;
} X64armCrtBufferPlan;
static inline X64armCrtBufferPlan x64arm_crt_buffer_plan(X64armCrtFormatWord length,int failed,
        X64armCrtFormatWord capacity,X64armCrtFormatWord limit,X64armCrtFormatWord truncate,
        X64armCrtFormatWord options,unsigned mode) {
    X64armCrtBufferPlan out={0,failed?0xffffffffULL:length,0,0,0};
    const int secure=(mode&X64ARM_FMT_SECURE)!=0,limited=(mode&X64ARM_FMT_LIMITED)!=0;
    const int common=(mode&X64ARM_FMT_COMMON)!=0;
    const int legacy=!secure && (common?(options&1)!=0:(mode&X64ARM_FMT_LEGACY)!=0);
    if((secure && !limited && length>=capacity) ||
       (limited && limit>=capacity && limit!=truncate && length>=capacity)) {out.invalid_parameter=34;return out;}
    const X64armCrtFormatWord available=capacity?capacity-(legacy?0U:1U):0;
    out.length=length<available?length:available;
    if(limited && limit<out.length)out.length=limit;
    out.terminate=capacity && out.length<capacity;
    if((limited && out.length<length) || (legacy && length>capacity) ||
       (common && !secure && !legacy && !(options&2) && length>=capacity))out.result=0xffffffffULL;
    if(!capacity && legacy) {out.errno_after_write=34;out.result=0xffffffffULL;}
    return out;
}
