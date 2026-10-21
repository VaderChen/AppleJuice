// 共用完成結果續跑常式；宿主只驗證／取出 packet，資料寫回經客體 JIT lowering。
// 保留原 API 堆疊：輸出即使與返回框架重疊，也依寫回後的 RET 取得返回位址。
.text
.code32
.p2align 5
.globl _guest_io_packet_x86
_guest_io_packet_x86:
    // XMM0 = {bytes 指標, key 指標, overlapped 指標, 0}
    // XMM1 = {bytes, key, overlapped, 0}；EAX = BOOL。
    movd %xmm0, %edx
    movd %xmm1, (%edx)
    psrldq $4, %xmm0
    psrldq $4, %xmm1
    movd %xmm0, %edx
    movd %xmm1, (%edx)
    psrldq $4, %xmm0
    psrldq $4, %xmm1
    movd %xmm0, %edx
    movd %xmm1, (%edx)
    ret $20
.globl _guest_io_packet_x86_end
_guest_io_packet_x86_end:
.p2align 5
.globl _guest_io_timeout_x86
_guest_io_timeout_x86:
    // XMM0 低 DWORD = overlapped 指標；EAX = FALSE。
    movd %xmm0, %edx
    movl $0, (%edx)
    ret $20
.globl _guest_io_timeout_x86_end
_guest_io_timeout_x86_end:

.code64
.p2align 5
.globl _guest_io_packet_x64
_guest_io_packet_x64:
    // XMM0 = {bytes 指標, key 指標}；XMM1 = {overlapped 指標, 0}。
    // XMM2 = {bytes / 0, key}；XMM3 = {overlapped, 0}；RAX = BOOL。
    movq %xmm0, %rdx
    movd %xmm2, (%rdx)
    psrldq $8, %xmm0
    psrldq $8, %xmm2
    movq %xmm0, %rdx
    movq %xmm2, (%rdx)
    movq %xmm1, %rdx
    movq %xmm3, (%rdx)
    ret
.globl _guest_io_packet_x64_end
_guest_io_packet_x64_end:
.p2align 5
.globl _guest_io_timeout_x64
_guest_io_timeout_x64:
    movq %xmm0, %rdx
    movq $0, (%rdx)
    ret
.globl _guest_io_timeout_x64_end
_guest_io_timeout_x64_end:
.globl _guest_io_end
_guest_io_end:
