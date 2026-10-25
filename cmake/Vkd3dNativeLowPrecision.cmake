# DXIL 原生 16 位元（-enable-16bit-types，全域旗標 0x800000）。
# vkd3d 的 DXIL 前端本來就把 16 位元值提升為 32 位元運算（與最小精度相同），SPIR-V 後端卻把旗標視為未支援。
# 提升模型下只有記憶體佈局不同：原生 16 位元的常數緩衝一列是 8 個 16 位元值（dx.types.CBufRet.f16.8／i16.8），
# 這裡改為讀出整列 32 位元資料，extractvalue 時以位元抽取取第 i 個值；浮點再經 f16tof32，整數維持符號延伸的提升表示。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/spirv.c"
  [=[    if (flags & VKD3DSGF_ENABLE_MINIMUM_PRECISION)
    {
        WARN("Ignoring the \"enableMinimumPrecision\" global flag.\n");
        flags &= ~VKD3DSGF_ENABLE_MINIMUM_PRECISION;
    }]=]
  [=[    if (flags & VKD3DSGF_ENABLE_MINIMUM_PRECISION)
    {
        WARN("Ignoring the \"enableMinimumPrecision\" global flag.\n");
        flags &= ~VKD3DSGF_ENABLE_MINIMUM_PRECISION;
    }

    /* x64arm: the DXIL front end promotes native 16-bit values to 32 bits,
     * and handles the packed 16-bit memory layouts itself. */
    flags &= ~VKD3DSGF_ENABLE_NATIVE_LOW_PRECISION;]=])

x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[    type = sm6_type_get_scalar_type(dst->type, 0);
    VKD3D_ASSERT(type);
    src_param->reg.data_type = vsir_data_type_from_dxil(type, 0, sm6);
    if (data_type_is_64_bit(src_param->reg.data_type))
        src_param->swizzle = vsir_swizzle_64_from_32(src_param->swizzle);
    else
        vsir_operand_convert_to_minimum_precision(&src_param->reg);

    if (!instruction_dst_param_init_ssa_vector(ins, sm6_type_max_vector_size(type), sm6))
        vkd3d_shader_instruction_make_nop(ins);
}]=]
  [=[    type = sm6_type_get_scalar_type(dst->type, 0);
    VKD3D_ASSERT(type);
    src_param->reg.data_type = vsir_data_type_from_dxil(type, 0, sm6);
    if (sm6_type_is_struct(dst->type) && dst->type->u.struc->elem_count == 8 && type->u.width == 16)
    {
        /* x64arm: native 16-bit row; keep the raw 32-bit words, see sm6_parser_emit_extractval(). */
        src_param->reg.data_type = VSIR_DATA_U32;
        if (!instruction_dst_param_init_ssa_vector(ins, VKD3D_VEC4_SIZE, sm6))
            vkd3d_shader_instruction_make_nop(ins);
        else
            ins->dst[0].reg.data_type = VSIR_DATA_U32;
        return;
    }
    if (data_type_is_64_bit(src_param->reg.data_type))
        src_param->swizzle = vsir_swizzle_64_from_32(src_param->swizzle);
    else
        vsir_operand_convert_to_minimum_precision(&src_param->reg);

    if (!instruction_dst_param_init_ssa_vector(ins, sm6_type_max_vector_size(type), sm6))
        vkd3d_shader_instruction_make_nop(ins);
}]=])

