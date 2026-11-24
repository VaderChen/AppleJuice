# DXIL 的 handle 前向參照：DXC 的基本區塊排列可讓使用處在位元碼中早於定義（定義仍支配使用），例如 SM6.6 的
# createHandleFromHeap／annotateHandle 放在後面的區塊。vkd3d 依序解析時把尚未定義的值當成 SSA，handle 便失去描述元。
# 這裡先建立「本函式值編號 → 紀錄」的對照；某值以 handle 型別被前向參照時，暫時把目前值位置移到該紀錄，
# 重用 sm6_parser_emit_call 解析那筆 createHandle*／annotateHandle（只限這幾種 dx.op；索引仍走既有的前向 SSA）。
# 之後依序處理到該紀錄時會再解析一次，得到相同的描述元。
set(X64ARM_VKD3D_DXIL "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c")

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    struct fixup_data *fixups;
    size_t fixup_count;]=]
  [=[    struct fixup_data *fixups;
    size_t fixup_count;
    /* x64arm: forward handle resolution (cmake/Vkd3dDxilForwardHandles.cmake). */
    const struct dxil_block *x64arm_function_block;
    struct sm6_function *x64arm_current_function;
    size_t *x64arm_value_records;
    size_t x64arm_value_base, x64arm_value_record_count;
    unsigned int x64arm_resolve_depth;]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[static void sm6_parser_pre_init_or_validate_referenced_value(struct sm6_parser *dxil,]=]
  [=[static bool x64arm_resolve_forward_handle(struct sm6_parser *sm6, size_t operand);

static void sm6_parser_pre_init_or_validate_referenced_value(struct sm6_parser *dxil,]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    else if (expect_ssa)
    {
        value->type = fwd_type;]=]
  [=[    else if (expect_ssa && sm6_type_is_handle(fwd_type) && x64arm_resolve_forward_handle(dxil, operand))
    {
        /* The handle was resolved from its defining record. */
    }
    else if (expect_ssa)
    {
        value->type = fwd_type;]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    function->block_count = block_count;

    sm6->cur_max_value = function->value_count;]=]
  [=[    function->block_count = block_count;

    sm6->cur_max_value = function->value_count;
    x64arm_map_function_values(sm6, block, function);]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[static enum vkd3d_shader_opcode dxil_map_cast_op(uint64_t code,]=]
  [=[/* x64arm: whether a call record defines a value, from the callee's return type. */
static bool x64arm_call_defines_value(struct sm6_parser *sm6, const struct dxil_record *record, size_t value, bool *known)
{
    const struct sm6_value *fn;
    unsigned int idx = 1;
    uint64_t ref;

    *known = false;
    if (record->operand_count < 3)
        return false;
    if (record->operands[idx++] & CALL_CONV_FLAG_EXPLICIT_TYPE)
        ++idx;
    if (idx >= record->operand_count || !(ref = record->operands[idx]) || ref > value)
        return false;
    fn = &sm6->values[value - (size_t)ref];
    if (!sm6_value_is_function_dcl(fn))
        return false;
    *known = true;
    return !sm6_type_is_void(fn->type->u.pointer.type->u.function->ret_type);
}

static void x64arm_map_function_values(struct sm6_parser *sm6, const struct dxil_block *block, struct sm6_function *function)
{
    size_t i, value = sm6->value_count, count = 0;
    bool defines, known;

    vkd3d_free(sm6->x64arm_value_records);
    sm6->x64arm_value_records = NULL;
    sm6->x64arm_value_record_count = 0;
    sm6->x64arm_value_base = value;
    sm6->x64arm_function_block = block;
    sm6->x64arm_current_function = function;
    if (!(sm6->x64arm_value_records = vkd3d_calloc(block->record_count, sizeof(*sm6->x64arm_value_records))))
        return;
    for (i = 1; i < block->record_count; ++i)
    {
        const struct dxil_record *record = block->records[i];

        switch (record->code)
        {
            case FUNC_CODE_INST_BR:
            case FUNC_CODE_INST_RET:
            case FUNC_CODE_INST_STORE:
            case FUNC_CODE_INST_SWITCH:
                defines = false;
                break;
            case FUNC_CODE_INST_CALL:
                defines = x64arm_call_defines_value(sm6, record, value, &known);
                if (!known)
                    goto unknown;
                break;
            case FUNC_CODE_INST_ALLOCA:
            case FUNC_CODE_INST_ATOMICRMW:
            case FUNC_CODE_INST_BINOP:
            case FUNC_CODE_INST_CAST:
            case FUNC_CODE_INST_CMP2:
            case FUNC_CODE_INST_CMPXCHG:
            case FUNC_CODE_INST_EXTRACTVAL:
            case FUNC_CODE_INST_GEP:
            case FUNC_CODE_INST_LOAD:
            case FUNC_CODE_INST_PHI:
            case FUNC_CODE_INST_VSELECT:
                defines = true;
                break;
            default:
                goto unknown;
        }
        if (defines)
        {
            sm6->x64arm_value_records[count++] = i;
            ++value;
        }
    }
    sm6->x64arm_value_record_count = count;
    return;

unknown:
    /* Without a reliable numbering, forward handles keep the default SSA treatment. */
    vkd3d_free(sm6->x64arm_value_records);
    sm6->x64arm_value_records = NULL;
}

static bool x64arm_resolve_forward_handle(struct sm6_parser *sm6, size_t operand)
{
    struct function_emission_state state = {0};
    const struct dxil_record *record;
    const struct sm6_value *fn;
    size_t local, saved_count;
    unsigned int idx = 1;
    uint64_t ref;

    if (!sm6->x64arm_function_block || !sm6->x64arm_value_records || operand < sm6->x64arm_value_base
            || operand == sm6->value_count || sm6->x64arm_resolve_depth >= 64)
        return false;
    local = operand - sm6->x64arm_value_base;
    if (local >= sm6->x64arm_value_record_count)
        return false;
    record = sm6->x64arm_function_block->records[sm6->x64arm_value_records[local]];
    if (record->code != FUNC_CODE_INST_CALL || record->operand_count < 3)
        return false;
    if (record->operands[idx++] & CALL_CONV_FLAG_EXPLICIT_TYPE)
        ++idx;
    if (idx >= record->operand_count || !(ref = record->operands[idx]) || ref > operand)
        return false;
    fn = &sm6->values[operand - (size_t)ref];
    if (!sm6_value_is_function_dcl(fn) || !fn->u.function.is_prototype
            || (strncmp(fn->u.function.name, "dx.op.createHandle", 18) && strcmp(fn->u.function.name, "dx.op.annotateHandle")))
        return false;

    saved_count = sm6->value_count;
    sm6->value_count = operand;
    ++sm6->x64arm_resolve_depth;
    state.function = sm6->x64arm_current_function;
    state.record = record;
    sm6_parser_emit_call(sm6, &state);
    --sm6->x64arm_resolve_depth;
    sm6->value_count = saved_count;

    return sm6->values[operand].value_type == VALUE_TYPE_HANDLE;
}

static enum vkd3d_shader_opcode dxil_map_cast_op(uint64_t code,]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    sm6_parser_metadata_cleanup(sm6);]=]
  [=[    sm6_parser_metadata_cleanup(sm6);
    vkd3d_free(sm6->x64arm_value_records);]=])
