// D3D render-target view 的 layer 索引相對於 view 起點；2D array 以 Metal view 重設起點。
struct MetalRenderTarget {
    id<MTLTexture> texture;
    unsigned layers{},depth_start{};
    bool volume{};
};
MetalRenderTarget metal_render_target(const GraphicsView& view){
    const auto& resource=metal_resource(view.resource);
    const bool volume=resource.desc.dimension==4;
    auto texture_view=view;
    if(volume){texture_view.slice=0;texture_view.slices=1;}
    auto texture=metal_shader_texture(texture_view);
    if(!texture)throw std::invalid_argument("Render target 必須是紋理");
    return {texture,view.slices,volume?view.slice:0,volume};
}
void metal_attach_target(MTLRenderPassAttachmentDescriptor* attachment,const MetalRenderTarget& target,unsigned layer=0){
    attachment.texture=target.texture;attachment.level=0;
    if(target.volume)attachment.depthPlane=target.depth_start+layer;
    else attachment.slice=layer;
}
void metal_attach_target(MetalPassSpec::Attachment& attachment,const MetalRenderTarget& target,unsigned layer=0){
    attachment.texture=target.texture;attachment.level=0;
    if(target.volume)attachment.depthPlane=target.depth_start+layer;
    else attachment.slice=layer;
}