x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[    dst->type = type;

    if (!(ins = sm6_parser_add_function_instruction(dxil, state)))
        return;

    vsir_instruction_init(ins, &dxil->p.location, VSIR_OP_MOV);

    if (!(src_param = instruction_src_params_alloc(ins, 1, dxil)))
    {
        vkd3d_shader_instruction_make_nop(ins);
        return;
    }

    vsir_operand_from_dxil_value(&src_param->reg, src, 0, dxil);
    src_param_init_scalar(src_param, elem_idx);]=]
  [=[    dst->type = type;

    if (sm6_type_is_struct(src->type) && src->type->u.struc->elem_count == 8 && type->u.width == 16)
    {
        /* x64arm: element of a native 16-bit constant buffer row (CBufRet.f16.8/i16.8).
         * Integers stay sign-extended like other promoted 16-bit values. */
        bool is_float = sm6_type_is_floating_point(type);
        struct vsir_dst_operand *dst_param;
        struct vsir_src_operand *bfe_src;

        if (!(ins = sm6_parser_add_function_instruction(dxil, state)))
            return;
        vsir_instruction_init(ins, &dxil->p.location, is_float ? VSIR_OP_UBFE : VSIR_OP_IBFE);
        if (!(bfe_src = instruction_src_params_alloc(ins, 3, dxil)))
        {
            vkd3d_shader_instruction_make_nop(ins);
            return;
        }
        src_param_make_constant_uint(&bfe_src[0], 16);
        src_param_make_constant_uint(&bfe_src[1], (elem_idx & 1) * 16);
        vsir_operand_from_dxil_value(&bfe_src[2].reg, src, 0, dxil);
        bfe_src[2].reg.data_type = is_float ? VSIR_DATA_U32 : VSIR_DATA_I32;
        src_param_init_scalar(&bfe_src[2], elem_idx >> 1);
        if (!is_float)
        {
            if (!instruction_dst_param_init_ssa_scalar(ins, 0, dxil))
                vkd3d_shader_instruction_make_nop(ins);
            return;
        }
        if (!(dst_param = instruction_dst_params_alloc(ins, 1, dxil)))
        {
            vkd3d_shader_instruction_make_nop(ins);
            return;
        }
        vsir_dst_operand_init(dst_param, VSIR_REGISTER_TEMP, VSIR_DATA_U32, 1);
        dst_param->reg.idx[0].offset = state->temp_idx;
        dst_param->reg.dimension = VSIR_DIMENSION_VEC4;
        dst_param->write_mask = VKD3DSP_WRITEMASK_0;

        if (!(ins = sm6_parser_add_function_instruction(dxil, state)))
            return;
        vsir_instruction_init(ins, &dxil->p.location, VSIR_OP_F16TOF32);
        if (!(src_param = instruction_src_params_alloc(ins, 1, dxil)))
        {
            vkd3d_shader_instruction_make_nop(ins);
            return;
        }
        src_param_init(src_param);
        vsir_operand_init_with_id(&src_param->reg, VSIR_REGISTER_TEMP, VSIR_DATA_U32, state->temp_idx++);
        src_param->reg.dimension = VSIR_DIMENSION_VEC4;
        if (!instruction_dst_param_init_ssa_scalar(ins, 0, dxil))
            vkd3d_shader_instruction_make_nop(ins);
        return;
    }

    if (!(ins = sm6_parser_add_function_instruction(dxil, state)))
        return;

    vsir_instruction_init(ins, &dxil->p.location, VSIR_OP_MOV);

    if (!(src_param = instruction_src_params_alloc(ins, 1, dxil)))
    {
        vkd3d_shader_instruction_make_nop(ins);
        return;
    }

    vsir_operand_from_dxil_value(&src_param->reg, src, 0, dxil);
    src_param_init_scalar(src_param, elem_idx);]=])

