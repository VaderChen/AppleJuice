# Typed SRV 使用 Metal texture_buffer 的格式轉換；view 起點、長度由後端提供。
# LD_UAV_TYPED 的 extended resource token 可省略；依宣告反射辨識 buffer。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  "if (psInst->eResDim == RESOURCE_DIMENSION_BUFFER) // Hack typed buffer as raw buf\n            {\n                psSrc->aeDataType[0]"
  "if (psRes->eDimension == REFLECT_RESOURCE_DIMENSION_BUFFER) // 以 UAV 宣告維度為準\n            {\n                psSrc->aeDataType[0]")
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[            // Fake single comp 32bit texel buffers by using raw buffer
            DeclareBufferVariable(psDecl, true, false);]=]
  [=[            // x64arm：由 native texture-buffer view 提供實際格式轉換。
            TranslateResourceTexture(psDecl, 0, TD_2D);]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  "TranslateOperand(psSrcAddr, TO_FLAG_UNSIGNED_INTEGER | TO_FLAG_INTEGER)"
  "TranslateOperand(psSrcAddr, TO_FLAG_UNSIGNED_INTEGER | TO_FLAG_INTEGER, OPERAND_4_COMPONENT_MASK_X)")
x64arm_hlslcc_patch(toMetalInstruction.cpp
  "TranslateOperand(psSrcByteOff, srcOffFlag)"
  "TranslateOperand(psSrcByteOff, srcOffFlag, OPERAND_4_COMPONENT_MASK_X)")
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[        case RESOURCE_DIMENSION_BUFFER:
        {
            oss << "texture1d<" << typeName << ", access::" << access << " >";]=]
  [=[        case RESOURCE_DIMENSION_BUFFER:
        {
            oss << "texture_buffer<" << typeName << ", access::" << access << " >";]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[            if (psInst->eResDim == RESOURCE_DIMENSION_BUFFER) // Hack typed buffer as raw buf
            {
                psInst->eOpcode = OPCODE_LD_UAV_TYPED;
                psInst->asOperands[1].eSelMode = OPERAND_4_COMPONENT_SELECT_1_MODE;
                if (psInst->asOperands[1].eType == OPERAND_TYPE_IMMEDIATE32)
                    psInst->asOperands[1].iNumComponents = 1;
                TranslateShaderStorageLoad(psInst);
                break;
            }]=]
  [=[            // x64arm：typed SRV 依 DXGI view 格式讀取，不改寫成 UAV raw load。]=])
# 兩個 texel-fetch 入口共用這段，包含 immediate-offset 形式。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[    glsl << TranslateOperand(&psInst->asOperands[2], TO_FLAG_NONE);
    bcatcstr(glsl, ".read(");

    switch (psBinding->eDimension)]=]
  [=[    if (psBinding->eDimension == REFLECT_RESOURCE_DIMENSION_BUFFER)
    {
        bcatcstr(glsl, "x64arm_buffer_read(");
        glsl << TranslateOperand(&psInst->asOperands[2], TO_FLAG_NONE);
        bformata(glsl, ", x64arm_buffer_views[%u], uint(", psInst->asOperands[2].ui32RegisterNumber);
        glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X);
        if (psInst->bAddressOffset) bformata(glsl, " + %d", psInst->iUAddrOffset);
        bcatcstr(glsl, "))");
        glsl << TranslateOperandSwizzle(&psInst->asOperands[2], psInst->asOperands[0].GetAccessMask(), 0);
        AddAssignPrologue(numParenthesis);
        return;
    }
    glsl << TranslateOperand(&psInst->asOperands[2], TO_FLAG_NONE);
    bcatcstr(glsl, ".read(");

    switch (psBinding->eDimension)]=])
# BUFINFO 對 typed／structured 回傳元素數，raw 回傳位元組數；未綁定回傳零。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[            psContext->m_Reflection.OnDiagnostics("Metal shading language does not support buffer size query from shader. Pass the size to shader as const instead.\n", 0, true);]=]
  [=[            psContext->AddIndentation();
            AddAssignToDest(&psInst->asOperands[0], SVT_UINT, 1, psInst->ui32PreciseMask, numParenthesis);
            bformata(glsl, "x64arm_buffer_views[%u].w", psInst->asOperands[1].ui32RegisterNumber +
                (psInst->asOperands[1].eType == OPERAND_TYPE_UNORDERED_ACCESS_VIEW ? 128 : 0));
            AddAssignPrologue(numParenthesis);]=])
