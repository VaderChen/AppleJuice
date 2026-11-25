// Win32 執行緒區域常式：每次讀取當前 FS 的 TEB；stdcall 參數由被呼叫端清除。
// 只使用 volatile EAX／ECX／EDX 與旗標，保留非 volatile 暫存器及浮點狀態。
#include "guest_tls_layout.h"
#include "gdi_batch_layout.h"
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
// Interlocked 純量家族沿同一 LOCK 原子指令契約編譯為 resident AOT。
// 自然對齊以外的呼叫尾跳原 provider；不改 LastError、非 volatile 暫存器及原始呼叫框架。
.macro interlocked_word name, operation, arguments
entry \name
    mov ecx, dword ptr [esp+4]
    test cl, 3
    jne 1f
    .ifc \operation,increment
    mov eax, 1
    lock xadd dword ptr [ecx], eax
    inc eax
    .endif
    .ifc \operation,decrement
    mov eax, -1
    lock xadd dword ptr [ecx], eax
    dec eax
    .endif
    .ifc \operation,add
    mov eax, dword ptr [esp+8]
    lock xadd dword ptr [ecx], eax
    .endif
    .ifc \operation,exchange
    mov eax, dword ptr [esp+8]
    xchg dword ptr [ecx], eax
    .endif
    .ifc \operation,compare
    mov edx, dword ptr [esp+8]
    mov eax, dword ptr [esp+12]
    lock cmpxchg dword ptr [ecx], edx
    .endif
    ret \arguments*4
1:
slow_end \name
.endm
interlocked_word InterlockedExchange, exchange, 2
interlocked_word InterlockedIncrement, increment, 1
interlocked_word InterlockedDecrement, decrement, 1
interlocked_word InterlockedExchangeAdd, add, 2
interlocked_word InterlockedCompareExchange, compare, 3
interlocked_word InterlockedExchangePointer, exchange, 2
interlocked_word InterlockedCompareExchangePointer, compare, 3
// D3D11／D3D10／DXGI 橋接物件的 COM AddRef／Release（stdcall，this 在 [esp+4]）：介面物件 [this+4] 指向身分共用的
// 32 位元引用計數格。與 x64 版相同：AddRef 原子加一；Release 計數大於 1 時以 CAS 減一，否則保留原堆疊尾跳 vtable 前一格的 Release 服務。
entry ComBridgeAddRef
    mov ecx, dword ptr [esp+4]
    mov edx, dword ptr [ecx+4]
    mov eax, 1
    lock xadd dword ptr [edx], eax
    inc eax
    ret 4
end ComBridgeAddRef
entry ComBridgeRelease
    mov ecx, dword ptr [esp+4]
    mov edx, dword ptr [ecx+4]
    mov eax, dword ptr [edx]
1:
    cmp eax, 1
    ja 2f
    mov eax, dword ptr [esp+4]
    mov eax, dword ptr [eax]
    jmp dword ptr [eax-4]
2:
    lea ecx, [eax-1]
    lock cmpxchg dword ptr [edx], ecx
    jne 1b
    mov eax, ecx
    ret 4
end ComBridgeRelease
// stdcall ABI 與 32-bit TEB；常用的本程序查詢留在 resident AOT，其他 PID 回到共用 provider。
entry GetProcessVersion
    mov eax, dword ptr [esp+4]
    test eax, eax
    jz 1f
    cmp eax, dword ptr fs:[0x20]
    jne 9f
1:
    mov edx, dword ptr fs:[0x30]
    mov edx, dword ptr [edx+8]
    cmp word ptr [edx], 0x5a4d
    jne 9f
    mov eax, dword ptr [edx+60]
    cmp eax, -77
    ja 9f
    add edx, eax
    jc 9f
    cmp dword ptr [edx], 0x4550
    jne 9f
    cmp word ptr [edx+24], 0x10b
    jne 9f
    movzx eax, word ptr [edx+72]
    shl eax, 16
    movzx edx, word ptr [edx+74]
    or eax, edx
    ret 4
9:
slow_end GetProcessVersion
// cdecl 零參數查詢；計數由共用 EH／fiber 邊界發布，保留 LastError。
entry CxxUncaughtCount
    mov eax, dword ptr fs:[X64ARM_CXX_X86_UNCAUGHT_OFFSET]
    ret
end CxxUncaughtCount
entry CxxUncaughtAny
    mov eax, dword ptr fs:[X64ARM_CXX_X86_UNCAUGHT_OFFSET]
    test eax, eax
    setne al
    movzx eax, al
    ret