# 原生 16 位元的 raw／structured buffer 元素以 2 位元組緊密排列；提升模型的 32 位元讀寫會錯位。
# 讀取改為逐元素：位元組位址 = 偏移 + 2k，讀所在 4 位元組對齊的字組，再以 (位址 & 2)×8 抽 16 位元；
# 浮點經 f16tof32，整數以 IBFE 維持符號延伸的提升表示。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[static void sm6_parser_emit_dx_raw_buffer_load(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{]=]
  [=[static struct vkd3d_shader_instruction *x64arm_emit_op(struct sm6_parser *sm6,
        struct function_emission_state *state, enum vkd3d_shader_opcode op, unsigned int src_count,
        struct vsir_src_operand **src)
{
    struct vkd3d_shader_instruction *ins;

    if (!(ins = sm6_parser_add_function_instruction(sm6, state)))
        return NULL;
    vsir_instruction_init(ins, &sm6->p.location, op);
    if (!(*src = instruction_src_params_alloc(ins, src_count, sm6)))
    {
        vkd3d_shader_instruction_make_nop(ins);
        return NULL;
    }
    return ins;
}

static bool x64arm_temp_dst(struct sm6_parser *sm6, struct vkd3d_shader_instruction *ins,
        unsigned int idx, unsigned int component, enum vsir_data_type data_type)
{
    struct vsir_dst_operand *dst;

    if (!(dst = instruction_dst_params_alloc(ins, 1, sm6)))
    {
        vkd3d_shader_instruction_make_nop(ins);
        return false;
    }
    vsir_dst_operand_init(dst, VSIR_REGISTER_TEMP, data_type, 1);
    dst->reg.idx[0].offset = idx;
    dst->reg.dimension = VSIR_DIMENSION_VEC4;
    dst->write_mask = 1u << component;
    return true;
}

static void x64arm_temp_src(struct vsir_src_operand *src, unsigned int idx, unsigned int component,
        enum vsir_data_type data_type)
{
    src_param_init(src);
    vsir_operand_init_with_id(&src->reg, VSIR_REGISTER_TEMP, data_type, idx);
    src->reg.dimension = VSIR_DIMENSION_VEC4;
    src->swizzle = vkd3d_shader_create_swizzle(component, component, component, component);
}

/* x64arm: native 16-bit raw/structured buffer load, one 16-bit element at a time. */
static void x64arm_emit_raw_load16(struct sm6_parser *sm6, const struct sm6_value **operands,
        const struct sm6_value *resource, bool raw, unsigned int count, bool is_float,
        struct function_emission_state *state)
{
    unsigned int work = state->temp_idx++, loaded = state->temp_idx++, result = state->temp_idx++, k;
    struct vkd3d_shader_instruction *ins;
    struct vsir_src_operand *src;

    for (k = 0; k < count; ++k)
    {
        /* work.x = byte address of element k; work.y = aligned word address; work.z = bit offset. */
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_IADD, 2, &src)))
            return;
        src_param_init_from_value(&src[0], operands[raw ? 1 : 2], 0, sm6);
        src[0].reg.data_type = VSIR_DATA_U32;
        src_param_make_constant_uint(&src[1], 2 * k);
        if (!x64arm_temp_dst(sm6, ins, work, 0, VSIR_DATA_U32))
            return;
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_AND, 2, &src)))
            return;
        x64arm_temp_src(&src[0], work, 0, VSIR_DATA_U32);
        src_param_make_constant_uint(&src[1], ~3u);
        if (!x64arm_temp_dst(sm6, ins, work, 1, VSIR_DATA_U32))
            return;
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_AND, 2, &src)))
            return;
        x64arm_temp_src(&src[0], work, 0, VSIR_DATA_U32);
        src_param_make_constant_uint(&src[1], 2);
        if (!x64arm_temp_dst(sm6, ins, work, 2, VSIR_DATA_U32))
            return;
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_ISHL, 2, &src)))
            return;
        x64arm_temp_src(&src[0], work, 2, VSIR_DATA_U32);
        src_param_make_constant_uint(&src[1], 3);
        if (!x64arm_temp_dst(sm6, ins, work, 2, VSIR_DATA_U32))
            return;

        if (!(ins = sm6_parser_add_function_instruction(sm6, state)))
            return;
        instruction_init_with_resource(ins, raw ? VSIR_OP_LD_RAW : VSIR_OP_LD_STRUCTURED, resource, sm6);
        if (!(src = instruction_src_params_alloc(ins, raw ? 2 : 3, sm6)))
        {
            vkd3d_shader_instruction_make_nop(ins);
            return;
        }
        if (!raw)
            src_param_init_from_value(&src[0], operands[1], 0, sm6);
        x64arm_temp_src(&src[raw ? 0 : 1], work, 1, VSIR_DATA_U32);
        src_param_init_vector_from_handle(sm6, &src[raw ? 1 : 2], &resource->u.handle);
        if (!x64arm_temp_dst(sm6, ins, loaded, 0, VSIR_DATA_U32))
            return;

        if (!(ins = x64arm_emit_op(sm6, state, is_float ? VSIR_OP_UBFE : VSIR_OP_IBFE, 3, &src)))
            return;
        src_param_make_constant_uint(&src[0], 16);
        x64arm_temp_src(&src[1], work, 2, VSIR_DATA_U32);
        x64arm_temp_src(&src[2], loaded, 0, is_float ? VSIR_DATA_U32 : VSIR_DATA_I32);
        if (!x64arm_temp_dst(sm6, ins, result, k, is_float ? VSIR_DATA_U32 : VSIR_DATA_I32))
            return;
        if (is_float)
        {
            if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_F16TOF32, 1, &src)))
                return;
            x64arm_temp_src(&src[0], result, k, VSIR_DATA_U32);
            if (!x64arm_temp_dst(sm6, ins, result, k, VSIR_DATA_F32))
                return;
        }
    }

    if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_MOV, 1, &src)))
        return;
    src_param_init(&src[0]);
    vsir_operand_init_with_id(&src[0].reg, VSIR_REGISTER_TEMP, is_float ? VSIR_DATA_F32 : VSIR_DATA_I32, result);
    src[0].reg.dimension = VSIR_DIMENSION_VEC4;
    src_param_init_vector(&src[0], count);
    if (!instruction_dst_param_init_ssa_vector(ins, count, sm6))
        vkd3d_shader_instruction_make_nop(ins);
}

