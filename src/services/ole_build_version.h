#pragma once
// 參考實作 的 OLE2 版本字由 rmm（高字）及 rup（低字）組成。
#define X64ARM_OLE_BUILD_VERSION ((23 << 16) | 824)

#ifdef __ASSEMBLER__
entry OleBuildVersion
    mov eax, X64ARM_OLE_BUILD_VERSION
    ret
end OleBuildVersion
#endif