end CxxUncaughtAny
// CRT 的目前磁碟由共用 PEB CurrentDirectory 取得；cdecl、保留 LastError 與浮點狀態。
entry CrtGetDrive
    mov ecx, dword ptr fs:[0x30]
    mov ecx, dword ptr [ecx+0x10]
    test ecx, ecx
    jz 1f
    cmp word ptr [ecx+0x24], 4
    jb 1f
    mov edx, dword ptr [ecx+0x28]
    test edx, edx
    jz 1f
    test byte ptr [ecx+8], 1
    jnz 2f
    add edx, ecx
2:
    cmp word ptr [edx+2], 58
    jne 1f
    movzx eax, word ptr [edx]
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
    mov eax, dword ptr [esp+4]
    rol ax, 8
    movzx eax, ax
    ret 4
end WinsockSwap16
entry WinsockSwap32
    mov eax, dword ptr [esp+4]
    bswap eax
    ret 4
end WinsockSwap32

// DWORD 參數與不可變 NLS 目錄沿用宿主查詢；保留 stdcall 堆疊及 LastError。
entry IsValidLanguageGroup
    mov ecx, dword ptr [esp+4]
    sub ecx, 1
    cmp ecx, 16
    ja 1f
    mov edx, dword ptr [esp+8]
    sub edx, 1
    cmp edx, 1
    ja 1f
    .byte 0xb8 // mov eax, imm32
Llanguage_groups_patch:
    .long 0
    bt eax, ecx
    setc al
    movzx eax, al
    ret 8
1:
    xor eax, eax
    ret 8
end IsValidLanguageGroup
.set _guest_language_groups_patch, Llanguage_groups_patch - _guest_IsValidLanguageGroup
.macro gui_timing name, offset, setter
entry \name
    .byte 0xba // mov edx, 客體時序指標槽
Lgui_timing_\name:
    .long 0
    mov edx, dword ptr [edx]
.if \setter
    mov eax, dword ptr [esp+4]
.if \offset == 0
    mov ecx, 500
    test eax, eax
    cmovz eax, ecx
    mov ecx, 5000
    cmp eax, ecx
    cmova eax, ecx
.endif
    mov dword ptr [edx+\offset], eax
    mov eax, 1
    ret 4
.else
    mov eax, dword ptr [edx+\offset]
    ret
.endif
end \name
.globl _guest_\name\()_gui_timing_patch
.set _guest_\name\()_gui_timing_patch, Lgui_timing_\name - _guest_\name
.endm
gui_timing GetDoubleClickTime, 0, 0
gui_timing GetCaretBlinkTime, 4, 0
gui_timing SetDoubleClickTime, 0, 1
gui_timing SetCaretBlinkTime, 4, 1
#include "guest_shell_counter.inc"
// 與 x64／宿主服務共用狀態，慢路徑保留原始 stdcall 堆疊。
entry GdiFlush
    cmp dword ptr fs:[X64ARM_GDI_BATCH_X86_OFFSET+X64ARM_GDI_BATCH_PENDING_OFFSET], 0
    jne 1f
    mov eax, 1
    ret
1:
slow_end GdiFlush

entry GdiGetBatchLimit
    cmp dword ptr fs:[X64ARM_GDI_BATCH_X86_OFFSET+X64ARM_GDI_BATCH_PENDING_OFFSET], 0
    jne 1f
    mov eax, dword ptr fs:[X64ARM_GDI_BATCH_X86_OFFSET]
    mov edx, X64ARM_GDI_BATCH_DEFAULT
    test eax, eax
    cmovz eax, edx
    ret
1:
slow_end GdiGetBatchLimit

entry GdiSetBatchLimit
    cmp dword ptr fs:[X64ARM_GDI_BATCH_X86_OFFSET+X64ARM_GDI_BATCH_PENDING_OFFSET], 0
    jne 1f
    mov eax, dword ptr fs:[X64ARM_GDI_BATCH_X86_OFFSET]
    mov ecx, dword ptr [esp+4]
    mov edx, X64ARM_GDI_BATCH_DEFAULT
    test eax, eax
    cmovz eax, edx
    test ecx, ecx
    cmovz ecx, edx
    mov dword ptr fs:[X64ARM_GDI_BATCH_X86_OFFSET], ecx
    ret 4
1:
slow_end GdiSetBatchLimit
// 臨界區段（字組協定與服務層及 guest_thread_x64.s 相同，見 windows_threads.cpp）：PE32 的 LockCount 在 +4、RecursionCount 在 +8、
// OwningThread 在 +12，擁有者為 TEB ClientId.UniqueThread（fs:[0x24]）。LockCount＝-1 未持有；持有時低 30 位元為遞迴次數−1、
// bit30 表示宿主有等待者；0x7fffffff 為未持有但有等待者。只在無等待者時以 lock cmpxchg 取得／完全釋放，遞迴由持有者遞增／遞減。
// loader lock（PEB+0x800）、有等待者的釋放、遞迴上限、非持有者解鎖與未持有但有等待者一律以原參數尾跳原服務（堆疊未動）。
// 只使用 EAX／ECX／EDX；多處理器自旋借用 ESI，離開前還原。
.macro critical_x86_loader_lock
    mov ecx, dword ptr [esp+4]
    mov eax, dword ptr fs:[0x30]
    add eax, 0x800
    cmp ecx, eax
    je 1f
