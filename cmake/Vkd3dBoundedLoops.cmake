# SM2 無法動態分支時，從每次迭代必定存取的陣列推導合法輸入的迴圈上限。
# 只接受非負常數起點、固定 +1、無其他寫入／內層控制流程的整數索引；保留原條件。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c"
  [=[static bool loop_unrolling_unroll_loop(struct hlsl_ctx *ctx, struct hlsl_block *block,]=]
  [=[static struct hlsl_ir_node *bounded_loop_strip_cast(struct hlsl_ir_node *node)
{
    while (node->type == HLSL_IR_EXPR && hlsl_ir_expr(node)->op == HLSL_OP1_CAST
            && (node->data_type->e.numeric.type == HLSL_TYPE_INT
            || node->data_type->e.numeric.type == HLSL_TYPE_UINT))
        node = hlsl_ir_expr(node)->operands[0].node;
    return node;
}

static unsigned int bounded_loop_array_limit(struct hlsl_ctx *ctx, struct hlsl_ir_loop *loop,
        struct copy_propagation_state *state)
{
    struct hlsl_ir_node *node, *iter_node, *index_node, *value, *left, *right;
    struct hlsl_ir_var *index_var;
    struct copy_propagation_value *initial;
    struct hlsl_ir_store *store;
    struct hlsl_ir_load *load;
    unsigned int bound = 0, start, count, writes, b;
    struct hlsl_block *blocks[2] = {&loop->body, &loop->iter};
    bool increment;

    if (!hlsl_version_lt(ctx, 2, 1) || loop->type != HLSL_LOOP_FOR)
        return 0;
    /* 不從可能被分支略過的陣列讀取推導上限，也不跨內層迴圈推導。 */
    for (b = 0; b < 2; ++b)
        LIST_FOR_EACH_ENTRY(node, &blocks[b]->instrs, struct hlsl_ir_node, entry)
            if (node->type == HLSL_IR_LOOP || node->type == HLSL_IR_SWITCH
                    || node->type == HLSL_IR_JUMP
                    || (node->type == HLSL_IR_IF && !hlsl_ir_if(node)->is_loop_conditional))
                return 0;

    LIST_FOR_EACH_ENTRY(node, &loop->body.instrs, struct hlsl_ir_node, entry)
    {
        if (node->type != HLSL_IR_LOAD)
            continue;
        load = hlsl_ir_load(node);
        if (!load->src.path_len || load->src.var->data_type->class != HLSL_CLASS_ARRAY)
            continue;
        index_node = bounded_loop_strip_cast(load->src.path[0].node);
        if (index_node->type != HLSL_IR_LOAD || hlsl_ir_load(index_node)->src.path_len)
            continue;
        index_var = hlsl_ir_load(index_node)->src.var;
        if (index_var->data_type->class != HLSL_CLASS_SCALAR
                || (index_var->data_type->e.numeric.type != HLSL_TYPE_INT
                && index_var->data_type->e.numeric.type != HLSL_TYPE_UINT))
            continue;
        initial = copy_propagation_get_value(state, index_var, 0, UINT_MAX);
        if (!initial || initial->node->type != HLSL_IR_CONSTANT)
            continue;
        start = hlsl_ir_constant(initial->node)->value.u[0].u;
        count = hlsl_type_element_count(load->src.var->data_type);
        if (start >= count || count > 1024)
            continue;
        writes = 0;
        increment = false;
        for (b = 0; b < 2; ++b)
            LIST_FOR_EACH_ENTRY(iter_node, &blocks[b]->instrs, struct hlsl_ir_node, entry)
            {
                if (iter_node->type != HLSL_IR_STORE)
                    continue;
                store = hlsl_ir_store(iter_node);
                if (store->lhs.var != index_var)
                    continue;
                ++writes;
                value = bounded_loop_strip_cast(store->rhs.node);
                if (b != 1 || value->type != HLSL_IR_EXPR || hlsl_ir_expr(value)->op != HLSL_OP2_ADD)
                    continue;
                left = bounded_loop_strip_cast(hlsl_ir_expr(value)->operands[0].node);
                right = bounded_loop_strip_cast(hlsl_ir_expr(value)->operands[1].node);
                if (left->type == HLSL_IR_CONSTANT)
                {
                    value = left; left = right; right = value;
                }
                increment = left->type == HLSL_IR_LOAD && !hlsl_ir_load(left)->src.path_len
                        && hlsl_ir_load(left)->src.var == index_var && right->type == HLSL_IR_CONSTANT
                        && hlsl_ir_constant(right)->value.u[0].u == 1;
            }
        if (writes == 1 && increment && (!bound || count - start < bound))
            bound = count - start;
    }
    return bound;
}

static bool loop_unrolling_unroll_loop(struct hlsl_ctx *ctx, struct hlsl_block *block,]=])

x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c"
  [=[    state.stopped = false;
    index = loop->node.index;

    loop_unrolling_generate_const_bool_store]=]
  [=[    state.stopped = false;
    index = loop->node.index;
    if (!unroll_limit && !identify_limiter)
    {
        unsigned int array_limit = bounded_loop_array_limit(ctx, loop, &state);
        if (array_limit)
            max_iterations = unroll_limit = array_limit;
    }

    loop_unrolling_generate_const_bool_store]=])
