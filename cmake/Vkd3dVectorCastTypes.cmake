# 向量化只能合併結果純量型別相同的運算；CAST opcode 相同不代表轉換相同。
# 例如同一 uint 的 float 與 int 轉換不可合成一條 utof。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c"
  [=[static bool is_same_vectorizable_expr(struct hlsl_ir_expr *a, struct hlsl_ir_expr *b)
{
    if (a->op != b->op)]=]
  [=[static bool is_same_vectorizable_expr(struct hlsl_ir_expr *a, struct hlsl_ir_expr *b)
{
    if (a->op != b->op || a->node.data_type->e.numeric.type != b->node.data_type->e.numeric.type)]=])
