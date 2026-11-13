# bfrev／firstbit_hi／firstbit_lo／firstbit_shi：原版把來源整個 4 分量帶入、直接指派給目的，
# 目的只有部分分量時 Metal 會拒絕（例如 int 被指派 int4），目的暫存器型別不同時也不會轉型。
# 改依目的遮罩取來源分量、經 AddAssignToDest 指派（與 BFI 相同的寫法）。
# bfrev 改用 Metal 內建 reverse_bits：原本的輔助函式對帶號整數做算術右移，最高位為 1 時結果錯誤。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [==[            psContext->AddIndentation();
            glsl << TranslateOperand(&psInst->asOperands[0], TO_FLAG_UNSIGNED_INTEGER | TO_FLAG_DESTINATION);
            bcatcstr(glsl, " = firstBit_hi(");
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER);
            bcatcstr(glsl, ");\n");]==]
  [==[            psContext->AddIndentation();
            {
                const uint32_t destMask = psInst->asOperands[0].GetAccessMask();
                AddAssignToDest(&psInst->asOperands[0], SVT_UINT, psInst->asOperands[0].GetNumSwizzleElements(), psInst->ui32PreciseMask, numParenthesis);
                bcatcstr(glsl, "firstBit_hi(");
                glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, destMask);
                bcatcstr(glsl, ")");
                AddAssignPrologue(numParenthesis);
            }]==])

x64arm_hlslcc_patch(toMetalInstruction.cpp
  [==[            psContext->AddIndentation();
            glsl << TranslateOperand(&psInst->asOperands[0], TO_FLAG_UNSIGNED_INTEGER | TO_FLAG_DESTINATION);
            bcatcstr(glsl, " = firstBit_lo(");
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER);
            bcatcstr(glsl, ");\n");]==]
  [==[            psContext->AddIndentation();
            {
                const uint32_t destMask = psInst->asOperands[0].GetAccessMask();
                AddAssignToDest(&psInst->asOperands[0], SVT_UINT, psInst->asOperands[0].GetNumSwizzleElements(), psInst->ui32PreciseMask, numParenthesis);
                bcatcstr(glsl, "firstBit_lo(");
                glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, destMask);
                bcatcstr(glsl, ")");
                AddAssignPrologue(numParenthesis);
            }]==])

x64arm_hlslcc_patch(toMetalInstruction.cpp
  [==[            psContext->AddIndentation();
            glsl << TranslateOperand(&psInst->asOperands[0], TO_FLAG_INTEGER | TO_FLAG_DESTINATION);
            bcatcstr(glsl, " = firstBit_shi(");
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_INTEGER);
            bcatcstr(glsl, ");\n");]==]
  [==[            psContext->AddIndentation();
            {
                const uint32_t destMask = psInst->asOperands[0].GetAccessMask();
                AddAssignToDest(&psInst->asOperands[0], SVT_INT, psInst->asOperands[0].GetNumSwizzleElements(), psInst->ui32PreciseMask, numParenthesis);
                bcatcstr(glsl, "firstBit_shi(");
                glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_INTEGER, destMask);
                bcatcstr(glsl, ")");
                AddAssignPrologue(numParenthesis);
            }]==])

x64arm_hlslcc_patch(toMetalInstruction.cpp
  [==[            DeclareExtraFunction("bitReverse", "template <typename UVecType> UVecType bitReverse(const UVecType input)\n\
\t\t{ UVecType x = input;\n\
\t\t\tx = (((x & 0xaaaaaaaa) >> 1) | ((x & 0x55555555) << 1));\n\
\t\t\tx = (((x & 0xcccccccc) >> 2) | ((x & 0x33333333) << 2));\n\
\t\t\tx = (((x & 0xf0f0f0f0) >> 4) | ((x & 0x0f0f0f0f) << 4));\n\
\t\t\tx = (((x & 0xff00ff00) >> 8) | ((x & 0x00ff00ff) << 8));\n\
\t\t\treturn((x >> 16) | (x << 16));\n\
\t\t}; ");
            psContext->AddIndentation();
            glsl << TranslateOperand(&psInst->asOperands[0], TO_FLAG_INTEGER | TO_FLAG_DESTINATION);
            bcatcstr(glsl, " = bitReverse(");
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_INTEGER);
            bcatcstr(glsl, ");\n");]==]
  [==[            psContext->AddIndentation();
            {
                const uint32_t destMask = psInst->asOperands[0].GetAccessMask();
                AddAssignToDest(&psInst->asOperands[0], SVT_UINT, psInst->asOperands[0].GetNumSwizzleElements(), psInst->ui32PreciseMask, numParenthesis);
                bcatcstr(glsl, "reverse_bits(");
                glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, destMask);
                bcatcstr(glsl, ")");
                AddAssignPrologue(numParenthesis);
            }]==])
