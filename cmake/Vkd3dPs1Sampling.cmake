# SM1 的取樣器由目的暫存器隱含指定；不能直接輸出 SM2 的雙來源 texld。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/d3dbc.c"
  [=[        case VSIR_OP_SLT:
        case VSIR_OP_TEXLD:]=]
  [=[        case VSIR_OP_SLT:
        case VSIR_OP_TEX:
        case VSIR_OP_TEXLD:]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c"
  [=[    if (ctx->profile->major_version == 1 && ctx->profile->type == VKD3D_SHADER_TYPE_PIXEL)
        ctx->temp_count = 1;]=]
  [=[    if (ctx->profile->major_version == 1 && ctx->profile->type == VKD3D_SHADER_TYPE_PIXEL)
    {
        ctx->temp_count = 1;
        if (ctx->profile->minor_version == 4)
            LIST_FOR_EACH_ENTRY(var, &ctx->extern_vars, struct hlsl_ir_var, extern_entry)
                if (var->regs[HLSL_REGSET_SAMPLERS].allocated)
                    ctx->temp_count = max(ctx->temp_count, var->regs[HLSL_REGSET_SAMPLERS].id
                            + var->bind_count[HLSL_REGSET_SAMPLERS]);
    }]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/d3dbc.c"
  [=[int d3dbc_compile(struct vsir_program *program, uint64_t config_flags,]=]
  [=[static int d3dbc_lower_ps1_samples(struct vsir_program *program,
        struct vkd3d_shader_message_context *messages)
{
    const struct vkd3d_shader_version *version = &program->shader_version;
    struct vsir_program_iterator it = vsir_program_iterator(&program->instructions);
    struct vkd3d_shader_instruction *ins, *move;
    unsigned int sampled = 0;
    if (version->type != VKD3D_SHADER_TYPE_PIXEL || version->major != 1)
        return VKD3D_OK;
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
    {
        struct vsir_dst_operand destination;
        unsigned int sampler;
        if (ins->opcode != VSIR_OP_TEXLD || ins->src_count != 2)
            continue;
        sampler = ins->src[1].reg.idx[0].offset;
        /* SSA 的單純座標複製可回溯到原始 t 暫存器，保留實際 swizzle。 */
        for (unsigned int depth = 0; depth < 64 && ins->src[0].reg.type == VSIR_REGISTER_SSA; ++depth)
        {
            struct vsir_program_iterator search = it;
            struct vkd3d_shader_instruction *definition;
            bool found = false;
            while ((definition = vsir_program_iterator_prev(&search)))
            {
                if (definition->dst_count == 1 && definition->dst[0].reg.type == VSIR_REGISTER_SSA
                        && definition->dst[0].reg.idx[0].offset == ins->src[0].reg.idx[0].offset)
                {
                    if (definition->opcode == VSIR_OP_MOV && definition->src_count == 1
                            && !definition->src[0].modifiers && !ins->src[0].modifiers)
                    {
                        uint32_t swizzle = 0;
                        for (unsigned int c = 0; c < 4; ++c)
                            vsir_swizzle_set_component(&swizzle, c, vsir_swizzle_get_component(definition->src[0].swizzle,
                                    vsir_swizzle_get_component(ins->src[0].swizzle, c)));
                        ins->src[0] = definition->src[0];
                        ins->src[0].swizzle = swizzle;
                        found = true;
                    }
                    break;
                }
            }
            if (!found)
                break;
        }
        if (sampler >= (version->minor == 4 ? 6 : 4) || ins->flags || ins->src[0].modifiers
                || vsir_swizzle_get_component(ins->src[0].swizzle, 0) != 0
                || vsir_swizzle_get_component(ins->src[0].swizzle, 1) != 1
                || ins->src[0].reg.type != VSIR_REGISTER_TEXTURE
                || (version->minor < 4 && ins->src[0].reg.idx[0].offset != sampler))
        {
            vkd3d_shader_error(messages, &ins->location, VKD3D_SHADER_ERROR_D3DBC_NOT_IMPLEMENTED,
                    "SM1 sampling requires an unmodified texture coordinate register (type %u, stage %u).",
                    ins->src[0].reg.type, sampler);
            return VKD3D_ERROR_NOT_IMPLEMENTED;
        }
        destination = ins->dst[0];
        if (version->minor == 4)
        {
            if (!vsir_program_iterator_insert_after(&it, 1))
                return VKD3D_ERROR_OUT_OF_MEMORY;
            ins = vsir_program_iterator_current(&it);
            ins->src_count = 1;
            vsir_dst_operand_init(&ins->dst[0], VSIR_REGISTER_TEMP, VSIR_DATA_F32, 1);
            ins->dst[0].reg.dimension = VSIR_DIMENSION_VEC4;
            ins->dst[0].reg.idx[0].offset = sampler;
            ins->dst[0].write_mask = VKD3DSP_WRITEMASK_ALL;
            move = vsir_program_iterator_next(&it);
            if (!vsir_instruction_init_with_params(program, move, &ins->location, VSIR_OP_MOV, 1, 1))
                return VKD3D_ERROR_OUT_OF_MEMORY;
        }
        else
        {
            sampled |= 1u << sampler;
            move = ins;
            move->opcode = VSIR_OP_MOV;
            move->src_count = 1;
        }
        move->dst[0] = destination;
        vsir_src_operand_init(&move->src[0], version->minor == 4 ? VSIR_REGISTER_TEMP : VSIR_REGISTER_TEXTURE,
                VSIR_DATA_F32, 1);
        move->src[0].reg.dimension = VSIR_DIMENSION_VEC4;
        move->src[0].reg.idx[0].offset = sampler;
        move->src[0].swizzle = VKD3D_SHADER_NO_SWIZZLE;
    }
    /* ps_1_1–1_3 的取樣先於算術；每個 t 暫存器只取樣一次。 */
    for (unsigned int sampler = 0; sampler < 4; ++sampler)
    {
        struct vsir_program_iterator inserted;
        if (!(sampled & (1u << sampler)))
            continue;
        vsir_program_iterator_head(&it);
        if (!(ins = vsir_program_iterator_insert_before(&it, &inserted, 1)))
            return VKD3D_ERROR_OUT_OF_MEMORY;
        if (!vsir_instruction_init_with_params(program, ins, &(struct vkd3d_shader_location){0}, VSIR_OP_TEX, 1, 0))
            return VKD3D_ERROR_OUT_OF_MEMORY;
        vsir_dst_operand_init(&ins->dst[0], VSIR_REGISTER_TEXTURE, VSIR_DATA_F32, 1);
        ins->dst[0].reg.dimension = VSIR_DIMENSION_VEC4;
        ins->dst[0].reg.idx[0].offset = sampler;
        ins->dst[0].write_mask = VKD3DSP_WRITEMASK_ALL;
    }
    return VKD3D_OK;
}

int d3dbc_compile(struct vsir_program *program, uint64_t config_flags,]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/d3dbc.c"
  [=[    if ((result = vsir_program_allocate_temp_registers(program, config_flags, compile_info, message_context)) < 0)]=]
  [=[    if ((result = d3dbc_lower_ps1_samples(program, message_context)) < 0)
        return result;
    if ((result = vsir_program_allocate_temp_registers(program, config_flags, compile_info, message_context)) < 0)]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[    bool ps_1_x;
};]=]
  [=[    bool ps_1_x;
    unsigned int ps_1_fixed_count;
};]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[            if (reg->idx == 0 && allocator->ps_1_x)]=]
  [=[            if (reg->idx < allocator->ps_1_fixed_count && allocator->ps_1_x)]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[                reg->force_first = true;
                liveness->ssa_regs[i].first_write = 0;]=]
  [=[                reg->force_first = true;
                liveness->ssa_regs[i].written = true;
                liveness->ssa_regs[i].mask = VKD3DSP_WRITEMASK_ALL;
                liveness->ssa_regs[i].fixed_mask = true;
                liveness->ssa_regs[i].first_write = 0;]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[    if ((ret = vkd3d_u32_compare((*reg1)->force_first, (*reg2)->force_first)))
        return -ret;]=]
  [=[    if ((ret = vkd3d_u32_compare((*reg1)->force_first, (*reg2)->force_first)))
        return -ret;
    if ((*reg1)->force_first && (*reg2)->force_first)
        return vkd3d_u32_compare((*reg1)->idx, (*reg2)->idx);]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[    if (program->shader_version.type == VKD3D_SHADER_TYPE_PIXEL && program->shader_version.major < 2)
        allocator.ps_1_x = true;]=]
  [=[    if (program->shader_version.type == VKD3D_SHADER_TYPE_PIXEL && program->shader_version.major < 2)
    {
        allocator.ps_1_x = true;
        allocator.ps_1_fixed_count = 1;
        if (program->shader_version.minor == 4)
            for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
                if (ins->opcode == VSIR_OP_TEXLD && ins->dst_count && ins->dst[0].reg.type == VSIR_REGISTER_TEMP)
                    allocator.ps_1_fixed_count = max(allocator.ps_1_fixed_count, ins->dst[0].reg.idx[0].offset + 1);
    }]=])
