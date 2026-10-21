# DXBC temp 各 lane 的資料型別彼此獨立；依實際讀取分量組成 Metal 向量。
# 原實作以未讀取的 swizzle[0] 選擇整個變數，會把 float 的 yzw 誤讀自 int3。
x64arm_hlslcc_patch(toMetalOperand.cpp
  [=[    uint32_t ui32IgnoreSwizzle = 0;
    int iRebase = 0;]=]
  [=[    if (psOperand->eType == OPERAND_TYPE_TEMP && psOperand->iNumComponents == 4 &&
        psOperand->eSelMode != OPERAND_4_COMPONENT_SELECT_1_MODE &&
        !(ui32TOFlag & (TO_FLAG_NAME_ONLY | TO_FLAG_DESTINATION | TO_FLAG_DECLARATION_NAME |
                       TO_AUTO_EXPAND_TO_VEC2 | TO_AUTO_EXPAND_TO_VEC3 | TO_AUTO_EXPAND_TO_VEC4)))
    {
        const unsigned mask = psOperand->eSelMode == OPERAND_4_COMPONENT_MASK_MODE ?
            psOperand->ui32CompMask & ui32ComponentMask : ui32ComponentMask;
        const unsigned count = HLSLcc::GetNumberBitsSet(mask);
        if (count)
        {
            std::ostringstream parts;
            if (count > 1) parts << GetConstructorForTypeMetal(rawType, count) << "(";
            bool first = true;
            for (unsigned lane = 0; lane < 4; ++lane)
            {
                if (!(mask & (1u << lane))) continue;
                Operand scalar = *psOperand;
                const unsigned component = psOperand->eSelMode == OPERAND_4_COMPONENT_SWIZZLE_MODE ?
                    psOperand->aui32Swizzle[lane] : lane;
                scalar.eSelMode = OPERAND_4_COMPONENT_SELECT_1_MODE;
                scalar.aui32Swizzle[0] = component;
                scalar.ui32Swizzle = component;
                if (!first) parts << ", ";
                first = false;
                parts << TranslateOperand(&scalar, ui32TOFlag, OPERAND_4_COMPONENT_MASK_ALL);
            }
            if (count > 1) parts << ")";
            return parts.str();
        }
    }
    uint32_t ui32IgnoreSwizzle = 0;
    int iRebase = 0;]=])
