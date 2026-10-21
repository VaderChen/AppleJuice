// Windows x64 執行緒區域常式。資料始終讀寫目前 GS 指向的 TEB，不快取執行緒值。
// 只使用 volatile GPR；不變更 XMM、非 volatile GPR 或 RSP（RET 除外）。
// TLS 的 64 個內建槽與 1024 個擴充槽沿用 WindowsServices 的既有配置。
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
    mov eax, dword ptr gs:[0x68]
    ret
end GetLastError

entry SetLastError
    mov dword ptr gs:[0x68], ecx
    xor eax, eax
    ret
end SetLastError

.macro tls_read name, clear_error
entry \name
    // DWORD 參數：不使用呼叫者未定義的 RCX 高半部。
    mov ecx, ecx
    cmp ecx, 1088
    jae Linvalid\@
    cmp ecx, 64
    jae Lexpanded\@
    mov rax, qword ptr gs:[rcx*8+0x1480]
    jmp Lvalue\@
Lexpanded\@:
    // 0x2000 + (index - 64) * 8；與 C provider 的實際配置相同。
    mov rax, qword ptr gs:[rcx*8+0x1e00]
Lvalue\@:
.if \clear_error
    mov dword ptr gs:[0x68], 0
.endif
    ret
Linvalid\@:
.if \clear_error
    mov dword ptr gs:[0x68], 87
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
    lea rdx, [rax + 4]
    lock cmpxchg qword ptr [rcx], rdx
    jne 1f
    mov eax, 1
    ret
1:
slow_end TryAcquireSRWLockShared
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
    mov dword ptr gs:[0x68], 0
.endif
    ret
2:
.if \set_error
    mov dword ptr gs:[0x68], 87
.endif
    xor eax, eax
    ret
1:
slow_end \name
.endm
fls_read FlsGetValue, 1
fls_read FlsGetValue2, 0
.globl _guest_thread_end
_guest_thread_end:
