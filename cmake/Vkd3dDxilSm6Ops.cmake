# SM6.4～6.6 的核心 DXIL 運算，以既有 VSIR 運算展開：
#   Dot2AddHalf（162）、Dot4AddI8Packed／Dot4AddU8Packed（163／164）、WaveMatch（165）、WaveMultiPrefixOp（166）、
#   WaveMultiPrefixBitCount（167）、Unpack4x8（219）、Pack4x8（220）、IsHelperLane（221）；
#   計算著色器的導數（DerivCoarse／Fine）與 2D 群組的 quad 運算依 D3D 的 2×2 quad 佈局以 SIMD shuffle 取值。
# Apple GPU 的 SIMD group（D3D wave）寬度固定 32：WaveMatch／MultiPrefix 對 32 個 lane 展開成直線程式，
# 結果的 uint4 遮罩只有 x 分量有 lane。契約參照 DirectXShaderCompiler/docs/DXIL.rst 與 SM6.4／6.5／6.6 規格。
set(X64ARM_VKD3D_DXIL "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c")

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    DX_RAW_BUFFER_LOAD              = 139,]=]
  [=[    DX_DOT2_ADD_HALF                = 162,
    DX_DOT4_ADD_I8_PACKED           = 163,
    DX_DOT4_ADD_U8_PACKED           = 164,
    DX_WAVE_MATCH                   = 165,
    DX_WAVE_MULTI_PREFIX_OP         = 166,
    DX_WAVE_MULTI_PREFIX_BIT_COUNT  = 167,
    DX_UNPACK4X8                    = 219,
    DX_PACK4X8                      = 220,
    DX_IS_HELPER_LANE               = 221,
    DX_RAW_BUFFER_LOAD              = 139,]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[struct sm6_dx_opcode_info
{]=]
  [=[/* x64arm: building blocks for intrinsics expanded into several VSIR instructions. */
#define X64ARM_WAVE_LANES 32u

static struct vsir_src_operand x64arm_src_ssa(unsigned int id, enum vsir_data_type type)
{
    struct vsir_src_operand src;

    memset(&src, 0, sizeof(src));
    vsir_operand_init_with_id(&src.reg, VSIR_REGISTER_SSA, type, id);
    src_param_init(&src);
    return src;
}

static struct vsir_src_operand x64arm_src_ssa_component(unsigned int id, enum vsir_data_type type, unsigned int component)
{
    struct vsir_src_operand src = x64arm_src_ssa(id, type);

    src.reg.dimension = VSIR_DIMENSION_VEC4;
    src.swizzle = vkd3d_shader_create_swizzle(component, component, component, component);
    return src;
}

static struct vsir_src_operand x64arm_src_u32(uint32_t value, enum vsir_data_type type)
{
    struct vsir_src_operand src;

    memset(&src, 0, sizeof(src));
    src_param_make_constant_uint(&src, value);
    src.reg.data_type = type;
    return src;
}

static struct vsir_src_operand x64arm_src_value(struct sm6_parser *sm6, const struct sm6_value *value, uint32_t type_flags)
{
    struct vsir_src_operand src;

    memset(&src, 0, sizeof(src));
    src_param_init_from_value(&src, value, type_flags, sm6);
    return src;
}

/* Emits one instruction into a fresh scalar or vec4 SSA value; returns its id or UINT_MAX. */
static unsigned int x64arm_emit(struct sm6_parser *sm6, struct function_emission_state *state, enum vkd3d_shader_opcode opcode,
        enum vsir_data_type type, unsigned int components, const struct vsir_src_operand *srcs, unsigned int src_count)
{
    struct vkd3d_shader_instruction *ins;
    struct vsir_src_operand *params;
    struct vsir_dst_operand *dst;
    unsigned int i, id;

    if (!(ins = sm6_parser_add_function_instruction(sm6, state)))
        return UINT_MAX;
    vsir_instruction_init(ins, &sm6->p.location, opcode);
    if (!(dst = instruction_dst_params_alloc(ins, 1, sm6)) || !(params = instruction_src_params_alloc(ins, src_count, sm6)))
    {
        vkd3d_shader_instruction_make_nop(ins);
        return UINT_MAX;
    }
    for (i = 0; i < src_count; ++i)
        params[i] = srcs[i];
    id = sm6_parser_alloc_ssa_id(sm6);
    dst_param_init_vector(dst, components);
    vsir_operand_init_with_id(&dst->reg, VSIR_REGISTER_SSA, type, id);
    dst->reg.dimension = components > 1 ? VSIR_DIMENSION_VEC4 : VSIR_DIMENSION_SCALAR;
    return id;
}

/* Writes the final result into the intrinsic's own value. */
static void x64arm_emit_result(struct sm6_parser *sm6, struct function_emission_state *state, enum vkd3d_shader_opcode opcode,
        const struct vsir_src_operand *srcs, unsigned int src_count, unsigned int components)
{
    struct vkd3d_shader_instruction *ins;
    struct vsir_src_operand *params;
    unsigned int i;

    if (!(ins = sm6_parser_add_function_instruction(sm6, state)))
        return;
    vsir_instruction_init(ins, &sm6->p.location, opcode);
    if (!(params = instruction_src_params_alloc(ins, src_count, sm6)))
    {
        vkd3d_shader_instruction_make_nop(ins);
        return;
    }
    for (i = 0; i < src_count; ++i)
        params[i] = srcs[i];
    if (!(components > 1 ? instruction_dst_param_init_ssa_vector(ins, components, sm6)
            : instruction_dst_param_init_ssa_scalar(ins, 0, sm6)))
        vkd3d_shader_instruction_make_nop(ins);
}

static unsigned int x64arm_lane_index(struct sm6_parser *sm6, struct function_emission_state *state)
{
    struct vsir_src_operand src;

    sm6_parser_dcl_register_builtin(sm6, VSIR_OP_DCL_INPUT, VSIR_REGISTER_WAVELANEINDEX, VSIR_DATA_U32, 1);
    memset(&src, 0, sizeof(src));
    vsir_operand_init(&src.reg, VSIR_REGISTER_WAVELANEINDEX, VSIR_DATA_U32, 0);
    src_param_init(&src);
    return x64arm_emit(sm6, state, VSIR_OP_MOV, VSIR_DATA_U32, 1, &src, 1);
}

/* Widens 16-bit sources to 32 bits; other types are returned unchanged. */
static struct vsir_src_operand x64arm_widen(struct sm6_parser *sm6, struct function_emission_state *state, struct vsir_src_operand src)
{
    unsigned int id;

    switch (src.reg.data_type)
    {
        case VSIR_DATA_F16:
            id = x64arm_emit(sm6, state, VSIR_OP_FTOD, VSIR_DATA_F32, 1, &src, 1);
            return x64arm_src_ssa(id, VSIR_DATA_F32);
        case VSIR_DATA_I16:
            id = x64arm_emit(sm6, state, VSIR_OP_ITOI, VSIR_DATA_I32, 1, &src, 1);
            return x64arm_src_ssa(id, VSIR_DATA_I32);
        case VSIR_DATA_U16:
            id = x64arm_emit(sm6, state, VSIR_OP_UTOU, VSIR_DATA_U32, 1, &src, 1);
            return x64arm_src_ssa(id, VSIR_DATA_U32);
        default:
            return src;
    }
}

/* lanes_below = (1 << lane) - 1, restricted to active lanes and an optional partition mask. */
static unsigned int x64arm_prefix_mask(struct sm6_parser *sm6, struct function_emission_state *state,
        unsigned int lane, const struct vsir_src_operand *mask)
{
    struct vsir_src_operand srcs[2];
    unsigned int shifted, below, truth, active, combined;

    srcs[0] = x64arm_src_u32(1, VSIR_DATA_U32);
    srcs[1] = x64arm_src_ssa(lane, VSIR_DATA_U32);
    shifted = x64arm_emit(sm6, state, VSIR_OP_ISHL, VSIR_DATA_U32, 1, srcs, 2);
    srcs[0] = x64arm_src_ssa(shifted, VSIR_DATA_U32);
    srcs[1] = x64arm_src_u32(0xffffffffu, VSIR_DATA_U32);
    below = x64arm_emit(sm6, state, VSIR_OP_IADD, VSIR_DATA_U32, 1, srcs, 2);
    srcs[0] = x64arm_src_ssa(lane, VSIR_DATA_U32);
    srcs[1] = x64arm_src_ssa(lane, VSIR_DATA_U32);
    truth = x64arm_emit(sm6, state, VSIR_OP_IEQ, VSIR_DATA_BOOL, 1, srcs, 2);
    srcs[0] = x64arm_src_ssa(truth, VSIR_DATA_BOOL);
    active = x64arm_emit(sm6, state, VSIR_OP_WAVE_ACTIVE_BALLOT, VSIR_DATA_U32, 4, srcs, 1);
    srcs[0] = x64arm_src_ssa(below, VSIR_DATA_U32);
    srcs[1] = x64arm_src_ssa_component(active, VSIR_DATA_U32, 0);
    combined = x64arm_emit(sm6, state, VSIR_OP_AND, VSIR_DATA_U32, 1, srcs, 2);
    if (!mask)
        return combined;
    srcs[0] = x64arm_src_ssa(combined, VSIR_DATA_U32);
    srcs[1] = *mask;
    srcs[1].reg.data_type = VSIR_DATA_U32;
    return x64arm_emit(sm6, state, VSIR_OP_AND, VSIR_DATA_U32, 1, srcs, 2);
}

static void sm6_parser_emit_dx_dot2_add_half(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    struct vsir_src_operand srcs[3], acc = x64arm_src_value(sm6, operands[0], 0);
    unsigned int i, id = UINT_MAX;

    for (i = 0; i < 2; ++i)
    {
        srcs[0] = x64arm_widen(sm6, state, x64arm_src_value(sm6, operands[1 + i], 0));
        srcs[1] = x64arm_widen(sm6, state, x64arm_src_value(sm6, operands[3 + i], 0));
        srcs[2] = id == UINT_MAX ? acc : x64arm_src_ssa(id, VSIR_DATA_F32);
        id = x64arm_emit(sm6, state, VSIR_OP_MAD, VSIR_DATA_F32, 1, srcs, 3);
    }
    srcs[0] = x64arm_src_ssa(id, VSIR_DATA_F32);
    x64arm_emit_result(sm6, state, VSIR_OP_MOV, srcs, 1, 1);
}

static void sm6_parser_emit_dx_dot4_add_packed(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    bool is_signed = op == DX_DOT4_ADD_I8_PACKED;
    enum vkd3d_shader_opcode extract = is_signed ? VSIR_OP_IBFE : VSIR_OP_UBFE;
    enum vsir_data_type type = is_signed ? VSIR_DATA_I32 : VSIR_DATA_U32;
    struct vsir_src_operand srcs[3], acc = x64arm_src_value(sm6, operands[0], 0);
    unsigned int k, a, b, id = UINT_MAX;

    acc.reg.data_type = type;
    for (k = 0; k < 4; ++k)
    {
        srcs[0] = x64arm_src_u32(8, VSIR_DATA_U32);
        srcs[1] = x64arm_src_u32(8 * k, VSIR_DATA_U32);
        srcs[2] = x64arm_src_value(sm6, operands[1], 0);
        srcs[2].reg.data_type = type;
        a = x64arm_emit(sm6, state, extract, type, 1, srcs, 3);
        srcs[2] = x64arm_src_value(sm6, operands[2], 0);
        srcs[2].reg.data_type = type;
        b = x64arm_emit(sm6, state, extract, type, 1, srcs, 3);
        srcs[0] = x64arm_src_ssa(a, type);
        srcs[1] = x64arm_src_ssa(b, type);
        srcs[2] = id == UINT_MAX ? acc : x64arm_src_ssa(id, type);
        id = x64arm_emit(sm6, state, VSIR_OP_IMAD, type, 1, srcs, 3);
    }
    srcs[0] = x64arm_src_ssa(id, type);
    x64arm_emit_result(sm6, state, VSIR_OP_MOV, srcs, 1, 1);
}

/* Compares are bitwise: floats are compared through their 32/64-bit patterns. */
static void sm6_parser_emit_dx_wave_match(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    struct vsir_src_operand srcs[3], value = x64arm_src_value(sm6, operands[0], 0), bits;
    unsigned int i, lane, read, equal, ballot, is_lane, current = UINT_MAX;
    enum vsir_data_type type, compare;

    if (value.reg.data_type == VSIR_DATA_BOOL)
    {
        srcs[0] = value;
        srcs[1] = x64arm_src_u32(1, VSIR_DATA_U32);
        srcs[2] = x64arm_src_u32(0, VSIR_DATA_U32);
        value = x64arm_src_ssa(x64arm_emit(sm6, state, VSIR_OP_MOVC, VSIR_DATA_U32, 1, srcs, 3), VSIR_DATA_U32);
    }
    value = x64arm_widen(sm6, state, value);
    type = value.reg.data_type;
    compare = data_type_is_64_bit(type) ? VSIR_DATA_U64 : VSIR_DATA_U32;
    lane = x64arm_lane_index(sm6, state);
    for (i = 0; i < X64ARM_WAVE_LANES; ++i)
    {
        srcs[0] = value;
        srcs[1] = x64arm_src_u32(i, VSIR_DATA_U32);
        read = x64arm_emit(sm6, state, VSIR_OP_WAVE_READ_LANE_AT, type, 1, srcs, 2);
        srcs[0] = value;
        srcs[0].reg.data_type = compare;
        srcs[1] = x64arm_src_ssa(read, compare);
        if (data_type_is_64_bit(type))
            srcs[0].swizzle = srcs[1].swizzle = vsir_swizzle_64_from_32(VKD3D_SHADER_SWIZZLE(X, X, X, X));
        equal = x64arm_emit(sm6, state, VSIR_OP_IEQ, VSIR_DATA_BOOL, 1, srcs, 2);
        srcs[0] = x64arm_src_ssa(equal, VSIR_DATA_BOOL);
        ballot = x64arm_emit(sm6, state, VSIR_OP_WAVE_ACTIVE_BALLOT, VSIR_DATA_U32, 4, srcs, 1);
        srcs[0] = x64arm_src_ssa(lane, VSIR_DATA_U32);
        srcs[1] = x64arm_src_u32(i, VSIR_DATA_U32);
        is_lane = x64arm_emit(sm6, state, VSIR_OP_IEQ, VSIR_DATA_BOOL, 1, srcs, 2);
        srcs[0] = x64arm_src_ssa(is_lane, VSIR_DATA_BOOL);
        srcs[1] = x64arm_src_ssa_component(ballot, VSIR_DATA_U32, 0);
        srcs[2] = current == UINT_MAX ? x64arm_src_u32(0, VSIR_DATA_U32) : x64arm_src_ssa(current, VSIR_DATA_U32);
        current = x64arm_emit(sm6, state, VSIR_OP_MOVC, VSIR_DATA_U32, 1, srcs, 3);
    }
    /* uint4 result: lanes beyond 32 do not exist on this hardware. */
    bits = x64arm_src_ssa(current, VSIR_DATA_U32);
    srcs[0] = bits;
    srcs[0].swizzle = VKD3D_SHADER_SWIZZLE(X, X, X, X);
    srcs[1] = x64arm_src_u32(0, VSIR_DATA_U32);
    srcs[1].reg.dimension = VSIR_DIMENSION_VEC4;
    srcs[1].reg.u.immconst_u32[0] = 0xffffffffu;
    srcs[1].swizzle = VKD3D_SHADER_NO_SWIZZLE;
    x64arm_emit_result(sm6, state, VSIR_OP_AND, srcs, 2, 4);
}

static void sm6_parser_emit_dx_wave_multi_prefix_bit_count(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    struct vsir_src_operand srcs[2], mask = x64arm_src_value(sm6, operands[1], 0);
    unsigned int lane, eligible, ballot, bits;

    lane = x64arm_lane_index(sm6, state);
    eligible = x64arm_prefix_mask(sm6, state, lane, &mask);
    srcs[0] = x64arm_src_value(sm6, operands[0], 0);
    ballot = x64arm_emit(sm6, state, VSIR_OP_WAVE_ACTIVE_BALLOT, VSIR_DATA_U32, 4, srcs, 1);
    srcs[0] = x64arm_src_ssa(eligible, VSIR_DATA_U32);
    srcs[1] = x64arm_src_ssa_component(ballot, VSIR_DATA_U32, 0);
    bits = x64arm_emit(sm6, state, VSIR_OP_AND, VSIR_DATA_U32, 1, srcs, 2);
    srcs[0] = x64arm_src_ssa(bits, VSIR_DATA_U32);
    x64arm_emit_result(sm6, state, VSIR_OP_COUNTBITS, srcs, 1, 1);
}

static void sm6_parser_emit_dx_wave_multi_prefix_op(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    unsigned int kind = sm6_value_get_constant_uint(operands[5], sm6), lane, eligible, j, read, shifted, bit, include, combined;
    struct vsir_src_operand srcs[3], value, mask = x64arm_src_value(sm6, operands[1], 0), acc;
    enum vkd3d_shader_opcode combine;
    enum vsir_data_type type;
    bool is_float;
    uint32_t identity;

    value = x64arm_src_value(sm6, operands[0], 0);
    type = value.reg.data_type;
    if (data_type_is_64_bit(type) || type == VSIR_DATA_F16 || type == VSIR_DATA_I16 || type == VSIR_DATA_U16)
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND,
                "WaveMultiPrefixOp on non-32-bit values is not implemented.");
        return;
    }
    is_float = type == VSIR_DATA_F32;
    /* DXIL WaveMultiPrefixOpKind: 0 Sum, 1 BitAnd, 2 BitOr, 3 BitXor, 4 Product.
     * SignedOpKind (operand 6) does not change two's complement sums or low products. */
    switch (kind)
    {
        case 0: combine = is_float ? VSIR_OP_ADD : VSIR_OP_IADD; identity = 0; break;
        case 1: combine = VSIR_OP_AND; identity = 0xffffffffu; break;
        case 2: combine = VSIR_OP_OR; identity = 0; break;
        case 3: combine = VSIR_OP_XOR; identity = 0; break;
        case 4: combine = is_float ? VSIR_OP_MUL : VSIR_OP_IMUL_LOW; identity = is_float ? 0x3f800000u : 1; break;
        default:
            vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND,
                    "Invalid WaveMultiPrefixOp kind %u.", kind);
            return;
    }
    if (is_float && kind >= 1 && kind <= 3)
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND,
                "Bitwise WaveMultiPrefixOp on floating point values is invalid.");
        return;
    }

    lane = x64arm_lane_index(sm6, state);
    eligible = x64arm_prefix_mask(sm6, state, lane, &mask);
    acc = x64arm_src_u32(identity, type);
    for (j = 0; j < X64ARM_WAVE_LANES; ++j)
    {
        srcs[0] = value;
        srcs[1] = x64arm_src_u32(j, VSIR_DATA_U32);
        read = x64arm_emit(sm6, state, VSIR_OP_WAVE_READ_LANE_AT, type, 1, srcs, 2);
        srcs[0] = x64arm_src_ssa(eligible, VSIR_DATA_U32);
        srcs[1] = x64arm_src_u32(j, VSIR_DATA_U32);
        shifted = x64arm_emit(sm6, state, VSIR_OP_USHR, VSIR_DATA_U32, 1, srcs, 2);
        srcs[0] = x64arm_src_ssa(shifted, VSIR_DATA_U32);
        srcs[1] = x64arm_src_u32(1, VSIR_DATA_U32);
        bit = x64arm_emit(sm6, state, VSIR_OP_AND, VSIR_DATA_U32, 1, srcs, 2);
        srcs[0] = x64arm_src_ssa(bit, VSIR_DATA_U32);
        srcs[1] = x64arm_src_u32(0, VSIR_DATA_U32);
        include = x64arm_emit(sm6, state, VSIR_OP_INE, VSIR_DATA_BOOL, 1, srcs, 2);
        srcs[0] = acc;
        srcs[1] = x64arm_src_ssa(read, type);
        combined = x64arm_emit(sm6, state, combine, type, 1, srcs, 2);
        srcs[0] = x64arm_src_ssa(include, VSIR_DATA_BOOL);
        srcs[1] = x64arm_src_ssa(combined, type);
        srcs[2] = acc;
        acc = x64arm_src_ssa(x64arm_emit(sm6, state, VSIR_OP_MOVC, type, 1, srcs, 3), type);
    }
    x64arm_emit_result(sm6, state, VSIR_OP_MOV, &acc, 1, 1);
}

