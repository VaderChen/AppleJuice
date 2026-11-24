# 不受支配 SSA 轉暫存器的步驟以「定義所在區塊」判斷支配關係；若某個來源 SSA 在本函式沒有定義（或編號超出範圍），
# 原本會解參考空的區塊指標而讓整個程序崩潰。改為回報編譯錯誤（附運算碼與 SSA 編號），只讓該著色器轉譯失敗。
set(X64ARM_VKD3D_IR "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c")
x64arm_vkd3d_patch("${X64ARM_VKD3D_IR}"
  [=[static void register_map_undominated_use(struct vsir_operand *reg, struct ssas_to_temps_alloc *alloc,
        struct vsir_block *block, struct vsir_block **origin_blocks)
{]=]
  [=[/* x64arm: returns the first SSA index (including relative addresses) without a defining block, or UINT_MAX. */
static unsigned int x64arm_undefined_ssa_use(const struct vsir_operand *reg, unsigned int ssa_count,
        struct vsir_block **origin_blocks)
{
    unsigned int i, undefined;

    if (vsir_operand_is_ssa(reg) && (reg->idx[0].offset >= ssa_count || !origin_blocks[reg->idx[0].offset]))
        return reg->idx[0].offset;
    for (i = 0; i < reg->idx_count; ++i)
    {
        if (reg->idx[i].rel_addr
                && (undefined = x64arm_undefined_ssa_use(&reg->idx[i].rel_addr->reg, ssa_count, origin_blocks)) != UINT_MAX)
            return undefined;
    }
    return UINT_MAX;
}

static void register_map_undominated_use(struct vsir_operand *reg, struct ssas_to_temps_alloc *alloc,
        struct vsir_block *block, struct vsir_block **origin_blocks)
{]=])
x64arm_vkd3d_patch("${X64ARM_VKD3D_IR}"
  [=[        for (ins = vsir_program_iterator_current(&it); ins != end; ins = vsir_program_iterator_next(&it))
        {
            for (j = 0; j < ins->src_count; ++j)
                register_map_undominated_use(&ins->src[j].reg, &alloc, block, origin_blocks);
        }]=]
  [=[        for (ins = vsir_program_iterator_current(&it); ins != end; ins = vsir_program_iterator_next(&it))
        {
            for (j = 0; j < ins->src_count; ++j)
            {
                unsigned int undefined = x64arm_undefined_ssa_use(&ins->src[j].reg, program->ssa_count, origin_blocks);

                if (undefined != UINT_MAX)
                {
                    vkd3d_shader_error(cfg->message_context, &ins->location, VKD3D_SHADER_ERROR_VSIR_INVALID_SSA_USAGE,
                            "Instruction %#x source %u reads SSA value %u (of %u) that has no definition in this function.",
                            ins->opcode, (unsigned int)j, undefined, program->ssa_count);
                    vkd3d_free(origin_blocks);
                    vkd3d_free(alloc.table);
                    return VKD3D_ERROR_INVALID_SHADER;
                }
                register_map_undominated_use(&ins->src[j].reg, &alloc, block, origin_blocks);
            }
        }]=])
