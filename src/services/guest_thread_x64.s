// Windows x64 執行緒區域常式。資料始終讀寫目前 GS 指向的 TEB，不快取執行緒值。
// 只使用 volatile GPR；不變更 XMM、非 volatile GPR 或 RSP（RET 除外）。
// TLS 的 64 個內建槽與 1024 個擴充槽沿用 WindowsServices 的既有配置。
#include "guest_tls_layout.h"
#include "guest_once_layout.h"
#include "gdi_batch_layout.h"
#include "d3d12_root_binding_layout.h"
#include "d3d12_descriptor_layout.h"
.intel_syntax noprefix
.text
.macro entry name
.p2align 5
.globl _guest_\name
_guest_\name:
.endm
.macro end name
.globl _guest_\name\()_end
_guest_\name\()_end:
.endm

entry GetLastError
    mov eax, dword ptr gs:[X64ARM_TLS_X64_ERROR_OFFSET]
    ret
end GetLastError

entry SetLastError
    mov dword ptr gs:[X64ARM_TLS_X64_ERROR_OFFSET], ecx
    xor eax, eax
    ret
end SetLastError

.macro tls_read name, clear_error
entry \name
    // DWORD 參數：不使用呼叫者未定義的 RCX 高半部。
    mov ecx, ecx
    cmp ecx, X64ARM_TLS_SLOTS
    jae Linvalid\@
    cmp ecx, X64ARM_TLS_INLINE_SLOTS
    jae Lexpanded\@
    mov rax, qword ptr gs:[rcx*8+X64ARM_TLS_X64_INLINE_OFFSET]
    jmp Lvalue\@
Lexpanded\@:
    // 0x2000 + (index - 64) * 8；與 C provider 的實際配置相同。
    mov rax, qword ptr gs:[rcx*8+X64ARM_TLS_EXPANSION_OFFSET-X64ARM_TLS_INLINE_SLOTS*8]
Lvalue\@:
.if \clear_error
    mov dword ptr gs:[X64ARM_TLS_X64_ERROR_OFFSET], 0
.endif
    ret
Linvalid\@:
.if \clear_error
    mov dword ptr gs:[X64ARM_TLS_X64_ERROR_OFFSET], 87
.endif
    xor eax, eax
    ret
end \name
.endm
tls_read TlsGetValue, 1
tls_read TlsGetValue2, 0

entry GetCurrentProcess
    mov rax, -1
    ret
end GetCurrentProcess
entry GetCurrentThread
    mov rax, -2
    ret
end GetCurrentThread

entry GetCurrentThreadId
    // TEB ClientId.UniqueThread：主執行緒與建立執行緒的 public_id 均由服務層寫入 gs:[0x48]。
    mov eax, dword ptr gs:[0x48]
    ret
end GetCurrentThreadId

// 成功保持 LastError；DWORD 索引截斷，LPVOID 保留完整 64 位元。
// 低 64 槽採最小驗證，擴充槽讀程序共用配置位元圖；只使用 volatile GPR。
entry TlsSetValue
    mov ecx, ecx
    cmp ecx, X64ARM_TLS_INLINE_SLOTS
    jb Lset_small
    cmp ecx, X64ARM_TLS_SLOTS
    jae Lset_invalid
    mov eax, ecx
    shr eax, 6
    .byte 0x49, 0xb8 // mov r8, imm64；初始化時填入配置位元圖位址。
Ltls_bitmap_patch:
    .quad 0
    mov r8, qword ptr [r8+rax*8]
    bt r8, rcx
    jnc Lset_invalid
    mov qword ptr gs:[rcx*8+X64ARM_TLS_EXPANSION_OFFSET-X64ARM_TLS_INLINE_SLOTS*8], rdx
    jmp Lset_success
Lset_small:
    mov qword ptr gs:[rcx*8+X64ARM_TLS_X64_INLINE_OFFSET], rdx
Lset_success:
    mov eax, 1
    ret
Lset_invalid:
    mov dword ptr gs:[X64ARM_TLS_X64_ERROR_OFFSET], 87
    xor eax, eax
    ret
end TlsSetValue
.set _guest_tls_bitmap_patch, Ltls_bitmap_patch - _guest_TlsSetValue

// SRW 鎖字組編碼（與服務層共用）：bit0 獨占持有、bit1 宿主有等待者、bit2 起為共用持有數。
// 快速路徑只在 bit0／bit1 皆零時以 lock cmpxchg（JIT 對應 LSE CASAL）取得或釋放；其餘狀態尾跳原服務，
// 由宿主處理競爭、等待與喚醒。只使用 RAX／RDX 與旗標，RCX 保持原值供慢路徑。
.macro slow_end name
    jmp qword ptr [rip + Lslot_\name]
