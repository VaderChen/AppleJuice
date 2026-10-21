// Windows x64 純量餘數共用常式。只使用 volatile GPR，XMM1 與 XMM0 未回傳部分不變。
// 有限正規輸入以整數有效位做分段餘數；零回傳保留被除數符號。
// 非有限值、非零次正規輸入、零除數、次正規結果及過大指數差尾跳原 CRT。
// 慢路徑尚未改寫 XMM0／XMM1／RSP；不猜測 errno、NaN 或浮點環境語義。
.intel_syntax noprefix
.text
.macro remainder name, width, fraction, max_exponent
.p2align 7
.globl _guest_\name
_guest_\name:
.if \width == 32
    movd eax, xmm0
    movd edx, xmm1
.else
    movq rax, xmm0
    movq rdx, xmm1
.endif
    mov r11, rax
    btr rax, (\width - 1)
    btr rdx, (\width - 1)
    mov r8, rax
    mov r9, rdx
    shr r8, \fraction
    shr r9, \fraction
    cmp r8d, \max_exponent
    je Lslow\@
    cmp r9d, \max_exponent
    je Lslow\@
    test r9d, r9d
    je Lslow\@
    test r8d, r8d
    jne Lnormal\@
    test rax, rax
    jne Lslow\@
    jmp Ldone\@
Lnormal\@:
    cmp rax, rdx
    jb Ldone\@
    je Lzero\@
    sub r8d, r9d
    // 限制最長常式成本；所有較大指數差仍由通用 CRT 處理。
    cmp r8d, \fraction
    ja Lslow\@
    shl rax, (64 - \fraction)
    shr rax, (64 - \fraction)
    shl rdx, (64 - \fraction)
    shr rdx, (64 - \fraction)
    bts rax, \fraction
    bts rdx, \fraction
    mov r10, rdx
Lreduce\@:
    // 每段最多左移 63-fraction 位，分子仍放得進 64 位。
    // RDX=0 且 R10 非零，既有 AOT 直接生成 UDIV／MSUB，不進寬除法 helper。
    mov ecx, (63 - \fraction)
    cmp r8d, ecx
    cmovb ecx, r8d
    shl rax, cl
    sub r8d, ecx
    mov edx, 0
    div r10
    mov rax, rdx
    test rax, rax
    je Lzero\@
    test r8d, r8d
    jne Lreduce\@
Lnormalize\@:
    bsr r8, rax
    mov ecx, \fraction
    sub ecx, r8d
    shl rax, cl
    sub r9d, ecx
    jle Lslow\@
    btr rax, \fraction
    shl r9, \fraction
    or rax, r9
    jmp Lsign\@
Lzero\@:
    xor eax, eax
Lsign\@:
    shr r11, (\width - 1)
    shl r11, (\width - 1)
    or rax, r11
.if \width == 32
    pinsrd xmm0, eax, 0
.else
    pinsrq xmm0, rax, 0
.endif
Ldone\@:
    mov eax, 0
    ret
Lslow\@:
    jmp qword ptr [rip + Lslot\@]
.p2align 3
Lslot\@:
    .quad 0
.globl _guest_\name\()_end
_guest_\name\()_end:
.endm

remainder fmod, 64, 52, 2047
remainder fmodf, 32, 23, 255

// isspace：ASCII 範圍（0..127）在所有 Windows 單／多位元組 locale 都相同，直接判定 9..13 與 32 回 1；
// 其餘值（EOF、>=128 的 locale 相關字元）尾跳原字元分類服務。只使用 EAX／EDX，ECX 保持原值供慢路徑。
.p2align 7
.globl _guest_isspace
_guest_isspace:
    xor eax, eax
    cmp ecx, 127
    ja Lisspace_slow
    lea edx, [rcx - 9]
    cmp edx, 4
    jbe Lisspace_true
    cmp ecx, 32
    jne Lisspace_done
Lisspace_true:
    mov eax, 1
Lisspace_done:
    ret
Lisspace_slow:
    jmp qword ptr [rip + Lisspace_slot]
.p2align 3
Lisspace_slot:
    .quad 0
.globl _guest_isspace_end
_guest_isspace_end:

// _stricmp／_strcmpi：與服務層相同的 C locale 語義——只把 'A'..'Z' 轉小寫，其餘位元組保持原值逐一比較，
// 回傳第一個不同位元組的差值（轉小寫後）。空指標交回原服務（errno／invalid parameter）。
// 只使用 volatile GPR，RCX／RDX 以索引 R10 存取而保持原值。
.macro stricmp_routine name
.p2align 7
.globl _guest_\name
_guest_\name:
    test rcx, rcx
    je Lslow\@
    test rdx, rdx
    je Lslow\@
    xor r10d, r10d
