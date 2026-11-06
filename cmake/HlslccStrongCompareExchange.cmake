# D3D 的 InterlockedCompareExchange 是強比較交換；MSL 只有 weak，假性失敗時 compare_value 不變，會被當成交換成功，
# 客體用它實作的互斥鎖就失效。假性失敗（舊值仍等於期望值）時重試。貼圖原子操作已於 HlslccTextureAtomics.cmake 處理。
# HlslccRobustBuffers.cmake 會再改寫這兩段，重新設定時新舊片段都找不到；來源已含 x64_compare_expected 就代表已修補，不重做。
file(READ "${hlslcc_SOURCE_DIR}/src/toMetalInstruction.cpp" x64arm_strong_cas_source)
string(FIND "${x64arm_strong_cas_source}" "x64_compare_expected" x64arm_strong_cas_applied)
if(x64arm_strong_cas_applied EQUAL -1)
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[        bcatcstr(glsl, "uint compare_value = ");
        glsl << TranslateOperand(compare, ui32DataTypeFlag);
        bcatcstr(glsl, ";\n");
        psContext->AddIndentation();
    }
    else if (previousValue)
        AddAssignToDest(previousValue, isUint ? SVT_UINT : SVT_INT, 1, psInst->ui32PreciseMask, numParenthesis);

    bcatcstr(glsl, func);]=]
  [=[        bcatcstr(glsl, "uint compare_value = ");
        glsl << TranslateOperand(compare, ui32DataTypeFlag);
        bcatcstr(glsl, ";\n");
        psContext->AddIndentation();
        bcatcstr(glsl, "const uint x64_compare_expected = compare_value;\n");
        psContext->AddIndentation();
        bcatcstr(glsl, "while (!");
    }
    else if (previousValue)
        AddAssignToDest(previousValue, isUint ? SVT_UINT : SVT_INT, 1, psInst->ui32PreciseMask, numParenthesis);

    bcatcstr(glsl, func);]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[    if (compare)
        bcatcstr(glsl, ", memory_order::memory_order_relaxed");
    bcatcstr(glsl, ")");
    if (previousValue)
    {
        AddAssignPrologue(numParenthesis);
    }]=]
  [=[    if (compare)
        bcatcstr(glsl, ", memory_order::memory_order_relaxed");
    bcatcstr(glsl, ")");
    if (compare)
        bcatcstr(glsl, " && compare_value == x64_compare_expected) {}\n");
    else if (previousValue)
    {
        AddAssignPrologue(numParenthesis);
    }]=])
endif()
