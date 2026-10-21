file(READ "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c" legacy_patch_source)
string(FIND "${legacy_patch_source}" "static bool legacy_remat_cost(" legacy_patch_present)
if(legacy_patch_present EQUAL -1)
# 為 SM2 的小型唯讀運算樹重新產生使用點附近的值，縮短長生命期向量。
# 只處理直線程式，排除相對位址、可寫暫存來源、predicate 與副作用。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[static enum vkd3d_result vsir_allocate_temp_registers(struct vsir_program *program,]=]
  [=[static bool legacy_remat_cost(const struct vkd3d_shader_instruction *const *defs,
        unsigned int id, unsigned int count, unsigned int *budget)
{
    const struct vkd3d_shader_instruction *ins;
    if (id >= count || !*budget || !(ins = defs[id]) || ins->predicate || ins->dst_count != 1) return false;
    --*budget;
    for (unsigned int s = 0; s < ins->src_count; ++s)
    {
        const struct vsir_operand *r = &ins->src[s].reg;
        if (!vsir_src_is_masked(ins->opcode, s)) return false;
        for (unsigned int i = 0; i < r->idx_count; ++i) if (r->idx[i].rel_addr) return false;
        if (r->type == VSIR_REGISTER_SSA)
        {
            if (!legacy_remat_cost(defs, r->idx[0].offset, count, budget)) return false;
        }
        else if (r->type != VSIR_REGISTER_CONST && r->type != VSIR_REGISTER_INPUT
                && r->type != VSIR_REGISTER_IMMCONST) return false;
    }
    return ins->src_count != 0;
}

static bool legacy_remat_clone(struct vsir_program *program, struct vkd3d_shader_instruction_array *array,
        const struct vkd3d_shader_instruction *const *defs, unsigned int old, unsigned int *out)
{
    const struct vkd3d_shader_instruction *source = defs[old];
    struct vkd3d_shader_instruction copy, *target;
    struct vsir_dst_operand *dst;
    struct vsir_src_operand *src;
    if (!vsir_instruction_init_with_params(program, &copy, &source->location, source->opcode, 1, source->src_count))
        return false;
    dst = copy.dst; src = copy.src; copy = *source; copy.dst = dst; copy.src = src;
    *dst = *source->dst;
    memcpy(src, source->src, source->src_count * sizeof(*src));
    for (unsigned int s = 0; s < copy.src_count; ++s)
        if (src[s].reg.type == VSIR_REGISTER_SSA)
        {
            unsigned int id;
            if (!legacy_remat_clone(program, array, defs, src[s].reg.idx[0].offset, &id)) return false;
            src[s].reg.idx[0].offset = id;
        }
    *out = program->ssa_count++;
    dst->reg.idx[0].offset = *out;
    if (!(target = shader_instruction_array_append(array))) return false;
    *target = copy;
    return true;
}

static enum vkd3d_result legacy_rematerialize(struct vsir_program *program)
{
    struct vsir_program_iterator it = vsir_program_iterator(&program->instructions);
    const struct vkd3d_shader_instruction **defs;
    struct vkd3d_shader_instruction_array array;
    struct vkd3d_shader_instruction *ins, *target;
    struct liveness_tracker tracker;
    unsigned int count = program->ssa_count, position = 0, *cached = NULL, *last_use = NULL;
    bool *selected, any = false;
    enum vkd3d_result ret;
    if (program->shader_version.major != 2 || program->shader_version.type != VKD3D_SHADER_TYPE_VERTEX || !count)
        return VKD3D_OK;
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
        switch (ins->opcode)
        {
            case VSIR_OP_IF: case VSIR_OP_IFC: case VSIR_OP_LOOP: case VSIR_OP_REP:
            case VSIR_OP_LABEL: case VSIR_OP_BRANCH: case VSIR_OP_SWITCH:
            case VSIR_OP_SWITCH_MONOLITHIC: case VSIR_OP_CALL: case VSIR_OP_CALLNZ:
            case VSIR_OP_RETP: return VKD3D_OK;
            default: break;
        }
    if ((ret = track_liveness(program, &tracker)) < 0) return ret;
    defs = vkd3d_calloc(count, sizeof(*defs));
    selected = vkd3d_calloc(count, sizeof(*selected));
    if (!defs || !selected)
    { vkd3d_free(defs); vkd3d_free(selected); liveness_tracker_cleanup(&tracker); return VKD3D_ERROR_OUT_OF_MEMORY; }
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
        if (ins->dst_count == 1 && ins->dst[0].reg.type == VSIR_REGISTER_SSA) defs[ins->dst[0].reg.idx[0].offset] = ins;
    for (unsigned int i = 0; i < count; ++i)
    {
        const struct liveness_tracker_reg *r = &tracker.ssa_regs[i];
        unsigned int budget = 8;
        if (r->written && r->last_read > r->first_write + 128 && vkd3d_popcount(r->mask) >= 2
                && legacy_remat_cost(defs, i, count, &budget)) any = selected[i] = true;
    }
    liveness_tracker_cleanup(&tracker);
    if (!any) { vkd3d_free(defs); vkd3d_free(selected); return VKD3D_OK; }
    if (!shader_instruction_array_init(&array, program->instructions.count))
    { vkd3d_free(defs); vkd3d_free(selected); return VKD3D_ERROR_OUT_OF_MEMORY; }
    cached = vkd3d_calloc((size_t)count * 16, sizeof(*cached));
    last_use = vkd3d_calloc((size_t)count * 16, sizeof(*last_use));
    if (!cached || !last_use) goto fail;
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it), ++position)
    {
        struct vkd3d_shader_instruction copy = *ins;
        if (copy.src_count && !(copy.src = vsir_program_clone_src_operands(program, ins->src, ins->src_count))) goto fail;
        for (unsigned int s = 0; s < copy.src_count; ++s)
            if (copy.src[s].reg.type == VSIR_REGISTER_SSA && copy.src[s].reg.idx[0].offset < count
                    && selected[copy.src[s].reg.idx[0].offset])
            {
                unsigned int id, mask = 0, output_mask = 0;
                for (unsigned int d = 0; d < copy.dst_count; ++d) output_mask |= copy.dst[d].write_mask;
                if (!output_mask || !vsir_src_is_masked(copy.opcode, s)) output_mask = 15;
                for (unsigned int c = 0; c < 4; ++c)
                    if (output_mask & (1u << c)) mask |= 1u << vsir_swizzle_get_component(copy.src[s].swizzle, c);
                size_t key = (size_t)copy.src[s].reg.idx[0].offset * 16 + mask;
                if (cached[key] && position - last_use[key] <= 128) id = cached[key] - 1;
                else
                {
                    if (!legacy_remat_clone(program, &array, defs, copy.src[s].reg.idx[0].offset, &id)) goto fail;
                    cached[key] = id + 1;
                }
                last_use[key] = position;
                copy.src[s].reg.idx[0].offset = id;
            }
        if (!(target = shader_instruction_array_append(&array))) goto fail;
        *target = copy;
    }
    vsir_program_replace_instructions(program, &array);
    vkd3d_free(defs); vkd3d_free(selected); vkd3d_free(cached); vkd3d_free(last_use);
    for (unsigned int pass = 0; pass < 8; ++pass)
        if ((ret = legacy_trim_load_components(program)) < 0) return ret;
    return VKD3D_OK;
fail:
    shader_instruction_array_cleanup(&array); vkd3d_free(defs); vkd3d_free(selected); vkd3d_free(cached); vkd3d_free(last_use);
    return VKD3D_ERROR_OUT_OF_MEMORY;
}

static enum vkd3d_result vsir_allocate_temp_registers(struct vsir_program *program,]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[    if ((ret = legacy_split_temp_lifetimes(program)) < 0)]=]
  [=[    if ((ret = legacy_rematerialize(program)) < 0)
        return ret;
    if ((ret = legacy_split_temp_lifetimes(program)) < 0)]=])

endif()