Lloop\@:
    movzx eax, byte ptr [rcx + r10]
    movzx r8d, byte ptr [rdx + r10]
    lea r9d, [rax - 65]
    cmp r9d, 25
    ja 1f
    add eax, 32
1:
    lea r9d, [r8 - 65]
    cmp r9d, 25
    ja 2f
    add r8d, 32
2:
    sub eax, r8d
    jne Ldone\@
    test r8d, r8d
    je Ldone\@
    inc r10
    jmp Lloop\@
Ldone\@:
    ret
Lslow\@:
    jmp qword ptr [rip + Lslot\@]
.p2align 3
Lslot\@:
    .quad 0
.globl _guest_\name\()_end
_guest_\name\()_end:
.endm
stricmp_routine _stricmp
stricmp_routine _strcmpi

// atoi：與服務層相同（C locale 的 strtoll 後夾到 int32）。略過 ASCII 空白、選用正負號、十進位數字；
// 累計值超過 int32 範圍（需設 ERANGE）或空指標交回原服務。只用 volatile GPR，RCX 保持原值。
.p2align 7
.globl _guest_atoi
_guest_atoi:
    test rcx, rcx
    je Latoi_slow
    mov r8, rcx
Latoi_space:
    movzx eax, byte ptr [r8]
    cmp eax, 32
    je Latoi_skip
    lea edx, [rax - 9]
    cmp edx, 4
    ja Latoi_sign
Latoi_skip:
    inc r8
    jmp Latoi_space
Latoi_sign:
    xor r9d, r9d
    cmp eax, 45
    jne Latoi_plus
    mov r9d, 1
    jmp Latoi_next
Latoi_plus:
    cmp eax, 43
    jne Latoi_digits
Latoi_next:
    inc r8
    movzx eax, byte ptr [r8]
Latoi_digits:
    xor edx, edx
    mov r10d, 0x80000000
Latoi_loop:
    sub eax, 48
    cmp eax, 9
    ja Latoi_done
    imul rdx, rdx, 10
    add rdx, rax
    cmp rdx, r10
    ja Latoi_slow
    inc r8
    movzx eax, byte ptr [r8]
    jmp Latoi_loop
Latoi_done:
    test r9d, r9d
    je Latoi_positive
    neg rdx
    mov eax, edx
    ret
Latoi_positive:
    cmp rdx, r10
    jae Latoi_slow
    mov eax, edx
    ret
Latoi_slow:
    jmp qword ptr [rip + Latoi_slot]
.p2align 3
Latoi_slot:
    .quad 0
.globl _guest_atoi_end
_guest_atoi_end:

// atof：Clinger 快速路徑，與服務層 strtod（C locale）逐位元相同。有效位數 <= 15（< 2^53）且
// 十進位指數 |e| <= 22 時，m 與 10^|e| 都是精確 double，一次 MULSD／DIVSD 即正確捨入。
// MXCSR 非就近捨入、十六進位、inf／nan、無數字、指數無數字、位數或指數超界、空指標都交回原服務。
// 只用 volatile GPR 與 XMM0；RCX 全程保持原值，MXCSR 暫存在呼叫者提供的 home space [RSP+8]。
// R8 游標、R9 bit0 尾數負號／bit1 指數負號、R10 有效位數值、R11 bit0 見過數字＋bit8 起位數（之後改為指數）、EDX 十進位指數。
.macro decimal_to_double_routine name, end_pointer
.p2align 7
.globl _guest_\name
_guest_\name:
    test rcx, rcx
    je Lfallback_\name
    stmxcsr dword ptr [rsp + 8]
    mov eax, dword ptr [rsp + 8]
    and eax, 0x7f80
    cmp eax, 0x1f80
    jne Lfallback_\name
.if \end_pointer
    mov qword ptr [rsp + 16], rdx
    xor r10d, r10d
.endif
    mov r8, rcx
Lspace_\name:
    movzx eax, byte ptr [r8]
    cmp eax, 32
    je Lskip_\name
    lea edx, [rax - 9]
    cmp edx, 4
    ja Lsign_\name
Lskip_\name:
.if \end_pointer
    inc r10d
    cmp r10d, 128
    jae Lslow_\name
.endif
    inc r8
    jmp Lspace_\name
Lsign_\name:
    xor r9d, r9d
    cmp eax, 45
    jne Lplus_\name
    mov r9d, 1
    jmp Lnext_\name
