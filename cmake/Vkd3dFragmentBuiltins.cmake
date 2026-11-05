# SM3 的 vPos／vFace 與 v0…v9 屬不同暫存器空間。
# 先分配不重疊的內部輸入編號，再交由共用 I/O 正規化轉為簽章索引；
# 公開掃描／反射仍保留原始 bytecode 的編號，不依個別 shader 重寫。
set(fragment_ir "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/ir.c")
set(fragment_before
  [=[    const struct vsir_signature_element *element;
    struct vkd3d_shader_instruction *ins;
    unsigned int signature_idx;

    if (normaliser->shader_type != VKD3D_SHADER_TYPE_PIXEL || normaliser->major != 3)
        return VKD3D_OK;

    if (vsir_signature_find_sysval(normaliser->input_signature, VKD3D_SHADER_SV_POSITION, 0, &signature_idx))]=])
set(fragment_base
  [=[    struct vsir_signature_element *element;
    struct vkd3d_shader_instruction *ins;
    unsigned int signature_idx, next_register = 0, i;

    if (normaliser->shader_type != VKD3D_SHADER_TYPE_PIXEL || normaliser->major != 3)
        return VKD3D_OK;

    /* Miscellaneous and varying inputs have independent SM3 register spaces.
     * The common I/O pass below resolves registers before signature indices. */
    for (i = 0; i < normaliser->input_signature->element_count; ++i)
    {
        element = &normaliser->input_signature->elements[i];
        next_register = max(next_register, element->register_index + max(element->register_count, 1));
    }
    for (i = 0; i < normaliser->input_signature->element_count; ++i)
    {
        element = &normaliser->input_signature->elements[i];
        if (element->sysval_semantic != VKD3D_SHADER_SV_POSITION
                && element->sysval_semantic != VKD3D_SHADER_SV_IS_FRONT_FACE)
            continue;
        if (next_register >= ARRAY_SIZE(normaliser->input_range_map))
        {
            vkd3d_shader_error(normaliser->message_context, &location,
                    VKD3D_SHADER_ERROR_VSIR_INVALID_SIGNATURE, "SM3 fragment input register space exhausted.");
            return VKD3D_ERROR_INVALID_SHADER;
        }
        element->register_index = next_register++;
    }

    if (vsir_signature_find_sysval(normaliser->input_signature, VKD3D_SHADER_SV_POSITION, 0, &signature_idx))]=])
# SV_Position 的座標分量固定；只宣告 y 時不能被一般 I/O 壓縮為 x。
string(REPLACE
  "element->register_index = next_register++;"
  "element->register_index = next_register++;\n        if (element->sysval_semantic == VKD3D_SHADER_SV_POSITION)\n            element->mask = VKD3DSP_WRITEMASK_0 | VKD3DSP_WRITEMASK_1;"
  fragment_after "${fragment_base}")
x64arm_vkd3d_patch("${fragment_ir}" "${fragment_before}" "${fragment_after}" "${fragment_base}")
x64arm_vkd3d_patch("${fragment_ir}"
  [=[        vsir_src_operand_init_io(&ins->src[0], VSIR_REGISTER_INPUT, element, 1);
        ins->src[0].reg.idx[0].offset = signature_idx;]=]
  [=[        vsir_src_operand_init_io(&ins->src[0], VSIR_REGISTER_INPUT, element, 1);
        ins->src[0].reg.idx[0].offset = element->register_index;]=])