.p2align 3
Lslot_\name:
    .quad 0
end \name
.endm

// 只有 provider 已發布、物件字組也相符的已完成結果可直接返回。
// 初始化、回呼、等待、重試及重新使用物件的狀態轉移仍走原服務。
// volatile 暫存器保留 RCX/RDX/R8/R9，任何 miss 都能用原參數尾跳。
.macro once_completed name, begin
entry \name
.if \begin
    test edx, -2
    jne 9f
    test r8b, 3
    jne 9f
.endif
    test cl, 7
    jne 9f
    test r9b, 7
    jne 9f
    mov rax, rcx
    shr rax, 3
    // 折疊高位，避免不同配置區中相同低位偏移的物件持續互相逐出。
    mov r11, rax
    shr r11, X64ARM_ONCE_CACHE_BITS
    xor rax, r11
    and eax, X64ARM_ONCE_CACHE_COUNT-1
    shl eax, 4
    .byte 0x49, 0xbb // mov r11, imm64；沿既有頁位址修補，不保留外部重定位。
Lonce_patch\@:
    .quad 0
.set _guest_\name\()_once_cache_patch, Lonce_patch\@ - _guest_\name
    mov r11, qword ptr [r11+X64ARM_ONCE_CACHE_POINTER_OFFSET]
    cmp r11, 1
    jbe 9f
    add r11, rax
    cmp qword ptr [r11], rcx
    jne 9f
    mov r10, qword ptr [r11+8]
    cmp qword ptr [r11], rcx
    jne 9f
    // 同值 CAS 同時核對物件世代與可寫性；不以純讀省略 provider 的 Write 契約。
    mov rax, r10
    lock cmpxchg qword ptr [rcx], r10
    jne 9f
.if \begin
    // 先驗證全部輸出再提交，失敗 CAS 也不改變原值。
    xor eax, eax
    lock cmpxchg dword ptr [r8], eax
.endif
    test r9, r9
    je 1f
    xor eax, eax
    lock cmpxchg qword ptr [r9], rax
1:
.if \begin
    mov dword ptr [r8], 0
.endif
    test r9, r9
    je 2f
    and r10, -4
    mov qword ptr [r9], r10
2:
    mov eax, 1
    ret
9:
slow_end \name
.endm
once_completed InitOnceBeginInitialize, 1
once_completed InitOnceExecuteOnce, 0

entry AcquireSRWLockExclusive
    xor eax, eax
    mov edx, 1
    lock cmpxchg qword ptr [rcx], rdx
    jne 1f
    ret
1:
slow_end AcquireSRWLockExclusive

entry ReleaseSRWLockExclusive
    mov eax, 1
    xor edx, edx
    lock cmpxchg qword ptr [rcx], rdx
    jne 1f
    ret
1:
slow_end ReleaseSRWLockExclusive

entry AcquireSRWLockShared
    mov rax, qword ptr [rcx]
    test al, 3
    jne 1f
    cmp rax, -4
    je 1f
    lea rdx, [rax + 4]
    lock cmpxchg qword ptr [rcx], rdx
    jne 1f
    ret
1:
slow_end AcquireSRWLockShared

entry ReleaseSRWLockShared
    mov rax, qword ptr [rcx]
    test al, 3
    jne 1f
    cmp rax, 4
    jb 1f
    lea rdx, [rax - 4]
    lock cmpxchg qword ptr [rcx], rdx
    jne 1f
    ret
1:
slow_end ReleaseSRWLockShared

entry TryAcquireSRWLockExclusive
    xor eax, eax
    mov edx, 1
    lock cmpxchg qword ptr [rcx], rdx
    jne 1f
    mov eax, 1
    ret
1:
slow_end TryAcquireSRWLockExclusive

entry TryAcquireSRWLockShared
    mov rax, qword ptr [rcx]
    test al, 3
    jne 1f
    cmp rax, -4
    je 1f
    lea rdx, [rax + 4]
    lock cmpxchg qword ptr [rcx], rdx
    jne 1f
    mov eax, 1
    ret
1:
slow_end TryAcquireSRWLockShared
// 臨界區段（字組協定與服務層共用，見 windows_threads.cpp）：LockCount＝-1 未持有；持有時低 30 位元為遞迴次數−1、
// bit30 表示宿主有等待者；0x7fffffff 為未持有但有等待者。OwningThread 為 TEB ClientId.UniqueThread（gs:[0x48]）。
// 只在無等待者時以 lock cmpxchg 取得／完全釋放，遞迴由持有者遞增／遞減。loader lock（PEB+0x800）、有等待者的釋放、
// 遞迴上限、非持有者解鎖與未持有但有等待者一律以原參數尾跳原服務。只使用 RAX／RDX 與旗標，RCX 保持原值。
.macro critical_loader_lock
    mov rax, qword ptr gs:[0x60]
    add rax, 0x800
    cmp rcx, rax
    je 1f
.endm

entry EnterCriticalSection
    critical_loader_lock
    mov eax, -1
    xor edx, edx
    lock cmpxchg dword ptr [rcx + 8], edx
    jne 2f
5:
    mov eax, dword ptr gs:[0x48]
    mov qword ptr [rcx + 16], rax
    mov dword ptr [rcx + 12], 1
    ret
2:
    mov edx, dword ptr gs:[0x48]
    cmp qword ptr [rcx + 16], rdx
    jne 3f
    mov edx, dword ptr [rcx + 12]
    cmp edx, 0x3ffffffe
    jae 1f
    inc edx
    mov dword ptr [rcx + 12], edx
    lock inc dword ptr [rcx + 8]
    ret
3:
    // 他人持有：多處理器（PEB NumberOfProcessors>1）時先有界自旋等字組回到 -1（與 Windows 臨界區段自旋相同用意），
    // 逾量才進原服務登記等待；單處理器直接進服務。只讀字組，不設等待位元。
    mov rdx, qword ptr gs:[0x60]
    cmp dword ptr [rdx + 0xb8], 1
    jbe 1f
    mov r8d, 4000
4:
    pause
    cmp dword ptr [rcx + 8], -1
    jne 6f
    mov eax, -1
    xor edx, edx
    lock cmpxchg dword ptr [rcx + 8], edx
    je 5b
6:
    dec r8d
    jne 4b
1:
slow_end EnterCriticalSection

entry LeaveCriticalSection
    critical_loader_lock
    mov eax, dword ptr gs:[0x48]
    cmp qword ptr [rcx + 16], rax
    jne 1f
    mov edx, dword ptr [rcx + 12]
    cmp edx, 1
    jne 2f
    // 先清除擁有者再釋放字組：新持有者寫入的擁有者不會被覆寫。
    mov qword ptr [rcx + 16], 0
    mov dword ptr [rcx + 12], 0
    xor eax, eax
    mov edx, -1
    lock cmpxchg dword ptr [rcx + 8], edx
    jne 3f
    ret
3:
    // 有等待者：恢復擁有權，由原服務釋放並喚醒。
    mov eax, dword ptr gs:[0x48]
    mov qword ptr [rcx + 16], rax
    mov dword ptr [rcx + 12], 1
    jmp 1f
2:
    jb 1f
    dec edx
    mov dword ptr [rcx + 12], edx
    lock dec dword ptr [rcx + 8]
    ret
1:
slow_end LeaveCriticalSection

entry TryEnterCriticalSection
    critical_loader_lock
    mov eax, -1
    xor edx, edx
    lock cmpxchg dword ptr [rcx + 8], edx
    jne 2f
    mov eax, dword ptr gs:[0x48]
    mov qword ptr [rcx + 16], rax
    mov dword ptr [rcx + 12], 1
    mov eax, 1
    ret
2:
    cmp eax, 0x7fffffff
    je 1f
    mov edx, dword ptr gs:[0x48]
    cmp qword ptr [rcx + 16], rdx
    jne 4f
    mov edx, dword ptr [rcx + 12]
    cmp edx, 0x3ffffffe
    jae 1f
    inc edx
    mov dword ptr [rcx + 12], edx
    lock inc dword ptr [rcx + 8]
    mov eax, 1
    ret
4:
    // 由其他執行緒持有：與 Windows 相同直接回 FALSE，不進服務。
    xor eax, eax
    ret
1:
slow_end TryEnterCriticalSection
// FLS：服務層在 TEB+0x1840 維護目前執行緒（或其目前 fiber）的 128 個值，+0x400 為程序共用的使用中位元圖，
// 並以 TEB FlsData（gs:[0x17c8]）指向該區。所有寫入（Alloc／Free／SetValue、fiber 切換、執行緒結束）都由
// 服務層同步，常式只讀。未使用的索引與服務層相同：回 0，FlsGetValue 另設 LastError 87；指標為零時尾跳原服務。
.macro fls_read name, set_error
entry \name
    mov ecx, ecx
    mov rax, qword ptr gs:[0x17c8]
    test rax, rax
    je 1f
    cmp ecx, 128
    jae 2f
    mov edx, ecx
    shr edx, 6
    mov rdx, qword ptr [rax + rdx*8 + 0x400]
    bt rdx, rcx
    jnc 2f
    mov rax, qword ptr [rax + rcx*8]
