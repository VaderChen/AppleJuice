# 同一 DXBC 暫存器的分量可有不同型別。MOV 必須依目的 mask 選取來源，
# 不能用未讀取的 x 分量決定整個來源的 Metal 變數。先保存所有來源以保留交疊語義。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[    const SHADER_VARIABLE_TYPE eSrcType = pSrc->GetDataType(psContext, pDest->GetDataType(psContext));
    uint32_t flags = SVTTypeToFlag(eSrcType);]=]
  [=[    if (pSrc->eType == OPERAND_TYPE_TEMP)
    {
        bcatcstr(glsl, "{\n");
        SHADER_VARIABLE_TYPE types[4]{};
        for (unsigned lane = 0; lane < 4; ++lane)
        {
            if (!(writeMask & (1u << lane))) continue;
            Operand source = *pSrc;
            const unsigned component = pSrc->eSelMode == OPERAND_4_COMPONENT_SELECT_1_MODE ? pSrc->aui32Swizzle[0] :
                pSrc->eSelMode == OPERAND_4_COMPONENT_SWIZZLE_MODE ? pSrc->aui32Swizzle[lane] : lane;
            source.eSelMode = OPERAND_4_COMPONENT_SELECT_1_MODE;
            source.aui32Swizzle[0] = component;
            source.ui32Swizzle = component;
            types[lane] = source.GetDataType(psContext);
            bformata(glsl, "auto x64Mov%u = ", lane);
            glsl << TranslateOperand(&source, SVTTypeToFlag(types[lane]));
            bcatcstr(glsl, ";\n");
        }
        for (unsigned lane = 0; lane < 4; ++lane)
        {
            if (!(writeMask & (1u << lane))) continue;
            Operand destination = *pDest;
            destination.eSelMode = OPERAND_4_COMPONENT_MASK_MODE;
            destination.ui32CompMask = 1u << lane;
            int parentheses = 0;
            AddAssignToDest(&destination, types[lane], 1, precise, parentheses);
            bformata(glsl, "x64Mov%u", lane);
            AddAssignPrologue(parentheses);
        }
        bcatcstr(glsl, "}\n");
        return;
    }
    const SHADER_VARIABLE_TYPE eSrcType = pSrc->GetDataType(psContext, pDest->GetDataType(psContext));
    uint32_t flags = SVTTypeToFlag(eSrcType);]=])
