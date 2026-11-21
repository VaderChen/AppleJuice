// ExecuteIndirect 的計數緩衝：GPU 依計數遮罩參數陣列，第 i≥min(計數, maximum) 筆的實例數（Draw／DrawIndexed 的第 2 個字）
// 或三個群組數（Dispatch）歸零，其餘原樣搬到 target（每筆 words×4 位元組緊密排列）。之後的間接繪製／派送直接讀 target，
// CPU 不必等前面的 GPU 工作完成再回讀計數。一批共用一個 compute encoder，只打斷一次 render pass。
// X64ARM_METAL_INDIRECT_COUNT_GPU=0 關閉（回到回讀路徑）作同成品對照。
bool metal_indirect_count_gpu_enabled(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_INDIRECT_COUNT_GPU");return !text || std::string_view(text)!="0";}();return value;}
bool metal_indirect_count_shapes(std::span<const GraphicsIndirectCountMask> masks,const GraphicsResource* target){
    if(!metal_indirect_count_gpu_enabled() || masks.empty() || !target || target->desc.dimension!=1)return false;
    for(const auto& m:masks){
        if(!m.arguments || !m.count || !m.maximum || m.words<3 || m.words>5 || m.stride<m.words*4 || (m.stride&3) || (m.offset&3) || (m.count_offset&3) || (m.target_offset&3))return false;
        if(m.arguments->desc.dimension!=1 || m.count->desc.dimension!=1)return false;
        if(std::uint64_t{m.offset}+std::uint64_t{m.maximum-1}*m.stride+m.words*4U>m.arguments->desc.width || std::uint64_t{m.count_offset}+4U>m.count->desc.width ||
           std::uint64_t{m.target_offset}+std::uint64_t{m.maximum}*m.words*4U>target->desc.width)return false;
    }
    return true;
}
bool MetalGl::graphics_indirect_count_masks(std::span<const GraphicsIndirectCountMask> masks,const std::shared_ptr<GraphicsResource>& target){@autoreleasepool{
    if(!metal_indirect_count_shapes(masks,target.get()))return false;
    auto& output=metal_resource(target);if(!output.buffer)throw std::invalid_argument("間接計數遮罩缺少 Metal 輸出緩衝");
    for(const auto& m:masks)if(!metal_resource(m.arguments).buffer || !metal_resource(m.count).buffer)throw std::invalid_argument("間接計數遮罩缺少 Metal 緩衝");
    if(!indirect_count_pipeline_){
        const char* code="#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void x64IndirectCount(device const uint* source [[buffer(0)]],device const uint* count [[buffer(1)]],device uint* out [[buffer(2)]],"
            "constant uint4& shape [[buffer(3)]],uint index [[thread_position_in_grid]]){"
            "if(index>=shape.z)return;const bool live=index<min(count[0],shape.z);"
            "for(uint w=0;w<shape.y;++w){uint value=source[index*shape.x+w];if(!live && (shape.w!=0u || w==1u))value=0u;out[index*shape.y+w]=value;}}\n";
        NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
        auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:code] options:options error:&error];
        if(!library)throw std::invalid_argument("Metal 間接計數遮罩編譯失敗："+std::string(error.localizedDescription.UTF8String));
        indirect_count_pipeline_=[device_ newComputePipelineStateWithFunction:[library newFunctionWithName:@"x64IndirectCount"] error:&error];
        if(!indirect_count_pipeline_)throw std::invalid_argument("Metal 間接計數遮罩 pipeline 失敗："+std::string(error.localizedDescription.UTF8String));
    }
    graphics_pending_.push_back(target);output.constant_bytes_valid=false;
    for(const auto& m:masks){graphics_pending_.push_back(m.arguments);graphics_pending_.push_back(m.count);}
    begin_commands(true,"indirect_count");auto encoder=[commands_ computeCommandEncoder];MetalEncoderScope scope{encoder};[encoder setComputePipelineState:indirect_count_pipeline_];
    const auto width=std::min<NSUInteger>(64,indirect_count_pipeline_.maxTotalThreadsPerThreadgroup);
    for(const auto& m:masks){
        [encoder setBuffer:metal_resource(m.arguments).buffer offset:m.offset atIndex:0];[encoder setBuffer:metal_resource(m.count).buffer offset:m.count_offset atIndex:1];
        [encoder setBuffer:output.buffer offset:m.target_offset atIndex:2];
        const std::array<std::uint32_t,4> shape{m.stride/4,m.words,m.maximum,m.dispatch?1U:0U};[encoder setBytes:shape.data() length:sizeof(shape) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake(m.maximum,1,1) threadsPerThreadgroup:MTLSizeMake(width,1,1)];
    }
    scope.end();indirect_count_masks_+=masks.size();++indirect_count_batches_;
    return true;
}}