/* DXIL UnpackMode: 0 Unsigned (zero extend), 1 Signed (sign extend). */
static void sm6_parser_emit_dx_unpack4x8(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    unsigned int mode = sm6_value_get_constant_uint(operands[0], sm6);
    enum vsir_data_type type = mode ? VSIR_DATA_I32 : VSIR_DATA_U32;
    struct vsir_src_operand srcs[3];
    unsigned int i;

    if (mode > 1)
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND, "Invalid Unpack4x8 mode %u.", mode);
        return;
    }
    for (i = 0; i < 2; ++i)
    {
        srcs[i] = x64arm_src_u32(0, VSIR_DATA_U32);
        srcs[i].reg.dimension = VSIR_DIMENSION_VEC4;
        srcs[i].swizzle = VKD3D_SHADER_NO_SWIZZLE;
    }
    srcs[0].reg.u.immconst_u32[0] = srcs[0].reg.u.immconst_u32[1] = srcs[0].reg.u.immconst_u32[2] = srcs[0].reg.u.immconst_u32[3] = 8;
    srcs[1].reg.u.immconst_u32[1] = 8;
    srcs[1].reg.u.immconst_u32[2] = 16;
    srcs[1].reg.u.immconst_u32[3] = 24;
    srcs[2] = x64arm_src_value(sm6, operands[1], 0);
    srcs[2].reg.data_type = type;
    srcs[2].swizzle = VKD3D_SHADER_SWIZZLE(X, X, X, X);
    x64arm_emit_result(sm6, state, mode ? VSIR_OP_IBFE : VSIR_OP_UBFE, srcs, 3, 4);
}

