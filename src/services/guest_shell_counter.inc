#include "gui_timing_layout.h"
// 每次載入當前共用頁。四個列舉入口與宿主慢路徑共用對齊的 32 位元槽。
.macro shell_counter name, operation
entry \name
#ifdef __i386__
    mov ecx, dword ptr [esp+4]
#endif
    xor eax, eax
    cmp ecx, X64ARM_SHELL_COUNTER_COUNT
    jae 1f
#ifdef __i386__
    .byte 0xba
Lcounter_\name:
    .long 0
    mov edx, dword ptr [edx]
    lea ecx, [edx+ecx*4+X64ARM_SHELL_COUNTER_OFFSET]
#define COUNTER_SLOT dword ptr [ecx]
#else
    .byte 0x48,0xb8
Lcounter_\name:
    .quad 0
    mov rax, qword ptr [rax]
    mov ecx, ecx
    lea rcx, [rax+rcx*4+X64ARM_SHELL_COUNTER_OFFSET]
#define COUNTER_SLOT dword ptr [rcx]
#endif
.if \operation == 0
    mov eax, COUNTER_SLOT
.elseif \operation == 2
#ifdef __i386__
    mov eax, dword ptr [esp+8]
#else
    mov eax, edx
#endif
    xchg COUNTER_SLOT, eax
.else
    mov eax, \operation
    lock xadd COUNTER_SLOT, eax
    add eax, \operation
.endif
1:
#ifdef __i386__
.if \operation == 2
    ret 8
.else
    ret 4
.endif
#else
    ret
#endif
end \name
.globl _guest_\name\()_gui_timing_patch
.set _guest_\name\()_gui_timing_patch, Lcounter_\name - _guest_\name
.endm
#undef COUNTER_SLOT
shell_counter SHGlobalCounterGetValue, 0
shell_counter SHGlobalCounterIncrement, 1
shell_counter SHGlobalCounterDecrement, -1
shell_counter SHGlobalCounterSetValue, 2
