# SM4.x 的 resinfo（HLSL GetDimensions）沒有擴充資源維度記號，那是 SM5 編譯器才會附的；
# Metal 後端原本只看指令上的維度，未知時寬高全部輸出 0（FXAA 以 1／尺寸算像素間距，會變成無限大而取樣錯亂）。
# 維度未知且運算元是貼圖資源時，改用 dcl_resource 宣告的維度，與 GLSL 後端相同。
x64arm_hlslcc_patch(toMetalInstruction.cpp
  [=[    const char *metalGetters[] = { ".get_width(", ".get_height(", ".get_depth(", ".get_num_mip_levels()" };
    int dim = GetNumTextureDimensions(psInst->eResDim);]=]
  [=[    const char *metalGetters[] = { ".get_width(", ".get_height(", ".get_depth(", ".get_num_mip_levels()" };
    if (psInst->eResDim == RESOURCE_DIMENSION_UNKNOWN && psInst->asOperands[2].eType == OPERAND_TYPE_RESOURCE)
        psInst->eResDim = psContext->psShader->aeResourceDims[psInst->asOperands[2].ui32RegisterNumber];
    int dim = GetNumTextureDimensions(psInst->eResDim);]=])
