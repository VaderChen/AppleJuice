# cbuffer 中的 static 仍是著色器私有變數，不是由宿主上傳的 uniform。
# 固定版前端誤把其初始化值只寫入反射，導致 DXBC 讀取未初始化暫存器。
# 同步修補 grammar 與隨附 parser；沿用共用、可重入的版本核對工具。
foreach(path "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.y" "${vkd3d_parsers_SOURCE_DIR}/hlsl.tab.c")
  x64arm_vkd3d_patch("${path}"
    [=[is_default_values_initializer = (ctx->cur_buffer != ctx->globals_buffer)
                    || (var->storage_modifiers & HLSL_STORAGE_UNIFORM)]=]
    [=[is_default_values_initializer = ((ctx->cur_buffer != ctx->globals_buffer)
                    && !(var->storage_modifiers & HLSL_STORAGE_STATIC))
                    || (var->storage_modifiers & HLSL_STORAGE_UNIFORM)]=])
endforeach()