/* DXIL PackMode: 0 Trunc, 1 UClamp (0..255), 2 SClamp (-128..127). */
static void sm6_parser_emit_dx_pack4x8(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    unsigned int mode = sm6_value_get_constant_uint(operands[0], sm6), c, id, acc = UINT_MAX;
    struct vsir_src_operand srcs[2], component;

    if (mode > 2)
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND, "Invalid Pack4x8 mode %u.", mode);
        return;
    }
    for (c = 0; c < 4; ++c)
    {
        component = x64arm_widen(sm6, state, x64arm_src_value(sm6, operands[1 + c], 0));
        component.reg.data_type = VSIR_DATA_I32;
        if (mode)
        {
            srcs[0] = component;
            srcs[1] = x64arm_src_u32(mode == 1 ? 0u : (uint32_t)-128, VSIR_DATA_I32);
            id = x64arm_emit(sm6, state, VSIR_OP_IMAX, VSIR_DATA_I32, 1, srcs, 2);
            srcs[0] = x64arm_src_ssa(id, VSIR_DATA_I32);
            srcs[1] = x64arm_src_u32(mode == 1 ? 255u : 127u, VSIR_DATA_I32);
            id = x64arm_emit(sm6, state, VSIR_OP_IMIN, VSIR_DATA_I32, 1, srcs, 2);
            component = x64arm_src_ssa(id, VSIR_DATA_I32);
        }
        srcs[0] = component;
        srcs[0].reg.data_type = VSIR_DATA_U32;
        srcs[1] = x64arm_src_u32(0xff, VSIR_DATA_U32);
        id = x64arm_emit(sm6, state, VSIR_OP_AND, VSIR_DATA_U32, 1, srcs, 2);
        if (c)
        {
            srcs[0] = x64arm_src_ssa(id, VSIR_DATA_U32);
            srcs[1] = x64arm_src_u32(8 * c, VSIR_DATA_U32);
            id = x64arm_emit(sm6, state, VSIR_OP_ISHL, VSIR_DATA_U32, 1, srcs, 2);
            srcs[0] = x64arm_src_ssa(acc, VSIR_DATA_U32);
            srcs[1] = x64arm_src_ssa(id, VSIR_DATA_U32);
            id = x64arm_emit(sm6, state, VSIR_OP_OR, VSIR_DATA_U32, 1, srcs, 2);
        }
        acc = id;
    }
    srcs[0] = x64arm_src_ssa(acc, VSIR_DATA_U32);
    x64arm_emit_result(sm6, state, VSIR_OP_MOV, srcs, 1, 1);
}

