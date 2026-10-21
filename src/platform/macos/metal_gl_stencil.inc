std::optional<MTLStencilOperation> gl_stencil_operation(unsigned value){
    switch(value){case 0x1e00:return MTLStencilOperationKeep;case 0:return MTLStencilOperationZero;case 0x1e01:return MTLStencilOperationReplace;case 0x1e02:return MTLStencilOperationIncrementClamp;case 0x1e03:return MTLStencilOperationDecrementClamp;case 0x150a:return MTLStencilOperationInvert;case 0x8507:return MTLStencilOperationIncrementWrap;case 0x8508:return MTLStencilOperationDecrementWrap;default:return std::nullopt;}
}
void MetalGl::gl_stencil(MTLDepthStencilDescriptor* depth,bool clearing,unsigned clear_mask){
    if(fixed_targets_set_ && !clearing){
        const auto& s=fixed_stencil_;if(!s[0] || !fixed_depth_.resource || metal_resource(fixed_depth_.resource).texture.pixelFormat!=MTLPixelFormatDepth32Float_Stencil8)return;
        for(unsigned face=0;face<2;++face){const auto at=4+face*4;auto stencil=make_metal_stencil(static_cast<MTLCompareFunction>(s[at]-1),static_cast<MTLStencilOperation>(s[at+1]-1),static_cast<MTLStencilOperation>(s[at+2]-1),static_cast<MTLStencilOperation>(s[at+3]-1),s[2]&255,s[3]&255);if(face)depth.backFaceStencil=stencil;else depth.frontFaceStencil=stencil;}
        return;
    }
    if(clearing?!(clear_mask&0x400):!state_.enabled.contains(0xb90))return;
    const auto& s=state_.stencil;
    auto stencil=make_metal_stencil(clearing?MTLCompareFunctionAlways:static_cast<MTLCompareFunction>(s.function-0x200),
        clearing?MTLStencilOperationKeep:*gl_stencil_operation(s.fail),clearing?MTLStencilOperationKeep:*gl_stencil_operation(s.depth_fail),
        clearing?MTLStencilOperationReplace:*gl_stencil_operation(s.pass),s.read_mask,s.write_mask);
    depth.frontFaceStencil=stencil;depth.backFaceStencil=stencil;
}
