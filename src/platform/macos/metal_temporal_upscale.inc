// 時間性升頻對應 Apple GPU 的 MetalFX 時間性升頻器：輸入（抖動的低解析度色彩、深度、動態向量）與 XeSS 等 API 相同。
// 升頻器帶有跨幀歷史，依呼叫端的歷史編號保存；格式或尺寸改變就重建。
bool MetalGl::graphics_temporal_upscale_supported(){
    if(@available(macOS 13.0,*))return [MTLFXTemporalScalerDescriptor supportsDevice:device_];
    return false;
}
void MetalGl::graphics_temporal_upscale_release(std::uint64_t history){temporal_scalers_.erase(history);}
void MetalGl::graphics_temporal_upscale(const GraphicsTemporalUpscale& u){@autoreleasepool{
    if(@available(macOS 13.0,*)){
        const auto texture=[&](const GraphicsView& view,const char* name)->id<MTLTexture>{
            if(!view.resource)throw std::invalid_argument(std::string("時間性升頻缺少")+name);
            auto& r=metal_resource(view.resource);
            if(!r.texture || r.desc.dimension!=3 || r.desc.samples>1 || view.level || view.slice)throw std::invalid_argument(std::string("時間性升頻的")+name+"必須是單一取樣的 2D 紋理第 0 層");
            graphics_pending_.push_back(view.resource);return r.texture;
        };
        auto color=texture(u.color,"色彩"),depth=texture(u.depth,"深度"),motion=texture(u.motion,"動態向量"),output=texture(u.output,"輸出");
        id<MTLTexture> exposure=u.exposure.resource?texture(u.exposure,"曝光"):nil;
        if(!u.input_width || !u.input_height || u.input_width>color.width || u.input_height>color.height || u.output_width>output.width || u.output_height>output.height)
            throw std::invalid_argument("時間性升頻的尺寸超出紋理");
        std::ostringstream key;key<<color.pixelFormat<<':'<<depth.pixelFormat<<':'<<motion.pixelFormat<<':'<<output.pixelFormat<<':'<<u.input_width<<'x'<<u.input_height<<':'<<u.output_width<<'x'<<u.output_height<<':'<<u.auto_exposure;
        auto& entry=temporal_scalers_[u.history];
        if(!entry.scaler || entry.key!=key.str()){
            auto descriptor=[MTLFXTemporalScalerDescriptor new];
            descriptor.colorTextureFormat=color.pixelFormat;descriptor.depthTextureFormat=depth.pixelFormat;descriptor.motionTextureFormat=motion.pixelFormat;descriptor.outputTextureFormat=output.pixelFormat;
            descriptor.inputWidth=u.input_width;descriptor.inputHeight=u.input_height;descriptor.outputWidth=u.output_width;descriptor.outputHeight=u.output_height;
            descriptor.autoExposureEnabled=u.auto_exposure;descriptor.inputContentPropertiesEnabled=NO;
            id<MTLFXTemporalScaler> created=[descriptor newTemporalScalerWithDevice:device_];
            if(!created)throw std::invalid_argument("MetalFX 不支援這組時間性升頻格式："+key.str());
            entry.scaler=created;entry.key=key.str();
        }
        id<MTLFXTemporalScaler> scaler=entry.scaler;
        begin_commands(true,"temporal-upscale");
        // 紋理用途不涵蓋升頻器要求時（例如輸出只有 UAV 用途），經同格式的暫存紋理以 blit 複製進出；暫存紋理依用途與尺寸重用。
        const auto staging=[&](id<MTLTexture> source,MTLTextureUsage usage,unsigned index)->id<MTLTexture>{
            if((source.usage&usage)==usage)return source;
            auto& slot=entry.staging[index];
            if(!slot || slot.width!=source.width || slot.height!=source.height || slot.pixelFormat!=source.pixelFormat || (slot.usage&usage)!=usage){
                auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat width:source.width height:source.height mipmapped:NO];
                descriptor.usage=usage;descriptor.storageMode=MTLStorageModePrivate;slot=[device_ newTextureWithDescriptor:descriptor];
                if(!slot)throw std::bad_alloc{};
            }
            return slot;
        };
        const auto copy=[&](id<MTLTexture> from,id<MTLTexture> to){if(from==to)return;auto blit=[commands_ blitCommandEncoder];MetalEncoderScope blit_scope{blit};[blit copyFromTexture:from toTexture:to];blit_scope.end();};
        auto color_in=staging(color,scaler.colorTextureUsage,0),depth_in=staging(depth,scaler.depthTextureUsage,1),motion_in=staging(motion,scaler.motionTextureUsage,2),output_to=staging(output,scaler.outputTextureUsage,3);
        copy(color,color_in);copy(depth,depth_in);copy(motion,motion_in);
        scaler.colorTexture=color_in;scaler.depthTexture=depth_in;scaler.motionTexture=motion_in;scaler.outputTexture=output_to;scaler.exposureTexture=exposure;
        scaler.jitterOffsetX=u.jitter_x;scaler.jitterOffsetY=u.jitter_y;scaler.motionVectorScaleX=u.motion_scale_x;scaler.motionVectorScaleY=u.motion_scale_y;
        scaler.preExposure=u.pre_exposure;scaler.reset=u.reset;scaler.depthReversed=u.depth_reversed;
        [scaler encodeToCommandBuffer:commands_];
        copy(output_to,output);
        return;
    }
    throw std::runtime_error("此系統沒有 MetalFX 時間性升頻");
}}