Lplus_\name:
    cmp eax, 43
    jne Lstart_\name
Lnext_\name:
    inc r8
    movzx eax, byte ptr [r8]
Lstart_\name:
    xor r10d, r10d
    xor r11d, r11d
    xor edx, edx
Lint_\name:
    sub eax, 48
    cmp eax, 9
    ja Lpoint_\name
    or r11d, 1
.if !\end_pointer
    test r10, r10
    jne Lint_digit_\name
    test eax, eax
    je Lint_next_\name
.endif
Lint_digit_\name:
    cmp r11d, 0x0f00
    jae Lslow_\name
    add r11d, 0x100
    imul r10, r10, 10
    add r10, rax
    jmp Lint_next_\name
Lint_next_\name:
    inc r8
    movzx eax, byte ptr [r8]
    jmp Lint_\name
Lpoint_\name:
    cmp eax, 72                 // 'x' - '0'：十六進位交回 strtod
    je Lslow_\name
    cmp eax, 40                 // 'X' - '0'
    je Lslow_\name
    cmp eax, -2
    jne Lexponent_\name
    inc r8
    movzx eax, byte ptr [r8]
Lfrac_\name:
    sub eax, 48
    cmp eax, 9
    ja Lexponent_\name
    or r11d, 1
.if !\end_pointer
    test r10, r10
    jne Lfrac_digit_\name
    test eax, eax
    je Lfrac_zero_\name
.endif
Lfrac_digit_\name:
    cmp r11d, 0x0f00
    jae Lslow_\name
    add r11d, 0x100
    imul r10, r10, 10
    add r10, rax
Lfrac_zero_\name:
    dec edx
    inc r8
    movzx eax, byte ptr [r8]
    jmp Lfrac_\name
Lexponent_\name:
    test r11d, 1
    je Lslow_\name
    or eax, 32
    cmp eax, 53
    jne Lvalue_\name
    inc r8
    movzx eax, byte ptr [r8]
    cmp eax, 45
    jne Lexp_plus_\name
    or r9d, 2
    jmp Lexp_sign_\name
Lexp_plus_\name:
    cmp eax, 43
    jne Lexp_first_\name
Lexp_sign_\name:
    inc r8
    movzx eax, byte ptr [r8]
Lexp_first_\name:
    sub eax, 48
    cmp eax, 9
    ja Lslow_\name
    xor r11d, r11d
Lexp_loop_\name:
.if \end_pointer
    add r9d, 0x100
    cmp r9d, 0x800
    jae Lslow_\name
.endif
    imul r11d, r11d, 10
    add r11d, eax
    cmp r11d, 1000
    ja Lslow_\name
    inc r8
    movzx eax, byte ptr [r8]
    sub eax, 48
    cmp eax, 9
    jbe Lexp_loop_\name
    test r9d, 2
    je Lexp_add_\name
    neg r11d
Lexp_add_\name:
    add edx, r11d
Lvalue_\name:
.if \end_pointer
    // 先確定掃描已讀到 NUL；帶尾綴的字串交回服務以保持完整讀取的故障順序。
    cmp byte ptr [r8], 0
    jne Lslow_\name
    test r10, r10
    je Lendptr_\name
.else
    test r10, r10
    je Lzero_\name
.endif
    cmp edx, 22
    jg Lslow_\name
    cmp edx, -22
    jl Lslow_\name
.if \end_pointer
Lendptr_\name:
    // 已確認有限範圍且所有來源讀取完成，先提交 endptr 再計算浮點值。
    // 寫入失敗時尚未產生浮點副作用；特殊輸入在這之前尾跳原服務。
    mov r11, qword ptr [rsp + 16]
    test r11, r11
    je Lconverted_\name
    mov qword ptr [r11], r8
Lconverted_\name:
    test r10, r10
    je Lzero_\name
.endif
    cvtsi2sd xmm0, r10
    lea rax, [rip + Lpow10_\name]
    test edx, edx
    js Ldivide_\name
    mulsd xmm0, qword ptr [rax + rdx*8]
    jmp Lsign_apply_\name
Ldivide_\name:
    neg edx
    divsd xmm0, qword ptr [rax + rdx*8]
    jmp Lsign_apply_\name
Lzero_\name:
    xorps xmm0, xmm0
Lsign_apply_\name:
    test r9d, 1
    je Lret_\name
    movq rax, xmm0
    btc rax, 63
    movq xmm0, rax
Lret_\name:
    ret
Lslow_\name:
.if \end_pointer
    mov rdx, qword ptr [rsp + 16]