static void sm6_parser_emit_dx_raw_buffer_load(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[    if (!(ins = sm6_parser_add_function_instruction(sm6, state)))
        return;

    instruction_init_with_resource(ins, raw ? VSIR_OP_LD_RAW : VSIR_OP_LD_STRUCTURED, resource, sm6);
    operand_count = 2 + !raw;]=]
  [=[    if (sm6_type_get_scalar_type(sm6_parser_get_current_value(sm6)->type, 0)
            && sm6_type_get_scalar_type(sm6_parser_get_current_value(sm6)->type, 0)->u.width == 16)
    {
        x64arm_emit_raw_load16(sm6, operands, resource, raw, component_count,
                sm6_type_is_floating_point(sm6_type_get_scalar_type(sm6_parser_get_current_value(sm6)->type, 0)), state);
        return;
    }

    if (!(ins = sm6_parser_add_function_instruction(sm6, state)))
        return;

    instruction_init_with_resource(ins, raw ? VSIR_OP_LD_RAW : VSIR_OP_LD_STRUCTURED, resource, sm6);
    operand_count = 2 + !raw;]=])
# 先前的暫時拒絕版本：還原成原始文字，讓上面的降階接手。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[OLD-REJECT-PLACEHOLDER]=]
  [=[    raw = resource->u.handle.d->kind == RESOURCE_KIND_RAWBUFFER;

    if (op == DX_RAW_BUFFER_LOAD)
    {]=]
  [=[    raw = resource->u.handle.d->kind == RESOURCE_KIND_RAWBUFFER;

    if (sm6_type_get_scalar_type(sm6_parser_get_current_value(sm6)->type, 0)
            && sm6_type_get_scalar_type(sm6_parser_get_current_value(sm6)->type, 0)->u.width == 16)
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND,
                "x64arm: 16-bit raw/structured buffer loads are not implemented yet.");
        return;
    }

    if (op == DX_RAW_BUFFER_LOAD)
    {]=])
# 16 位元寫入：同一個 32 位元字組可能由不同執行緒寫入相鄰的兩個 16 位元元素，
# 因此以原子 AND 清掉目標半字組、再原子 OR 寫入新值，不做會互相覆蓋的讀改寫。
# 既有依賴可能帶有 count 版 helper；先精確移除舊版本，保留 write_mask 版及其他修補。
# 沿用共用修補器的空替換契約，重複 configure 不會再次插入同名函式。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[/* x64arm: native 16-bit raw/structured buffer store, one 16-bit element at a time. */
static void x64arm_emit_raw_store16(struct sm6_parser *sm6, const struct sm6_value **operands,
        const struct sm6_value *resource, bool raw, unsigned int count, bool is_float,
        struct function_emission_state *state)
{
    unsigned int work = state->temp_idx++, address = state->temp_idx++, k;
    struct vkd3d_shader_instruction *ins;
    struct vsir_dst_operand *dst;
    struct vsir_src_operand *src;
    enum vkd3d_shader_opcode op;

