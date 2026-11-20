#pragma once
// 客體增強型鍵盤的硬體能力，同時供裝置描述與常駐查詢使用。
#define X64ARM_KEYBOARD_TYPE 4
#define X64ARM_KEYBOARD_SUBTYPE 0
#define X64ARM_KEYBOARD_FUNCTION_KEYS 12

#ifdef __ASSEMBLER__
entry GetKeyboardType
#ifdef __i386__
    mov ecx, dword ptr [esp+4]
#endif
    mov eax, X64ARM_KEYBOARD_TYPE
    test ecx, ecx
    je 1f
    mov eax, X64ARM_KEYBOARD_SUBTYPE
    cmp ecx, 1
    je 1f
    mov eax, X64ARM_KEYBOARD_FUNCTION_KEYS
    cmp ecx, 2
    je 1f
    xor eax, eax
#ifdef __i386__
    mov dword ptr fs:[X64ARM_TLS_X86_ERROR_OFFSET], 87
1:  ret 4
#else
    mov dword ptr gs:[X64ARM_TLS_X64_ERROR_OFFSET], 87
1:  ret
#endif
end GetKeyboardType
#endif
