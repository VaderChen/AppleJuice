# FX2 效果編譯：原生 SM1–3 shader、sampler 狀態與參數引用。
# 以正式 FX2 容器資料表承載，不以來源文字猜測 pass 行為。
# FX2 的 89–92 是逆時針面的 stencil 狀態，不能重複登錄正面名稱。
foreach(pair "StencilFail;89" "StencilZFail;90" "StencilPass;91" "StencilFunc;92")
  list(GET pair 0 state_name)
  list(GET pair 1 state_id)
  if(state_id EQUAL 92)
    set(state_values fx_2_cmpfunc_values)
  else()
    set(state_values fx_2_stencilcaps_values)
  endif()
  # 固定來源的欄位對齊；不修改其他同名的正面狀態。
  if(state_name STREQUAL "StencilZFail")
    set(state_space "              ")
  else()
    set(state_space "               ")
  endif()
  x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
    "{ \"${state_name}\",${state_space}HLSL_CLASS_PASS, HLSL_CLASS_SCALAR, FX_UINT,  1, 1, ${state_id}, ${state_values} }"
    "{ \"CCW_${state_name}\",${state_space}HLSL_CLASS_PASS, HLSL_CLASS_SCALAR, FX_UINT,  1, 1, ${state_id}, ${state_values} }")
endforeach()
# 這個歷史命名的表實際用於 StencilFail／ZFail／Pass；值必須是操作 enum，並非 caps bit。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[static const struct rhs_named_value fx_2_stencilcaps_values[] =
{
    { "KEEP", 0x1 },
    { "ZERO", 0x2 },
    { "REPLACE", 0x4 },
    { "INCRSAT", 0x8 },
    { "DECRSAT", 0x10 },
    { "INVERT", 0x20 },
    { "INCR", 0x40 },
    { "DECR", 0x80 },
    { "TWOSIDED", 0x100 },
    { NULL }
};]=]
  [=[static const struct rhs_named_value fx_2_stencilcaps_values[] =
{
    { "KEEP", 1 },
    { "ZERO", 2 },
    { "REPLACE", 3 },
    { "INCRSAT", 4 },
    { "DECRSAT", 5 },
    { "INVERT", 6 },
    { "INCR", 7 },
    { "DECR", 8 },
    { NULL }
};]=])
# 對應隨附 parser 產物的 grammar 修補；陣列與結構成員使用完整 postfix。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.y"
  [=[state_block stateblock_lhs_identifier state_block_index_opt '=' '<' primary_expr '>' ';']=]
  [=[state_block stateblock_lhs_identifier state_block_index_opt '=' '<' postfix_expr '>' ';']=])
# FX 狀態中的物件型別名稱也可作為列舉值，例如 Texture。
# 同步修補 lexer 與隨附產生檔，保留數值型別建構式解析。
foreach(path "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.l" "${vkd3d_parsers_SOURCE_DIR}/hlsl.yy.c")
  x64arm_vkd3d_patch("${path}"
    [=[                            if (hlsl_get_type(ctx->cur_scope, yytext, true, true))
                                return TYPE_IDENTIFIER;]=]
    [=[                            struct hlsl_type *type = hlsl_get_type(ctx->cur_scope, yytext, true, true);
                            if (type && !(ctx->in_state_block && type->class == HLSL_CLASS_TEXTURE))
                                return TYPE_IDENTIFIER;]=])
endforeach()
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[    if (!replace_context->values)
        return false;
    if (instr->type != HLSL_IR_STATEBLOCK_CONSTANT)
        return false;

    state_constant = hlsl_ir_stateblock_constant(instr);
    if (!get_fx_4_state_enum_value(replace_context->values, state_constant->name, &value))]=]
  [=[    if (instr->type != HLSL_IR_STATEBLOCK_CONSTANT)
        return false;

    state_constant = hlsl_ir_stateblock_constant(instr);
    if (!ascii_strcasecmp(state_constant->name, "null") || !ascii_strcasecmp(state_constant->name, "false"))
        value = 0;
    else if (!ascii_strcasecmp(state_constant->name, "true"))
        value = 1;
    else if (!replace_context->values)
        return false;
    else if (!get_fx_4_state_enum_value(replace_context->values, state_constant->name, &value))]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[hlsl_fixme(ctx, &ctx->location, "Unhandled node type for object-typed field.");]=]
  [=[hlsl_fixme(ctx, &ctx->location, "Unhandled node type %u (class %u) for object-typed field.", node->type, node->data_type->class);]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[    struct vkd3d_bytecode_buffer objects;]=]
  [=[    struct vkd3d_bytecode_buffer objects;
    struct vkd3d_bytecode_buffer fx2_resources;
    uint32_t fx2_resource_count, fx2_technique, fx2_index, fx2_element, fx2_state;]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[static void write_fx_2_technique(struct hlsl_ir_var *var, struct fx_write_context *fx)
{
    uint32_t name_offset, pass_count_offset, annotation_count_offset, count = 0;
    struct vkd3d_bytecode_buffer *buffer = &fx->structured;
    struct hlsl_ir_var *pass;

    name_offset = write_string(var->name, fx);
    put_u32(buffer, name_offset);
    annotation_count_offset = put_u32(buffer, 0);
    pass_count_offset = put_u32(buffer, 0);

    write_fx_2_annotations(var, annotation_count_offset, fx);

    LIST_FOR_EACH_ENTRY(pass, &var->scope->vars, struct hlsl_ir_var, scope_entry)
    {
        write_pass(pass, fx);
        ++count;
    }

    set_u32(buffer, pass_count_offset, count);
}

]=]
  [=[static void write_fx_2_technique(struct hlsl_ir_var *var, struct fx_write_context *fx)
{
    uint32_t name_offset, pass_count_offset, annotation_count_offset, count = 0;
    struct vkd3d_bytecode_buffer *buffer = &fx->structured;
    struct hlsl_ir_var *pass;

    name_offset = write_string(var->name, fx);
    put_u32(buffer, name_offset);
    annotation_count_offset = put_u32(buffer, 0);
    pass_count_offset = put_u32(buffer, 0);

    write_fx_2_annotations(var, annotation_count_offset, fx);

    LIST_FOR_EACH_ENTRY(pass, &var->scope->vars, struct hlsl_ir_var, scope_entry)
    {
        fx->fx2_index = count;
        write_pass(pass, fx);
        ++count;
    }

    set_u32(buffer, pass_count_offset, count);
}

]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[    count = write_state_block(var, 0, fx);
    set_u32(buffer, count_offset, count);]=]
  [=[    fx->fx2_technique = fx->technique_count;
    fx->fx2_element = 0;
    fx->fx2_state = 0;
    count = write_state_block(var, 0, fx);
    set_u32(buffer, count_offset, count);]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[    /* TODO: only handles numeric types */

    offset = put_u32(buffer, hlsl_sm1_base_type(data_type, false, HLSL_SAMPLER_DIM_GENERIC));
    put_u32(buffer, D3DXPC_MATRIX_ROWS);]=]
  [=[    bool object = data_type->class == HLSL_CLASS_TEXTURE || data_type->class == HLSL_CLASS_SAMPLER
            || data_type->class == HLSL_CLASS_VERTEX_SHADER || data_type->class == HLSL_CLASS_PIXEL_SHADER;

    offset = put_u32(buffer, hlsl_sm1_base_type(data_type, false, HLSL_SAMPLER_DIM_GENERIC));
    put_u32(buffer, object ? D3DXPC_OBJECT : D3DXPC_MATRIX_ROWS);]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[    put_u32(buffer, entry->args_count); /* Rows */
    put_u32(buffer, 1); /* Columns */]=]
  [=[    if (!object)
    {
        put_u32(buffer, entry->args_count); /* Rows */
        put_u32(buffer, 1); /* Columns */
    }]=])