.if \set_error
    mov dword ptr gs:[X64ARM_TLS_X64_ERROR_OFFSET], 0
.endif
    ret
2:
.if \set_error
    mov dword ptr gs:[X64ARM_TLS_X64_ERROR_OFFSET], 87
.endif
    xor eax, eax
    ret
1:
slow_end \name
.endm
fls_read FlsGetValue, 1
fls_read FlsGetValue2, 0
// SList（x64 SLIST_HEADER：低 64 位元 Depth:16／Sequence:48，高 64 位元 16 位元組對齊的第一項指標｜低 4 位元保留旗標），
// 與服務層 slist::（windows_slist.hpp）格式相同。每次更新序號加一並以 LOCK CMPXCHG16B 一次比較交換整個標頭（防 ABA），
// worker 上直接完成，不經協調者。CMPXCHG16B 以 RCX:RBX 為新值：RBX 暫存於 R11，不動堆疊。
entry InitializeSListHead
    mov qword ptr [rcx], 0
    mov qword ptr [rcx + 8], 0
    ret
end InitializeSListHead
entry QueryDepthSList
    movzx eax, word ptr [rcx]
    ret
end QueryDepthSList
// 回傳原本的第一項；entry->Next 每次重試都改寫成目前的第一項。
entry InterlockedPushEntrySList
    mov r10, rcx
    mov r9, rdx
    mov r11, rbx
    mov rax, qword ptr [r10]
    mov rdx, qword ptr [r10 + 8]
1:
    mov r8, rdx
    and r8, -16
    mov qword ptr [r9], r8
    lea rbx, [rax + 0x10000]
    and rbx, -65536
    lea ecx, [rax + 1]
    movzx ecx, cx
    or rbx, rcx
    mov rcx, rdx
    and ecx, 15
    or rcx, r9
    lock cmpxchg16b xmmword ptr [r10]
    jne 1b
    mov rbx, r11
    mov rax, r8
    ret
end InterlockedPushEntrySList
// 空串列回 NULL（不改標頭）。
entry InterlockedPopEntrySList
    mov r10, rcx
    mov r11, rbx
    mov rax, qword ptr [r10]
    mov rdx, qword ptr [r10 + 8]
1:
    mov r8, rdx
    and r8, -16
    je 2f
    mov r9, qword ptr [r8]
    lea rbx, [rax + 0x10000]
    and rbx, -65536
    lea ecx, [rax - 1]
    movzx ecx, cx
    or rbx, rcx
    mov rcx, rdx
    and ecx, 15
    or rcx, r9
    lock cmpxchg16b xmmword ptr [r10]
    jne 1b
    mov rbx, r11
    mov rax, r8
    ret
2:
    mov rbx, r11
    xor eax, eax
    ret
end InterlockedPopEntrySList
// 取走整串：回傳原本的第一項，深度歸零；已空時回 NULL。
entry InterlockedFlushSList
    mov r10, rcx
    mov r11, rbx
    mov rax, qword ptr [r10]
    mov rdx, qword ptr [r10 + 8]
1:
    mov r8, rdx
    and r8, -16
    je 2f
    lea rbx, [rax + 0x10000]
    and rbx, -65536
    mov rcx, rdx
    and ecx, 15
    lock cmpxchg16b xmmword ptr [r10]
    jne 1b
2:
    mov rbx, r11
    mov rax, r8
    ret
end InterlockedFlushSList
// D3D12 ID3D12Resource::GetGPUVirtualAddress：橋接物件 [this+8] 保存緩衝的 GPU 虛擬位址（紋理為 0），建立時寫入、
// 物件存續期間不變（見 windows_d3d12.cpp 的 make_resource）。直接返回，不經服務、不離開原生碼。
entry D3D12ResourceGetGPUVirtualAddress
    mov rax, qword ptr [rcx + 8]
    ret
