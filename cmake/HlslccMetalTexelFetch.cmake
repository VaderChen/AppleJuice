# LD／LD_MS 的位移只作用於 texel 座標；轉成無號向量後以 32-bit 加法保留負位移的回繞語義。
# 一維紋理沿用 metal_texture1d_shader 的二維 backing，保留 mip 與 array slice。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[bformata(glsl, "+ ivec2(%d, %d), ", psInst->iUAddrOffset, psInst->iVAddrOffset);]=]
  [=[bformata(glsl, "+ uint2(int2(%d, %d)), ", psInst->iUAddrOffset, psInst->iVAddrOffset);]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[bformata(glsl, "+ ivec3(%d, %d, %d), ", psInst->iUAddrOffset, psInst->iVAddrOffset, psInst->iWAddrOffset);]=]
  [=[bformata(glsl, "+ uint3(int3(%d, %d, %d)), ", psInst->iUAddrOffset, psInst->iVAddrOffset, psInst->iWAddrOffset);]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[        case REFLECT_RESOURCE_DIMENSION_TEXTURE1D:
        {
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X);
            break;
        }]=]
  [=[        case REFLECT_RESOURCE_DIMENSION_TEXTURE1D:
        {
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X);
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_W);
            break;
        }]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[        case REFLECT_RESOURCE_DIMENSION_TEXTURE1DARRAY:
        {
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X);
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_W);
            break;
        }]=]
  [=[        case REFLECT_RESOURCE_DIMENSION_TEXTURE1DARRAY:
        {
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X);
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_Y);
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_W);
            break;
        }]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[            bformata(glsl, " + %d", psInst->iUAddrOffset);
            break;
        }]=]
  [=[            bformata(glsl, " + uint(int(%d)), ", psInst->iUAddrOffset);
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_W);
            break;
        }]=])
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[            bformata(glsl, " + %d, ", psInst->iUAddrOffset);

            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_Y);
            break;
        }]=]
  [=[            bformata(glsl, " + uint(int(%d)), ", psInst->iUAddrOffset);
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_Y);
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_W);
            break;
        }]=])
