file(READ "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c" legacy_patch_source)
string(FIND "${legacy_patch_source}" "static void legacy_record_components(" legacy_patch_present)
if(legacy_patch_present EQUAL -1)
# 直線 shader 的同一個可寫暫存器可有多段互不相干的生命期。
# 在完整覆寫處重新命名，避免線性掃描把所有區段合成一段長生命期。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[static enum vkd3d_result vsir_allocate_temp_registers(struct vsir_program *program,]=]
  [=[static void legacy_record_components(const struct vsir_src_operand *src,
        unsigned int mask, unsigned int *used, unsigned int ssa_count)
{
    for (unsigned int i = 0; i < src->reg.idx_count; ++i)
        if (src->reg.idx[i].rel_addr)
            legacy_record_components(src->reg.idx[i].rel_addr, 1, used, ssa_count);
    if (src->reg.type != VSIR_REGISTER_SSA && src->reg.type != VSIR_REGISTER_TEMP) return;
    unsigned int id = src->reg.idx[0].offset + (src->reg.type == VSIR_REGISTER_TEMP ? ssa_count : 0);
    for (unsigned int i = 0; i < 4; ++i)
        if (mask & (1u << i)) used[id] |= 1u << vsir_swizzle_get_component(src->swizzle, i);
}

static void legacy_compact_source(struct vsir_src_operand *src, const unsigned int *masks)
{
    for (unsigned int i = 0; i < src->reg.idx_count; ++i)
        if (src->reg.idx[i].rel_addr) legacy_compact_source(src->reg.idx[i].rel_addr, masks);
    if (src->reg.type != VSIR_REGISTER_SSA) return;
    unsigned int mask = masks[src->reg.idx[0].offset];
    if (!mask) return;
    uint32_t swizzle = 0;
    for (unsigned int i = 0; i < 4; ++i)
    {
        unsigned int component = vsir_swizzle_get_component(src->swizzle, i);
        vsir_swizzle_set_component(&swizzle, i, (mask & (1u << component))
                ? vkd3d_popcount(mask & ((1u << component) - 1)) : 0);
    }
    src->swizzle = swizzle;
}

static enum vkd3d_result legacy_trim_load_components(struct vsir_program *program)
{
    struct vsir_program_iterator it = vsir_program_iterator(&program->instructions);
    struct vkd3d_shader_instruction *ins;
    unsigned int *used, *compact;
    if (program->shader_version.major > 2 || program->shader_version.major < 2) return VKD3D_OK;
    if (!(used = vkd3d_calloc(program->ssa_count * 2 + program->temp_count, sizeof(*used))))
        return VKD3D_ERROR_OUT_OF_MEMORY;
    compact = used + program->ssa_count + program->temp_count;
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
    {
        unsigned int mask = 0;
        for (unsigned int j = 0; j < ins->dst_count; ++j)
        {
            mask |= ins->dst[j].write_mask;
            for (unsigned int k = 0; k < ins->dst[j].reg.idx_count; ++k)
                if (ins->dst[j].reg.idx[k].rel_addr)
                    legacy_record_components(ins->dst[j].reg.idx[k].rel_addr, 1, used, program->ssa_count);
        }
        for (unsigned int j = 0; j < ins->src_count; ++j)
            legacy_record_components(&ins->src[j], vsir_src_is_masked(ins->opcode, j) && mask ? mask : 15,
                    used, program->ssa_count);
    }
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
        if (ins->dst_count == 1 && ins->src_count
                && (ins->dst[0].reg.type == VSIR_REGISTER_SSA || (ins->opcode == VSIR_OP_MOV && ins->dst[0].reg.type == VSIR_REGISTER_TEMP)))
        {
            bool componentwise = true;
            for (unsigned int j = 0; j < ins->src_count; ++j)
                componentwise &= vsir_src_is_masked(ins->opcode, j);
            if (!componentwise) continue;
            unsigned int id = ins->dst[0].reg.idx[0].offset
                    + (ins->dst[0].reg.type == VSIR_REGISTER_TEMP ? program->ssa_count : 0);
            unsigned int mask = ins->dst[0].write_mask & used[id];
            /* Scalar relative loads otherwise reserve four lanes until their use. */
            if (mask) ins->dst[0].write_mask = mask;
            if (mask && ins->dst[0].reg.type == VSIR_REGISTER_SSA)
            {
                compact[id] = mask;
                for (unsigned int j = 0; j < ins->src_count; ++j)
                {
                    uint32_t swizzle = 0;
                    unsigned int target = 0;
                    for (unsigned int c = 0; c < 4; ++c)
                        if (mask & (1u << c))
                            vsir_swizzle_set_component(&swizzle, target++,
                                    vsir_swizzle_get_component(ins->src[j].swizzle, c));
                    ins->src[j].swizzle = swizzle;
                }
                ins->dst[0].write_mask = (1u << vkd3d_popcount(mask)) - 1;
            }
        }
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
    {
        for (unsigned int j = 0; j < ins->src_count; ++j) legacy_compact_source(&ins->src[j], compact);
        for (unsigned int j = 0; j < ins->dst_count; ++j)
            for (unsigned int k = 0; k < ins->dst[j].reg.idx_count; ++k)
                if (ins->dst[j].reg.idx[k].rel_addr) legacy_compact_source(ins->dst[j].reg.idx[k].rel_addr, compact);
    }
    vkd3d_free(used);
    return VKD3D_OK;
}

static void legacy_split_temp_source(struct vsir_operand *reg, const unsigned int *map)
{
    for (unsigned int i = 0; i < reg->idx_count; ++i)
        if (reg->idx[i].rel_addr)
            legacy_split_temp_source(&reg->idx[i].rel_addr->reg, map);
    if (reg->type == VSIR_REGISTER_TEMP)
        reg->idx[0].offset = map[reg->idx[0].offset];
}

static enum vkd3d_result legacy_split_temp_lifetimes(struct vsir_program *program)
{
    const unsigned int count = program->temp_count;
    struct vsir_program_iterator it = vsir_program_iterator(&program->instructions);
    struct vkd3d_shader_instruction *ins;
    unsigned int *map, *masks, next = count;
    if (!count || program->shader_version.major > 2 || program->shader_version.major < 2)
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
            if (ins->dst[j].reg.type == VSIR_REGISTER_TEMP && ins->dst[j].reg.idx[0].rel_addr)
                return VKD3D_OK;
        for (unsigned int j = 0; j < ins->src_count; ++j)
            if (ins->src[j].reg.type == VSIR_REGISTER_TEMP && ins->src[j].reg.idx[0].rel_addr)
                return VKD3D_OK;
    }
    if (!(map = vkd3d_calloc(count * 2, sizeof(*map)))) return VKD3D_ERROR_OUT_OF_MEMORY;
    masks = map + count;
    for (unsigned int i = 0; i < count; ++i) map[i] = i;
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
        for (unsigned int j = 0; j < ins->dst_count; ++j)
            if (ins->dst[j].reg.type == VSIR_REGISTER_TEMP)
                masks[ins->dst[j].reg.idx[0].offset] |= ins->dst[j].write_mask;
    for (ins = vsir_program_iterator_head(&it); ins; ins = vsir_program_iterator_next(&it))
    {
        for (unsigned int j = 0; j < ins->src_count; ++j)
            legacy_split_temp_source(&ins->src[j].reg, map);
        for (unsigned int j = 0; j < ins->dst_count; ++j)
        {
            struct vsir_operand *reg = &ins->dst[j].reg;
            for (unsigned int k = 0; k < reg->idx_count; ++k)
                if (reg->idx[k].rel_addr) legacy_split_temp_source(&reg->idx[k].rel_addr->reg, map);
            if (reg->type == VSIR_REGISTER_TEMP)
            {
                unsigned int old = reg->idx[0].offset;
                if (ins->dst[j].write_mask == masks[old]) map[old] = next++;
                reg->idx[0].offset = map[old];
            }
        }
    }
    program->temp_count = next;
    vkd3d_free(map);
    return VKD3D_OK;
}

static enum vkd3d_result vsir_allocate_temp_registers(struct vsir_program *program,]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[    if (!program->ssa_count && !program->temp_count)
        return VKD3D_OK;

    if ((ret = track_liveness(program, &tracker)))]=]
  [=[    if (!program->ssa_count && !program->temp_count)
        return VKD3D_OK;

    if ((ret = legacy_split_temp_lifetimes(program)) < 0)
        return ret;
    if ((ret = legacy_trim_load_components(program)) < 0)
        return ret;
    if ((ret = track_liveness(program, &tracker)))]=])

endif()
