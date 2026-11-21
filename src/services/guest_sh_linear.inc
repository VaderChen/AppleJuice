// 每次先讀來源再寫出一個係數，保留陣列重疊與逐項故障的順序。
// 階數平方依 UINT 截斷，浮點內積分別進行乘法與加法。
.macro sh_transform name, scale
entry \name
#ifdef __x86_64__
    imul edx, edx
    xor eax, eax
    test edx, edx
    jz 2f
1:
    movss xmm0, dword ptr [r8+rax*4]
.if \scale
    mulss xmm0, xmm3
.else
    addss xmm0, dword ptr [r9+rax*4]
.endif
    movss dword ptr [rcx+rax*4], xmm0
    inc eax
    cmp eax, edx
    jb 1b
2:
    mov rax, rcx
    ret
#else
    mov edx, dword ptr [esp+8]
    imul edx, edx
    xor ecx, ecx
    test edx, edx
    jz 2f
1:
    mov eax, dword ptr [esp+12]
    movss xmm0, dword ptr [eax+ecx*4]
.if \scale
    mulss xmm0, dword ptr [esp+16]
.else
    mov eax, dword ptr [esp+16]
    addss xmm0, dword ptr [eax+ecx*4]
.endif
    mov eax, dword ptr [esp+4]
    movss dword ptr [eax+ecx*4], xmm0
    inc ecx
    cmp ecx, edx
    jb 1b
2:
    mov eax, dword ptr [esp+4]
    ret 16
#endif
end \name
.endm
sh_transform D3DXSHAdd, 0
sh_transform D3DXSHScale, 1

entry D3DXSHDot
#ifdef __x86_64__
    imul ecx, ecx
    movss xmm0, dword ptr [rdx]
    mulss xmm0, dword ptr [r8]
    mov eax, 1
    cmp eax, ecx
    jae 2f
1:
    movss xmm1, dword ptr [rdx+rax*4]
    mulss xmm1, dword ptr [r8+rax*4]
    addss xmm0, xmm1
    inc eax
    cmp eax, ecx
    jb 1b
2:
    ret
#else
    mov edx, dword ptr [esp+4]
    imul edx, edx
    mov eax, dword ptr [esp+8]
    movss xmm0, dword ptr [eax]
    mov eax, dword ptr [esp+12]
    mulss xmm0, dword ptr [eax]
    mov ecx, 1
    cmp ecx, edx
    jae 2f
1:
    mov eax, dword ptr [esp+8]
    movss xmm1, dword ptr [eax+ecx*4]
    mov eax, dword ptr [esp+12]
    mulss xmm1, dword ptr [eax+ecx*4]
    addss xmm0, xmm1
    inc ecx
    cmp ecx, edx
    jb 1b
2:
    // Win32 以 ST(0) 回傳；最後一個堆疊參數已用完。
    movss dword ptr [esp+12], xmm0
    fld dword ptr [esp+12]
    ret 12
#endif
end D3DXSHDot