    for (k = 0; k < count; ++k)
    {
        /* work.x = byte address of element k; address.x/y = word address; work.z = bit offset. */
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_IADD, 2, &src)))
            return;
        src_param_init_from_value(&src[0], operands[raw ? 1 : 2], 0, sm6);
        src[0].reg.data_type = VSIR_DATA_U32;
        src_param_make_constant_uint(&src[1], 2 * k);
        if (!x64arm_temp_dst(sm6, ins, work, 0, VSIR_DATA_U32))
            return;
        if (!raw)
        {
            if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_MOV, 1, &src)))
                return;
            src_param_init_from_value(&src[0], operands[1], 0, sm6);
            src[0].reg.data_type = VSIR_DATA_U32;
            if (!x64arm_temp_dst(sm6, ins, address, 0, VSIR_DATA_U32))
                return;
        }
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_AND, 2, &src)))
            return;
        x64arm_temp_src(&src[0], work, 0, VSIR_DATA_U32);
        src_param_make_constant_uint(&src[1], ~3u);
        if (!x64arm_temp_dst(sm6, ins, address, raw ? 0 : 1, VSIR_DATA_U32))
            return;
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_AND, 2, &src)))
            return;
        x64arm_temp_src(&src[0], work, 0, VSIR_DATA_U32);
        src_param_make_constant_uint(&src[1], 2);
        if (!x64arm_temp_dst(sm6, ins, work, 2, VSIR_DATA_U32))
            return;
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_ISHL, 2, &src)))
            return;
        x64arm_temp_src(&src[0], work, 2, VSIR_DATA_U32);
        src_param_make_constant_uint(&src[1], 3);
        if (!x64arm_temp_dst(sm6, ins, work, 2, VSIR_DATA_U32))
            return;

        /* work.y = 16-bit pattern of the value. */
        if (!(ins = x64arm_emit_op(sm6, state, is_float ? VSIR_OP_F32TOF16 : VSIR_OP_AND, is_float ? 1 : 2, &src)))
            return;
        src_param_init_from_value(&src[0], operands[3 + k], 0, sm6);
        if (!is_float)
        {
            src[0].reg.data_type = VSIR_DATA_U32;
            src_param_make_constant_uint(&src[1], 0xffff);
        }
        if (!x64arm_temp_dst(sm6, ins, work, 1, VSIR_DATA_U32))
            return;
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_ISHL, 2, &src)))
            return;
        x64arm_temp_src(&src[0], work, 1, VSIR_DATA_U32);
        x64arm_temp_src(&src[1], work, 2, VSIR_DATA_U32);
        if (!x64arm_temp_dst(sm6, ins, work, 1, VSIR_DATA_U32))
            return;
        /* work.w = ~(0xffff << shift) */
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_ISHL, 2, &src)))
            return;
        src_param_make_constant_uint(&src[0], 0xffff);
        x64arm_temp_src(&src[1], work, 2, VSIR_DATA_U32);
        if (!x64arm_temp_dst(sm6, ins, work, 3, VSIR_DATA_U32))
            return;
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_NOT, 1, &src)))
            return;
        x64arm_temp_src(&src[0], work, 3, VSIR_DATA_U32);
        if (!x64arm_temp_dst(sm6, ins, work, 3, VSIR_DATA_U32))
            return;

        for (op = VSIR_OP_ATOMIC_AND; ; op = VSIR_OP_ATOMIC_OR)
        {
            if (!(ins = x64arm_emit_op(sm6, state, op, 2, &src)))
                return;
            src_param_init(&src[0]);
            vsir_operand_init_with_id(&src[0].reg, VSIR_REGISTER_TEMP, VSIR_DATA_U32, address);
            src[0].reg.dimension = VSIR_DIMENSION_VEC4;
            src[0].swizzle = raw ? VKD3D_SHADER_SWIZZLE(X, X, X, X) : VKD3D_SHADER_SWIZZLE(X, Y, Y, Y);
            x64arm_temp_src(&src[1], work, op == VSIR_OP_ATOMIC_AND ? 3 : 1, VSIR_DATA_U32);
            if (!(dst = instruction_dst_params_alloc(ins, 1, sm6)))
            {
                vkd3d_shader_instruction_make_nop(ins);
                return;
            }
            dst_param_init(dst);
            sm6_register_from_handle(sm6, &resource->u.handle, &dst->reg);
            if (op == VSIR_OP_ATOMIC_OR)
                break;
        }
    }
}

]=] [=[]=])

x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[static void sm6_parser_emit_dx_raw_buffer_store(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{]=]
  [=[/* x64arm: native 16-bit raw/structured buffer store, one 16-bit element at a time. */
static void x64arm_emit_raw_store16(struct sm6_parser *sm6, const struct sm6_value **operands,
        const struct sm6_value *resource, bool raw, unsigned int write_mask, bool is_float,
        struct function_emission_state *state)
{
    unsigned int work = state->temp_idx++, address = state->temp_idx++, k;
    struct vkd3d_shader_instruction *ins;
    struct vsir_dst_operand *dst;
    struct vsir_src_operand *src;
    enum vkd3d_shader_opcode op;

    for (k = 0; k < 4; ++k)
    {
        if (!(write_mask & (1u << k)))
            continue;
        /* work.x = byte address of element k; address.x/y = word address; work.z = bit offset. */
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_IADD, 2, &src)))
            return;
        src_param_init_from_value(&src[0], operands[raw ? 1 : 2], 0, sm6);
        src[0].reg.data_type = VSIR_DATA_U32;
        src_param_make_constant_uint(&src[1], 2 * k);
        if (!x64arm_temp_dst(sm6, ins, work, 0, VSIR_DATA_U32))
            return;
        if (!raw)
        {
            if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_MOV, 1, &src)))
                return;
            src_param_init_from_value(&src[0], operands[1], 0, sm6);
            src[0].reg.data_type = VSIR_DATA_U32;
            if (!x64arm_temp_dst(sm6, ins, address, 0, VSIR_DATA_U32))
                return;
        }
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_AND, 2, &src)))
            return;
        x64arm_temp_src(&src[0], work, 0, VSIR_DATA_U32);
        src_param_make_constant_uint(&src[1], ~3u);
        if (!x64arm_temp_dst(sm6, ins, address, raw ? 0 : 1, VSIR_DATA_U32))
            return;
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_AND, 2, &src)))
            return;
        x64arm_temp_src(&src[0], work, 0, VSIR_DATA_U32);
        src_param_make_constant_uint(&src[1], 2);
        if (!x64arm_temp_dst(sm6, ins, work, 2, VSIR_DATA_U32))
            return;
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_ISHL, 2, &src)))
            return;
        x64arm_temp_src(&src[0], work, 2, VSIR_DATA_U32);
        src_param_make_constant_uint(&src[1], 3);
        if (!x64arm_temp_dst(sm6, ins, work, 2, VSIR_DATA_U32))
            return;

        /* work.y = 16-bit pattern of the value. */
        if (!(ins = x64arm_emit_op(sm6, state, is_float ? VSIR_OP_F32TOF16 : VSIR_OP_AND, is_float ? 1 : 2, &src)))
            return;
        src_param_init_from_value(&src[0], operands[3 + k], 0, sm6);
        if (!is_float)
        {
            src[0].reg.data_type = VSIR_DATA_U32;
            src_param_make_constant_uint(&src[1], 0xffff);
        }
        if (!x64arm_temp_dst(sm6, ins, work, 1, VSIR_DATA_U32))
            return;
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_ISHL, 2, &src)))
            return;
        x64arm_temp_src(&src[0], work, 1, VSIR_DATA_U32);
        x64arm_temp_src(&src[1], work, 2, VSIR_DATA_U32);
        if (!x64arm_temp_dst(sm6, ins, work, 1, VSIR_DATA_U32))
            return;
        /* work.w = ~(0xffff << shift) */
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_ISHL, 2, &src)))
            return;
        src_param_make_constant_uint(&src[0], 0xffff);
        x64arm_temp_src(&src[1], work, 2, VSIR_DATA_U32);
        if (!x64arm_temp_dst(sm6, ins, work, 3, VSIR_DATA_U32))
            return;
        if (!(ins = x64arm_emit_op(sm6, state, VSIR_OP_NOT, 1, &src)))
            return;
        x64arm_temp_src(&src[0], work, 3, VSIR_DATA_U32);
        if (!x64arm_temp_dst(sm6, ins, work, 3, VSIR_DATA_U32))
            return;

        for (op = VSIR_OP_ATOMIC_AND; ; op = VSIR_OP_ATOMIC_OR)
        {
            if (!(ins = x64arm_emit_op(sm6, state, op, 2, &src)))
                return;
            src_param_init(&src[0]);
            vsir_operand_init_with_id(&src[0].reg, VSIR_REGISTER_TEMP, VSIR_DATA_U32, address);
            src[0].reg.dimension = VSIR_DIMENSION_VEC4;
            src[0].swizzle = raw ? VKD3D_SHADER_SWIZZLE(X, X, X, X) : VKD3D_SHADER_SWIZZLE(X, Y, Y, Y);
            x64arm_temp_src(&src[1], work, op == VSIR_OP_ATOMIC_AND ? 3 : 1, VSIR_DATA_U32);
            if (!(dst = instruction_dst_params_alloc(ins, 1, sm6)))
            {
                vkd3d_shader_instruction_make_nop(ins);
                return;
            }
            dst_param_init(dst);
            sm6_register_from_handle(sm6, &resource->u.handle, &dst->reg);
            if (op == VSIR_OP_ATOMIC_OR)
                break;
        }
    }
}

