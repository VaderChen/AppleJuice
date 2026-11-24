# LLVM lifetime.start／end 是記憶體可供最佳化的生命週期標記，不是 dx.op 編號。
# 沿用既有執行緒私有儲存，驗證 i64 大小、指標與 void ABI 後不縮短實際配置生命週期；
# 這是 LLVM 允許的保守處理，不能把 size 誤解為 DXIL opcode 而改變運算。
# https://llvm.org/docs/LangRef.html#llvm-lifetime-start-intrinsic
set(X64ARM_VKD3D_LIFETIME "${vkd3d_shader_SOURCE_DIR}/libs/vkd3d-shader/dxil.c")
x64arm_vkd3d_patch("${X64ARM_VKD3D_LIFETIME}"
  [=[    if (!sm6_value_is_dx_intrinsic_dcl(fn_value))
        WARN("External function is not a dx intrinsic.\n");]=]
  [=[    if (!sm6_value_is_dx_intrinsic_dcl(fn_value))
    {
        const char *name = fn_value->u.function.name;
        if (!strcmp(name, "llvm.lifetime.start") || !strncmp(name, "llvm.lifetime.start.", 20)
                || !strcmp(name, "llvm.lifetime.end") || !strncmp(name, "llvm.lifetime.end.", 18))
        {
            if (operand_count != 2 || !sm6_type_is_void(type->u.function->ret_type)
                    || !sm6_value_is_constant(operands[0]) || !sm6_type_is_i64(operands[0]->type)
                    || !sm6_type_is_pointer(operands[1]->type))
            {
                vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_INVALID_OPERAND,
                        "Invalid lifetime marker signature for '%s'.", name);
                return;
            }
            /* Retain all private storage; these hints never dispatch a GPU instruction.
             * A void call defines no value: dst is the slot of the next value, which may already hold
             * a forward-allocated SSA id (e.g. a phi incoming defined right after this marker). */
            dst->type = NULL;
            return;
        }
        vkd3d_shader_parser_error(&sm6->p, VKD3D_SHADER_ERROR_DXIL_UNHANDLED_INTRINSIC,
                "External function '%s' is not a supported LLVM intrinsic.", name);
        return;
    }]=])
