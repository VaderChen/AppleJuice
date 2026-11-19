# UBFE／IBFE／BFI 的參數逐分量運算，純量來源要明確廣播為目的向量。
# width 與 offset 依 SM5 契約只取低五位；所有中間位移限制在 0～31。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[            bcatcstr(glsl, "(");
            glsl << TranslateOperand(&psInst->asOperands[1], ui32Flags | TO_FLAG_UNSIGNED_INTEGER, destMask);
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[2], ui32Flags | TO_FLAG_UNSIGNED_INTEGER, destMask);
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[3], ui32Flags | (isUBFE ? TO_FLAG_UNSIGNED_INTEGER : TO_FLAG_INTEGER), destMask);
            bcatcstr(glsl, ")");]=]
  [=[            bcatcstr(glsl, "(");
            const auto components = psInst->asOperands[0].GetNumSwizzleElements();
            const auto argument = [&](unsigned source, bool signedValue, bool limited) {
                const auto constructor = GetConstructorForTypeMetal(signedValue ? SVT_INT : SVT_UINT, components);
                if (limited) bcatcstr(glsl, "(");
                bcatcstr(glsl, constructor);
                bcatcstr(glsl, "(");
                glsl << TranslateOperand(&psInst->asOperands[source], ui32Flags | (signedValue ? TO_FLAG_INTEGER : TO_FLAG_UNSIGNED_INTEGER), destMask);
                bcatcstr(glsl, ")");
                if (limited) { bcatcstr(glsl, " & "); bcatcstr(glsl, constructor); bcatcstr(glsl, "(31u))"); }
            };
            argument(1, false, true);
            bcatcstr(glsl, ", ");
            argument(2, false, true);
            bcatcstr(glsl, ", ");
            argument(3, !isUBFE, false);
            bcatcstr(glsl, ")");]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[            bcatcstr(glsl, "bitFieldInsert(");
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, destMask);
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[2], TO_FLAG_UNSIGNED_INTEGER, destMask);
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[3], TO_FLAG_UNSIGNED_INTEGER, destMask);
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[4], TO_FLAG_UNSIGNED_INTEGER, destMask);
            bcatcstr(glsl, ")");]=]
  [=[            bcatcstr(glsl, "bitFieldInsert(");
            const auto constructor = GetConstructorForTypeMetal(SVT_UINT, psInst->asOperands[0].GetNumSwizzleElements());
            for (unsigned source = 1; source <= 4; ++source)
            {
                if (source > 1) bcatcstr(glsl, ", ");
                if (source <= 2) bcatcstr(glsl, "(");
                bcatcstr(glsl, constructor);
                bcatcstr(glsl, "(");
                glsl << TranslateOperand(&psInst->asOperands[source], TO_FLAG_UNSIGNED_INTEGER, destMask);
                bcatcstr(glsl, ")");
                if (source <= 2) { bcatcstr(glsl, " & "); bcatcstr(glsl, constructor); bcatcstr(glsl, "(31u))"); }
            }
            bcatcstr(glsl, ")");]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[uint clampVersion = src << (32-(width+offset));]=]
  [=[uint clampVersion = src << ((32-(width+offset)) & 31u);]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[vec<uint, N> clampVersion = src << (32-(width+offset));]=]
  [=[vec<uint, N> clampVersion = src << ((32-(width+offset)) & 31u);]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[int clampVersion = src << (32-(width+offset));]=]
  [=[int clampVersion = as_type<int>(as_type<uint>(src) << ((32-(width+offset)) & 31u));]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[vec<int, N> clampVersion = src << (32-(width+offset));]=]
  [=[vec<int, N> clampVersion = as_type<vec<int, N>>(as_type<vec<uint, N>>(src) << ((32-(width+offset)) & 31u));]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[clampVersion = clampVersion >> (32 - width);]=]
  [=[clampVersion = clampVersion >> ((32 - width) & 31u);]=])