.endif
Lfallback_\name:
    jmp qword ptr [rip + Lslot_\name]
.p2align 3
Lpow10_\name:
    .double 1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11
    .double 1e12,1e13,1e14,1e15,1e16,1e17,1e18,1e19,1e20,1e21,1e22
Lslot_\name:
    .quad 0
.globl _guest_\name\()_end
_guest_\name\()_end:
.endm
decimal_to_double_routine atof, 0

// 客體配置區：malloc／free／_aligned_malloc（對齊 <=16）／_aligned_free 的快速路徑。
// 狀態頁位址由宿主填入各常式慢路徑 slot 前 8 bytes 的 cell；0 表示未啟用，一律交回原服務。
// 狀態頁：+0 鎖、+8 配置區起點、+16 長度、+24 大小上限、+0x40+class*8 空閒串列頭。
// 4 KiB 內為 16-byte 級距；其後為 256-byte 級距，含對齊標頭最多 497 級。
// chunk 256 KiB 對齊：+0 magic、+8 class；區塊自 +64 起，空閒時以區塊首 8 bytes 串接。
// 對齊配置額外有 16-byte 標頭：+0 原始大小、+8 alignment；回傳標頭之後的指標。
// 空串列、鎖忙、超過上限或非配置區指標都尾跳原服務；宿主使用相同狀態並負責補充 chunk。
.macro heap_size_class
    cmp rax, 4096
    jbe Lsmall\@
    sub rax, 3841
    shr rax, 8
    add rax, 256
    jmp Lclass\@
Lsmall\@:
    add rax, 15
    shr rax, 4
    mov r9d, 1
    test rax, rax
    cmove rax, r9
Lclass\@:
.endm
.p2align 7
.globl _guest_malloc
_guest_malloc:
    mov r10, qword ptr [rip + Lmalloc_cell]
    test r10, r10
    je Lmalloc_slow
    cmp rcx, qword ptr [r10 + 24]
    ja Lmalloc_slow
    mov rax, rcx
    heap_size_class
    mov edx, 1
    xchg dword ptr [r10], edx
    test edx, edx
    jne Lmalloc_slow
    mov r8, qword ptr [r10 + rax*8 + 0x40]
    test r8, r8
    je Lmalloc_empty
    mov r9, qword ptr [r8]
    mov qword ptr [r10 + rax*8 + 0x40], r9
    mov dword ptr [r10], 0
    mov rax, r8
    ret
Lmalloc_empty:
    mov dword ptr [r10], 0
Lmalloc_slow:
    jmp qword ptr [rip + Lmalloc_slot]
.p2align 3
Lmalloc_cell:
    .quad 0
Lmalloc_slot:
    .quad 0
.globl _guest_malloc_end
_guest_malloc_end:

.p2align 7
.globl _guest__aligned_malloc
_guest__aligned_malloc:
    test rcx, rcx
    je Laligned_malloc_slow
    lea rax, [rdx - 1]
    cmp rax, 15
    ja Laligned_malloc_slow
    test rax, rdx
    jne Laligned_malloc_slow
    mov r10, qword ptr [rip + Laligned_malloc_cell]
    test r10, r10
    je Laligned_malloc_slow
    cmp rcx, qword ptr [r10 + 24]
    ja Laligned_malloc_slow
    mov rax, rcx
    mov r9d, 1
    test rax, rax
    cmove rax, r9
    add rax, 16
    jc Laligned_malloc_slow
    heap_size_class
    mov r9d, 1
    xchg dword ptr [r10], r9d
    test r9d, r9d
    jne Laligned_malloc_slow
    mov r8, qword ptr [r10 + rax*8 + 0x40]
    test r8, r8
    je Laligned_malloc_empty
    mov r11, qword ptr [r8]
    mov qword ptr [r10 + rax*8 + 0x40], r11
    mov dword ptr [r10], 0
    mov qword ptr [r8], rcx
    mov qword ptr [r8 + 8], rdx
    lea rax, [r8 + 16]
    ret
Laligned_malloc_empty:
    mov dword ptr [r10], 0
Laligned_malloc_slow:
    jmp qword ptr [rip + Laligned_malloc_slot]
.p2align 3
Laligned_malloc_cell:
    .quad 0
Laligned_malloc_slot:
    .quad 0
.globl _guest__aligned_malloc_end
_guest__aligned_malloc_end:

// free／_aligned_free：NULL 無動作；配置區指標（範圍與 chunk magic 皆符合）推回級距串列，其餘交回原服務。
.macro heap_free_routine name, pointer=rcx, header=0
.p2align 7
.globl _guest_\name
_guest_\name:
    test rcx, rcx
    je Ldone\@
