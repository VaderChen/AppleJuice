# tbuffer 保留 FX／RDEF 契約，以真正的 typed-buffer SRV 與 LD 執行。
# 常數緩衝區的相對索引已是 float4 單位，保留混合型別的原始位元及分量選擇。
set(source "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/hlsl_codegen.c")
x64arm_vkd3d_patch("${source}"
  [=[            FIXME("Allocate registers for texture buffers.\n");]=]
  [=[            /* tbuffer 在一般紋理完成配置後共用 t 暫存器命名空間。 */]=])
x64arm_vkd3d_patch("${source}"
  [=[static bool sm4_generate_vsir_instr_load(struct hlsl_ctx *ctx, struct vsir_program *program, struct hlsl_ir_load *load)]=]
  [=[static bool x64arm_tbuffer_load(struct hlsl_ctx *ctx, struct vsir_program *program, struct hlsl_ir_load *load)
{
    struct hlsl_ir_node *node = &load->node;
    const struct hlsl_buffer *buffer = load->src.var->buffer;
    struct vsir_src_operand original, coordinate, loaded;
    struct vsir_dst_operand destination;
    struct vkd3d_shader_instruction *ins;
    struct hlsl_constant_value value = {0};
    unsigned int temporary = ctx->temp_count - 2;
    bool boolean = node->data_type->e.numeric.type == HLSL_TYPE_BOOL;

    vsir_dst_from_hlsl_node(&destination, ctx, node);
    if (!sm4_generate_vsir_init_src_operand_from_deref(ctx, program, &original,
            &load->src, destination.write_mask, &node->loc))
        return false;
    value.u[0].u = original.reg.idx[2].offset;
    vsir_src_from_hlsl_constant_value(&coordinate, ctx, &value, VSIR_DATA_U32, 1, VKD3DSP_WRITEMASK_0);
    if (original.reg.idx[2].rel_addr)
    {
        if (!(ins = generate_vsir_add_program_instruction(ctx, program, &node->loc, VSIR_OP_IADD, 1, 2)))
            return false;
        vsir_dst_operand_init(&ins->dst[0], VSIR_REGISTER_TEMP, VSIR_DATA_U32, 1);
        ins->dst[0].reg.idx[0].offset = temporary;
        ins->dst[0].write_mask = VKD3DSP_WRITEMASK_0;
        ins->src[0] = *original.reg.idx[2].rel_addr;
        ins->src[1] = coordinate;
        vsir_src_operand_init(&coordinate, VSIR_REGISTER_TEMP, VSIR_DATA_U32, 1);
        coordinate.reg.idx[0].offset = temporary;
        coordinate.swizzle = 0;
    }
    if (!(ins = generate_vsir_add_program_instruction(ctx, program, &node->loc, VSIR_OP_LD, 1, 2)))
        return false;
    vsir_dst_operand_init(&ins->dst[0], VSIR_REGISTER_TEMP, VSIR_DATA_F32, 1);
    ins->dst[0].reg.idx[0].offset = temporary + 1;
    ins->dst[0].write_mask = VKD3DSP_WRITEMASK_ALL;
    ins->src[0] = coordinate;
    vsir_src_operand_init(&ins->src[1], VSIR_REGISTER_RESOURCE, VSIR_DATA_F32, 2);
    ins->src[1].reg.idx[0].offset = buffer->reg.id;
    ins->src[1].reg.idx[1].offset = buffer->reg.index;

    vsir_src_operand_init(&loaded, VSIR_REGISTER_TEMP, original.reg.data_type, 1);
    loaded.reg.idx[0].offset = temporary + 1;
    loaded.swizzle = original.swizzle;
    if (!(ins = generate_vsir_add_program_instruction(ctx, program, &node->loc,
            boolean ? VSIR_OP_MOVC : VSIR_OP_MOV, 1, boolean ? 3 : 1)))
        return false;
    ins->dst[0] = destination;
    ins->src[0] = loaded;
    if (boolean)
    {
        memset(&value, 0xff, sizeof(value));
        vsir_src_from_hlsl_constant_value(&ins->src[1], ctx, &value,
                VSIR_DATA_U32, node->data_type->e.numeric.dimx, destination.write_mask);
        memset(&value, 0, sizeof(value));
        vsir_src_from_hlsl_constant_value(&ins->src[2], ctx, &value,
                VSIR_DATA_U32, node->data_type->e.numeric.dimx, destination.write_mask);
    }
    return true;
}

static bool sm4_generate_vsir_instr_load(struct hlsl_ctx *ctx, struct vsir_program *program, struct hlsl_ir_load *load)]=])
x64arm_vkd3d_patch("${source}"
  [=[    VKD3D_ASSERT(!load->src.var->is_tgsm);
    VKD3D_ASSERT(hlsl_is_numeric_type(type));]=]
  [=[    VKD3D_ASSERT(!load->src.var->is_tgsm);
    VKD3D_ASSERT(hlsl_is_numeric_type(type));
    if (load->src.var->is_uniform && load->src.var->buffer
            && load->src.var->buffer->type != HLSL_BUFFER_CONSTANT)
        return x64arm_tbuffer_load(ctx, program, load);]=])
