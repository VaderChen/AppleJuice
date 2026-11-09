# DXBC 的 ld2dms 讀取 Texture2DMSArray：Metal 的 texture2d_ms_array 有 read(座標, 陣列索引, 取樣)，宣告已對應，
# 這裡補上 texel fetch 的兩條路徑（一般與帶位移），不再回報「不支援」。
file(READ "${HLSLCC_METAL_INST}" X64ARM_MSARRAY_SOURCE)
string(FIND "${X64ARM_MSARRAY_SOURCE}" "x64arm_msarray_fetch" X64ARM_MSARRAY_PATCHED)
if(X64ARM_MSARRAY_PATCHED LESS 0)
  set(X64ARM_MSARRAY_ERROR [=[        case REFLECT_RESOURCE_DIMENSION_TEXTURE2DMSARRAY:
        {
            psContext->m_Reflection.OnDiagnostics("Multisampled texture arrays not supported in Metal (in texel fetch)", 0, true);
            return;
        }
]=])
  string(FIND "${X64ARM_MSARRAY_SOURCE}" "${X64ARM_MSARRAY_ERROR}" X64ARM_MSARRAY_FIRST)
  if(X64ARM_MSARRAY_FIRST LESS 0)
    message(FATAL_ERROR "找不到 Metal Texture2DMSArray texel fetch 區段")
  endif()
  string(LENGTH "${X64ARM_MSARRAY_ERROR}" X64ARM_MSARRAY_LENGTH)
  string(SUBSTRING "${X64ARM_MSARRAY_SOURCE}" 0 ${X64ARM_MSARRAY_FIRST} X64ARM_MSARRAY_PREFIX)
  math(EXPR X64ARM_MSARRAY_AFTER "${X64ARM_MSARRAY_FIRST}+${X64ARM_MSARRAY_LENGTH}")
  string(SUBSTRING "${X64ARM_MSARRAY_SOURCE}" ${X64ARM_MSARRAY_AFTER} -1 X64ARM_MSARRAY_SUFFIX)
  set(X64ARM_MSARRAY_PLAIN [=[        case REFLECT_RESOURCE_DIMENSION_TEXTURE2DMSARRAY:
        {
            // x64arm_msarray_fetch：座標 .xy、陣列索引 .z、取樣索引在第四個運算元。
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER | TO_AUTO_EXPAND_TO_VEC2, 3 /* .xy */);
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_Z); // Array index
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[3], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X); // Sample index
            break;
        }
]=])
  set(X64ARM_MSARRAY_OFFSET [=[        case REFLECT_RESOURCE_DIMENSION_TEXTURE2DMSARRAY:
        {
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER | TO_AUTO_EXPAND_TO_VEC2, 3 /* .xy */);
            bformata(glsl, "+ ivec2(%d, %d), ", psInst->iUAddrOffset, psInst->iVAddrOffset);
            glsl << TranslateOperand(&psInst->asOperands[1], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_Z); // Array index
            bcatcstr(glsl, ", ");
            glsl << TranslateOperand(&psInst->asOperands[3], TO_FLAG_UNSIGNED_INTEGER, OPERAND_4_COMPONENT_MASK_X); // Sample index
            break;
        }
]=])
  string(FIND "${X64ARM_MSARRAY_SUFFIX}" "${X64ARM_MSARRAY_ERROR}" X64ARM_MSARRAY_SECOND)
  if(X64ARM_MSARRAY_SECOND LESS 0)
    message(FATAL_ERROR "找不到 Metal Texture2DMSArray 帶位移 texel fetch 區段")
  endif()
  string(SUBSTRING "${X64ARM_MSARRAY_SUFFIX}" 0 ${X64ARM_MSARRAY_SECOND} X64ARM_MSARRAY_MIDDLE)
  math(EXPR X64ARM_MSARRAY_AFTER "${X64ARM_MSARRAY_SECOND}+${X64ARM_MSARRAY_LENGTH}")
  string(SUBSTRING "${X64ARM_MSARRAY_SUFFIX}" ${X64ARM_MSARRAY_AFTER} -1 X64ARM_MSARRAY_REST)
  file(WRITE "${HLSLCC_METAL_INST}" "${X64ARM_MSARRAY_PREFIX}${X64ARM_MSARRAY_PLAIN}${X64ARM_MSARRAY_MIDDLE}${X64ARM_MSARRAY_OFFSET}${X64ARM_MSARRAY_REST}")
endif()
