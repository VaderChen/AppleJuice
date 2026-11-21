#pragma once
/* COM 指標槽固定保留 8 bytes，兩種客體 ABI 共用常數資料。 */
#define D9_CONSTANT_FLAGS 8
#define D9_CONSTANT_HOST 16
/* 多執行緒裝置（旗標位元 2）的客體端寫入鎖與寫入序號：寫入者之間以鎖互斥，寫入前後各遞增序號；宿主讀取以序號核對取得一致的快照。
   位於宿主入口表（12 個 × 8 bytes，到 112）之後、常數資料（128）之前。 */
#define D9_CONSTANT_LOCK 112
#define D9_CONSTANT_SEQUENCE 116
#define D9_CONSTANT_DATA 128
#define D9_CONSTANT_BYTES 8320
#define D9_CONSTANT_DEVICE_BYTES (D9_CONSTANT_DATA+D9_CONSTANT_BYTES)
static inline unsigned d9_constant_limit(unsigned stage,unsigned kind){return kind?16U:stage?224U:256U;}
static inline unsigned d9_constant_stride(unsigned kind){return kind==2?4U:16U;}
static inline unsigned d9_constant_offset(unsigned stage,unsigned kind){
    return stage?(kind==0?4416U:kind==1?8000U:8256U):(kind==0?0U:kind==1?4096U:4352U);
}
