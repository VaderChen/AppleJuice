# 深度比較值、LOD 與 bias 是純量；DXBC 可用 xxxx swizzle 表示純量來源。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[        bcatcstr(glsl, ", saturate("); // TODO: why the saturate here?
        glsl << TranslateOperand(psSrcRef, TO_AUTO_BITCAST_TO_FLOAT);
        bcatcstr(glsl, ")");]=]
  [=[        bcatcstr(glsl, ", ");
        glsl << TranslateOperand(psSrcRef, TO_AUTO_BITCAST_TO_FLOAT, OPERAND_4_COMPONENT_MASK_X);]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  "TranslateOperand(psSrcLOD, TO_AUTO_BITCAST_TO_FLOAT)"
  "TranslateOperand(psSrcLOD, TO_AUTO_BITCAST_TO_FLOAT, psContext->psShader->ui32MajorVersion < 4 ? OPERAND_4_COMPONENT_MASK_ALL : OPERAND_4_COMPONENT_MASK_X)")
