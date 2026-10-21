# 固定版 vkd3d 2.1 的 buffer GetDimensions 前端與 SM5 BUFINFO 輸出。
# 原始 grammar 與隨附產生檔同步修補，避免額外依賴本機 bison 版本。
function(x64arm_vkd3d_patch path before after)
  file(READ "${path}" source)
  string(FIND "${source}" "${after}" applied)
  string(FIND "${source}" "${before}" found)
  string(FIND "${before}" "${after}" deletion)
  if(NOT applied EQUAL -1 AND (deletion EQUAL -1 OR found EQUAL -1))
    return()
  endif()
  # 額外參數是明確接受的舊修補版本；逐一完整比對，讓既有依賴快取可升級。
  # 不以任意 marker 跳過失配，也不重置整份依賴來源。
  set(previous_index 3)
  while(found EQUAL -1 AND previous_index LESS ARGC)
    set(previous "${ARGV${previous_index}}")
    string(FIND "${source}" "${previous}" found)
    if(NOT found EQUAL -1)
      set(before "${previous}")
    endif()
    math(EXPR previous_index "${previous_index} + 1")
  endwhile()
  if(found EQUAL -1)
    message(FATAL_ERROR "vkd3d buffer 修補位置不符：${path}")
  endif()
  string(REPLACE "${before}" "${after}" patched "${source}")
  file(WRITE "${path}" "${patched}")
endfunction()
foreach(path "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.y" "${vkd3d_parsers_SOURCE_DIR}/hlsl.tab.c")
  x64arm_vkd3d_patch("${path}"
    [=[    if (object_type->sampler_dim == HLSL_SAMPLER_DIM_STRUCTURED_BUFFER)
    {
        hlsl_fixme(ctx, loc, "Method '%s' for structured buffers.", name);
        return false;
    }

    uint_type = hlsl_get_scalar_type(ctx, HLSL_TYPE_UINT);]=]
    [=[    if (object_type->sampler_dim == HLSL_SAMPLER_DIM_BUFFER ||
            object_type->sampler_dim == HLSL_SAMPLER_DIM_STRUCTURED_BUFFER ||
            object_type->sampler_dim == HLSL_SAMPLER_DIM_RAW_BUFFER)
    {
        const bool structured = object_type->sampler_dim == HLSL_SAMPLER_DIM_STRUCTURED_BUFFER;
        if (params->args_count != (structured ? 2 : 1))
        {
            hlsl_error(ctx, loc, VKD3D_SHADER_ERROR_HLSL_WRONG_PARAMETER_COUNT,
                    "Wrong number of arguments to method '%s': expected %u.", name, structured ? 2 : 1);
            return false;
        }
        for (i = 0; i < params->args_count; ++i)
        {
            if (params->args[i]->data_type->class != HLSL_CLASS_SCALAR)
            {
                hlsl_error(ctx, loc, VKD3D_SHADER_ERROR_HLSL_INVALID_TYPE, "Expected scalar arguments.");
                return false;
            }
        }
        memset(&load_params, 0, sizeof(load_params));
        load_params.type = HLSL_RESOURCE_RESINFO;
        load_params.resource = object;
        load_params.lod = hlsl_block_add_uint_constant(ctx, block, 0, loc);
        load_params.format = hlsl_get_vector_type(ctx, HLSL_TYPE_UINT, 4);
        res_info = hlsl_block_add_resource_load(ctx, block, &load_params, loc);
        if (!add_assignment_from_component(ctx, block, params->args[0], res_info, 0, loc))
            return false;
        if (structured)
        {
            struct hlsl_ir_node *stride = hlsl_block_add_uint_constant(ctx, block,
                    hlsl_type_get_packed_size(object_type->e.resource.format), loc);
            if (!add_assignment(ctx, block, params->args[1], ASSIGN_OP_ASSIGN, stride, false))
                return false;
        }
        return true;
    }

    uint_type = hlsl_get_scalar_type(ctx, HLSL_TYPE_UINT);]=])
  x64arm_vkd3d_patch("${path}"
    [=[{ "GetDimensions",      add_getdimensions_method_call, "00111111111110" }]=]
    [=[{ "GetDimensions",      add_getdimensions_method_call, "00111111111111" }]=])
  x64arm_vkd3d_patch("${path}"
    [=[uav_methods[] =
{]=]
    [=[uav_methods[] =
{
    { "GetDimensions", add_getdimensions_method_call, "00000000000111" },]=])
endforeach()
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/tpf.c"
  "        case VSIR_OP_RCP:\n        case VSIR_OP_RESINFO:"
  "        case VSIR_OP_RCP:\n        case VSIR_OP_BUFINFO:\n        case VSIR_OP_RESINFO:")
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl.h"
  "static inline struct hlsl_type *hlsl_get_scalar_type("
  "unsigned int hlsl_type_get_packed_size(const struct hlsl_type *type);\nstatic inline struct hlsl_type *hlsl_get_scalar_type(")
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c"
  "static unsigned int hlsl_type_get_packed_size(const struct hlsl_type *type)"
  "unsigned int hlsl_type_get_packed_size(const struct hlsl_type *type)")
x64arm_vkd3d_patch("${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c"
  [=[    if (resource->data_type->sampler_dim == HLSL_SAMPLER_DIM_BUFFER
            || resource->data_type->sampler_dim == HLSL_SAMPLER_DIM_STRUCTURED_BUFFER)
    {
        hlsl_fixme(ctx, &load->node.loc, "resinfo for buffers.");
        return false;
    }]=]
  [=[    if (resource->data_type->sampler_dim == HLSL_SAMPLER_DIM_BUFFER
            || resource->data_type->sampler_dim == HLSL_SAMPLER_DIM_STRUCTURED_BUFFER
            || resource->data_type->sampler_dim == HLSL_SAMPLER_DIM_RAW_BUFFER)
    {
        if (program->shader_version.major < 5)
        {
            hlsl_error(ctx, &instr->loc, VKD3D_SHADER_ERROR_HLSL_INVALID_TYPE, "Buffer GetDimensions requires shader model 5.");
            return false;
        }
        if (!(ins = generate_vsir_add_program_instruction(ctx, program, &instr->loc, VSIR_OP_BUFINFO, 1, 1)))
            return false;
        vsir_dst_from_hlsl_node(&ins->dst[0], ctx, instr);
        return sm4_generate_vsir_init_src_operand_from_deref(ctx, program,
                &ins->src[0], resource, ins->dst[0].write_mask, &instr->loc);
    }]=])
