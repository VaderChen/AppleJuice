# UAV 以 storage buffer 宣告時（VKD3D_SHADER_COMPILE_OPTION_BUFFER_UAV_STORAGE_BUFFER），
# 描述元陣列（RWByteAddressBuffer[]、RWStructuredBuffer[] 等 bindless 表）在 vkd3d 2.1 的
# raw／structured／typed 讀寫、原子運算與 bufinfo 路徑都直接以陣列變數建存取鏈，漏掉描述元索引，
# 產生 [0, 位址] 這種把位址當結構成員索引的無效 SPIR-V。影像路徑已先以描述元索引取出單一資源，
# 這裡比照：有描述元陣列時先取出該索引的緩衝區塊，再以 [0, 位址] 存取。
set(X64ARM_VKD3D_SPIRV "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/spirv.c")

x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[static void spirv_compiler_emit_ld_raw_structured_srv_uav(struct spirv_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{]=]
  [=[/* x64arm: storage buffer descriptor arrays select one buffer block before member access. */
static uint32_t x64arm_storage_buffer_block(struct spirv_compiler *compiler,
        const struct vsir_operand *reg, const struct vkd3d_symbol *symbol)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_symbol_descriptor_array_data *array_data;
    uint32_t ptr_type_id, index_id;

    if (!symbol->descriptor_array)
        return symbol->id;
    array_data = &symbol->descriptor_array->info.descriptor_array;
    index_id = spirv_compiler_get_descriptor_index(compiler, reg, symbol->descriptor_array,
            symbol->info.resource.binding_base_idx, VKD3D_SHADER_RESOURCE_BUFFER);
    ptr_type_id = vkd3d_spirv_get_op_type_pointer(builder, array_data->storage_class, array_data->contained_type_id);
    return vkd3d_spirv_build_op_access_chain(builder, ptr_type_id, symbol->id, &index_id, 1);
}

static void spirv_compiler_emit_ld_raw_structured_srv_uav(struct spirv_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{]=])

# raw／structured 讀取（每個分量一條存取鏈）。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[            ptr_id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id, resource_symbol->id, indices, 2);
            constituents[j++] = vkd3d_spirv_build_op_load(builder, texel_type_id, ptr_id, SpvMemoryAccessMaskNone);]=]
  [=[            ptr_id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id,
                    x64arm_storage_buffer_block(compiler, &resource->reg, resource_symbol), indices, 2);
            constituents[j++] = vkd3d_spirv_build_op_load(builder, texel_type_id, ptr_id, SpvMemoryAccessMaskNone);]=])

# raw／structured 寫入。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[            ptr_id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id, resource_symbol->id, indices, 2);
            vkd3d_spirv_build_op_store(builder, ptr_id, data_id, SpvMemoryAccessMaskNone);]=]
  [=[            ptr_id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id,
                    x64arm_storage_buffer_block(compiler, &dst->reg, resource_symbol), indices, 2);
            vkd3d_spirv_build_op_store(builder, ptr_id, data_id, SpvMemoryAccessMaskNone);]=])

# typed UAV 讀取。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[        ptr_id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id, resource_symbol->id, indices, 2);
        val_id = vkd3d_spirv_build_op_load(builder, type_id, ptr_id, SpvMemoryAccessMaskNone);]=]
  [=[        ptr_id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id,
                x64arm_storage_buffer_block(compiler, &src[1].reg, resource_symbol), indices, 2);
        val_id = vkd3d_spirv_build_op_load(builder, type_id, ptr_id, SpvMemoryAccessMaskNone);]=])

# typed UAV 寫入。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[        ptr_id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id, resource_symbol->id, indices, 2);
        vkd3d_spirv_build_op_store(builder, ptr_id, val_id, SpvMemoryAccessMaskNone);]=]
  [=[        ptr_id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id,
                x64arm_storage_buffer_block(compiler, &dst->reg, resource_symbol), indices, 2);
        vkd3d_spirv_build_op_store(builder, ptr_id, val_id, SpvMemoryAccessMaskNone);]=])

# 原子運算。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[            pointer_id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id, resource_symbol->id, operands, 2);]=]
  [=[            pointer_id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id,
                    x64arm_storage_buffer_block(compiler, &resource->reg, resource_symbol), operands, 2);]=])

# bufinfo：OpArrayLength 需要單一緩衝區塊的指標。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[        val_id = vkd3d_spirv_build_op_array_length(builder, type_id, resource_symbol->id, 0);]=]
  [=[        val_id = vkd3d_spirv_build_op_array_length(builder, type_id,
                x64arm_storage_buffer_block(compiler, &src->reg, resource_symbol), 0);]=])