end D3D12ResourceGetGPUVirtualAddress
// ID3D12DescriptorHeap::GetCPU／GPUDescriptorHandleForHeapStart（MSVC ABI：結構經 RDX 隱藏指標返回，RAX=RDX）：
// 橋接物件 [this+8]／[this+16] 保存建立時配置的 CPU／GPU 起點（非 shader-visible 堆積 GPU 為 0），存續期間不變。
entry D3D12HeapGetCPUDescriptorHandleForHeapStart
    mov rax, qword ptr [rcx + 8]
    mov qword ptr [rdx], rax
    mov rax, rdx
    ret
end D3D12HeapGetCPUDescriptorHandleForHeapStart
entry D3D12HeapGetGPUDescriptorHandleForHeapStart
    mov rax, qword ptr [rcx + 16]
    mov qword ptr [rdx], rax
    mov rax, rdx
    ret
end D3D12HeapGetGPUDescriptorHandleForHeapStart
// D3D11／D3D10／DXGI 橋接物件的 COM AddRef／Release：介面物件 [this+8] 指向該物件身分共用的 32 位元引用計數格
// （見 windows_d3d11.cpp 的 reference_count）。AddRef 只做原子加一並回傳新值；Release 在計數大於 1 時以 CAS 減一，
// 否則（最後一次或計數異常）尾跳 vtable 前一格保存的同家族 Release 服務，由服務在圖形鎖下銷毀或回報錯誤。
entry ComBridgeAddRef
    mov rdx, qword ptr [rcx + 8]
    mov eax, 1
    lock xadd dword ptr [rdx], eax
    inc eax
    ret
end ComBridgeAddRef
entry ComBridgeRelease
    mov rdx, qword ptr [rcx + 8]
    mov eax, dword ptr [rdx]
1:
    cmp eax, 1
    ja 2f
    mov rax, qword ptr [rcx]
    jmp qword ptr [rax - 8]
2:
    lea r8d, [rax - 1]
    lock cmpxchg dword ptr [rdx], r8d
    jne 1b
    mov eax, r8d
    ret
end ComBridgeRelease
// LONG 固定為 32 位元；Pointer 使用完整 64 位元。LOCK／隱含鎖定 XCHG
// 共用既有原子指令的完整記憶體順序、故障及執行緒中斷契約。
.macro interlocked_word name, operation
entry \name
    test cl, 3
    jne 1f
    .ifc \operation,increment
    mov eax, 1
    lock xadd dword ptr [rcx], eax
    inc eax
    .endif
    .ifc \operation,decrement
    mov eax, -1
    lock xadd dword ptr [rcx], eax
    dec eax
    .endif
    .ifc \operation,add
    mov eax, edx
    lock xadd dword ptr [rcx], eax
    .endif
    .ifc \operation,exchange
    mov eax, edx
    xchg dword ptr [rcx], eax
    .endif
    .ifc \operation,compare
    mov eax, r8d
    lock cmpxchg dword ptr [rcx], edx
    .endif
    ret
1:
slow_end \name
.endm
interlocked_word InterlockedExchange, exchange
interlocked_word InterlockedIncrement, increment
interlocked_word InterlockedDecrement, decrement
interlocked_word InterlockedExchangeAdd, add
interlocked_word InterlockedCompareExchange, compare

entry InterlockedExchangePointer
    test cl, 7
    jne 1f
    mov rax, rdx
    xchg qword ptr [rcx], rax
    ret
1:
slow_end InterlockedExchangePointer

entry InterlockedCompareExchangePointer
    test cl, 7
    jne 1f
    mov rax, r8
    lock cmpxchg qword ptr [rcx], rdx
    ret
1:
slow_end InterlockedCompareExchangePointer

// __chkstk（kernel32／ntdll）：函式序言以任意 8 位元組對齊呼叫，不能進入要求 RSP 為 16n+8 的主機服務。
// RAX 為要配置的位元組數；自呼叫端堆疊（RSP+8）所在頁往下逐頁讀取到目標所在頁，未映射範圍照常故障。
// 與 MSVC 相同只使用 R10／R11 並保留 RAX；配置大小超過堆疊位址時不探測（由之後的 SUB RSP 自行故障）。
entry __chkstk
    lea r10, [rsp+8]
    mov r11, r10
    sub r11, rax
    jb 2f
    and r10, -4096
1:
    cmp r10, r11
    jbe 2f
    sub r10, 4096
    test byte ptr [r10], 0
    jmp 1b
2:
    ret
end __chkstk
// 本程序版本直接讀目前 TEB／PEB 與 PE 標頭；遠端 PID、損毀標頭由宿主完整流程承接。
entry GetProcessVersion
    test ecx, ecx
    jz 1f
    cmp ecx, dword ptr gs:[0x40]
    jne 9f
