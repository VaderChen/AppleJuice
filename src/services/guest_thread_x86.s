// Win32 執行緒區域常式：每次讀取當前 FS 的 TEB；stdcall 參數由被呼叫端清除。
// 只使用 volatile EAX／ECX／EDX 與旗標，保留非 volatile 暫存器及浮點狀態。
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
    mov eax, dword ptr fs:[X64ARM_TLS_X86_ERROR_OFFSET]
    ret
end GetLastError

entry SetLastError
    mov eax, dword ptr [esp+4]
    mov dword ptr fs:[X64ARM_TLS_X86_ERROR_OFFSET], eax
    xor eax, eax
    ret 4
end SetLastError

.macro tls_read name, clear_error
entry \name
    mov ecx, dword ptr [esp+4]
    cmp ecx, X64ARM_TLS_SLOTS
    jae Linvalid\@
    cmp ecx, X64ARM_TLS_INLINE_SLOTS
    jae Lexpanded\@
    mov eax, dword ptr fs:[ecx*4+X64ARM_TLS_X86_INLINE_OFFSET]
    jmp Lvalue\@
Lexpanded\@:
    // 與服務層共用 0x2000 + (index - 64) * 4 的擴充槽配置。
    mov eax, dword ptr fs:[ecx*4+X64ARM_TLS_EXPANSION_OFFSET-X64ARM_TLS_INLINE_SLOTS*4]
Lvalue\@:
.if \clear_error
    mov dword ptr fs:[X64ARM_TLS_X86_ERROR_OFFSET], 0
.endif
    ret 4
Linvalid\@:
.if \clear_error
    mov dword ptr fs:[X64ARM_TLS_X86_ERROR_OFFSET], 87
.endif
    xor eax, eax
    ret 4
end \name
.endm
tls_read TlsGetValue, 1
tls_read TlsGetValue2, 0

entry GetCurrentProcess
    mov eax, -1
    ret
end GetCurrentProcess
entry GetCurrentThread
    mov eax, -2
    ret
end GetCurrentThread
entry GetCurrentThreadId
    mov eax, dword ptr fs:[0x24]
    ret
end GetCurrentThreadId

// 低 64 槽依 Windows 的最小驗證語義直接寫入；擴充槽須仍在配置中。
// 位元圖是程序共用頁，只由 Alloc／Free 的協調邊界發布，新執行緒共用同一份。
entry TlsSetValue
    mov ecx, dword ptr [esp+4]
    cmp ecx, X64ARM_TLS_INLINE_SLOTS
    jb Lset_small
    cmp ecx, X64ARM_TLS_SLOTS
    jae Lset_invalid
    mov eax, ecx
    shr eax, 5
    .byte 0xba // mov edx, imm32；初始化時填入配置位元圖位址。
Ltls_bitmap_patch:
    .long 0
    mov edx, dword ptr [edx+eax*4]
    bt edx, ecx
    jnc Lset_invalid
    mov edx, dword ptr [esp+8]
    mov dword ptr fs:[ecx*4+X64ARM_TLS_EXPANSION_OFFSET-X64ARM_TLS_INLINE_SLOTS*4], edx
    jmp Lset_success
Lset_small:
    mov edx, dword ptr [esp+8]
    mov dword ptr fs:[ecx*4+X64ARM_TLS_X86_INLINE_OFFSET], edx
Lset_success:
    mov eax, 1
    ret 8
Lset_invalid:
    mov dword ptr fs:[X64ARM_TLS_X86_ERROR_OFFSET], 87
    xor eax, eax
    ret 8
end TlsSetValue
// 絕對差值不切開 Mach-O code atom，跨此位置的分支仍能在組譯時完全解析。
.set _guest_tls_bitmap_patch, Ltls_bitmap_patch - _guest_TlsSetValue

// 與 Win64 常式及 provider 共用鎖字組契約：bit0 獨占、bit1 等待者、bit2 起為讀者數。
// 原生 CAS 成功直接 stdcall 返回；競爭、等待者及溢位尾跳同名完整服務。
// 慢路徑保留原始堆疊與引數，只改 volatile 暫存器；立即值由共用初始化流程修補。
.macro slow_end name
    mov eax, 0
    jmp eax
end \name
.endm

entry AcquireSRWLockExclusive
    mov ecx, dword ptr [esp+4]
    xor eax, eax
    mov edx, 1
    lock cmpxchg dword ptr [ecx], edx
    jne 1f
    ret 4
1:
slow_end AcquireSRWLockExclusive

entry ReleaseSRWLockExclusive
    mov ecx, dword ptr [esp+4]
    mov eax, 1
    xor edx, edx
    lock cmpxchg dword ptr [ecx], edx
    jne 1f
    ret 4
1:
slow_end ReleaseSRWLockExclusive

entry AcquireSRWLockShared
    mov ecx, dword ptr [esp+4]
    mov eax, dword ptr [ecx]
    test al, 3
    jne 1f
    cmp eax, -4
    je 1f
    lea edx, [eax+4]
    lock cmpxchg dword ptr [ecx], edx
    jne 1f
    ret 4
1:
slow_end AcquireSRWLockShared

entry ReleaseSRWLockShared
    mov ecx, dword ptr [esp+4]
    mov eax, dword ptr [ecx]
    test al, 3
    jne 1f
    cmp eax, 4
    jb 1f
    lea edx, [eax-4]
    lock cmpxchg dword ptr [ecx], edx
    jne 1f
    ret 4
1:
slow_end ReleaseSRWLockShared

entry TryAcquireSRWLockExclusive
    mov ecx, dword ptr [esp+4]
    xor eax, eax
    mov edx, 1
    lock cmpxchg dword ptr [ecx], edx
    jne 1f
    mov eax, 1
    ret 4
1:
slow_end TryAcquireSRWLockExclusive

entry TryAcquireSRWLockShared
    mov ecx, dword ptr [esp+4]
    mov eax, dword ptr [ecx]
    test al, 3
    jne 1f
    cmp eax, -4
    je 1f
    lea edx, [eax+4]
    lock cmpxchg dword ptr [ecx], edx
    jne 1f
    mov eax, 1
    ret 4
1:
slow_end TryAcquireSRWLockShared
.globl _guest_thread_end
_guest_thread_end:
