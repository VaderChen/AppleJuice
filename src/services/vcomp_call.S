// 逐個複製編譯器傳來的機器字；x64 保留 shadow space 與 Windows 展開框架。
.intel_syntax noprefix
.text
#ifdef __x86_64__
.globl vcomp_call
.seh_proc vcomp_call
vcomp_call:
    push rbp
    .seh_pushreg rbp
    mov rbp,rsp
    .seh_setframe rbp,0
    .seh_endprologue
    mov r11,rcx
    mov r10,r8
    mov eax,edx
    cmp eax,4
    jae 1f
    mov eax,4
1:  add rax,1
    and rax,-2
    // 以 push 分頁探測；外層呼叫者的參數在整個 fork/join 期間有效。
2:  dec rax
    xor ecx,ecx
    cmp eax,edx
    jae 3f
    mov rcx,[r10+rax*8]
3:  push rcx
    test rax,rax
    jne 2b
    mov rcx,[rsp]
    mov rdx,[rsp+8]
    mov r8,[rsp+16]
    mov r9,[rsp+24]
    call r11
    mov rsp,rbp
    pop rbp
    ret
.seh_endproc
#else
.globl _vcomp_call
_vcomp_call:
    push ebp
    mov ebp,esp
    mov ecx,[ebp+12]
    mov edx,[ebp+16]
    and esp,-16
    mov eax,ecx
    neg eax
    and eax,3
    shl eax,2
    sub esp,eax
    test ecx,ecx
    je 2f
1:  dec ecx
    push DWORD PTR [edx+ecx*4]
    test ecx,ecx
    jne 1b
2:  call DWORD PTR [ebp+8]
    leave
    ret
#endif
