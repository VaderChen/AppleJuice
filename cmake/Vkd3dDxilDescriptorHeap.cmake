# SM6.6 動態資源（ResourceDescriptorHeap／SamplerDescriptorHeap，dx.op.createHandleFromHeap）。
# createHandleFromHeap 只帶堆積索引；型別在 dx.op.annotateHandle 的 ResourceProperties 才確定。每種不同的堆積視圖
# 建立一個宣告，範圍是整個堆積 [0, UINT_MAX]，存取時以堆積索引當陣列索引。每個宣告放在保留的 register space
# （資源 0xFFFF0000+n、取樣器 0xFFFF8000+n），讓介面綁定一對一；執行期把這些 space 綁到目前的描述元堆積鏡像（偏移 0）。
# 堆積宣告逐一配置，handle 保留其指標，不受描述元陣列擴充影響。
# 列舉、前置宣告、annotateHandle 的堆積分支與運算表項目位於 Vkd3dDxilResourceHandles.cmake（同一組修補區塊）。
# 契約參照 DirectXShaderCompiler/docs/DXIL.rst（CreateHandleFromHeap、AnnotateHandle）與 include/dxc/DXIL/DxilResourceProperties.h。
set(X64ARM_VKD3D_DXIL "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c")

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[struct sm6_handle_data
{
    const struct sm6_descriptor_info *d;
    const struct sm6_value *index;
    bool non_uniform;
};]=]
  [=[struct sm6_handle_data
{
    const struct sm6_descriptor_info *d;
    const struct sm6_value *index;
    bool non_uniform;
    /* x64arm: descriptor heap handles have no descriptor until dx.op.annotateHandle. */
    bool heap, sampler_heap;
};]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    enum vsir_data_type resource_data_type;
    unsigned int declaration_flags, byte_stride, byte_count, sample_count;
};]=]
  [=[    enum vsir_data_type resource_data_type;
    unsigned int declaration_flags, byte_stride, byte_count, sample_count;
};

/* x64arm: SM6.6 dynamic resources; see cmake/Vkd3dDxilDescriptorHeap.cmake. */
#define X64ARM_DXIL_RESOURCE_HEAP_SPACE 0xffff0000u
#define X64ARM_DXIL_SAMPLER_HEAP_SPACE 0xffff8000u
#define X64ARM_DXIL_HEAP_ID_BASE 0x10000u
#define X64ARM_DXIL_HEAP_DESCRIPTOR_LIMIT 0x7fefu
struct sm6_heap_descriptor
{
    struct sm6_descriptor_info info;
    struct sm6_heap_descriptor *next;
};]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    struct sm6_descriptor_info *descriptors;
    size_t descriptor_capacity;
    size_t descriptor_count;]=]
  [=[    struct sm6_descriptor_info *descriptors;
    size_t descriptor_capacity;
    size_t descriptor_count;
    struct sm6_heap_descriptor *heap_descriptors;
    unsigned int heap_descriptor_count;]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    if (!sm6_parser_validate_dx_op(sm6, op, name, operands, operand_count, dst))
        return;

    sm6_dx_op_table[op].handler(sm6, op, operands, state);]=]
  [=[    if (!sm6_parser_validate_dx_op(sm6, op, name, operands, operand_count, dst))
        return;
    /* x64arm: a heap handle has no descriptor until dx.op.annotateHandle supplies its resource properties. */
    if (op != DX_ANNOTATE_HANDLE)
    {
        unsigned int k;

        for (k = 0; k < operand_count; ++k)
        {
            if (sm6_value_is_handle(operands[k]) && !operands[k]->u.handle.d)
            {
                vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_RESOURCES,
                        "Descriptor heap handle is used before dx.op.annotateHandle.");
                return;
            }
        }
    }

    sm6_dx_op_table[op].handler(sm6, op, operands, state);]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[static enum vkd3d_result sm6_parser_descriptor_type_init(struct sm6_parser *sm6,]=]
  [=[static void sm6_parser_emit_dx_create_handle_from_heap(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    struct sm6_value *dst = sm6_parser_get_current_value(sm6);

    dst->value_type = VALUE_TYPE_HANDLE;
    memset(&dst->u.handle, 0, sizeof(dst->u.handle));
    dst->u.handle.index = operands[0];
    dst->u.handle.sampler_heap = !!sm6_value_get_constant_uint(operands[1], sm6);
    dst->u.handle.non_uniform = !!sm6_value_get_constant_uint(operands[2], sm6);
    dst->u.handle.heap = true;
}

/* Emit the declaration of a heap view, mirroring the metadata loaders above. */
static bool sm6_parser_declare_heap_descriptor(struct sm6_parser *sm6, const struct sm6_descriptor_info *d)
{
    bool is_uav = d->type == VKD3D_SHADER_DESCRIPTOR_TYPE_UAV;
    struct vkd3d_shader_resource *resource;
    struct vkd3d_shader_instruction *ins;
    struct vsir_operand *reg;
    unsigned int i;

    if (!(ins = sm6_parser_add_instruction(sm6, VSIR_OP_NOP)))
        return false;

    if (d->type == VKD3D_SHADER_DESCRIPTOR_TYPE_CBV)
    {
        vsir_instruction_init(ins, &sm6->p.location, VSIR_OP_DCL_CONSTANT_BUFFER);
        ins->resource_type = VKD3D_SHADER_RESOURCE_BUFFER;
        ins->declaration.cb.size = d->byte_count;
        ins->declaration.cb.src.swizzle = VKD3D_SHADER_NO_SWIZZLE;
        ins->declaration.cb.src.modifiers = VKD3DSPSM_NONE;
        reg = &ins->declaration.cb.src.reg;
        vsir_operand_init(reg, VSIR_REGISTER_CONSTBUFFER, VSIR_DATA_F32, 3);
        reg->idx[0].offset = d->id;
        reg->idx[1].offset = d->range.first;
        reg->idx[2].offset = d->range.last;
        ins->declaration.cb.range = d->range;
        return true;
    }
    if (d->type == VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER)
    {
        vsir_instruction_init(ins, &sm6->p.location, VSIR_OP_DCL_SAMPLER);
        ins->resource_type = VKD3D_SHADER_RESOURCE_NONE;
        ins->flags = d->declaration_flags;
        ins->declaration.sampler.src.swizzle = VKD3D_SHADER_NO_SWIZZLE;
        ins->declaration.sampler.src.modifiers = VKD3DSPSM_NONE;
        reg = &ins->declaration.sampler.src.reg;
        vsir_operand_init(reg, VSIR_REGISTER_SAMPLER, VSIR_DATA_UNUSED, 3);
        reg->idx[0].offset = d->id;
        reg->idx[1].offset = d->range.first;
        reg->idx[2].offset = d->range.last;
        ins->declaration.sampler.range = d->range;
        return true;
    }

    vsir_instruction_init(ins, &sm6->p.location, VSIR_OP_INVALID);
    ins->resource_type = shader_resource_type_from_dxil_resource_kind(d->kind);
    if (is_uav)
        ins->flags = d->declaration_flags;
    if (d->kind == RESOURCE_KIND_RAWBUFFER)
    {
        ins->opcode = is_uav ? VSIR_OP_DCL_UAV_RAW : VSIR_OP_DCL_RESOURCE_RAW;
        ins->declaration.raw_resource.resource.reg.write_mask = 0;
        resource = &ins->declaration.raw_resource.resource;
    }
    else if (d->kind == RESOURCE_KIND_STRUCTUREDBUFFER)
    {
        ins->opcode = is_uav ? VSIR_OP_DCL_UAV_STRUCTURED : VSIR_OP_DCL_RESOURCE_STRUCTURED;
        ins->declaration.structured_resource.byte_stride = d->byte_stride;
        ins->declaration.structured_resource.resource.reg.write_mask = 0;
        resource = &ins->declaration.structured_resource.resource;
    }
    else
    {
        ins->opcode = is_uav ? VSIR_OP_DCL_UAV_TYPED : VSIR_OP_DCL;
        for (i = 0; i < VKD3D_VEC4_SIZE; ++i)
            ins->declaration.semantic.resource_data_type[i] = d->resource_data_type;
        ins->declaration.semantic.resource_type = ins->resource_type;
        ins->declaration.semantic.resource.reg.write_mask = VKD3DSP_WRITEMASK_ALL;
        ins->declaration.semantic.sample_count = d->sample_count;
        resource = &ins->declaration.semantic.resource;
    }
    init_resource_declaration(resource, is_uav ? VSIR_REGISTER_UAV : VSIR_REGISTER_RESOURCE, VSIR_DATA_UNUSED, d->id, &d->range);
    return true;
}

static void sm6_parser_emit_dx_annotate_heap_handle(struct sm6_parser *sm6,
        const struct sm6_value **operands, struct sm6_value *dst)
{
    const unsigned int *properties = operands[1]->u.constant.immconst.immconst_u32;
    const struct sm6_handle_data *source = &operands[0]->u.handle;
    unsigned int kind = properties[0] & 0xff, flags = properties[0] & 0xf000, count;
    struct sm6_heap_descriptor *entry;
    struct sm6_descriptor_info info;

    memset(&info, 0, sizeof(info));
    if (properties[0] & 0xffff0000u)
        goto invalid;
    info.kind = kind;
    if (kind == RESOURCE_KIND_SAMPLER)
    {
        if (!source->sampler_heap || (flags & ~0x8000u) || properties[1])
            goto invalid;
        info.type = VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER;
        info.declaration_flags = (flags & 0x8000u) ? VKD3DSI_SAMPLER_COMPARISON_MODE : 0;
        info.resource_data_type = VSIR_DATA_UNUSED;
    }
    else if (source->sampler_heap)
    {
        goto invalid;
    }
    else if (kind == RESOURCE_KIND_CBUFFER)
    {
        /* The annotation may omit the used size; a CBV view covers at most 64 KiB. */
        if (flags || properties[1] > 65536)
            goto invalid;
        info.type = VKD3D_SHADER_DESCRIPTOR_TYPE_CBV;
        info.byte_count = properties[1] ? (properties[1] + 15u) & ~15u : 65536;
        info.resource_data_type = VSIR_DATA_F32;
    }
    else if (kind >= RESOURCE_KIND_TEXTURE1D && kind <= RESOURCE_KIND_STRUCTUREDBUFFER)
    {
        info.type = (flags & 0x1000u) ? VKD3D_SHADER_DESCRIPTOR_TYPE_UAV : VKD3D_SHADER_DESCRIPTOR_TYPE_SRV;
        if (info.type == VKD3D_SHADER_DESCRIPTOR_TYPE_SRV && flags)
            goto invalid;
        if (flags & 0x2000u)
            info.declaration_flags |= VKD3DSUF_RASTERISER_ORDERED_VIEW;
        if (flags & 0x4000u)
            info.declaration_flags |= VKD3DSUF_GLOBALLY_COHERENT;
        if (flags & 0x8000u)
            info.declaration_flags |= VKD3DSUF_ORDER_PRESERVING_COUNTER;
        if (kind == RESOURCE_KIND_RAWBUFFER)
        {
            if (properties[1])
                goto invalid;
            info.resource_data_type = VSIR_DATA_UNUSED;
        }
        else if (kind == RESOURCE_KIND_STRUCTUREDBUFFER)
        {
            if (!properties[1] || properties[1] % 4u)
                goto invalid;
            info.byte_stride = properties[1];
            info.resource_data_type = VSIR_DATA_UNUSED;
        }
        else
        {
            count = (properties[1] >> 8) & 0xff;
            info.sample_count = (properties[1] >> 16) & 0xff;
            if (!count || count > 4 || (properties[1] >> 24)
                    || (info.sample_count && !resource_kind_is_multisampled(kind)))
                goto invalid;
            info.resource_data_type = vsir_data_type_from_dxil_component_type(properties[1] & 0xff, sm6);
            if (info.resource_data_type == VSIR_DATA_UNUSED)
                goto invalid;
        }
    }
    else
    {
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_RESOURCES,
                "Descriptor heap resource kind %u is unhandled.", kind);
        return;
    }

    for (entry = sm6->heap_descriptors; entry; entry = entry->next)
    {
        const struct sm6_descriptor_info *d = &entry->info;

        if (d->type == info.type && d->kind == info.kind && d->resource_data_type == info.resource_data_type
                && d->declaration_flags == info.declaration_flags && d->byte_stride == info.byte_stride
                && d->byte_count == info.byte_count && d->sample_count == info.sample_count)
            break;
    }
    if (!entry)
    {
        if (sm6->heap_descriptor_count >= X64ARM_DXIL_HEAP_DESCRIPTOR_LIMIT)
        {
            vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_RESOURCES,
                    "Too many distinct descriptor heap views.");
            return;
        }
        if (!(entry = vkd3d_calloc(1, sizeof(*entry))))
        {
            vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_OUT_OF_MEMORY,
                    "Out of memory allocating a descriptor heap view.");
            return;
        }
        entry->info = info;
        entry->info.id = X64ARM_DXIL_HEAP_ID_BASE + sm6->heap_descriptor_count;
        entry->info.range.space = (info.type == VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER
                ? X64ARM_DXIL_SAMPLER_HEAP_SPACE : X64ARM_DXIL_RESOURCE_HEAP_SPACE) + sm6->heap_descriptor_count;
        entry->info.range.first = 0;
        entry->info.range.last = UINT_MAX;
        entry->next = sm6->heap_descriptors;
        sm6->heap_descriptors = entry;
        ++sm6->heap_descriptor_count;
        if (!sm6_parser_declare_heap_descriptor(sm6, &entry->info))
            return;
    }

    dst->value_type = VALUE_TYPE_HANDLE;
    dst->u.handle = *source;
    dst->u.handle.d = &entry->info;
    return;

invalid:
    vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_RESOURCES,
            "Descriptor heap handle properties {%#x, %#x} are invalid.", properties[0], properties[1]);
}

static enum vkd3d_result sm6_parser_descriptor_type_init(struct sm6_parser *sm6,]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    vkd3d_free(sm6->descriptors);
    vkd3d_free(sm6->values);
}]=]
  [=[    while (sm6->heap_descriptors)
    {
        struct sm6_heap_descriptor *next = sm6->heap_descriptors->next;

        vkd3d_free(sm6->heap_descriptors);
        sm6->heap_descriptors = next;
    }
    vkd3d_free(sm6->descriptors);
    vkd3d_free(sm6->values);
}]=])

# 堆積索引的全域旗標只表示著色器使用了 ResourceDescriptorHeap／SamplerDescriptorHeap；宣告與存取已由上方的堆積視圖承接。
set(X64ARM_VKD3D_SPIRV "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/spirv.c")
x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[            | VKD3DSGF_ENABLE_UAVS_AT_EVERY_STAGE
            | VKD3DSGF_ENABLE_RASTERIZER_ORDERED_VIEWS;]=]
  [=[            | VKD3DSGF_ENABLE_UAVS_AT_EVERY_STAGE
            | VKD3DSGF_ENABLE_RASTERIZER_ORDERED_VIEWS
            | VKD3DSGF_ENABLE_RESOURCE_DESCRIPTOR_HEAP_INDEXING
            | VKD3DSGF_ENABLE_SAMPLER_DESCRIPTOR_HEAP_INDEXING;]=])
