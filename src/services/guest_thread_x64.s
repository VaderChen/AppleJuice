// Windows x64 執行緒區域常式。資料始終讀寫目前 GS 指向的 TEB，不快取執行緒值。
// 只使用 volatile GPR；不變更 XMM、非 volatile GPR 或 RSP（RET 除外）。
// TLS 的 64 個內建槽與 1024 個擴充槽沿用 WindowsServices 的既有配置。
#include "guest_tls_layout.h"
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
.globl _guest_thread_end
_guest_thread_end:
