# vkd3d 2.1 HLSL 編譯器的 SM5／5.1 補齊（內建 D3DCompile，D3D12 程式以 *_5_1 執行期編譯）。各段：
#  1. SM 5.1 描述元陣列（Texture2D t[4] 等）：語法層原本以「尚未實作」拒絕；後段已具備 5.1 契約（單一 id、連續暫存器、
#     外部資源以整個變數回報、運算元 [id, 基底＋偏移]）。這裡解除限制，宣告改為單一範圍 dcl T{id}[first:last]。
#  2. 動態索引：NonUniformResourceIndex()、驗證放行、運算元第二個索引帶相對暫存器（TPF 寫出端已支援）。
#  3. UAV 方法：RWTexture* 的 GetDimensions／Load、RWByteAddressBuffer 的 Load2～4 與 Interlocked*、
#     RWStructuredBuffer 的 IncrementCounter／DecrementCounter（interlocked 新增 alloc／consume，計數器旗標進宣告、反射與描述元資訊）。
#     方法表由 Vkd3dBufferDimensions.cmake 統一寫入，這裡只插入處理函式。
#  4. reversebits()（bfrev，含常數摺疊）與 TPF 寫出分派。
# 結構成員中的資源陣列、Append／ConsumeStructuredBuffer、tex.mips[][] 與資源陣列參數大小不同仍未支援。
# 以 x64arm_vkd3d_patch 套用（定義於 Vkd3dBufferDimensions.cmake）。修改插入式修補的內容時，要把上一版內容列為升級來源，
# 否則既有解開的來源會留下上一版插入；同一處只能有一個修補。

# 變數宣告（hlsl.y 與預先產生的 hlsl.tab.c 文字相同）。
foreach(path "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.y" "${vkd3d_parsers_SOURCE_DIR}/hlsl.tab.c")
  x64arm_vkd3d_patch("${path}"
    [=[    if (hlsl_version_ge(ctx, 5, 1) && type->class == HLSL_CLASS_ARRAY && hlsl_type_is_resource(type))
    {
        /* SM 5.1/6.x descriptor arrays act differently from previous versions.
         * Not only are they treated as a single object in reflection, but they
         * act as a single component for the purposes of assignment and
         * initialization. */
        hlsl_fixme(ctx, &v->loc, "Shader model 5.1+ resource array.");
    }
]=]
    [=[    /* x64arm: SM 5.1 descriptor arrays are reflected and declared as one range
     * (see Vkd3dSm51ResourceArrays.cmake); constant indices are supported. */
]=])
endforeach()

# 函式參數：hlsl.y 與 hlsl.tab.c 的位置參照寫法不同。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.y"
  [=[            if (hlsl_version_ge(ctx, 5, 1) && type->class == HLSL_CLASS_ARRAY && hlsl_type_is_resource(type))
                hlsl_fixme(ctx, &@2, "Shader model 5.1+ resource array.");
]=]
  [=[            /* x64arm: SM 5.1 resource array parameters are accepted (constant indices). */
]=])
x64arm_vkd3d_patch("${vkd3d_parsers_SOURCE_DIR}/hlsl.tab.c"
  [=[            if (hlsl_version_ge(ctx, 5, 1) && type->class == HLSL_CLASS_ARRAY && hlsl_type_is_resource(type))
                hlsl_fixme(ctx, &(yylsp[-3]), "Shader model 5.1+ resource array.");
]=]
  [=[            /* x64arm: SM 5.1 resource array parameters are accepted (constant indices). */
]=])

set(X64ARM_VKD3D_SM51_CODEGEN "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c")

# 範圍內是否有任何元素被使用（SM 5.1 整段宣告；SM 4／5.0 逐元素判斷）。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  [=[static void sm4_generate_vsir_add_dcl_sampler(struct hlsl_ctx *ctx,
        struct vsir_program *program, const struct extern_resource *resource)
{]=]
  [=[/* x64arm: SM 5.1 declares a whole descriptor array; skip it only if no element is used. */
static bool x64arm_resource_used(const struct hlsl_ctx *ctx, const struct extern_resource *resource,
        enum hlsl_regset regset, unsigned int i)
{
    unsigned int k;

    if (!resource->var)
        return true;
    if (!hlsl_version_ge(ctx, 5, 1))
        return resource->var->objects_usage[regset][i].used;
    for (k = 0; k < resource->bind_count; ++k)
    {
        if (resource->var->objects_usage[regset][k].used)
            return true;
    }
    return false;
}

static void sm4_generate_vsir_add_dcl_sampler(struct hlsl_ctx *ctx,
        struct vsir_program *program, const struct extern_resource *resource)
{]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  [=[    VKD3D_ASSERT(resource->regset == HLSL_REGSET_SAMPLERS);
    VKD3D_ASSERT(hlsl_version_lt(ctx, 5, 1) || resource->bind_count == 1);

    for (i = 0; i < resource->bind_count; ++i)
    {
        unsigned int array_first = resource->index + i;
        unsigned int array_last = resource->index + i; /* FIXME: array end. */

        if (resource->var && !resource->var->objects_usage[HLSL_REGSET_SAMPLERS][i].used)
            continue;
]=]
  [=[    VKD3D_ASSERT(resource->regset == HLSL_REGSET_SAMPLERS);

    for (i = 0; i < (hlsl_version_ge(ctx, 5, 1) ? 1 : resource->bind_count); ++i)
    {
        unsigned int array_first = resource->index + i;
        unsigned int array_last = hlsl_version_ge(ctx, 5, 1)
                ? resource->index + resource->bind_count - 1 : resource->index + i;

        if (!x64arm_resource_used(ctx, resource, HLSL_REGSET_SAMPLERS, i))
            continue;
]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  [=[    VKD3D_ASSERT(resource->regset == regset);
    VKD3D_ASSERT(hlsl_version_lt(ctx, 5, 1) || resource->bind_count == 1);

    component_type = resource->component_type;

    for (i = 0; i < resource->bind_count; ++i)
    {
        unsigned int array_first = resource->index + i;
        unsigned int array_last = resource->index + i; /* FIXME: array end. */
        struct vkd3d_shader_resource *vsir_resource;

        if (resource->var && !resource->var->objects_usage[regset][i].used)
            continue;
]=]
  [=[    VKD3D_ASSERT(resource->regset == regset);

    component_type = resource->component_type;

    for (i = 0; i < (hlsl_version_ge(ctx, 5, 1) ? 1 : resource->bind_count); ++i)
    {
        unsigned int array_first = resource->index + i;
        unsigned int array_last = hlsl_version_ge(ctx, 5, 1)
                ? resource->index + resource->bind_count - 1 : resource->index + i;
        struct vkd3d_shader_resource *vsir_resource;

        if (!x64arm_resource_used(ctx, resource, regset, i))
            continue;
]=])

# ---- 動態索引（NonUniformResourceIndex 與非常數描述元索引） ----
# 前段已具備：非常數索引時使用標記落在陣列最後一個元素（bind_count＝陣列大小、整段宣告），
# deref 降為以資源暫存器為單位的 rel_offset；TPF 寫出端支援任意索引的相對定址。
# 這裡補上內建函式、放行驗證，並讓 SM 5.1 資源運算元以 [id, 基底＋常數＋相對暫存器] 編碼。

# NonUniformResourceIndex(i)：索引可能在波內分歧的提示，值即為索引本身（無號整數）；消費端（SPIR-V 描述元索引）不依賴此提示。
foreach(path "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.y" "${vkd3d_parsers_SOURCE_DIR}/hlsl.tab.c")
  x64arm_vkd3d_patch("${path}"
    [=[static const struct intrinsic_function
{
    const char *name;]=]
    [=[/* x64arm: NonUniformResourceIndex() only marks a possibly divergent descriptor index. */
static bool intrinsic_NonUniformResourceIndex(struct hlsl_ctx *ctx,
        const struct parse_initializer *params, const struct vkd3d_shader_location *loc)
{
    return !!hlsl_block_add_cast(ctx, params->instrs, params->args[0],
            hlsl_get_scalar_type(ctx, HLSL_TYPE_UINT), loc);
}

static const struct intrinsic_function
{
    const char *name;]=])
  x64arm_vkd3d_patch("${path}"
    [=[    {"InterlockedXor",                     -1, true,  intrinsic_InterlockedXor},
]=]
    [=[    {"InterlockedXor",                     -1, true,  intrinsic_InterlockedXor},
    {"NonUniformResourceIndex",             1, true,  intrinsic_NonUniformResourceIndex},
]=])
endforeach()

# 驗證：SM 5.1 的 uniform 描述元陣列允許非常數索引。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  [=[static bool validate_dereferences(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr,
        void *context)
{]=]
  [=[/* x64arm: SM 5.1 descriptor arrays may be indexed dynamically. */
static bool x64arm_dynamic_descriptor(struct hlsl_ctx *ctx, const struct hlsl_deref *deref)
{
    return hlsl_version_ge(ctx, 5, 1) && deref->var->is_uniform
            && deref->var->data_type->class == HLSL_CLASS_ARRAY && hlsl_type_is_resource(deref->var->data_type);
}

static bool validate_dereferences(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr,
        void *context)
{]=])
foreach(field "load->resource" "load->sampler" "store->resource")
  x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
    "else if (validate_component_index_range_from_deref(ctx, &${field}) == DEREF_VALIDATION_NOT_CONSTANT)\n"
    "else if (validate_component_index_range_from_deref(ctx, &${field}) == DEREF_VALIDATION_NOT_CONSTANT\n                    && !x64arm_dynamic_descriptor(ctx, &${field}))\n")
endforeach()

# 運算元：SM 5.1 資源／UAV／取樣器的第二個索引＝基底＋常數偏移＋相對暫存器。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  [=[static bool sm4_generate_vsir_reg_from_deref(struct hlsl_ctx *ctx, struct vsir_program *program,
        struct vsir_operand *reg, uint32_t *writemask, const struct hlsl_deref *deref)
{]=]
  [=[/* x64arm: SM 5.1 descriptor array with a dynamic index. */
static bool x64arm_descriptor_index(struct hlsl_ctx *ctx, struct vsir_program *program,
        struct vsir_operand *reg, const struct hlsl_deref *deref)
{
    if (!deref->rel_offset.node || vkd3d_shader_ver_le(&program->shader_version, 5, 0))
        return false;
    reg->idx[1].offset += deref->const_offset;
    reg->idx[1].rel_addr = sm4_generate_vsir_new_idx_src(ctx, program, deref->rel_offset.node);
    return true;
}

static bool sm4_generate_vsir_reg_from_deref(struct hlsl_ctx *ctx, struct vsir_program *program,
        struct vsir_operand *reg, uint32_t *writemask, const struct hlsl_deref *deref)
{]=])
foreach(set "TEXTURES" "UAVS" "SAMPLERS")
  x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
    "            reg->idx[1].offset = var->regs[HLSL_REGSET_${set}].index;\n            reg->idx[1].offset += hlsl_offset_from_deref_safe(ctx, deref);\n"
    "            reg->idx[1].offset = var->regs[HLSL_REGSET_${set}].index;\n            if (!x64arm_descriptor_index(ctx, program, reg, deref))\n                reg->idx[1].offset += hlsl_offset_from_deref_safe(ctx, deref);\n")
endforeach()

# ---- UAV 物件方法 ----
# RWTexture1D／2D／3D／1DArray／2DArray 的 GetDimensions（resinfo 可讀 UAV，產碼已通用）；Load(位置) 等同 uav[位置]
# （沿用索引節點的降階），RWByteAddressBuffer 的 Load／Load2～4 為原始讀取；RWStructuredBuffer 的 IncrementCounter／
# DecrementCounter 見下段。方法表本身由 Vkd3dBufferDimensions.cmake 統一寫入，這裡只在表格前插入處理函式。
foreach(path "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.y" "${vkd3d_parsers_SOURCE_DIR}/hlsl.tab.c")
  x64arm_vkd3d_patch("${path}"
    [=[static const struct method_function uav_methods[] =
]=]
    [=[/* x64arm: UAV Load(location) is the same access as uav[location]; raw buffers use raw loads. */
static bool x64arm_add_uav_load_method_call(struct hlsl_ctx *ctx, struct hlsl_block *block, struct hlsl_ir_node *object,
        const char *name, const struct parse_initializer *params, const struct vkd3d_shader_location *loc)
{
    const struct hlsl_type *object_type = object->data_type;
    struct hlsl_ir_node *coords;

    if (object_type->sampler_dim == HLSL_SAMPLER_DIM_RAW_BUFFER)
        return add_raw_load_method_call(ctx, block, object, name, params, loc);
    if (params->args_count != 1)
    {
        hlsl_error(ctx, loc, VKD3D_SHADER_ERROR_HLSL_WRONG_PARAMETER_COUNT,
                "Wrong number of arguments to method '%s': expected 1, but got %u.", name, params->args_count);
        return false;
    }
    coords = add_implicit_conversion(ctx, block, params->args[0],
            hlsl_get_vector_type(ctx, HLSL_TYPE_UINT, hlsl_sampler_dim_count(object_type->sampler_dim)), loc);
    hlsl_block_add_index(ctx, block, object, coords, loc);
    return true;
}

/* x64arm: RWStructuredBuffer hidden counter. */
static bool x64arm_add_counter_method_call(struct hlsl_ctx *ctx, struct hlsl_block *block, struct hlsl_ir_node *object,
        const char *name, const struct parse_initializer *params, const struct vkd3d_shader_location *loc)
{
    struct hlsl_deref resource_deref;
    struct hlsl_ir_node *counter;

    if (params->args_count)
    {
        hlsl_error(ctx, loc, VKD3D_SHADER_ERROR_HLSL_WRONG_PARAMETER_COUNT,
                "Wrong number of arguments to method '%s': expected 0, but got %u.", name, params->args_count);
        return false;
    }
    if (!hlsl_deref_init_from_index_chain(&resource_deref, object, ctx))
        return false;
    counter = hlsl_new_interlocked(ctx, !strcmp(name, "IncrementCounter") ? HLSL_INTERLOCKED_ALLOC : HLSL_INTERLOCKED_CONSUME,
            hlsl_get_scalar_type(ctx, HLSL_TYPE_UINT), &resource_deref, NULL, NULL, NULL, loc);
    hlsl_deref_cleanup(&resource_deref);
    if (!counter)
        return false;
    hlsl_block_add_instr(block, counter);
    return true;
}

/* x64arm: RWByteAddressBuffer Interlocked*(address, [compare,] value [, out original]). */
static bool x64arm_add_raw_interlocked_method_call(struct hlsl_ctx *ctx, struct hlsl_block *block, struct hlsl_ir_node *object,
        const char *name, const struct parse_initializer *params, const struct vkd3d_shader_location *loc)
{
    static const struct
    {
        const char *name;
        enum hlsl_interlocked_op op;
        bool compare, original;
    }
    ops[] =
    {
        {"InterlockedAdd", HLSL_INTERLOCKED_ADD, false, false},
        {"InterlockedAnd", HLSL_INTERLOCKED_AND, false, false},
        {"InterlockedCompareExchange", HLSL_INTERLOCKED_CMP_EXCH, true, true},
        {"InterlockedCompareStore", HLSL_INTERLOCKED_CMP_EXCH, true, false},
        {"InterlockedExchange", HLSL_INTERLOCKED_EXCH, false, true},
        {"InterlockedMax", HLSL_INTERLOCKED_MAX, false, false},
        {"InterlockedMin", HLSL_INTERLOCKED_MIN, false, false},
        {"InterlockedOr", HLSL_INTERLOCKED_OR, false, false},
        {"InterlockedXor", HLSL_INTERLOCKED_XOR, false, false},
    };
    struct hlsl_ir_node *address, *compare = NULL, *value, *original = NULL, *interlocked;
    struct hlsl_type *value_type = hlsl_get_scalar_type(ctx, HLSL_TYPE_UINT);
    struct hlsl_deref resource_deref;
    unsigned int i, expected;

    for (i = 0; i < ARRAY_SIZE(ops) && strcmp(ops[i].name, name); ++i)
        ;
    VKD3D_ASSERT(i < ARRAY_SIZE(ops));
    expected = 2 + ops[i].compare;
    if (params->args_count < expected || params->args_count > expected + 1
            || (ops[i].original && params->args_count != expected + 1)
            || (!strcmp(name, "InterlockedCompareStore") && params->args_count != expected))
    {
        hlsl_error(ctx, loc, VKD3D_SHADER_ERROR_HLSL_WRONG_PARAMETER_COUNT,
                "Wrong number of arguments to method '%s': got %u.", name, params->args_count);
        return false;
    }
    /* Max/Min follow the signedness of the value; the others always use uint. */
    if ((ops[i].op == HLSL_INTERLOCKED_MAX || ops[i].op == HLSL_INTERLOCKED_MIN)
            && params->args[expected - 1]->data_type->class <= HLSL_CLASS_VECTOR
            && params->args[expected - 1]->data_type->e.numeric.type == HLSL_TYPE_INT)
        value_type = hlsl_get_scalar_type(ctx, HLSL_TYPE_INT);
    if (!(address = add_implicit_conversion(ctx, block, params->args[0], hlsl_get_scalar_type(ctx, HLSL_TYPE_UINT), loc)))
        return false;
    if (ops[i].compare && !(compare = add_implicit_conversion(ctx, block, params->args[1], value_type, loc)))
        return false;
    if (!(value = add_implicit_conversion(ctx, block, params->args[expected - 1], value_type, loc)))
        return false;
    if (params->args_count > expected)
        original = params->args[expected];
    if (!hlsl_deref_init_from_index_chain(&resource_deref, object, ctx))
        return false;
    interlocked = hlsl_new_interlocked(ctx, ops[i].op, original ? value_type : NULL, &resource_deref, address, compare, value, loc);
    hlsl_deref_cleanup(&resource_deref);
    if (!interlocked)
        return false;
    hlsl_block_add_instr(block, interlocked);
    if (original)
    {
        if (original->data_type->modifiers & HLSL_MODIFIER_CONST)
            hlsl_error(ctx, &original->loc, VKD3D_SHADER_ERROR_HLSL_MODIFIES_CONST,
                    "Output argument to '%s' is const.", name);
        if (!add_assignment(ctx, block, original, ASSIGN_OP_ASSIGN, interlocked, true))
            return false;
    }
    add_void_expr(ctx, block, loc);
    return true;
}

static const struct method_function uav_methods[] =
]=])
endforeach()


# ---- RWStructuredBuffer 隱藏計數器：IncrementCounter()／DecrementCounter() ----
# interlocked IR 新增 alloc／consume（目的地為整個 UAV、沒有座標與值），產生 imm_atomic_alloc／imm_atomic_consume；
# 使用標記 x64arm_counter 供 UAV 宣告旗標（ORDER_PRESERVING_COUNTER）、反射型別（RWSTRUCTURED_WITH_COUNTER）
# 與描述元資訊（UAV_COUNTER，SPIR-V 端據此宣告計數器緩衝）。
set(X64ARM_VKD3D_HLSL_H "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.h")
set(X64ARM_VKD3D_HLSL_C "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.c")
x64arm_vkd3d_patch("${X64ARM_VKD3D_HLSL_H}"
  "    HLSL_INTERLOCKED_XOR,\n};"
  "    HLSL_INTERLOCKED_XOR,\n    /* x64arm: UAV hidden counter (IncrementCounter / DecrementCounter). */\n    HLSL_INTERLOCKED_ALLOC,\n    HLSL_INTERLOCKED_CONSUME,\n};")
x64arm_vkd3d_patch("${X64ARM_VKD3D_HLSL_H}"
  "        bool uav_atomics;\n        enum hlsl_sampler_dim sampler_dim;"
  "        bool uav_atomics;\n        bool x64arm_counter;\n        enum hlsl_sampler_dim sampler_dim;")
x64arm_vkd3d_patch("${X64ARM_VKD3D_HLSL_H}"
  "    unsigned int id, space, index, bind_count;\n\n    struct vkd3d_shader_location loc;\n};"
  "    unsigned int id, space, index, bind_count;\n    bool x64arm_counter;\n\n    struct vkd3d_shader_location loc;\n};")
x64arm_vkd3d_patch("${X64ARM_VKD3D_HLSL_C}"
  "        [HLSL_INTERLOCKED_XOR] = \"xor\",\n"
  "        [HLSL_INTERLOCKED_XOR] = \"xor\",\n        [HLSL_INTERLOCKED_ALLOC] = \"alloc\",\n        [HLSL_INTERLOCKED_CONSUME] = \"consume\",\n")
x64arm_vkd3d_patch("${X64ARM_VKD3D_HLSL_C}"
  "    vkd3d_string_buffer_printf(buffer, \", value = \");\n    dump_src(buffer, &interlocked->value);\n"
  "    if (interlocked->value.node)\n    {\n        vkd3d_string_buffer_printf(buffer, \", value = \");\n        dump_src(buffer, &interlocked->value);\n    }\n")

# 計數器運算與原始緩衝的 Interlocked* 方法（已帶位元組位址座標）的目的地已是整個 UAV：不做元素存取降階。
# 同一處只能有一個修補（串接的修補在下次設定時找不到前一版文字）。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  "    interlocked = hlsl_ir_interlocked(instr);\n    deref = &interlocked->dst;\n"
  "    interlocked = hlsl_ir_interlocked(instr);\n    if (interlocked->op == HLSL_INTERLOCKED_ALLOC || interlocked->op == HLSL_INTERLOCKED_CONSUME\n            || interlocked->coords.node)\n        return NULL;\n    deref = &interlocked->dst;\n"
  "    interlocked = hlsl_ir_interlocked(instr);\n    if (interlocked->op == HLSL_INTERLOCKED_ALLOC || interlocked->op == HLSL_INTERLOCKED_CONSUME)\n        return NULL;\n    deref = &interlocked->dst;\n")
# 存活範圍：計數器運算沒有值運算元。
# 計數器運算即使結果未使用也配置暫存器（與 fxc 相同；imm_atomic_alloc 的 null 目的地 HLSLcc 與 SPIR-V 端都不接受）。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  "            interlocked->value.node->last_read = last_read;\n            if (interlocked->coords.node)\n"
  "            if (interlocked->value.node)\n                interlocked->value.node->last_read = last_read;\n            else if (!instr->last_read)\n                instr->last_read = instr->index;\n            if (interlocked->coords.node)\n"
  "            if (interlocked->value.node)\n                interlocked->value.node->last_read = last_read;\n            if (interlocked->coords.node)\n")
# SPIR-V：計數器指令的目的地為 null 時不寫回（容許其他編譯器產生的 DXBC）。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/spirv.c"
  "    spirv_compiler_emit_store_dst(compiler, dst, result_id);\n}\n\nstatic SpvOp spirv_compiler_map_atomic_instruction"
  "    if (dst->reg.type != VSIR_REGISTER_NULL)\n        spirv_compiler_emit_store_dst(compiler, dst, result_id);\n}\n\nstatic SpvOp spirv_compiler_map_atomic_instruction")
# 使用標記。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  "                interlocked->dst.var->objects_usage[HLSL_REGSET_UAVS][index].uav_atomics = true;\n"
  "                interlocked->dst.var->objects_usage[HLSL_REGSET_UAVS][index].uav_atomics = true;\n                if (interlocked->op == HLSL_INTERLOCKED_ALLOC || interlocked->op == HLSL_INTERLOCKED_CONSUME)\n                    interlocked->dst.var->objects_usage[HLSL_REGSET_UAVS][index].x64arm_counter = true;\n")
# 外部資源帶出計數器使用（三條路徑都要初始化）。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  "\n                extern_resources[*count].bind_count = 1;\n"
  "\n                extern_resources[*count].bind_count = 1;\n                extern_resources[*count].x64arm_counter = var->objects_usage[regset][regset_offset].x64arm_counter;\n")
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  "\n                extern_resources[*count].bind_count = var->bind_count[r];\n"
  "\n                extern_resources[*count].bind_count = var->bind_count[r];\n                extern_resources[*count].x64arm_counter = false;\n                for (unsigned int k = 0; r <= HLSL_REGSET_LAST_OBJECT && k < var->bind_count[r]; ++k)\n                    extern_resources[*count].x64arm_counter |= var->objects_usage[r][k].x64arm_counter;\n")
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  "\n        extern_resources[*count].bind_count = 1;\n"
  "\n        extern_resources[*count].bind_count = 1;\n        extern_resources[*count].x64arm_counter = false;\n")
# UAV 宣告旗標與反射型別。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  "        if (uav && component_type->e.resource.rasteriser_ordered)\n            ins->flags = VKD3DSUF_RASTERISER_ORDERED_VIEW;\n"
  "        if (uav && component_type->e.resource.rasteriser_ordered)\n            ins->flags = VKD3DSUF_RASTERISER_ORDERED_VIEW;\n        if (uav && resource->x64arm_counter)\n            ins->flags |= VKD3DSUF_ORDER_PRESERVING_COUNTER;\n")
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  "            put_u32(buffer, sm4_resource_type(resource->component_type));\n"
  "            put_u32(buffer, resource->x64arm_counter ? D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER : sm4_resource_type(resource->component_type));\n")
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  "            if (var->objects_usage[r][regset_offset].uav_atomics)\n                d->flags |= VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_ATOMICS;\n"
  "            if (var->objects_usage[r][regset_offset].uav_atomics)\n                d->flags |= VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_ATOMICS;\n            if (var->objects_usage[r][regset_offset].x64arm_counter)\n                d->flags |= VKD3D_SHADER_DESCRIPTOR_INFO_FLAG_UAV_COUNTER;\n")
# 產碼：imm_atomic_alloc／imm_atomic_consume dst, u#。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  "    opcode = is_imm ? imm_opcodes[interlocked->op] : opcodes[interlocked->op];\n"
  "    if (interlocked->op == HLSL_INTERLOCKED_ALLOC || interlocked->op == HLSL_INTERLOCKED_CONSUME)\n    {\n        if (!(ins = generate_vsir_add_program_instruction(ctx, program, &instr->loc, interlocked->op == HLSL_INTERLOCKED_ALLOC\n                ? VSIR_OP_IMM_ATOMIC_ALLOC : VSIR_OP_IMM_ATOMIC_CONSUME, 1, 1)))\n            return false;\n        if (is_imm)\n            vsir_dst_from_hlsl_node(&ins->dst[0], ctx, instr);\n        else\n            vsir_dst_operand_init_null(&ins->dst[0]);\n        if (!sm4_generate_vsir_init_src_operand_from_deref(ctx, program, &ins->src[0],\n                &interlocked->dst, VKD3DSP_WRITEMASK_ALL, &instr->loc))\n            return false;\n        ins->src[0].reg.dimension = VSIR_DIMENSION_NONE;\n        return true;\n    }\n\n    opcode = is_imm ? imm_opcodes[interlocked->op] : opcodes[interlocked->op];\n")

# TPF 寫出分派：imm_atomic_alloc／imm_atomic_consume 與 bfrev 都是一般指令格式。
set(X64ARM_VKD3D_TPF "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/tpf.c")
x64arm_vkd3d_patch("${X64ARM_VKD3D_TPF}"
  "        case VSIR_OP_IMM_ATOMIC_XOR:\n        case VSIR_OP_SYNC:\n"
  "        case VSIR_OP_IMM_ATOMIC_XOR:\n        case VSIR_OP_IMM_ATOMIC_ALLOC:\n        case VSIR_OP_IMM_ATOMIC_CONSUME:\n        case VSIR_OP_SYNC:\n")
x64arm_vkd3d_patch("${X64ARM_VKD3D_TPF}"
  "        case VSIR_OP_COUNTBITS:\n"
  "        case VSIR_OP_COUNTBITS:\n        case VSIR_OP_BFREV:\n")

# ---- reversebits()（SM5 bfrev）：新增 HLSL_OP1_REVERSEBITS，含常數摺疊 ----
x64arm_vkd3d_patch("${X64ARM_VKD3D_HLSL_H}"
  "    HLSL_OP1_COUNTBITS,\n"
  "    HLSL_OP1_COUNTBITS,\n    HLSL_OP1_REVERSEBITS, /* x64arm */\n")
x64arm_vkd3d_patch("${X64ARM_VKD3D_HLSL_C}"
  "        [HLSL_OP1_COUNTBITS]    = \"countbits\",\n"
  "        [HLSL_OP1_COUNTBITS]    = \"countbits\",\n        [HLSL_OP1_REVERSEBITS]  = \"reversebits\",\n")
set(X64ARM_VKD3D_CONSTANT_OPS "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_constant_ops.c")
x64arm_vkd3d_patch("${X64ARM_VKD3D_CONSTANT_OPS}"
  "static bool fold_ctz(struct hlsl_ctx *ctx, struct hlsl_constant_value *dst,\n"
  "/* x64arm: reversebits() folding. */\nstatic bool fold_reversebits(struct hlsl_ctx *ctx, struct hlsl_constant_value *dst,\n        const struct hlsl_type *dst_type, const struct hlsl_ir_constant *src)\n{\n    unsigned int k;\n\n    if (dst_type->e.numeric.type != HLSL_TYPE_UINT)\n        return false;\n    for (k = 0; k < dst_type->e.numeric.dimx; ++k)\n    {\n        uint32_t v = src->value.u[k].u, r = 0;\n        unsigned int b;\n\n        for (b = 0; b < 32; ++b)\n            r |= ((v >> b) & 1u) << (31 - b);\n        dst->u[k].u = r;\n    }\n    return true;\n}\n\nstatic bool fold_ctz(struct hlsl_ctx *ctx, struct hlsl_constant_value *dst,\n")
x64arm_vkd3d_patch("${X64ARM_VKD3D_CONSTANT_OPS}"
  "        case HLSL_OP1_COUNTBITS:\n            success = fold_countbits(ctx, &res, instr->data_type, arg1);\n            break;\n"
  "        case HLSL_OP1_COUNTBITS:\n            success = fold_countbits(ctx, &res, instr->data_type, arg1);\n            break;\n\n        case HLSL_OP1_REVERSEBITS:\n            success = fold_reversebits(ctx, &res, instr->data_type, arg1);\n            break;\n")
x64arm_vkd3d_patch("${X64ARM_VKD3D_SM51_CODEGEN}"
  "            generate_vsir_instr_expr_single_instr_op(ctx, program, expr, VSIR_OP_COUNTBITS, 0, 0, true);\n            return true;\n"
  "            generate_vsir_instr_expr_single_instr_op(ctx, program, expr, VSIR_OP_COUNTBITS, 0, 0, true);\n            return true;\n\n        case HLSL_OP1_REVERSEBITS:\n            VKD3D_ASSERT(hlsl_type_is_integer(dst_type));\n            VKD3D_ASSERT(hlsl_version_ge(ctx, 5, 0));\n            generate_vsir_instr_expr_single_instr_op(ctx, program, expr, VSIR_OP_BFREV, 0, 0, true);\n            return true;\n")
foreach(path "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.y" "${vkd3d_parsers_SOURCE_DIR}/hlsl.tab.c")
  x64arm_vkd3d_patch("${path}"
    [=[static bool intrinsic_cross(struct hlsl_ctx *ctx,
        const struct parse_initializer *params, const struct vkd3d_shader_location *loc)
{]=]
    [=[/* x64arm: reversebits() maps to SM5 bfrev. */
static bool intrinsic_reversebits(struct hlsl_ctx *ctx,
        const struct parse_initializer *params, const struct vkd3d_shader_location *loc)
{
    struct hlsl_ir_node *operands[HLSL_MAX_OPERANDS] = {0};
    struct hlsl_type *type;

    if (!elementwise_intrinsic_uint_convert_args(ctx, params, loc))
        return false;
    type = hlsl_change_base_type(ctx, params->args[0]->data_type, HLSL_TYPE_UINT);

    operands[0] = params->args[0];
    return add_expr(ctx, params->instrs, HLSL_OP1_REVERSEBITS, operands, type, loc);
}

static bool intrinsic_cross(struct hlsl_ctx *ctx,
        const struct parse_initializer *params, const struct vkd3d_shader_location *loc)
{]=])
  x64arm_vkd3d_patch("${path}"
    "    {\"refract\",                             3, true,  intrinsic_refract},\n"
    "    {\"refract\",                             3, true,  intrinsic_refract},\n    {\"reversebits\",                         1, true,  intrinsic_reversebits},\n")
endforeach()