.if \header
    lea \pointer, [rcx - \header]
.endif
    mov r10, qword ptr [rip + Lcell\@]
    test r10, r10
    je Lslow\@
    mov rax, \pointer
    sub rax, qword ptr [r10 + 8]
    cmp rax, qword ptr [r10 + 16]
    jae Lslow\@
    mov r8, \pointer
    and r8, -0x40000
    movabs r9, 0x50414548343658ab
    cmp qword ptr [r8], r9
    jne Lslow\@
    mov eax, dword ptr [r8 + 8]
    mov edx, 1
Llock\@:
    xchg dword ptr [r10], edx
    test edx, edx
    je Llocked\@
    pause
    mov edx, 1
    jmp Llock\@
Llocked\@:
    mov r9, qword ptr [r10 + rax*8 + 0x40]
    mov qword ptr [\pointer], r9
    mov qword ptr [r10 + rax*8 + 0x40], \pointer
    mov dword ptr [r10], 0
Ldone\@:
    ret
Lslow\@:
    jmp qword ptr [rip + Lslot\@]
.p2align 3
Lcell\@:
    .quad 0
Lslot\@:
    .quad 0
.globl _guest_\name\()_end
_guest_\name\()_end:
.endm
heap_free_routine free
heap_free_routine _aligned_free, r11, 16

// _lock_locales／_unlock_locales：locale 鎖的客體快速路徑。cell 指向狀態格：+0 擁有者公開執行緒編號
// （TEB+0x48）、+4 遞迴次數、+8 宿主登記的等待者數；0 表示未啟用，一律交回原服務。
// 無主以 LOCK CMPXCHG 取得、自身持有則遞增；其他執行緒持有、遞迴上限、非持有者解鎖或有等待者時尾跳原服務。
.p2align 7
.globl _guest__lock_locales
_guest__lock_locales:
    mov r10, qword ptr [rip + Llock_locales_cell]
    test r10, r10
    je Llock_locales_slow
    mov r11d, dword ptr gs:[0x48]
    test r11d, r11d
    je Llock_locales_slow
    mov eax, dword ptr [r10]
    cmp eax, r11d
    je Llock_locales_recurse
    test eax, eax
    jne Llock_locales_slow
    lock cmpxchg dword ptr [r10], r11d
    jne Llock_locales_slow
    mov dword ptr [r10 + 4], 1
    ret
Llock_locales_recurse:
    cmp dword ptr [r10 + 4], -1
    je Llock_locales_slow
    inc dword ptr [r10 + 4]
    ret
Llock_locales_slow:
    jmp qword ptr [rip + Llock_locales_slot]
.p2align 3
Llock_locales_cell:
    .quad 0
Llock_locales_slot:
    .quad 0
.globl _guest__lock_locales_end
_guest__lock_locales_end:

.p2align 7
.globl _guest__unlock_locales
_guest__unlock_locales:
    mov r10, qword ptr [rip + Lunlock_locales_cell]
    test r10, r10
    je Lunlock_locales_slow
    mov r11d, dword ptr gs:[0x48]
    test r11d, r11d
    je Lunlock_locales_slow
    cmp dword ptr [r10], r11d
    jne Lunlock_locales_slow
    cmp dword ptr [r10 + 8], 0
    jne Lunlock_locales_slow
    mov eax, dword ptr [r10 + 4]
    test eax, eax
    je Lunlock_locales_slow
    dec eax
    mov dword ptr [r10 + 4], eax
    jne Lunlock_locales_done
    mov dword ptr [r10], 0
Lunlock_locales_done:
    ret
Lunlock_locales_slow:
    jmp qword ptr [rip + Lunlock_locales_slot]
.p2align 3
Lunlock_locales_cell:
    .quad 0
Lunlock_locales_slot:
    .quad 0
.globl _guest__unlock_locales_end
_guest__unlock_locales_end:

// strtod 共用 atof 的 Clinger 解析及捨入；新增常式放在末尾，保留既有常式位移。
// 快速路徑接受完整十進位字串、最多 15 位數字、少於 128 個前置空白及少於 8 位指數。
// RDX 原 endptr 存於 home space；非空時回填結尾，任何慢路徑都恢復原參數。
// 長字串、尾綴、特殊值、非就近捨入及超界一律使用原 strtod 完整契約。
decimal_to_double_routine strtod, 1

.globl _guest_scalar_math_end
_guest_scalar_math_end:
