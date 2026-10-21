#pragma once
// C++ 與組合語言共用入口表寬度；碰撞仍須核對完整客體 RIP。
// 512 KiB 的原生入口表避免大型 runtime 的間接呼叫頻繁返回協調者。
#define X64ARM_NATIVE_DISPATCH_BITS 16
#define X64ARM_NATIVE_DISPATCH_MASK ((1 << X64ARM_NATIVE_DISPATCH_BITS) - 1)
// 原生串接的入口表（IBTC／串接器查找）。大型程式常有數十萬個串接入口，16-bit 直接映射表會互相擠掉，
// 未命中即結束整批回到 C++；查找表涵蓋整個工作集，使用 2^20 格（8 MiB）。
#define X64ARM_CHAIN_DISPATCH_BITS 16
#define X64ARM_CHAIN_DISPATCH_MASK ((1 << X64ARM_CHAIN_DISPATCH_BITS) - 1)
