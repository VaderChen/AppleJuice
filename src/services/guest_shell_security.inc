// NT 絕對描述元。欄位與宿主 Shell 處理常式一致；保留 SECURITY_ATTRIBUTES 的對齊填充。
entry CreateAllAccessSecurityAttributes
#ifdef __i386__
    mov ecx, dword ptr [esp+4]
    mov edx, dword ptr [esp+8]
#endif
    xor eax, eax
#ifdef __i386__
    test ecx, ecx
    jz 1f
    test edx, edx
    jz 1f
    mov dword ptr [edx], 0x40001
    mov [edx+4], eax
    mov [edx+8], eax
    mov [edx+12], eax
    mov [edx+16], eax
    mov dword ptr [ecx], 12
    mov [ecx+4], edx
    mov [ecx+8], eax
    mov eax, ecx
1:  ret 12
#else
    test rcx, rcx
    jz 1f
    test rdx, rdx
    jz 1f
    mov qword ptr [rdx], 0x40001
    mov [rdx+8], rax
    mov [rdx+16], rax
    mov [rdx+24], rax
    mov [rdx+32], rax
    mov dword ptr [rcx], 24
    mov [rcx+8], rdx
    mov [rcx+16], eax
    mov rax, rcx
1:  ret
#endif
end CreateAllAccessSecurityAttributes