1:
    mov rdx, qword ptr gs:[0x60]
    mov rdx, qword ptr [rdx+0x10]
    cmp word ptr [rdx], 0x5a4d
    jne 9f
    mov eax, dword ptr [rdx+60]
    cmp eax, -77
    ja 9f
    add rdx, rax
    cmp dword ptr [rdx], 0x4550
    jne 9f
    cmp word ptr [rdx+24], 0x20b
    jne 9f
    movzx eax, word ptr [rdx+72]
    shl eax, 16
    movzx edx, word ptr [rdx+74]
    or eax, edx
    ret
9:
slow_end GetProcessVersion
// 共用 EH／fiber 邊界發布的目前計數；僅使用 volatile RAX 與旗標。
entry CxxUncaughtCount
    mov eax, dword ptr gs:[X64ARM_CXX_X64_UNCAUGHT_OFFSET]
    ret
end CxxUncaughtCount
entry CxxUncaughtAny
    mov eax, dword ptr gs:[X64ARM_CXX_X64_UNCAUGHT_OFFSET]
    test eax, eax
    setne al
    movzx eax, al
    ret
end CxxUncaughtAny
// 與 x86／宿主查詢相同，讀取 Win64 PEB 的 CurrentDirectory，無宿主呼叫。
entry CrtGetDrive
    mov rcx, qword ptr gs:[0x60]
    mov rcx, qword ptr [rcx+0x20]
    test rcx, rcx
    jz 1f
    cmp word ptr [rcx+0x38], 4
    jb 1f
    mov rdx, qword ptr [rcx+0x40]
    test rdx, rdx
    jz 1f
    test byte ptr [rcx+8], 1
    jnz 2f
    add rdx, rcx
2:
    cmp word ptr [rdx+2], 58
    jne 1f
    movzx eax, word ptr [rdx]
    or eax, 32
    sub eax, 97
    cmp eax, 25
    ja 1f
    inc eax
    ret
1:
    xor eax, eax
    ret
end CrtGetDrive

// Winsock 純數值轉換；保留 LastError，無指標存取或宿主服務。
entry WinsockSwap16
    mov eax, ecx
    rol ax, 8
    movzx eax, ax
    ret
end WinsockSwap16
entry WinsockSwap32
    mov eax, ecx
    bswap eax
    ret
end WinsockSwap32

// 不可變 NLS 目錄的群組位元圖由同一份宿主資料修補，查詢不離開常駐執行。
entry IsValidLanguageGroup
    sub ecx, 1
    cmp ecx, 16
    ja 1f
    sub edx, 1
    cmp edx, 1
    ja 1f
    .byte 0xb8 // mov eax, imm32
Llanguage_groups_patch:
    .long 0
    bt eax, ecx
    setc al
    movzx eax, al
    ret
1:
    xor eax, eax
    ret
end IsValidLanguageGroup
.set _guest_language_groups_patch, Llanguage_groups_patch - _guest_IsValidLanguageGroup
.macro gui_timing name, offset, setter
entry \name
    .byte 0x48,0xb8 // movabs rax, 客體時序指標槽
Lgui_timing_\name:
    .quad 0
    mov rax, qword ptr [rax]
.if \setter
.if \offset == 0
    mov edx, 500
    test ecx, ecx
    cmovz ecx, edx
    mov edx, 5000
    cmp ecx, edx
    cmova ecx, edx
.endif
    mov dword ptr [rax+\offset], ecx
    mov eax, 1
.else
    mov eax, dword ptr [rax+\offset]
.endif
    ret
end \name
.globl _guest_\name\()_gui_timing_patch
.set _guest_\name\()_gui_timing_patch, Lgui_timing_\name - _guest_\name
.endm
gui_timing GetDoubleClickTime, 0, 0
gui_timing GetCaretBlinkTime, 4, 0
gui_timing SetDoubleClickTime, 0, 1
gui_timing SetCaretBlinkTime, 4, 1
// 空批次直接讀寫目前 TEB；有待發布的像素則保留 ABI 尾跳共用 GDI provider。
entry GdiFlush
    cmp dword ptr gs:[X64ARM_GDI_BATCH_X64_OFFSET+X64ARM_GDI_BATCH_PENDING_OFFSET], 0
    jne 1f
    mov eax, 1
    ret
1:
slow_end GdiFlush

