// SDK d3d10effect.h 的完整虛擬方法順序。
case Kind::EffectPool10:count=4;methods[3]="FxPoolAsEffect";break;
case Kind::Effect10:count=16;methods={{0,"QueryInterface"},{1,"AddRef"},{2,"Release"},{3,"FxIsValid"},{4,"FxIsPool"},{5,"FxGetDevice"},{6,"FxGetDesc"},{7,"FxGetConstantBufferByIndex"},{8,"FxGetConstantBufferByName"},{9,"FxGetVariableByIndex"},{10,"FxGetVariableByName"},{11,"FxGetVariableBySemantic"},{12,"FxGetTechniqueByIndex"},{13,"FxGetTechniqueByName"},{14,"FxOptimize"},{15,"FxIsOptimized"}};break;
case Kind::EffectType10:count=7;methods={{0,"FxTypeIsValid"},{1,"FxTypeGetDesc"},{2,"FxTypeGetMemberTypeByIndex"},{3,"FxTypeGetMemberTypeByName"},{4,"FxTypeGetMemberTypeBySemantic"},{5,"FxTypeGetMemberName"},{6,"FxTypeGetMemberSemantic"}};break;
case Kind::EffectVariable10:
case Kind::EffectScalar10:
case Kind::EffectVector10:
case Kind::EffectMatrix10:
case Kind::EffectString10:
case Kind::EffectResource10:
case Kind::EffectRenderTarget10:
case Kind::EffectDepthView10:
case Kind::EffectBuffer10:
case Kind::EffectShader10:
case Kind::EffectBlend10:
case Kind::EffectDepth10:
case Kind::EffectRaster10:
case Kind::EffectSampler10: {
    // 各衍生介面都繼承完整的 ID3D10EffectVariable vtable，包含 slot 0 的 IsValid。
    methods={
        {0,"FxVariableIsValid"},{1,"FxVariableGetType"},{2,"FxVariableGetDesc"},
        {3,"FxVariableGetAnnotationByIndex"},{4,"FxVariableGetAnnotationByName"},{5,"FxVariableGetMemberByIndex"},
        {6,"FxVariableGetMemberByName"},{7,"FxVariableGetMemberBySemantic"},{8,"FxVariableGetElement"},
        {9,"FxVariableGetParentConstantBuffer"},{10,"FxVariableAsScalar"},{11,"FxVariableAsVector"},
        {12,"FxVariableAsMatrix"},{13,"FxVariableAsString"},{14,"FxVariableAsShaderResource"},
        {15,"FxVariableAsRenderTargetView"},{16,"FxVariableAsDepthStencilView"},{17,"FxVariableAsConstantBuffer"},
        {18,"FxVariableAsShader"},{19,"FxVariableAsBlend"},{20,"FxVariableAsDepthStencil"},
        {21,"FxVariableAsRasterizer"},{22,"FxVariableAsSampler"},{23,"FxVariableSetRawValue"},
        {24,"FxVariableGetRawValue"},
    };
    count=methods.size();
    const auto extend=[&](std::initializer_list<std::string_view> names){
        for(const auto name:names)methods.emplace(count++,name);
    };
    switch(kind){
        case Kind::EffectScalar10:
            extend({"FxScalarSetFloat", "FxScalarGetFloat", "FxScalarSetFloatArray",
            "FxScalarGetFloatArray", "FxScalarSetInt", "FxScalarGetInt",
            "FxScalarSetIntArray", "FxScalarGetIntArray", "FxScalarSetBool",
            "FxScalarGetBool", "FxScalarSetBoolArray", "FxScalarGetBoolArray"});break;
        case Kind::EffectVector10:
            extend({"FxVectorSetBoolVector", "FxVectorSetIntVector", "FxVectorSetFloatVector",
            "FxVectorGetBoolVector", "FxVectorGetIntVector", "FxVectorGetFloatVector",
            "FxVectorSetBoolVectorArray", "FxVectorSetIntVectorArray", "FxVectorSetFloatVectorArray",
            "FxVectorGetBoolVectorArray", "FxVectorGetIntVectorArray", "FxVectorGetFloatVectorArray"});break;
        case Kind::EffectMatrix10:
            extend({"FxMatrixSetMatrix", "FxMatrixGetMatrix", "FxMatrixSetMatrixArray",
            "FxMatrixGetMatrixArray", "FxMatrixSetMatrixTranspose", "FxMatrixGetMatrixTranspose",
            "FxMatrixSetMatrixTransposeArray", "FxMatrixGetMatrixTransposeArray"});break;
        case Kind::EffectString10:extend({"FxStringGetString", "FxStringGetStringArray"});break;
        case Kind::EffectResource10:extend({"FxResourceSetResource", "FxResourceGetResource", "FxResourceSetResourceArray", "FxResourceGetResourceArray"});break;
        case Kind::EffectRenderTarget10:extend({"FxRenderTargetSetRenderTarget", "FxRenderTargetGetRenderTarget", "FxRenderTargetSetRenderTargetArray", "FxRenderTargetGetRenderTargetArray"});break;
        case Kind::EffectDepthView10:extend({"FxDepthViewSetDepthStencil", "FxDepthViewGetDepthStencil", "FxDepthViewSetDepthStencilArray", "FxDepthViewGetDepthStencilArray"});break;
        case Kind::EffectBuffer10:extend({"FxVariableSetConstantBuffer", "FxBufferGetConstantBuffer", "FxBufferSetTextureBuffer", "FxBufferGetTextureBuffer"});break;
        case Kind::EffectShader10:
            extend({"FxVariableGetShaderDesc", "FxShaderGetVertexShader", "FxShaderGetGeometryShader",
            "FxShaderGetPixelShader", "FxShaderGetInputSignatureElementDesc", "FxShaderGetOutputSignatureElementDesc"});break;
        case Kind::EffectBlend10:extend({"FxVariableGetBlendState", "FxBlendGetBackingStore"});break;
        case Kind::EffectDepth10:extend({"FxVariableGetDepthStencilState", "FxDepthGetBackingStore"});break;
        case Kind::EffectRaster10:extend({"FxVariableGetRasterizerState", "FxRasterGetBackingStore"});break;
        case Kind::EffectSampler10:extend({"FxVariableGetSampler", "FxSamplerGetBackingStore"});break;
        default:break;
    }
    break;
}
case Kind::EffectTechnique10:count=7;methods={{0,"FxTechniqueIsValid"},{1,"FxTechniqueGetDesc"},{2,"FxTechniqueGetAnnotationByIndex"},{3,"FxTechniqueGetAnnotationByName"},{4,"FxTechniqueGetPassByIndex"},{5,"FxTechniqueGetPassByName"},{6,"FxTechniqueComputeStateBlockMask"}};break;
case Kind::EffectPass10:count=9;methods={{0,"FxPassIsValid"},{1,"FxPassGetDesc"},{2,"FxPassGetVertexShaderDesc"},{3,"FxPassGetGeometryShaderDesc"},{4,"FxPassGetPixelShaderDesc"},{5,"FxPassGetAnnotationByIndex"},{6,"FxPassGetAnnotationByName"},{7,"FxPassApply"},{8,"FxPassComputeStateBlockMask"}};break;