/* Compute-shader quads (SM6.6 derivatives and quad ops in compute). D3D forms quads from 4 consecutive threads in
 * 1D thread groups, which matches Metal quad-groups, and from 2x2 blocks of SV_GroupThreadID.xy otherwise. Metal
 * SIMD-groups (32 lanes) follow the linear thread order, so when the group width divides 16 the 2x2 block stays in
 * one SIMD-group and a quad neighbour is lane + dx + dy * width.
 * Returns 0 for native quads (non-compute, or 1D groups), 1 for 2x2 emulation, -1 when unsupported. */
static int x64arm_compute_quad_mode(struct sm6_parser *sm6)
{
    const struct vsir_thread_group_size *size = &sm6->program->thread_group_size;

    if (sm6->program->shader_version.type != VKD3D_SHADER_TYPE_COMPUTE || (size->y <= 1 && size->z <= 1))
        return 0;
    if (!size->x || (size->x & 1) || (size->y & 1) || 16 % size->x)
        return -1;
    return 1;
}

static bool x64arm_compute_quad_supported(struct sm6_parser *sm6, int mode)
{
    const struct vsir_thread_group_size *size = &sm6->program->thread_group_size;

    if (mode >= 0)
        return true;
    vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_UNHANDLED_INTRINSIC,
            "Compute quads for thread group %ux%ux%u are not supported (the width must divide 16 and the height must be even).",
            size->x, size->y, size->z);
    return false;
}

static unsigned int x64arm_local_thread_component(struct sm6_parser *sm6, struct function_emission_state *state,
        unsigned int component)
{
    struct vsir_src_operand src;

    sm6_parser_dcl_register_builtin(sm6, VSIR_OP_DCL_INPUT, VSIR_REGISTER_LOCALTHREADID, VSIR_DATA_U32, 3);
    memset(&src, 0, sizeof(src));
    vsir_operand_init(&src.reg, VSIR_REGISTER_LOCALTHREADID, VSIR_DATA_U32, 0);
    src.reg.dimension = VSIR_DIMENSION_VEC4;
    src_param_init_scalar(&src, component);
    return x64arm_emit(sm6, state, VSIR_OP_MOV, VSIR_DATA_U32, 1, &src, 1);
}

static unsigned int x64arm_binary_u32(struct sm6_parser *sm6, struct function_emission_state *state,
        enum vkd3d_shader_opcode opcode, struct vsir_src_operand a, struct vsir_src_operand b)
{
    struct vsir_src_operand srcs[2] = {a, b};

    srcs[0].reg.data_type = srcs[1].reg.data_type = VSIR_DATA_U32;
    return x64arm_emit(sm6, state, opcode, VSIR_DATA_U32, 1, srcs, 2);
}

/* The caller's quad lane (0..3). */
static unsigned int x64arm_quad_self(struct sm6_parser *sm6, struct function_emission_state *state, int mode)
{
    unsigned int x, y;

    if (!mode)
        return x64arm_binary_u32(sm6, state, VSIR_OP_AND, x64arm_src_ssa(x64arm_lane_index(sm6, state), VSIR_DATA_U32),
                x64arm_src_u32(3, VSIR_DATA_U32));
    x = x64arm_binary_u32(sm6, state, VSIR_OP_AND, x64arm_src_ssa(x64arm_local_thread_component(sm6, state, 0), VSIR_DATA_U32),
            x64arm_src_u32(1, VSIR_DATA_U32));
    y = x64arm_binary_u32(sm6, state, VSIR_OP_AND, x64arm_src_ssa(x64arm_local_thread_component(sm6, state, 1), VSIR_DATA_U32),
            x64arm_src_u32(1, VSIR_DATA_U32));
    y = x64arm_binary_u32(sm6, state, VSIR_OP_ISHL, x64arm_src_ssa(y, VSIR_DATA_U32), x64arm_src_u32(1, VSIR_DATA_U32));
    return x64arm_binary_u32(sm6, state, VSIR_OP_OR, x64arm_src_ssa(x, VSIR_DATA_U32), x64arm_src_ssa(y, VSIR_DATA_U32));
}

/* The SIMD lane holding quad lane `target` (0..3) of the caller's quad. */
static unsigned int x64arm_quad_target_lane(struct sm6_parser *sm6, struct function_emission_state *state, int mode,
        struct vsir_src_operand target)
{
    unsigned int lane = x64arm_lane_index(sm6, state), self, tx, ty, sx, sy, dx, dy, delta;
    struct vsir_src_operand srcs[3];

    if (!mode)
    {
        unsigned int base = x64arm_binary_u32(sm6, state, VSIR_OP_AND, x64arm_src_ssa(lane, VSIR_DATA_U32),
                x64arm_src_u32(~3u, VSIR_DATA_U32));
        unsigned int index = x64arm_binary_u32(sm6, state, VSIR_OP_AND, target, x64arm_src_u32(3, VSIR_DATA_U32));
        return x64arm_binary_u32(sm6, state, VSIR_OP_OR, x64arm_src_ssa(base, VSIR_DATA_U32), x64arm_src_ssa(index, VSIR_DATA_U32));
    }
    self = x64arm_quad_self(sm6, state, mode);
    tx = x64arm_binary_u32(sm6, state, VSIR_OP_AND, target, x64arm_src_u32(1, VSIR_DATA_U32));
    ty = x64arm_binary_u32(sm6, state, VSIR_OP_USHR, target, x64arm_src_u32(1, VSIR_DATA_U32));
    ty = x64arm_binary_u32(sm6, state, VSIR_OP_AND, x64arm_src_ssa(ty, VSIR_DATA_U32), x64arm_src_u32(1, VSIR_DATA_U32));
    sx = x64arm_binary_u32(sm6, state, VSIR_OP_AND, x64arm_src_ssa(self, VSIR_DATA_U32), x64arm_src_u32(1, VSIR_DATA_U32));
    sy = x64arm_binary_u32(sm6, state, VSIR_OP_USHR, x64arm_src_ssa(self, VSIR_DATA_U32), x64arm_src_u32(1, VSIR_DATA_U32));
    /* Two's complement differences: (t - s) wraps, and lane + delta wraps back into range. */
    srcs[0] = x64arm_src_ssa(sx, VSIR_DATA_U32);
    sx = x64arm_emit(sm6, state, VSIR_OP_INEG, VSIR_DATA_U32, 1, srcs, 1);
    srcs[0] = x64arm_src_ssa(sy, VSIR_DATA_U32);
    sy = x64arm_emit(sm6, state, VSIR_OP_INEG, VSIR_DATA_U32, 1, srcs, 1);
    dx = x64arm_binary_u32(sm6, state, VSIR_OP_IADD, x64arm_src_ssa(tx, VSIR_DATA_U32), x64arm_src_ssa(sx, VSIR_DATA_U32));
    dy = x64arm_binary_u32(sm6, state, VSIR_OP_IADD, x64arm_src_ssa(ty, VSIR_DATA_U32), x64arm_src_ssa(sy, VSIR_DATA_U32));
    srcs[0] = x64arm_src_ssa(dy, VSIR_DATA_U32);
    srcs[1] = x64arm_src_u32(sm6->program->thread_group_size.x, VSIR_DATA_U32);
    srcs[2] = x64arm_src_ssa(dx, VSIR_DATA_U32);
    delta = x64arm_emit(sm6, state, VSIR_OP_IMAD, VSIR_DATA_U32, 1, srcs, 3);
    return x64arm_binary_u32(sm6, state, VSIR_OP_IADD, x64arm_src_ssa(lane, VSIR_DATA_U32), x64arm_src_ssa(delta, VSIR_DATA_U32));
}

static unsigned int x64arm_quad_read(struct sm6_parser *sm6, struct function_emission_state *state, int mode,
        struct vsir_src_operand value, struct vsir_src_operand target)
{
    struct vsir_src_operand srcs[2];

    srcs[0] = value;
    srcs[1] = x64arm_src_ssa(x64arm_quad_target_lane(sm6, state, mode, target), VSIR_DATA_U32);
    return x64arm_emit(sm6, state, VSIR_OP_WAVE_READ_LANE_AT, value.reg.data_type, 1, srcs, 2);
}

/* Derivatives in compute shaders: coarse X/Y are lane 1 - lane 0 and lane 2 - lane 0 of the quad; fine derivatives
 * use the caller's own row or column. Pixel shaders keep the native instructions. */
static void sm6_parser_emit_dx_derivative(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    unsigned int axis = (op == DX_DERIV_COARSEY || op == DX_DERIV_FINEY) ? 2 : 1, low, high, a, b;
    struct vsir_src_operand value, low_lane, high_lane, srcs[2];
    int mode;

    if (sm6->program->shader_version.type != VKD3D_SHADER_TYPE_COMPUTE)
    {
        sm6_parser_emit_dx_unary(sm6, op, operands, state);
        return;
    }
    if (!x64arm_compute_quad_supported(sm6, mode = x64arm_compute_quad_mode(sm6)))
        return;
    value = x64arm_src_value(sm6, operands[0], 0);
    if (op == DX_DERIV_FINEX || op == DX_DERIV_FINEY)
    {
        unsigned int self = x64arm_quad_self(sm6, state, mode);

        low = x64arm_binary_u32(sm6, state, VSIR_OP_AND, x64arm_src_ssa(self, VSIR_DATA_U32), x64arm_src_u32(~axis, VSIR_DATA_U32));
        high = x64arm_binary_u32(sm6, state, VSIR_OP_OR, x64arm_src_ssa(self, VSIR_DATA_U32), x64arm_src_u32(axis, VSIR_DATA_U32));
        low_lane = x64arm_src_ssa(low, VSIR_DATA_U32);
        high_lane = x64arm_src_ssa(high, VSIR_DATA_U32);
    }
    else
    {
        low_lane = x64arm_src_u32(0, VSIR_DATA_U32);
        high_lane = x64arm_src_u32(axis, VSIR_DATA_U32);
    }
    a = x64arm_quad_read(sm6, state, mode, value, low_lane);
    b = x64arm_quad_read(sm6, state, mode, value, high_lane);
    srcs[0] = x64arm_src_ssa(a, value.reg.data_type);
    a = x64arm_emit(sm6, state, VSIR_OP_NEG, value.reg.data_type, 1, srcs, 1);
    srcs[0] = x64arm_src_ssa(b, value.reg.data_type);
    srcs[1] = x64arm_src_ssa(a, value.reg.data_type);
    x64arm_emit_result(sm6, state, VSIR_OP_ADD, srcs, 2, 1);
}