# 保留最新 FX2 目標，同時完整辨識上一版已寫入快取的 state assignment。
# 舊版內容由同一目標移除本次新增的 selector 部分產生，不複製整個函式。
set(X64ARM_FX2_STATE_ASSIGNMENT [=[static bool fx2_write_scalar_selector(struct vkd3d_bytecode_buffer *buffer, struct hlsl_ir_node *node)
{
    struct vkd3d_bytecode_buffer ctab = {0};
    static const uint32_t pad;
    while (node->type == HLSL_IR_EXPR && hlsl_ir_expr(node)->op == HLSL_OP1_CAST)
        node = hlsl_ir_expr(node)->operands[0].node;
    if (node->type != HLSL_IR_LOAD || hlsl_ir_load(node)->src.path_len
            || node->data_type->class != HLSL_CLASS_SCALAR)
        return false;
    const char *name = hlsl_ir_load(node)->src.var->name;
    /* Standard FX2 preshader: named CTAB scalar converted to float c0.x,
     * then MOV to integer output oi0.x. No private container extension. */
    put_u32(&ctab, 28); put_u32(&ctab, 64); put_u32(&ctab, 0xfffe0200);
    put_u32(&ctab, 1); put_u32(&ctab, 28); put_u32(&ctab, 0); put_u32(&ctab, 0);
    put_u32(&ctab, 64); put_u32(&ctab, 2); put_u32(&ctab, 1);
    put_u32(&ctab, 48); put_u32(&ctab, 0);
    put_u32(&ctab, 3 << 16); put_u32(&ctab, 0x10001); put_u32(&ctab, 1); put_u32(&ctab, 0);
    bytecode_put_bytes(&ctab, name, strlen(name) + 1);
    if (ctab.size % 4) bytecode_put_bytes_unaligned(&ctab, &pad, 4 - ctab.size % 4);
    put_u32(buffer, TAG_FX20);
    put_u32(buffer, ((ctab.size / 4 + 1) << 16) | 0xfffe);
    put_u32(buffer, TAG_CTAB); bytecode_put_bytes(buffer, ctab.data, ctab.size);
    put_u32(buffer, (10 << 16) | 0xfffe); put_u32(buffer, TAG_FXLC);
    put_u32(buffer, 1); put_u32(buffer, 0x10000001); put_u32(buffer, 1);
    put_u32(buffer, 0); put_u32(buffer, 2); put_u32(buffer, 0);
    put_u32(buffer, 0); put_u32(buffer, 6); put_u32(buffer, 0);
    put_u32(buffer, 0xffff);
    vkd3d_free(ctab.data);
    return true;
}

static void write_fx_2_state_assignment(const struct hlsl_ir_var *var,
        struct hlsl_state_block_entry *entry, struct fx_write_context *fx)
{
    struct vkd3d_bytecode_buffer *unstructured = &fx->unstructured;
    struct vkd3d_bytecode_buffer *buffer = &fx->structured;
    uint32_t value = 0, type = 0, rhs_offset, type_offset;
    struct hlsl_ir_node *value_node = entry->args->node;
    struct hlsl_ctx *ctx = fx->ctx;
    struct hlsl_ir_constant *c;

    put_u32(buffer, entry->name_id);
    put_u32(buffer, entry->lhs_index);
    type_offset = put_u32(buffer, 0);
    rhs_offset = put_u32(buffer, 0);

    unsigned int state_index = fx->fx2_state++;
    struct hlsl_ir_node *reference = value_node, *reference_indices[64];
    unsigned int reference_count = 0;
    for (;;)
    {
        if (reference->type == HLSL_IR_EXPR && hlsl_ir_expr(reference)->op == HLSL_OP1_CAST)
            reference = hlsl_ir_expr(reference)->operands[0].node;
        else if (reference->type == HLSL_IR_SWIZZLE)
        {
            struct hlsl_ir_swizzle *swizzle = hlsl_ir_swizzle(reference);
            bool identity = true;
            for (unsigned int i = 0; i < reference->data_type->e.numeric.dimx; ++i)
                identity &= hlsl_swizzle_get_component(swizzle->u.vector, i) == i;
            if (!identity)
                break;
            reference = swizzle->val.node;
        }
        else if (reference->type == HLSL_IR_INDEX && reference_count < 64)
        {
            struct hlsl_ir_index *index = hlsl_ir_index(reference);
            reference_indices[reference_count++] = index->idx.node;
            reference = index->val.node;
        }
        else
            break;
    }
    if (reference->type == HLSL_IR_LOAD)
        value_node = reference;

    switch (value_node->type)
    {
        case HLSL_IR_CONSTANT:
        {
            type = write_fx_2_state_type(entry, fx);
            value = unstructured->size;

            for (int i = 0; i < entry->args_count; ++i)
            {
                c = hlsl_ir_constant(entry->args[i].node);
                put_u32(unstructured, c->value.u[0].u);
            }
            break;
        }
        case HLSL_IR_COMPILE:
        {
            struct vkd3d_shader_code code = {0};
            uint32_t id = fx->object_variable_count++;
            type = write_fx_2_state_type(entry, fx);
            value = put_u32(unstructured, id);
            if (compile_shader(hlsl_ir_compile(value_node), fx, &code))
            {
                static const uint32_t pad;
                put_u32(&fx->objects, id);
                put_u32(&fx->objects, code.size);
                bytecode_put_bytes(&fx->objects, code.code, code.size);
                if (code.size % 4)
                    bytecode_put_bytes_unaligned(&fx->objects, &pad, 4 - code.size % 4);
            }
            else
                hlsl_error(ctx, &var->loc, VKD3D_SHADER_ERROR_HLSL_INVALID_SYNTAX,
                        "FX2 shader compilation failed for %s.", hlsl_ir_compile(value_node)->profile->name);
            vkd3d_shader_free_shader_code(&code);
            break;
        }
        case HLSL_IR_LOAD:
        {
            struct hlsl_ir_load *load = hlsl_ir_load(value_node);
            struct vkd3d_string_buffer qualified;
            struct hlsl_type *ref_type = load->src.var->data_type;
            static const uint32_t pad;
            bool valid = true;
            struct hlsl_ir_node *selector = NULL;
            vkd3d_string_buffer_init(&qualified);
            vkd3d_string_buffer_printf(&qualified, "%s", load->src.var->name);
            for (unsigned int i = 0; i < load->src.path_len + reference_count; ++i)
            {
                struct hlsl_ir_node *node = i < load->src.path_len ? load->src.path[i].node
                        : reference_indices[reference_count - 1 - (i - load->src.path_len)];
                if (node->type != HLSL_IR_CONSTANT)
                {
                    if (ref_type->class == HLSL_CLASS_ARRAY
                            && i + 1 == load->src.path_len + reference_count)
                    {
                        selector = node;
                        ref_type = ref_type->e.array.type;
                        break;
                    }
                    valid = false;
                    break;
                }
                unsigned int index = hlsl_ir_constant(node)->value.u[0].u;
                if (ref_type->class == HLSL_CLASS_ARRAY && index < ref_type->e.array.elements_count)
                {
                    vkd3d_string_buffer_printf(&qualified, "[%u]", index);
                    ref_type = ref_type->e.array.type;
                }
                else if (ref_type->class == HLSL_CLASS_STRUCT && index < ref_type->e.record.field_count)
                {
                    vkd3d_string_buffer_printf(&qualified, ".%s", ref_type->e.record.fields[index].name);
                    ref_type = ref_type->e.record.fields[index].type;
                }
                else
                {
                    valid = false;
                    break;
                }
            }
            if (!valid)
            {
                hlsl_fixme(ctx, &var->loc, "Dynamic or component FX2 state reference is not supported.");
                vkd3d_string_buffer_cleanup(&qualified);
                break;
            }
            const char *name = qualified.buffer;
            uint32_t size = strlen(name) + 1;
            type = write_fx_2_state_type(entry, fx);
            value = put_u32(unstructured, 0);
            put_u32(&fx->fx2_resources, fx->fx2_technique);
            put_u32(&fx->fx2_resources, fx->fx2_index);
            put_u32(&fx->fx2_resources, fx->fx2_element);
            put_u32(&fx->fx2_resources, state_index);
            if (selector)
            {
                struct vkd3d_bytecode_buffer expression = {0};
                if (!fx2_write_scalar_selector(&expression, selector))
                    hlsl_fixme(ctx, &var->loc, "FX2 array selector expression is not supported.");
                uint32_t aligned = (size + 3) & ~3u;
                put_u32(&fx->fx2_resources, FX_2_ASSIGNMENT_ARRAY_SELECTOR);
                put_u32(&fx->fx2_resources, 4 + aligned + expression.size);
                put_u32(&fx->fx2_resources, aligned);
                bytecode_put_bytes(&fx->fx2_resources, name, size);
                if (size % 4) bytecode_put_bytes_unaligned(&fx->fx2_resources, &pad, 4 - size % 4);
                bytecode_put_bytes(&fx->fx2_resources, expression.data, expression.size);
                vkd3d_free(expression.data);
            }
            else
            {
                put_u32(&fx->fx2_resources, FX_2_ASSIGNMENT_PARAMETER);
                put_u32(&fx->fx2_resources, size);
                bytecode_put_bytes(&fx->fx2_resources, name, size);
                if (size % 4) bytecode_put_bytes_unaligned(&fx->fx2_resources, &pad, 4 - size % 4);
            }
            ++fx->fx2_resource_count;
            vkd3d_string_buffer_cleanup(&qualified);
            break;
        }
        default:
            hlsl_fixme(ctx, &var->loc, "Unsupported assignment type for state %s.", entry->name);
    }

    set_u32(buffer, type_offset, type);
    set_u32(buffer, rhs_offset, value);
}
]=])
string(FIND "${X64ARM_FX2_STATE_ASSIGNMENT}" "static void write_fx_2_state_assignment" X64ARM_FX2_ASSIGNMENT_START)
string(SUBSTRING "${X64ARM_FX2_STATE_ASSIGNMENT}" ${X64ARM_FX2_ASSIGNMENT_START} -1 X64ARM_FX2_PREVIOUS_ASSIGNMENT)
string(REPLACE [=[            struct hlsl_ir_node *selector = NULL;
]=] [=[]=]
  X64ARM_FX2_PREVIOUS_ASSIGNMENT "${X64ARM_FX2_PREVIOUS_ASSIGNMENT}")
string(REPLACE [=[                    if (ref_type->class == HLSL_CLASS_ARRAY
                            && i + 1 == load->src.path_len + reference_count)
                    {
                        selector = node;
                        ref_type = ref_type->e.array.type;
                        break;
                    }
]=] [=[]=]
  X64ARM_FX2_PREVIOUS_ASSIGNMENT "${X64ARM_FX2_PREVIOUS_ASSIGNMENT}")
string(REPLACE [=[            if (selector)
            {
                struct vkd3d_bytecode_buffer expression = {0};
                if (!fx2_write_scalar_selector(&expression, selector))
                    hlsl_fixme(ctx, &var->loc, "FX2 array selector expression is not supported.");
                uint32_t aligned = (size + 3) & ~3u;
                put_u32(&fx->fx2_resources, FX_2_ASSIGNMENT_ARRAY_SELECTOR);
                put_u32(&fx->fx2_resources, 4 + aligned + expression.size);
                put_u32(&fx->fx2_resources, aligned);
                bytecode_put_bytes(&fx->fx2_resources, name, size);
                if (size % 4) bytecode_put_bytes_unaligned(&fx->fx2_resources, &pad, 4 - size % 4);
                bytecode_put_bytes(&fx->fx2_resources, expression.data, expression.size);
                vkd3d_free(expression.data);
            }
            else
            {
                put_u32(&fx->fx2_resources, FX_2_ASSIGNMENT_PARAMETER);
                put_u32(&fx->fx2_resources, size);
                bytecode_put_bytes(&fx->fx2_resources, name, size);
                if (size % 4) bytecode_put_bytes_unaligned(&fx->fx2_resources, &pad, 4 - size % 4);
            }
]=] [=[            put_u32(&fx->fx2_resources, FX_2_ASSIGNMENT_PARAMETER);
            put_u32(&fx->fx2_resources, size);
            bytecode_put_bytes(&fx->fx2_resources, name, size);
            if (size % 4)
                bytecode_put_bytes_unaligned(&fx->fx2_resources, &pad, 4 - size % 4);
]=]
  X64ARM_FX2_PREVIOUS_ASSIGNMENT "${X64ARM_FX2_PREVIOUS_ASSIGNMENT}")
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[static void write_fx_2_state_assignment(const struct hlsl_ir_var *var,
        struct hlsl_state_block_entry *entry, struct fx_write_context *fx)
{
    struct vkd3d_bytecode_buffer *unstructured = &fx->unstructured;
    struct vkd3d_bytecode_buffer *buffer = &fx->structured;
    uint32_t value = 0, type = 0, rhs_offset, type_offset;
    struct hlsl_ir_node *value_node = entry->args->node;
    struct hlsl_ctx *ctx = fx->ctx;
    struct hlsl_ir_constant *c;

    put_u32(buffer, entry->name_id);
    put_u32(buffer, entry->lhs_index);
    type_offset = put_u32(buffer, 0);
    rhs_offset = put_u32(buffer, 0);

    switch (value_node->type)
    {
        case HLSL_IR_CONSTANT:
        {
            type = write_fx_2_state_type(entry, fx);
            value = unstructured->size;

            for (int i = 0; i < entry->args_count; ++i)
            {
                c = hlsl_ir_constant(entry->args[i].node);
                put_u32(unstructured, c->value.u[0].u);
            }
            break;
        }
        default:
            hlsl_fixme(ctx, &var->loc, "Unsupported assignment type for state %s.", entry->name);
    }

    set_u32(buffer, type_offset, type);
    set_u32(buffer, rhs_offset, value);
}
]=]
  "${X64ARM_FX2_STATE_ASSIGNMENT}" "${X64ARM_FX2_PREVIOUS_ASSIGNMENT}")

