#pragma once
#include <stdint.h>

typedef uint8_t (*MapiHexRead)(void *,uint64_t);
typedef void (*MapiHexWrite)(void *,uint64_t,uint8_t);
enum MapiHexOperation { MapiHexEncode, MapiHexDecode, MapiHexBounded, MapiHexNumber };

static inline uint32_t mapi_unsigned_decimal(uint64_t address,void *memory,MapiHexRead read_byte){
    uint32_t value=0;
    if(address){
        for(;;){
            const uint8_t ch=read_byte(memory,address++);
            if(ch<'0' || ch>'9')break;
            value=value*10u+(uint32_t)(ch-'0');
        }
    }
    return value;
}

static inline int mapi_hex_digit(unsigned c){
    if(c>='0' && c<='9')return (int)c-'0';
    if(c>='A' && c<='F')return (int)c-'A'+10;
    if(c>='a' && c<='f')return (int)c-'a'+10;
    return -1;
}

static inline uint32_t mapi_hex_convert(enum MapiHexOperation op,uint64_t src,uint64_t dst,
                                        uint32_t count,void *memory,MapiHexRead read_byte,MapiHexWrite write_byte){
    if(op==MapiHexEncode){
        static const uint8_t digits[]="0123456789ABCDEF";
        for(int32_t i=0;i<(int32_t)count;++i){
            unsigned byte=read_byte(memory,src++);
            write_byte(memory,dst++,digits[byte>>4]);
            write_byte(memory,dst++,digits[byte&15]);
        }
        write_byte(memory,dst,0);
        return 0;
    }
    const uint32_t invalid=0x80070057u,too_big=0x80040305u;
    if(op==MapiHexBounded && (!src || (!dst && count)))return invalid;
    uint32_t value=0;
    for(;;){
        unsigned high=read_byte(memory,src++);
        if(!high)break;
        int hi=mapi_hex_digit(high);
        int lo=hi<0?-1:mapi_hex_digit(read_byte(memory,src++));
        if(lo<0)return op==MapiHexBounded?invalid:op==MapiHexNumber?value:0;
        unsigned byte=((unsigned)hi<<4)|(unsigned)lo;
        if(op==MapiHexNumber){
            /* 參考實作每消耗兩個字元即乘 16，累計結果保留低 32 位。 */
            value=value*16+byte;
        }else{
            if(op==MapiHexBounded && !count--)return too_big;
            write_byte(memory,dst++,(uint8_t)byte);
        }
    }
    return op==MapiHexNumber?value:op==MapiHexDecode;
}
