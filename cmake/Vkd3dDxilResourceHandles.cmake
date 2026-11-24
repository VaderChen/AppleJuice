# SM6.6 的資源 handle 共用既有 DXIL 描述元與 VSIR 讀寫路徑；CreateHandleFromHeap 的堆積視圖見 Vkd3dDxilDescriptorHeap.cmake。
# ResBind 使用 register space／完整範圍選取資源，索引與 non-uniform 屬性保留；
# ResourceProperties 與 metadata 交叉驗證後傳遞同一 handle，既有宣告承接 UAV 一致性、counter 與 ROV。
# 契約參照 DirectXShaderCompiler/docs/DXIL.rst 及 include/dxc/DXIL/DxilResourceProperties.h。
set(X64ARM_VKD3D_DXIL "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c")

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    DX_RAW_BUFFER_STORE             = 140,
};]=]
  [=[    DX_RAW_BUFFER_STORE             = 140,
    DX_ANNOTATE_HANDLE              = 216,
    DX_CREATE_HANDLE_FROM_BINDING   = 217,
    DX_CREATE_HANDLE_FROM_HEAP      = 218,
};]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    enum dxil_resource_kind kind;
    enum vsir_data_type resource_data_type;
};]=]
  [=[    enum dxil_resource_kind kind;
    enum vsir_data_type resource_data_type;
    unsigned int declaration_flags, byte_stride, byte_count, sample_count;
};]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[static void sm6_parser_emit_dx_create_handle(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,]=]
  [=[static enum vsir_data_type vsir_data_type_from_dxil_component_type(enum dxil_component_type type,
        struct sm6_parser *sm6);

/* x64arm: SM6.6 register bindings select existing metadata, never a fabricated resource. */
static void sm6_parser_emit_dx_create_handle_from_binding(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    const unsigned int *binding = operands[0]->u.constant.immconst.immconst_u32;
    const struct sm6_descriptor_info *d = NULL, *candidate;
    struct sm6_value *dst = sm6_parser_get_current_value(sm6);
    unsigned int i;

    if (binding[0] > binding[1] || binding[3] > VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER)
        goto invalid;
    for (i = 0; i < sm6->descriptor_count; ++i)
    {
        candidate = &sm6->descriptors[i];
        if (candidate->type != binding[3] || candidate->range.space != binding[2]
                || candidate->range.first > binding[0] || candidate->range.last < binding[1])
            continue;
        if (d)
            goto invalid;
        d = candidate;
    }
    if (!d)
        goto invalid;
    if (sm6_value_is_constant(operands[1]))
    {
        unsigned int index = sm6_value_get_constant_uint(operands[1], sm6);
        if (index < binding[0] || index > binding[1])
            goto invalid;
    }
    dst->value_type = VALUE_TYPE_HANDLE;
    dst->u.handle.d = d;
    dst->u.handle.index = operands[1];
    dst->u.handle.non_uniform = !!sm6_value_get_constant_uint(operands[2], sm6);
    return;

invalid:
    vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_RESOURCES,
            "Resource binding {%u, %u, %u, %u} or index does not match unique descriptor metadata.",
            binding[0], binding[1], binding[2], binding[3]);
}

/* x64arm: SM6.6 descriptor heap handles; see cmake/Vkd3dDxilDescriptorHeap.cmake. */
static void sm6_parser_emit_dx_create_handle_from_heap(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state);
static void sm6_parser_emit_dx_annotate_heap_handle(struct sm6_parser *sm6,
        const struct sm6_value **operands, struct sm6_value *dst);

/* x64arm: annotations refine compile-time type information; memory operations still use the shared descriptor. */
static void sm6_parser_emit_dx_annotate_handle(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,
        const struct sm6_value **operands, struct function_emission_state *state)
{
    const struct sm6_descriptor_info *d = operands[0]->u.handle.d;
    const unsigned int *properties = operands[1]->u.constant.immconst.immconst_u32;
    unsigned int kind = properties[0] & 0xff, expected = 0, count, samples;
    struct sm6_value *dst = sm6_parser_get_current_value(sm6);

    if (!d && operands[0]->u.handle.heap)
    {
        sm6_parser_emit_dx_annotate_heap_handle(sm6, operands, dst);
        return;
    }
    if (!d || kind != d->kind || (properties[0] & 0xffff0000u))
        goto invalid;
    if (d->type == VKD3D_SHADER_DESCRIPTOR_TYPE_UAV)
    {
        expected = 0x1000;
        if (d->declaration_flags & VKD3DSUF_RASTERISER_ORDERED_VIEW) expected |= 0x2000;
        if (d->declaration_flags & VKD3DSUF_GLOBALLY_COHERENT) expected |= 0x4000;
        if (d->declaration_flags & VKD3DSUF_ORDER_PRESERVING_COUNTER) expected |= 0x8000;
    }
    else if (d->type == VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER
            && (d->declaration_flags & VKD3DSI_SAMPLER_COMPARISON_MODE))
        expected = 0x8000;
    if ((properties[0] & 0xf000) != expected)
        goto invalid;
    if (kind >= RESOURCE_KIND_TEXTURE1D && kind <= RESOURCE_KIND_TYPEDBUFFER)
    {
        count = (properties[1] >> 8) & 0xff;
        samples = (properties[1] >> 16) & 0xff;
        if (!count || count > 4 || (properties[1] >> 24)
                || vsir_data_type_from_dxil_component_type(properties[1] & 0xff, sm6) != d->resource_data_type)
            goto invalid;
        if (kind == RESOURCE_KIND_TEXTURE2DMS || kind == RESOURCE_KIND_TEXTURE2DMSARRAY)
        {
            if (samples && samples != d->sample_count)
                goto invalid;
        }
        else if (samples)
            goto invalid;
    }
    else if (kind == RESOURCE_KIND_STRUCTUREDBUFFER)
    {
        if (!properties[1] || properties[1] != d->byte_stride)
            goto invalid;
    }
    else if (kind == RESOURCE_KIND_CBUFFER)
    {
        /* SM6.6 may omit the used size; metadata retains the complete declaration. */
        if (properties[1] > d->byte_count)
            goto invalid;
    }
    else if (properties[1])
        goto invalid;

    dst->value_type = VALUE_TYPE_HANDLE;
    dst->u.handle = operands[0]->u.handle;
    return;

invalid:
    vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_RESOURCES,
            "Handle resource properties {%#x, %#x} disagree with descriptor metadata.", properties[0], properties[1]);
}

