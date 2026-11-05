#pragma once
/* 兩種客體 ABI 共用的資料配置；指標槽保留 8 bytes，x86 使用低 32 bits。 */
#define MF_BUFFER_BYTES 72
#define MF_BUFFER_REFS 8
#define MF_BUFFER_LOCKS 12
#define MF_BUFFER_DATA 16
#define MF_BUFFER_MAXIMUM 24
#define MF_BUFFER_LENGTH 28
#define MF_BUFFER_PLAIN 32
#define MF_BUFFER_DESTROY 40
#define MF_BUFFER_LOCK_HOST 48
#define MF_BUFFER_UNLOCK_HOST 56
#define MF_BUFFER_QUERY_HOST 64
static const unsigned char mf_buffer_iunknown[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const unsigned char mf_buffer_interface[16]={0x93,0xa5,0x5f,0x04,0x99,0x87,0xb8,0x42,0xbc,0x8d,0x89,0x68,0xc6,0x45,0x35,0x07};
