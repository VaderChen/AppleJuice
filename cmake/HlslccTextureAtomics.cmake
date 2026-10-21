# Metal 3.1 的整數紋理原子操作；保留 UAV 座標、signedness 與舊值回傳。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[    if (texDim > 0)
    {
        psContext->m_Reflection.OnDiagnostics("Texture atomics are not supported in Metal", 0, true);
        return;
    }]=]
  [=[    if (texDim > 0)
    {
        const auto dimension = psBinding->eDimension;
        const bool array = dimension == REFLECT_RESOURCE_DIMENSION_TEXTURE1DARRAY || dimension == REFLECT_RESOURCE_DIMENSION_TEXTURE2DARRAY;
        if (dimension != REFLECT_RESOURCE_DIMENSION_TEXTURE1D && dimension != REFLECT_RESOURCE_DIMENSION_TEXTURE1DARRAY &&
            dimension != REFLECT_RESOURCE_DIMENSION_TEXTURE2D && dimension != REFLECT_RESOURCE_DIMENSION_TEXTURE2DARRAY &&
            dimension != REFLECT_RESOURCE_DIMENSION_TEXTURE3D)
        {
            psContext->m_Reflection.OnDiagnostics("Unsupported atomic UAV texture dimension", 0, true);
            return;
        }
        const unsigned coordinates = texDim - (array ? 1 : 0);
        const unsigned coordMask = (1u << coordinates) - 1;
        const uint32_t valueFlags = isUint ? TO_FLAG_UNSIGNED_INTEGER | TO_AUTO_BITCAST_TO_UINT : TO_FLAG_INTEGER | TO_AUTO_BITCAST_TO_INT;
        const char* vectorType = isUint ? "uint4" : "int4";
        const std::string texture = TranslateOperand(dest, TO_FLAG_DESTINATION | TO_FLAG_NAME_ONLY);
        std::string address = TranslateOperand(destAddr, TO_FLAG_UNSIGNED_INTEGER | TO_AUTO_BITCAST_TO_UINT, coordMask);
        if (array) address += ", " + TranslateOperand(destAddr, TO_FLAG_UNSIGNED_INTEGER | TO_AUTO_BITCAST_TO_UINT, 1u << coordinates);
        const std::string operand = std::string(vectorType) + "(" + TranslateOperand(src, valueFlags, OPERAND_4_COMPONENT_MASK_X) + ")";
        if (compare)
        {
            const std::string expected = TranslateOperand(compare, valueFlags, OPERAND_4_COMPONENT_MASK_X);
            bcatcstr(glsl, "{\n");
            bformata(glsl, "%s x64Expected = %s(%s);\n", vectorType, vectorType, expected.c_str());
            // weak CAS 可虛假失敗；D3D CompareExchange 必須重試至交換或實值不符。
            bformata(glsl, "while (!%s.atomic_compare_exchange_weak(%s, &x64Expected, %s) && x64Expected.x == %s) {}\n",
                texture.c_str(), address.c_str(), operand.c_str(), expected.c_str());
            if (previousValue)
            {
                AddAssignToDest(previousValue, isUint ? SVT_UINT : SVT_INT, 1, psInst->ui32PreciseMask, numParenthesis);
                bcatcstr(glsl, "x64Expected.x");
                AddAssignPrologue(numParenthesis);
            }
            bcatcstr(glsl, "}\n");
        }
        else
        {
            if (previousValue) AddAssignToDest(previousValue, isUint ? SVT_UINT : SVT_INT, 1, psInst->ui32PreciseMask, numParenthesis);
            std::string method(func);
            const auto suffix = method.find("_explicit");
            if (suffix != std::string::npos) method.resize(suffix);
            bformata(glsl, "%s.%s(%s, %s).x", texture.c_str(), method.c_str(), address.c_str(), operand.c_str());
            if (previousValue) AddAssignPrologue(numParenthesis); else bcatcstr(glsl, ";\n");
        }
        return;
    }]=])