entry GdiGetBatchLimit
    cmp dword ptr gs:[X64ARM_GDI_BATCH_X64_OFFSET+X64ARM_GDI_BATCH_PENDING_OFFSET], 0
    jne 1f
    mov eax, dword ptr gs:[X64ARM_GDI_BATCH_X64_OFFSET]
    mov edx, X64ARM_GDI_BATCH_DEFAULT
    test eax, eax
    cmovz eax, edx
    ret
1:
slow_end GdiGetBatchLimit

entry GdiSetBatchLimit
    cmp dword ptr gs:[X64ARM_GDI_BATCH_X64_OFFSET+X64ARM_GDI_BATCH_PENDING_OFFSET], 0
    jne 1f
    mov eax, dword ptr gs:[X64ARM_GDI_BATCH_X64_OFFSET]
    mov edx, X64ARM_GDI_BATCH_DEFAULT
    test eax, eax
    cmovz eax, edx
    test ecx, ecx
    cmovz ecx, edx
    mov dword ptr gs:[X64ARM_GDI_BATCH_X64_OFFSET], ecx
    ret
1:
slow_end GdiSetBatchLimit

// D3D12 根描述元及描述元表：清單由應用程式同步錄製。在清單自己的客體緩衝內依序追加，
// 確認有空間後才寫入，最後更新筆數。緩衝滿、停用或已關閉時，保留參數尾跳原服務。
.macro d3d12_root_binding name, opcode
entry \name
    mov r10, qword ptr [rcx + X64ARM_D3D12_LIST_ROOT_BUFFER]
    test r10, r10
    je 1f
    mov eax, dword ptr [r10]
    cmp eax, X64ARM_D3D12_ROOT_BUFFER_CAPACITY
    jae 1f
    lea r11d, [rax + 1]
    shl eax, X64ARM_D3D12_ROOT_BINDING_SHIFT
    mov dword ptr [r10 + rax + X64ARM_D3D12_ROOT_BUFFER_HEADER], \opcode
    mov dword ptr [r10 + rax + X64ARM_D3D12_ROOT_BUFFER_HEADER + 4], edx
    mov qword ptr [r10 + rax + X64ARM_D3D12_ROOT_BUFFER_HEADER + 8], r8
    mov dword ptr [r10], r11d
    xor eax, eax
    ret
1:
slow_end \name
.endm
#define D3D12_ROOT_ENTRY(method, opcode) d3d12_root_binding D3D12List##method, opcode ;
X64ARM_D3D12_ROOT_BINDING_METHODS(D3D12_ROOT_ENTRY)
#undef D3D12_ROOT_ENTRY

// 直接繪製與根描述元共用清單緩衝。堆疊跨頁或不符合一般呼叫框架時交回宿主核對。
.macro d3d12_draw name, opcode, indexed
entry \name
    mov eax, esp
    and eax, 4095
    cmp eax, 4096 - 48 - 8 * \indexed
    ja 1f
    and eax, 15
    cmp eax, 8
    jne 1f
    mov r10, qword ptr [rcx + X64ARM_D3D12_LIST_ROOT_BUFFER]
    test r10, r10
    je 1f
    mov eax, dword ptr [r10]
    cmp eax, X64ARM_D3D12_ROOT_BUFFER_CAPACITY - 2
    ja 1f
    mov r11d, eax
    shl r11, X64ARM_D3D12_ROOT_BINDING_SHIFT
    add r11, r10
    mov rax, qword ptr [rsp + 40]
.if \indexed
    mov rcx, qword ptr [rsp + 48]
.endif
    mov dword ptr [r11 + X64ARM_D3D12_ROOT_BUFFER_HEADER], \opcode
    mov dword ptr [r11 + X64ARM_D3D12_ROOT_BUFFER_HEADER + 4], edx
    mov dword ptr [r11 + X64ARM_D3D12_ROOT_BUFFER_HEADER + 8], r8d
    mov dword ptr [r11 + X64ARM_D3D12_ROOT_BUFFER_HEADER + 12], r9d
    mov dword ptr [r11 + X64ARM_D3D12_ROOT_BUFFER_HEADER + 16], eax
.if \indexed
    mov dword ptr [r11 + X64ARM_D3D12_ROOT_BUFFER_HEADER + 20], ecx
.endif
    mov eax, dword ptr [r10]
    lea eax, [rax + 2]
    mov dword ptr [r10], eax
    xor eax, eax
    ret
1:
slow_end \name
.endm
d3d12_draw D3D12ListDrawInstanced, X64ARM_D3D12_DRAW_INSTANCED, 0
d3d12_draw D3D12ListDrawIndexedInstanced, X64ARM_D3D12_DRAW_INDEXED_INSTANCED, 1

