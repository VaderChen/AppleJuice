# 細分管線把頂點與外殼著色器各自的翻譯結果合併成同一份 Metal 原始碼（共用 GLSLCrossDependencyData）。
# HLSLcc 的輔助函式（BFI、UBFE、firstBit_* 等）只在單次翻譯內去重，兩個階段都用到時合併後會重複定義而編譯失敗。
# 以前置處理器防護包住每個輔助函式，第二份自動略過；單一階段的輸出不受影響。
x64arm_hlslcc_patch(toMetal.cpp
  [==[    std::for_each(m_FunctionDefinitions.begin(), m_FunctionDefinitions.end(), [&glsl](const FunctionDefinitions::value_type &p)
    {
        bcatcstr(glsl, p.second.c_str());
        bcatcstr(glsl, "\n");
    });]==]
  [==[    std::for_each(m_FunctionDefinitions.begin(), m_FunctionDefinitions.end(), [&glsl](const FunctionDefinitions::value_type &p)
    {
        const std::string guard = "X64ARM_EXTRA_FUNCTION_" + p.first;
        bformata(glsl, "\n#ifndef %s\n#define %s\n", guard.c_str(), guard.c_str());
        bcatcstr(glsl, p.second.c_str());
        bcatcstr(glsl, "\n#endif\n");
    });]==])