x64arm_vkd3d_patch("${source}"
  [=[    program->temp_count = max(program->temp_count, ctx->temp_count);

    LIST_FOR_EACH_ENTRY(var, semantic_vars, struct hlsl_ir_var, extern_entry)]=]
  [=[    {
        struct hlsl_buffer *buffer;
        LIST_FOR_EACH_ENTRY(buffer, &ctx->buffers, struct hlsl_buffer, entry)
        {
            if (buffer->reg.allocated && buffer->type != HLSL_BUFFER_CONSTANT)
            {
                ctx->temp_count += 2;
                break;
            }
        }
    }
    program->temp_count = max(program->temp_count, ctx->temp_count);

    LIST_FOR_EACH_ENTRY(var, semantic_vars, struct hlsl_ir_var, extern_entry)]=])
x64arm_vkd3d_patch("${source}"
  [=[        extern_resources[*count].component_type = NULL;

        extern_resources[*count].regset = HLSL_REGSET_NUMERIC;]=]
  [=[        extern_resources[*count].component_type = buffer->type == HLSL_BUFFER_CONSTANT ? NULL
                : hlsl_new_texture_type(ctx, HLSL_SAMPLER_DIM_BUFFER,
                        hlsl_get_vector_type(ctx, HLSL_TYPE_FLOAT, 4), 0);

        extern_resources[*count].regset = buffer->type == HLSL_BUFFER_CONSTANT
                ? HLSL_REGSET_NUMERIC : HLSL_REGSET_TEXTURES;]=])
x64arm_vkd3d_patch("${source}"
  [=[        if (cbuffer->reg.allocated)
            sm4_generate_vsir_add_dcl_constant_buffer(ctx, program, cbuffer);]=]
  [=[        if (cbuffer->reg.allocated && cbuffer->type == HLSL_BUFFER_CONSTANT)
            sm4_generate_vsir_add_dcl_constant_buffer(ctx, program, cbuffer);]=])
x64arm_vkd3d_patch("${source}"
  [=[        allocate_objects(ctx, &semantic_vars, HLSL_REGSET_SAMPLERS);
        allocate_tgsms(ctx);]=]
  [=[        allocate_objects(ctx, &semantic_vars, HLSL_REGSET_SAMPLERS);
        {
            struct hlsl_buffer *buffer, *other;
            struct hlsl_ir_var *texture;
            LIST_FOR_EACH_ENTRY(buffer, &ctx->buffers, struct hlsl_buffer, entry)
            {
                uint32_t slot;
                bool occupied;
                if (!buffer->used_size || buffer->type == HLSL_BUFFER_CONSTANT)
                    continue;
                if (buffer->reservation.reg_type && buffer->reservation.reg_type != 't')
                {
                    hlsl_error(ctx, &buffer->loc, VKD3D_SHADER_ERROR_HLSL_INVALID_RESERVATION,
                            "Texture buffers require register type 't'.");
                    continue;
                }
                slot = buffer->reservation.reg_type ? buffer->reservation.reg_index : 0;
                do
                {
                    occupied = false;
                    LIST_FOR_EACH_ENTRY(texture, &ctx->extern_vars, struct hlsl_ir_var, extern_entry)
                    {
                        const struct hlsl_reg *reg = &texture->regs[HLSL_REGSET_TEXTURES];
                        if (reg->allocated && reg->space == buffer->reservation.reg_space
                                && slot >= reg->index && slot - reg->index < reg->allocation_size)
                            occupied = true;
                    }
                    LIST_FOR_EACH_ENTRY(other, &ctx->buffers, struct hlsl_buffer, entry)
                    {
                        if (other == buffer || other->type == HLSL_BUFFER_CONSTANT || !other->used_size)
                            continue;
                        if ((other->reg.allocated && other->reg.space == buffer->reservation.reg_space
                                && other->reg.index == slot)
                                || (other->reservation.reg_type == 't'
                                && other->reservation.reg_space == buffer->reservation.reg_space
                                && other->reservation.reg_index == slot))
                            occupied = true;
                    }
                    if (occupied && !buffer->reservation.reg_type)
                        ++slot;
                    else
                        break;
                } while (slot < 128);
                if (occupied || slot >= 128)
                {
                    hlsl_error(ctx, &buffer->loc, VKD3D_SHADER_ERROR_HLSL_INVALID_RESERVATION,
                            "Texture buffer register overlaps another resource or exceeds the limit.");
                    continue;
                }
                buffer->reg.space = buffer->reservation.reg_space;
                buffer->reg.index = buffer->reg.id = slot;
                buffer->reg.allocation_size = 1;
                buffer->reg.allocated = true;
            }
        }
        allocate_tgsms(ctx);]=])