x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[            hlsl_fixme(ctx, &var->loc, "Writing fx_2_0 sampler objects initializers is not implemented.");]=]
  [=[            struct vkd3d_bytecode_buffer saved = fx->structured, states = {0};
            uint32_t count_offset, count;
            uint32_t saved_technique = fx->fx2_technique, saved_index = fx->fx2_index;
            uint32_t saved_element = fx->fx2_element, saved_state = fx->fx2_state;
            fx->structured = states;
            for (unsigned int element = 0; element < elements_count; ++element)
            {
                fx->fx2_technique = ~0u;
                fx->fx2_index = fx->parameter_count;
                fx->fx2_element = element;
                fx->fx2_state = 0;
                count_offset = put_u32(&fx->structured, 0);
                count = write_state_block((struct hlsl_ir_var *)var, element, fx);
                set_u32(&fx->structured, count_offset, count);
            }
            states = fx->structured;
            fx->structured = saved;
            offset = fx->unstructured.size;
            bytecode_put_bytes(&fx->unstructured, states.data, states.size);
            set_status(fx, states.status);
            vkd3d_free(states.data);
            fx->fx2_technique = saved_technique;
            fx->fx2_index = saved_index;
            fx->fx2_element = saved_element;
            fx->fx2_state = saved_state;
            return offset;]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[    put_u32(structured, 0); /* Resource count */

    bytecode_put_bytes(structured, fx.objects.data, fx.objects.size);
    /* TODO: resources */]=]
  [=[    put_u32(structured, fx.fx2_resource_count);

    bytecode_put_bytes(structured, fx.objects.data, fx.objects.size);
    bytecode_put_bytes(structured, fx.fx2_resources.data, fx.fx2_resources.size);
    set_status(&fx, fx.fx2_resources.status);
    vkd3d_free(fx.fx2_resources.data);]=])
