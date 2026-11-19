# 三種 SM4 整數位移共用逐分量運算，純量來源明確廣播。
# ISHR 一律做有號右移；ISHL 先以無號位元運算，保留完整 32 位結果。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[    uint32_t ui32Flags = SVTTypeToFlag(eDataType);
    bstring glsl = *psContext->currentGLSLString;
    uint32_t destMask = psInst->asOperands[dest].GetAccessMask();]=]
  [=[    const bool x64armIntegerShift = psInst->eOpcode == OPCODE_ISHL || psInst->eOpcode == OPCODE_ISHR || psInst->eOpcode == OPCODE_USHR;
    if (x64armIntegerShift) eDataType = psInst->eOpcode == OPCODE_ISHR ? SVT_INT : SVT_UINT;
    uint32_t ui32Flags = SVTTypeToFlag(eDataType);
    bstring glsl = *psContext->currentGLSLString;
    uint32_t destMask = psInst->asOperands[dest].GetAccessMask();]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[    glsl << TranslateOperand(&psInst->asOperands[src0], ui32Flags, destMask);
    bformata(glsl, " %s ", name);
    glsl << TranslateOperand(&psInst->asOperands[src1], ui32Flags, destMask);]=]
  [=[    if (x64armIntegerShift)
    {
        bcatcstr(glsl, GetConstructorForTypeMetal(eDataType, dstSwizCount));
        bcatcstr(glsl, "(");
        glsl << TranslateOperand(&psInst->asOperands[src0], SVTTypeToFlag(eDataType), destMask);
        bformata(glsl, ") %s (", name);
        const auto shiftType = GetConstructorForTypeMetal(SVT_UINT, dstSwizCount);
        bcatcstr(glsl, shiftType);
        bcatcstr(glsl, "(");
        glsl << TranslateOperand(&psInst->asOperands[src1], TO_FLAG_UNSIGNED_INTEGER, destMask);
        bcatcstr(glsl, ") & ");
        bcatcstr(glsl, shiftType);
        bcatcstr(glsl, "(31u))");
    }
    else
    {
        glsl << TranslateOperand(&psInst->asOperands[src0], ui32Flags, destMask);
        bformata(glsl, " %s ", name);
        glsl << TranslateOperand(&psInst->asOperands[src1], ui32Flags, destMask);
    }]=])
