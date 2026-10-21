bits 64
default rel
global entry
extern __imp_GetStdHandle, __imp_WriteFile, __imp_ExitProcess
section .text
entry:
    sub rsp, 56
    mov ecx, -11
    call [__imp_GetStdHandle]
    mov rcx, rax
    mov rdx, [message_pointer]
    mov r8d, message_end - message
    lea r9, [rsp + 40]
    mov qword [rsp + 32], 0
    call [__imp_WriteFile]
    test eax, eax
    je failed
    cmp dword [rsp + 40], message_end - message
    jne failed
    xor eax, eax
    mov ecx, 100
sum:
    add eax, ecx
    dec ecx
    jne sum
    cmp eax, 5050
    jne failed
    xor ecx, ecx
    call [__imp_ExitProcess]
failed:
    mov ecx, 9
    call [__imp_ExitProcess]
section .rdata
message: db 'Relocated PE: sum(1..100) = 5050', 10
message_end:
section .data
; 絕對位址必須由 PE DIR64 重定位修正；驗證替代 image base。
message_pointer: dq message
