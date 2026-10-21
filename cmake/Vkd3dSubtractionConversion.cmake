# 二元減法先依共同型別轉換 RHS，再取負值；否則 float - uint
# 會先在 uint 發生環繞。一般減法與 -= 使用同一個轉換流程。
foreach(path "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.y" "${vkd3d_parsers_SOURCE_DIR}/hlsl.tab.c")
  x64arm_vkd3d_patch("${path}"
    [=[static struct hlsl_ir_node *add_binary_bitwise_expr(]=]
    [=[static struct hlsl_ir_node *x64arm_subtract_rhs(struct hlsl_ctx *ctx, struct hlsl_block *block,
        struct hlsl_ir_node *lhs, struct hlsl_ir_node *rhs, const struct vkd3d_shader_location *loc)
{
    struct hlsl_type *common;
    if (lhs->data_type->class == HLSL_CLASS_ERROR || rhs->data_type->class == HLSL_CLASS_ERROR)
        return ctx->error_instr;
    if (!(common = get_common_numeric_type(ctx, lhs, rhs, loc)))
        return ctx->error_instr;
    if (!(rhs = add_implicit_conversion(ctx, block, rhs, common, loc)))
        return NULL;
    return add_unary_arithmetic_expr(ctx, block, HLSL_OP1_NEG, rhs, loc);
}

static struct hlsl_ir_node *add_binary_bitwise_expr(]=])
  x64arm_vkd3d_patch("${path}"
    [=[rhs = add_unary_arithmetic_expr(ctx, block, HLSL_OP1_NEG, rhs, &rhs->loc)]=]
    [=[rhs = x64arm_subtract_rhs(ctx, block, lhs, rhs, &rhs->loc)]=])
endforeach()
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.y"
  [=[neg = add_unary_arithmetic_expr(ctx, $3, HLSL_OP1_NEG, node_from_block($3), &@2)]=]
  [=[neg = x64arm_subtract_rhs(ctx, $3, node_from_block($1), node_from_block($3), &@2)]=])
x64arm_vkd3d_patch("${vkd3d_parsers_SOURCE_DIR}/hlsl.tab.c"
  [=[neg = add_unary_arithmetic_expr(ctx, (yyvsp[0].block), HLSL_OP1_NEG, node_from_block((yyvsp[0].block)), &(yylsp[-1]))]=]
  [=[neg = x64arm_subtract_rhs(ctx, (yyvsp[0].block), node_from_block((yyvsp[-2].block)), node_from_block((yyvsp[0].block)), &(yylsp[-1]))]=])