# raw／structured SRV（ByteAddressBuffer、StructuredBuffer）也宣告為唯讀 storage buffer：
# Metal texture buffer 的起點有對齊限制，無法表達 D3D 任意的 FirstElement／StructureByteStride 位移；
# storage buffer 以「位址＋位移」綁定（與 UAV 及 HLSLcc 路徑相同），描述元表的緩衝項目也已帶位址與長度。
# typed SRV（Buffer<T>）需要格式轉換，維持 texel buffer。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[/* x64arm: storage buffer descriptor arrays select one buffer block before member access. */]=]
  [=[/* x64arm: UAV buffers, and raw/structured SRV buffers, are storage buffers when ssbo_uavs is enabled. */
static bool x64arm_resource_is_ssbo(struct spirv_compiler *compiler, const struct vsir_operand *reg)
{
    const struct vkd3d_symbol *symbol;

    if (!compiler->compile_info.ssbo_uavs || (reg->type != VSIR_REGISTER_UAV && reg->type != VSIR_REGISTER_RESOURCE))
        return false;
    symbol = spirv_compiler_find_resource(compiler, reg);
    return spirv_compiler_use_storage_buffer(compiler, &symbol->info.resource) && (reg->type == VSIR_REGISTER_UAV
            || symbol->info.resource.raw || symbol->info.resource.structure_stride);
}

/* x64arm: storage buffer descriptor arrays select one buffer block before member access. */]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[    if (compiler->compile_info.ssbo_uavs && is_uav && resource_type == VKD3D_SHADER_RESOURCE_BUFFER)
    {
        uint32_t array_type_id, struct_id;

        type_id = spirv_get_type_id(compiler, sampled_type, 1);

        array_type_id = vkd3d_spirv_get_op_type_runtime_array(builder, type_id);
        vkd3d_spirv_build_op_decorate1(builder, array_type_id, SpvDecorationArrayStride, 4);

        struct_id = vkd3d_spirv_build_op_type_struct(builder, &array_type_id, 1);
        vkd3d_spirv_build_op_decorate(builder, struct_id, SpvDecorationBufferBlock, NULL, 0);
        vkd3d_spirv_build_op_member_decorate1(builder, struct_id, 0, SpvDecorationOffset, 0);
]=]
  [=[    if (compiler->compile_info.ssbo_uavs && (is_uav || raw || structure_stride)
            && resource_type == VKD3D_SHADER_RESOURCE_BUFFER)
    {
        uint32_t array_type_id, struct_id;

        type_id = spirv_get_type_id(compiler, sampled_type, 1);

        array_type_id = vkd3d_spirv_get_op_type_runtime_array(builder, type_id);
        vkd3d_spirv_build_op_decorate1(builder, array_type_id, SpvDecorationArrayStride, 4);

        struct_id = vkd3d_spirv_build_op_type_struct(builder, &array_type_id, 1);
        vkd3d_spirv_build_op_decorate(builder, struct_id, SpvDecorationBufferBlock, NULL, 0);
        vkd3d_spirv_build_op_member_decorate1(builder, struct_id, 0, SpvDecorationOffset, 0);
        if (!is_uav)
            vkd3d_spirv_build_op_member_decorate(builder, struct_id, 0, SpvDecorationNonWritable, NULL, 0);
]=])

x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[    if (resource->reg.type == VSIR_REGISTER_UAV)
    {
        resource_symbol = spirv_compiler_find_resource(compiler, &resource->reg);
        storage_buffer_uav = spirv_compiler_use_storage_buffer(compiler, &resource_symbol->info.resource);
    }]=]
  [=[    if (x64arm_resource_is_ssbo(compiler, &resource->reg))
    {
        resource_symbol = spirv_compiler_find_resource(compiler, &resource->reg);
        storage_buffer_uav = true;
    }]=])

# bufinfo：storage buffer 分支的 raw／structured 資訊取自資源符號（上游讀取未初始化的 image 欄位）。
x64arm_vkd3d_patch("${X64ARM_VKD3D_SPIRV}"
  [=[    if (compiler->compile_info.ssbo_uavs && src->reg.type == VSIR_REGISTER_UAV)
    {
        resource_symbol = spirv_compiler_find_resource(compiler, &src->reg);

        type_id = spirv_get_type_id(compiler, VSIR_DATA_U32, 1);
        val_id = vkd3d_spirv_build_op_array_length(builder, type_id,
                x64arm_storage_buffer_block(compiler, &src->reg, resource_symbol), 0);
        write_mask = VKD3DSP_WRITEMASK_0;
    }]=]
  [=[    if (x64arm_resource_is_ssbo(compiler, &src->reg))
    {
        resource_symbol = spirv_compiler_find_resource(compiler, &src->reg);

        type_id = spirv_get_type_id(compiler, VSIR_DATA_U32, 1);
        val_id = vkd3d_spirv_build_op_array_length(builder, type_id,
                x64arm_storage_buffer_block(compiler, &src->reg, resource_symbol), 0);
        write_mask = VKD3DSP_WRITEMASK_0;
        image.structure_stride = resource_symbol->info.resource.structure_stride;
        image.raw = resource_symbol->info.resource.raw;
    }]=])
