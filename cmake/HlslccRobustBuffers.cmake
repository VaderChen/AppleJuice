# D3D11 的緩衝存取越界是安全的：讀回 0、寫入丟棄，未綁定的 view 長度視為 0。Metal 的 device 指標沒有這層保護，
# 負值或過大的索引會寫到其他資源（UE5 Skin Cache 的切線累加把泛光核心貼圖寫壞、造成頁面錯誤與 GPU 卡死）。
# 原子操作與 raw／structured 讀寫一律以 x64arm_buffer_views 的 view 長度檢查；THREAD_GROUP_SHARED_MEMORY 不受影響。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[            psContext->AddIndentation();
            glsl << TranslateOperand(psDest, TO_FLAG_DESTINATION | TO_FLAG_NAME_ONLY);

            if (psDestAddr)
            {
                bcatcstr(glsl, "[");
                glsl << TranslateOperand(psDestAddr, TO_FLAG_INTEGER | TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X);
                bcatcstr(glsl, "].value");
            }]=]
  [=[            psContext->AddIndentation();
            if (psDest->eType == OPERAND_TYPE_UNORDERED_ACCESS_VIEW)
            {
                const unsigned x64View = psDest->ui32RegisterNumber + 128;
                bcatcstr(glsl, "if (");
                if (psDestAddr)
                {
                    bcatcstr(glsl, "uint(");
                    glsl << TranslateOperand(psDestAddr, TO_FLAG_INTEGER | TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X);
                    bformata(glsl, ") < x64arm_buffer_views[%u].y && ", x64View);
                }
                if (psInst->eOpcode == OPCODE_STORE_UAV_TYPED)
                {
                    bcatcstr(glsl, "uint(");
                    glsl << TranslateOperand(psDestByteOff, dstOffFlag, OPERAND_4_COMPONENT_MASK_X);
                    bformata(glsl, ") < x64arm_buffer_views[%u].y", x64View);
                }
                else
                {
                    bcatcstr(glsl, "(uint(");
                    glsl << TranslateOperand(psDestByteOff, dstOffFlag, OPERAND_4_COMPONENT_MASK_X);
                    if (psDestAddr)
                        bformata(glsl, ") >> 2u) + %du < x64arm_buffer_views[%u].z / 4u", component, x64View);
                    else
                        bformata(glsl, ") >> 2u) + %du < x64arm_buffer_views[%u].y", component, x64View);
                }
                bcatcstr(glsl, ") ");
            }
            glsl << TranslateOperand(psDest, TO_FLAG_DESTINATION | TO_FLAG_NAME_ONLY);

            if (psDestAddr)
            {
                bcatcstr(glsl, "[");
                glsl << TranslateOperand(psDestAddr, TO_FLAG_INTEGER | TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X);
                bcatcstr(glsl, "].value");
            }]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[        glsl << TranslateOperand(psSrc, TO_FLAG_NAME_ONLY);

        if (psSrcAddr)
        {
            bcatcstr(glsl, "[");
            glsl << TranslateOperand(psSrcAddr, TO_FLAG_UNSIGNED_INTEGER | TO_FLAG_INTEGER, OPERAND_4_COMPONENT_MASK_X);
            bcatcstr(glsl, "].value");
        }]=]
  [=[        const bool x64Guarded = psSrc->eType == OPERAND_TYPE_UNORDERED_ACCESS_VIEW || psSrc->eType == OPERAND_TYPE_RESOURCE;
        if (x64Guarded)
        {
            const unsigned x64View = psSrc->ui32RegisterNumber + (psSrc->eType == OPERAND_TYPE_UNORDERED_ACCESS_VIEW ? 128 : 0);
            const int x64Word = psSrc->eSelMode == OPERAND_4_COMPONENT_SWIZZLE_MODE ? psSrc->aui32Swizzle[component] : component;
            bcatcstr(glsl, "((");
            if (psSrcAddr)
            {
                bcatcstr(glsl, "uint(");
                glsl << TranslateOperand(psSrcAddr, TO_FLAG_UNSIGNED_INTEGER | TO_FLAG_INTEGER, OPERAND_4_COMPONENT_MASK_X);
                bformata(glsl, ") < x64arm_buffer_views[%u].y && ", x64View);
            }
            if (psInst->eOpcode == OPCODE_LD_UAV_TYPED)
            {
                bcatcstr(glsl, "uint(");
                glsl << TranslateOperand(psSrcByteOff, srcOffFlag, OPERAND_4_COMPONENT_MASK_X);
                bformata(glsl, ") < x64arm_buffer_views[%u].y", x64View);
            }
            else
            {
                bcatcstr(glsl, "(uint(");
                glsl << TranslateOperand(psSrcByteOff, srcOffFlag, OPERAND_4_COMPONENT_MASK_X);
                if (psSrcAddr)
                    bformata(glsl, ") >> 2u) + %du < x64arm_buffer_views[%u].z / 4u", x64Word, x64View);
                else
                    bformata(glsl, ") >> 2u) + %du < x64arm_buffer_views[%u].y", x64Word, x64View);
            }
            bcatcstr(glsl, ") ? ");
        }
        glsl << TranslateOperand(psSrc, TO_FLAG_NAME_ONLY);

        if (psSrcAddr)
        {
            bcatcstr(glsl, "[");
            glsl << TranslateOperand(psSrcAddr, TO_FLAG_UNSIGNED_INTEGER | TO_FLAG_INTEGER, OPERAND_4_COMPONENT_MASK_X);
            bcatcstr(glsl, "].value");
        }]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[                bcatcstr(glsl, "u");
        }
        bcatcstr(glsl, "]");

        if (addedBitcast)
            bcatcstr(glsl, ")");
    }
    AddAssignPrologue(numParenthesis);]=]
  [=[                bcatcstr(glsl, "u");
        }
        bcatcstr(glsl, "]");
        if (x64Guarded)
            bcatcstr(glsl, " : 0u)");

        if (addedBitcast)
            bcatcstr(glsl, ")");
    }
    AddAssignPrologue(numParenthesis);]=])
