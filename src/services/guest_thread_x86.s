// Win32 執行緒區域常式：每次讀取當前 FS 的 TEB；stdcall 參數由被呼叫端清除。
// 只使用 volatile EAX／ECX 與旗標，保留非 volatile 暫存器及浮點狀態。
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
    mov eax, dword ptr fs:[0x34]
    ret
end GetLastError

entry SetLastError
    mov eax, dword ptr [esp+4]
    mov dword ptr fs:[0x34], eax
    xor eax, eax
    ret 4
end SetLastError

.macro tls_read name, clear_error
entry \name
    mov ecx, dword ptr [esp+4]
    cmp ecx, 1088
    jae Linvalid\@
    cmp ecx, 64
    jae Lexpanded\@
    mov eax, dword ptr fs:[ecx*4+0xe10]
    jmp Lvalue\@
Lexpanded\@:
    // 與服務層共用 0x2000 + (index - 64) * 4 的擴充槽配置。
    mov eax, dword ptr fs:[ecx*4+0x1f00]
Lvalue\@:
.if \clear_error
    mov dword ptr fs:[0x34], 0
.endif
    ret 4
Linvalid\@:
.if \clear_error
    mov dword ptr fs:[0x34], 87
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
.globl _guest_thread_end
_guest_thread_end:
