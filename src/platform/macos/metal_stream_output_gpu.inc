// 前一階段輸出簽章建立的 SO 只轉送 VS 結果，完整圖元數可由 IA 推得。
// GPU 以 uint 搬移位元，保留多 buffer 同步溢位、SKIP、stride 尾端及追加游標。
bool metal_stream_output_gpu_enabled(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_STREAM_OUTPUT_GPU");return text && std::string_view(text)=="1";}();return value;}
bool MetalGl::graphics_stream_output(const GraphicsDraw& draw,const MetalShader& shader,id<MTLBuffer> vertices,
                                    unsigned invocations,unsigned capacity,unsigned width){@autoreleasepool{
    if(!metal_stream_output_gpu_enabled() || shader.stage!=0 || !width || capacity%width)return false;
    const auto layout=metal_stream_layout(shader);
    for(const auto& copy:layout.copies)if(copy.destination+copy.bytes>shader.stream_strides[copy.slot])return false;
    const std::uint64_t needed=std::uint64_t{invocations}*(capacity/width);
    std::uint64_t written=needed;unsigned mask=0;std::array<std::uint32_t,4> origins{};
    std::array<id<MTLBuffer>,4> targets{};std::set<const GraphicsResource*> distinct;
    for(unsigned slot=0;slot<4;++slot)if(layout.used[slot] && draw.stream_targets[slot]){
        auto& target=metal_resource(draw.stream_targets[slot]);const auto stride=shader.stream_strides[slot];
        if(!target.buffer || !stride || stride%4 || target.stream_cursor%4 || !distinct.insert(&target).second)return false;
        mask|=1U<<slot;targets[slot]=target.buffer;
        if(target.stream_cursor>target.desc.width)written=0;
        else {origins[slot]=static_cast<std::uint32_t>(target.stream_cursor);written=std::min(written,(target.desc.width-target.stream_cursor)/(std::uint64_t{width}*stride));}
    }
    if(!mask)written=0;
    const auto count=written*width;
    if(count>UINT32_MAX)return false;
    if(count && !layout.copies.empty()){
        std::ostringstream key;key<<shader.output_varyings.size();for(auto stride:shader.stream_strides)key<<':'<<stride;
        for(const auto& copy:layout.copies)key<<':'<<copy.source<<':'<<copy.destination<<':'<<copy.bytes<<':'<<copy.slot;
        const auto text=key.str();auto found=stream_copy_pipelines_.find(text);id<MTLComputePipelineState> pipeline=nil;
        if(found!=stream_copy_pipelines_.end())pipeline=found->second;
        else {
            std::ostringstream code;code<<"#include <metal_stdlib>\nusing namespace metal;\n"
                "kernel void x64StreamCopy(device const uint* source [[buffer(0)]],"
                "device uint* out0 [[buffer(1)]],device uint* out1 [[buffer(2)]],"
                "device uint* out2 [[buffer(3)]],device uint* out3 [[buffer(4)]],"
                "constant uint4& origins [[buffer(5)]],constant uint2& range [[buffer(6)]],uint index [[thread_position_in_grid]]){"
                "if(index>=range.x)return;uint at=index*"<<shader.output_varyings.size()*4<<"u;\n";
            for(const auto& copy:layout.copies)for(unsigned lane=0;lane<copy.bytes/4;++lane)
                code<<"if(range.y & "<<(1U<<copy.slot)<<"u)out"<<copy.slot<<"[origins["<<copy.slot<<"]/4u+index*"<<shader.stream_strides[copy.slot]/4<<"u+"<<copy.destination/4+lane<<"u]=source[at+"<<copy.source/4+lane<<"u];\n";
            code<<"}\n";NSError* error=nil;auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;
            auto library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:code.str().c_str()] options:options error:&error];
            if(!library)throw std::invalid_argument("Metal SO 搬移編譯失敗："+std::string(error.localizedDescription.UTF8String));
            pipeline=[device_ newComputePipelineStateWithFunction:[library newFunctionWithName:@"x64StreamCopy"] error:&error];
            if(!pipeline)throw std::invalid_argument("Metal SO 搬移 pipeline 失敗："+std::string(error.localizedDescription.UTF8String));
            if(stream_copy_pipelines_.size()>=128)stream_copy_pipelines_.clear();stream_copy_pipelines_.emplace(text,pipeline);
        }
        for(unsigned slot=0;slot<4;++slot)if(mask&(1U<<slot)){
            graphics_pending_.push_back(draw.stream_targets[slot]);metal_resource(draw.stream_targets[slot]).constant_bytes_valid=false;
        }
        begin_commands();auto encoder=[commands_ computeCommandEncoder];MetalEncoderScope scope{encoder};[encoder setComputePipelineState:pipeline];
        [encoder setBuffer:vertices offset:0 atIndex:0];
        for(unsigned slot=0;slot<4;++slot)[encoder setBuffer:targets[slot]?targets[slot]:vertices offset:0 atIndex:slot+1];
        const std::array<std::uint32_t,2> range{static_cast<std::uint32_t>(count),mask};
        [encoder setBytes:origins.data() length:sizeof(origins) atIndex:5];[encoder setBytes:range.data() length:sizeof(range) atIndex:6];
        [encoder dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(64,pipeline.maxTotalThreadsPerThreadgroup),1,1)];scope.end();
        for(const auto& copy:layout.copies)if(mask&(1U<<copy.slot))stream_gpu_bytes_+=count*copy.bytes;
        ++stream_gpu_draws_;
    }
    for(unsigned slot=0;slot<4;++slot)if(mask&(1U<<slot)){
        auto& target=*draw.stream_targets[slot];target.stream_cursor+=count*shader.stream_strides[slot];target.stream_vertices+=count;
    }
    if(draw.stream_statistics){draw.stream_statistics->written+=written;draw.stream_statistics->needed+=needed;}
    return true;
}}
