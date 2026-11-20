// 客體常駐編解碼。種子為零時由宿主取得客體系統時間，尾跳時保留原始 ABI。
.macro rtl_run_string name, encode
entry \name
#ifdef __i386__
    mov ecx, [esp+4]
    mov edx, [esp+8]
#define HASH ecx
#define DATA edx
#define LENGTH esi
#else
#define HASH rcx
#define DATA rdx
#define LENGTH r8d
#endif
.if \encode
    cmp byte ptr [HASH], 0
    je 9f
.endif
#ifdef __i386__
    push esi
#endif
    movzx LENGTH, word ptr [DATA]
#ifdef __i386__
    mov DATA, [DATA+4]
#else
    mov DATA, [DATA+8]
#endif
    test LENGTH, LENGTH
    jz 4f
.if \encode
    mov al, [HASH]
    or al, 0x43
    xor al, [DATA]
    mov [DATA], al
1:
    dec LENGTH
    jz 4f
    inc DATA
    xor al, [HASH]
    xor al, [DATA]
    mov [DATA], al
    jmp 1b
.else
#ifdef __i386__
    lea DATA, [DATA+esi-1]
#else
    lea DATA, [DATA+r8-1]
#endif
2:
    dec LENGTH
    jz 3f
    mov al, [DATA-1]
    xor al, cl
    xor [DATA], al
    dec DATA
    jmp 2b
3:
    or cl, 0x43
    xor [DATA], cl
.endif
4:
#ifdef __i386__
    pop esi
    ret 8
#else
    ret
#endif
.if \encode
9:
slow_end \name
.else
end \name
.endif
#undef HASH
#undef DATA
#undef LENGTH
.endm
rtl_run_string RtlRunEncodeUnicodeString, 1
rtl_run_string RtlRunDecodeUnicodeString, 0
