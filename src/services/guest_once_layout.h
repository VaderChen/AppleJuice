// INIT_ONCE 已完成結果的派生鏡像；權威狀態與等待者仍由既有 provider 管理。
// TLS 共用頁的保留空間只保存表指標；表由既有 heap 按需配置，最多 64 KiB。
// 0 表示尚未配置，1 表示配置失敗後停用；兩者均沿原 provider。
#pragma once
#include "guest_tls_layout.h"
#define X64ARM_ONCE_CACHE_POINTER_OFFSET 0x100
#define X64ARM_ONCE_CACHE_BITS 12
#define X64ARM_ONCE_CACHE_COUNT (1 << X64ARM_ONCE_CACHE_BITS)
#define X64ARM_ONCE_CACHE_STRIDE 16
#define X64ARM_ONCE_CACHE_BYTES (X64ARM_ONCE_CACHE_COUNT * X64ARM_ONCE_CACHE_STRIDE)