# 原子操作：比較交換越界時舊值為 0；其他操作越界時不執行，舊值為 0。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[    if (compare)
    {
        bcatcstr(glsl, "{\n");
        ++psContext->indent;
        psContext->AddIndentation();
        bcatcstr(glsl, "uint compare_value = ");]=]
  [=[    std::string x64Guard;
    if (dest->eType == OPERAND_TYPE_UNORDERED_ACCESS_VIEW)
    {
        const std::string x64View = "x64arm_buffer_views[" + std::to_string(dest->ui32RegisterNumber + 128) + "]";
        const uint32_t x64AddrFlag = destAddr->GetDataType(psContext) == SVT_INT || destAddr->GetDataType(psContext) == SVT_INT16 || destAddr->GetDataType(psContext) == SVT_INT12 ? TO_FLAG_INTEGER : TO_FLAG_UNSIGNED_INTEGER;
        const std::string x64First = "uint(" + std::string(TranslateOperand(destAddr, x64AddrFlag, OPERAND_4_COMPONENT_MASK_X)) + ")";
        if (psBinding && psBinding->eType == RTYPE_UAV_RWTYPED)
            x64Guard = x64First + " < " + x64View + ".y";
        else if (destAddr->GetNumSwizzleElements(OPERAND_4_COMPONENT_MASK_X | OPERAND_4_COMPONENT_MASK_Y) == 2)
            x64Guard = x64First + " < " + x64View + ".y && (uint(" + std::string(TranslateOperand(destAddr, x64AddrFlag, OPERAND_4_COMPONENT_MASK_Y)) + ") >> 2u) < " + x64View + ".z / 4u";
        else
            x64Guard = "(" + x64First + " >> 2u) < " + x64View + ".y";
    }
    if (compare)
    {
        bcatcstr(glsl, "{\n");
        ++psContext->indent;
        psContext->AddIndentation();
        bcatcstr(glsl, "uint compare_value = ");]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[        bcatcstr(glsl, "const uint x64_compare_expected = compare_value;\n");
        psContext->AddIndentation();
        bcatcstr(glsl, "while (!");
    }
    else if (previousValue)
        AddAssignToDest(previousValue, isUint ? SVT_UINT : SVT_INT, 1, psInst->ui32PreciseMask, numParenthesis);

    bcatcstr(glsl, func);]=]
  [=[        bcatcstr(glsl, "const uint x64_compare_expected = compare_value;\n");
        psContext->AddIndentation();
        if (!x64Guard.empty())
            bformata(glsl, "if (%s) ", x64Guard.c_str());
        bcatcstr(glsl, "while (!");
    }
    else if (previousValue)
    {
        AddAssignToDest(previousValue, isUint ? SVT_UINT : SVT_INT, 1, psInst->ui32PreciseMask, numParenthesis);
        if (!x64Guard.empty())
            bformata(glsl, "((%s) ? ", x64Guard.c_str());
    }
    else if (!x64Guard.empty())
        bformata(glsl, "if (%s) ", x64Guard.c_str());

    bcatcstr(glsl, func);]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[    if (compare)
        bcatcstr(glsl, " && compare_value == x64_compare_expected) {}\n");
    else if (previousValue)
    {
        AddAssignPrologue(numParenthesis);
    }]=]
  [=[    if (compare)
    {
        bcatcstr(glsl, " && compare_value == x64_compare_expected) {}\n");
        if (!x64Guard.empty())
        {
            psContext->AddIndentation();
            bcatcstr(glsl, "else compare_value = 0u;\n");
        }
    }
    else if (previousValue)
    {
        if (!x64Guard.empty())
            bcatcstr(glsl, " : 0u)");
        AddAssignPrologue(numParenthesis);
    }]=])
