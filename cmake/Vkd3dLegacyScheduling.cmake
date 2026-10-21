file(READ "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c" legacy_patch_source)
string(FIND "${legacy_patch_source}" "static enum vkd3d_result legacy_resolve_undefined(" legacy_patch_present)
if(legacy_patch_present EQUAL -1)
# 診斷舊式 shader 的暫存器配置；預設不輸出。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[    allocator.new_temp_count = 0;]=]
  [=[    allocator.new_temp_count = 0;
    if (getenv("X64ARM_TRACE_D3D9_ALLOC"))
    {
        fprintf(stderr, "[shader allocation] stage=%u version=%u ssa=%u temp=%u\n",
                program->shader_version.type, program->shader_version.major, program->ssa_count, program->temp_count);
        for (unsigned int i = 0; i < program->ssa_count + program->temp_count; ++i)
        {
            const struct liveness_tracker_reg *r = &tracker.ssa_regs[i];
            if (r->written) fprintf(stderr, "[shader interval] %c%u mask=%x fixed=%u start=%u end=%u\n",
                    i < program->ssa_count ? 's' : 't', i < program->ssa_count ? i : i - program->ssa_count,
                    r->mask, r->fixed_mask, r->first_write, r->last_access);
        }
    }]=])


x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/d3d_asm.c"
  [=[        TRACE("    %.*s", (int)(q - p), p);]=]
  [=[        if (getenv("X64ARM_TRACE_D3D9_ALLOC")) fprintf(stderr, "[shader ir] %.*s", (int)(q - p), p);
        else TRACE("    %.*s", (int)(q - p), p);]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[        fprintf(stderr, "[shader allocation] stage=%u version=%u ssa=%u temp=%u\n",]=]
  [=[        vsir_program_trace(program);
        fprintf(stderr, "[shader allocation] stage=%u version=%u ssa=%u temp=%u\n",]=])

# 分支展開可能引入「第一次寫入前先讀取舊值」；直線程式中的未定義分量取零，
# 讓後續常數傳播消除這些不存在的跨分支資料相依，已有定義的分量完全保留。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[static void vsir_optimize(struct vsir_transformation_context *ctx)]=]
  [=[static enum vkd3d_result legacy_resolve_undefined(struct vsir_program *program, struct vsir_transformation_context *ctx)
{
    struct vsir_program_iterator it = vsir_program_iterator(&program->instructions);
    struct vkd3d_shader_instruction *ins;
    unsigned int *written, zero_register = UINT_MAX, zero_component = 0;
    if (program->shader_version.major != 2 || ctx->compile_info->target_type != VKD3D_SHADER_TARGET_D3D_BYTECODE)
        return VKD3D_OK;
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
    {
        switch (ins->opcode)
        {
            case VSIR_OP_IF: case VSIR_OP_IFC: case VSIR_OP_LOOP: case VSIR_OP_REP:
            case VSIR_OP_LABEL: case VSIR_OP_BRANCH: case VSIR_OP_SWITCH:
            case VSIR_OP_SWITCH_MONOLITHIC: case VSIR_OP_CALL: case VSIR_OP_CALLNZ:
            case VSIR_OP_RETP: return VKD3D_OK;
            default: break;
        }
        for (unsigned int j = 0; j < ins->dst_count; ++j)
            if (ins->dst[j].reg.type == VSIR_REGISTER_TEMP && ins->dst[j].reg.idx[0].rel_addr) return VKD3D_OK;
        if (ins->opcode == VSIR_OP_DEF)
            for (unsigned int c = 0; c < 4; ++c)
                if (ins->src[0].reg.u.immconst_f32[c] == 0.0f)
                { zero_register = ins->dst[0].reg.idx[0].offset; zero_component = c; }
    }
    if (zero_register == UINT_MAX) return VKD3D_OK;
    for (unsigned int pass = 0; pass < 4; ++pass)
    {
        enum vkd3d_result ret = legacy_trim_load_components(program);
        if (ret < 0) return ret;
    }
    if (!(written = vkd3d_calloc(program->temp_count ? program->temp_count : 1, sizeof(*written))))
        return VKD3D_ERROR_OUT_OF_MEMORY;
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
    {
        if (ins->opcode == VSIR_OP_MOV && ins->src_count == 1 && ins->dst_count == 1
                && ins->src[0].reg.type == VSIR_REGISTER_TEMP && !ins->src[0].reg.idx[0].rel_addr)
        {
            unsigned int mask = 0;
            for (unsigned int c = 0; c < 4; ++c)
                if (ins->dst[0].write_mask & (1u << c)) mask |= 1u << vsir_swizzle_get_component(ins->src[0].swizzle, c);
            if (!(mask & written[ins->src[0].reg.idx[0].offset]))
            {
                vsir_src_operand_init(&ins->src[0], VSIR_REGISTER_CONST, VSIR_DATA_F32, 1);
                ins->src[0].reg.dimension = VSIR_DIMENSION_VEC4;
                ins->src[0].reg.idx[0].offset = zero_register;
                ins->src[0].swizzle = 0;
                for (unsigned int c = 0; c < 4; ++c) vsir_swizzle_set_component(&ins->src[0].swizzle, c, zero_component);
                ctx->progress = true;
            }
        }
        for (unsigned int j = 0; j < ins->dst_count; ++j)
            if (ins->dst[j].reg.type == VSIR_REGISTER_TEMP) written[ins->dst[j].reg.idx[0].offset] |= ins->dst[j].write_mask;
    }
    vkd3d_free(written);
    return VKD3D_OK;
}
static void vsir_optimize(struct vsir_transformation_context *ctx)]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[        vsir_transform(ctx, vsir_program_cse);]=]
  [=[        vsir_transform(ctx, legacy_resolve_undefined);
        if (ctx->compile_info->target_type != VKD3D_SHADER_TARGET_D3D_BYTECODE
                || ctx->program->shader_version.major > 2)
            vsir_transform(ctx, vsir_program_cse);]=])

endif()