/* Quad ops in 2D compute groups read the 2x2 neighbour through a SIMD shuffle; elsewhere they stay native. */
static void sm6_parser_emit_dx_quad_op_x64arm(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    int mode = x64arm_compute_quad_mode(sm6);
    struct vsir_src_operand srcs[2];
    unsigned int kind, self, target;

    if (!mode)
    {
        if (op == DX_QUAD_OP)
            sm6_parser_emit_dx_quad_op(sm6, op, operands, state);
        else
            sm6_parser_emit_dx_binary(sm6, op, operands, state);
        return;
    }
    if (!x64arm_compute_quad_supported(sm6, mode))
        return;
    srcs[0] = x64arm_src_value(sm6, operands[0], 0);
    if (op == DX_QUAD_OP)
    {
        /* QUAD_READ_ACROSS_X / Y / D: the neighbour's quad lane is self ^ 1 / 2 / 3. */
        if ((kind = sm6_value_get_constant_uint(operands[1], sm6)) > QUAD_READ_ACROSS_D)
        {
            vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_UNHANDLED_INTRINSIC, "Quad op kind %u is unhandled.", kind);
            return;
        }
        self = x64arm_quad_self(sm6, state, mode);
        target = x64arm_binary_u32(sm6, state, VSIR_OP_XOR, x64arm_src_ssa(self, VSIR_DATA_U32), x64arm_src_u32(kind + 1, VSIR_DATA_U32));
        srcs[1] = x64arm_src_ssa(x64arm_quad_target_lane(sm6, state, mode, x64arm_src_ssa(target, VSIR_DATA_U32)), VSIR_DATA_U32);
    }
    else
    {
        srcs[1] = x64arm_src_ssa(x64arm_quad_target_lane(sm6, state, mode, x64arm_src_value(sm6, operands[1], 0)), VSIR_DATA_U32);
    }
    x64arm_emit_result(sm6, state, VSIR_OP_WAVE_READ_LANE_AT, srcs, 2, 1);
}

/* Helper lanes exist only in pixel shaders; elsewhere IsHelperLane() is false. Killed lanes terminate (OpKill),
 * so the HelperInvocation built-in stays exact. */
static void sm6_parser_emit_dx_is_helper_lane(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    struct vsir_src_operand srcs[2];

    if (sm6->program->shader_version.type == VKD3D_SHADER_TYPE_PIXEL)
    {
        sm6_parser_emit_dx_input_register_mov(sm6, state, VSIR_REGISTER_HELPERLANE, VSIR_DATA_BOOL, true);
        return;
    }
    srcs[0] = x64arm_src_u32(0, VSIR_DATA_U32);
    srcs[1] = x64arm_src_u32(1, VSIR_DATA_U32);
    x64arm_emit_result(sm6, state, VSIR_OP_IEQ, srcs, 2, 1);
}

struct sm6_dx_opcode_info
{]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    [DX_WAVE_PREFIX_BIT_COUNT         ] = {"i", "1",    sm6_parser_emit_dx_unary},]=]
  [=[    [DX_WAVE_PREFIX_BIT_COUNT         ] = {"i", "1",    sm6_parser_emit_dx_unary},
    [DX_DOT2_ADD_HALF                 ] = {"f", "feeee", sm6_parser_emit_dx_dot2_add_half},
    [DX_DOT4_ADD_I8_PACKED            ] = {"i", "iii",  sm6_parser_emit_dx_dot4_add_packed},
    [DX_DOT4_ADD_U8_PACKED            ] = {"i", "iii",  sm6_parser_emit_dx_dot4_add_packed},
    [DX_WAVE_MATCH                    ] = {"V", "n",    sm6_parser_emit_dx_wave_match},
    [DX_WAVE_MULTI_PREFIX_OP          ] = {"n", "Riiiicc", sm6_parser_emit_dx_wave_multi_prefix_op},
    [DX_WAVE_MULTI_PREFIX_BIT_COUNT   ] = {"i", "1iiii", sm6_parser_emit_dx_wave_multi_prefix_bit_count},
    [DX_UNPACK4X8                     ] = {"o", "ci",   sm6_parser_emit_dx_unpack4x8},
    [DX_PACK4X8                       ] = {"i", "cmmmm", sm6_parser_emit_dx_pack4x8},
    [DX_IS_HELPER_LANE                ] = {"1", "",     sm6_parser_emit_dx_is_helper_lane},]=])
foreach(x64arm_deriv COARSEX COARSEY FINEX FINEY)
  string(LENGTH "DX_DERIV_${x64arm_deriv}" x64arm_deriv_length)
  math(EXPR x64arm_deriv_pad "33 - ${x64arm_deriv_length}")
  string(REPEAT " " ${x64arm_deriv_pad} x64arm_deriv_spaces)
  x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
    "    [DX_DERIV_${x64arm_deriv}${x64arm_deriv_spaces}] = {\"e\", \"R\",    sm6_parser_emit_dx_unary},"
    "    [DX_DERIV_${x64arm_deriv}${x64arm_deriv_spaces}] = {\"e\", \"R\",    sm6_parser_emit_dx_derivative},")
