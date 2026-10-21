#ifndef X64ARM_NATIVE_V8_GUEST_H
#define X64ARM_NATIVE_V8_GUEST_H
#include <stdint.h>
/* ABI v1：x86／x64 共用固定寬度封包。所有位址是客體位址，token 不是 V8 指標。 */
#pragma pack(push, 1)
typedef struct X64ArmV8Request {
    uint32_t op,isolate,handle,aux;
    uint64_t input;uint32_t input_size,reserved;
    uint64_t output;uint32_t output_capacity,output_size,result,status;
    uint64_t callback,userdata;
} X64ArmV8Request;
#pragma pack(pop)
enum X64ArmV8Op { XV8_VERSION=0,XV8_CREATE=1,XV8_DISPOSE=2,XV8_EVAL=3,XV8_RETAIN=4,XV8_RELEASE=5,
    XV8_STRING=6,XV8_GET=7,XV8_SET=8,XV8_CALL=9,XV8_FUNCTION=10,XV8_EQUAL=11,XV8_COLLECT=12,
    XV8_MICROTASKS=13,XV8_READ_BUFFER=14,XV8_WRITE_BUFFER=15 };
enum X64ArmV8Status { XV8_OK=0,XV8_EXCEPTION=1,XV8_INVALID=2,XV8_THREAD=3,XV8_BUSY=4,XV8_CAPACITY=5 };
#endif
