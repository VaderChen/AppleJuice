# 僅引擎展開動態 interface 時啟用額外的內部常數槽；公開 HLSL 仍限 cb0–cb13。
# 私有選項不由 D3DCompile Flags 對外暴露，且額外資料仍由 Metal 真正讀取。
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/vkd3d_shader_private.h"
  [=[    uint32_t child_effect : 1;]=]
  [=[    uint32_t child_effect : 1;
    uint32_t x64arm_dynamic_linkage : 1;]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c"
  [=[        option = &vkd3d->options[i];

        switch (option->name)]=]
  [=[        option = &vkd3d->options[i];
        if ((unsigned int)option->name == 0x58434c53u)
        {
            vsir->x64arm_dynamic_linkage = !!option->value;
            continue;
        }

        switch (option->name)]=])
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c"
  [=[static unsigned int get_max_cbuffer_reg_index(struct hlsl_ctx *ctx)
{
    if (hlsl_version_ge(ctx, 5, 1))
        return UINT_MAX;

    return 13;
}]=]
  [=[static unsigned int get_max_cbuffer_reg_index(struct hlsl_ctx *ctx)
{
    if (hlsl_version_ge(ctx, 5, 1))
        return UINT_MAX;

    return ctx->compile_info.x64arm_dynamic_linkage ? 14 : 13;
}]=])