# 無原子操作的 typed UAV 以 texture_buffer 實作，讓 Metal 執行分量寬度與格式轉換。
# 含原子操作的 R32 buffer 保留既有 atomic pointer 路徑。
x64arm_hlslcc_patch(toMetalDeclaration.cpp
  [=[if (psDecl->value.eResourceDimension == RESOURCE_DIMENSION_BUFFER)
            {
                DeclareBufferVariable(psDecl, true, true);]=]
  [=[if (psDecl->value.eResourceDimension == RESOURCE_DIMENSION_BUFFER && (psDecl->sUAV.ui32AccessFlags & ACCESS_FLAG_ATOMIC))
            {
                DeclareBufferVariable(psDecl, true, true);]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[            if (psRes->eDimension == REFLECT_RESOURCE_DIMENSION_BUFFER) // 以 UAV 宣告維度為準]=]
  [=[            if (psRes->eDimension == REFLECT_RESOURCE_DIMENSION_BUFFER &&
                std::none_of(psContext->psShader->asPhases[psContext->currentPhase].psDecl.begin(), psContext->psShader->asPhases[psContext->currentPhase].psDecl.end(),
                    [&](const Declaration& d) { return d.eOpcode == OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED && d.asOperands[0].ui32RegisterNumber == psSrc->ui32RegisterNumber && (d.sUAV.ui32AccessFlags & ACCESS_FLAG_ATOMIC); }))
            {
                psContext->AddIndentation();
                AddAssignToDest(psDest, srcDataType, 4, psInst->ui32PreciseMask, numParenthesis);
                bcatcstr(glsl, "x64arm_buffer_read(");
                glsl << TranslateOperand(psSrc, TO_FLAG_NAME_ONLY);
                bformata(glsl, ", x64arm_buffer_views[%u], uint(", 128 + psSrc->ui32RegisterNumber);
                glsl << TranslateOperand(psSrcAddr, TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X);
                bcatcstr(glsl, "))");
                glsl << TranslateOperandSwizzle(psSrc, psDest->GetAccessMask(), 0);
                AddAssignPrologue(numParenthesis);
                break;
            }
            if (psRes->eDimension == REFLECT_RESOURCE_DIMENSION_BUFFER) // 以 UAV 宣告維度為準]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[            if (psRes->eDimension == REFLECT_RESOURCE_DIMENSION_BUFFER) // Hack typed buffer as raw buf]=]
  [=[            if (psRes->eDimension == REFLECT_RESOURCE_DIMENSION_BUFFER &&
                std::none_of(psContext->psShader->asPhases[psContext->currentPhase].psDecl.begin(), psContext->psShader->asPhases[psContext->currentPhase].psDecl.end(),
                    [&](const Declaration& d) { return d.eOpcode == OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED && d.asOperands[0].ui32RegisterNumber == psInst->asOperands[0].ui32RegisterNumber && (d.sUAV.ui32AccessFlags & ACCESS_FLAG_ATOMIC); }))
            {
                psContext->AddIndentation();
                bcatcstr(glsl, "x64arm_buffer_write(");
                glsl << TranslateOperand(&psInst->asOperands[0], TO_FLAG_NAME_ONLY);
                bformata(glsl, ", x64arm_buffer_views[%u], uint(", 128 + psInst->asOperands[0].ui32RegisterNumber);
                glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X);
                bcatcstr(glsl, "), ");
                glsl << TranslateOperand(&psInst->asOperands[2], ResourceReturnTypeToFlag(psRes->ui32ReturnType) | TO_AUTO_EXPAND_TO_VEC4);
                bcatcstr(glsl, ");\n");
                break;
            }
            if (psRes->eDimension == REFLECT_RESOURCE_DIMENSION_BUFFER) // Hack typed buffer as raw buf]=])
