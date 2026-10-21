# VS 1–3 的 lit：先讀來源，再寫目的；保留遮罩、別名與指數鉗制語義。
# https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/lit---vs
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/spirv.c"
  [=[static void spirv_compiler_emit_mov(struct spirv_compiler *compiler,]=]
  [=[static void spirv_compiler_emit_lit(struct spirv_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vsir_src_operand *src = instruction->src;
    const struct vsir_dst_operand *dst = instruction->dst;
    uint32_t type = spirv_get_type_id(compiler, VSIR_DATA_F32, 1);
    uint32_t boolean = spirv_get_type_id(compiler, VSIR_DATA_BOOL, 1);
    uint32_t zero = spirv_compiler_get_constant_float(compiler, 0.0f);
    uint32_t one = spirv_compiler_get_constant_float(compiler, 1.0f);
    uint32_t values[4] = {one, zero, zero, one}, packed[4];
    uint32_t x, y, power, positive_x, positive_y, positive, operands[3];
    uint32_t set = vkd3d_spirv_get_glsl_std450_instr_set(builder);
    unsigned int count = 0;
    if (dst->write_mask & (VKD3DSP_WRITEMASK_1 | VKD3DSP_WRITEMASK_2))
    {
        x = spirv_compiler_emit_load_src(compiler, src, VKD3DSP_WRITEMASK_0);
        positive_x = vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
                SpvOpFOrdGreaterThan, boolean, x, zero);
        values[1] = vkd3d_spirv_build_op_select(builder, type, positive_x, x, zero);
        if (dst->write_mask & VKD3DSP_WRITEMASK_2)
        {
            y = spirv_compiler_emit_load_src(compiler, src, VKD3DSP_WRITEMASK_1);
            power = spirv_compiler_emit_load_src(compiler, src, VKD3DSP_WRITEMASK_3);
            positive_y = vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
                    SpvOpFOrdGreaterThan, boolean, y, zero);
            positive = vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
                    SpvOpLogicalAnd, boolean, positive_x, positive_y);
            operands[0] = power;
            operands[1] = spirv_compiler_get_constant_float(compiler, -127.9961f);
            operands[2] = spirv_compiler_get_constant_float(compiler, 127.9961f);
            power = vkd3d_spirv_build_op_ext_inst(builder, type, set, GLSLstd450FClamp, operands, 3);
            /* 非正底數不送入 pow，避免未選中的分支產生未定義運算。 */
            operands[0] = vkd3d_spirv_build_op_select(builder, type, positive_y, y, one);
            operands[1] = power;
            power = vkd3d_spirv_build_op_ext_inst(builder, type, set, GLSLstd450Pow, operands, 2);
            values[2] = vkd3d_spirv_build_op_select(builder, type, positive, power, zero);
        }
    }
    for (unsigned int i = 0; i < 4; ++i)
        if (dst->write_mask & (1u << i)) packed[count++] = values[i];
    spirv_compiler_emit_store_dst_components(compiler, dst, VSIR_DATA_F32, packed);
}

static void spirv_compiler_emit_mov(struct spirv_compiler *compiler,]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/spirv.c"
  [=[        case VSIR_OP_DMOV:
        case VSIR_OP_MOV:]=]
  [=[        case VSIR_OP_LIT:
            spirv_compiler_emit_lit(compiler, instruction);
            break;
        case VSIR_OP_DMOV:
        case VSIR_OP_MOV:]=])