static void sm6_parser_emit_dx_create_handle(struct sm6_parser *sm6, enum dx_intrinsic_opcode op,]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    [DX_CREATE_HANDLE                 ] = {"H", "ccib", sm6_parser_emit_dx_create_handle},]=]
  [=[    [DX_ANNOTATE_HANDLE               ] = {"H", "HP",   sm6_parser_emit_dx_annotate_handle},
    [DX_CREATE_HANDLE_FROM_BINDING    ] = {"H", "Bib",  sm6_parser_emit_dx_create_handle_from_binding},
    [DX_CREATE_HANDLE_FROM_HEAP       ] = {"H", "ibb",  sm6_parser_emit_dx_create_handle_from_heap},
    [DX_CREATE_HANDLE                 ] = {"H", "ccib", sm6_parser_emit_dx_create_handle},]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[        case 'D':
            return sm6_type_is_struct(type) && type->u.struc->well_known == WELL_KNOWN_DIMENSIONS;]=]
  [=[        case 'B':
            return sm6_value_is_constant(value) && sm6_type_is_struct(type) && type->u.struc->elem_count == 4
                    && sm6_type_is_i32(type->u.struc->elem_types[0]) && sm6_type_is_i32(type->u.struc->elem_types[1])
                    && sm6_type_is_i32(type->u.struc->elem_types[2]) && sm6_type_is_i8(type->u.struc->elem_types[3]);
        case 'P':
            return sm6_value_is_constant(value) && sm6_type_is_struct(type) && type->u.struc->elem_count == 2
                    && sm6_type_is_i32(type->u.struc->elem_types[0]) && sm6_type_is_i32(type->u.struc->elem_types[1]);
        case 'D':
            return sm6_type_is_struct(type) && type->u.struc->well_known == WELL_KNOWN_DIMENSIONS;]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[    d->resource_data_type = VSIR_DATA_F32;

    return VKD3D_OK;]=]
  [=[    d->kind = RESOURCE_KIND_CBUFFER;
    d->resource_data_type = VSIR_DATA_F32;

    return VKD3D_OK;]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[        d = &sm6->descriptors[sm6->descriptor_count];
        d->type = type;]=]
  [=[        d = &sm6->descriptors[sm6->descriptor_count];
        memset(d, 0, sizeof(*d));
        d->type = type;]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_DXIL}"
  [=[        ++sm6->descriptor_count;
    }

done:]=]
  [=[        d->declaration_flags = ins->flags;
        if (type == VKD3D_SHADER_DESCRIPTOR_TYPE_CBV)
            d->byte_count = ins->declaration.cb.size;
        else if (d->kind == RESOURCE_KIND_STRUCTUREDBUFFER)
            d->byte_stride = ins->declaration.structured_resource.byte_stride;
        else if (d->kind == RESOURCE_KIND_TEXTURE2DMS || d->kind == RESOURCE_KIND_TEXTURE2DMSARRAY)
            d->sample_count = ins->declaration.semantic.sample_count;
        ++sm6->descriptor_count;
    }

done:]=])