endforeach()
x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    [DX_QUAD_OP                       ] = {"n", "Rc",   sm6_parser_emit_dx_quad_op},
    [DX_QUAD_READ_LANE_AT             ] = {"n", "Ri",   sm6_parser_emit_dx_binary},]=]
  [=[    [DX_QUAD_OP                       ] = {"n", "Rc",   sm6_parser_emit_dx_quad_op_x64arm},
    [DX_QUAD_READ_LANE_AT             ] = {"n", "Ri",   sm6_parser_emit_dx_quad_op_x64arm},]=])

# IsHelperLane 的 VSIR 暫存器：對應 SPIR-V HelperInvocation（MSL simd_is_helper_thread()）。
set(X64ARM_VKD3D_SHADER_DIR "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader")
x64arm_vkd3d_patch("${X64ARM_VKD3D_SHADER_DIR}/vkd3d_shader_private.h"
  [=[    VSIR_REGISTER_OUT_POINT_SIZE,

    VSIR_REGISTER_TYPE_COUNT,]=]
  [=[    VSIR_REGISTER_OUT_POINT_SIZE,
    VSIR_REGISTER_HELPERLANE,

    VSIR_REGISTER_TYPE_COUNT,]=])
x64arm_vkd3d_patch("${X64ARM_VKD3D_SHADER_DIR}/ir.c"
  [=[        [VSIR_REGISTER_WAVELANEINDEX     ] = "vWaveLaneIndex",]=]
  [=[        [VSIR_REGISTER_WAVELANEINDEX     ] = "vWaveLaneIndex",
        [VSIR_REGISTER_HELPERLANE        ] = "vHelperLane",]=])
# 其餘把 vWaveLaneIndex 當成無索引內建輸入的分支（宣告、驗證、可寫性等）一併涵蓋。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SHADER_DIR}/ir.c"
  [=[case VSIR_REGISTER_WAVELANEINDEX:
]=]
  [=[case VSIR_REGISTER_WAVELANEINDEX: case VSIR_REGISTER_HELPERLANE:
]=])
x64arm_vkd3d_patch("${X64ARM_VKD3D_SHADER_DIR}/spirv.c"
  [=[    {VSIR_REGISTER_WAVELANEINDEX,    SpvStorageClassInput,  {VSIR_DATA_U32, 1, SpvBuiltInSubgroupLocalInvocationId}},]=]
  [=[    {VSIR_REGISTER_WAVELANEINDEX,    SpvStorageClassInput,  {VSIR_DATA_U32, 1, SpvBuiltInSubgroupLocalInvocationId}},
    {VSIR_REGISTER_HELPERLANE,       SpvStorageClassInput,  {VSIR_DATA_BOOL, 1, SpvBuiltInHelperInvocation}},]=])
x64arm_vkd3d_patch("${X64ARM_VKD3D_SHADER_DIR}/spirv.c"
  [=[        case VSIR_REGISTER_WAVELANEINDEX:
            snprintf(buffer, buffer_size, "vWaveLaneIndex");
            break;]=]
  [=[        case VSIR_REGISTER_WAVELANEINDEX:
            snprintf(buffer, buffer_size, "vWaveLaneIndex");
            break;
        case VSIR_REGISTER_HELPERLANE:
            snprintf(buffer, buffer_size, "vHelperLane");
            break;]=])
x64arm_vkd3d_patch("${X64ARM_VKD3D_SHADER_DIR}/d3d_asm.c"
  [=[        X(OUT_POINT_SIZE)
#undef X]=]
  [=[        X(OUT_POINT_SIZE)
        X(HELPERLANE)
#undef X]=])

# SM6.6 [WaveSize(N)]（tag 11，!{i32 N}）與 SM6.8 [WaveSize(min,max,pref)]（tag 23）：
# 硬體 wave 寬固定 32（OPTIONS1 回報 WaveLaneCountMin／Max = 32），涵蓋 32 才接受，否則與實機一樣無法建立。
x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    SHADER_PROPERTIES_ENTRY_ROOT_SIG     = 12,
};]=]
  [=[    SHADER_PROPERTIES_ENTRY_ROOT_SIG     = 12,
    SHADER_PROPERTIES_WAVE_SIZE_RANGE    = 23,
};]=])
x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[                default:
                    vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_PROPERTIES,
                            "Shader properties tag %#x is unhandled.", tag);
                    break;]=]
  [=[                case SHADER_PROPERTIES_WAVE_SIZE:
                case SHADER_PROPERTIES_WAVE_SIZE_RANGE:
                {
                    const struct sm6_metadata_value *wave = node->operands[i + 1];
                    unsigned int sizes[3] = {0}, size_count = 0, k;

                    if (sm6_metadata_value_is_node(wave))
                    {
                        for (k = 0; k < wave->u.node->operand_count && k < 3; ++k)
                            if (sm6_metadata_get_uint_value(sm6, wave->u.node->operands[k], &sizes[k]))
                                ++size_count;
                    }
                    else if (sm6_metadata_get_uint_value(sm6, wave, &sizes[0]))
                    {
                        size_count = 1;
                    }
                    if (size_count < 2 || !sizes[1])
                        sizes[1] = sizes[0];
                    if (!size_count || sizes[0] > 32 || sizes[1] < 32)
                        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_PROPERTIES,
                                "Wave size %u..%u does not include the hardware wave size 32.", sizes[0], sizes[1]);
                    break;
                }
                default:
                    vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_PROPERTIES,
                            "Shader properties tag %#x is unhandled.", tag);
                    break;]=])
