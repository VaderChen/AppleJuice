#pragma once
/* COM 指標槽固定保留 8 bytes，兩種客體 ABI 共用常數資料。 */
#define D9_CONSTANT_FLAGS 8
#define D9_CONSTANT_HOST 16
#define D9_CONSTANT_DATA 128
#define D9_CONSTANT_BYTES 8320
#define D9_CONSTANT_DEVICE_BYTES (D9_CONSTANT_DATA+D9_CONSTANT_BYTES)
static inline unsigned d9_constant_limit(unsigned stage,unsigned kind){return kind?16U:stage?224U:256U;}
static inline unsigned d9_constant_stride(unsigned kind){return kind==2?4U:16U;}
static inline unsigned d9_constant_offset(unsigned stage,unsigned kind){
    return stage?(kind==0?4416U:kind==1?8000U:8256U):(kind==0?0U:kind==1?4096U:4352U);
}