// 將檢視值在呼叫當下複製到清單。多筆、空檢視及滿緩衝仍交回完整錄製。
.macro d3d12_input name, opcode, view, vertex
entry \name
.if \vertex
    cmp r8d, 1
    jne 1f
.endif
    test \view, \view
    je 1f
    mov r10, qword ptr [rcx + X64ARM_D3D12_LIST_ROOT_BUFFER]
    test r10, r10
    je 1f
    mov eax, dword ptr [r10]
    cmp eax, X64ARM_D3D12_ROOT_BUFFER_CAPACITY - 2
    ja 1f
    mov r11d, eax
    shl r11, X64ARM_D3D12_ROOT_BINDING_SHIFT
    add r11, r10
    mov rax, qword ptr [\view]
    mov rcx, qword ptr [\view + 8]
    mov dword ptr [r11 + X64ARM_D3D12_ROOT_BUFFER_HEADER], \opcode
.if \vertex
    mov dword ptr [r11 + X64ARM_D3D12_ROOT_BUFFER_HEADER + 4], edx
.endif
    mov qword ptr [r11 + X64ARM_D3D12_ROOT_BUFFER_HEADER + 8], rax
    mov qword ptr [r11 + X64ARM_D3D12_ROOT_BUFFER_HEADER + 16], rcx
    mov eax, dword ptr [r10]
    lea eax, [rax + 2]
    mov dword ptr [r10], eax
    xor eax, eax
    ret
1:
slow_end \name
.endm
d3d12_input D3D12ListIASetIndexBuffer, X64ARM_D3D12_IA_INDEX_BUFFER, rdx, 0
d3d12_input D3D12ListIASetVertexBuffers, X64ARM_D3D12_IA_VERTEX_BUFFER, r9, 1

// 第二頁獨立映射。保留 Win64 呼叫框架，慢路徑仍能讀到原始參數。
.p2align 12, 0xcc
.macro descriptor_offset handle, low, slow
    mov rax, \handle
    sub rax, r11
    cmp rax, X64ARM_D3D12_DESCRIPTOR_ARENA_HANDLES
    jae \slow
    test \low, X64ARM_D3D12_DESCRIPTOR_STRIDE - 1
    jnz \slow
    shr rax, X64ARM_D3D12_DESCRIPTOR_WORD_SHIFT
.endm

entry D3D12DeviceCreateConstantBufferView
    test rcx, rcx
    jz 1f
    test rdx, rdx
    jz 1f
    mov r10, qword ptr [rcx + X64ARM_D3D12_DESCRIPTOR_DEVICE_ARENA]
    test r10, r10
    jz 1f
    movabs r11, X64ARM_D3D12_DESCRIPTOR_ARENA_HANDLE
    descriptor_offset r8, r8b, 1f
    add r10, rax
    mov rax, qword ptr [r10]
    test rax, rax
    jz 1f
    mov r9d, edx
    and r9d, 4095
    cmp r9d, 4084
    ja 1f
    mov r11, qword ptr [rdx]
    test r11, r11
    js 1f
    test r11b, r11b
    jnz 1f
    mov r9d, dword ptr [rdx + 8]
    sub r9d, 256
    cmp r9d, 65280
    ja 1f
    test r9b, r9b
    jnz 1f
    shr r9d, 8
    or r11, r9
    bts r11, 63
    lock cmpxchg qword ptr [r10], r11
    jne 1f
    xor eax, eax
    ret
1:
slow_end D3D12DeviceCreateConstantBufferView

entry D3D12DeviceCopyDescriptorsSimple
    test rcx, rcx
    jz 1f
    cmp edx, 1
    jne 1f
    mov r10, qword ptr [rcx + X64ARM_D3D12_DESCRIPTOR_DEVICE_ARENA]
    test r10, r10
    jz 1f
    movabs r11, X64ARM_D3D12_DESCRIPTOR_ARENA_HANDLE
    descriptor_offset r8, r8b, 1f
    add r10, rax
    descriptor_offset r9, r9b, 1f
    mov r11, qword ptr [rcx + X64ARM_D3D12_DESCRIPTOR_DEVICE_ARENA]
    mov r11, qword ptr [r11 + rax]
    test r11, r11
    jz 1f
    mov rax, qword ptr [r10]
    test rax, rax
    jz 1f
    lock cmpxchg qword ptr [r10], r11
    jne 1f
    xor eax, eax
    ret
1:
slow_end D3D12DeviceCopyDescriptorsSimple

.globl _guest_thread_end
_guest_thread_end:
