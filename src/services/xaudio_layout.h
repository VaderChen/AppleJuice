#pragma once
/* 兩種客體 ABI 共用；lowering 和宿主服務讀取同一份控制狀態。 */
#define XA_OBJECT_BYTES 352
#define XA_VERSION 8
#define XA_FLAGS 12
#define XA_ACTIVE 16
#define XA_CHANNELS 20
#define XA_RATE 24
#define XA_VOLUME 28
#define XA_RATIO 32
#define XA_FILTER 36
#define XA_MASK 48
#define XA_VOLUMES 64
#define XA_CONTEXT 320
#define XA_QUEUED 328
#define XA_SAMPLES 332
