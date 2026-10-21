// Typed buffer 與原始 MTLBuffer 共用儲存；shader 額外接收 view 範圍，
// 支援未對齊 Metal texture offset 的 D3D FirstElement、越界讀零及 BUFINFO。
void metal_buffer_shader(MetalShader& shader,const char* entry=nullptr){
    if(shader.source.find("x64arm_buffer_views[")==std::string::npos)return;
    if(shader.buffer_views_slot==UINT32_MAX)shader.buffer_views_slot=metal_free_buffers(shader,nullptr,1).front();
    const auto function=metal_function_text(shader.source,entry?entry:shader.stage==5?"computeMain":shader.stage==2?"x64GeometryMain":"xlatMtlMain");
    shader.source.insert(function.begin,"constant uint4* x64arm_buffer_views [[buffer("+std::to_string(shader.buffer_views_slot)+")]]"+(function.parameters.empty()?"":",")+"\n");
    if(shader.source.find("x64arm_buffer_read(")!=std::string::npos || shader.source.find("x64arm_buffer_write(")!=std::string::npos)
        shader.source="#ifndef X64ARM_BUFFER_HELPERS\n#define X64ARM_BUFFER_HELPERS\n#include <metal_stdlib>\nusing namespace metal;\n"
            "template<typename T, access A> vec<T,4> x64arm_buffer_read(texture_buffer<T,A> resource, uint4 view, uint index) {\n"
            "    if(index >= view.y) return vec<T,4>(0);\n"
            "    if(view.z == 12) { uint base = (index + view.x) * 3; return vec<T,4>(resource.read(base).x, resource.read(base+1).x, resource.read(base+2).x, T(1)); }\n"
            "    return resource.read(index + view.x);\n}\n"
            "template<typename T, access A> void x64arm_buffer_write(texture_buffer<T,A> resource, uint4 view, uint index, vec<T,4> value) {\n"
            "    if(index < view.y) { if(view.z == 12) { uint base = (index + view.x) * 3; resource.write(vec<T,4>(value.x), base); resource.write(vec<T,4>(value.y), base+1); resource.write(vec<T,4>(value.z), base+2); } else resource.write(value, index + view.x); resource.fence(); }\n}\n#endif\n"+shader.source;
}
std::array<std::uint32_t,4> metal_buffer_view_info(const GraphicsView& view){
    if(!view.resource || view.resource->desc.dimension!=1)return {};
    const auto& d=view.resource->desc;
    const auto stride=d.stride?d.stride:graphics_format_bytes(view.format);
    if(!stride || view.first>d.width)throw std::invalid_argument("緩衝區 view 的 stride／起點不合法");
    const auto count=view.count?view.count:d.width-view.first;
    if(count>d.width-view.first || view.first%stride || count%stride)throw std::invalid_argument("緩衝區 view 範圍不合法");
    return {view.first/stride,count/stride,stride,(view.flags&1)?count:count/stride};
}
id<MTLTexture> metal_buffer_texture(const GraphicsView& view){
    auto& resource=metal_resource(view.resource);
    if(!resource.buffer || resource.desc.stride || (view.flags&1))throw std::invalid_argument("Typed buffer 與 view 類型不符");
    (void)metal_buffer_view_info(view);
    const bool rgb32=view.format>=5 && view.format<=8;const auto format=metal_format(rgb32?(view.format<=6?41:view.format==7?42:43):view.format);const auto stride=rgb32?4U:graphics_format_bytes(view.format);
    const auto alignment=[resource.buffer.device minimumTextureBufferAlignmentForPixelFormat:format];
    if(!alignment)throw std::invalid_argument("Metal 不支援此 typed buffer 格式");
    const auto row=((std::uint64_t{resource.desc.width}+alignment-1)/alignment)*alignment;
    if(row>resource.buffer.length)throw std::invalid_argument("Metal typed buffer 配置長度不足");
    MTLTextureUsage usage=MTLTextureUsageShaderRead;if(resource.desc.bind&0x80)usage|=MTLTextureUsageShaderWrite;
    auto descriptor=[MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:format width:resource.desc.width/stride resourceOptions:MTLResourceStorageModeShared usage:usage];
    auto texture=[resource.buffer newTextureWithDescriptor:descriptor offset:0 bytesPerRow:row];
    if(!texture)throw std::invalid_argument("Metal typed buffer view 建立失敗");
    return texture;
}
