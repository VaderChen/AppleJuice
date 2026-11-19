// SHAnsiToAnsi／SHUnicodeToUnicode 原樣複製碼元；傳回長度含 NUL，目的地不足時傳回零。
.macro shell_copy_encoding name, unit
entry \name
#ifdef __i386__
    push esi
    push edi
    mov esi, [esp+12]
    mov edi, [esp+16]
    mov ecx, [esp+20]
#define SRC esi
#define DST edi
#define CAP ecx
#else
#define SRC rcx
#define DST rdx
#define CAP r8d
#endif
    xor eax,eax
    test SRC,SRC
    jz 4f
    test DST,DST
    jz 4f
    test CAP,CAP
    jle 4f
1:
    inc eax
    dec CAP
    jz 2f
.if \unit == 1
    cmp byte ptr [SRC],0
.else
    cmp word ptr [SRC],0
.endif
    je 3f
#ifdef __i386__
.if \unit == 1
    movsb
.else
    movsw
.endif
#else
.if \unit == 1
    mov r9b,[SRC]
    mov [DST],r9b
.else
    mov r9w,[SRC]
    mov [DST],r9w
.endif
    add SRC,\unit
    add DST,\unit
#endif
    jmp 1b
2:
    cmp SRC,DST
    je 3f
.if \unit == 1
    cmp byte ptr [SRC],0
.else
    cmp word ptr [SRC],0
.endif
    je 3f
    xor eax,eax
3:
.if \unit == 1
    mov byte ptr [DST],0
.else
    mov word ptr [DST],0
.endif
4:
#ifdef __i386__
    pop edi
    pop esi
    ret 12
#else
    ret
#endif
end \name
.endm
#undef SRC
#undef DST
#undef CAP
shell_copy_encoding SHAnsiToAnsi, 1
shell_copy_encoding SHUnicodeToUnicode, 2
