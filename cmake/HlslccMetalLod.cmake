# DXBC 的 lod 指令（HLSL CalculateLevelOfDetail）在 Metal 沒有 textureQueryLod：
# 以 calculate_clamped_lod／calculate_unclamped_lod 組出 (ClampedLOD, NonClampedLOD, 0, 0)。
# 1D 紋理在 Metal 沒有 LOD 查詢，也只有單一 mip，固定為 0。
file(READ "${HLSLCC_METAL_INST}" X64ARM_LOD_SOURCE)
string(FIND "${X64ARM_LOD_SOURCE}" "x64arm_lod_dim" X64ARM_LOD_PATCHED)
if(X64ARM_LOD_PATCHED LESS 0)
  set(X64ARM_LOD_BEGIN "            //If the core language does not have query-lod feature,")
  set(X64ARM_LOD_END "            //The swizzle on srcResource allows the returned values")
  string(FIND "${X64ARM_LOD_SOURCE}" "${X64ARM_LOD_BEGIN}" X64ARM_LOD_START)
  string(FIND "${X64ARM_LOD_SOURCE}" "${X64ARM_LOD_END}" X64ARM_LOD_STOP)
  if(X64ARM_LOD_START LESS 0 OR X64ARM_LOD_STOP LESS X64ARM_LOD_START)
    message(FATAL_ERROR "找不到 Metal OPCODE_LOD 區段")
  endif()
  string(SUBSTRING "${X64ARM_LOD_SOURCE}" 0 ${X64ARM_LOD_START} X64ARM_LOD_PREFIX)
  string(SUBSTRING "${X64ARM_LOD_SOURCE}" ${X64ARM_LOD_STOP} -1 X64ARM_LOD_SUFFIX)
  set(X64ARM_LOD_BODY [=[            {
                const RESOURCE_DIMENSION x64arm_lod_dim = psContext->psShader->aeResourceDims[psInst->asOperands[2].ui32RegisterNumber];
                if (x64arm_lod_dim == RESOURCE_DIMENSION_TEXTURE1D || x64arm_lod_dim == RESOURCE_DIMENSION_TEXTURE1DARRAY)
                {
                    bcatcstr(glsl, "float4(0.0)");
                }
                else
                {
                    const bool x64arm_lod_volume = x64arm_lod_dim == RESOURCE_DIMENSION_TEXTURE3D || x64arm_lod_dim == RESOURCE_DIMENSION_TEXTURECUBE || x64arm_lod_dim == RESOURCE_DIMENSION_TEXTURECUBEARRAY;
                    const std::string x64arm_lod_texture = TranslateOperand(&psInst->asOperands[2], TO_FLAG_NAME_ONLY);
                    std::string x64arm_lod_sampler = TranslateOperand(&psInst->asOperands[3], TO_FLAG_NAME_ONLY);
                    if (x64arm_lod_texture == x64arm_lod_sampler)
                        x64arm_lod_sampler.insert(0, "sampler");
                    const std::string x64arm_lod_coord = TranslateOperand(&psInst->asOperands[1],
                        TO_AUTO_BITCAST_TO_FLOAT | (x64arm_lod_volume ? TO_AUTO_EXPAND_TO_VEC3 : TO_AUTO_EXPAND_TO_VEC2),
                        OPERAND_4_COMPONENT_MASK_X | OPERAND_4_COMPONENT_MASK_Y | (x64arm_lod_volume ? OPERAND_4_COMPONENT_MASK_Z : 0));
                    bcatcstr(glsl, "float4(");
                    glsl << x64arm_lod_texture;
                    bcatcstr(glsl, ".calculate_clamped_lod(");
                    glsl << x64arm_lod_sampler;
                    bcatcstr(glsl, ", ");
                    glsl << x64arm_lod_coord;
                    bcatcstr(glsl, "), ");
                    glsl << x64arm_lod_texture;
                    bcatcstr(glsl, ".calculate_unclamped_lod(");
                    glsl << x64arm_lod_sampler;
                    bcatcstr(glsl, ", ");
                    glsl << x64arm_lod_coord;
                    bcatcstr(glsl, "), 0.0, 0.0)");
                }
            }

]=])
  file(WRITE "${HLSLCC_METAL_INST}" "${X64ARM_LOD_PREFIX}${X64ARM_LOD_BODY}${X64ARM_LOD_SUFFIX}")
endif()