.endm

entry EnterCriticalSection
    critical_x86_loader_lock
    mov eax, -1
    xor edx, edx
    lock cmpxchg dword ptr [ecx + 4], edx
    jne 2f
5:
    mov eax, dword ptr fs:[0x24]
    mov dword ptr [ecx + 12], eax
    mov dword ptr [ecx + 8], 1
    ret 4
2:
    // 未持有但有等待者：搶得並保留等待者位元（不公平鎖，與 x64 版相同）。
    cmp eax, 0x7fffffff
    jne 7f
    mov edx, 0x40000000
    lock cmpxchg dword ptr [ecx + 4], edx
    je 5b
7:
    mov edx, dword ptr fs:[0x24]
    cmp dword ptr [ecx + 12], edx
    jne 3f
    mov edx, dword ptr [ecx + 8]
    cmp edx, 0x3ffffffe
    jae 1f
    inc edx
    mov dword ptr [ecx + 8], edx
    lock inc dword ptr [ecx + 4]
    ret 4
3:
    // 他人持有：多處理器（PEB32 NumberOfProcessors>1）時先有界自旋等字組回到未持有（-1 或 0x7fffffff），逾量才進原服務登記等待。
    mov edx, dword ptr fs:[0x30]
    cmp dword ptr [edx + 0x64], 1
    jbe 1f
    push esi
    mov esi, 4000
4:
    pause
    mov eax, dword ptr [ecx + 4]
    cmp eax, -1
    je 8f
    cmp eax, 0x7fffffff
    jne 6f
    mov edx, 0x40000000
    lock cmpxchg dword ptr [ecx + 4], edx
    jne 6f
    pop esi
    jmp 5b
8:
    xor edx, edx
    lock cmpxchg dword ptr [ecx + 4], edx
    jne 6f
    pop esi
    jmp 5b
6:
    dec esi
    jne 4b
    pop esi
1:
slow_end EnterCriticalSection

entry LeaveCriticalSection
    critical_x86_loader_lock
    mov eax, dword ptr fs:[0x24]
    cmp dword ptr [ecx + 12], eax
    jne 1f
    mov edx, dword ptr [ecx + 8]
    cmp edx, 1
    jne 2f
    // 先清除擁有者再釋放字組：新持有者寫入的擁有者不會被覆寫。
    mov dword ptr [ecx + 12], 0
    mov dword ptr [ecx + 8], 0
    xor eax, eax
    mov edx, -1
    lock cmpxchg dword ptr [ecx + 4], edx
    jne 3f
    ret 4
3:
    // 有等待者：恢復擁有權，由原服務釋放並喚醒。
    mov eax, dword ptr fs:[0x24]
    mov dword ptr [ecx + 12], eax
    mov dword ptr [ecx + 8], 1
    jmp 1f
2:
    jb 1f
    dec edx
    mov dword ptr [ecx + 8], edx
    lock dec dword ptr [ecx + 4]
    ret 4
1:
slow_end LeaveCriticalSection

entry TryEnterCriticalSection
    critical_x86_loader_lock
    mov eax, -1
    xor edx, edx
    lock cmpxchg dword ptr [ecx + 4], edx
    jne 2f
    mov eax, dword ptr fs:[0x24]
    mov dword ptr [ecx + 12], eax
    mov dword ptr [ecx + 8], 1
    mov eax, 1
    ret 4
2:
    cmp eax, 0x7fffffff
    jne 7f
    mov edx, 0x40000000
    lock cmpxchg dword ptr [ecx + 4], edx
    jne 4f
    mov eax, dword ptr fs:[0x24]
    mov dword ptr [ecx + 12], eax
    mov dword ptr [ecx + 8], 1
    mov eax, 1
    ret 4
7:
    mov edx, dword ptr fs:[0x24]
    cmp dword ptr [ecx + 12], edx
    jne 4f
    mov edx, dword ptr [ecx + 8]
    cmp edx, 0x3ffffffe
    jae 1f
    inc edx
    mov dword ptr [ecx + 8], edx
    lock inc dword ptr [ecx + 4]
    mov eax, 1
    ret 4
4:
    // 由其他執行緒持有：與 Windows 相同直接回 FALSE，不進服務。
    xor eax, eax
    ret 4
1:
slow_end TryEnterCriticalSection

.macro copy_nx name, unit
entry \name
    mov eax, dword ptr [esp+4]
    mov edx, dword ptr [esp+8]
    mov ecx, dword ptr [esp+12]
    test eax, eax
    jz 4f
    test edx, edx
    jz 4f
    test ecx, ecx
    jle 4f
    push ebx
