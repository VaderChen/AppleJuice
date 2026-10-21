# 同一個 FX 會連續編譯多個 VS／PS／GS；每個 shader 必須重新配置資源。
# 保留宣告的 reservation，但清除上一個 shader 的配置與使用紀錄。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c"
  [=[    hlsl_ctx_init_entry_function_attributes(ctx);
    parse_entry_function_attributes(ctx, entry_func);]=]
  [=[    {
        struct hlsl_scope *scope;
        struct hlsl_buffer *buffer;
        unsigned int regset;
        LIST_FOR_EACH_ENTRY(scope, &ctx->scopes, struct hlsl_scope, entry)
        {
            LIST_FOR_EACH_ENTRY(var, &scope->vars, struct hlsl_ir_var, scope_entry)
            {
                memset(var->regs, 0, sizeof(var->regs));
                memset(var->bind_count, 0, sizeof(var->bind_count));
                for (regset = 0; regset <= HLSL_REGSET_LAST_OBJECT; ++regset)
                    if (var->objects_usage[regset])
                        memset(var->objects_usage[regset], 0,
                                var->data_type->reg_size[regset] * sizeof(*var->objects_usage[regset]));
            }
        }
        LIST_FOR_EACH_ENTRY(buffer, &ctx->buffers, struct hlsl_buffer, entry)
            memset(&buffer->reg, 0, sizeof(buffer->reg));
    }
    hlsl_ctx_init_entry_function_attributes(ctx);
    parse_entry_function_attributes(ctx, entry_func);]=])
# 已存在的 combined／separated sampler 合成資源也要重新加入本 shader。
# 它們不是使用者 uniform 宣告，不需插入資料複製，但必須參與資源配置。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c"
  [=[    LIST_FOR_EACH_ENTRY(var, &ctx->globals->vars, struct hlsl_ir_var, scope_entry)
    {
        if (var->storage_modifiers & HLSL_STORAGE_UNIFORM)]=]
  [=[    LIST_FOR_EACH_ENTRY(var, &ctx->globals->vars, struct hlsl_ir_var, scope_entry)
    {
        if (var->is_separated_resource || var->is_combined_sampler)
        {
            list_add_tail(&ctx->extern_vars, &var->extern_entry);
        }
        else if (var->storage_modifiers & HLSL_STORAGE_UNIFORM)]=])

# FX 共用 HLSL context，但 SM1–3 的 def/defi literal 表必須每個 shader 重設。
# 否則 find_constant 會沿用上一個 pass 的 register，覆蓋本 pass 的 uniform。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/fx.c"
  [=[    ctx->profile = compile->profile;
    ret = hlsl_emit_vsir]=]
  [=[    ctx->profile = compile->profile;
    ctx->constant_defs.count = 0;
    ret = hlsl_emit_vsir]=])