static void sm6_parser_emit_dx_raw_buffer_store(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[    write_mask = sm6_value_get_constant_uint(operands[7], sm6);
    if (!write_mask || write_mask > VKD3DSP_WRITEMASK_ALL)
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND,
                "Write mask %#x for a raw/structured buffer store operation is invalid.", write_mask);
        return;
    }]=]
  [=[    write_mask = sm6_value_get_constant_uint(operands[7], sm6);
    if (!write_mask || write_mask > VKD3DSP_WRITEMASK_ALL)
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND,
                "Write mask %#x for a raw/structured buffer store operation is invalid.", write_mask);
        return;
    }
    if (sm6_type_is_scalar(operands[3]->type) && operands[3]->type->u.width == 16)
    {
        x64arm_emit_raw_store16(sm6, operands, resource, raw, write_mask,
                sm6_type_is_floating_point(operands[3]->type), state);
        return;
    }]=]
  [=[    write_mask = sm6_value_get_constant_uint(operands[7], sm6);
    if (!write_mask || write_mask > VKD3DSP_WRITEMASK_ALL)
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND,
                "Write mask %#x for a raw/structured buffer store operation is invalid.", write_mask);
        return;
    }
    if (sm6_type_is_scalar(operands[3]->type) && operands[3]->type->u.width == 16)
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND,
                "x64arm: 16-bit raw/structured buffer stores are not implemented yet.");
        return;
    }]=])

# 純量 ALLOCA（例如 -Od 或以位址傳遞的區域變數：alloca i32, i32 1）與全域純量相同，
# 以 1 個元素的索引暫存器承接；直接 load／store 該指標時索引為 0。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[    if (!sm6_type_is_array(type[0]) || !sm6_type_is_numeric(elem_type = type[0]->u.array.elem_type))
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND,
                "Result type of an ALLOCA instruction is not a numeric array.");
        return;
    }]=]
  [=[    if (sm6_type_is_numeric(type[0]))
    {
        elem_type = type[0];
    }
    else if (!sm6_type_is_array(type[0]) || !sm6_type_is_numeric(elem_type = type[0]->u.array.elem_type))
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND,
                "Result type of an ALLOCA instruction is not a numeric array.");
        return;
    }]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[    sm6_parser_declare_indexable_temp(sm6, elem_type, type[0]->u.array.count, alignment, true, 0, ins, dst);]=]
  [=[    sm6_parser_declare_indexable_temp(sm6, elem_type, sm6_type_is_array(type[0]) ? type[0]->u.array.count : 1,
            alignment, true, 0, ins, dst);]=])

# 提升模型下 16 位元值一律以 32 位元運算，索引暫存器（區域／靜態陣列）與 I/O 簽章也須同樣提升；
# 否則原生 16 位元旗標會留下 F16／FLOAT16 型別，SPIR-V 後端無法宣告（Unhandled data type "f16"）。
# 簽章改走最小精度表示（32 位元＋precision 標記），輸入版面的 16 位元格式由頂點擷取轉換。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[    enum vsir_data_type data_type = vsir_data_type_from_dxil(elem_type, 0, sm6);

    if (!(sm6->program->global_flags & VKD3DSGF_ENABLE_NATIVE_LOW_PRECISION))
    {]=]
  [=[    enum vsir_data_type data_type = vsir_data_type_from_dxil(elem_type, 0, sm6);

    /* x64arm: native 16-bit values are promoted to 32 bits by the front end. */
    {]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c"
  [=[    native_16bit = program->global_flags & VKD3DSGF_ENABLE_NATIVE_LOW_PRECISION;
    for (i = 0; i < operand_count; ++i)]=]
  [=[    /* x64arm: signatures follow the promoted 32-bit model, as for minimum precision. */
    native_16bit = false;
    for (i = 0; i < operand_count; ++i)]=])