1:
    dec ecx
    jz 3f
.if \unit == 1
    movzx ebx, byte ptr [edx]
.else
    movzx ebx, word ptr [edx]
.endif
    test ebx, ebx
    jz 3f
.if \unit == 1
    mov byte ptr [eax], bl
.else
    mov word ptr [eax], bx
.endif
    add edx, \unit
    add eax, \unit
    jmp 1b
3:
.if \unit == 1
    mov byte ptr [eax], 0
.else
    mov word ptr [eax], 0
.endif
    pop ebx
4:
    ret 12
end \name
.endm

copy_nx StrCpyNXA, 1
copy_nx StrCpyNXW, 2

#include "guest_shell_security.inc"
#include "guest_shell_copy_encoding.inc"


// ECX／EDX 與 stdcall 兩種入口共用 CMPXCHG8B 迴圈；慢路徑保留原始參數。
.macro push_list name, fast
entry \name
.if !\fast
    mov ecx, [esp + 4]
    mov edx, [esp + 8]
.endif
    test ecx, ecx
    jz 2f
    test ecx, 7
    jnz 2f
    test edx, edx
    jz 2f
    test edx, 3
    jnz 2f
    mov eax, [esp + 12 - 8 * \fast]
    test eax, eax
    jz 2f
    test eax, 3
    jnz 2f
    push esi
    push edi
    push ebx
    push ebp
    mov esi, ecx
    mov ebx, edx
    mov edi, eax
    mov ebp, [esp + 32 - 8 * \fast]
    mov eax, [esi]
    mov edx, [esi + 4]
1:
    mov [edi], eax
    lea ecx, [edx + 65536]
    add cx, bp
    lock cmpxchg8b qword ptr [esi]
    jne 1b
    pop ebp
    pop ebx
    pop edi
    pop esi
    ret 16 - 8 * \fast
2:
slow_end \name
.endm
push_list InterlockedPushListSList, 1
push_list InterlockedPushListSListEx, 0

#include "guest_rtl_run_string.inc"
#include "ole_build_version.h"

#include "message_metadata_layout.h"
entry GetMessagePos
    mov eax, dword ptr fs:[X64ARM_MESSAGE_X86_BASE+X64ARM_MESSAGE_POSITION]
    ret
end GetMessagePos
entry GetMessageTime
    mov eax, dword ptr fs:[X64ARM_MESSAGE_X86_BASE+X64ARM_MESSAGE_TIME]
    ret
end GetMessageTime
entry GetMessageExtraInfo
    mov eax, dword ptr fs:[X64ARM_MESSAGE_X86_BASE+X64ARM_MESSAGE_EXTRA]
    ret
end GetMessageExtraInfo
entry SetMessageExtraInfo
    mov eax, dword ptr fs:[X64ARM_MESSAGE_X86_BASE+X64ARM_MESSAGE_EXTRA]
    mov ecx, dword ptr [esp+4]
    mov dword ptr fs:[X64ARM_MESSAGE_X86_BASE+X64ARM_MESSAGE_EXTRA], ecx
    ret 4
end SetMessageExtraInfo

#include "../../include/x64arm/windows_keyboard_model.h"

entry SwapMouseButton
    .byte 0xba
Lswap_patch:
    .long 0
    mov edx, dword ptr [edx]
    mov ecx, dword ptr [esp+4]
    xor eax, eax
    test ecx, ecx
    setne al
    xchg dword ptr [edx+X64ARM_GUI_MOUSE_SWAP_OFFSET], eax
    ret 4
end SwapMouseButton
.globl _guest_SwapMouseButton_gui_timing_patch
.set _guest_SwapMouseButton_gui_timing_patch, Lswap_patch - _guest_SwapMouseButton

#include "guest_sh_linear.inc"

// NT 的 fastcall 和 CRT 的 cdecl 共用位元組交換，只有參數位置及清棧責任不同。
.macro byte_swap name, width, fast
entry \name
.if \width == 64
    mov eax, dword ptr [esp+8]
    mov edx, dword ptr [esp+4]
    bswap eax
    bswap edx
.else
.if \fast
    mov eax, ecx
.else
    mov eax, dword ptr [esp+4]
.endif
    bswap eax
.if \width == 16
    shr eax, 16
.endif
.endif
.if \fast && \width == 64
    ret 8
.else
    ret
.endif
end \name
.endm
byte_swap ByteSwapFast16, 16, 1
byte_swap ByteSwapFast32, 32, 1
byte_swap ByteSwapFast64, 64, 1
byte_swap ByteSwapCrt16, 16, 0
byte_swap ByteSwapCrt32, 32, 0
byte_swap ByteSwapCrt64, 64, 0

.globl _guest_thread_end
_guest_thread_end:
